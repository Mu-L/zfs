// SPDX-License-Identifier: CDDL-1.0
/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or https://opensource.org/licenses/CDDL-1.0.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2012, 2015 by Delphix. All rights reserved.
 * Copyright (c) 2014 Integros [integros.com]
 * Copyright 2017 Nexenta Systems, Inc.
 * Copyright (c) 2025, Klara, Inc.
 */

/* Portions Copyright 2007 Jeremy Teo */
/* Portions Copyright 2010 Robert Milkowski */

#include <sys/param.h>
#include <sys/time.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <security/mac/mac_framework.h>
#include <sys/vfs.h>
#include <sys/endian.h>
#include <sys/vm.h>
#include <sys/vnode.h>
#include <sys/smr.h>
#include <sys/dirent.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/kmem.h>
#include <sys/taskq.h>
#include <sys/uio.h>
#include <sys/atomic.h>
#include <sys/namei.h>
#include <sys/mman.h>
#include <sys/cmn_err.h>
#include <sys/kdb.h>
#include <sys/sysproto.h>
#include <sys/errno.h>
#include <sys/unistd.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/policy.h>
#include <sys/sunddi.h>
#include <sys/filio.h>
#include <sys/sid.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_fuid.h>
#include <sys/zfs_quota.h>
#include <sys/zfs_sa.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_project.h>
#include <sys/bio.h>
#include <sys/buf.h>
#include <sys/sched.h>
#include <sys/acl.h>
#include <sys/vmmeter.h>
#include <vm/vm_param.h>
#include <sys/zil.h>
#include <sys/zfs_vnops.h>
#include <sys/module.h>
#include <sys/sysent.h>
#include <sys/dmu_impl.h>
#include <sys/brt.h>
#include <sys/zfeature.h>

#include <vm/vm_object.h>

#include <sys/extattr.h>
#include <sys/priv.h>

#ifndef VN_OPEN_INVFS
#define	VN_OPEN_INVFS	0x0
#endif

VFS_SMR_DECLARE;

#ifdef DEBUG_VFS_LOCKS
#define	VNCHECKREF(vp)				  \
	VNASSERT((vp)->v_holdcnt > 0 && (vp)->v_usecount > 0, vp,	\
	    ("%s: wrong ref counts", __func__));
#else
#define	VNCHECKREF(vp)
#endif

#if __FreeBSD_version >= 1400045
typedef uint64_t cookie_t;
#else
typedef ulong_t cookie_t;
#endif

static int zfs_check_attrname(const char *name);

/*
 * Programming rules.
 *
 * Each vnode op performs some logical unit of work.  To do this, the ZPL must
 * properly lock its in-core state, create a DMU transaction, do the work,
 * record this work in the intent log (ZIL), commit the DMU transaction,
 * and wait for the intent log to commit if it is a synchronous operation.
 * Moreover, the vnode ops must work in both normal and log replay context.
 * The ordering of events is important to avoid deadlocks and references
 * to freed memory.  The example below illustrates the following Big Rules:
 *
 *  (1)	A check must be made in each zfs thread for a mounted file system.
 *	This is done avoiding races using zfs_enter(zfsvfs).
 *	A zfs_exit(zfsvfs) is needed before all returns.  Any znodes
 *	must be checked with zfs_verify_zp(zp).  Both of these macros
 *	can return EIO from the calling function.
 *
 *  (2)	VN_RELE() should always be the last thing except for zil_commit()
 *	(if necessary) and zfs_exit(). This is for 3 reasons:
 *	First, if it's the last reference, the vnode/znode
 *	can be freed, so the zp may point to freed memory.  Second, the last
 *	reference will call zfs_zinactive(), which may induce a lot of work --
 *	pushing cached pages (which acquires range locks) and syncing out
 *	cached atime changes.  Third, zfs_zinactive() may require a new tx,
 *	which could deadlock the system if you were already holding one.
 *	If you must call VN_RELE() within a tx then use VN_RELE_ASYNC().
 *
 *  (3)	All range locks must be grabbed before calling dmu_tx_assign(),
 *	as they can span dmu_tx_assign() calls.
 *
 *  (4) If ZPL locks are held, pass DMU_TX_NOWAIT as the second argument to
 *      dmu_tx_assign().  This is critical because we don't want to block
 *      while holding locks.
 *
 *	If no ZPL locks are held (aside from zfs_enter()), use DMU_TX_WAIT.
 *	This reduces lock contention and CPU usage when we must wait (note
 *	that if throughput is constrained by the storage, nearly every
 *	transaction must wait).
 *
 *      Note, in particular, that if a lock is sometimes acquired before
 *      the tx assigns, and sometimes after (e.g. z_lock), then failing
 *      to use a non-blocking assign can deadlock the system.  The scenario:
 *
 *	Thread A has grabbed a lock before calling dmu_tx_assign().
 *	Thread B is in an already-assigned tx, and blocks for this lock.
 *	Thread A calls dmu_tx_assign(DMU_TX_WAIT) and blocks in
 *	txg_wait_open() forever, because the previous txg can't quiesce
 *	until B's tx commits.
 *
 *	If dmu_tx_assign() returns ERESTART and zfsvfs->z_assign is
 *	DMU_TX_NOWAIT, then drop all locks, call dmu_tx_wait(), and try
 *	again.  On subsequent calls to dmu_tx_assign(), pass
 *	DMU_TX_NOTHROTTLE in addition to DMU_TX_NOWAIT, to indicate that
 *	this operation has already called dmu_tx_wait().  This will ensure
 *	that we don't retry forever, waiting a short bit each time.
 *
 *  (5)	If the operation succeeded, generate the intent log entry for it
 *	before dropping locks.  This ensures that the ordering of events
 *	in the intent log matches the order in which they actually occurred.
 *	During ZIL replay the zfs_log_* functions will update the sequence
 *	number to indicate the zil transaction has replayed.
 *
 *  (6)	At the end of each vnode op, the DMU tx must always commit,
 *	regardless of whether there were any errors.
 *
 *  (7)	After dropping all locks, invoke zil_commit(zilog, foid)
 *	to ensure that synchronous semantics are provided when necessary.
 *
 * In general, this is how things should be ordered in each vnode op:
 *
 *	zfs_enter(zfsvfs);		// exit if unmounted
 * top:
 *	zfs_dirent_lookup(&dl, ...)	// lock directory entry (may VN_HOLD())
 *	rw_enter(...);			// grab any other locks you need
 *	tx = dmu_tx_create(...);	// get DMU tx
 *	dmu_tx_hold_*();		// hold each object you might modify
 *	error = dmu_tx_assign(tx,
 *	    (waited ? DMU_TX_NOTHROTTLE : 0) | DMU_TX_NOWAIT);
 *	if (error) {
 *		rw_exit(...);		// drop locks
 *		zfs_dirent_unlock(dl);	// unlock directory entry
 *		VN_RELE(...);		// release held vnodes
 *		if (error == ERESTART) {
 *			waited = B_TRUE;
 *			dmu_tx_wait(tx);
 *			dmu_tx_abort(tx);
 *			goto top;
 *		}
 *		dmu_tx_abort(tx);	// abort DMU tx
 *		zfs_exit(zfsvfs);	// finished in zfs
 *		return (error);		// really out of space
 *	}
 *	error = do_real_work();		// do whatever this VOP does
 *	if (error == 0)
 *		zfs_log_*(...);		// on success, make ZIL entry
 *	dmu_tx_commit(tx);		// commit DMU tx -- error or not
 *	rw_exit(...);			// drop locks
 *	zfs_dirent_unlock(dl);		// unlock directory entry
 *	VN_RELE(...);			// release held vnodes
 *	zil_commit(zilog, foid);	// synchronous when necessary
 *	zfs_exit(zfsvfs);		// finished in zfs
 *	return (error);			// done, report error
 */
static int
zfs_open(vnode_t **vpp, int flag, cred_t *cr)
{
	(void) cr;
	znode_t	*zp = VTOZ(*vpp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((flag & FWRITE) && (zp->z_pflags & ZFS_APPENDONLY) &&
	    ((flag & FAPPEND) == 0)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Keep a count of the synchronous opens in the znode.  On first
	 * synchronous open we must convert all previous async transactions
	 * into sync to keep correct ordering.
	 */
	if (flag & O_SYNC) {
		if (atomic_inc_32_nv(&zp->z_sync_cnt) == 1)
			zil_async_to_sync(zfsvfs->z_log, zp->z_id);
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_close(vnode_t *vp, int flag, int count, offset_t offset, cred_t *cr)
{
	(void) offset, (void) cr;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/* Decrement the synchronous opens in the znode */
	if ((flag & O_SYNC) && (count == 1))
		atomic_dec_32(&zp->z_sync_cnt);

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_ioctl_getxattr(vnode_t *vp, zfsxattr_t *fsx)
{
	znode_t *zp = VTOZ(vp);

	memset(fsx, 0, sizeof (*fsx));
	fsx->fsx_xflags = (zp->z_pflags & ZFS_PROJINHERIT) ?
	    ZFS_PROJINHERIT_FL : 0;
	fsx->fsx_projid = zp->z_projid;

	return (0);
}

static int
zfs_ioctl_setflags(vnode_t *vp, uint32_t ioctl_flags, xvattr_t *xva)
{
	uint64_t zfs_flags = VTOZ(vp)->z_pflags;
	xoptattr_t *xoap;

	if (ioctl_flags & ~(ZFS_PROJINHERIT_FL))
		return (SET_ERROR(EOPNOTSUPP));

	xva_init(xva);
	xoap = xva_getxoptattr(xva);

#define	FLAG_CHANGE(iflag, zflag, xflag, xfield)	do {		\
	if (((ioctl_flags & (iflag)) && !(zfs_flags & (zflag))) ||	\
	    ((zfs_flags & (zflag)) && !(ioctl_flags & (iflag)))) {	\
		XVA_SET_REQ(xva, (xflag));				\
		(xfield) = ((ioctl_flags & (iflag)) != 0);		\
	}								\
} while (0)

	FLAG_CHANGE(ZFS_PROJINHERIT_FL, ZFS_PROJINHERIT, XAT_PROJINHERIT,
	    xoap->xoa_projinherit);

#undef	FLAG_CHANGE

	return (0);
}

static int
zfs_ioctl_setxattr(vnode_t *vp, zfsxattr_t *fsx, cred_t *cr)
{
	znode_t *zp = VTOZ(vp);
	xvattr_t xva;
	xoptattr_t *xoap;
	int err;

	if (!zpl_is_valid_projid(fsx->fsx_projid))
		return (SET_ERROR(EINVAL));

	err = zfs_ioctl_setflags(vp, fsx->fsx_xflags, &xva);
	if (err)
		return (err);

	xoap = xva_getxoptattr(&xva);
	XVA_SET_REQ(&xva, XAT_PROJID);
	xoap->xoa_projid = fsx->fsx_projid;

	err = zfs_setattr(zp, (vattr_t *)&xva, 0, cr, NULL);

	return (err);
}

static int
zfs_ioctl(vnode_t *vp, ulong_t com, intptr_t data, int flag, cred_t *cred,
    int *rvalp)
{
	(void) flag, (void) cred, (void) rvalp;
	loff_t off;
	int error;

	switch (com) {
	case _FIOFFS:
	{
		return (0);

		/*
		 * The following two ioctls are used by bfu.  Faking out,
		 * necessary to avoid bfu errors.
		 */
	}
	case _FIOGDIO:
	case _FIOSDIO:
	{
		return (0);
	}

	case F_SEEK_DATA:
	case F_SEEK_HOLE:
	{
		off = *(offset_t *)data;
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		/* offset parameter is in/out */
		error = zfs_holey(VTOZ(vp), com, &off);
		VOP_UNLOCK(vp);
		if (error)
			return (error);
		*(offset_t *)data = off;
		return (0);
	}
	case ZFS_IOC_FSGETXATTR: {
		zfsxattr_t *fsx = (zfsxattr_t *)data;
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		error = zfs_ioctl_getxattr(vp, fsx);
		VOP_UNLOCK(vp);
		return (error);
	}
	case ZFS_IOC_FSSETXATTR: {
		zfsxattr_t *fsx = (zfsxattr_t *)data;
		error = vn_lock(vp, LK_EXCLUSIVE);
		if (error)
			return (error);
		error = zfs_ioctl_setxattr(vp, fsx, cred);
		VOP_UNLOCK(vp);
		return (error);
	}
	case ZFS_IOC_REWRITE: {
		zfs_rewrite_args_t *args = (zfs_rewrite_args_t *)data;
		if ((flag & FWRITE) == 0)
			return (SET_ERROR(EBADF));
		error = vn_lock(vp, LK_SHARED);
		if (error)
			return (error);
		error = zfs_rewrite(VTOZ(vp), args->off, args->len,
		    args->flags, args->arg);
		VOP_UNLOCK(vp);
		return (error);
	}
	}
	return (SET_ERROR(ENOTTY));
}

static vm_page_t
page_busy(vnode_t *vp, int64_t start, int64_t off, int64_t nbytes)
{
	vm_object_t obj;
	vm_page_t pp;
	int64_t end;

	/*
	 * At present vm_page_clear_dirty extends the cleared range to DEV_BSIZE
	 * aligned boundaries, if the range is not aligned.  As a result a
	 * DEV_BSIZE subrange with partially dirty data may get marked as clean.
	 * It may happen that all DEV_BSIZE subranges are marked clean and thus
	 * the whole page would be considered clean despite have some
	 * dirty data.
	 * For this reason we should shrink the range to DEV_BSIZE aligned
	 * boundaries before calling vm_page_clear_dirty.
	 */
	end = rounddown2(off + nbytes, DEV_BSIZE);
	off = roundup2(off, DEV_BSIZE);
	nbytes = end - off;

	obj = vp->v_object;
	vm_page_grab_valid_unlocked(&pp, obj, OFF_TO_IDX(start),
	    VM_ALLOC_NOCREAT | VM_ALLOC_SBUSY | VM_ALLOC_NORMAL |
	    VM_ALLOC_IGN_SBUSY);
	if (pp != NULL) {
		ASSERT3U(pp->valid, ==, VM_PAGE_BITS_ALL);
		vm_object_pip_add(obj, 1);
		pmap_remove_write(pp);
		if (nbytes != 0)
			vm_page_clear_dirty(pp, off, nbytes);
	}
	return (pp);
}

static void
page_unbusy(vm_page_t pp)
{

	vm_page_sunbusy(pp);
	vm_object_pip_wakeup(pp->object);
}

static vm_page_t
page_hold(vnode_t *vp, int64_t start)
{
	vm_object_t obj;
	vm_page_t m;

	obj = vp->v_object;
	vm_page_grab_valid_unlocked(&m, obj, OFF_TO_IDX(start),
	    VM_ALLOC_NOCREAT | VM_ALLOC_WIRED | VM_ALLOC_IGN_SBUSY |
	    VM_ALLOC_NOBUSY);
	return (m);
}

static void
page_unhold(vm_page_t pp)
{
	vm_page_unwire(pp, PQ_ACTIVE);
}

/*
 * When a file is memory mapped, we must keep the IO data synchronized
 * between the DMU cache and the memory mapped pages.  What this means:
 *
 * On Write:	If we find a memory mapped page, we write to *both*
 *		the page and the dmu buffer.
 */
void
update_pages(znode_t *zp, int64_t start, int len, objset_t *os)
{
	vm_object_t obj;
	struct sf_buf *sf;
	vnode_t *vp = ZTOV(zp);
	caddr_t va;
	int off;

	ASSERT3P(vp->v_mount, !=, NULL);
	obj = vp->v_object;
	ASSERT3P(obj, !=, NULL);

	off = start & PAGEOFFSET;
	vm_object_pip_add(obj, 1);
	for (start &= PAGEMASK; len > 0; start += PAGESIZE) {
		vm_page_t pp;
		int nbytes = imin(PAGESIZE - off, len);

		if ((pp = page_busy(vp, start, off, nbytes)) != NULL) {
			va = zfs_map_page(pp, &sf);
			(void) dmu_read(os, zp->z_id, start + off, nbytes,
			    va + off, DMU_READ_PREFETCH);
			zfs_unmap_page(sf);
			page_unbusy(pp);
		}
		len -= nbytes;
		off = 0;
	}
	vm_object_pip_wakeup(obj);
}

/*
 * Read with UIO_NOCOPY flag means that sendfile(2) requests
 * ZFS to populate a range of page cache pages with data.
 *
 * NOTE: this function could be optimized to pre-allocate
 * all pages in advance, drain exclusive busy on all of them,
 * map them into contiguous KVA region and populate them
 * in one single dmu_read() call.
 */
int
mappedread_sf(znode_t *zp, int nbytes, zfs_uio_t *uio)
{
	vnode_t *vp = ZTOV(zp);
	objset_t *os = zp->z_zfsvfs->z_os;
	struct sf_buf *sf;
	vm_object_t obj;
	vm_page_t pp;
	int64_t start;
	caddr_t va;
	int len = nbytes;
	int error = 0;

	ASSERT3U(zfs_uio_segflg(uio), ==, UIO_NOCOPY);
	ASSERT3P(vp->v_mount, !=, NULL);
	obj = vp->v_object;
	ASSERT3P(obj, !=, NULL);
	ASSERT0(zfs_uio_offset(uio) & PAGEOFFSET);

	for (start = zfs_uio_offset(uio); len > 0; start += PAGESIZE) {
		int bytes = MIN(PAGESIZE, len);

		pp = vm_page_grab_unlocked(obj, OFF_TO_IDX(start),
		    VM_ALLOC_SBUSY | VM_ALLOC_NORMAL | VM_ALLOC_IGN_SBUSY);
		if (vm_page_none_valid(pp)) {
			va = zfs_map_page(pp, &sf);
			error = dmu_read(os, zp->z_id, start, bytes, va,
			    DMU_READ_PREFETCH);
			if (bytes != PAGESIZE && error == 0)
				memset(va + bytes, 0, PAGESIZE - bytes);
			zfs_unmap_page(sf);
			if (error == 0) {
				vm_page_valid(pp);
				vm_page_activate(pp);
				vm_page_sunbusy(pp);
			} else {
				zfs_vmobject_wlock(obj);
				if (!vm_page_wired(pp) && pp->valid == 0 &&
				    vm_page_busy_tryupgrade(pp))
					vm_page_free(pp);
				else {
					vm_page_deactivate_noreuse(pp);
					vm_page_sunbusy(pp);
				}
				zfs_vmobject_wunlock(obj);
			}
		} else {
			ASSERT3U(pp->valid, ==, VM_PAGE_BITS_ALL);
			vm_page_sunbusy(pp);
		}
		if (error)
			break;
		zfs_uio_advance(uio, bytes);
		len -= bytes;
	}
	return (error);
}

/*
 * When a file is memory mapped, we must keep the IO data synchronized
 * between the DMU cache and the memory mapped pages.  What this means:
 *
 * On Read:	We "read" preferentially from memory mapped pages,
 *		else we default from the dmu buffer.
 *
 * NOTE: We will always "break up" the IO into PAGESIZE uiomoves when
 *	 the file is memory mapped.
 */
int
mappedread(znode_t *zp, int nbytes, zfs_uio_t *uio)
{
	vnode_t *vp = ZTOV(zp);
	vm_object_t obj;
	int64_t start;
	int len = nbytes;
	int off;
	int error = 0;

	ASSERT3P(vp->v_mount, !=, NULL);
	obj = vp->v_object;
	ASSERT3P(obj, !=, NULL);

	start = zfs_uio_offset(uio);
	off = start & PAGEOFFSET;
	for (start &= PAGEMASK; len > 0; start += PAGESIZE) {
		vm_page_t pp;
		uint64_t bytes = MIN(PAGESIZE - off, len);

		if ((pp = page_hold(vp, start))) {
			struct sf_buf *sf;
			caddr_t va;

			va = zfs_map_page(pp, &sf);
			error = vn_io_fault_uiomove(va + off, bytes,
			    GET_UIO_STRUCT(uio));
			zfs_unmap_page(sf);
			page_unhold(pp);
		} else {
			error = dmu_read_uio_dbuf(sa_get_db(zp->z_sa_hdl),
			    uio, bytes, DMU_READ_PREFETCH);
		}
		len -= bytes;
		off = 0;
		if (error)
			break;
	}
	return (error);
}

int
zfs_write_simple(znode_t *zp, const void *data, size_t len,
    loff_t pos, size_t *presid)
{
	int error = 0;
	ssize_t resid;

	error = vn_rdwr(UIO_WRITE, ZTOV(zp), __DECONST(void *, data), len, pos,
	    UIO_SYSSPACE, IO_SYNC, kcred, NOCRED, &resid, curthread);

	if (error) {
		return (SET_ERROR(error));
	} else if (presid == NULL) {
		if (resid != 0) {
			error = SET_ERROR(EIO);
		}
	} else {
		*presid = resid;
	}
	return (error);
}

void
zfs_zrele_async(znode_t *zp)
{
	vnode_t *vp = ZTOV(zp);
	objset_t *os = ITOZSB(vp)->z_os;

	VN_RELE_ASYNC(vp, dsl_pool_zrele_taskq(dmu_objset_pool(os)));
}

static int
zfs_dd_callback(struct mount *mp, void *arg, int lkflags, struct vnode **vpp)
{
	int error;

	*vpp = arg;
	error = vn_lock(*vpp, lkflags);
	if (error != 0)
		vrele(*vpp);
	return (error);
}

static int
zfs_lookup_lock(vnode_t *dvp, vnode_t *vp, const char *name, int lkflags)
{
	znode_t *zdp = VTOZ(dvp);
	zfsvfs_t *zfsvfs __unused = zdp->z_zfsvfs;
	int error;
	int ltype;

	if (zfsvfs->z_replay == B_FALSE)
		ASSERT_VOP_LOCKED(dvp, __func__);

	if (name[0] == 0 || (name[0] == '.' && name[1] == 0)) {
		ASSERT3P(dvp, ==, vp);
		vref(dvp);
		ltype = lkflags & LK_TYPE_MASK;
		if (ltype != VOP_ISLOCKED(dvp)) {
			if (ltype == LK_EXCLUSIVE)
				vn_lock(dvp, LK_UPGRADE | LK_RETRY);
			else /* if (ltype == LK_SHARED) */
				vn_lock(dvp, LK_DOWNGRADE | LK_RETRY);

			/*
			 * Relock for the "." case could leave us with
			 * reclaimed vnode.
			 */
			if (VN_IS_DOOMED(dvp)) {
				vrele(dvp);
				return (SET_ERROR(ENOENT));
			}
		}
		return (0);
	} else if (name[0] == '.' && name[1] == '.' && name[2] == 0) {
		/*
		 * Note that in this case, dvp is the child vnode, and we
		 * are looking up the parent vnode - exactly reverse from
		 * normal operation.  Unlocking dvp requires some rather
		 * tricky unlock/relock dance to prevent mp from being freed;
		 * use vn_vget_ino_gen() which takes care of all that.
		 *
		 * XXX Note that there is a time window when both vnodes are
		 * unlocked.  It is possible, although highly unlikely, that
		 * during that window the parent-child relationship between
		 * the vnodes may change, for example, get reversed.
		 * In that case we would have a wrong lock order for the vnodes.
		 * All other filesystems seem to ignore this problem, so we
		 * do the same here.
		 * A potential solution could be implemented as follows:
		 * - using LK_NOWAIT when locking the second vnode and retrying
		 *   if necessary
		 * - checking that the parent-child relationship still holds
		 *   after locking both vnodes and retrying if it doesn't
		 */
		error = vn_vget_ino_gen(dvp, zfs_dd_callback, vp, lkflags, &vp);
		return (error);
	} else {
		error = vn_lock(vp, lkflags);
		if (error != 0)
			vrele(vp);
		return (error);
	}
}

/*
 * Lookup an entry in a directory, or an extended attribute directory.
 * If it exists, return a held vnode reference for it.
 *
 *	IN:	dvp	- vnode of directory to search.
 *		nm	- name of entry to lookup.
 *		pnp	- full pathname to lookup [UNUSED].
 *		flags	- LOOKUP_XATTR set if looking for an attribute.
 *		rdir	- root directory vnode [UNUSED].
 *		cr	- credentials of caller.
 *		ct	- caller context
 *
 *	OUT:	vpp	- vnode of located entry, NULL if not found.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	NA
 */
static int
zfs_lookup(vnode_t *dvp, const char *nm, vnode_t **vpp,
    struct componentname *cnp, int nameiop, cred_t *cr, int flags,
    boolean_t cached)
{
	znode_t *zdp = VTOZ(dvp);
	znode_t *zp;
	zfsvfs_t *zfsvfs = zdp->z_zfsvfs;
	seqc_t dvp_seqc;
	int	error = 0;

	/*
	 * Fast path lookup, however we must skip DNLC lookup
	 * for case folding or normalizing lookups because the
	 * DNLC code only stores the passed in name.  This means
	 * creating 'a' and removing 'A' on a case insensitive
	 * file system would work, but DNLC still thinks 'a'
	 * exists and won't let you create it again on the next
	 * pass through fast path.
	 */
	if (!(flags & LOOKUP_XATTR)) {
		if (dvp->v_type != VDIR) {
			return (SET_ERROR(ENOTDIR));
		} else if (zdp->z_sa_hdl == NULL) {
			return (SET_ERROR(EIO));
		}
	}

	DTRACE_PROBE2(zfs__fastpath__lookup__miss, vnode_t *, dvp,
	    const char *, nm);

	if ((error = zfs_enter_verify_zp(zfsvfs, zdp, FTAG)) != 0)
		return (error);

	dvp_seqc = vn_seqc_read_notmodify(dvp);

	*vpp = NULL;

	if (flags & LOOKUP_XATTR) {
		/*
		 * If the xattr property is off, refuse the lookup request.
		 */
		if (!(zfsvfs->z_flags & ZSB_XATTR)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOPNOTSUPP));
		}

		/*
		 * We don't allow recursive attributes..
		 * Maybe someday we will.
		 */
		if (zdp->z_pflags & ZFS_XATTR) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EINVAL));
		}

		if ((error = zfs_get_xattrdir(VTOZ(dvp), &zp, cr, flags))) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
		*vpp = ZTOV(zp);

		/*
		 * Do we have permission to get into attribute directory?
		 */
		if (flags & LOOKUP_NAMED_ATTR)
			error = zfs_zaccess(zp, ACE_EXECUTE, V_NAMEDATTR,
			    B_FALSE, cr, NULL);
		else
			error = zfs_zaccess(zp, ACE_EXECUTE, 0, B_FALSE, cr,
			    NULL);
		if (error) {
			vrele(ZTOV(zp));
		}

		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Check accessibility of directory if we're not coming in via
	 * VOP_CACHEDLOOKUP.
	 */
	if (!cached) {
#ifdef NOEXECCHECK
		if ((cnp->cn_flags & NOEXECCHECK) != 0) {
			cnp->cn_flags &= ~NOEXECCHECK;
		} else
#endif
		if ((error = zfs_zaccess(zdp, ACE_EXECUTE, 0, B_FALSE, cr,
		    NULL))) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	if (zfsvfs->z_utf8 && u8_validate(nm, strlen(nm),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}


	/*
	 * First handle the special cases.
	 */
	if ((cnp->cn_flags & ISDOTDOT) != 0) {
		/*
		 * If we are a snapshot mounted under .zfs, return
		 * the vp for the snapshot directory.
		 */
		if (zdp->z_id == zfsvfs->z_root && zfsvfs->z_parent != zfsvfs) {
			struct componentname cn;
			vnode_t *zfsctl_vp;
			int ltype;

			zfs_exit(zfsvfs, FTAG);
			ltype = VOP_ISLOCKED(dvp);
			VOP_UNLOCK(dvp);
			error = zfsctl_root(zfsvfs->z_parent, LK_SHARED,
			    &zfsctl_vp);
			if (error == 0) {
				cn.cn_nameptr = "snapshot";
				cn.cn_namelen = strlen(cn.cn_nameptr);
				cn.cn_nameiop = cnp->cn_nameiop;
				cn.cn_flags = cnp->cn_flags & ~ISDOTDOT;
				cn.cn_lkflags = cnp->cn_lkflags;
				error = VOP_LOOKUP(zfsctl_vp, vpp, &cn);
				vput(zfsctl_vp);
			}
			vn_lock(dvp, ltype | LK_RETRY);
			return (error);
		}
	}
	if (zfs_has_ctldir(zdp) && strcmp(nm, ZFS_CTLDIR_NAME) == 0) {
		zfs_exit(zfsvfs, FTAG);
		if (zfsvfs->z_show_ctldir == ZFS_SNAPDIR_DISABLED)
			return (SET_ERROR(ENOENT));
		if ((cnp->cn_flags & ISLASTCN) != 0 && nameiop != LOOKUP)
			return (SET_ERROR(ENOTSUP));
		error = zfsctl_root(zfsvfs, cnp->cn_lkflags, vpp);
		return (error);
	}

	/*
	 * The loop is retry the lookup if the parent-child relationship
	 * changes during the dot-dot locking complexities.
	 */
	for (;;) {
		uint64_t parent;

		error = zfs_dirlook(zdp, nm, &zp);
		if (error == 0)
			*vpp = ZTOV(zp);

		zfs_exit(zfsvfs, FTAG);
		if (error != 0)
			break;

		error = zfs_lookup_lock(dvp, *vpp, nm, cnp->cn_lkflags);
		if (error != 0) {
			/*
			 * If we've got a locking error, then the vnode
			 * got reclaimed because of a force unmount.
			 * We never enter doomed vnodes into the name cache.
			 */
			*vpp = NULL;
			return (error);
		}

		if ((cnp->cn_flags & ISDOTDOT) == 0)
			break;

		if ((error = zfs_enter(zfsvfs, FTAG)) != 0) {
			vput(ZTOV(zp));
			*vpp = NULL;
			return (error);
		}
		if (zdp->z_sa_hdl == NULL) {
			error = SET_ERROR(EIO);
		} else {
			error = sa_lookup(zdp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
			    &parent, sizeof (parent));
		}
		if (error != 0) {
			zfs_exit(zfsvfs, FTAG);
			vput(ZTOV(zp));
			break;
		}
		if (zp->z_id == parent) {
			zfs_exit(zfsvfs, FTAG);
			break;
		}
		vput(ZTOV(zp));
	}

	if (error != 0)
		*vpp = NULL;

	/* Translate errors and add SAVENAME when needed. */
	if (cnp->cn_flags & ISLASTCN) {
		switch (nameiop) {
		case CREATE:
		case RENAME:
			if (error == ENOENT) {
				error = EJUSTRETURN;
#if __FreeBSD_version < 1400068
				cnp->cn_flags |= SAVENAME;
#endif
				break;
			}
			zfs_fallthrough;
		case DELETE:
#if __FreeBSD_version < 1400068
			if (error == 0)
				cnp->cn_flags |= SAVENAME;
#endif
			break;
		}
	}

	if ((cnp->cn_flags & ISDOTDOT) != 0) {
		/*
		 * FIXME: zfs_lookup_lock relocks vnodes and does nothing to
		 * handle races. In particular different callers may end up
		 * with different vnodes and will try to add conflicting
		 * entries to the namecache.
		 *
		 * While finding different result may be acceptable in face
		 * of concurrent modification, adding conflicting entries
		 * trips over an assert in the namecache.
		 *
		 * Ultimately let an entry through once everything settles.
		 */
		if (!vn_seqc_consistent(dvp, dvp_seqc)) {
			cnp->cn_flags &= ~MAKEENTRY;
		}
	}

	/* Insert name into cache (as non-existent) if appropriate. */
	if (zfsvfs->z_use_namecache && !zfsvfs->z_replay &&
	    error == ENOENT && (cnp->cn_flags & MAKEENTRY) != 0)
		cache_enter(dvp, NULL, cnp);

	/* Insert name into cache if appropriate. */
	if (zfsvfs->z_use_namecache && !zfsvfs->z_replay &&
	    error == 0 && (cnp->cn_flags & MAKEENTRY)) {
		if (!(cnp->cn_flags & ISLASTCN) ||
		    (nameiop != DELETE && nameiop != RENAME)) {
			cache_enter(dvp, *vpp, cnp);
		}
	}

	return (error);
}

static inline bool
is_nametoolong(zfsvfs_t *zfsvfs, const char *name)
{
	size_t dlen = strlen(name);
	return ((!zfsvfs->z_longname && dlen >= ZAP_MAXNAMELEN) ||
	    dlen >= ZAP_MAXNAMELEN_NEW);
}

/*
 * Attempt to create a new entry in a directory.  If the entry
 * already exists, truncate the file if permissible, else return
 * an error.  Return the vp of the created or trunc'd file.
 *
 *	IN:	dvp	- vnode of directory to put new file entry in.
 *		name	- name of new file entry.
 *		vap	- attributes of new file.
 *		excl	- flag indicating exclusive or non-exclusive mode.
 *		mode	- mode to open file with.
 *		cr	- credentials of caller.
 *		flag	- large file flag [UNUSED].
 *		ct	- caller context
 *		vsecp	- ACL to be set
 *		mnt_ns	- Unused on FreeBSD
 *
 *	OUT:	vpp	- vnode of created or trunc'd entry.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated if new entry created
 *	 vp - ctime|mtime always, atime if new
 */
int
zfs_create(znode_t *dzp, const char *name, vattr_t *vap, int excl, int mode,
    znode_t **zpp, cred_t *cr, int flag, vsecattr_t *vsecp, zidmap_t *mnt_ns)
{
	(void) excl, (void) mode, (void) flag;
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	objset_t	*os;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid = crgetuid(cr);
	gid_t		gid = crgetgid(cr);
	uint64_t	projid = ZFS_DEFAULT_PROJID;
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;
	uint64_t	txtype;
#ifdef DEBUG_VFS_LOCKS
	vnode_t	*dvp = ZTOV(dzp);
#endif

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */
	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (vsecp || (vap->va_mask & AT_XVATTR) ||
	    IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	os = zfsvfs->z_os;
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (vap->va_mask & AT_XVATTR) {
		if ((error = secpolicy_xvattr(ZTOV(dzp), (xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_type)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	*zpp = NULL;

	if ((vap->va_mode & S_ISVTX) && secpolicy_vnode_stky_modify(cr))
		vap->va_mode &= ~S_ISVTX;

	error = zfs_dirent_lookup(dzp, name, &zp, ZNEW);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	ASSERT3P(zp, ==, NULL);

	/*
	 * Create a new file object and update the directory
	 * to reference it.
	 */
	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr, mnt_ns))) {
		goto out;
	}

	/*
	 * We only support the creation of regular files in
	 * extended attribute directories.
	 */

	if ((dzp->z_pflags & ZFS_XATTR) &&
	    (vap->va_type != VREG)) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	if ((error = zfs_acl_ids_create(dzp, 0, vap,
	    cr, vsecp, &acl_ids, NULL)) != 0)
		goto out;

	if (S_ISREG(vap->va_mode) || S_ISDIR(vap->va_mode))
		projid = zfs_inherit_projid(dzp);
	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, projid)) {
		zfs_acl_ids_free(&acl_ids);
		error = SET_ERROR(EDQUOT);
		goto out;
	}

	getnewvnode_reserve();

	tx = dmu_tx_create(os);

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	if (!zfsvfs->z_use_sa &&
	    acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
		    0, acl_ids.z_aclp->z_acl_bytes);
	}
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		/*
		 * Since, we failed to add the directory entry for it,
		 * delete the newly created dnode.
		 */
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_commit(tx);
		getnewvnode_drop_reserve();
		goto out;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	txtype = zfs_log_create_txtype(Z_FILE, vsecp, vap);
	zfs_log_create(zilog, tx, txtype, dzp, zp, name,
	    vsecp, acl_ids.z_fuidp, vap);
	zfs_acl_ids_free(&acl_ids);
	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

out:
	VNCHECKREF(dvp);
	if (error == 0) {
		*zpp = zp;
	}

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove an entry from a directory.
 *
 *	IN:	dvp	- vnode of directory to remove entry from.
 *		name	- name of entry to remove.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime
 *	 vp - ctime (if nlink > 0)
 */
static int
zfs_remove_(vnode_t *dvp, vnode_t *vp, const char *name, cred_t *cr)
{
	znode_t		*dzp = VTOZ(dvp);
	znode_t		*zp;
	znode_t		*xzp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	xattr_obj;
	uint64_t	obj = 0;
	dmu_tx_t	*tx;
	boolean_t	unlinked;
	uint64_t	txtype;
	int		error;


	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zp = VTOZ(vp);
	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;

	xattr_obj = 0;
	xzp = NULL;

	if ((error = zfs_zaccess_delete(dzp, zp, cr, NULL))) {
		goto out;
	}

	/*
	 * Need to use rmdir for removing directories.
	 */
	if (vp->v_type == VDIR) {
		error = SET_ERROR(EPERM);
		goto out;
	}

	vnevent_remove(vp, dvp, name, ct);

	obj = zp->z_id;

	/* are there any extended attributes? */
	error = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
	    &xattr_obj, sizeof (xattr_obj));
	if (error == 0 && xattr_obj) {
		error = zfs_zget(zfsvfs, xattr_obj, &xzp);
		ASSERT0(error);
	}

	/*
	 * We may delete the znode now, or we may put it in the unlinked set;
	 * it depends on whether we're the last link, and on whether there are
	 * other holds on the vnode.  So we dmu_tx_hold() the right things to
	 * allow for either case.
	 */
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);

	if (xzp) {
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		dmu_tx_hold_sa(tx, xzp->z_sa_hdl, B_FALSE);
	}

	/* charge as an update -- would be nice not to charge at all */
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);

	/*
	 * Mark this transaction as typically resulting in a net free of space
	 */
	dmu_tx_mark_netfree(tx);

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Remove the directory entry.
	 */
	error = zfs_link_destroy(dzp, name, zp, tx, ZEXISTS, &unlinked);

	if (error) {
		dmu_tx_commit(tx);
		goto out;
	}

	if (unlinked) {
		zfs_unlinked_add(zp, tx);
		vp->v_vflag |= VV_NOSYNC;
	}
	/* XXX check changes to linux vnops */
	txtype = TX_REMOVE;
	zfs_log_remove(zilog, tx, txtype, dzp, name, obj, unlinked);

	dmu_tx_commit(tx);
out:

	if (xzp)
		vrele(ZTOV(xzp));

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);


	zfs_exit(zfsvfs, FTAG);
	return (error);
}


static int
zfs_lookup_internal(znode_t *dzp, const char *name, vnode_t **vpp,
    struct componentname *cnp, int nameiop)
{
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	int error;

	cnp->cn_nameptr = __DECONST(char *, name);
	cnp->cn_namelen = strlen(name);
	cnp->cn_nameiop = nameiop;
	cnp->cn_flags = ISLASTCN;
#if __FreeBSD_version < 1400068
	cnp->cn_flags |= SAVENAME;
#endif
	cnp->cn_lkflags = LK_EXCLUSIVE | LK_RETRY;
	cnp->cn_cred = kcred;
#if __FreeBSD_version < 1400037
	cnp->cn_thread = curthread;
#endif

	if (zfsvfs->z_use_namecache && !zfsvfs->z_replay) {
		struct vop_lookup_args a;

		a.a_gen.a_desc = &vop_lookup_desc;
		a.a_dvp = ZTOV(dzp);
		a.a_vpp = vpp;
		a.a_cnp = cnp;
		error = vfs_cache_lookup(&a);
	} else {
		error = zfs_lookup(ZTOV(dzp), name, vpp, cnp, nameiop, kcred, 0,
		    B_FALSE);
	}
#ifdef ZFS_DEBUG
	if (error) {
		printf("got error %d on name %s on op %d\n", error, name,
		    nameiop);
		kdb_backtrace();
	}
#endif
	return (error);
}

int
zfs_remove(znode_t *dzp, const char *name, cred_t *cr, int flags)
{
	vnode_t *vp;
	int error;
	struct componentname cn;

	if ((error = zfs_lookup_internal(dzp, name, &vp, &cn, DELETE)))
		return (error);

	error = zfs_remove_(ZTOV(dzp), vp, name, cr);
	vput(vp);
	return (error);
}
/*
 * Create a new directory and insert it into dvp using the name
 * provided.  Return a pointer to the inserted directory.
 *
 *	IN:	dvp	- vnode of directory to add subdir to.
 *		dirname	- name of new directory.
 *		vap	- attributes of new directory.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *		vsecp	- ACL to be set
 *		mnt_ns	- Unused on FreeBSD
 *
 *	OUT:	vpp	- vnode of created directory.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 *	 vp - ctime|mtime|atime updated
 */
int
zfs_mkdir(znode_t *dzp, const char *dirname, vattr_t *vap, znode_t **zpp,
    cred_t *cr, int flags, vsecattr_t *vsecp, zidmap_t *mnt_ns)
{
	(void) flags, (void) vsecp;
	znode_t		*zp;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	txtype;
	dmu_tx_t	*tx;
	int		error;
	uid_t		uid = crgetuid(cr);
	gid_t		gid = crgetgid(cr);
	zfs_acl_ids_t   acl_ids;
	boolean_t	fuid_dirtied;

	ASSERT3U(vap->va_type, ==, VDIR);

	if (is_nametoolong(zfsvfs, dirname))
		return (SET_ERROR(ENAMETOOLONG));

	/*
	 * If we have an ephemeral id, ACL, or XVATTR then
	 * make sure file system is at proper version
	 */
	if (zfsvfs->z_use_fuids == B_FALSE &&
	    ((vap->va_mask & AT_XVATTR) ||
	    IS_EPHEMERAL(uid) || IS_EPHEMERAL(gid)))
		return (SET_ERROR(EINVAL));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (dzp->z_pflags & ZFS_XATTR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (zfsvfs->z_utf8 && u8_validate(dirname,
	    strlen(dirname), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (vap->va_mask & AT_XVATTR) {
		if ((error = secpolicy_xvattr(ZTOV(dzp), (xvattr_t *)vap,
		    crgetuid(cr), cr, vap->va_type)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	if ((error = zfs_acl_ids_create(dzp, 0, vap, cr,
	    NULL, &acl_ids, NULL)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * First make sure the new directory doesn't exist.
	 *
	 * Existence is checked first to make sure we don't return
	 * EACCES instead of EEXIST which can cause some applications
	 * to fail.
	 */
	*zpp = NULL;

	if ((error = zfs_dirent_lookup(dzp, dirname, &zp, ZNEW))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	ASSERT3P(zp, ==, NULL);

	if ((error = zfs_zaccess(dzp, ACE_ADD_SUBDIRECTORY, 0, B_FALSE, cr,
	    mnt_ns))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, zfs_inherit_projid(dzp))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	/*
	 * Add a new entry to the directory.
	 */
	getnewvnode_reserve();
	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, dirname);
	dmu_tx_hold_zap(tx, DMU_NEW_OBJECT, FALSE, NULL);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}

	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE);

	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create new node.
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	/*
	 * Now put new name in parent dir.
	 */
	error = zfs_link_create(dzp, dirname, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
		goto out;
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	*zpp = zp;

	txtype = zfs_log_create_txtype(Z_DIR, NULL, vap);
	zfs_log_create(zilog, tx, txtype, dzp, zp, dirname, NULL,
	    acl_ids.z_fuidp, vap);

out:
	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Remove a directory subdir entry.  If the current working
 * directory is the same as the subdir to be removed, the
 * remove will fail.
 *
 *	IN:	dvp	- vnode of directory to remove from.
 *		name	- name of directory to be removed.
 *		cwd	- vnode of current working directory.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 */
static int
zfs_rmdir_(vnode_t *dvp, vnode_t *vp, const char *name, cred_t *cr)
{
	znode_t		*dzp = VTOZ(dvp);
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(zp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;


	if ((error = zfs_zaccess_delete(dzp, zp, cr, NULL))) {
		goto out;
	}

	if (vp->v_type != VDIR) {
		error = SET_ERROR(ENOTDIR);
		goto out;
	}

	vnevent_rmdir(vp, dvp, name, ct);

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_zap(tx, dzp->z_id, FALSE, name);
	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	zfs_sa_upgrade_txholds(tx, zp);
	zfs_sa_upgrade_txholds(tx, dzp);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	error = zfs_link_destroy(dzp, name, zp, tx, ZEXISTS, NULL);

	if (error == 0) {
		uint64_t txtype = TX_RMDIR;
		zfs_log_remove(zilog, tx, txtype, dzp, name,
		    ZFS_NO_OBJECT, B_FALSE);
	}

	dmu_tx_commit(tx);

	if (zfsvfs->z_use_namecache)
		cache_vop_rmdir(dvp, vp);
out:
	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

int
zfs_rmdir(znode_t *dzp, const char *name, znode_t *cwd, cred_t *cr, int flags)
{
	struct componentname cn;
	vnode_t *vp;
	int error;

	if ((error = zfs_lookup_internal(dzp, name, &vp, &cn, DELETE)))
		return (error);

	error = zfs_rmdir_(ZTOV(dzp), vp, name, cr);
	vput(vp);
	return (error);
}

/*
 * Read as many directory entries as will fit into the provided
 * buffer from the given directory cursor position (specified in
 * the uio structure).
 *
 *	IN:	vp	- vnode of directory to read.
 *		uio	- structure supplying read location, range info,
 *			  and return buffer.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *
 *	OUT:	uio	- updated offset and range, buffer filled.
 *		eofp	- set to true if end-of-file detected.
 *		ncookies- number of entries in cookies
 *		cookies	- offsets to directory entries
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - atime updated
 *
 * Note that the low 4 bits of the cookie returned by zap is always zero.
 * This allows us to use the low range for "special" directory entries:
 * We use 0 for '.', and 1 for '..'.  If this is the root of the filesystem,
 * we use the offset 2 for the '.zfs' directory.
 */
static int
zfs_readdir(vnode_t *vp, zfs_uio_t *uio, cred_t *cr, int *eofp,
    int *ncookies, cookie_t **cookies)
{
	znode_t		*zp = VTOZ(vp);
	iovec_t		*iovp;
	dirent64_t	*odp;
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	objset_t	*os;
	caddr_t		outbuf;
	size_t		bufsize;
	zap_cursor_t	zc;
	zap_attribute_t	*zap;
	uint_t		bytes_wanted;
	uint64_t	offset; /* must be unsigned; checks for < 1 */
	uint64_t	parent;
	int		local_eof;
	int		outcount;
	int		error;
	uint8_t		prefetch;
	uint8_t		type;
	int		ncooks;
	cookie_t	*cooks = NULL;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (parent))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If we are not given an eof variable,
	 * use a local one.
	 */
	if (eofp == NULL)
		eofp = &local_eof;

	/*
	 * Check for valid iov_len.
	 */
	if (GET_UIO_STRUCT(uio)->uio_iov->iov_len <= 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Quit if directory has been removed (posix)
	 */
	if ((*eofp = zp->z_unlinked) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	error = 0;
	os = zfsvfs->z_os;
	offset = zfs_uio_offset(uio);
	prefetch = zp->z_zn_prefetch;
	zap = zap_attribute_long_alloc();

	/*
	 * Initialize the iterator cursor.
	 */
	if (offset <= 3) {
		/*
		 * Start iteration from the beginning of the directory.
		 */
		zap_cursor_init(&zc, os, zp->z_id);
	} else {
		/*
		 * The offset is a serialized cursor.
		 */
		zap_cursor_init_serialized(&zc, os, zp->z_id, offset);
	}

	/*
	 * Get space to change directory entries into fs independent format.
	 */
	iovp = GET_UIO_STRUCT(uio)->uio_iov;
	bytes_wanted = iovp->iov_len;
	if (zfs_uio_segflg(uio) != UIO_SYSSPACE || zfs_uio_iovcnt(uio) != 1) {
		bufsize = bytes_wanted;
		outbuf = kmem_alloc(bufsize, KM_SLEEP);
		odp = (struct dirent64 *)outbuf;
	} else {
		bufsize = bytes_wanted;
		outbuf = NULL;
		odp = (struct dirent64 *)iovp->iov_base;
	}

	if (ncookies != NULL) {
		/*
		 * Minimum entry size is dirent size and 1 byte for a file name.
		 */
		ncooks = zfs_uio_resid(uio) / (sizeof (struct dirent) -
		    sizeof (((struct dirent *)NULL)->d_name) + 1);
		cooks = malloc(ncooks * sizeof (*cooks), M_TEMP, M_WAITOK);
		*cookies = cooks;
		*ncookies = ncooks;
	}

	/*
	 * Transform to file-system independent format
	 */
	outcount = 0;
	while (outcount < bytes_wanted) {
		ino64_t objnum;
		ushort_t reclen;
		off64_t *next = NULL;

		/*
		 * Special case `.', `..', and `.zfs'.
		 */
		if (offset == 0) {
			(void) strcpy(zap->za_name, ".");
			zap->za_normalization_conflict = 0;
			objnum = zp->z_id;
			type = DT_DIR;
		} else if (offset == 1) {
			(void) strcpy(zap->za_name, "..");
			zap->za_normalization_conflict = 0;
			objnum = parent;
			type = DT_DIR;
		} else if (offset == 2 && zfs_show_ctldir(zp)) {
			(void) strcpy(zap->za_name, ZFS_CTLDIR_NAME);
			zap->za_normalization_conflict = 0;
			objnum = ZFSCTL_INO_ROOT;
			type = DT_DIR;
		} else {
			/*
			 * Grab next entry.
			 */
			if ((error = zap_cursor_retrieve(&zc, zap))) {
				if ((*eofp = (error == ENOENT)) != 0)
					break;
				else
					goto update;
			}

			if (zap->za_integer_length != 8 ||
			    zap->za_num_integers != 1) {
				cmn_err(CE_WARN, "zap_readdir: bad directory "
				    "entry, obj = %lld, offset = %lld\n",
				    (u_longlong_t)zp->z_id,
				    (u_longlong_t)offset);
				error = SET_ERROR(ENXIO);
				goto update;
			}

			objnum = ZFS_DIRENT_OBJ(zap->za_first_integer);
			/*
			 * MacOS X can extract the object type here such as:
			 * uint8_t type = ZFS_DIRENT_TYPE(zap.za_first_integer);
			 */
			type = ZFS_DIRENT_TYPE(zap->za_first_integer);
		}

		reclen = DIRENT64_RECLEN(strlen(zap->za_name));

		/*
		 * Will this entry fit in the buffer?
		 */
		if (outcount + reclen > bufsize) {
			/*
			 * Did we manage to fit anything in the buffer?
			 */
			if (!outcount) {
				error = SET_ERROR(EINVAL);
				goto update;
			}
			break;
		}
		/*
		 * Add normal entry:
		 */
		odp->d_ino = objnum;
		odp->d_reclen = reclen;
		odp->d_namlen = strlen(zap->za_name);
		/* NOTE: d_off is the offset for the *next* entry. */
		next = &odp->d_off;
		strlcpy(odp->d_name, zap->za_name, odp->d_namlen + 1);
		odp->d_type = type;
		dirent_terminate(odp);
		odp = (dirent64_t *)((intptr_t)odp + reclen);

		outcount += reclen;

		ASSERT3S(outcount, <=, bufsize);

		if (prefetch)
			dmu_prefetch_dnode(os, objnum, ZIO_PRIORITY_SYNC_READ);

		/*
		 * Move to the next entry, fill in the previous offset.
		 */
		if (offset > 2 || (offset == 2 && !zfs_show_ctldir(zp))) {
			zap_cursor_advance(&zc);
			offset = zap_cursor_serialize(&zc);
		} else {
			offset += 1;
		}

		/* Fill the offset right after advancing the cursor. */
		if (next != NULL)
			*next = offset;
		if (cooks != NULL) {
			*cooks++ = offset;
			ncooks--;
			KASSERT(ncooks >= 0, ("ncookies=%d", ncooks));
		}
	}
	zp->z_zn_prefetch = B_FALSE; /* a lookup will re-enable pre-fetching */

	/* Subtract unused cookies */
	if (ncookies != NULL)
		*ncookies -= ncooks;

	if (zfs_uio_segflg(uio) == UIO_SYSSPACE && zfs_uio_iovcnt(uio) == 1) {
		iovp->iov_base += outcount;
		iovp->iov_len -= outcount;
		zfs_uio_resid(uio) -= outcount;
	} else if ((error =
	    zfs_uiomove(outbuf, (long)outcount, UIO_READ, uio))) {
		/*
		 * Reset the pointer.
		 */
		offset = zfs_uio_offset(uio);
	}

update:
	zap_cursor_fini(&zc);
	zap_attribute_free(zap);
	if (zfs_uio_segflg(uio) != UIO_SYSSPACE || zfs_uio_iovcnt(uio) != 1)
		kmem_free(outbuf, bufsize);

	if (error == ENOENT)
		error = 0;

	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);

	zfs_uio_setoffset(uio, offset);
	zfs_exit(zfsvfs, FTAG);
	if (error != 0 && cookies != NULL) {
		free(*cookies, M_TEMP);
		*cookies = NULL;
		*ncookies = 0;
	}
	return (error);
}

/*
 * Get the requested file attributes and place them in the provided
 * vattr structure.
 *
 *	IN:	vp	- vnode of file.
 *		vap	- va_mask identifies requested attributes.
 *			  If AT_XVATTR set, then optional attrs are requested
 *		flags	- ATTR_NOACLCHECK (CIFS server context)
 *		cr	- credentials of caller.
 *
 *	OUT:	vap	- attribute values.
 *
 *	RETURN:	0 (always succeeds).
 */
static int
zfs_getattr(vnode_t *vp, vattr_t *vap, int flags, cred_t *cr)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int	error = 0;
	uint32_t blksize;
	u_longlong_t nblocks;
	uint64_t mtime[2], ctime[2], crtime[2], rdev;
	xvattr_t *xvap = (xvattr_t *)vap;	/* vap may be an xvattr_t * */
	xoptattr_t *xoap = NULL;
	boolean_t skipaclchk = (flags & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	sa_bulk_attr_t bulk[4];
	int count = 0;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	zfs_fuid_map_ids(zp, cr, &vap->va_uid, &vap->va_gid);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL, &mtime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL, &ctime, 16);
	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CRTIME(zfsvfs), NULL, &crtime, 16);
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_RDEV(zfsvfs), NULL,
		    &rdev, 8);

	if ((error = sa_bulk_lookup(zp->z_sa_hdl, bulk, count)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If ACL is trivial don't bother looking for ACE_READ_ATTRIBUTES.
	 * Also, if we are the owner don't bother, since owner should
	 * always be allowed to read basic attributes of file.
	 */
	if (!(zp->z_pflags & ZFS_ACL_TRIVIAL) &&
	    (vap->va_uid != crgetuid(cr))) {
		if ((error = zfs_zaccess(zp, ACE_READ_ATTRIBUTES, 0,
		    skipaclchk, cr, NULL))) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	/*
	 * Return all attributes.  It's cheaper to provide the answer
	 * than to determine whether we were asked the question.
	 */

	vap->va_type = IFTOVT(zp->z_mode);
	vap->va_mode = zp->z_mode & ~S_IFMT;
	vn_fsid(vp, vap);
	vap->va_nodeid = zp->z_id;
	vap->va_nlink = zp->z_links;
	if ((vp->v_flag & VROOT) && zfs_show_ctldir(zp) &&
	    zp->z_links < ZFS_LINK_MAX)
		vap->va_nlink++;
	vap->va_size = zp->z_size;
	if (vp->v_type == VBLK || vp->v_type == VCHR)
		vap->va_rdev = zfs_cmpldev(rdev);
	else
		vap->va_rdev = 0;
	vap->va_gen = zp->z_gen;
	vap->va_flags = 0;	/* FreeBSD: Reset chflags(2) flags. */
	vap->va_filerev = zp->z_seq;

	/*
	 * Add in any requested optional attributes and the create time.
	 * Also set the corresponding bits in the returned attribute bitmap.
	 */
	if ((xoap = xva_getxoptattr(xvap)) != NULL && zfsvfs->z_use_fuids) {
		if (XVA_ISSET_REQ(xvap, XAT_ARCHIVE)) {
			xoap->xoa_archive =
			    ((zp->z_pflags & ZFS_ARCHIVE) != 0);
			XVA_SET_RTN(xvap, XAT_ARCHIVE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_READONLY)) {
			xoap->xoa_readonly =
			    ((zp->z_pflags & ZFS_READONLY) != 0);
			XVA_SET_RTN(xvap, XAT_READONLY);
		}

		if (XVA_ISSET_REQ(xvap, XAT_SYSTEM)) {
			xoap->xoa_system =
			    ((zp->z_pflags & ZFS_SYSTEM) != 0);
			XVA_SET_RTN(xvap, XAT_SYSTEM);
		}

		if (XVA_ISSET_REQ(xvap, XAT_HIDDEN)) {
			xoap->xoa_hidden =
			    ((zp->z_pflags & ZFS_HIDDEN) != 0);
			XVA_SET_RTN(xvap, XAT_HIDDEN);
		}

		if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
			xoap->xoa_nounlink =
			    ((zp->z_pflags & ZFS_NOUNLINK) != 0);
			XVA_SET_RTN(xvap, XAT_NOUNLINK);
		}

		if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
			xoap->xoa_immutable =
			    ((zp->z_pflags & ZFS_IMMUTABLE) != 0);
			XVA_SET_RTN(xvap, XAT_IMMUTABLE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
			xoap->xoa_appendonly =
			    ((zp->z_pflags & ZFS_APPENDONLY) != 0);
			XVA_SET_RTN(xvap, XAT_APPENDONLY);
		}

		if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
			xoap->xoa_nodump =
			    ((zp->z_pflags & ZFS_NODUMP) != 0);
			XVA_SET_RTN(xvap, XAT_NODUMP);
		}

		if (XVA_ISSET_REQ(xvap, XAT_OPAQUE)) {
			xoap->xoa_opaque =
			    ((zp->z_pflags & ZFS_OPAQUE) != 0);
			XVA_SET_RTN(xvap, XAT_OPAQUE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
			xoap->xoa_av_quarantined =
			    ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0);
			XVA_SET_RTN(xvap, XAT_AV_QUARANTINED);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
			xoap->xoa_av_modified =
			    ((zp->z_pflags & ZFS_AV_MODIFIED) != 0);
			XVA_SET_RTN(xvap, XAT_AV_MODIFIED);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP) &&
		    vp->v_type == VREG) {
			zfs_sa_get_scanstamp(zp, xvap);
		}

		if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
			xoap->xoa_reparse = ((zp->z_pflags & ZFS_REPARSE) != 0);
			XVA_SET_RTN(xvap, XAT_REPARSE);
		}
		if (XVA_ISSET_REQ(xvap, XAT_GEN)) {
			xoap->xoa_generation = zp->z_gen;
			XVA_SET_RTN(xvap, XAT_GEN);
		}

		if (XVA_ISSET_REQ(xvap, XAT_OFFLINE)) {
			xoap->xoa_offline =
			    ((zp->z_pflags & ZFS_OFFLINE) != 0);
			XVA_SET_RTN(xvap, XAT_OFFLINE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_SPARSE)) {
			xoap->xoa_sparse =
			    ((zp->z_pflags & ZFS_SPARSE) != 0);
			XVA_SET_RTN(xvap, XAT_SPARSE);
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
			xoap->xoa_projinherit =
			    ((zp->z_pflags & ZFS_PROJINHERIT) != 0);
			XVA_SET_RTN(xvap, XAT_PROJINHERIT);
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
			xoap->xoa_projid = zp->z_projid;
			XVA_SET_RTN(xvap, XAT_PROJID);
		}
	}

	ZFS_TIME_DECODE(&vap->va_atime, zp->z_atime);
	ZFS_TIME_DECODE(&vap->va_mtime, mtime);
	ZFS_TIME_DECODE(&vap->va_ctime, ctime);
	ZFS_TIME_DECODE(&vap->va_birthtime, crtime);


	sa_object_size(zp->z_sa_hdl, &blksize, &nblocks);
	vap->va_blksize = blksize;
	vap->va_bytes = nblocks << 9;	/* nblocks * 512 */

	if (zp->z_blksz == 0) {
		/*
		 * Block size hasn't been set; suggest maximal I/O transfers.
		 */
		vap->va_blksize = zfsvfs->z_max_blksz;
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

/*
 * For the operation of changing file's user/group/project, we need to
 * handle not only the main object that is assigned to the file directly,
 * but also the ones that are used by the file via hidden xattr directory.
 *
 * Because the xattr directory may contains many EA entries, as to it may
 * be impossible to change all of them via the transaction of changing the
 * main object's user/group/project attributes. Then we have to change them
 * via other multiple independent transactions one by one. It may be not good
 * solution, but we have no better idea yet.
 */
static int
zfs_setattr_dir(znode_t *dzp)
{
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	objset_t	*os = zfsvfs->z_os;
	zap_cursor_t	zc;
	zap_attribute_t	*zap;
	znode_t		*zp = NULL;
	dmu_tx_t	*tx = NULL;
	uint64_t	uid, gid;
	sa_bulk_attr_t	bulk[4];
	int		count;
	int		err;

	zap = zap_attribute_alloc();
	zap_cursor_init(&zc, os, dzp->z_id);
	while ((err = zap_cursor_retrieve(&zc, zap)) == 0) {
		count = 0;
		if (zap->za_integer_length != 8 || zap->za_num_integers != 1) {
			err = ENXIO;
			break;
		}

		err = zfs_dirent_lookup(dzp, zap->za_name, &zp, ZEXISTS);
		if (err == ENOENT)
			goto next;
		if (err)
			break;

		if (zp->z_uid == dzp->z_uid &&
		    zp->z_gid == dzp->z_gid &&
		    zp->z_projid == dzp->z_projid)
			goto next;

		tx = dmu_tx_create(os);
		if (!(zp->z_pflags & ZFS_PROJID))
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);

		err = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (err)
			break;

		mutex_enter(&dzp->z_lock);

		if (zp->z_uid != dzp->z_uid) {
			uid = dzp->z_uid;
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &uid, sizeof (uid));
			zp->z_uid = uid;
		}

		if (zp->z_gid != dzp->z_gid) {
			gid = dzp->z_gid;
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs), NULL,
			    &gid, sizeof (gid));
			zp->z_gid = gid;
		}

		uint64_t projid = dzp->z_projid;
		if (zp->z_projid != projid) {
			if (!(zp->z_pflags & ZFS_PROJID)) {
				err = sa_add_projid(zp->z_sa_hdl, tx, projid);
				if (unlikely(err == EEXIST)) {
					err = 0;
				} else if (err != 0) {
					goto sa_add_projid_err;
				} else {
					projid = ZFS_INVALID_PROJID;
				}
			}

			if (projid != ZFS_INVALID_PROJID) {
				zp->z_projid = projid;
				SA_ADD_BULK_ATTR(bulk, count,
				    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
				    sizeof (zp->z_projid));
			}
		}

sa_add_projid_err:
		mutex_exit(&dzp->z_lock);

		if (likely(count > 0)) {
			err = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
			dmu_tx_commit(tx);
		} else if (projid == ZFS_INVALID_PROJID) {
			dmu_tx_commit(tx);
		} else {
			dmu_tx_abort(tx);
		}
		tx = NULL;
		if (err != 0 && err != ENOENT)
			break;

next:
		if (zp) {
			zrele(zp);
			zp = NULL;
		}
		zap_cursor_advance(&zc);
	}

	if (tx)
		dmu_tx_abort(tx);
	if (zp) {
		zrele(zp);
	}
	zap_cursor_fini(&zc);
	zap_attribute_free(zap);

	return (err == ENOENT ? 0 : err);
}

/*
 * Set the file attributes to the values contained in the
 * vattr structure.
 *
 *	IN:	zp	- znode of file to be modified.
 *		vap	- new attribute values.
 *			  If AT_XVATTR set, then optional attrs are being set
 *		flags	- ATTR_UTIME set if non-default time values provided.
 *			- ATTR_NOACLCHECK (CIFS context only).
 *		cr	- credentials of caller.
 *		mnt_ns	- Unused on FreeBSD
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - ctime updated, mtime updated if size changed.
 */
int
zfs_setattr(znode_t *zp, vattr_t *vap, int flags, cred_t *cr, zidmap_t *mnt_ns)
{
	vnode_t		*vp = ZTOV(zp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	objset_t	*os;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	vattr_t		oldva;
	xvattr_t	tmpxvattr;
	uint_t		mask = vap->va_mask;
	uint_t		saved_mask = 0;
	uint64_t	saved_mode;
	int		trim_mask = 0;
	uint64_t	new_mode;
	uint64_t	new_uid, new_gid;
	uint64_t	xattr_obj;
	uint64_t	mtime[2], ctime[2];
	uint64_t	projid = ZFS_INVALID_PROJID;
	znode_t		*attrzp;
	int		need_policy = FALSE;
	int		err, err2;
	zfs_fuid_info_t *fuidp = NULL;
	xvattr_t *xvap = (xvattr_t *)vap;	/* vap may be an xvattr_t * */
	xoptattr_t	*xoap;
	zfs_acl_t	*aclp;
	boolean_t skipaclchk = (flags & ATTR_NOACLCHECK) ? B_TRUE : B_FALSE;
	boolean_t	fuid_dirtied = B_FALSE;
	boolean_t	handle_eadir = B_FALSE;
	sa_bulk_attr_t	bulk[7], xattr_bulk[7];
	int		count = 0, xattr_count = 0;

	if (mask == 0)
		return (0);

	if (mask & AT_NOSET)
		return (SET_ERROR(EINVAL));

	if ((err = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (err);

	os = zfsvfs->z_os;
	zilog = zfsvfs->z_log;

	/*
	 * Make sure that if we have ephemeral uid/gid or xvattr specified
	 * that file system is at proper version level
	 */

	if (zfsvfs->z_use_fuids == B_FALSE &&
	    (((mask & AT_UID) && IS_EPHEMERAL(vap->va_uid)) ||
	    ((mask & AT_GID) && IS_EPHEMERAL(vap->va_gid)) ||
	    (mask & AT_XVATTR))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	if (mask & AT_SIZE && vp->v_type == VDIR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EISDIR));
	}

	if (mask & AT_SIZE && vp->v_type != VREG && vp->v_type != VFIFO) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * If this is an xvattr_t, then get a pointer to the structure of
	 * optional attributes.  If this is NULL, then we have a vattr_t.
	 */
	xoap = xva_getxoptattr(xvap);

	xva_init(&tmpxvattr);

	/*
	 * Immutable files can only alter immutable bit and atime
	 */
	if ((zp->z_pflags & ZFS_IMMUTABLE) &&
	    ((mask & (AT_SIZE|AT_UID|AT_GID|AT_MTIME|AT_MODE)) ||
	    ((mask & AT_XVATTR) && XVA_ISSET_REQ(xvap, XAT_CREATETIME)))) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/*
	 * Note: ZFS_READONLY is handled in zfs_zaccess_common.
	 */

	/*
	 * Verify timestamps doesn't overflow 32 bits.
	 * ZFS can handle large timestamps, but 32bit syscalls can't
	 * handle times greater than 2039.  This check should be removed
	 * once large timestamps are fully supported.
	 */
	if (mask & (AT_ATIME | AT_MTIME)) {
		if (((mask & AT_ATIME) && TIMESPEC_OVERFLOW(&vap->va_atime)) ||
		    ((mask & AT_MTIME) && TIMESPEC_OVERFLOW(&vap->va_mtime))) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOVERFLOW));
		}
	}
	if (xoap != NULL && (mask & AT_XVATTR)) {
		if (XVA_ISSET_REQ(xvap, XAT_CREATETIME) &&
		    TIMESPEC_OVERFLOW(&vap->va_birthtime)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOVERFLOW));
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJID)) {
			if (!dmu_objset_projectquota_enabled(os) ||
			    (!S_ISREG(zp->z_mode) && !S_ISDIR(zp->z_mode))) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(EOPNOTSUPP));
			}

			projid = xoap->xoa_projid;
			if (unlikely(projid == ZFS_INVALID_PROJID)) {
				zfs_exit(zfsvfs, FTAG);
				return (SET_ERROR(EINVAL));
			}

			if (projid == zp->z_projid && zp->z_pflags & ZFS_PROJID)
				projid = ZFS_INVALID_PROJID;
			else
				need_policy = TRUE;
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT) &&
		    (xoap->xoa_projinherit !=
		    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) &&
		    (!dmu_objset_projectquota_enabled(os) ||
		    (!S_ISREG(zp->z_mode) && !S_ISDIR(zp->z_mode)))) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EOPNOTSUPP));
		}
	}

	attrzp = NULL;
	aclp = NULL;

	if (zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	/*
	 * First validate permissions
	 */

	if (mask & AT_SIZE) {
		/*
		 * XXX - Note, we are not providing any open
		 * mode flags here (like FNDELAY), so we may
		 * block if there are locks present... this
		 * should be addressed in openat().
		 */
		/* XXX - would it be OK to generate a log record here? */
		err = zfs_freesp(zp, vap->va_size, 0, 0, FALSE);
		if (err) {
			zfs_exit(zfsvfs, FTAG);
			return (err);
		}
	}

	if (mask & (AT_ATIME|AT_MTIME) ||
	    ((mask & AT_XVATTR) && (XVA_ISSET_REQ(xvap, XAT_HIDDEN) ||
	    XVA_ISSET_REQ(xvap, XAT_READONLY) ||
	    XVA_ISSET_REQ(xvap, XAT_ARCHIVE) ||
	    XVA_ISSET_REQ(xvap, XAT_OFFLINE) ||
	    XVA_ISSET_REQ(xvap, XAT_SPARSE) ||
	    XVA_ISSET_REQ(xvap, XAT_CREATETIME) ||
	    XVA_ISSET_REQ(xvap, XAT_SYSTEM)))) {
		need_policy = zfs_zaccess(zp, ACE_WRITE_ATTRIBUTES, 0,
		    skipaclchk, cr, mnt_ns);
	}

	if (mask & (AT_UID|AT_GID)) {
		int	idmask = (mask & (AT_UID|AT_GID));
		int	take_owner;
		int	take_group;

		/*
		 * NOTE: even if a new mode is being set,
		 * we may clear S_ISUID/S_ISGID bits.
		 */

		if (!(mask & AT_MODE))
			vap->va_mode = zp->z_mode;

		/*
		 * Take ownership or chgrp to group we are a member of
		 */

		take_owner = (mask & AT_UID) && (vap->va_uid == crgetuid(cr));
		take_group = (mask & AT_GID) &&
		    zfs_groupmember(zfsvfs, vap->va_gid, cr);

		/*
		 * If both AT_UID and AT_GID are set then take_owner and
		 * take_group must both be set in order to allow taking
		 * ownership.
		 *
		 * Otherwise, send the check through secpolicy_vnode_setattr()
		 *
		 */

		if (((idmask == (AT_UID|AT_GID)) && take_owner && take_group) ||
		    ((idmask == AT_UID) && take_owner) ||
		    ((idmask == AT_GID) && take_group)) {
			if (zfs_zaccess(zp, ACE_WRITE_OWNER, 0,
			    skipaclchk, cr, mnt_ns) == 0) {
				/*
				 * Remove setuid/setgid for non-privileged users
				 */
				secpolicy_setid_clear(vap, vp, cr);
				trim_mask = (mask & (AT_UID|AT_GID));
			} else {
				need_policy =  TRUE;
			}
		} else {
			need_policy =  TRUE;
		}
	}

	oldva.va_mode = zp->z_mode;
	zfs_fuid_map_ids(zp, cr, &oldva.va_uid, &oldva.va_gid);
	if (mask & AT_XVATTR) {
		/*
		 * Update xvattr mask to include only those attributes
		 * that are actually changing.
		 *
		 * the bits will be restored prior to actually setting
		 * the attributes so the caller thinks they were set.
		 */
		if (XVA_ISSET_REQ(xvap, XAT_APPENDONLY)) {
			if (xoap->xoa_appendonly !=
			    ((zp->z_pflags & ZFS_APPENDONLY) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_APPENDONLY);
				XVA_SET_REQ(&tmpxvattr, XAT_APPENDONLY);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_PROJINHERIT)) {
			if (xoap->xoa_projinherit !=
			    ((zp->z_pflags & ZFS_PROJINHERIT) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_PROJINHERIT);
				XVA_SET_REQ(&tmpxvattr, XAT_PROJINHERIT);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NOUNLINK)) {
			if (xoap->xoa_nounlink !=
			    ((zp->z_pflags & ZFS_NOUNLINK) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NOUNLINK);
				XVA_SET_REQ(&tmpxvattr, XAT_NOUNLINK);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_IMMUTABLE)) {
			if (xoap->xoa_immutable !=
			    ((zp->z_pflags & ZFS_IMMUTABLE) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_IMMUTABLE);
				XVA_SET_REQ(&tmpxvattr, XAT_IMMUTABLE);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_NODUMP)) {
			if (xoap->xoa_nodump !=
			    ((zp->z_pflags & ZFS_NODUMP) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_NODUMP);
				XVA_SET_REQ(&tmpxvattr, XAT_NODUMP);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_MODIFIED)) {
			if (xoap->xoa_av_modified !=
			    ((zp->z_pflags & ZFS_AV_MODIFIED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_MODIFIED);
				XVA_SET_REQ(&tmpxvattr, XAT_AV_MODIFIED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_QUARANTINED)) {
			if ((vp->v_type != VREG &&
			    xoap->xoa_av_quarantined) ||
			    xoap->xoa_av_quarantined !=
			    ((zp->z_pflags & ZFS_AV_QUARANTINED) != 0)) {
				need_policy = TRUE;
			} else {
				XVA_CLR_REQ(xvap, XAT_AV_QUARANTINED);
				XVA_SET_REQ(&tmpxvattr, XAT_AV_QUARANTINED);
			}
		}

		if (XVA_ISSET_REQ(xvap, XAT_REPARSE)) {
			zfs_exit(zfsvfs, FTAG);
			return (SET_ERROR(EPERM));
		}

		if (need_policy == FALSE &&
		    (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP) ||
		    XVA_ISSET_REQ(xvap, XAT_OPAQUE))) {
			need_policy = TRUE;
		}
	}

	if (mask & AT_MODE) {
		if (zfs_zaccess(zp, ACE_WRITE_ACL, 0, skipaclchk, cr,
		    mnt_ns) == 0) {
			err = secpolicy_setid_setsticky_clear(vp, vap,
			    &oldva, cr);
			if (err) {
				zfs_exit(zfsvfs, FTAG);
				return (err);
			}
			trim_mask |= AT_MODE;
		} else {
			need_policy = TRUE;
		}
	}

	if (need_policy) {
		/*
		 * If trim_mask is set then take ownership
		 * has been granted or write_acl is present and user
		 * has the ability to modify mode.  In that case remove
		 * UID|GID and or MODE from mask so that
		 * secpolicy_vnode_setattr() doesn't revoke it.
		 */

		if (trim_mask) {
			saved_mask = vap->va_mask;
			vap->va_mask &= ~trim_mask;
			if (trim_mask & AT_MODE) {
				/*
				 * Save the mode, as secpolicy_vnode_setattr()
				 * will overwrite it with ova.va_mode.
				 */
				saved_mode = vap->va_mode;
			}
		}
		err = secpolicy_vnode_setattr(cr, vp, vap, &oldva, flags,
		    (int (*)(void *, int, cred_t *))zfs_zaccess_unix, zp);
		if (err) {
			zfs_exit(zfsvfs, FTAG);
			return (err);
		}

		if (trim_mask) {
			vap->va_mask |= saved_mask;
			if (trim_mask & AT_MODE) {
				/*
				 * Recover the mode after
				 * secpolicy_vnode_setattr().
				 */
				vap->va_mode = saved_mode;
			}
		}
	}

	/*
	 * secpolicy_vnode_setattr, or take ownership may have
	 * changed va_mask
	 */
	mask = vap->va_mask;

	if ((mask & (AT_UID | AT_GID)) || projid != ZFS_INVALID_PROJID) {
		handle_eadir = B_TRUE;
		err = sa_lookup(zp->z_sa_hdl, SA_ZPL_XATTR(zfsvfs),
		    &xattr_obj, sizeof (xattr_obj));

		if (err == 0 && xattr_obj) {
			err = zfs_zget(zp->z_zfsvfs, xattr_obj, &attrzp);
			if (err == 0) {
				err = vn_lock(ZTOV(attrzp), LK_EXCLUSIVE);
				if (err != 0)
					vrele(ZTOV(attrzp));
			}
			if (err)
				goto out2;
		}
		if (mask & AT_UID) {
			new_uid = zfs_fuid_create(zfsvfs,
			    (uint64_t)vap->va_uid, cr, ZFS_OWNER, &fuidp);
			if (new_uid != zp->z_uid &&
			    zfs_id_overquota(zfsvfs, DMU_USERUSED_OBJECT,
			    new_uid)) {
				if (attrzp)
					vput(ZTOV(attrzp));
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (mask & AT_GID) {
			new_gid = zfs_fuid_create(zfsvfs, (uint64_t)vap->va_gid,
			    cr, ZFS_GROUP, &fuidp);
			if (new_gid != zp->z_gid &&
			    zfs_id_overquota(zfsvfs, DMU_GROUPUSED_OBJECT,
			    new_gid)) {
				if (attrzp)
					vput(ZTOV(attrzp));
				err = SET_ERROR(EDQUOT);
				goto out2;
			}
		}

		if (projid != ZFS_INVALID_PROJID &&
		    zfs_id_overquota(zfsvfs, DMU_PROJECTUSED_OBJECT, projid)) {
			if (attrzp)
				vput(ZTOV(attrzp));
			err = SET_ERROR(EDQUOT);
			goto out2;
		}
	}
	tx = dmu_tx_create(os);

	if (mask & AT_MODE) {
		uint64_t pmode = zp->z_mode;
		uint64_t acl_obj;
		new_mode = (pmode & S_IFMT) | (vap->va_mode & ~S_IFMT);

		if (zp->z_zfsvfs->z_acl_mode == ZFS_ACL_RESTRICTED &&
		    !(zp->z_pflags & ZFS_ACL_TRIVIAL)) {
			err = SET_ERROR(EPERM);
			goto out;
		}

		if ((err = zfs_acl_chmod_setattr(zp, &aclp, new_mode)))
			goto out;

		if (!zp->z_is_sa && ((acl_obj = zfs_external_acl(zp)) != 0)) {
			/*
			 * Are we upgrading ACL from old V0 format
			 * to V1 format?
			 */
			if (zfsvfs->z_version >= ZPL_VERSION_FUID &&
			    zfs_znode_acl_version(zp) ==
			    ZFS_ACL_VERSION_INITIAL) {
				dmu_tx_hold_free(tx, acl_obj, 0,
				    DMU_OBJECT_END);
				dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
				    0, aclp->z_acl_bytes);
			} else {
				dmu_tx_hold_write(tx, acl_obj, 0,
				    aclp->z_acl_bytes);
			}
		} else if (!zp->z_is_sa && aclp->z_acl_bytes > ZFS_ACE_SPACE) {
			dmu_tx_hold_write(tx, DMU_NEW_OBJECT,
			    0, aclp->z_acl_bytes);
		}
		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
	} else {
		if (((mask & AT_XVATTR) &&
		    XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP)) ||
		    (projid != ZFS_INVALID_PROJID &&
		    !(zp->z_pflags & ZFS_PROJID)))
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_TRUE);
		else
			dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	}

	if (attrzp) {
		dmu_tx_hold_sa(tx, attrzp->z_sa_hdl, B_FALSE);
	}

	fuid_dirtied = zfsvfs->z_fuid_dirty;
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);

	zfs_sa_upgrade_txholds(tx, zp);

	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err)
		goto out;

	count = 0;
	/*
	 * Set each attribute requested.
	 * We group settings according to the locks they need to acquire.
	 *
	 * Note: you cannot set ctime directly, although it will be
	 * updated as a side-effect of calling this function.
	 */

	if (projid != ZFS_INVALID_PROJID && !(zp->z_pflags & ZFS_PROJID)) {
		/*
		 * For the existed object that is upgraded from old system,
		 * its on-disk layout has no slot for the project ID attribute.
		 * But quota accounting logic needs to access related slots by
		 * offset directly. So we need to adjust old objects' layout
		 * to make the project ID to some unified and fixed offset.
		 */
		if (attrzp)
			err = sa_add_projid(attrzp->z_sa_hdl, tx, projid);
		if (err == 0)
			err = sa_add_projid(zp->z_sa_hdl, tx, projid);

		if (unlikely(err == EEXIST))
			err = 0;
		else if (err != 0)
			goto out;
		else
			projid = ZFS_INVALID_PROJID;
	}

	if (mask & (AT_UID|AT_GID|AT_MODE))
		mutex_enter(&zp->z_acl_lock);

	SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
	    &zp->z_pflags, sizeof (zp->z_pflags));

	if (attrzp) {
		if (mask & (AT_UID|AT_GID|AT_MODE))
			mutex_enter(&attrzp->z_acl_lock);
		SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
		    SA_ZPL_FLAGS(zfsvfs), NULL, &attrzp->z_pflags,
		    sizeof (attrzp->z_pflags));
		if (projid != ZFS_INVALID_PROJID) {
			attrzp->z_projid = projid;
			SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
			    SA_ZPL_PROJID(zfsvfs), NULL, &attrzp->z_projid,
			    sizeof (attrzp->z_projid));
		}
	}

	if (mask & (AT_UID|AT_GID)) {

		if (mask & AT_UID) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_UID(zfsvfs), NULL,
			    &new_uid, sizeof (new_uid));
			zp->z_uid = new_uid;
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_UID(zfsvfs), NULL, &new_uid,
				    sizeof (new_uid));
				attrzp->z_uid = new_uid;
			}
		}

		if (mask & AT_GID) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_GID(zfsvfs),
			    NULL, &new_gid, sizeof (new_gid));
			zp->z_gid = new_gid;
			if (attrzp) {
				SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
				    SA_ZPL_GID(zfsvfs), NULL, &new_gid,
				    sizeof (new_gid));
				attrzp->z_gid = new_gid;
			}
		}
		if (!(mask & AT_MODE)) {
			SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs),
			    NULL, &new_mode, sizeof (new_mode));
			new_mode = zp->z_mode;
		}
		err = zfs_acl_chown_setattr(zp);
		ASSERT0(err);
		if (attrzp) {
			vn_seqc_write_begin(ZTOV(attrzp));
			err = zfs_acl_chown_setattr(attrzp);
			vn_seqc_write_end(ZTOV(attrzp));
			ASSERT0(err);
		}
	}

	if (mask & AT_MODE) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MODE(zfsvfs), NULL,
		    &new_mode, sizeof (new_mode));
		zp->z_mode = new_mode;
		ASSERT3P(aclp, !=, NULL);
		err = zfs_aclset_common(zp, aclp, cr, tx);
		ASSERT0(err);
		if (zp->z_acl_cached)
			zfs_acl_free(zp->z_acl_cached);
		zp->z_acl_cached = aclp;
		aclp = NULL;
	}


	if (mask & AT_ATIME) {
		ZFS_TIME_ENCODE(&vap->va_atime, zp->z_atime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_ATIME(zfsvfs), NULL,
		    &zp->z_atime, sizeof (zp->z_atime));
	}

	if (mask & AT_MTIME) {
		ZFS_TIME_ENCODE(&vap->va_mtime, mtime);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    mtime, sizeof (mtime));
	}

	if (projid != ZFS_INVALID_PROJID) {
		zp->z_projid = projid;
		SA_ADD_BULK_ATTR(bulk, count,
		    SA_ZPL_PROJID(zfsvfs), NULL, &zp->z_projid,
		    sizeof (zp->z_projid));
	}

	/* XXX - shouldn't this be done *before* the ATIME/MTIME checks? */
	if (mask & AT_SIZE && !(mask & AT_MTIME)) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs),
		    NULL, mtime, sizeof (mtime));
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, sizeof (ctime));
		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
	} else if (mask != 0) {
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, sizeof (ctime));
		zfs_tstamp_update_setup(zp, STATE_CHANGED, mtime, ctime);
		if (attrzp) {
			SA_ADD_BULK_ATTR(xattr_bulk, xattr_count,
			    SA_ZPL_CTIME(zfsvfs), NULL,
			    &ctime, sizeof (ctime));
			zfs_tstamp_update_setup(attrzp, STATE_CHANGED,
			    mtime, ctime);
		}
	}

	/*
	 * Do this after setting timestamps to prevent timestamp
	 * update from toggling bit
	 */

	if (xoap && (mask & AT_XVATTR)) {

		if (XVA_ISSET_REQ(xvap, XAT_CREATETIME))
			xoap->xoa_createtime = vap->va_birthtime;
		/*
		 * restore trimmed off masks
		 * so that return masks can be set for caller.
		 */

		if (XVA_ISSET_REQ(&tmpxvattr, XAT_APPENDONLY)) {
			XVA_SET_REQ(xvap, XAT_APPENDONLY);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_NOUNLINK)) {
			XVA_SET_REQ(xvap, XAT_NOUNLINK);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_IMMUTABLE)) {
			XVA_SET_REQ(xvap, XAT_IMMUTABLE);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_NODUMP)) {
			XVA_SET_REQ(xvap, XAT_NODUMP);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_AV_MODIFIED)) {
			XVA_SET_REQ(xvap, XAT_AV_MODIFIED);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_AV_QUARANTINED)) {
			XVA_SET_REQ(xvap, XAT_AV_QUARANTINED);
		}
		if (XVA_ISSET_REQ(&tmpxvattr, XAT_PROJINHERIT)) {
			XVA_SET_REQ(xvap, XAT_PROJINHERIT);
		}

		if (XVA_ISSET_REQ(xvap, XAT_AV_SCANSTAMP))
			ASSERT3S(vp->v_type, ==, VREG);

		zfs_xvattr_set(zp, xvap, tx);
	}

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	if (mask != 0)
		zfs_log_setattr(zilog, tx, TX_SETATTR, zp, vap, mask, fuidp);

	if (mask & (AT_UID|AT_GID|AT_MODE))
		mutex_exit(&zp->z_acl_lock);

	if (attrzp) {
		if (mask & (AT_UID|AT_GID|AT_MODE))
			mutex_exit(&attrzp->z_acl_lock);
	}
out:
	if (err == 0 && attrzp) {
		err2 = sa_bulk_update(attrzp->z_sa_hdl, xattr_bulk,
		    xattr_count, tx);
		ASSERT0(err2);
	}

	if (attrzp)
		vput(ZTOV(attrzp));

	if (aclp)
		zfs_acl_free(aclp);

	if (fuidp) {
		zfs_fuid_info_free(fuidp);
		fuidp = NULL;
	}

	if (err) {
		dmu_tx_abort(tx);
	} else {
		err2 = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
		dmu_tx_commit(tx);
		if (attrzp) {
			if (err2 == 0 && handle_eadir)
				err = zfs_setattr_dir(attrzp);
		}
	}

out2:
	if (os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (err);
}

/*
 * Look up the directory entries corresponding to the source and target
 * directory/name pairs.
 */
static int
zfs_rename_relock_lookup(znode_t *sdzp, const struct componentname *scnp,
    znode_t **szpp, znode_t *tdzp, const struct componentname *tcnp,
    znode_t **tzpp)
{
	zfsvfs_t *zfsvfs;
	znode_t *szp, *tzp;
	int error;

	/*
	 * Before using sdzp and tdzp we must ensure that they are live.
	 * As a porting legacy from illumos we have two things to worry
	 * about.  One is typical for FreeBSD and it is that the vnode is
	 * not reclaimed (doomed).  The other is that the znode is live.
	 * The current code can invalidate the znode without acquiring the
	 * corresponding vnode lock if the object represented by the znode
	 * and vnode is no longer valid after a rollback or receive operation.
	 * z_teardown_lock hidden behind zfs_enter and zfs_exit is the lock
	 * that protects the znodes from the invalidation.
	 */
	zfsvfs = sdzp->z_zfsvfs;
	ASSERT3P(zfsvfs, ==, tdzp->z_zfsvfs);
	if ((error = zfs_enter_verify_zp(zfsvfs, sdzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(tdzp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Re-resolve svp to be certain it still exists and fetch the
	 * correct vnode.
	 */
	error = zfs_dirent_lookup(sdzp, scnp->cn_nameptr, &szp, ZEXISTS);
	if (error != 0) {
		/* Source entry invalid or not there. */
		if ((scnp->cn_flags & ISDOTDOT) != 0 ||
		    (scnp->cn_namelen == 1 && scnp->cn_nameptr[0] == '.'))
			error = SET_ERROR(EINVAL);
		goto out;
	}
	*szpp = szp;

	/*
	 * Re-resolve tvp, if it disappeared we just carry on.
	 */
	error = zfs_dirent_lookup(tdzp, tcnp->cn_nameptr, &tzp, 0);
	if (error != 0) {
		vrele(ZTOV(szp));
		if ((tcnp->cn_flags & ISDOTDOT) != 0)
			error = SET_ERROR(EINVAL);
		goto out;
	}
	*tzpp = tzp;
out:
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * We acquire all but fdvp locks using non-blocking acquisitions.  If we
 * fail to acquire any lock in the path we will drop all held locks,
 * acquire the new lock in a blocking fashion, and then release it and
 * restart the rename.  This acquire/release step ensures that we do not
 * spin on a lock waiting for release.  On error release all vnode locks
 * and decrement references the way tmpfs_rename() would do.
 */
static int
zfs_rename_relock(struct vnode *sdvp, struct vnode **svpp,
    struct vnode *tdvp, struct vnode **tvpp,
    const struct componentname *scnp, const struct componentname *tcnp)
{
	struct vnode	*nvp, *svp, *tvp;
	znode_t		*sdzp, *tdzp, *szp, *tzp;
	int		error;

	VOP_UNLOCK(tdvp);
	if (*tvpp != NULL && *tvpp != tdvp)
		VOP_UNLOCK(*tvpp);

relock:
	error = vn_lock(sdvp, LK_EXCLUSIVE);
	if (error)
		goto out;
	error = vn_lock(tdvp, LK_EXCLUSIVE | LK_NOWAIT);
	if (error != 0) {
		VOP_UNLOCK(sdvp);
		if (error != EBUSY)
			goto out;
		error = vn_lock(tdvp, LK_EXCLUSIVE);
		if (error)
			goto out;
		VOP_UNLOCK(tdvp);
		goto relock;
	}
	tdzp = VTOZ(tdvp);
	sdzp = VTOZ(sdvp);

	error = zfs_rename_relock_lookup(sdzp, scnp, &szp, tdzp, tcnp, &tzp);
	if (error != 0) {
		VOP_UNLOCK(sdvp);
		VOP_UNLOCK(tdvp);
		goto out;
	}
	svp = ZTOV(szp);
	tvp = tzp != NULL ? ZTOV(tzp) : NULL;

	/*
	 * Now try acquire locks on svp and tvp.
	 */
	nvp = svp;
	error = vn_lock(nvp, LK_EXCLUSIVE | LK_NOWAIT);
	if (error != 0) {
		VOP_UNLOCK(sdvp);
		VOP_UNLOCK(tdvp);
		if (tvp != NULL)
			vrele(tvp);
		if (error != EBUSY) {
			vrele(nvp);
			goto out;
		}
		error = vn_lock(nvp, LK_EXCLUSIVE);
		if (error != 0) {
			vrele(nvp);
			goto out;
		}
		VOP_UNLOCK(nvp);
		/*
		 * Concurrent rename race.
		 * XXX ?
		 */
		if (nvp == tdvp) {
			vrele(nvp);
			error = SET_ERROR(EINVAL);
			goto out;
		}
		vrele(*svpp);
		*svpp = nvp;
		goto relock;
	}
	vrele(*svpp);
	*svpp = nvp;

	if (*tvpp != NULL)
		vrele(*tvpp);
	*tvpp = NULL;
	if (tvp != NULL) {
		nvp = tvp;
		error = vn_lock(nvp, LK_EXCLUSIVE | LK_NOWAIT);
		if (error != 0) {
			VOP_UNLOCK(sdvp);
			VOP_UNLOCK(tdvp);
			VOP_UNLOCK(*svpp);
			if (error != EBUSY) {
				vrele(nvp);
				goto out;
			}
			error = vn_lock(nvp, LK_EXCLUSIVE);
			if (error != 0) {
				vrele(nvp);
				goto out;
			}
			vput(nvp);
			goto relock;
		}
		*tvpp = nvp;
	}

	return (0);

out:
	return (error);
}

/*
 * Note that we must use VRELE_ASYNC in this function as it walks
 * up the directory tree and vrele may need to acquire an exclusive
 * lock if a last reference to a vnode is dropped.
 */
static int
zfs_rename_check(znode_t *szp, znode_t *sdzp, znode_t *tdzp)
{
	zfsvfs_t	*zfsvfs;
	znode_t		*zp, *zp1;
	uint64_t	parent;
	int		error;

	zfsvfs = tdzp->z_zfsvfs;
	if (tdzp == szp)
		return (SET_ERROR(EINVAL));
	if (tdzp == sdzp)
		return (0);
	if (tdzp->z_id == zfsvfs->z_root)
		return (0);
	zp = tdzp;
	for (;;) {
		ASSERT(!zp->z_unlinked);
		if ((error = sa_lookup(zp->z_sa_hdl,
		    SA_ZPL_PARENT(zfsvfs), &parent, sizeof (parent))) != 0)
			break;

		if (parent == szp->z_id) {
			error = SET_ERROR(EINVAL);
			break;
		}
		if (parent == zfsvfs->z_root)
			break;
		if (parent == sdzp->z_id)
			break;

		error = zfs_zget(zfsvfs, parent, &zp1);
		if (error != 0)
			break;

		if (zp != tdzp)
			VN_RELE_ASYNC(ZTOV(zp),
			    dsl_pool_zrele_taskq(
			    dmu_objset_pool(zfsvfs->z_os)));
		zp = zp1;
	}

	if (error == ENOTDIR)
		panic("checkpath: .. not a directory\n");
	if (zp != tdzp)
		VN_RELE_ASYNC(ZTOV(zp),
		    dsl_pool_zrele_taskq(dmu_objset_pool(zfsvfs->z_os)));
	return (error);
}

static int
zfs_do_rename_impl(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr);

/*
 * Move an entry from the provided source directory to the target
 * directory.  Change the entry name as indicated.
 *
 *	IN:	sdvp	- Source directory containing the "old entry".
 *		scnp	- Old entry name.
 *		tdvp	- Target directory to contain the "new entry".
 *		tcnp	- New entry name.
 *		cr	- credentials of caller.
 *	INOUT:	svpp	- Source file
 *		tvpp	- Target file, may point to NULL initially
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	sdvp,tdvp - ctime|mtime updated
 */
static int
zfs_do_rename(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr)
{
	int	error;

	ASSERT_VOP_ELOCKED(tdvp, __func__);
	if (*tvpp != NULL)
		ASSERT_VOP_ELOCKED(*tvpp, __func__);

	/* Reject renames across filesystems. */
	if ((*svpp)->v_mount != tdvp->v_mount ||
	    ((*tvpp) != NULL && (*svpp)->v_mount != (*tvpp)->v_mount)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	if (zfsctl_is_node(tdvp)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	/*
	 * Lock all four vnodes to ensure safety and semantics of renaming.
	 */
	error = zfs_rename_relock(sdvp, svpp, tdvp, tvpp, scnp, tcnp);
	if (error != 0) {
		/* no vnodes are locked in the case of error here */
		return (error);
	}

	error = zfs_do_rename_impl(sdvp, svpp, scnp, tdvp, tvpp, tcnp, cr);
	VOP_UNLOCK(sdvp);
	VOP_UNLOCK(*svpp);
out:
	if (*tvpp != NULL)
		VOP_UNLOCK(*tvpp);
	if (tdvp != *tvpp)
		VOP_UNLOCK(tdvp);

	return (error);
}

static int
zfs_do_rename_impl(vnode_t *sdvp, vnode_t **svpp, struct componentname *scnp,
    vnode_t *tdvp, vnode_t **tvpp, struct componentname *tcnp,
    cred_t *cr)
{
	dmu_tx_t	*tx;
	zfsvfs_t	*zfsvfs;
	zilog_t		*zilog;
	znode_t		*tdzp, *sdzp, *tzp, *szp;
	const char	*snm = scnp->cn_nameptr;
	const char	*tnm = tcnp->cn_nameptr;
	int		error;

	tdzp = VTOZ(tdvp);
	sdzp = VTOZ(sdvp);
	zfsvfs = tdzp->z_zfsvfs;

	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	if ((error = zfs_verify_zp(sdzp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(tnm,
	    strlen(tnm), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		error = SET_ERROR(EILSEQ);
		goto out;
	}

	/* If source and target are the same file, there is nothing to do. */
	if ((*svpp) == (*tvpp)) {
		error = 0;
		goto out;
	}

	if (((*svpp)->v_type == VDIR && (*svpp)->v_mountedhere != NULL) ||
	    ((*tvpp) != NULL && (*tvpp)->v_type == VDIR &&
	    (*tvpp)->v_mountedhere != NULL)) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	szp = VTOZ(*svpp);
	if ((error = zfs_verify_zp(szp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	tzp = *tvpp == NULL ? NULL : VTOZ(*tvpp);
	if (tzp != NULL) {
		if ((error = zfs_verify_zp(tzp)) != 0) {
			zfs_exit(zfsvfs, FTAG);
			return (error);
		}
	}

	/*
	 * This is to prevent the creation of links into attribute space
	 * by renaming a linked file into/outof an attribute directory.
	 * See the comment in zfs_link() for why this is considered bad.
	 */
	if ((tdzp->z_pflags & ZFS_XATTR) != (sdzp->z_pflags & ZFS_XATTR)) {
		error = SET_ERROR(EINVAL);
		goto out;
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow renames into our tree when the project
	 * IDs are the same.
	 */
	if (tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		error = SET_ERROR(EXDEV);
		goto out;
	}

	/*
	 * Must have write access at the source to remove the old entry
	 * and write access at the target to create the new entry.
	 * Note that if target and source are the same, this can be
	 * done in a single check.
	 */
	if ((error = zfs_zaccess_rename(sdzp, szp, tdzp, tzp, cr, NULL)))
		goto out;

	if ((*svpp)->v_type == VDIR) {
		/*
		 * Avoid ".", "..", and aliases of "." for obvious reasons.
		 */
		if ((scnp->cn_namelen == 1 && scnp->cn_nameptr[0] == '.') ||
		    sdzp == szp ||
		    (scnp->cn_flags | tcnp->cn_flags) & ISDOTDOT) {
			error = EINVAL;
			goto out;
		}

		/*
		 * Check to make sure rename is valid.
		 * Can't do a move like this: /usr/a/b to /usr/a/b/c/d
		 */
		if ((error = zfs_rename_check(szp, sdzp, tdzp)))
			goto out;
	}

	/*
	 * Does target exist?
	 */
	if (tzp) {
		/*
		 * Source and target must be the same type.
		 */
		if ((*svpp)->v_type == VDIR) {
			if ((*tvpp)->v_type != VDIR) {
				error = SET_ERROR(ENOTDIR);
				goto out;
			} else {
				cache_purge(tdvp);
				if (sdvp != tdvp)
					cache_purge(sdvp);
			}
		} else {
			if ((*tvpp)->v_type == VDIR) {
				error = SET_ERROR(EISDIR);
				goto out;
			}
		}
	}

	vn_seqc_write_begin(*svpp);
	vn_seqc_write_begin(sdvp);
	if (*tvpp != NULL)
		vn_seqc_write_begin(*tvpp);
	if (tdvp != *tvpp)
		vn_seqc_write_begin(tdvp);

	vnevent_rename_src(*svpp, sdvp, scnp->cn_nameptr, ct);
	if (tzp)
		vnevent_rename_dest(*tvpp, tdvp, tnm, ct);

	/*
	 * notify the target directory if it is not the same
	 * as source directory.
	 */
	if (tdvp != sdvp) {
		vnevent_rename_dest_dir(tdvp, ct);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_sa(tx, sdzp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, sdzp->z_id, FALSE, snm);
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, tnm);
	if (sdzp != tdzp) {
		dmu_tx_hold_sa(tx, tdzp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, tdzp);
	}
	if (tzp) {
		dmu_tx_hold_sa(tx, tzp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, tzp);
	}

	zfs_sa_upgrade_txholds(tx, szp);
	dmu_tx_hold_zap(tx, zfsvfs->z_unlinkedobj, FALSE, NULL);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		goto out_seq;
	}

	if (tzp)	/* Attempt to remove the existing target */
		error = zfs_link_destroy(tdzp, tnm, tzp, tx, 0, NULL);

	if (error == 0) {
		error = zfs_link_create(tdzp, tnm, szp, tx, ZRENAMING);
		if (error == 0) {
			szp->z_pflags |= ZFS_AV_MODIFIED;

			error = sa_update(szp->z_sa_hdl, SA_ZPL_FLAGS(zfsvfs),
			    (void *)&szp->z_pflags, sizeof (uint64_t), tx);
			ASSERT0(error);

			error = zfs_link_destroy(sdzp, snm, szp, tx, ZRENAMING,
			    NULL);
			if (error == 0) {
				zfs_log_rename(zilog, tx, TX_RENAME, sdzp,
				    snm, tdzp, tnm, szp);
			} else {
				/*
				 * At this point, we have successfully created
				 * the target name, but have failed to remove
				 * the source name.  Since the create was done
				 * with the ZRENAMING flag, there are
				 * complications; for one, the link count is
				 * wrong.  The easiest way to deal with this
				 * is to remove the newly created target, and
				 * return the original error.  This must
				 * succeed; fortunately, it is very unlikely to
				 * fail, since we just created it.
				 */
				VERIFY0(zfs_link_destroy(tdzp, tnm, szp, tx,
				    ZRENAMING, NULL));
			}
		}
		if (error == 0) {
			cache_vop_rename(sdvp, *svpp, tdvp, *tvpp, scnp, tcnp);
		}
	}

	dmu_tx_commit(tx);

out_seq:
	vn_seqc_write_end(*svpp);
	vn_seqc_write_end(sdvp);
	if (*tvpp != NULL)
		vn_seqc_write_end(*tvpp);
	if (tdvp != *tvpp)
		vn_seqc_write_end(tdvp);

out:
	if (error == 0 && zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);
	zfs_exit(zfsvfs, FTAG);

	return (error);
}

int
zfs_rename(znode_t *sdzp, const char *sname, znode_t *tdzp, const char *tname,
    cred_t *cr, int flags, uint64_t rflags, vattr_t *wo_vap, zidmap_t *mnt_ns)
{
	struct componentname scn, tcn;
	vnode_t *sdvp, *tdvp;
	vnode_t *svp, *tvp;
	int error;
	svp = tvp = NULL;

	if (is_nametoolong(tdzp->z_zfsvfs, tname))
		return (SET_ERROR(ENAMETOOLONG));

	if (rflags != 0 || wo_vap != NULL)
		return (SET_ERROR(EINVAL));

	sdvp = ZTOV(sdzp);
	tdvp = ZTOV(tdzp);
	error = zfs_lookup_internal(sdzp, sname, &svp, &scn, DELETE);
	if (sdzp->z_zfsvfs->z_replay == B_FALSE)
		VOP_UNLOCK(sdvp);
	if (error != 0)
		goto fail;
	VOP_UNLOCK(svp);

	vn_lock(tdvp, LK_EXCLUSIVE | LK_RETRY);
	error = zfs_lookup_internal(tdzp, tname, &tvp, &tcn, RENAME);
	if (error == EJUSTRETURN)
		tvp = NULL;
	else if (error != 0) {
		VOP_UNLOCK(tdvp);
		goto fail;
	}

	error = zfs_do_rename(sdvp, &svp, &scn, tdvp, &tvp, &tcn, cr);
fail:
	if (svp != NULL)
		vrele(svp);
	if (tvp != NULL)
		vrele(tvp);

	return (error);
}

/*
 * Insert the indicated symbolic reference entry into the directory.
 *
 *	IN:	dvp	- Directory to contain new symbolic link.
 *		link	- Name for new symlink entry.
 *		vap	- Attributes of new entry.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *		flags	- case flags
 *		mnt_ns	- Unused on FreeBSD
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	dvp - ctime|mtime updated
 */
int
zfs_symlink(znode_t *dzp, const char *name, vattr_t *vap,
    const char *link, znode_t **zpp, cred_t *cr, int flags, zidmap_t *mnt_ns)
{
	(void) flags;
	znode_t		*zp;
	dmu_tx_t	*tx;
	zfsvfs_t	*zfsvfs = dzp->z_zfsvfs;
	zilog_t		*zilog;
	uint64_t	len = strlen(link);
	int		error;
	zfs_acl_ids_t	acl_ids;
	boolean_t	fuid_dirtied;
	uint64_t	txtype = TX_SYMLINK;

	ASSERT3S(vap->va_type, ==, VLNK);

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	if ((error = zfs_enter_verify_zp(zfsvfs, dzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	if (zfsvfs->z_utf8 && u8_validate(name, strlen(name),
	    NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	if (len > MAXPATHLEN) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(ENAMETOOLONG));
	}

	if ((error = zfs_acl_ids_create(dzp, 0,
	    vap, cr, NULL, &acl_ids, NULL)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lookup(dzp, name, &zp, ZNEW);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if ((error = zfs_zaccess(dzp, ACE_ADD_FILE, 0, B_FALSE, cr, mnt_ns))) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	if (zfs_acl_ids_overquota(zfsvfs, &acl_ids, ZFS_DEFAULT_PROJID)) {
		zfs_acl_ids_free(&acl_ids);
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EDQUOT));
	}

	getnewvnode_reserve();
	tx = dmu_tx_create(zfsvfs->z_os);
	fuid_dirtied = zfsvfs->z_fuid_dirty;
	dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0, MAX(1, len));
	dmu_tx_hold_zap(tx, dzp->z_id, TRUE, name);
	dmu_tx_hold_sa_create(tx, acl_ids.z_aclp->z_acl_bytes +
	    ZFS_SA_BASE_ATTR_SIZE + len);
	dmu_tx_hold_sa(tx, dzp->z_sa_hdl, B_FALSE);
	if (!zfsvfs->z_use_sa && acl_ids.z_aclp->z_acl_bytes > ZFS_ACE_SPACE) {
		dmu_tx_hold_write(tx, DMU_NEW_OBJECT, 0,
		    acl_ids.z_aclp->z_acl_bytes);
	}
	if (fuid_dirtied)
		zfs_fuid_txhold(zfsvfs, tx);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		zfs_acl_ids_free(&acl_ids);
		dmu_tx_abort(tx);
		getnewvnode_drop_reserve();
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Create a new object for the symlink.
	 * for version 4 ZPL datasets the symlink will be an SA attribute
	 */
	zfs_mknode(dzp, vap, tx, cr, 0, &zp, &acl_ids);

	if (fuid_dirtied)
		zfs_fuid_sync(zfsvfs, tx);

	if (zp->z_is_sa)
		error = sa_update(zp->z_sa_hdl, SA_ZPL_SYMLINK(zfsvfs),
		    __DECONST(void *, link), len, tx);
	else
		zfs_sa_symlink(zp, __DECONST(char *, link), len, tx);

	zp->z_size = len;
	(void) sa_update(zp->z_sa_hdl, SA_ZPL_SIZE(zfsvfs),
	    &zp->z_size, sizeof (zp->z_size), tx);
	/*
	 * Insert the new object into the directory.
	 */
	error = zfs_link_create(dzp, name, zp, tx, ZNEW);
	if (error != 0) {
		zfs_znode_delete(zp, tx);
		VOP_UNLOCK(ZTOV(zp));
		zrele(zp);
	} else {
		zfs_log_symlink(zilog, tx, txtype, dzp, zp, name, link);
	}

	zfs_acl_ids_free(&acl_ids);

	dmu_tx_commit(tx);

	getnewvnode_drop_reserve();

	if (error == 0) {
		*zpp = zp;

		if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
			zil_commit(zilog, 0);
	}

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Return, in the buffer contained in the provided uio structure,
 * the symbolic path referred to by vp.
 *
 *	IN:	vp	- vnode of symbolic link.
 *		uio	- structure to contain the link path.
 *		cr	- credentials of caller.
 *		ct	- caller context
 *
 *	OUT:	uio	- structure containing the link path.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	vp - atime updated
 */
static int
zfs_readlink(vnode_t *vp, zfs_uio_t *uio, cred_t *cr, caller_context_t *ct)
{
	(void) cr, (void) ct;
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (zp->z_is_sa)
		error = sa_lookup_uio(zp->z_sa_hdl,
		    SA_ZPL_SYMLINK(zfsvfs), uio);
	else
		error = zfs_sa_readlink(zp, uio);

	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Insert a new entry into directory tdvp referencing svp.
 *
 *	IN:	tdvp	- Directory to contain new entry.
 *		svp	- vnode of new entry.
 *		name	- name of new entry.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	tdvp - ctime|mtime updated
 *	 svp - ctime updated
 */
int
zfs_link(znode_t *tdzp, znode_t *szp, const char *name, cred_t *cr,
    int flags)
{
	(void) flags;
	znode_t		*tzp;
	zfsvfs_t	*zfsvfs = tdzp->z_zfsvfs;
	zilog_t		*zilog;
	dmu_tx_t	*tx;
	int		error;
	uint64_t	parent;
	uid_t		owner;

	ASSERT3S(ZTOV(tdzp)->v_type, ==, VDIR);

	if (is_nametoolong(zfsvfs, name))
		return (SET_ERROR(ENAMETOOLONG));

	if ((error = zfs_enter_verify_zp(zfsvfs, tdzp, FTAG)) != 0)
		return (error);
	zilog = zfsvfs->z_log;

	/*
	 * POSIX dictates that we return EPERM here.
	 * Better choices include ENOTSUP or EISDIR.
	 */
	if (ZTOV(szp)->v_type == VDIR) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_verify_zp(szp)) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * If we are using project inheritance, means if the directory has
	 * ZFS_PROJINHERIT set, then its descendant directories will inherit
	 * not only the project ID, but also the ZFS_PROJINHERIT flag. Under
	 * such case, we only allow hard link creation in our tree when the
	 * project IDs are the same.
	 */
	if (tdzp->z_pflags & ZFS_PROJINHERIT &&
	    tdzp->z_projid != szp->z_projid) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EXDEV));
	}

	if (szp->z_pflags & (ZFS_APPENDONLY |
	    ZFS_IMMUTABLE | ZFS_READONLY)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	/* Prevent links to .zfs/shares files */

	if ((error = sa_lookup(szp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
	    &parent, sizeof (uint64_t))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	if (parent == zfsvfs->z_shares_dir) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if (zfsvfs->z_utf8 && u8_validate(name,
	    strlen(name), NULL, U8_VALIDATE_ENTIRE, &error) < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EILSEQ));
	}

	/*
	 * We do not support links between attributes and non-attributes
	 * because of the potential security risk of creating links
	 * into "normal" file space in order to circumvent restrictions
	 * imposed in attribute space.
	 */
	if ((szp->z_pflags & ZFS_XATTR) != (tdzp->z_pflags & ZFS_XATTR)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}


	owner = zfs_fuid_map_id(zfsvfs, szp->z_uid, cr, ZFS_OWNER);
	if (owner != crgetuid(cr) && secpolicy_basic_link(ZTOV(szp), cr) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EPERM));
	}

	if ((error = zfs_zaccess(tdzp, ACE_ADD_FILE, 0, B_FALSE, cr, NULL))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	/*
	 * Attempt to lock directory; fail if entry already exists.
	 */
	error = zfs_dirent_lookup(tdzp, name, &tzp, ZNEW);
	if (error) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_sa(tx, szp->z_sa_hdl, B_FALSE);
	dmu_tx_hold_zap(tx, tdzp->z_id, TRUE, name);
	zfs_sa_upgrade_txholds(tx, szp);
	zfs_sa_upgrade_txholds(tx, tdzp);
	error = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	error = zfs_link_create(tdzp, name, szp, tx, 0);

	if (error == 0) {
		uint64_t txtype = TX_LINK;
		zfs_log_link(zilog, tx, txtype, tdzp, szp, name);
	}

	dmu_tx_commit(tx);

	if (error == 0) {
		vnevent_link(ZTOV(szp), ct);
	}

	if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS)
		zil_commit(zilog, 0);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

/*
 * Free or allocate space in a file.  Currently, this function only
 * supports the `F_FREESP' command.  However, this command is somewhat
 * misnamed, as its functionality includes the ability to allocate as
 * well as free space.
 *
 *	IN:	ip	- inode of file to free data in.
 *		cmd	- action to take (only F_FREESP supported).
 *		bfp	- section of file to free/alloc.
 *		flag	- current file open mode flags.
 *		offset	- current file offset.
 *		cr	- credentials of caller.
 *
 *	RETURN:	0 on success, error code on failure.
 *
 * Timestamps:
 *	ip - ctime|mtime updated
 */
int
zfs_space(znode_t *zp, int cmd, flock64_t *bfp, int flag,
    offset_t offset, cred_t *cr)
{
	(void) offset;
	zfsvfs_t	*zfsvfs = ZTOZSB(zp);
	uint64_t	off, len;
	int		error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if (cmd != F_FREESP) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	if (bfp->l_len < 0) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Permissions aren't checked on Solaris because on this OS
	 * zfs_space() can only be called with an opened file handle.
	 * On Linux we can get here through truncate_range() which
	 * operates directly on inodes, so we need to check access rights.
	 */
	if ((error = zfs_zaccess(zp, ACE_WRITE_DATA, 0, B_FALSE, cr, NULL))) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	off = bfp->l_start;
	len = bfp->l_len; /* 0 means from off to end of file */

	error = zfs_freesp(zp, off, len, flag, TRUE);

	zfs_exit(zfsvfs, FTAG);
	return (error);
}

static void
zfs_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	(void) cr, (void) ct;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int error;

	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	if (zp->z_sa_hdl == NULL) {
		/*
		 * The fs has been unmounted, or we did a
		 * suspend/resume and this file no longer exists.
		 */
		ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
		vrecycle(vp);
		return;
	}

	if (zp->z_unlinked) {
		/*
		 * Fast path to recycle a vnode of a removed file.
		 */
		ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
		vrecycle(vp);
		return;
	}

	if (zp->z_atime_dirty && zp->z_unlinked == 0) {
		dmu_tx_t *tx = dmu_tx_create(zfsvfs->z_os);

		dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
		zfs_sa_upgrade_txholds(tx, zp);
		error = dmu_tx_assign(tx, DMU_TX_WAIT);
		if (error) {
			dmu_tx_abort(tx);
		} else {
			(void) sa_update(zp->z_sa_hdl, SA_ZPL_ATIME(zfsvfs),
			    (void *)&zp->z_atime, sizeof (zp->z_atime), tx);
			zp->z_atime_dirty = 0;
			dmu_tx_commit(tx);
		}
	}
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);
}


_Static_assert(sizeof (struct zfid_short) <= sizeof (struct fid),
	"struct zfid_short bigger than struct fid");
_Static_assert(sizeof (struct zfid_long) <= sizeof (struct fid),
	"struct zfid_long bigger than struct fid");

static int
zfs_fid(vnode_t *vp, fid_t *fidp, caller_context_t *ct)
{
	(void) ct;
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	uint32_t	gen;
	uint64_t	gen64;
	uint64_t	object = zp->z_id;
	zfid_short_t	*zfid;
	int		size, i, error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	if ((error = sa_lookup(zp->z_sa_hdl, SA_ZPL_GEN(zfsvfs),
	    &gen64, sizeof (uint64_t))) != 0) {
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}

	gen = (uint32_t)gen64;

	size = (zfsvfs->z_parent != zfsvfs) ? LONG_FID_LEN : SHORT_FID_LEN;
	fidp->fid_len = size;

	zfid = (zfid_short_t *)fidp;

	zfid->zf_len = size;

	for (i = 0; i < sizeof (zfid->zf_object); i++)
		zfid->zf_object[i] = (uint8_t)(object >> (8 * i));

	/* Must have a non-zero generation number to distinguish from .zfs */
	if (gen == 0)
		gen = 1;
	for (i = 0; i < sizeof (zfid->zf_gen); i++)
		zfid->zf_gen[i] = (uint8_t)(gen >> (8 * i));

	if (size == LONG_FID_LEN) {
		uint64_t	objsetid = dmu_objset_id(zfsvfs->z_os);
		zfid_long_t	*zlfid;

		zlfid = (zfid_long_t *)fidp;

		for (i = 0; i < sizeof (zlfid->zf_setid); i++)
			zlfid->zf_setid[i] = (uint8_t)(objsetid >> (8 * i));

		/* XXX - this should be the generation number for the objset */
		for (i = 0; i < sizeof (zlfid->zf_setgen); i++)
			zlfid->zf_setgen[i] = 0;
	}

	zfs_exit(zfsvfs, FTAG);
	return (0);
}

static int
zfs_pathconf(vnode_t *vp, int cmd, ulong_t *valp, cred_t *cr,
    caller_context_t *ct)
{
	znode_t *zp;
	zfsvfs_t *zfsvfs;
	int error;

	switch (cmd) {
	case _PC_LINK_MAX:
		*valp = MIN(LONG_MAX, ZFS_LINK_MAX);
		return (0);

	case _PC_FILESIZEBITS:
		*valp = 64;
		return (0);
	case _PC_MIN_HOLE_SIZE:
		*valp = (int)SPA_MINBLOCKSIZE;
		return (0);
	case _PC_ACL_EXTENDED:
#if 0		/* POSIX ACLs are not implemented for ZFS on FreeBSD yet. */
		zp = VTOZ(vp);
		zfsvfs = zp->z_zfsvfs;
		if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
			return (error);
		*valp = zfsvfs->z_acl_type == ZFSACLTYPE_POSIX ? 1 : 0;
		zfs_exit(zfsvfs, FTAG);
#else
		*valp = 0;
#endif
		return (0);

	case _PC_ACL_NFS4:
		zp = VTOZ(vp);
		zfsvfs = zp->z_zfsvfs;
		if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
			return (error);
		*valp = zfsvfs->z_acl_type == ZFS_ACLTYPE_NFSV4 ? 1 : 0;
		zfs_exit(zfsvfs, FTAG);
		return (0);

	case _PC_ACL_PATH_MAX:
		*valp = ACL_MAX_ENTRIES;
		return (0);

	default:
		return (EOPNOTSUPP);
	}
}

static int
zfs_getpages(struct vnode *vp, vm_page_t *ma, int count, int *rbehind,
    int *rahead)
{
	znode_t *zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t *lr;
	vm_object_t object;
	off_t start, end, obj_size;
	uint_t blksz;
	int pgsin_b, pgsin_a;
	int error;

	if (zfs_enter_verify_zp(zfsvfs, zp, FTAG) != 0)
		return (zfs_vm_pagerret_error);

	object = ma[0]->object;
	start = IDX_TO_OFF(ma[0]->pindex);
	end = IDX_TO_OFF(ma[count - 1]->pindex + 1);

	/*
	 * Lock a range covering all required and optional pages.
	 * Note that we need to handle the case of the block size growing.
	 */
	for (;;) {
		uint64_t len;

		blksz = zp->z_blksz;
		len = roundup(end, blksz) - rounddown(start, blksz);

		lr = zfs_rangelock_tryenter(&zp->z_rangelock,
		    rounddown(start, blksz), len, RL_READER);
		if (lr == NULL) {
			/*
			 * Avoid a deadlock with update_pages().  We need to
			 * hold the range lock when copying from the DMU, so
			 * give up the busy lock to allow update_pages() to
			 * proceed.  We might need to allocate new pages, which
			 * isn't quite right since this allocation isn't subject
			 * to the page fault handler's OOM logic, but this is
			 * the best we can do for now.
			 */
			for (int i = 0; i < count; i++)
				vm_page_xunbusy(ma[i]);

			lr = zfs_rangelock_enter(&zp->z_rangelock,
			    rounddown(start, blksz), len, RL_READER);

			zfs_vmobject_wlock(object);
			(void) vm_page_grab_pages(object, OFF_TO_IDX(start),
			    VM_ALLOC_NORMAL | VM_ALLOC_WAITOK | VM_ALLOC_ZERO,
			    ma, count);
			zfs_vmobject_wunlock(object);
		}
		if (blksz == zp->z_blksz)
			break;
		zfs_rangelock_exit(lr);
	}

	zfs_vmobject_wlock(object);
	obj_size = object->un_pager.vnp.vnp_size;
	zfs_vmobject_wunlock(object);
	if (IDX_TO_OFF(ma[count - 1]->pindex) >= obj_size) {
		zfs_rangelock_exit(lr);
		zfs_exit(zfsvfs, FTAG);
		return (zfs_vm_pagerret_bad);
	}

	pgsin_b = 0;
	if (rbehind != NULL) {
		pgsin_b = OFF_TO_IDX(start - rounddown(start, blksz));
		pgsin_b = MIN(*rbehind, pgsin_b);
	}

	pgsin_a = 0;
	if (rahead != NULL) {
		pgsin_a = OFF_TO_IDX(roundup(end, blksz) - end);
		if (end + IDX_TO_OFF(pgsin_a) >= obj_size)
			pgsin_a = OFF_TO_IDX(round_page(obj_size) - end);
		pgsin_a = MIN(*rahead, pgsin_a);
	}

	/*
	 * NB: we need to pass the exact byte size of the data that we expect
	 * to read after accounting for the file size.  This is required because
	 * ZFS will panic if we request DMU to read beyond the end of the last
	 * allocated block.
	 */
	for (int i = 0; i < count; i++) {
		int dummypgsin, count1, j, last_size;

		if (vm_page_any_valid(ma[i])) {
			ASSERT(vm_page_all_valid(ma[i]));
			continue;
		}
		for (j = i + 1; j < count; j++) {
			if (vm_page_any_valid(ma[j])) {
				ASSERT(vm_page_all_valid(ma[j]));
				break;
			}
		}
		count1 = j - i;
		dummypgsin = 0;
		last_size = j == count ?
		    MIN(end, obj_size) - (end - PAGE_SIZE) : PAGE_SIZE;
		error = dmu_read_pages(zfsvfs->z_os, zp->z_id, &ma[i], count1,
		    i == 0 ? &pgsin_b : &dummypgsin,
		    j == count ? &pgsin_a : &dummypgsin,
		    last_size);
		if (error != 0)
			break;
		i += count1 - 1;
	}

	zfs_rangelock_exit(lr);
	ZFS_ACCESSTIME_STAMP(zfsvfs, zp);

	dataset_kstats_update_read_kstats(&zfsvfs->z_kstat, count*PAGE_SIZE);

	zfs_exit(zfsvfs, FTAG);

	if (error != 0)
		return (zfs_vm_pagerret_error);

	VM_CNT_INC(v_vnodein);
	VM_CNT_ADD(v_vnodepgsin, count + pgsin_b + pgsin_a);
	if (rbehind != NULL)
		*rbehind = pgsin_b;
	if (rahead != NULL)
		*rahead = pgsin_a;
	return (zfs_vm_pagerret_ok);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_getpages_args {
	struct vnode *a_vp;
	vm_page_t *a_m;
	int a_count;
	int *a_rbehind;
	int *a_rahead;
};
#endif

static int
zfs_freebsd_getpages(struct vop_getpages_args *ap)
{

	return (zfs_getpages(ap->a_vp, ap->a_m, ap->a_count, ap->a_rbehind,
	    ap->a_rahead));
}

typedef struct {
	uint_t		pca_npages;
	vm_page_t	pca_pages[];
} putpage_commit_arg_t;

static void
zfs_putpage_commit_cb(void *arg)
{
	putpage_commit_arg_t *pca = arg;
	vm_object_t object = pca->pca_pages[0]->object;

	zfs_vmobject_wlock(object);

	for (uint_t i = 0; i < pca->pca_npages; i++) {
		vm_page_t pp = pca->pca_pages[i];
		vm_page_undirty(pp);
		vm_page_sunbusy(pp);
	}

	vm_object_pip_wakeupn(object, pca->pca_npages);

	zfs_vmobject_wunlock(object);

	kmem_free(pca,
	    offsetof(putpage_commit_arg_t, pca_pages[pca->pca_npages]));
}

static int
zfs_putpages(struct vnode *vp, vm_page_t *ma, size_t len, int flags,
    int *rtvals)
{
	znode_t		*zp = VTOZ(vp);
	zfsvfs_t	*zfsvfs = zp->z_zfsvfs;
	zfs_locked_range_t		*lr;
	dmu_tx_t	*tx;
	struct sf_buf	*sf;
	vm_object_t	object;
	vm_page_t	m;
	caddr_t		va;
	size_t		tocopy;
	size_t		lo_len;
	vm_ooffset_t	lo_off;
	vm_ooffset_t	off;
	uint_t		blksz;
	int		ncount;
	int		pcount;
	int		err;
	int		i;

	object = vp->v_object;
	KASSERT(ma[0]->object == object, ("mismatching object"));
	KASSERT(len > 0 && (len & PAGE_MASK) == 0, ("unexpected length"));

	pcount = btoc(len);
	ncount = pcount;
	for (i = 0; i < pcount; i++)
		rtvals[i] = zfs_vm_pagerret_error;

	if (zfs_enter_verify_zp(zfsvfs, zp, FTAG) != 0)
		return (zfs_vm_pagerret_error);

	off = IDX_TO_OFF(ma[0]->pindex);
	blksz = zp->z_blksz;
	lo_off = rounddown(off, blksz);
	lo_len = roundup(len + (off - lo_off), blksz);
	lr = zfs_rangelock_enter(&zp->z_rangelock, lo_off, lo_len, RL_WRITER);

	zfs_vmobject_wlock(object);
	if (len + off > object->un_pager.vnp.vnp_size) {
		if (object->un_pager.vnp.vnp_size > off) {
			int pgoff;

			len = object->un_pager.vnp.vnp_size - off;
			ncount = btoc(len);
			if ((pgoff = (int)len & PAGE_MASK) != 0) {
				/*
				 * If the object is locked and the following
				 * conditions hold, then the page's dirty
				 * field cannot be concurrently changed by a
				 * pmap operation.
				 */
				m = ma[ncount - 1];
				vm_page_assert_sbusied(m);
				KASSERT(!pmap_page_is_write_mapped(m),
				    ("zfs_putpages: page %p is not read-only",
				    m));
				vm_page_clear_dirty(m, pgoff, PAGE_SIZE -
				    pgoff);
			}
		} else {
			len = 0;
			ncount = 0;
		}
		if (ncount < pcount) {
			for (i = ncount; i < pcount; i++) {
				rtvals[i] = zfs_vm_pagerret_bad;
			}
		}
	}
	zfs_vmobject_wunlock(object);

	boolean_t commit = (flags & (zfs_vm_pagerput_sync |
	    zfs_vm_pagerput_inval)) != 0 ||
	    zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS;

	if (ncount == 0)
		goto out;

	if (zfs_id_overblockquota(zfsvfs, DMU_USERUSED_OBJECT, zp->z_uid) ||
	    zfs_id_overblockquota(zfsvfs, DMU_GROUPUSED_OBJECT, zp->z_gid) ||
	    (zp->z_projid != ZFS_DEFAULT_PROJID &&
	    zfs_id_overblockquota(zfsvfs, DMU_PROJECTUSED_OBJECT,
	    zp->z_projid))) {
		goto out;
	}

	tx = dmu_tx_create(zfsvfs->z_os);
	dmu_tx_hold_write(tx, zp->z_id, off, len);

	dmu_tx_hold_sa(tx, zp->z_sa_hdl, B_FALSE);
	zfs_sa_upgrade_txholds(tx, zp);
	err = dmu_tx_assign(tx, DMU_TX_WAIT);
	if (err != 0) {
		dmu_tx_abort(tx);
		goto out;
	}

	if (zp->z_blksz < PAGE_SIZE) {
		vm_ooffset_t woff = off;
		size_t wlen = len;
		for (i = 0; wlen > 0; woff += tocopy, wlen -= tocopy, i++) {
			tocopy = MIN(PAGE_SIZE, wlen);
			va = zfs_map_page(ma[i], &sf);
			dmu_write(zfsvfs->z_os, zp->z_id, woff, tocopy, va, tx);
			zfs_unmap_page(sf);
		}
	} else {
		err = dmu_write_pages(zfsvfs->z_os, zp->z_id, off, len, ma, tx);
	}

	if (err == 0) {
		uint64_t mtime[2], ctime[2];
		sa_bulk_attr_t bulk[3];
		int count = 0;

		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_MTIME(zfsvfs), NULL,
		    &mtime, 16);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_CTIME(zfsvfs), NULL,
		    &ctime, 16);
		SA_ADD_BULK_ATTR(bulk, count, SA_ZPL_FLAGS(zfsvfs), NULL,
		    &zp->z_pflags, 8);
		zfs_tstamp_update_setup(zp, CONTENT_MODIFIED, mtime, ctime);
		err = sa_bulk_update(zp->z_sa_hdl, bulk, count, tx);
		ASSERT0(err);

		if (commit) {
			/*
			 * Caller requested that we commit immediately. We set
			 * a callback on the log entry, to be called once its
			 * on disk after the call to zil_commit() below. The
			 * pages will be undirtied and unbusied there.
			 */
			putpage_commit_arg_t *pca = kmem_alloc(
			    offsetof(putpage_commit_arg_t, pca_pages[ncount]),
			    KM_SLEEP);
			pca->pca_npages = ncount;
			memcpy(pca->pca_pages, ma, sizeof (vm_page_t) * ncount);

			zfs_log_write(zfsvfs->z_log, tx, TX_WRITE, zp, off, len,
			    B_TRUE, B_FALSE, zfs_putpage_commit_cb, pca);

			for (i = 0; i < ncount; i++)
				rtvals[i] = zfs_vm_pagerret_pend;
		} else {
			/*
			 * Caller just wants the page written back somewhere,
			 * but doesn't need it committed yet. We've already
			 * written it back to the DMU, so we just need to put
			 * it on the async log, then undirty the page and
			 * return.
			 *
			 * We cannot use a callback here, because it would keep
			 * the page busy (locked) until it is eventually
			 * written down at txg sync.
			 */
			zfs_log_write(zfsvfs->z_log, tx, TX_WRITE, zp, off, len,
			    B_FALSE, B_FALSE, NULL, NULL);

			zfs_vmobject_wlock(object);
			for (i = 0; i < ncount; i++) {
				rtvals[i] = zfs_vm_pagerret_ok;
				vm_page_undirty(ma[i]);
			}
			zfs_vmobject_wunlock(object);
		}

		VM_CNT_INC(v_vnodeout);
		VM_CNT_ADD(v_vnodepgsout, ncount);
	}
	dmu_tx_commit(tx);

out:
	zfs_rangelock_exit(lr);
	if (commit)
		zil_commit(zfsvfs->z_log, zp->z_id);

	dataset_kstats_update_write_kstats(&zfsvfs->z_kstat, len);

	zfs_exit(zfsvfs, FTAG);
	return (rtvals[0]);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_putpages_args {
	struct vnode *a_vp;
	vm_page_t *a_m;
	int a_count;
	int a_sync;
	int *a_rtvals;
};
#endif

static int
zfs_freebsd_putpages(struct vop_putpages_args *ap)
{

	return (zfs_putpages(ap->a_vp, ap->a_m, ap->a_count, ap->a_sync,
	    ap->a_rtvals));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_bmap_args {
	struct vnode *a_vp;
	daddr_t  a_bn;
	struct bufobj **a_bop;
	daddr_t *a_bnp;
	int *a_runp;
	int *a_runb;
};
#endif

static int
zfs_freebsd_bmap(struct vop_bmap_args *ap)
{

	if (ap->a_bop != NULL)
		*ap->a_bop = &ap->a_vp->v_bufobj;
	if (ap->a_bnp != NULL)
		*ap->a_bnp = ap->a_bn;
	if (ap->a_runp != NULL)
		*ap->a_runp = 0;
	if (ap->a_runb != NULL)
		*ap->a_runb = 0;

	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_open_args {
	struct vnode *a_vp;
	int a_mode;
	struct ucred *a_cred;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_open(struct vop_open_args *ap)
{
	vnode_t	*vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	int error;

	error = zfs_open(&vp, ap->a_mode, ap->a_cred);
	if (error == 0)
		vnode_create_vobject(vp, zp->z_size, ap->a_td);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_close_args {
	struct vnode *a_vp;
	int  a_fflag;
	struct ucred *a_cred;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_close(struct vop_close_args *ap)
{

	return (zfs_close(ap->a_vp, ap->a_fflag, 1, 0, ap->a_cred));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_ioctl_args {
	struct vnode *a_vp;
	ulong_t a_command;
	caddr_t a_data;
	int a_fflag;
	struct ucred *cred;
	struct thread *td;
};
#endif

static int
zfs_freebsd_ioctl(struct vop_ioctl_args *ap)
{

	return (zfs_ioctl(ap->a_vp, ap->a_command, (intptr_t)ap->a_data,
	    ap->a_fflag, ap->a_cred, NULL));
}

static int
ioflags(int ioflags)
{
	int flags = 0;

	if (ioflags & IO_APPEND)
		flags |= O_APPEND;
	if (ioflags & IO_NDELAY)
		flags |= O_NONBLOCK;
	if (ioflags & IO_DIRECT)
		flags |= O_DIRECT;
	if (ioflags & IO_SYNC)
		flags |= O_SYNC;

	return (flags);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_read_args {
	struct vnode *a_vp;
	struct uio *a_uio;
	int a_ioflag;
	struct ucred *a_cred;
};
#endif

static int
zfs_freebsd_read(struct vop_read_args *ap)
{
	zfs_uio_t uio;
	int error = 0;
	zfs_uio_init(&uio, ap->a_uio);
	error = zfs_read(VTOZ(ap->a_vp), &uio, ioflags(ap->a_ioflag),
	    ap->a_cred);
	/*
	 * XXX We occasionally get an EFAULT for Direct I/O reads on
	 * FreeBSD 13. This still needs to be resolved. The EFAULT comes
	 * from:
	 * zfs_uio_get__dio_pages_alloc() ->
	 * zfs_uio_get_dio_pages_impl() ->
	 * zfs_uio_iov_step() ->
	 * zfs_uio_get_user_pages().
	 * We return EFAULT from zfs_uio_iov_step(). When a Direct I/O
	 * read fails to map in the user pages (returning EFAULT) the
	 * Direct I/O request is broken up into two separate IO requests
	 * and issued separately using Direct I/O.
	 */
#ifdef ZFS_DEBUG
	if (error == EFAULT && uio.uio_extflg & UIO_DIRECT) {
#if 0
		printf("%s(%d): Direct I/O read returning EFAULT "
		    "uio = %p, zfs_uio_offset(uio) = %lu "
		    "zfs_uio_resid(uio) = %lu\n",
		    __FUNCTION__, __LINE__, &uio, zfs_uio_offset(&uio),
		    zfs_uio_resid(&uio));
#endif
	}

#endif
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_write_args {
	struct vnode *a_vp;
	struct uio *a_uio;
	int a_ioflag;
	struct ucred *a_cred;
};
#endif

static int
zfs_freebsd_write(struct vop_write_args *ap)
{
	zfs_uio_t uio;
	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_write(VTOZ(ap->a_vp), &uio, ioflags(ap->a_ioflag),
	    ap->a_cred));
}

/*
 * VOP_FPLOOKUP_VEXEC routines are subject to special circumstances, see
 * the comment above cache_fplookup for details.
 */
static int
zfs_freebsd_fplookup_vexec(struct vop_fplookup_vexec_args *v)
{
	vnode_t *vp;
	znode_t *zp;
	uint64_t pflags;

	vp = v->a_vp;
	zp = VTOZ_SMR(vp);
	if (__predict_false(zp == NULL))
		return (EAGAIN);
	pflags = atomic_load_64(&zp->z_pflags);
	if (pflags & ZFS_AV_QUARANTINED)
		return (EAGAIN);
	if (pflags & ZFS_XATTR)
		return (EAGAIN);
	if ((pflags & ZFS_NO_EXECS_DENIED) == 0)
		return (EAGAIN);
	return (0);
}

static int
zfs_freebsd_fplookup_symlink(struct vop_fplookup_symlink_args *v)
{
	vnode_t *vp;
	znode_t *zp;
	char *target;

	vp = v->a_vp;
	zp = VTOZ_SMR(vp);
	if (__predict_false(zp == NULL)) {
		return (EAGAIN);
	}

	target = atomic_load_consume_ptr(&zp->z_cached_symlink);
	if (target == NULL) {
		return (EAGAIN);
	}
	return (cache_symlink_resolve(v->a_fpl, target, strlen(target)));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_access_args {
	struct vnode *a_vp;
	accmode_t a_accmode;
	struct ucred *a_cred;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_access(struct vop_access_args *ap)
{
	vnode_t *vp = ap->a_vp;
	znode_t *zp = VTOZ(vp);
	accmode_t accmode;
	int error = 0;


	if (ap->a_accmode == VEXEC) {
		if (zfs_fastaccesschk_execute(zp, ap->a_cred) == 0)
			return (0);
	}

	/*
	 * ZFS itself only knowns about VREAD, VWRITE, VEXEC and VAPPEND,
	 */
	accmode = ap->a_accmode & (VREAD|VWRITE|VEXEC|VAPPEND);
	if (accmode != 0) {
#if __FreeBSD_version >= 1500040
		/* For named attributes, do the checks. */
		if ((vn_irflag_read(vp) & VIRF_NAMEDATTR) != 0)
			error = zfs_access(zp, accmode, V_NAMEDATTR,
			    ap->a_cred);
		else
#endif
			error = zfs_access(zp, accmode, 0, ap->a_cred);
	}

	/*
	 * VADMIN has to be handled by vaccess().
	 */
	if (error == 0) {
		accmode = ap->a_accmode & ~(VREAD|VWRITE|VEXEC|VAPPEND);
		if (accmode != 0) {
			error = vaccess(vp->v_type, zp->z_mode, zp->z_uid,
			    zp->z_gid, accmode, ap->a_cred);
		}
	}

	/*
	 * For VEXEC, ensure that at least one execute bit is set for
	 * non-directories.
	 */
	if (error == 0 && (ap->a_accmode & VEXEC) != 0 && vp->v_type != VDIR &&
	    (zp->z_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0) {
		error = EACCES;
	}

	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_lookup_args {
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
};
#endif

#if __FreeBSD_version >= 1500040
static int
zfs_lookup_nameddir(struct vnode *dvp, struct componentname *cnp,
    struct vnode **vpp)
{
	struct vnode *xvp;
	int error, flags;

	*vpp = NULL;
	flags = LOOKUP_XATTR | LOOKUP_NAMED_ATTR;
	if ((cnp->cn_flags & CREATENAMED) != 0)
		flags |= CREATE_XATTR_DIR;
	error = zfs_lookup(dvp, NULL, &xvp, NULL, 0, cnp->cn_cred, flags,
	    B_FALSE);
	if (error == 0) {
		if ((cnp->cn_flags & LOCKLEAF) != 0)
			error = vn_lock(xvp, cnp->cn_lkflags);
		if (error == 0) {
			vn_irflag_set_cond(xvp, VIRF_NAMEDDIR);
			*vpp = xvp;
		} else {
			vrele(xvp);
		}
	}
	return (error);
}

static ssize_t
zfs_readdir_named(struct vnode *vp, char *buf, ssize_t blen, off_t *offp,
    int *eofflagp, struct ucred *cred, struct thread *td)
{
	struct uio io;
	struct iovec iv;
	zfs_uio_t uio;
	int error;

	io.uio_offset = *offp;
	io.uio_segflg = UIO_SYSSPACE;
	io.uio_rw = UIO_READ;
	io.uio_td = td;
	iv.iov_base = buf;
	iv.iov_len = blen;
	io.uio_iov = &iv;
	io.uio_iovcnt = 1;
	io.uio_resid = blen;
	zfs_uio_init(&uio, &io);
	error = zfs_readdir(vp, &uio, cred, eofflagp, NULL, NULL);
	if (error != 0)
		return (-1);
	*offp = io.uio_offset;
	return (blen - io.uio_resid);
}

static bool
zfs_has_namedattr(struct vnode *vp, struct ucred *cred)
{
	struct componentname cn;
	struct vnode *xvp;
	struct dirent *dp;
	off_t offs;
	ssize_t rsize;
	char *buf, *cp, *endcp;
	int eofflag, error;
	bool ret;

	MNT_ILOCK(vp->v_mount);
	if ((vp->v_mount->mnt_flag & MNT_NAMEDATTR) == 0) {
		MNT_IUNLOCK(vp->v_mount);
		return (false);
	}
	MNT_IUNLOCK(vp->v_mount);

	/* Now see if a named attribute directory exists. */
	cn.cn_flags = LOCKLEAF;
	cn.cn_lkflags = LK_SHARED;
	cn.cn_cred = cred;
	error = zfs_lookup_nameddir(vp, &cn, &xvp);
	if (error != 0)
		return (false);

	/* It exists, so see if there is any entry other than "." and "..". */
	buf = malloc(DEV_BSIZE, M_TEMP, M_WAITOK);
	ret = false;
	offs = 0;
	do {
		rsize = zfs_readdir_named(xvp, buf, DEV_BSIZE, &offs, &eofflag,
		    cred, curthread);
		if (rsize <= 0)
			break;
		cp = buf;
		endcp = &buf[rsize];
		while (cp < endcp) {
			dp = (struct dirent *)cp;
			if (dp->d_fileno != 0 && (dp->d_type == DT_REG ||
			    dp->d_type == DT_UNKNOWN) &&
			    !ZFS_XA_NS_PREFIX_FORBIDDEN(dp->d_name) &&
			    ((dp->d_namlen == 1 && dp->d_name[0] != '.') ||
			    (dp->d_namlen == 2 && (dp->d_name[0] != '.' ||
			    dp->d_name[1] != '.')) || dp->d_namlen > 2)) {
				ret = true;
				break;
			}
			cp += dp->d_reclen;
		}
	} while (!ret && rsize > 0 && eofflag == 0);
	vput(xvp);
	free(buf, M_TEMP);
	return (ret);
}

static int
zfs_freebsd_lookup(struct vop_lookup_args *ap, boolean_t cached)
{
	struct componentname *cnp = ap->a_cnp;
	char nm[NAME_MAX + 1];
	int error;
	struct vnode **vpp = ap->a_vpp, *dvp = ap->a_dvp, *xvp;
	bool is_nameddir, needs_nameddir, opennamed = false;

	/*
	 * These variables are used to handle the named attribute cases:
	 * opennamed - Is true when this is a call from open with O_NAMEDATTR
	 *    specified and it is the last component.
	 * is_nameddir - Is true when the directory is a named attribute dir.
	 * needs_nameddir - Is set when the lookup needs to look for/create
	 *    a named attribute directory.  It is only set when is_nameddir
	 *    is_nameddir is false and opennamed is true.
	 * xvp - Is the directory that the lookup needs to be done in.
	 *    Usually dvp, unless needs_nameddir is true where it is the
	 *    result of the first non-named directory lookup.
	 * Note that name caching must be disabled for named attribute
	 * handling.
	 */
	needs_nameddir = false;
	xvp = dvp;
	opennamed = (cnp->cn_flags & (OPENNAMED | ISLASTCN)) ==
	    (OPENNAMED | ISLASTCN);
	is_nameddir = (vn_irflag_read(dvp) & VIRF_NAMEDDIR) != 0;
	if (is_nameddir && (cnp->cn_flags & ISLASTCN) == 0)
		return (ENOATTR);
	if (opennamed && !is_nameddir && (cnp->cn_flags & ISDOTDOT) != 0)
		return (ENOATTR);
	if (opennamed || is_nameddir)
		cnp->cn_flags &= ~MAKEENTRY;
	if (opennamed && !is_nameddir)
		needs_nameddir = true;
	ASSERT3U(cnp->cn_namelen, <, sizeof (nm));
	error = 0;
	*vpp = NULL;
	if (needs_nameddir) {
		if (VOP_ISLOCKED(dvp) != LK_EXCLUSIVE)
			vn_lock(dvp, LK_UPGRADE | LK_RETRY);
		error = zfs_lookup_nameddir(dvp, cnp, &xvp);
		if (error == 0)
			is_nameddir = true;
	}
	if (error == 0) {
		if (!needs_nameddir || cnp->cn_namelen != 1 ||
		    *cnp->cn_nameptr != '.') {
			strlcpy(nm, cnp->cn_nameptr, MIN(cnp->cn_namelen + 1,
			    sizeof (nm)));
			error = zfs_lookup(xvp, nm, vpp, cnp, cnp->cn_nameiop,
			    cnp->cn_cred, 0, cached);
			if (is_nameddir && error == 0 &&
			    (cnp->cn_namelen != 1 || *cnp->cn_nameptr != '.') &&
			    (cnp->cn_flags & ISDOTDOT) == 0) {
				if ((*vpp)->v_type == VDIR)
					vn_irflag_set_cond(*vpp, VIRF_NAMEDDIR);
				else
					vn_irflag_set_cond(*vpp,
					    VIRF_NAMEDATTR);
			}
			if (needs_nameddir && xvp != *vpp)
				vput(xvp);
		} else {
			/*
			 * Lookup of "." when a named attribute dir is needed.
			 */
			*vpp = xvp;
		}
	}
	return (error);
}
#else
static int
zfs_freebsd_lookup(struct vop_lookup_args *ap, boolean_t cached)
{
	struct componentname *cnp = ap->a_cnp;
	char nm[NAME_MAX + 1];

	ASSERT3U(cnp->cn_namelen, <, sizeof (nm));
	strlcpy(nm, cnp->cn_nameptr, MIN(cnp->cn_namelen + 1, sizeof (nm)));

	return (zfs_lookup(ap->a_dvp, nm, ap->a_vpp, cnp, cnp->cn_nameiop,
	    cnp->cn_cred, 0, cached));
}
#endif

static int
zfs_freebsd_cachedlookup(struct vop_cachedlookup_args *ap)
{

	return (zfs_freebsd_lookup((struct vop_lookup_args *)ap, B_TRUE));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_lookup_args {
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
};
#endif

static int
zfs_cache_lookup(struct vop_lookup_args *ap)
{
	zfsvfs_t *zfsvfs;

	zfsvfs = ap->a_dvp->v_mount->mnt_data;
#if __FreeBSD_version >= 1500040
	if (zfsvfs->z_use_namecache && (ap->a_cnp->cn_flags & OPENNAMED) == 0)
#else
	if (zfsvfs->z_use_namecache)
#endif
		return (vfs_cache_lookup(ap));
	else
		return (zfs_freebsd_lookup(ap, B_FALSE));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_create_args {
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
	struct vattr *a_vap;
};
#endif

static int
zfs_freebsd_create(struct vop_create_args *ap)
{
	zfsvfs_t *zfsvfs;
	struct componentname *cnp = ap->a_cnp;
	vattr_t *vap = ap->a_vap;
	znode_t *zp = NULL;
	int rc, mode;
	struct vnode *dvp = ap->a_dvp;
#if __FreeBSD_version >= 1500040
	struct vnode *xvp;
	bool is_nameddir;
#endif

#if __FreeBSD_version < 1400068
	ASSERT(cnp->cn_flags & SAVENAME);
#endif

	vattr_init_mask(vap);
	mode = vap->va_mode & ALLPERMS;
	zfsvfs = ap->a_dvp->v_mount->mnt_data;
	*ap->a_vpp = NULL;

	rc = 0;
#if __FreeBSD_version >= 1500040
	xvp = NULL;
	is_nameddir = (vn_irflag_read(dvp) & VIRF_NAMEDDIR) != 0;
	if (!is_nameddir && (cnp->cn_flags & OPENNAMED) != 0) {
		/* Needs a named attribute directory. */
		rc = zfs_lookup_nameddir(dvp, cnp, &xvp);
		if (rc == 0) {
			dvp = xvp;
			is_nameddir = true;
		}
	}
	if (is_nameddir && rc == 0)
		rc = zfs_check_attrname(cnp->cn_nameptr);
#endif

	if (rc == 0)
		rc = zfs_create(VTOZ(dvp), cnp->cn_nameptr, vap, 0, mode,
		    &zp, cnp->cn_cred, 0 /* flag */, NULL /* vsecattr */, NULL);
#if __FreeBSD_version >= 1500040
	if (xvp != NULL)
		vput(xvp);
#endif
	if (rc == 0) {
		*ap->a_vpp = ZTOV(zp);
#if __FreeBSD_version >= 1500040
		if (is_nameddir)
			vn_irflag_set_cond(*ap->a_vpp, VIRF_NAMEDATTR);
#endif
	}
	if (zfsvfs->z_use_namecache &&
	    rc == 0 && (cnp->cn_flags & MAKEENTRY) != 0)
		cache_enter(ap->a_dvp, *ap->a_vpp, cnp);

	return (rc);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_remove_args {
	struct vnode *a_dvp;
	struct vnode *a_vp;
	struct componentname *a_cnp;
};
#endif

static int
zfs_freebsd_remove(struct vop_remove_args *ap)
{
	int error = 0;

#if __FreeBSD_version < 1400068
	ASSERT(ap->a_cnp->cn_flags & SAVENAME);
#endif

#if __FreeBSD_version >= 1500040
	if ((vn_irflag_read(ap->a_dvp) & VIRF_NAMEDDIR) != 0)
		error = zfs_check_attrname(ap->a_cnp->cn_nameptr);
#endif

	if (error == 0)
		error = zfs_remove_(ap->a_dvp, ap->a_vp, ap->a_cnp->cn_nameptr,
		    ap->a_cnp->cn_cred);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_mkdir_args {
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
	struct vattr *a_vap;
};
#endif

static int
zfs_freebsd_mkdir(struct vop_mkdir_args *ap)
{
	vattr_t *vap = ap->a_vap;
	znode_t *zp = NULL;
	int rc;

#if __FreeBSD_version < 1400068
	ASSERT(ap->a_cnp->cn_flags & SAVENAME);
#endif

	vattr_init_mask(vap);
	*ap->a_vpp = NULL;

	rc = zfs_mkdir(VTOZ(ap->a_dvp), ap->a_cnp->cn_nameptr, vap, &zp,
	    ap->a_cnp->cn_cred, 0, NULL, NULL);

	if (rc == 0)
		*ap->a_vpp = ZTOV(zp);
	return (rc);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_rmdir_args {
	struct vnode *a_dvp;
	struct vnode *a_vp;
	struct componentname *a_cnp;
};
#endif

static int
zfs_freebsd_rmdir(struct vop_rmdir_args *ap)
{
	struct componentname *cnp = ap->a_cnp;

#if __FreeBSD_version < 1400068
	ASSERT(cnp->cn_flags & SAVENAME);
#endif

	return (zfs_rmdir_(ap->a_dvp, ap->a_vp, cnp->cn_nameptr, cnp->cn_cred));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_readdir_args {
	struct vnode *a_vp;
	struct uio *a_uio;
	struct ucred *a_cred;
	int *a_eofflag;
	int *a_ncookies;
	cookie_t **a_cookies;
};
#endif

static int
zfs_freebsd_readdir(struct vop_readdir_args *ap)
{
	zfs_uio_t uio;
	zfs_uio_init(&uio, ap->a_uio);
	return (zfs_readdir(ap->a_vp, &uio, ap->a_cred, ap->a_eofflag,
	    ap->a_ncookies, ap->a_cookies));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_fsync_args {
	struct vnode *a_vp;
	int a_waitfor;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_fsync(struct vop_fsync_args *ap)
{

	return (zfs_fsync(VTOZ(ap->a_vp), 0, ap->a_td->td_ucred));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_getattr_args {
	struct vnode *a_vp;
	struct vattr *a_vap;
	struct ucred *a_cred;
};
#endif

static int
zfs_freebsd_getattr(struct vop_getattr_args *ap)
{
	vattr_t *vap = ap->a_vap;
	xvattr_t xvap;
	ulong_t fflags = 0;
	int error;

	xva_init(&xvap);
	xvap.xva_vattr = *vap;
	xvap.xva_vattr.va_mask |= AT_XVATTR;

	/* Convert chflags into ZFS-type flags. */
	/* XXX: what about SF_SETTABLE?. */
	XVA_SET_REQ(&xvap, XAT_IMMUTABLE);
	XVA_SET_REQ(&xvap, XAT_APPENDONLY);
	XVA_SET_REQ(&xvap, XAT_NOUNLINK);
	XVA_SET_REQ(&xvap, XAT_NODUMP);
	XVA_SET_REQ(&xvap, XAT_READONLY);
	XVA_SET_REQ(&xvap, XAT_ARCHIVE);
	XVA_SET_REQ(&xvap, XAT_SYSTEM);
	XVA_SET_REQ(&xvap, XAT_HIDDEN);
	XVA_SET_REQ(&xvap, XAT_REPARSE);
	XVA_SET_REQ(&xvap, XAT_OFFLINE);
	XVA_SET_REQ(&xvap, XAT_SPARSE);

	error = zfs_getattr(ap->a_vp, (vattr_t *)&xvap, 0, ap->a_cred);
	if (error != 0)
		return (error);

	/* Convert ZFS xattr into chflags. */
#define	FLAG_CHECK(fflag, xflag, xfield)	do {			\
	if (XVA_ISSET_RTN(&xvap, (xflag)) && (xfield) != 0)		\
		fflags |= (fflag);					\
} while (0)
	FLAG_CHECK(SF_IMMUTABLE, XAT_IMMUTABLE,
	    xvap.xva_xoptattrs.xoa_immutable);
	FLAG_CHECK(SF_APPEND, XAT_APPENDONLY,
	    xvap.xva_xoptattrs.xoa_appendonly);
	FLAG_CHECK(SF_NOUNLINK, XAT_NOUNLINK,
	    xvap.xva_xoptattrs.xoa_nounlink);
	FLAG_CHECK(UF_ARCHIVE, XAT_ARCHIVE,
	    xvap.xva_xoptattrs.xoa_archive);
	FLAG_CHECK(UF_NODUMP, XAT_NODUMP,
	    xvap.xva_xoptattrs.xoa_nodump);
	FLAG_CHECK(UF_READONLY, XAT_READONLY,
	    xvap.xva_xoptattrs.xoa_readonly);
	FLAG_CHECK(UF_SYSTEM, XAT_SYSTEM,
	    xvap.xva_xoptattrs.xoa_system);
	FLAG_CHECK(UF_HIDDEN, XAT_HIDDEN,
	    xvap.xva_xoptattrs.xoa_hidden);
	FLAG_CHECK(UF_REPARSE, XAT_REPARSE,
	    xvap.xva_xoptattrs.xoa_reparse);
	FLAG_CHECK(UF_OFFLINE, XAT_OFFLINE,
	    xvap.xva_xoptattrs.xoa_offline);
	FLAG_CHECK(UF_SPARSE, XAT_SPARSE,
	    xvap.xva_xoptattrs.xoa_sparse);

#undef	FLAG_CHECK
	*vap = xvap.xva_vattr;
	vap->va_flags = fflags;

#if __FreeBSD_version >= 1500040
	if ((vn_irflag_read(ap->a_vp) & (VIRF_NAMEDDIR | VIRF_NAMEDATTR)) != 0)
		vap->va_bsdflags |= SFBSD_NAMEDATTR;
#endif
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_setattr_args {
	struct vnode *a_vp;
	struct vattr *a_vap;
	struct ucred *a_cred;
};
#endif

static int
zfs_freebsd_setattr(struct vop_setattr_args *ap)
{
	vnode_t *vp = ap->a_vp;
	vattr_t *vap = ap->a_vap;
	cred_t *cred = ap->a_cred;
	xvattr_t xvap;
	ulong_t fflags;
	uint64_t zflags;

	vattr_init_mask(vap);
	vap->va_mask &= ~AT_NOSET;

	xva_init(&xvap);
	xvap.xva_vattr = *vap;

	zflags = VTOZ(vp)->z_pflags;

	if (vap->va_flags != VNOVAL) {
		zfsvfs_t *zfsvfs = VTOZ(vp)->z_zfsvfs;
		int error;

		if (zfsvfs->z_use_fuids == B_FALSE)
			return (EOPNOTSUPP);

		fflags = vap->va_flags;
		/*
		 * XXX KDM
		 * We need to figure out whether it makes sense to allow
		 * UF_REPARSE through, since we don't really have other
		 * facilities to handle reparse points and zfs_setattr()
		 * doesn't currently allow setting that attribute anyway.
		 */
		if ((fflags & ~(SF_IMMUTABLE|SF_APPEND|SF_NOUNLINK|UF_ARCHIVE|
		    UF_NODUMP|UF_SYSTEM|UF_HIDDEN|UF_READONLY|UF_REPARSE|
		    UF_OFFLINE|UF_SPARSE)) != 0)
			return (EOPNOTSUPP);
		/*
		 * Unprivileged processes are not permitted to unset system
		 * flags, or modify flags if any system flags are set.
		 * Privileged non-jail processes may not modify system flags
		 * if securelevel > 0 and any existing system flags are set.
		 * Privileged jail processes behave like privileged non-jail
		 * processes if the PR_ALLOW_CHFLAGS permission bit is set;
		 * otherwise, they behave like unprivileged processes.
		 */
		if (secpolicy_fs_owner(vp->v_mount, cred) == 0 ||
		    priv_check_cred(cred, PRIV_VFS_SYSFLAGS) == 0) {
			if (zflags &
			    (ZFS_IMMUTABLE | ZFS_APPENDONLY | ZFS_NOUNLINK)) {
				error = securelevel_gt(cred, 0);
				if (error != 0)
					return (error);
			}
		} else {
			/*
			 * Callers may only modify the file flags on
			 * objects they have VADMIN rights for.
			 */
			if ((error = VOP_ACCESS(vp, VADMIN, cred,
			    curthread)) != 0)
				return (error);
			if (zflags &
			    (ZFS_IMMUTABLE | ZFS_APPENDONLY |
			    ZFS_NOUNLINK)) {
				return (EPERM);
			}
			if (fflags &
			    (SF_IMMUTABLE | SF_APPEND | SF_NOUNLINK)) {
				return (EPERM);
			}
		}

#define	FLAG_CHANGE(fflag, zflag, xflag, xfield)	do {		\
	if (((fflags & (fflag)) && !(zflags & (zflag))) ||		\
	    ((zflags & (zflag)) && !(fflags & (fflag)))) {		\
		XVA_SET_REQ(&xvap, (xflag));				\
		(xfield) = ((fflags & (fflag)) != 0);			\
	}								\
} while (0)
		/* Convert chflags into ZFS-type flags. */
		/* XXX: what about SF_SETTABLE?. */
		FLAG_CHANGE(SF_IMMUTABLE, ZFS_IMMUTABLE, XAT_IMMUTABLE,
		    xvap.xva_xoptattrs.xoa_immutable);
		FLAG_CHANGE(SF_APPEND, ZFS_APPENDONLY, XAT_APPENDONLY,
		    xvap.xva_xoptattrs.xoa_appendonly);
		FLAG_CHANGE(SF_NOUNLINK, ZFS_NOUNLINK, XAT_NOUNLINK,
		    xvap.xva_xoptattrs.xoa_nounlink);
		FLAG_CHANGE(UF_ARCHIVE, ZFS_ARCHIVE, XAT_ARCHIVE,
		    xvap.xva_xoptattrs.xoa_archive);
		FLAG_CHANGE(UF_NODUMP, ZFS_NODUMP, XAT_NODUMP,
		    xvap.xva_xoptattrs.xoa_nodump);
		FLAG_CHANGE(UF_READONLY, ZFS_READONLY, XAT_READONLY,
		    xvap.xva_xoptattrs.xoa_readonly);
		FLAG_CHANGE(UF_SYSTEM, ZFS_SYSTEM, XAT_SYSTEM,
		    xvap.xva_xoptattrs.xoa_system);
		FLAG_CHANGE(UF_HIDDEN, ZFS_HIDDEN, XAT_HIDDEN,
		    xvap.xva_xoptattrs.xoa_hidden);
		FLAG_CHANGE(UF_REPARSE, ZFS_REPARSE, XAT_REPARSE,
		    xvap.xva_xoptattrs.xoa_reparse);
		FLAG_CHANGE(UF_OFFLINE, ZFS_OFFLINE, XAT_OFFLINE,
		    xvap.xva_xoptattrs.xoa_offline);
		FLAG_CHANGE(UF_SPARSE, ZFS_SPARSE, XAT_SPARSE,
		    xvap.xva_xoptattrs.xoa_sparse);
#undef	FLAG_CHANGE
	}
	if (vap->va_birthtime.tv_sec != VNOVAL) {
		xvap.xva_vattr.va_mask |= AT_XVATTR;
		XVA_SET_REQ(&xvap, XAT_CREATETIME);
	}
	return (zfs_setattr(VTOZ(vp), (vattr_t *)&xvap, 0, cred, NULL));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_rename_args {
	struct vnode *a_fdvp;
	struct vnode *a_fvp;
	struct componentname *a_fcnp;
	struct vnode *a_tdvp;
	struct vnode *a_tvp;
	struct componentname *a_tcnp;
};
#endif

static int
zfs_freebsd_rename(struct vop_rename_args *ap)
{
	vnode_t *fdvp = ap->a_fdvp;
	vnode_t *fvp = ap->a_fvp;
	vnode_t *tdvp = ap->a_tdvp;
	vnode_t *tvp = ap->a_tvp;
	int error = 0;

#if __FreeBSD_version < 1400068
	ASSERT(ap->a_fcnp->cn_flags & (SAVENAME|SAVESTART));
	ASSERT(ap->a_tcnp->cn_flags & (SAVENAME|SAVESTART));
#endif

#if __FreeBSD_version >= 1500040
	if ((vn_irflag_read(fdvp) & VIRF_NAMEDDIR) != 0) {
		error = zfs_check_attrname(ap->a_fcnp->cn_nameptr);
		if (error == 0)
			error = zfs_check_attrname(ap->a_tcnp->cn_nameptr);
	}
#endif

	if (error == 0)
		error = zfs_do_rename(fdvp, &fvp, ap->a_fcnp, tdvp, &tvp,
		    ap->a_tcnp, ap->a_fcnp->cn_cred);

	vrele(fdvp);
	vrele(fvp);
	vrele(tdvp);
	if (tvp != NULL)
		vrele(tvp);

	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_symlink_args {
	struct vnode *a_dvp;
	struct vnode **a_vpp;
	struct componentname *a_cnp;
	struct vattr *a_vap;
	char *a_target;
};
#endif

static int
zfs_freebsd_symlink(struct vop_symlink_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	vattr_t *vap = ap->a_vap;
	znode_t *zp = NULL;
	char *symlink;
	size_t symlink_len;
	int rc;

#if __FreeBSD_version < 1400068
	ASSERT(cnp->cn_flags & SAVENAME);
#endif

	vap->va_type = VLNK;	/* FreeBSD: Syscall only sets va_mode. */
	vattr_init_mask(vap);
	*ap->a_vpp = NULL;

	rc = zfs_symlink(VTOZ(ap->a_dvp), cnp->cn_nameptr, vap,
	    ap->a_target, &zp, cnp->cn_cred, 0 /* flags */, NULL);
	if (rc == 0) {
		*ap->a_vpp = ZTOV(zp);
		ASSERT_VOP_ELOCKED(ZTOV(zp), __func__);
		MPASS(zp->z_cached_symlink == NULL);
		symlink_len = strlen(ap->a_target);
		symlink = cache_symlink_alloc(symlink_len + 1, M_WAITOK);
		if (symlink != NULL) {
			memcpy(symlink, ap->a_target, symlink_len);
			symlink[symlink_len] = '\0';
			atomic_store_rel_ptr((uintptr_t *)&zp->z_cached_symlink,
			    (uintptr_t)symlink);
		}
	}
	return (rc);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_readlink_args {
	struct vnode *a_vp;
	struct uio *a_uio;
	struct ucred *a_cred;
};
#endif

static int
zfs_freebsd_readlink(struct vop_readlink_args *ap)
{
	zfs_uio_t uio;
	int error;
	znode_t	*zp = VTOZ(ap->a_vp);
	char *symlink, *base;
	size_t symlink_len;
	bool trycache;

	zfs_uio_init(&uio, ap->a_uio);
	trycache = false;
	if (zfs_uio_segflg(&uio) == UIO_SYSSPACE &&
	    zfs_uio_iovcnt(&uio) == 1) {
		base = zfs_uio_iovbase(&uio, 0);
		symlink_len = zfs_uio_iovlen(&uio, 0);
		trycache = true;
	}
	error = zfs_readlink(ap->a_vp, &uio, ap->a_cred, NULL);
	if (atomic_load_ptr(&zp->z_cached_symlink) != NULL ||
	    error != 0 || !trycache) {
		return (error);
	}
	symlink_len -= zfs_uio_resid(&uio);
	symlink = cache_symlink_alloc(symlink_len + 1, M_WAITOK);
	if (symlink != NULL) {
		memcpy(symlink, base, symlink_len);
		symlink[symlink_len] = '\0';
		if (!atomic_cmpset_rel_ptr((uintptr_t *)&zp->z_cached_symlink,
		    (uintptr_t)NULL, (uintptr_t)symlink)) {
			cache_symlink_free(symlink, symlink_len + 1);
		}
	}
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_link_args {
	struct vnode *a_tdvp;
	struct vnode *a_vp;
	struct componentname *a_cnp;
};
#endif

static int
zfs_freebsd_link(struct vop_link_args *ap)
{
	struct componentname *cnp = ap->a_cnp;
	vnode_t *vp = ap->a_vp;
	vnode_t *tdvp = ap->a_tdvp;

	if (tdvp->v_mount != vp->v_mount)
		return (EXDEV);

#if __FreeBSD_version < 1400068
	ASSERT(cnp->cn_flags & SAVENAME);
#endif

	return (zfs_link(VTOZ(tdvp), VTOZ(vp),
	    cnp->cn_nameptr, cnp->cn_cred, 0));
}

#ifndef _SYS_SYSPROTO_H_
struct vop_inactive_args {
	struct vnode *a_vp;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_inactive(struct vop_inactive_args *ap)
{
	vnode_t *vp = ap->a_vp;

	zfs_inactive(vp, curthread->td_ucred, NULL);
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_need_inactive_args {
	struct vnode *a_vp;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_need_inactive(struct vop_need_inactive_args *ap)
{
	vnode_t *vp = ap->a_vp;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	int need;

	if (vn_need_pageq_flush(vp))
		return (1);

	if (!ZFS_TEARDOWN_INACTIVE_TRY_ENTER_READ(zfsvfs))
		return (1);
	need = (zp->z_sa_hdl == NULL || zp->z_unlinked || zp->z_atime_dirty);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);

	return (need);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_reclaim_args {
	struct vnode *a_vp;
	struct thread *a_td;
};
#endif

static int
zfs_freebsd_reclaim(struct vop_reclaim_args *ap)
{
	vnode_t	*vp = ap->a_vp;
	znode_t	*zp = VTOZ(vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	ASSERT3P(zp, !=, NULL);

	/*
	 * z_teardown_inactive_lock protects from a race with
	 * zfs_znode_dmu_fini in zfsvfs_teardown during
	 * force unmount.
	 */
	ZFS_TEARDOWN_INACTIVE_ENTER_READ(zfsvfs);
	if (zp->z_sa_hdl == NULL)
		zfs_znode_free(zp);
	else
		zfs_zinactive(zp);
	ZFS_TEARDOWN_INACTIVE_EXIT_READ(zfsvfs);

	vp->v_data = NULL;
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_fid_args {
	struct vnode *a_vp;
	struct fid *a_fid;
};
#endif

static int
zfs_freebsd_fid(struct vop_fid_args *ap)
{

	return (zfs_fid(ap->a_vp, (void *)ap->a_fid, NULL));
}


#ifndef _SYS_SYSPROTO_H_
struct vop_pathconf_args {
	struct vnode *a_vp;
	int a_name;
	register_t *a_retval;
} *ap;
#endif

static int
zfs_freebsd_pathconf(struct vop_pathconf_args *ap)
{
	ulong_t val;
	int error;

	error = zfs_pathconf(ap->a_vp, ap->a_name, &val,
	    curthread->td_ucred, NULL);
	if (error == 0) {
		*ap->a_retval = val;
		return (error);
	}
	if (error != EOPNOTSUPP)
		return (error);

	switch (ap->a_name) {
	case _PC_NAME_MAX:
		*ap->a_retval = NAME_MAX;
		return (0);
#if __FreeBSD_version >= 1400032
	case _PC_DEALLOC_PRESENT:
		*ap->a_retval = 1;
		return (0);
#endif
	case _PC_PIPE_BUF:
		if (ap->a_vp->v_type == VDIR || ap->a_vp->v_type == VFIFO) {
			*ap->a_retval = PIPE_BUF;
			return (0);
		}
		return (EINVAL);
#if __FreeBSD_version >= 1500040
	case _PC_NAMEDATTR_ENABLED:
		MNT_ILOCK(ap->a_vp->v_mount);
		if ((ap->a_vp->v_mount->mnt_flag & MNT_NAMEDATTR) != 0)
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		MNT_IUNLOCK(ap->a_vp->v_mount);
		return (0);
	case _PC_HAS_NAMEDATTR:
		if (zfs_has_namedattr(ap->a_vp, curthread->td_ucred))
			*ap->a_retval = 1;
		else
			*ap->a_retval = 0;
		return (0);
#endif
#ifdef _PC_HAS_HIDDENSYSTEM
	case _PC_HAS_HIDDENSYSTEM:
		*ap->a_retval = 1;
		return (0);
#endif
	default:
		return (vop_stdpathconf(ap));
	}
}

int zfs_xattr_compat = 1;

static int
zfs_check_attrname(const char *name)
{
	/* We don't allow '/' character in attribute name. */
	if (strchr(name, '/') != NULL)
		return (SET_ERROR(EINVAL));
	/* We don't allow attribute names that start with a namespace prefix. */
	if (ZFS_XA_NS_PREFIX_FORBIDDEN(name))
		return (SET_ERROR(EINVAL));
	return (0);
}

/*
 * FreeBSD's extended attributes namespace defines file name prefix for ZFS'
 * extended attribute name:
 *
 *	NAMESPACE	XATTR_COMPAT	PREFIX
 *	system		*		freebsd:system:
 *	user		1		(none, can be used to access ZFS
 *					fsattr(5) attributes created on Solaris)
 *	user		0		user.
 */
static int
zfs_create_attrname(int attrnamespace, const char *name, char *attrname,
    size_t size, boolean_t compat)
{
	const char *namespace, *prefix, *suffix;

	memset(attrname, 0, size);

	switch (attrnamespace) {
	case EXTATTR_NAMESPACE_USER:
		if (compat) {
			/*
			 * This is the default namespace by which we can access
			 * all attributes created on Solaris.
			 */
			prefix = namespace = suffix = "";
		} else {
			/*
			 * This is compatible with the user namespace encoding
			 * on Linux prior to xattr_compat, but nothing
			 * else.
			 */
			prefix = "";
			namespace = "user";
			suffix = ".";
		}
		break;
	case EXTATTR_NAMESPACE_SYSTEM:
		prefix = "freebsd:";
		namespace = EXTATTR_NAMESPACE_SYSTEM_STRING;
		suffix = ":";
		break;
	case EXTATTR_NAMESPACE_EMPTY:
	default:
		return (SET_ERROR(EINVAL));
	}
	if (snprintf(attrname, size, "%s%s%s%s", prefix, namespace, suffix,
	    name) >= size) {
		return (SET_ERROR(ENAMETOOLONG));
	}
	return (0);
}

static int
zfs_ensure_xattr_cached(znode_t *zp)
{
	int error = 0;

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));

	if (zp->z_xattr_cached != NULL)
		return (0);

	if (rw_write_held(&zp->z_xattr_lock))
		return (zfs_sa_get_xattr(zp));

	if (!rw_tryupgrade(&zp->z_xattr_lock)) {
		rw_exit(&zp->z_xattr_lock);
		rw_enter(&zp->z_xattr_lock, RW_WRITER);
	}
	if (zp->z_xattr_cached == NULL)
		error = zfs_sa_get_xattr(zp);
	rw_downgrade(&zp->z_xattr_lock);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_getextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	INOUT struct uio *a_uio;
	OUT size_t *a_size;
	IN struct ucred *a_cred;
	IN struct thread *a_td;
};
#endif

static int
zfs_getextattr_dir(struct vop_getextattr_args *ap, const char *attrname)
{
	struct thread *td = ap->a_td;
	struct nameidata nd;
	struct vattr va;
	vnode_t *xvp = NULL, *vp;
	int error, flags;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR, B_FALSE);
	if (error != 0)
		return (error);

	flags = FREAD;
#if __FreeBSD_version < 1400043
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, attrname,
	    xvp, td);
#else
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, attrname, xvp);
#endif
	error = vn_open_cred(&nd, &flags, 0, VN_OPEN_INVFS, ap->a_cred, NULL);
	if (error != 0)
		return (SET_ERROR(error));
	vp = nd.ni_vp;
	NDFREE_PNBUF(&nd);

	if (ap->a_size != NULL) {
		error = VOP_GETATTR(vp, &va, ap->a_cred);
		if (error == 0)
			*ap->a_size = (size_t)va.va_size;
	} else if (ap->a_uio != NULL)
		error = VOP_READ(vp, ap->a_uio, IO_UNIT, ap->a_cred);

	VOP_UNLOCK(vp);
	vn_close(vp, flags, ap->a_cred, td);
	return (error);
}

static int
zfs_getextattr_sa(struct vop_getextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	uchar_t *nv_value;
	uint_t nv_size;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	error = nvlist_lookup_byte_array(zp->z_xattr_cached, attrname,
	    &nv_value, &nv_size);
	if (error != 0)
		return (SET_ERROR(error));

	if (ap->a_size != NULL)
		*ap->a_size = nv_size;
	else if (ap->a_uio != NULL)
		error = uiomove(nv_value, nv_size, ap->a_uio);
	if (error != 0)
		return (SET_ERROR(error));

	return (0);
}

static int
zfs_getextattr_impl(struct vop_getextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_getextattr_sa(ap, attrname);
	if (error == ENOENT)
		error = zfs_getextattr_dir(ap, attrname);
	return (error);
}

/*
 * Vnode operation to retrieve a named extended attribute.
 */
static int
zfs_getextattr(struct vop_getextattr_args *ap)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, ap->a_td, VREAD);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	error = ENOENT;
	rw_enter(&zp->z_xattr_lock, RW_READER);

	error = zfs_getextattr_impl(ap, zfs_xattr_compat);
	if ((error == ENOENT || error == ENOATTR) &&
	    ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Fall back to the alternate namespace format if we failed to
		 * find a user xattr.
		 */
		error = zfs_getextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	if (error == ENOENT)
		error = SET_ERROR(ENOATTR);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_deleteextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	IN struct ucred *a_cred;
	IN struct thread *a_td;
};
#endif

static int
zfs_deleteextattr_dir(struct vop_deleteextattr_args *ap, const char *attrname)
{
	struct nameidata nd;
	vnode_t *xvp = NULL, *vp;
	int error;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR, B_FALSE);
	if (error != 0)
		return (error);

#if __FreeBSD_version < 1400043
	NDINIT_ATVP(&nd, DELETE, NOFOLLOW | LOCKPARENT | LOCKLEAF,
	    UIO_SYSSPACE, attrname, xvp, ap->a_td);
#else
	NDINIT_ATVP(&nd, DELETE, NOFOLLOW | LOCKPARENT | LOCKLEAF,
	    UIO_SYSSPACE, attrname, xvp);
#endif
	error = namei(&nd);
	if (error != 0)
		return (SET_ERROR(error));

	vp = nd.ni_vp;
	error = VOP_REMOVE(nd.ni_dvp, vp, &nd.ni_cnd);
	NDFREE_PNBUF(&nd);

	vput(nd.ni_dvp);
	if (vp == nd.ni_dvp)
		vrele(vp);
	else
		vput(vp);

	return (error);
}

static int
zfs_deleteextattr_sa(struct vop_deleteextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	nvlist_t *nvl;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_WRITE_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	nvl = zp->z_xattr_cached;
	error = nvlist_remove(nvl, attrname, DATA_TYPE_BYTE_ARRAY);
	if (error != 0)
		error = SET_ERROR(error);
	else
		error = zfs_sa_set_xattr(zp, attrname, NULL, 0);
	if (error != 0) {
		zp->z_xattr_cached = NULL;
		nvlist_free(nvl);
	}
	return (error);
}

static int
zfs_deleteextattr_impl(struct vop_deleteextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_deleteextattr_sa(ap, attrname);
	if (error == ENOENT)
		error = zfs_deleteextattr_dir(ap, attrname);
	return (error);
}

/*
 * Vnode operation to remove a named attribute.
 */
static int
zfs_deleteextattr(struct vop_deleteextattr_args *ap)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, ap->a_td, VWRITE);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_WRITER);

	error = zfs_deleteextattr_impl(ap, zfs_xattr_compat);
	if ((error == ENOENT || error == ENOATTR) &&
	    ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Fall back to the alternate namespace format if we failed to
		 * find a user xattr.
		 */
		error = zfs_deleteextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	if (error == ENOENT)
		error = SET_ERROR(ENOATTR);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_setextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	IN const char *a_name;
	INOUT struct uio *a_uio;
	IN struct ucred *a_cred;
	IN struct thread *a_td;
};
#endif

static int
zfs_setextattr_dir(struct vop_setextattr_args *ap, const char *attrname)
{
	struct thread *td = ap->a_td;
	struct nameidata nd;
	struct vattr va;
	vnode_t *xvp = NULL, *vp;
	int error, flags;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR | CREATE_XATTR_DIR, B_FALSE);
	if (error != 0)
		return (error);

	flags = FFLAGS(O_WRONLY | O_CREAT);
#if __FreeBSD_version < 1400043
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, attrname, xvp, td);
#else
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW, UIO_SYSSPACE, attrname, xvp);
#endif
	error = vn_open_cred(&nd, &flags, 0600, VN_OPEN_INVFS, ap->a_cred,
	    NULL);
	if (error != 0)
		return (SET_ERROR(error));
	vp = nd.ni_vp;
	NDFREE_PNBUF(&nd);

	VATTR_NULL(&va);
	va.va_size = 0;
	error = VOP_SETATTR(vp, &va, ap->a_cred);
	if (error == 0)
		VOP_WRITE(vp, ap->a_uio, IO_UNIT, ap->a_cred);

	VOP_UNLOCK(vp);
	vn_close(vp, flags, ap->a_cred, td);
	return (error);
}

static int
zfs_setextattr_sa(struct vop_setextattr_args *ap, const char *attrname)
{
	znode_t *zp = VTOZ(ap->a_vp);
	nvlist_t *nvl;
	size_t sa_size;
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_WRITE_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	nvl = zp->z_xattr_cached;
	size_t entry_size = ap->a_uio->uio_resid;
	if (entry_size > DXATTR_MAX_ENTRY_SIZE)
		return (SET_ERROR(EFBIG));
	error = nvlist_size(nvl, &sa_size, NV_ENCODE_XDR);
	if (error != 0)
		return (SET_ERROR(error));
	if (sa_size > DXATTR_MAX_SA_SIZE)
		return (SET_ERROR(EFBIG));
	uchar_t *buf = kmem_alloc(entry_size, KM_SLEEP);
	error = uiomove(buf, entry_size, ap->a_uio);
	if (error != 0) {
		error = SET_ERROR(error);
	} else {
		error = nvlist_add_byte_array(nvl, attrname, buf, entry_size);
		if (error != 0)
			error = SET_ERROR(error);
	}
	if (error == 0)
		error = zfs_sa_set_xattr(zp, attrname, buf, entry_size);
	kmem_free(buf, entry_size);
	if (error != 0) {
		zp->z_xattr_cached = NULL;
		nvlist_free(nvl);
	}
	return (error);
}

static int
zfs_setextattr_impl(struct vop_setextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrname[EXTATTR_MAXNAMELEN+1];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, ap->a_name, attrname,
	    sizeof (attrname), compat);
	if (error != 0)
		return (error);

	struct vop_deleteextattr_args vda = {
		.a_vp = ap->a_vp,
		.a_attrnamespace = ap->a_attrnamespace,
		.a_name = ap->a_name,
		.a_cred = ap->a_cred,
		.a_td = ap->a_td,
	};
	error = ENOENT;
	if (zfsvfs->z_use_sa && zp->z_is_sa && zfsvfs->z_xattr_sa) {
		error = zfs_setextattr_sa(ap, attrname);
		if (error == 0) {
			/*
			 * Successfully put into SA, we need to clear the one
			 * in dir if present.
			 */
			zfs_deleteextattr_dir(&vda, attrname);
		}
	}
	if (error != 0) {
		error = zfs_setextattr_dir(ap, attrname);
		if (error == 0 && zp->z_is_sa) {
			/*
			 * Successfully put into dir, we need to clear the one
			 * in SA if present.
			 */
			zfs_deleteextattr_sa(&vda, attrname);
		}
	}
	if (error == 0 && ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/*
		 * Also clear all versions of the alternate compat name.
		 */
		zfs_deleteextattr_impl(&vda, !compat);
	}
	return (error);
}

/*
 * Vnode operation to set a named attribute.
 */
static int
zfs_setextattr(struct vop_setextattr_args *ap)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, ap->a_td, VWRITE);
	if (error != 0)
		return (SET_ERROR(error));

	error = zfs_check_attrname(ap->a_name);
	if (error != 0)
		return (error);

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_WRITER);

	error = zfs_setextattr_impl(ap, zfs_xattr_compat);

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_listextattr {
	IN struct vnode *a_vp;
	IN int a_attrnamespace;
	INOUT struct uio *a_uio;
	OUT size_t *a_size;
	IN struct ucred *a_cred;
	IN struct thread *a_td;
};
#endif

static int
zfs_listextattr_dir(struct vop_listextattr_args *ap, const char *attrprefix)
{
	struct thread *td = ap->a_td;
	struct nameidata nd;
	uint8_t dirbuf[sizeof (struct dirent)];
	struct iovec aiov;
	struct uio auio;
	vnode_t *xvp = NULL, *vp;
	int error, eof;

	error = zfs_lookup(ap->a_vp, NULL, &xvp, NULL, 0, ap->a_cred,
	    LOOKUP_XATTR, B_FALSE);
	if (error != 0) {
		/*
		 * ENOATTR means that the EA directory does not yet exist,
		 * i.e. there are no extended attributes there.
		 */
		if (error == ENOATTR)
			error = 0;
		return (error);
	}

#if __FreeBSD_version < 1400043
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW | LOCKLEAF | LOCKSHARED,
	    UIO_SYSSPACE, ".", xvp, td);
#else
	NDINIT_ATVP(&nd, LOOKUP, NOFOLLOW | LOCKLEAF | LOCKSHARED,
	    UIO_SYSSPACE, ".", xvp);
#endif
	error = namei(&nd);
	if (error != 0)
		return (SET_ERROR(error));
	vp = nd.ni_vp;
	NDFREE_PNBUF(&nd);

	auio.uio_iov = &aiov;
	auio.uio_iovcnt = 1;
	auio.uio_segflg = UIO_SYSSPACE;
	auio.uio_td = td;
	auio.uio_rw = UIO_READ;
	auio.uio_offset = 0;

	size_t plen = strlen(attrprefix);

	do {
		aiov.iov_base = (void *)dirbuf;
		aiov.iov_len = sizeof (dirbuf);
		auio.uio_resid = sizeof (dirbuf);
		error = VOP_READDIR(vp, &auio, ap->a_cred, &eof, NULL, NULL);
		if (error != 0)
			break;
		int done = sizeof (dirbuf) - auio.uio_resid;
		for (int pos = 0; pos < done; ) {
			struct dirent *dp = (struct dirent *)(dirbuf + pos);
			pos += dp->d_reclen;
			/*
			 * XXX: Temporarily we also accept DT_UNKNOWN, as this
			 * is what we get when attribute was created on Solaris.
			 */
			if (dp->d_type != DT_REG && dp->d_type != DT_UNKNOWN)
				continue;
			else if (plen == 0 &&
			    ZFS_XA_NS_PREFIX_FORBIDDEN(dp->d_name))
				continue;
			else if (strncmp(dp->d_name, attrprefix, plen) != 0)
				continue;
			uint8_t nlen = dp->d_namlen - plen;
			if (ap->a_size != NULL) {
				*ap->a_size += 1 + nlen;
			} else if (ap->a_uio != NULL) {
				/*
				 * Format of extattr name entry is one byte for
				 * length and the rest for name.
				 */
				error = uiomove(&nlen, 1, ap->a_uio);
				if (error == 0) {
					char *namep = dp->d_name + plen;
					error = uiomove(namep, nlen, ap->a_uio);
				}
				if (error != 0) {
					error = SET_ERROR(error);
					break;
				}
			}
		}
	} while (!eof && error == 0);

	vput(vp);
	return (error);
}

static int
zfs_listextattr_sa(struct vop_listextattr_args *ap, const char *attrprefix)
{
	znode_t *zp = VTOZ(ap->a_vp);
	int error;

	error = zfs_ensure_xattr_cached(zp);
	if (error != 0)
		return (error);

	ASSERT(RW_LOCK_HELD(&zp->z_xattr_lock));
	ASSERT3P(zp->z_xattr_cached, !=, NULL);

	size_t plen = strlen(attrprefix);
	nvpair_t *nvp = NULL;
	while ((nvp = nvlist_next_nvpair(zp->z_xattr_cached, nvp)) != NULL) {
		ASSERT3U(nvpair_type(nvp), ==, DATA_TYPE_BYTE_ARRAY);

		const char *name = nvpair_name(nvp);
		if (plen == 0 && ZFS_XA_NS_PREFIX_FORBIDDEN(name))
			continue;
		else if (strncmp(name, attrprefix, plen) != 0)
			continue;
		uint8_t nlen = strlen(name) - plen;
		if (ap->a_size != NULL) {
			*ap->a_size += 1 + nlen;
		} else if (ap->a_uio != NULL) {
			/*
			 * Format of extattr name entry is one byte for
			 * length and the rest for name.
			 */
			error = uiomove(&nlen, 1, ap->a_uio);
			if (error == 0) {
				char *namep = __DECONST(char *, name) + plen;
				error = uiomove(namep, nlen, ap->a_uio);
			}
			if (error != 0) {
				error = SET_ERROR(error);
				break;
			}
		}
	}

	return (error);
}

static int
zfs_listextattr_impl(struct vop_listextattr_args *ap, boolean_t compat)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	char attrprefix[16];
	int error;

	error = zfs_create_attrname(ap->a_attrnamespace, "", attrprefix,
	    sizeof (attrprefix), compat);
	if (error != 0)
		return (error);

	if (zfsvfs->z_use_sa && zp->z_is_sa)
		error = zfs_listextattr_sa(ap, attrprefix);
	if (error == 0)
		error = zfs_listextattr_dir(ap, attrprefix);
	return (error);
}

/*
 * Vnode operation to retrieve extended attributes on a vnode.
 */
static int
zfs_listextattr(struct vop_listextattr_args *ap)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = ZTOZSB(zp);
	int error;

	if (ap->a_size != NULL)
		*ap->a_size = 0;

	/*
	 * If the xattr property is off, refuse the request.
	 */
	if (!(zfsvfs->z_flags & ZSB_XATTR))
		return (SET_ERROR(EOPNOTSUPP));

	error = extattr_check_cred(ap->a_vp, ap->a_attrnamespace,
	    ap->a_cred, ap->a_td, VREAD);
	if (error != 0)
		return (SET_ERROR(error));

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);
	rw_enter(&zp->z_xattr_lock, RW_READER);

	error = zfs_listextattr_impl(ap, zfs_xattr_compat);
	if (error == 0 && ap->a_attrnamespace == EXTATTR_NAMESPACE_USER) {
		/* Also list user xattrs with the alternate format. */
		error = zfs_listextattr_impl(ap, !zfs_xattr_compat);
	}

	rw_exit(&zp->z_xattr_lock);
	zfs_exit(zfsvfs, FTAG);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_getacl_args {
	struct vnode *vp;
	acl_type_t type;
	struct acl *aclp;
	struct ucred *cred;
	struct thread *td;
};
#endif

static int
zfs_freebsd_getacl(struct vop_getacl_args *ap)
{
	int		error;
	vsecattr_t	vsecattr;

	if (ap->a_type != ACL_TYPE_NFS4)
		return (EINVAL);

	vsecattr.vsa_mask = VSA_ACE | VSA_ACECNT;
	if ((error = zfs_getsecattr(VTOZ(ap->a_vp),
	    &vsecattr, 0, ap->a_cred)))
		return (error);

	error = acl_from_aces(ap->a_aclp, vsecattr.vsa_aclentp,
	    vsecattr.vsa_aclcnt);
	if (vsecattr.vsa_aclentp != NULL)
		kmem_free(vsecattr.vsa_aclentp, vsecattr.vsa_aclentsz);

	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_setacl_args {
	struct vnode *vp;
	acl_type_t type;
	struct acl *aclp;
	struct ucred *cred;
	struct thread *td;
};
#endif

static int
zfs_freebsd_setacl(struct vop_setacl_args *ap)
{
	int		error;
	vsecattr_t vsecattr;
	int		aclbsize;	/* size of acl list in bytes */
	aclent_t	*aaclp;

	if (ap->a_type != ACL_TYPE_NFS4)
		return (EINVAL);

	if (ap->a_aclp == NULL)
		return (EINVAL);

	if (ap->a_aclp->acl_cnt < 1 || ap->a_aclp->acl_cnt > MAX_ACL_ENTRIES)
		return (EINVAL);

	/*
	 * With NFSv4 ACLs, chmod(2) may need to add additional entries,
	 * splitting every entry into two and appending "canonical six"
	 * entries at the end.  Don't allow for setting an ACL that would
	 * cause chmod(2) to run out of ACL entries.
	 */
	if (ap->a_aclp->acl_cnt * 2 + 6 > ACL_MAX_ENTRIES)
		return (ENOSPC);

	error = acl_nfs4_check(ap->a_aclp, ap->a_vp->v_type == VDIR);
	if (error != 0)
		return (error);

	vsecattr.vsa_mask = VSA_ACE;
	aclbsize = ap->a_aclp->acl_cnt * sizeof (ace_t);
	vsecattr.vsa_aclentp = kmem_alloc(aclbsize, KM_SLEEP);
	aaclp = vsecattr.vsa_aclentp;
	vsecattr.vsa_aclentsz = aclbsize;

	aces_from_acl(vsecattr.vsa_aclentp, &vsecattr.vsa_aclcnt, ap->a_aclp);
	error = zfs_setsecattr(VTOZ(ap->a_vp), &vsecattr, 0, ap->a_cred);
	kmem_free(aaclp, aclbsize);

	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_aclcheck_args {
	struct vnode *vp;
	acl_type_t type;
	struct acl *aclp;
	struct ucred *cred;
	struct thread *td;
};
#endif

static int
zfs_freebsd_aclcheck(struct vop_aclcheck_args *ap)
{

	return (EOPNOTSUPP);
}

#ifndef _SYS_SYSPROTO_H_
struct vop_advise_args {
	struct vnode *a_vp;
	off_t a_start;
	off_t a_end;
	int a_advice;
};
#endif

static int
zfs_freebsd_advise(struct vop_advise_args *ap)
{
	vnode_t *vp = ap->a_vp;
	off_t start = ap->a_start;
	off_t end = ap->a_end;
	int advice = ap->a_advice;
	off_t len;
	znode_t *zp;
	zfsvfs_t *zfsvfs;
	objset_t *os;
	int error = 0;

	if (end < start)
		return (EINVAL);

	error = vn_lock(vp, LK_SHARED);
	if (error)
		return (error);

	zp = VTOZ(vp);
	zfsvfs = zp->z_zfsvfs;
	os = zp->z_zfsvfs->z_os;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		goto out_unlock;

	/* kern_posix_fadvise points to the last byte, we want one past */
	if (end != OFF_MAX)
		end += 1;
	len = end - start;

	switch (advice) {
	case POSIX_FADV_WILLNEED:
		/*
		 * Pass on the caller's size directly, but note that
		 * dmu_prefetch_max will effectively cap it.  If there really
		 * is a larger sequential access pattern, perhaps dmu_zfetch
		 * will detect it.
		 */
		dmu_prefetch(os, zp->z_id, 0, start, len,
		    ZIO_PRIORITY_ASYNC_READ);
		break;
	case POSIX_FADV_NORMAL:
	case POSIX_FADV_RANDOM:
	case POSIX_FADV_SEQUENTIAL:
	case POSIX_FADV_DONTNEED:
	case POSIX_FADV_NOREUSE:
		/* ignored for now */
		break;
	default:
		error = EINVAL;
		break;
	}

	zfs_exit(zfsvfs, FTAG);

out_unlock:
	VOP_UNLOCK(vp);

	return (error);
}

static int
zfs_vptocnp(struct vop_vptocnp_args *ap)
{
	vnode_t *covered_vp;
	vnode_t *vp = ap->a_vp;
	zfsvfs_t *zfsvfs = vp->v_vfsp->vfs_data;
	znode_t *zp = VTOZ(vp);
	int ltype;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/*
	 * If we are a snapshot mounted under .zfs, run the operation
	 * on the covered vnode.
	 */
	if (zp->z_id != zfsvfs->z_root || zfsvfs->z_parent == zfsvfs) {
		char name[MAXNAMLEN + 1];
		znode_t *dzp;
		size_t len;

		error = zfs_znode_parent_and_name(zp, &dzp, name,
		    sizeof (name));
		if (error == 0) {
			len = strlen(name);
			if (*ap->a_buflen < len)
				error = SET_ERROR(ENOMEM);
		}
		if (error == 0) {
			*ap->a_buflen -= len;
			memcpy(ap->a_buf + *ap->a_buflen, name, len);
			*ap->a_vpp = ZTOV(dzp);
		}
		zfs_exit(zfsvfs, FTAG);
		return (error);
	}
	zfs_exit(zfsvfs, FTAG);

	covered_vp = vp->v_mount->mnt_vnodecovered;
	enum vgetstate vs = vget_prep(covered_vp);
	ltype = VOP_ISLOCKED(vp);
	VOP_UNLOCK(vp);
	error = vget_finish(covered_vp, LK_SHARED, vs);
	if (error == 0) {
		error = VOP_VPTOCNP(covered_vp, ap->a_vpp, ap->a_buf,
		    ap->a_buflen);
		vput(covered_vp);
	}
	vn_lock(vp, ltype | LK_RETRY);
	if (VN_IS_DOOMED(vp))
		error = SET_ERROR(ENOENT);
	return (error);
}

#if __FreeBSD_version >= 1400032
static int
zfs_deallocate(struct vop_deallocate_args *ap)
{
	znode_t *zp = VTOZ(ap->a_vp);
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;
	zilog_t *zilog;
	off_t off, len, file_sz;
	int error;

	if ((error = zfs_enter_verify_zp(zfsvfs, zp, FTAG)) != 0)
		return (error);

	/*
	 * Callers might not be able to detect properly that we are read-only,
	 * so check it explicitly here.
	 */
	if (zfs_is_readonly(zfsvfs)) {
		zfs_exit(zfsvfs, FTAG);
		return (SET_ERROR(EROFS));
	}

	zilog = zfsvfs->z_log;
	off = *ap->a_offset;
	len = *ap->a_len;
	file_sz = zp->z_size;
	if (off + len > file_sz)
		len = file_sz - off;
	/* Fast path for out-of-range request. */
	if (len <= 0) {
		*ap->a_len = 0;
		zfs_exit(zfsvfs, FTAG);
		return (0);
	}

	error = zfs_freesp(zp, off, len, O_RDWR, TRUE);
	if (error == 0) {
		if (zfsvfs->z_os->os_sync == ZFS_SYNC_ALWAYS ||
		    (ap->a_ioflag & IO_SYNC) != 0)
			zil_commit(zilog, zp->z_id);
		*ap->a_offset = off + len;
		*ap->a_len = 0;
	}

	zfs_exit(zfsvfs, FTAG);
	return (error);
}
#endif

#ifndef _SYS_SYSPROTO_H_
struct vop_copy_file_range_args {
	struct vnode *a_invp;
	off_t *a_inoffp;
	struct vnode *a_outvp;
	off_t *a_outoffp;
	size_t *a_lenp;
	unsigned int a_flags;
	struct ucred *a_incred;
	struct ucred *a_outcred;
	struct thread *a_fsizetd;
}
#endif
/*
 * TODO: FreeBSD will only call file system-specific copy_file_range() if both
 * files resides under the same mountpoint. In case of ZFS we want to be called
 * even is files are in different datasets (but on the same pools, but we need
 * to check that ourselves).
 */
static int
zfs_freebsd_copy_file_range(struct vop_copy_file_range_args *ap)
{
	zfsvfs_t *outzfsvfs;
	struct vnode *invp = ap->a_invp;
	struct vnode *outvp = ap->a_outvp;
	struct mount *mp;
	int error;
	uint64_t len = *ap->a_lenp;

	if (!zfs_bclone_enabled) {
		mp = NULL;
		goto bad_write_fallback;
	}

	/*
	 * TODO: If offset/length is not aligned to recordsize, use
	 * vn_generic_copy_file_range() on this fragment.
	 * It would be better to do this after we lock the vnodes, but then we
	 * need something else than vn_generic_copy_file_range().
	 */

	vn_start_write(outvp, &mp, V_WAIT);
	if (__predict_true(mp == outvp->v_mount)) {
		outzfsvfs = (zfsvfs_t *)mp->mnt_data;
		if (!spa_feature_is_enabled(dmu_objset_spa(outzfsvfs->z_os),
		    SPA_FEATURE_BLOCK_CLONING)) {
			goto bad_write_fallback;
		}
	}
	if (invp == outvp) {
		if (vn_lock(outvp, LK_EXCLUSIVE) != 0) {
			goto bad_write_fallback;
		}
	} else {
#if (__FreeBSD_version >= 1302506 && __FreeBSD_version < 1400000) || \
	__FreeBSD_version >= 1400086
		vn_lock_pair(invp, false, LK_SHARED, outvp, false,
		    LK_EXCLUSIVE);
#else
		vn_lock_pair(invp, false, outvp, false);
#endif
		if (VN_IS_DOOMED(invp) || VN_IS_DOOMED(outvp)) {
			goto bad_locked_fallback;
		}
	}

#ifdef MAC
	error = mac_vnode_check_write(curthread->td_ucred, ap->a_outcred,
	    outvp);
	if (error != 0)
		goto out_locked;
#endif

	error = zfs_clone_range(VTOZ(invp), ap->a_inoffp, VTOZ(outvp),
	    ap->a_outoffp, &len, ap->a_outcred);
	if (error == EXDEV || error == EAGAIN || error == EINVAL ||
	    error == EOPNOTSUPP)
		goto bad_locked_fallback;
	*ap->a_lenp = (size_t)len;
#ifdef MAC
out_locked:
#endif
	if (invp != outvp)
		VOP_UNLOCK(invp);
	VOP_UNLOCK(outvp);
	if (mp != NULL)
		vn_finished_write(mp);
	return (error);

bad_locked_fallback:
	if (invp != outvp)
		VOP_UNLOCK(invp);
	VOP_UNLOCK(outvp);
bad_write_fallback:
	if (mp != NULL)
		vn_finished_write(mp);
	error = vn_generic_copy_file_range(ap->a_invp, ap->a_inoffp,
	    ap->a_outvp, ap->a_outoffp, ap->a_lenp, ap->a_flags,
	    ap->a_incred, ap->a_outcred, ap->a_fsizetd);
	return (error);
}

struct vop_vector zfs_vnodeops;
struct vop_vector zfs_fifoops;
struct vop_vector zfs_shareops;

struct vop_vector zfs_vnodeops = {
	.vop_default =		&default_vnodeops,
	.vop_inactive =		zfs_freebsd_inactive,
	.vop_need_inactive =	zfs_freebsd_need_inactive,
	.vop_reclaim =		zfs_freebsd_reclaim,
	.vop_fplookup_vexec = zfs_freebsd_fplookup_vexec,
	.vop_fplookup_symlink = zfs_freebsd_fplookup_symlink,
	.vop_access =		zfs_freebsd_access,
	.vop_allocate =		VOP_EOPNOTSUPP,
#if __FreeBSD_version >= 1400032
	.vop_deallocate =	zfs_deallocate,
#endif
	.vop_lookup =		zfs_cache_lookup,
	.vop_cachedlookup =	zfs_freebsd_cachedlookup,
	.vop_getattr =		zfs_freebsd_getattr,
	.vop_setattr =		zfs_freebsd_setattr,
	.vop_create =		zfs_freebsd_create,
	.vop_mknod =		(vop_mknod_t *)zfs_freebsd_create,
	.vop_mkdir =		zfs_freebsd_mkdir,
	.vop_readdir =		zfs_freebsd_readdir,
	.vop_fsync =		zfs_freebsd_fsync,
	.vop_open =		zfs_freebsd_open,
	.vop_close =		zfs_freebsd_close,
	.vop_rmdir =		zfs_freebsd_rmdir,
	.vop_ioctl =		zfs_freebsd_ioctl,
	.vop_link =		zfs_freebsd_link,
	.vop_symlink =		zfs_freebsd_symlink,
	.vop_readlink =		zfs_freebsd_readlink,
	.vop_advise =		zfs_freebsd_advise,
	.vop_read =		zfs_freebsd_read,
	.vop_write =		zfs_freebsd_write,
	.vop_remove =		zfs_freebsd_remove,
	.vop_rename =		zfs_freebsd_rename,
	.vop_pathconf =		zfs_freebsd_pathconf,
	.vop_bmap =		zfs_freebsd_bmap,
	.vop_fid =		zfs_freebsd_fid,
	.vop_getextattr =	zfs_getextattr,
	.vop_deleteextattr =	zfs_deleteextattr,
	.vop_setextattr =	zfs_setextattr,
	.vop_listextattr =	zfs_listextattr,
	.vop_getacl =		zfs_freebsd_getacl,
	.vop_setacl =		zfs_freebsd_setacl,
	.vop_aclcheck =		zfs_freebsd_aclcheck,
	.vop_getpages =		zfs_freebsd_getpages,
	.vop_putpages =		zfs_freebsd_putpages,
	.vop_vptocnp =		zfs_vptocnp,
	.vop_lock1 =		vop_lock,
	.vop_unlock =		vop_unlock,
	.vop_islocked =		vop_islocked,
#if __FreeBSD_version >= 1400043
	.vop_add_writecount =	vop_stdadd_writecount_nomsync,
#endif
	.vop_copy_file_range =	zfs_freebsd_copy_file_range,
};
VFS_VOP_VECTOR_REGISTER(zfs_vnodeops);

struct vop_vector zfs_fifoops = {
	.vop_default =		&fifo_specops,
	.vop_fsync =		zfs_freebsd_fsync,
	.vop_fplookup_vexec =	zfs_freebsd_fplookup_vexec,
	.vop_fplookup_symlink = zfs_freebsd_fplookup_symlink,
	.vop_access =		zfs_freebsd_access,
	.vop_getattr =		zfs_freebsd_getattr,
	.vop_inactive =		zfs_freebsd_inactive,
	.vop_read =		VOP_PANIC,
	.vop_reclaim =		zfs_freebsd_reclaim,
	.vop_setattr =		zfs_freebsd_setattr,
	.vop_write =		VOP_PANIC,
	.vop_pathconf = 	zfs_freebsd_pathconf,
	.vop_fid =		zfs_freebsd_fid,
	.vop_getacl =		zfs_freebsd_getacl,
	.vop_setacl =		zfs_freebsd_setacl,
	.vop_aclcheck =		zfs_freebsd_aclcheck,
#if __FreeBSD_version >= 1400043
	.vop_add_writecount =	vop_stdadd_writecount_nomsync,
#endif
};
VFS_VOP_VECTOR_REGISTER(zfs_fifoops);

/*
 * special share hidden files vnode operations template
 */
struct vop_vector zfs_shareops = {
	.vop_default =		&default_vnodeops,
	.vop_fplookup_vexec =	VOP_EAGAIN,
	.vop_fplookup_symlink =	VOP_EAGAIN,
	.vop_access =		zfs_freebsd_access,
	.vop_inactive =		zfs_freebsd_inactive,
	.vop_reclaim =		zfs_freebsd_reclaim,
	.vop_fid =		zfs_freebsd_fid,
	.vop_pathconf =		zfs_freebsd_pathconf,
#if __FreeBSD_version >= 1400043
	.vop_add_writecount =	vop_stdadd_writecount_nomsync,
#endif
};
VFS_VOP_VECTOR_REGISTER(zfs_shareops);

ZFS_MODULE_PARAM(zfs, zfs_, xattr_compat, INT, ZMOD_RW,
	"Use legacy ZFS xattr naming for writing new user namespace xattrs");
