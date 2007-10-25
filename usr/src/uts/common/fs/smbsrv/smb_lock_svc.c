/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
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
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * This module provides range lock functionality for CIFS/SMB clients.
 * Lock range service functions process SMB lock and and unlock
 * requests for a file by applying lock rules and marks file range
 * as locked if the lock is successful otherwise return proper
 * error code.
 */

#include <smbsrv/smb_incl.h>

static int
smb_lock_range_overlap(struct smb_lock *lock, uint64_t start, uint64_t length);

static uint32_t smb_lock_range_lckrules(struct smb_request *sr,
    smb_ofile_t *file, struct smb_node *node, smb_lock_t *dlock,
    smb_lock_t **clockp);

static uint32_t
smb_lock_wait(struct smb_request *sr, smb_lock_t *b_lock, smb_lock_t *c_lock);

static uint32_t
smb_lock_range_ulckrules(struct smb_request *sr,
    struct smb_node *node,
    uint64_t start,
    uint64_t length,
    struct smb_lock **nodelock);

static smb_lock_t *smb_lock_create(smb_request_t *sr,
    uint64_t start, uint64_t length, uint32_t locktype, uint32_t timeout);
static void smb_lock_destroy(smb_lock_t *lock);
static void smb_lock_free(smb_lock_t *lock);

/*
 * smb_lock_range_overlap
 *
 * Checks if lock range(start, length) overlaps
 * range in lock structure.
 *
 * return values:
 *	0 - Lock range doesn't overlap
 *	1 - Lock range overlaps.
 */

#define	RANGE_NO_OVERLAP	0
#define	RANGE_OVERLAP		1

static int
smb_lock_range_overlap(struct smb_lock *lock, uint64_t start, uint64_t length)
{
	/* A zero-length range doesn't overlap anything */
	if (length == 0 || lock->l_length == 0)
		return (RANGE_NO_OVERLAP);

	if (start < lock->l_start) {
		if (start + length > lock->l_start)
			return (RANGE_OVERLAP);
	} else if (start < lock->l_start + lock->l_length)
		return (RANGE_OVERLAP);

	if (start + length > lock->l_start + lock->l_length) {
		if (start < lock->l_start + lock->l_length)
			return (RANGE_OVERLAP);
	} else if (start + length > lock->l_start)
		return (RANGE_OVERLAP);

	/* Lock range doen't overlap */
	return (RANGE_NO_OVERLAP);
}

/*
 * smb_lock_range_lckrules
 *
 * Lock range rules:
 *	1. Overlapping read locks are allowed if the
 *	   current locks in the region are only read locks
 *	   irrespective of pid of smb client issuing lock request.
 *
 *	2. Read lock in the overlapped region of write lock
 *	   are allowed if the pervious lock is performed by the
 *	   same pid and connection.
 *
 * return status:
 *	NT_STATUS_SUCCESS - Input lock range adapts to lock rules.
 *	NT_STATUS_LOCK_NOT_GRANTED - Input lock conflicts lock rules.
 *	NT_STATUS_CANCELLED - Error in processing lock rules
 */
static uint32_t
smb_lock_range_lckrules(
    struct smb_request	*sr,
    smb_ofile_t		*file,
    struct smb_node	*node,
    smb_lock_t		*dlock,
    smb_lock_t		**clockp)
{
	smb_lock_t	*lock;
	uint32_t	status = NT_STATUS_SUCCESS;

	/* Check if file is closed */
	if (!smb_ofile_is_open(file)) {
		return (NT_STATUS_RANGE_NOT_LOCKED);
	}

	/* Caller must hold lock for node->n_lock_list */
	for (lock = smb_llist_head(&node->n_lock_list);
	    lock != NULL;
	    lock = smb_llist_next(&node->n_lock_list, lock)) {

		if (!smb_lock_range_overlap(lock, dlock->l_start,
		    dlock->l_length))
			continue;

		/*
		 * Check to see if lock in the overlapping record
		 * is only read lock. Current finding is read
		 * locks can overlapped irrespective of pids.
		 */
		if ((lock->l_type == SMB_LOCK_TYPE_READONLY) &&
		    (dlock->l_type == SMB_LOCK_TYPE_READONLY)) {
			continue;
		}

		/*
		 * When the read lock overlaps write lock, check if
		 * allowed.
		 */
		if ((dlock->l_type == SMB_LOCK_TYPE_READONLY) &&
		    !(lock->l_type == SMB_LOCK_TYPE_READONLY)) {
			if (lock->l_file == sr->fid_ofile &&
			    lock->l_session_kid == sr->session->s_kid &&
			    lock->l_pid == sr->smb_pid &&
			    lock->l_uid == sr->smb_uid) {
				continue;
			}
		}

		/* Conflict in overlapping lock element */
		*clockp = lock;
		status = NT_STATUS_LOCK_NOT_GRANTED;
		break;
	}

	return (status);
}

/*
 * smb_lock_wait
 *
 * Wait operation for smb overlapping lock to be released.  Caller must hold
 * write lock for node->n_lock_list so that the set of active locks can't
 * change unexpectedly.  The lock for node->n_lock_list  will be released
 * within this function during the sleep after the lock dependency has
 * been recorded.
 *
 * return value
 *
 *	NT_STATUS_CANCELLED		Error occurred during wait operation
 *	NT_STATUS_SUCCESS		Wait completed.
 */
static uint32_t
smb_lock_wait(smb_request_t *sr, smb_lock_t *b_lock, smb_lock_t *c_lock)
{
	clock_t		result;
	uint32_t	status = NT_STATUS_SUCCESS;

	ASSERT(sr->sr_awaiting == NULL);

	mutex_enter(&sr->sr_mutex);

	switch (sr->sr_state) {
	case SMB_REQ_STATE_ACTIVE:
		/*
		 * Wait up till the timeout time keeping track of actual
		 * time waited for possible retry failure.
		 */
		sr->sr_state = SMB_REQ_STATE_WAITING_LOCK;
		sr->sr_awaiting = c_lock;
		mutex_exit(&sr->sr_mutex);

		mutex_enter(&c_lock->l_mutex);
		/*
		 * The conflict list (l_conflict_list) for a lock contains
		 * all the locks that are blocked by and in conflict with
		 * that lock.  Add the new lock to the conflict list for the
		 * active lock.
		 *
		 * l_conflict_list is currently a fancy way of representing
		 * the references/dependencies on a lock.  It could be
		 * replaced with a reference count but this approach
		 * has the advantage that MDB can display the lock
		 * dependencies at any point in time.  In the future
		 * we should be able to leverage the list to implement
		 * an asynchronous locking model.
		 *
		 * l_blocked_by is the reverse of the conflict list.  It
		 * points to the lock that the new lock conflicts with.
		 * As currently implemented this value is purely for
		 * debug purposes -- there are windows of time when
		 * l_blocked_by may be non-NULL even though there is no
		 * conflict list
		 */
		b_lock->l_blocked_by = c_lock;
		smb_slist_insert_tail(&c_lock->l_conflict_list, b_lock);
		smb_llist_exit(&c_lock->l_file->f_node->n_lock_list);

		/*
		 * XXX Hack.. drop s_lock to avoid blocking subsequent SMBs
		 * that might affect the state of this lock (i.e.
		 * smb_com_close).  We shouldn't sleep while holding
		 * locks anyway.
		 */
		smb_rwx_rwexit(&sr->session->s_lock);

		if (SMB_LOCK_INDEFINITE_WAIT(b_lock)) {
			cv_wait(&c_lock->l_cv, &c_lock->l_mutex);
		} else {
			result = cv_timedwait(&c_lock->l_cv,
			    &c_lock->l_mutex, b_lock->l_end_time);
			if (result == -1) {
				status = NT_STATUS_CANCELLED;
			}
		}

		/*
		 * XXX Hack continued from above... re-acquire s_lock
		 * OK to hardcode RW_READER since this is just a hack and
		 * we really should yank it out and do something else.
		 */
		smb_rwx_rwenter(&sr->session->s_lock, RW_READER);

		mutex_exit(&c_lock->l_mutex);

		smb_llist_enter(&c_lock->l_file->f_node->n_lock_list,
		    RW_WRITER);
		smb_slist_remove(&c_lock->l_conflict_list, b_lock);

		mutex_enter(&sr->sr_mutex);
		sr->sr_awaiting = NULL;
		if (sr->sr_state == SMB_REQ_STATE_CANCELED) {
			status = NT_STATUS_CANCELLED;
		} else {
			sr->sr_state = SMB_REQ_STATE_ACTIVE;
		}
		break;

	case SMB_REQ_STATE_CANCELED:
		status = NT_STATUS_CANCELLED;
		mutex_exit(&sr->sr_mutex);
		break;

	default:
		ASSERT(0);
		break;
	}

	mutex_exit(&sr->sr_mutex);

	return (status);
}

/*
 * smb_lock_range_ulckrules
 *
 *	1. Unlock should be performed at exactly matching ends.
 *	   This has been changed because overlapping ends is
 *	   allowed and there is no other precise way of locating
 *	   lock entity in node lock list.
 *
 *	2. Unlock is failed if there is no corresponding lock exists.
 *
 * Return values
 *
 *	NT_STATUS_SUCCESS		Unlock request matches lock record
 *					pointed by 'nodelock' lock structure.
 *
 *	NT_STATUS_RANGE_NOT_LOCKED	Unlock request doen't match any
 *					of lock record in node lock request or
 *					error in unlock range processing.
 */
static uint32_t
smb_lock_range_ulckrules(
    struct smb_request *sr,
    struct smb_node *node,
    uint64_t start,
    uint64_t length,
    struct smb_lock **nodelock)
{
	smb_lock_t	*lock;
	uint32_t	status = NT_STATUS_RANGE_NOT_LOCKED;

	/* Caller must hold lock for node->n_lock_list */
	for (lock = smb_llist_head(&node->n_lock_list);
	    lock != NULL;
	    lock = smb_llist_next(&node->n_lock_list, lock)) {

		if ((start == lock->l_start) &&
		    (length == lock->l_length) &&
		    lock->l_file == sr->fid_ofile &&
		    lock->l_session_kid == sr->session->s_kid &&
		    lock->l_pid == sr->smb_pid &&
		    lock->l_uid == sr->smb_uid) {
			*nodelock = lock;
			status = NT_STATUS_SUCCESS;
			break;
		}
	}

	return (status);
}


/*
 * smb_unlock_range
 *
 * locates lock range performed for corresponding to unlock request.
 *
 * NT_STATUS_SUCCESS - Lock range performed successfully.
 * !NT_STATUS_SUCCESS - Error in unlock range operation.
 */
uint32_t
smb_unlock_range(
    struct smb_request *sr,
    struct smb_node *node,
    uint64_t start,
    uint64_t length)
{
	struct smb_lock *lock = 0;
	uint32_t status;

	/* Apply unlocking rules */
	smb_llist_enter(&node->n_lock_list, RW_WRITER);
	status = smb_lock_range_ulckrules(sr, node, start, length, &lock);
	if (status != NT_STATUS_SUCCESS) {
		/*
		 * If lock range is not matching in the list
		 * return error.
		 */
		ASSERT(lock == 0);
		smb_llist_exit(&node->n_lock_list);
		return (status);
	}

	smb_llist_remove(&node->n_lock_list, lock);
	smb_llist_exit(&node->n_lock_list);

	smb_lock_destroy(lock);

	return (status);
}

/*
 * smb_lock_range
 *
 * checks for integrity of
 * file lock operation for the given range of file data.
 * This is performed by applying lock rules with all
 * the elements of the node lock list.
 *
 * The function returns with new lock added if lock
 * request is non-conflicting with existing range
 * lock for the file. Otherwise smb request is filed
 * without returning.
 *
 * NT_STATUS_SUCCESS - Lock range performed successfully.
 * !NT_STATUS_SUCCESS - Error in lock range operation.
 */
uint32_t
smb_lock_range(
    struct smb_request	*sr,
    smb_ofile_t		*file,
    uint64_t		start,
    uint64_t		length,
    uint32_t		timeout,
    uint32_t		locktype)
{
	smb_node_t *node = file->f_node;
	smb_lock_t *lock;
	smb_lock_t *clock = 0;
	uint32_t result = NT_STATUS_SUCCESS;

	lock = smb_lock_create(sr, start, length, locktype, timeout);

	smb_llist_enter(&node->n_lock_list, RW_WRITER);
	for (;;) {
		/* Apply locking rules */
		result = smb_lock_range_lckrules(sr, file, node, lock, &clock);

		if ((result == NT_STATUS_CANCELLED) ||
		    (result == NT_STATUS_SUCCESS) ||
		    (result == NT_STATUS_RANGE_NOT_LOCKED)) {
			ASSERT(clock == 0);
			break;
		} else {
			ASSERT(result == NT_STATUS_LOCK_NOT_GRANTED);
			ASSERT(clock);
			/*
			 * Call smb_lock_wait holding write lock for
			 * node lock list.  smb_lock_wait will release
			 * this lock if it blocks.
			 */
			ASSERT(node == clock->l_file->f_node);
			if ((timeout == 0) ||
			    ((result = smb_lock_wait(sr, lock, clock)) !=
			    NT_STATUS_SUCCESS)) {
				break;
			}
			clock = 0;
		}
	}

	lock->l_blocked_by = NULL;

	if (result != NT_STATUS_SUCCESS) {
		/*
		 * Under certain conditions NT_STATUS_FILE_LOCK_CONFLICT
		 * should be returned instead of NT_STATUS_LOCK_NOT_GRANTED.
		 */
		if (result == NT_STATUS_LOCK_NOT_GRANTED) {
			/*
			 * Locks with timeouts always return
			 * NT_STATUS_FILE_LOCK_CONFLICT
			 */
			if (timeout != 0) {
				result = NT_STATUS_FILE_LOCK_CONFLICT;
			}

			/*
			 * Locks starting higher than 0xef000000 that do not
			 * have the MSB set always return
			 * NT_STATUS_FILE_LOCK_CONFLICT
			 */
			if ((lock->l_start >= 0xef000000) &&
			    !(lock->l_start & (1ULL << 63))) {
				result = NT_STATUS_FILE_LOCK_CONFLICT;
			}

			/*
			 * If the last lock attempt to fail on this file handle
			 * started at the same offset as this one then return
			 * NT_STATUS_FILE_LOCK_CONFLICT
			 */
			mutex_enter(&file->f_mutex);
			if ((file->f_flags & SMB_OFLAGS_LLF_POS_VALID) &&
			    (lock->l_start == file->f_llf_pos)) {
				result = NT_STATUS_FILE_LOCK_CONFLICT;
			}
			mutex_exit(&file->f_mutex);
		}

		/* Update last lock failed offset */
		mutex_enter(&file->f_mutex);
		file->f_llf_pos = lock->l_start;
		file->f_flags |= SMB_OFLAGS_LLF_POS_VALID;
		mutex_exit(&file->f_mutex);

		smb_lock_free(lock);
	} else {
		smb_llist_insert_tail(&node->n_lock_list, lock);
	}
	smb_llist_exit(&node->n_lock_list);

	return (result);
}


/*
 * smb_lock_range_access
 *
 * scans node lock list
 * to check if there is any overlapping lock. Overlapping
 * lock is allowed only under same session and client pid.
 *
 * Return values
 *	NT_STATUS_SUCCESS		lock access granted.
 *	NT_STATUS_FILE_LOCK_CONFLICT 	access denied due to lock conflict.
 */
int
smb_lock_range_access(
    struct smb_request *sr,
    struct smb_node *node,
    uint64_t start,
    uint64_t length,
    uint32_t desired_access)
{
	smb_lock_t	*lock;
	smb_llist_t	*llist;
	int		status = NT_STATUS_SUCCESS;

	ASSERT((desired_access & ~(FILE_READ_DATA | FILE_WRITE_DATA)) == 0);
	ASSERT((desired_access & (FILE_READ_DATA | FILE_WRITE_DATA)) != 0);

	llist = &node->n_lock_list;
	smb_llist_enter(llist, RW_READER);
	/* Search for any applicable lock */
	for (lock = smb_llist_head(llist);
	    lock != 0;
	    lock = smb_llist_next(llist, lock)) {

		if (!smb_lock_range_overlap(lock, start, length))
			/* Lock does not overlap */
			continue;

		if (lock->l_type == SMB_LOCK_TYPE_READONLY &&
		    desired_access == FILE_READ_DATA)
			continue;

		if (lock->l_type == SMB_LOCK_TYPE_READWRITE &&
		    lock->l_session_kid == sr->session->s_kid &&
		    lock->l_pid == sr->smb_pid)
			continue;

		status = NT_STATUS_FILE_LOCK_CONFLICT;
		break;
	}
	smb_llist_exit(llist);
	return (status);
}

static smb_lock_t *
smb_lock_create(
    smb_request_t *sr,
    uint64_t start,
    uint64_t length,
    uint32_t locktype,
    uint32_t timeout)
{
	smb_lock_t *lock;

	ASSERT(locktype == SMB_LOCK_TYPE_READWRITE ||
	    locktype == SMB_LOCK_TYPE_READONLY);

	lock = kmem_zalloc(sizeof (smb_lock_t), KM_SLEEP);
	lock->l_magic = SMB_LOCK_MAGIC;
	lock->l_sr = sr; /* Invalid after lock is active */
	lock->l_session_kid = sr->session->s_kid;
	lock->l_session = sr->session;
	lock->l_file = sr->fid_ofile;
	lock->l_uid = sr->smb_uid;
	lock->l_pid = sr->smb_pid;
	lock->l_type = locktype;
	lock->l_start = start;
	lock->l_length = length;
	/*
	 * Calculate the absolute end time so that we can use it
	 * in cv_timedwait.
	 */
	lock->l_end_time = lbolt + MSEC_TO_TICK(timeout);
	if (timeout == 0xffffffff) {
		lock->l_flags |= SMB_LOCK_FLAG_INDEFINITE;
	}
	mutex_init(&lock->l_mutex, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&lock->l_cv, NULL, CV_DEFAULT, NULL);
	smb_slist_constructor(&lock->l_conflict_list, sizeof (smb_lock_t),
	    offsetof(smb_lock_t, l_conflict_lnd));

	return (lock);
}

static void
smb_lock_free(smb_lock_t *lock)
{
	smb_slist_destructor(&lock->l_conflict_list);
	cv_destroy(&lock->l_cv);
	mutex_destroy(&lock->l_mutex);

	kmem_free(lock, sizeof (smb_lock_t));
}

/*
 * smb_lock_destroy
 *
 * Caller must hold node->n_lock_list
 */
static void
smb_lock_destroy(smb_lock_t *lock)
{
	/*
	 * Caller must hold node->n_lock_list lock.
	 */
	mutex_enter(&lock->l_mutex);
	cv_broadcast(&lock->l_cv);
	mutex_exit(&lock->l_mutex);

	/*
	 * The cv_broadcast above should wake up any locks that previous
	 * had conflicts with this lock.  Wait for the locking threads
	 * to remove their references to this lock.
	 */
	smb_slist_wait_for_empty(&lock->l_conflict_list);

	smb_lock_free(lock);
}

void
smb_node_destroy_lock_by_ofile(smb_node_t *node, smb_ofile_t *file)
{
	smb_lock_t	*lock;
	smb_lock_t	*nxtl;
	list_t		destroy_list;

	ASSERT(node);
	ASSERT(node->n_magic == SMB_NODE_MAGIC);
	ASSERT(node->n_state == SMB_NODE_STATE_AVAILABLE);
	ASSERT(node->n_refcnt);

	/*
	 * Move locks matching the specified file from the node->n_lock_list
	 * to a temporary list (holding the lock the entire time) then
	 * destroy all the matching locks.  We can't call smb_lock_destroy
	 * while we are holding the lock for node->n_lock_list because we will
	 * deadlock and we can't drop the lock because the list contents might
	 * change (for example nxtl might get removed on another thread).
	 */
	list_create(&destroy_list, sizeof (smb_lock_t),
	    offsetof(smb_lock_t, l_lnd));

	smb_llist_enter(&node->n_lock_list, RW_WRITER);
	lock = smb_llist_head(&node->n_lock_list);
	while (lock) {
		nxtl = smb_llist_next(&node->n_lock_list, lock);
		if (lock->l_file == file) {
			smb_llist_remove(&node->n_lock_list, lock);
			list_insert_tail(&destroy_list, lock);
		}
		lock = nxtl;
	}
	smb_llist_exit(&node->n_lock_list);

	lock = list_head(&destroy_list);
	while (lock) {
		nxtl = list_next(&destroy_list, lock);
		list_remove(&destroy_list, lock);
		smb_lock_destroy(lock);
		lock = nxtl;
	}

	list_destroy(&destroy_list);
}

void
smb_lock_range_raise_error(smb_request_t *sr, uint32_t ntstatus)
{
	switch (ntstatus) {
	case NT_STATUS_CANCELLED:
		/*
		 * XXX What is the proper error here?
		 */
		smbsr_raise_error(sr, ERRDOS, ERRlock);
		/* NOTREACHED */
	case NT_STATUS_FILE_LOCK_CONFLICT:
		smbsr_raise_cifs_error(sr, NT_STATUS_FILE_LOCK_CONFLICT,
		    ERRDOS, ERRlock);
		/* NOTREACHED */
	case NT_STATUS_LOCK_NOT_GRANTED:
		smbsr_raise_cifs_error(sr, NT_STATUS_LOCK_NOT_GRANTED,
		    ERRDOS, ERRlock);
		/* NOTREACHED */
	case NT_STATUS_RANGE_NOT_LOCKED:
		smbsr_raise_cifs_error(sr, NT_STATUS_RANGE_NOT_LOCKED,
		    ERRDOS, ERRlock);
		/* NOTREACHED */
	default:
		ASSERT(0);
		smbsr_raise_error(sr, ERRDOS, ntstatus);
		/* NOTREACHED */
	}
}


void
smb_unlock_range_raise_error(smb_request_t *sr, uint32_t ntstatus)
{
	switch (ntstatus) {
	case NT_STATUS_RANGE_NOT_LOCKED:
		smbsr_raise_cifs_error(sr, NT_STATUS_RANGE_NOT_LOCKED,
		    ERRDOS, ERRnotlocked);
		/* NOTREACHED */
	default:
		ASSERT(0);
		smbsr_raise_error(sr, ERRDOS, ntstatus);
		/* NOTREACHED */
	}
}
