/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * Implementation of cl_io for VVP layer.
 *
 *   Author: Nikita Danilov <nikita.danilov@sun.com>
 *   Author: Jinshan Xiong <jinshan.xiong@whamcloud.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE


#include <obd.h>
#include "vvp_internal.h"

static struct vvp_io *cl2vvp_io(const struct lu_env *env,
                                const struct cl_io_slice *slice);

/**
 * True, if \a io is a normal io, False for splice_{read,write}
 */
int cl_is_normalio(const struct lu_env *env, const struct cl_io *io)
{
        struct vvp_io *vio = vvp_env_io(env);

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

        return vio->cui_io_subtype == IO_NORMAL;
}

/**
 * For swapping layout. The file's layout may have changed.
 * To avoid populating pages to a wrong stripe, we have to verify the
 * correctness of layout. It works because swapping layout processes
 * have to acquire group lock.
 */
static bool can_populate_pages(const struct lu_env *env, struct cl_io *io,
				struct inode *inode)
{
	struct ll_inode_info	*lli = ll_i2info(inode);
	struct ccc_io		*cio = ccc_env_io(env);
	bool rc = true;

	switch (io->ci_type) {
	case CIT_READ:
	case CIT_WRITE:
		/* don't need lock here to check lli_layout_gen as we have held
		 * extent lock and GROUP lock has to hold to swap layout */
		if (ll_layout_version_get(lli) != cio->cui_layout_gen) {
			io->ci_need_restart = 1;
			/* this will return application a short read/write */
			io->ci_continue = 0;
			rc = false;
		}
	case CIT_FAULT:
		/* fault is okay because we've already had a page. */
	default:
		break;
	}

	return rc;
}

/*****************************************************************************
 *
 * io operations.
 *
 */

static int vvp_io_write_iter_init(const struct lu_env *env,
				  const struct cl_io_slice *ios)
{
	struct ccc_io *cio = cl2ccc_io(env, ios);

	cl_page_list_init(&cio->u.write.cui_queue);
	cio->u.write.cui_written = 0;
	cio->u.write.cui_from = 0;
	cio->u.write.cui_to = PAGE_SIZE;

	return 0;
}

static void vvp_io_write_iter_fini(const struct lu_env *env,
				   const struct cl_io_slice *ios)
{
	struct ccc_io *cio = cl2ccc_io(env, ios);

	LASSERT(cio->u.write.cui_queue.pl_nr == 0);
}

static int vvp_io_fault_iter_init(const struct lu_env *env,
                                  const struct cl_io_slice *ios)
{
        struct vvp_io *vio   = cl2vvp_io(env, ios);
        struct inode  *inode = ccc_object_inode(ios->cis_obj);

        LASSERT(inode ==
                cl2ccc_io(env, ios)->cui_fd->fd_file->f_dentry->d_inode);
        vio->u.fault.ft_mtime = LTIME_S(inode->i_mtime);
        return 0;
}

static void vvp_io_fini(const struct lu_env *env, const struct cl_io_slice *ios)
{
	struct cl_io     *io  = ios->cis_io;
	struct cl_object *obj = io->ci_obj;
	struct ccc_io    *cio = cl2ccc_io(env, ios);
	struct inode     *inode = ccc_object_inode(obj);

	CLOBINVRNT(env, obj, ccc_object_invariant(obj));

	CDEBUG(D_VFSTRACE, DFID" ignore/verify layout %d/%d, layout version %d "
			   "restore needed %d\n",
	       PFID(lu_object_fid(&obj->co_lu)),
	       io->ci_ignore_layout, io->ci_verify_layout,
	       cio->cui_layout_gen, io->ci_restore_needed);

	if (io->ci_restore_needed == 1) {
		int	rc;

		/* file was detected release, we need to restore it
		 * before finishing the io
		 */
		rc = ll_layout_restore(inode, 0, OBD_OBJECT_EOF);
		/* if restore registration failed, no restart,
		 * we will return -ENODATA */
		/* The layout will change after restore, so we need to
		 * block on layout lock hold by the MDT
		 * as MDT will not send new layout in lvb (see LU-3124)
		 * we have to explicitly fetch it, all this will be done
		 * by ll_layout_refresh()
		 */
		if (rc == 0) {
			io->ci_restore_needed = 0;
			io->ci_need_restart = 1;
			io->ci_verify_layout = 1;
		} else {
			io->ci_restore_needed = 1;
			io->ci_need_restart = 0;
			io->ci_verify_layout = 0;
			io->ci_result = rc;
		}
	}

	if (!io->ci_ignore_layout && io->ci_verify_layout) {
		__u32 gen = 0;

		/* check layout version */
		ll_layout_refresh(inode, &gen);
		io->ci_need_restart = cio->cui_layout_gen != gen;
		if (io->ci_need_restart) {
			CDEBUG(D_VFSTRACE,
			       DFID" layout changed from %d to %d.\n",
			       PFID(lu_object_fid(&obj->co_lu)),
			       cio->cui_layout_gen, gen);
			/* today successful restore is the only possible
			 * case */
			/* restore was done, clear restoring state */
			ll_i2info(ccc_object_inode(obj))->lli_flags &=
				~LLIF_FILE_RESTORING;
		}
	}
}

static void vvp_io_fault_fini(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct cl_io   *io   = ios->cis_io;
        struct cl_page *page = io->u.ci_fault.ft_page;

        CLOBINVRNT(env, io->ci_obj, ccc_object_invariant(io->ci_obj));

        if (page != NULL) {
                lu_ref_del(&page->cp_reference, "fault", io);
                cl_page_put(env, page);
                io->u.ci_fault.ft_page = NULL;
        }
        vvp_io_fini(env, ios);
}

static enum cl_lock_mode vvp_mode_from_vma(struct vm_area_struct *vma)
{
        /*
         * we only want to hold PW locks if the mmap() can generate
         * writes back to the file and that only happens in shared
         * writable vmas
         */
        if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_WRITE))
                return CLM_WRITE;
        return CLM_READ;
}

static int vvp_mmap_locks(const struct lu_env *env,
                          struct ccc_io *vio, struct cl_io *io)
{
        struct ccc_thread_info *cti = ccc_env_info(env);
        struct mm_struct       *mm = current->mm;
        struct vm_area_struct  *vma;
        struct cl_lock_descr   *descr = &cti->cti_descr;
        ldlm_policy_data_t      policy;
        unsigned long           addr;
        unsigned long           seg;
        ssize_t                 count;
	int                     result = 0;
        ENTRY;

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);

        if (!cl_is_normalio(env, io))
                RETURN(0);

        if (vio->cui_iov == NULL) /* nfs or loop back device write */
                RETURN(0);

        /* No MM (e.g. NFS)? No vmas too. */
        if (mm == NULL)
                RETURN(0);

        for (seg = 0; seg < vio->cui_nrsegs; seg++) {
                const struct iovec *iv = &vio->cui_iov[seg];

                addr = (unsigned long)iv->iov_base;
                count = iv->iov_len;
                if (count == 0)
                        continue;

                count += addr & (~CFS_PAGE_MASK);
                addr &= CFS_PAGE_MASK;

                down_read(&mm->mmap_sem);
                while((vma = our_vma(mm, addr, count)) != NULL) {
                        struct inode *inode = vma->vm_file->f_dentry->d_inode;
                        int flags = CEF_MUST;

			if (ll_file_nolock(vma->vm_file)) {
				/*
				 * For no lock case is not allowed for mmap
				 */
				result = -EINVAL;
				break;
			}

                        /*
                         * XXX: Required lock mode can be weakened: CIT_WRITE
                         * io only ever reads user level buffer, and CIT_READ
                         * only writes on it.
                         */
                        policy_from_vma(&policy, vma, addr, count);
                        descr->cld_mode = vvp_mode_from_vma(vma);
                        descr->cld_obj = ll_i2info(inode)->lli_clob;
                        descr->cld_start = cl_index(descr->cld_obj,
                                                    policy.l_extent.start);
                        descr->cld_end = cl_index(descr->cld_obj,
                                                  policy.l_extent.end);
                        descr->cld_enq_flags = flags;
                        result = cl_io_lock_alloc_add(env, io, descr);

                        CDEBUG(D_VFSTRACE, "lock: %d: [%lu, %lu]\n",
                               descr->cld_mode, descr->cld_start,
                               descr->cld_end);

			if (result < 0)
				break;

			if (vma->vm_end - addr >= count)
				break;

			count -= vma->vm_end - addr;
			addr = vma->vm_end;
		}
		up_read(&mm->mmap_sem);
		if (result < 0)
			break;
	}
	RETURN(result);
}

static int vvp_io_rw_lock(const struct lu_env *env, struct cl_io *io,
                          enum cl_lock_mode mode, loff_t start, loff_t end)
{
        struct ccc_io *cio = ccc_env_io(env);
        int result;
        int ast_flags = 0;

        LASSERT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);
        ENTRY;

        ccc_io_update_iov(env, cio, io);

        if (io->u.ci_rw.crw_nonblock)
                ast_flags |= CEF_NONBLOCK;
        result = vvp_mmap_locks(env, cio, io);
        if (result == 0)
                result = ccc_io_one_lock(env, io, ast_flags, mode, start, end);
        RETURN(result);
}

static int vvp_io_read_lock(const struct lu_env *env,
                            const struct cl_io_slice *ios)
{
	struct cl_io		*io  = ios->cis_io;
	struct cl_io_rw_common	*rd = &io->u.ci_rd.rd;
	int result;

	ENTRY;
	result = vvp_io_rw_lock(env, io, CLM_READ, rd->crw_pos,
				rd->crw_pos + rd->crw_count - 1);
	RETURN(result);
}

static int vvp_io_fault_lock(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct cl_io *io   = ios->cis_io;
        struct vvp_io *vio = cl2vvp_io(env, ios);
        /*
         * XXX LDLM_FL_CBPENDING
         */
        return ccc_io_one_lock_index
                (env, io, 0, vvp_mode_from_vma(vio->u.fault.ft_vma),
                 io->u.ci_fault.ft_index, io->u.ci_fault.ft_index);
}

static int vvp_io_write_lock(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct cl_io *io = ios->cis_io;
        loff_t start;
        loff_t end;

        if (io->u.ci_wr.wr_append) {
                start = 0;
                end   = OBD_OBJECT_EOF;
        } else {
                start = io->u.ci_wr.wr.crw_pos;
                end   = start + io->u.ci_wr.wr.crw_count - 1;
        }
        return vvp_io_rw_lock(env, io, CLM_WRITE, start, end);
}

static int vvp_io_setattr_iter_init(const struct lu_env *env,
				    const struct cl_io_slice *ios)
{
	return 0;
}

/**
 * Implementation of cl_io_operations::cio_lock() method for CIT_SETATTR io.
 *
 * Handles "lockless io" mode when extent locking is done by server.
 */
static int vvp_io_setattr_lock(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
	struct ccc_io *cio = ccc_env_io(env);
	struct cl_io  *io  = ios->cis_io;
	__u64 new_size;
	__u32 enqflags = 0;

        if (cl_io_is_trunc(io)) {
                new_size = io->u.ci_setattr.sa_attr.lvb_size;
                if (new_size == 0)
                        enqflags = CEF_DISCARD_DATA;
        } else {
                if ((io->u.ci_setattr.sa_attr.lvb_mtime >=
                     io->u.ci_setattr.sa_attr.lvb_ctime) ||
                    (io->u.ci_setattr.sa_attr.lvb_atime >=
                     io->u.ci_setattr.sa_attr.lvb_ctime))
                        return 0;
                new_size = 0;
        }
        cio->u.setattr.cui_local_lock = SETATTR_EXTENT_LOCK;
        return ccc_io_one_lock(env, io, enqflags, CLM_WRITE,
                               new_size, OBD_OBJECT_EOF);
}

static int vvp_do_vmtruncate(struct inode *inode, size_t size)
{
	int     result;

	/*
	 * Only ll_inode_size_lock is taken at this level.
	 */
	ll_inode_size_lock(inode);
	result = inode_newsize_ok(inode, size);
	if (result < 0) {
		ll_inode_size_unlock(inode);
		return result;
	}
	i_size_write(inode, size);

	ll_truncate_pagecache(inode, size);
	ll_inode_size_unlock(inode);
	return result;
}

static int vvp_io_setattr_trunc(const struct lu_env *env,
                                const struct cl_io_slice *ios,
                                struct inode *inode, loff_t size)
{
	inode_dio_wait(inode);
	return 0;
}

static int vvp_io_setattr_time(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
        struct cl_io       *io    = ios->cis_io;
        struct cl_object   *obj   = io->ci_obj;
        struct cl_attr     *attr  = ccc_env_thread_attr(env);
        int result;
        unsigned valid = CAT_CTIME;

        cl_object_attr_lock(obj);
        attr->cat_ctime = io->u.ci_setattr.sa_attr.lvb_ctime;
        if (io->u.ci_setattr.sa_valid & ATTR_ATIME_SET) {
                attr->cat_atime = io->u.ci_setattr.sa_attr.lvb_atime;
                valid |= CAT_ATIME;
        }
        if (io->u.ci_setattr.sa_valid & ATTR_MTIME_SET) {
                attr->cat_mtime = io->u.ci_setattr.sa_attr.lvb_mtime;
                valid |= CAT_MTIME;
        }
        result = cl_object_attr_set(env, obj, attr, valid);
        cl_object_attr_unlock(obj);

        return result;
}

static int vvp_io_setattr_start(const struct lu_env *env,
				const struct cl_io_slice *ios)
{
	struct cl_io	*io    = ios->cis_io;
	struct inode	*inode = ccc_object_inode(io->ci_obj);
	int result = 0;

	mutex_lock(&inode->i_mutex);
	if (cl_io_is_trunc(io))
		result = vvp_io_setattr_trunc(env, ios, inode,
					io->u.ci_setattr.sa_attr.lvb_size);
	if (result == 0)
		result = vvp_io_setattr_time(env, ios);
	return result;
}

static void vvp_io_setattr_end(const struct lu_env *env,
                               const struct cl_io_slice *ios)
{
	struct cl_io *io    = ios->cis_io;
	struct inode *inode = ccc_object_inode(io->ci_obj);

	if (cl_io_is_trunc(io)) {
		/* Truncate in memory pages - they must be clean pages
		 * because osc has already notified to destroy osc_extents. */
		vvp_do_vmtruncate(inode, io->u.ci_setattr.sa_attr.lvb_size);
		inode_dio_write_done(inode);
	}
	mutex_unlock(&inode->i_mutex);
}

static void vvp_io_setattr_fini(const struct lu_env *env,
				const struct cl_io_slice *ios)
{
	vvp_io_fini(env, ios);
}

static int vvp_io_read_start(const struct lu_env *env,
                             const struct cl_io_slice *ios)
{
        struct vvp_io     *vio   = cl2vvp_io(env, ios);
        struct ccc_io     *cio   = cl2ccc_io(env, ios);
        struct cl_io      *io    = ios->cis_io;
        struct cl_object  *obj   = io->ci_obj;
        struct inode      *inode = ccc_object_inode(obj);
        struct ll_ra_read *bead  = &vio->cui_bead;
        struct file       *file  = cio->cui_fd->fd_file;

        int     result;
        loff_t  pos = io->u.ci_rd.rd.crw_pos;
        long    cnt = io->u.ci_rd.rd.crw_count;
        long    tot = cio->cui_tot_count;
        int     exceed = 0;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));

        CDEBUG(D_VFSTRACE, "read: -> [%lli, %lli)\n", pos, pos + cnt);

	if (!can_populate_pages(env, io, inode))
		return 0;

        result = ccc_prep_size(env, obj, io, pos, tot, &exceed);
        if (result != 0)
                return result;
        else if (exceed != 0)
                goto out;

        LU_OBJECT_HEADER(D_INODE, env, &obj->co_lu,
                        "Read ino %lu, %lu bytes, offset %lld, size %llu\n",
                        inode->i_ino, cnt, pos, i_size_read(inode));

        /* turn off the kernel's read-ahead */
        cio->cui_fd->fd_file->f_ra.ra_pages = 0;

        /* initialize read-ahead window once per syscall */
        if (!vio->cui_ra_window_set) {
                vio->cui_ra_window_set = 1;
                bead->lrr_start = cl_index(obj, pos);
		bead->lrr_count = cl_index(obj, tot + PAGE_CACHE_SIZE - 1);
                ll_ra_read_in(file, bead);
        }

        /* BUG: 5972 */
        file_accessed(file);
        switch (vio->cui_io_subtype) {
        case IO_NORMAL:
		LASSERT(cio->cui_iocb->ki_pos == pos);
		result = generic_file_aio_read(cio->cui_iocb,
					       cio->cui_iov, cio->cui_nrsegs,
					       cio->cui_iocb->ki_pos);
		break;
        case IO_SPLICE:
                result = generic_file_splice_read(file, &pos,
                                vio->u.splice.cui_pipe, cnt,
                                vio->u.splice.cui_flags);
                /* LU-1109: do splice read stripe by stripe otherwise if it
                 * may make nfsd stuck if this read occupied all internal pipe
                 * buffers. */
                io->ci_continue = 0;
                break;
        default:
                CERROR("Wrong IO type %u\n", vio->cui_io_subtype);
                LBUG();
        }

out:
	if (result >= 0) {
		if (result < cnt)
			io->ci_continue = 0;
		io->ci_nob += result;
		ll_rw_stats_tally(ll_i2sbi(inode), current->pid, cio->cui_fd,
				  pos, result, READ);
		result = 0;
	}

	return result;
}

static void vvp_io_read_fini(const struct lu_env *env, const struct cl_io_slice *ios)
{
	struct vvp_io *vio = cl2vvp_io(env, ios);
	struct ccc_io *cio = cl2ccc_io(env, ios);

	if (vio->cui_ra_window_set)
		ll_ra_read_ex(cio->cui_fd->fd_file, &vio->cui_bead);

	vvp_io_fini(env, ios);
}

static int vvp_io_commit_sync(const struct lu_env *env, struct cl_io *io,
			      struct cl_page_list *plist, int from, int to)
{
	struct cl_2queue *queue = &io->ci_queue;
	struct cl_page *page;
	unsigned int bytes = 0;
	int rc = 0;
	ENTRY;

	if (plist->pl_nr == 0)
		RETURN(0);

	if (from > 0 || to != PAGE_SIZE) {
		page = cl_page_list_first(plist);
		if (plist->pl_nr == 1) {
			cl_page_clip(env, page, from, to);
		} else {
			if (from > 0)
				cl_page_clip(env, page, from, PAGE_SIZE);
			if (to != PAGE_SIZE) {
				page = cl_page_list_last(plist);
				cl_page_clip(env, page, 0, to);
			}
		}
	}

	cl_2queue_init(queue);
	cl_page_list_splice(plist, &queue->c2_qin);
	rc = cl_io_submit_sync(env, io, CRT_WRITE, queue, 0);

	/* plist is not sorted any more */
	cl_page_list_splice(&queue->c2_qin, plist);
	cl_page_list_splice(&queue->c2_qout, plist);
	cl_2queue_fini(env, queue);

	if (rc == 0) {
		/* calculate bytes */
		bytes = plist->pl_nr << PAGE_SHIFT;
		bytes -= from + PAGE_SIZE - to;

		while (plist->pl_nr > 0) {
			page = cl_page_list_first(plist);
			cl_page_list_del(env, plist, page);

			cl_page_clip(env, page, 0, PAGE_SIZE);

			SetPageUptodate(cl_page_vmpage(page));
			cl_page_disown(env, io, page);

			/* held in ll_cl_init() */
			lu_ref_del(&page->cp_reference, "cl_io", io);
			cl_page_put(env, page);
		}
	}

	RETURN(bytes > 0 ? bytes : rc);
}

static void write_commit_callback(const struct lu_env *env, struct cl_io *io,
				struct cl_page *page)
{
	struct ccc_page *cp;
	struct page *vmpage = page->cp_vmpage;
	struct cl_object *clob = cl_io_top(io)->ci_obj;

	SetPageUptodate(vmpage);
	set_page_dirty(vmpage);

	cp = cl2ccc_page(cl_object_page_slice(clob, page));
	vvp_write_pending(cl2ccc(clob), cp);

	cl_page_disown(env, io, page);

	/* held in ll_cl_init() */
	lu_ref_del(&page->cp_reference, "cl_io", cl_io_top(io));
	cl_page_put(env, page);
}

/* make sure the page list is contiguous */
static bool page_list_sanity_check(struct cl_object *obj,
				   struct cl_page_list *plist)
{
	struct cl_page *page;
	pgoff_t index = CL_PAGE_EOF;

	cl_page_list_for_each(page, plist) {
		struct ccc_page *cp = cl_object_page_slice(obj, page);

		if (index == CL_PAGE_EOF) {
			index = ccc_index(cp);
			continue;
		}

		++index;
		if (index == ccc_index(cp))
			continue;

		return false;
	}
	return true;
}

/* Return how many bytes have queued or written */
int vvp_io_write_commit(const struct lu_env *env, struct cl_io *io)
{
	struct cl_object *obj = io->ci_obj;
	struct inode *inode = ccc_object_inode(obj);
	struct ccc_io *cio = ccc_env_io(env);
	struct cl_page_list *queue = &cio->u.write.cui_queue;
	struct cl_page *page;
	int rc = 0;
	int bytes = 0;
	unsigned int npages = cio->u.write.cui_queue.pl_nr;
	ENTRY;

	if (npages == 0)
		RETURN(0);

	CDEBUG(D_VFSTRACE, "commit async pages: %d, from %d, to %d\n",
		npages, cio->u.write.cui_from, cio->u.write.cui_to);

	LASSERT(page_list_sanity_check(obj, queue));

	/* submit IO with async write */
	rc = cl_io_commit_async(env, io, queue,
				cio->u.write.cui_from, cio->u.write.cui_to,
				write_commit_callback);
	npages -= queue->pl_nr; /* already committed pages */
	if (npages > 0) {
		/* calculate how many bytes were written */
		bytes = npages << PAGE_SHIFT;

		/* first page */
		bytes -= cio->u.write.cui_from;
		if (queue->pl_nr == 0) /* last page */
			bytes -= PAGE_SIZE - cio->u.write.cui_to;
		LASSERTF(bytes > 0, "bytes = %d, pages = %d\n", bytes, npages);

		cio->u.write.cui_written += bytes;

		CDEBUG(D_VFSTRACE, "Committed %d pages %d bytes, tot: %ld\n",
			npages, bytes, cio->u.write.cui_written);

		/* the first page must have been written. */
		cio->u.write.cui_from = 0;
	}
	LASSERT(page_list_sanity_check(obj, queue));
	LASSERT(ergo(rc == 0, queue->pl_nr == 0));

	/* out of quota, try sync write */
	if (rc == -EDQUOT && !cl_io_is_mkwrite(io)) {
		rc = vvp_io_commit_sync(env, io, queue,
					cio->u.write.cui_from,
					cio->u.write.cui_to);
		if (rc > 0) {
			cio->u.write.cui_written += rc;
			rc = 0;
		}
	}

	/* update inode size */
	ll_merge_lvb(env, inode);

	/* Now the pages in queue were failed to commit, discard them
	 * unless they were dirtied before. */
	while (queue->pl_nr > 0) {
		page = cl_page_list_first(queue);
		cl_page_list_del(env, queue, page);

		if (!PageDirty(cl_page_vmpage(page)))
			cl_page_discard(env, io, page);

		cl_page_disown(env, io, page);

		/* held in ll_cl_init() */
		lu_ref_del(&page->cp_reference, "cl_io", io);
		cl_page_put(env, page);
	}
	cl_page_list_fini(env, queue);

	RETURN(rc);
}

static int vvp_io_write_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
        struct ccc_io      *cio   = cl2ccc_io(env, ios);
        struct cl_io       *io    = ios->cis_io;
        struct cl_object   *obj   = io->ci_obj;
        struct inode       *inode = ccc_object_inode(obj);
        ssize_t result = 0;
        loff_t pos = io->u.ci_wr.wr.crw_pos;
        size_t cnt = io->u.ci_wr.wr.crw_count;

        ENTRY;

	if (!can_populate_pages(env, io, inode))
		RETURN(0);

        if (cl_io_is_append(io)) {
                /*
                 * PARALLEL IO This has to be changed for parallel IO doing
                 * out-of-order writes.
                 */
		ll_merge_lvb(env, inode);
                pos = io->u.ci_wr.wr.crw_pos = i_size_read(inode);
                cio->cui_iocb->ki_pos = pos;
        } else {
		LASSERT(cio->cui_iocb->ki_pos == pos);
	}

	CDEBUG(D_VFSTRACE, "write: [%lli, %lli)\n", pos, pos + (long long)cnt);

	if (cio->cui_iov == NULL) {
		/* from a temp io in ll_cl_init(). */
		result = 0;
	} else {
		/*
		 * When using the locked AIO function (generic_file_aio_write())
		 * testing has shown the inode mutex to be a limiting factor
		 * with multi-threaded single shared file performance. To get
		 * around this, we now use the lockless version. To maintain
		 * consistency, proper locking to protect against writes,
		 * trucates, etc. is handled in the higher layers of lustre.
		 */
		result = __generic_file_aio_write(cio->cui_iocb,
						  cio->cui_iov, cio->cui_nrsegs,
						  &cio->cui_iocb->ki_pos);
		if (result > 0 || result == -EIOCBQUEUED) {
			ssize_t err;

			err = generic_write_sync(cio->cui_iocb->ki_filp,
						 pos, result);
			if (err < 0 && result > 0)
				result = err;
		}

	}
	if (result > 0) {
		result = vvp_io_write_commit(env, io);
		if (cio->u.write.cui_written > 0) {
			result = cio->u.write.cui_written;
			io->ci_nob += result;

			CDEBUG(D_VFSTRACE, "write: nob %zd, result: %zd\n",
				io->ci_nob, result);
		}
	}
	if (result > 0) {
		struct ll_inode_info *lli = ll_i2info(inode);

		spin_lock(&lli->lli_lock);
		lli->lli_flags |= LLIF_DATA_MODIFIED;
		spin_unlock(&lli->lli_lock);

		if (result < cnt)
			io->ci_continue = 0;
		ll_rw_stats_tally(ll_i2sbi(inode), current->pid,
				  cio->cui_fd, pos, result, WRITE);
		result = 0;
	}

	RETURN(result);
}

static int vvp_io_kernel_fault(struct vvp_fault_io *cfio)
{
        struct vm_fault *vmf = cfio->fault.ft_vmf;

        cfio->fault.ft_flags = filemap_fault(cfio->ft_vma, vmf);
	cfio->fault.ft_flags_valid = 1;

        if (vmf->page) {
                LL_CDEBUG_PAGE(D_PAGE, vmf->page, "got addr %p type NOPAGE\n",
                               vmf->virtual_address);
                if (unlikely(!(cfio->fault.ft_flags & VM_FAULT_LOCKED))) {
                        lock_page(vmf->page);
			cfio->fault.ft_flags |= VM_FAULT_LOCKED;
                }

                cfio->ft_vmpage = vmf->page;
                return 0;
        }

        if (cfio->fault.ft_flags & VM_FAULT_SIGBUS) {
                CDEBUG(D_PAGE, "got addr %p - SIGBUS\n", vmf->virtual_address);
                return -EFAULT;
        }

        if (cfio->fault.ft_flags & VM_FAULT_OOM) {
                CDEBUG(D_PAGE, "got addr %p - OOM\n", vmf->virtual_address);
                return -ENOMEM;
        }

        if (cfio->fault.ft_flags & VM_FAULT_RETRY)
                return -EAGAIN;

        CERROR("unknow error in page fault %d!\n", cfio->fault.ft_flags);
        return -EINVAL;
}

static void mkwrite_commit_callback(const struct lu_env *env, struct cl_io *io,
				    struct cl_page *page)
{
	struct ccc_page *cp;
	struct cl_object *clob = cl_io_top(io)->ci_obj;

	set_page_dirty(page->cp_vmpage);

	cp = cl2ccc_page(cl_object_page_slice(clob, page));
	vvp_write_pending(cl2ccc(clob), cp);
}

static int vvp_io_fault_start(const struct lu_env *env,
                              const struct cl_io_slice *ios)
{
	struct vvp_io       *vio     = cl2vvp_io(env, ios);
	struct cl_io        *io      = ios->cis_io;
	struct cl_object    *obj     = io->ci_obj;
	struct inode        *inode   = ccc_object_inode(obj);
	struct cl_fault_io  *fio     = &io->u.ci_fault;
	struct vvp_fault_io *cfio    = &vio->u.fault;
	loff_t               offset;
	int                  result  = 0;
	struct page          *vmpage  = NULL;
	struct cl_page      *page;
	loff_t               size;
	pgoff_t		     last_index;
	ENTRY;

        if (fio->ft_executable &&
            LTIME_S(inode->i_mtime) != vio->u.fault.ft_mtime)
                CWARN("binary "DFID
                      " changed while waiting for the page fault lock\n",
                      PFID(lu_object_fid(&obj->co_lu)));

        /* offset of the last byte on the page */
        offset = cl_offset(obj, fio->ft_index + 1) - 1;
        LASSERT(cl_index(obj, offset) == fio->ft_index);
        result = ccc_prep_size(env, obj, io, 0, offset + 1, NULL);
	if (result != 0)
		RETURN(result);

	/* must return locked page */
	if (fio->ft_mkwrite) {
		LASSERT(cfio->ft_vmpage != NULL);
		lock_page(cfio->ft_vmpage);
	} else {
		result = vvp_io_kernel_fault(cfio);
		if (result != 0)
			RETURN(result);
	}

	vmpage = cfio->ft_vmpage;
	LASSERT(PageLocked(vmpage));

	if (OBD_FAIL_CHECK(OBD_FAIL_LLITE_FAULT_TRUNC_RACE))
		ll_invalidate_page(vmpage);

	size = i_size_read(inode);
        /* Though we have already held a cl_lock upon this page, but
         * it still can be truncated locally. */
	if (unlikely((vmpage->mapping != inode->i_mapping) ||
		     (page_offset(vmpage) > size))) {
                CDEBUG(D_PAGE, "llite: fault and truncate race happened!\n");

                /* return +1 to stop cl_io_loop() and ll_fault() will catch
                 * and retry. */
                GOTO(out, result = +1);
        }

	last_index = cl_index(obj, size - 1);

	if (fio->ft_mkwrite ) {
		/*
		 * Capture the size while holding the lli_trunc_sem from above
		 * we want to make sure that we complete the mkwrite action
		 * while holding this lock. We need to make sure that we are
		 * not past the end of the file.
		 */
		if (last_index < fio->ft_index) {
			CDEBUG(D_PAGE,
				"llite: mkwrite and truncate race happened: "
				"%p: 0x%lx 0x%lx\n",
				vmpage->mapping,fio->ft_index,last_index);
			/*
			 * We need to return if we are
			 * passed the end of the file. This will propagate
			 * up the call stack to ll_page_mkwrite where
			 * we will return VM_FAULT_NOPAGE. Any non-negative
			 * value returned here will be silently
			 * converted to 0. If the vmpage->mapping is null
			 * the error code would be converted back to ENODATA
			 * in ll_page_mkwrite0. Thus we return -ENODATA
			 * to handle both cases
			 */
			GOTO(out, result = -ENODATA);
		}
	}

	page = cl_page_find(env, obj, fio->ft_index, vmpage, CPT_CACHEABLE);
	if (IS_ERR(page))
		GOTO(out, result = PTR_ERR(page));

	/* if page is going to be written, we should add this page into cache
	 * earlier. */
	if (fio->ft_mkwrite) {
		wait_on_page_writeback(vmpage);
		if (!PageDirty(vmpage)) {
			struct cl_page_list *plist = &io->ci_queue.c2_qin;
			struct ccc_page *cp = cl_object_page_slice(obj, page);
			int to = PAGE_SIZE;

			/* vvp_page_assume() calls wait_on_page_writeback(). */
			cl_page_assume(env, io, page);

			cl_page_list_init(plist);
			cl_page_list_add(plist, page);

			/* size fixup */
			if (last_index == ccc_index(cp))
				to = size & ~CFS_PAGE_MASK;

			/* Do not set Dirty bit here so that in case IO is
			 * started before the page is really made dirty, we
			 * still have chance to detect it. */
			result = cl_io_commit_async(env, io, plist, 0, to,
						    mkwrite_commit_callback);
			LASSERT(cl_page_is_owned(page, io));
			cl_page_list_fini(env, plist);

			vmpage = NULL;
			if (result < 0) {
				cl_page_discard(env, io, page);
				cl_page_disown(env, io, page);

				cl_page_put(env, page);

				/* we're in big trouble, what can we do now? */
				if (result == -EDQUOT)
					result = -ENOSPC;
				GOTO(out, result);
			} else
				cl_page_disown(env, io, page);
		}
	}

	/*
	 * The ft_index is only used in the case of
	 * a mkwrite action. We need to check
	 * our assertions are correct, since
	 * we should have caught this above
	 */
	LASSERT(!fio->ft_mkwrite || fio->ft_index <= last_index);
	if (fio->ft_index == last_index)
                /*
                 * Last page is mapped partially.
                 */
                fio->ft_nob = size - cl_offset(obj, fio->ft_index);
        else
                fio->ft_nob = cl_page_size(obj);

        lu_ref_add(&page->cp_reference, "fault", io);
        fio->ft_page = page;
        EXIT;

out:
	/* return unlocked vmpage to avoid deadlocking */
	if (vmpage != NULL)
		unlock_page(vmpage);
	cfio->fault.ft_flags &= ~VM_FAULT_LOCKED;
	return result;
}

static int vvp_io_fsync_start(const struct lu_env *env,
			      const struct cl_io_slice *ios)
{
	/* we should mark TOWRITE bit to each dirty page in radix tree to
	 * verify pages have been written, but this is difficult because of
	 * race. */
	return 0;
}

static int vvp_io_read_page(const struct lu_env *env,
                            const struct cl_io_slice *ios,
                            const struct cl_page_slice *slice)
{
	struct cl_io              *io     = ios->cis_io;
	struct ccc_page           *cp     = cl2ccc_page(slice);
	struct cl_page            *page   = slice->cpl_page;
	struct inode              *inode  = ccc_object_inode(slice->cpl_obj);
	struct ll_sb_info         *sbi    = ll_i2sbi(inode);
	struct ll_file_data       *fd     = cl2ccc_io(env, ios)->cui_fd;
	struct ll_readahead_state *ras    = &fd->fd_ras;
	struct cl_2queue          *queue  = &io->ci_queue;

	ENTRY;

	if (sbi->ll_ra_info.ra_max_pages_per_file > 0 &&
	    sbi->ll_ra_info.ra_max_pages > 0)
		ras_update(sbi, inode, ras, ccc_index(cp),
			   cp->cpg_defer_uptodate);

        if (cp->cpg_defer_uptodate) {
                cp->cpg_ra_used = 1;
                cl_page_export(env, page, 1);
        }

        /*
         * Add page into the queue even when it is marked uptodate above.
         * this will unlock it automatically as part of cl_page_list_disown().
         */
        cl_2queue_add(queue, page);
	if (sbi->ll_ra_info.ra_max_pages_per_file > 0 &&
	    sbi->ll_ra_info.ra_max_pages > 0)
		ll_readahead(env, io, &queue->c2_qin, ras,
			     cp->cpg_defer_uptodate);

	RETURN(0);
}

static const struct cl_io_operations vvp_io_ops = {
        .op = {
                [CIT_READ] = {
                        .cio_fini      = vvp_io_read_fini,
                        .cio_lock      = vvp_io_read_lock,
                        .cio_start     = vvp_io_read_start,
                        .cio_advance   = ccc_io_advance
                },
                [CIT_WRITE] = {
			.cio_fini      = vvp_io_fini,
			.cio_iter_init = vvp_io_write_iter_init,
			.cio_iter_fini = vvp_io_write_iter_fini,
			.cio_lock      = vvp_io_write_lock,
			.cio_start     = vvp_io_write_start,
			.cio_advance   = ccc_io_advance
                },
                [CIT_SETATTR] = {
                        .cio_fini       = vvp_io_setattr_fini,
                        .cio_iter_init  = vvp_io_setattr_iter_init,
                        .cio_lock       = vvp_io_setattr_lock,
                        .cio_start      = vvp_io_setattr_start,
                        .cio_end        = vvp_io_setattr_end
                },
                [CIT_FAULT] = {
                        .cio_fini      = vvp_io_fault_fini,
                        .cio_iter_init = vvp_io_fault_iter_init,
                        .cio_lock      = vvp_io_fault_lock,
                        .cio_start     = vvp_io_fault_start,
                        .cio_end       = ccc_io_end
                },
		[CIT_FSYNC] = {
			.cio_start  = vvp_io_fsync_start,
			.cio_fini   = vvp_io_fini
		},
                [CIT_MISC] = {
                        .cio_fini   = vvp_io_fini
                }
        },
        .cio_read_page     = vvp_io_read_page,
};

int vvp_io_init(const struct lu_env *env, struct cl_object *obj,
                struct cl_io *io)
{
	struct vvp_io      *vio   = vvp_env_io(env);
	struct ccc_io      *cio   = ccc_env_io(env);
	struct inode       *inode = ccc_object_inode(obj);
        int                 result;

        CLOBINVRNT(env, obj, ccc_object_invariant(obj));
        ENTRY;

	CDEBUG(D_VFSTRACE, DFID" ignore/verify layout %d/%d, layout version %d "
			   "restore needed %d\n",
	       PFID(lu_object_fid(&obj->co_lu)),
	       io->ci_ignore_layout, io->ci_verify_layout,
	       cio->cui_layout_gen, io->ci_restore_needed);

        CL_IO_SLICE_CLEAN(cio, cui_cl);
        cl_io_slice_add(io, &cio->cui_cl, obj, &vvp_io_ops);
        vio->cui_ra_window_set = 0;
	result = 0;
	if (io->ci_type == CIT_READ || io->ci_type == CIT_WRITE) {
		size_t count;
		struct ll_inode_info *lli = ll_i2info(inode);

                count = io->u.ci_rw.crw_count;
                /* "If nbyte is 0, read() will return 0 and have no other
                 *  results."  -- Single Unix Spec */
                if (count == 0)
                        result = 1;
                else {
                        cio->cui_tot_count = count;
                        cio->cui_tot_nrsegs = 0;
                }

		/* for read/write, we store the jobid in the inode, and
		 * it'll be fetched by osc when building RPC.
		 *
		 * it's not accurate if the file is shared by different
		 * jobs.
		 */
		lustre_get_jobid(lli->lli_jobid);
	} else if (io->ci_type == CIT_SETATTR) {
		if (!cl_io_is_trunc(io))
			io->ci_lockreq = CILR_MANDATORY;
	}

	/* ignore layout change for generic CIT_MISC but not for glimpse.
	 * io context for glimpse must set ci_verify_layout to true,
	 * see cl_glimpse_size0() for details. */
	if (io->ci_type == CIT_MISC && !io->ci_verify_layout)
		io->ci_ignore_layout = 1;

	/* Enqueue layout lock and get layout version. We need to do this
	 * even for operations requiring to open file, such as read and write,
	 * because it might not grant layout lock in IT_OPEN. */
	if (result == 0 && !io->ci_ignore_layout) {
		result = ll_layout_refresh(inode, &cio->cui_layout_gen);
		if (result == -ENOENT)
			/* If the inode on MDS has been removed, but the objects
			 * on OSTs haven't been destroyed (async unlink), layout
			 * fetch will return -ENOENT, we'd ingore this error
			 * and continue with dirty flush. LU-3230. */
			result = 0;
		if (result < 0)
			CERROR("%s: refresh file layout " DFID " error %d.\n",
				ll_get_fsname(inode->i_sb, NULL, 0),
				PFID(lu_object_fid(&obj->co_lu)), result);
	}

	RETURN(result);
}

static struct vvp_io *cl2vvp_io(const struct lu_env *env,
                                const struct cl_io_slice *slice)
{
        /* Caling just for assertion */
        cl2ccc_io(env, slice);
        return vvp_env_io(env);
}
