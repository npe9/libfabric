/*
 * Copyright (c) 2013-2019 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "psmx3.h"

/* It is necessary to have a separate thread making progress in order
 * for the wait functions to succeed. This thread is only created when
 * wait functions are called and. In order to minimize performance
 * impact, it only goes active during te time when wait calls are
 * blocked.
 */
#if HAVE_LITHE
static ofi_thread_t	psmx3_wait_thread;
#else
static pthread_t	psmx3_wait_thread;
#endif
#if HAVE_LITHE
static lithe_mutex_t	psmx3_wait_mutex;
static lithe_condvar_t	psmx3_wait_cond;
#else
static pthread_mutex_t	psmx3_wait_mutex;
static pthread_cond_t	psmx3_wait_cond;
#endif
static volatile int	psmx3_wait_thread_ready = 0;
static volatile int	psmx3_wait_thread_enabled = 0;
static volatile int	psmx3_wait_thread_busy = 0;

// When fi_wait is called, this thread is activated (psmx3_wait_thread_enabled)
// While activiated it loops checking each domain for progress.
// When progress is made, CQs/CNTRs with wait_sets will get fd events and
// be woken from the fi_wait call below which will then deactivate this thread.
// In future, once psm3_wait supports multi-EP and multi-rail/QP/addr
// could use psm3_wait to wait for a potentially interesting event and then
// use psmx3_progress_all to check for CQ and CNTR events which will wake
// FDs in the waitset and hence wake the fi_wait.  However, psm3_wait is not
// supported by all modes or HALs so need to use the existing polling approach
// if the HAL does not support psm3_wait, which could be determined by an
// error return from  psm3_wait (PSM2_PARAM_ERR or PSM2_INTERNAL_ERR).
static void *psmx3_wait_progress(void *args)
{
	struct psmx3_fid_fabric *fabric = args;
	struct psmx3_fid_domain *domain;
	struct dlist_entry *item;

	psmx3_wait_thread_ready = 1;
#if HAVE_LITHE
	/* In Lithe, condvar_wait already yields, so we don't need explicit cancellation */
#else
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#endif

	while (1) {
#if HAVE_LITHE
		lithe_mutex_lock(&psmx3_wait_mutex);
		if (!psmx3_wait_thread_enabled)
			lithe_condvar_wait(&psmx3_wait_cond, &psmx3_wait_mutex);
		lithe_mutex_unlock(&psmx3_wait_mutex);
#else
		pthread_mutex_lock(&psmx3_wait_mutex);
		if (!psmx3_wait_thread_enabled)
			pthread_cond_wait(&psmx3_wait_cond, &psmx3_wait_mutex);
		pthread_mutex_unlock(&psmx3_wait_mutex);
		pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
#endif

		psmx3_wait_thread_busy = 1;
		while (psmx3_wait_thread_enabled) {
			psmx3_lock(&fabric->domain_lock, 1);
			dlist_foreach(&fabric->domain_list, item) {
				domain = container_of(item, struct psmx3_fid_domain, entry);
#if HAVE_LITHE
				/* For Lithe, we can't easily compare contexts, so skip if progress thread is enabled */
				if (domain->progress_thread_enabled && domain->progress_thread.context)
					continue;
#else
				if (domain->progress_thread_enabled &&
				    domain->progress_thread != pthread_self())
					continue;
#endif

				psmx3_progress_all(domain);

				if (!psmx3_wait_thread_enabled)
					break;
			}
			psmx3_unlock(&fabric->domain_lock, 1);
#if HAVE_LITHE
			/* Yield periodically to allow other contexts to run (similar to allowing cancellation) */
			lithe_context_yield();
#endif
		}

		psmx3_wait_thread_busy = 0;
#if HAVE_LITHE
		/* In Lithe, we'll yield again when we wait on the condvar */
#else
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
#endif
	}

	return NULL;
}

static void psmx3_wait_start_progress(struct psmx3_fid_fabric *fabric)
{
	struct dlist_entry *item;
	struct psmx3_fid_domain *domain;
	int run_wait_thread = 0;
	pthread_attr_t attr;
	int err;

	if (!fabric)
		return;

#if HAVE_LITHE
	/* Wait threads should never be in the base scheduler - they need a child scheduler */
	lithe_sched_t *current_sched = lithe_sched_current();
	if (current_sched == NULL || current_sched->parent == NULL) {
		/* In base scheduler - can't create wait thread, return */
		return;
	}
#endif

	psmx3_lock(&fabric->domain_lock, 1);
	dlist_foreach(&fabric->domain_list, item) {
		domain = container_of(item, struct psmx3_fid_domain, entry);
#if HAVE_LITHE
		/* For Lithe, we can't easily compare contexts, so run wait thread if progress thread is not enabled */
		if (!domain->progress_thread_enabled || !domain->progress_thread.context)
			run_wait_thread = 1;
#else
		if (!domain->progress_thread_enabled ||
		    domain->progress_thread == pthread_self())
			run_wait_thread = 1;
#endif
	}
	psmx3_unlock(&fabric->domain_lock, 1);

	if (!run_wait_thread)
		return;

#if HAVE_LITHE
	if (!psmx3_wait_thread.context) {
		lithe_mutexattr_t attr;
		lithe_mutexattr_init(&attr);
		lithe_mutex_init(&psmx3_wait_mutex, &attr);
		lithe_condvar_init(&psmx3_wait_cond);
		err = ofi_thread_create(&psmx3_wait_thread, psmx3_wait_progress, (void *)fabric);
		if (err) {
			PSMX3_WARN(&psmx3_prov, FI_LOG_EQ,
				"cannot create wait progress thread\n");
			/* If thread creation failed, don't wait forever */
			return;
		}
		/* Wait for thread to be ready, yielding to allow scheduler to run it */
		/* The thread should start quickly since we yielded in ofi_thread_create */
		/* Keep yielding until the thread sets the ready flag */
		int wait_count = 0;
		const int max_wait = 10000; /* Allow more time for scheduler to run the thread */
		while (!psmx3_wait_thread_ready && wait_count < max_wait) {
			/* Yield to give the scheduler a chance to run the new context */
			lithe_context_yield();
			wait_count++;
		}
		if (!psmx3_wait_thread_ready) {
			PSMX3_WARN(&psmx3_prov, FI_LOG_EQ,
				"wait progress thread did not become ready after %d yields - scheduler may not be running\n", max_wait);
			/* Continue anyway - the code should handle missing wait thread gracefully */
		}
	}
#else
	if (!psmx3_wait_thread) {
		pthread_mutex_init(&psmx3_wait_mutex, NULL);
		pthread_cond_init(&psmx3_wait_cond, NULL);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
		err = pthread_create(&psmx3_wait_thread, &attr,
				     psmx3_wait_progress, (void *)fabric);
		if (err)
			PSMX3_WARN(&psmx3_prov, FI_LOG_EQ,
				"cannot create wait progress thread\n");
		pthread_attr_destroy(&attr);
		while (!psmx3_wait_thread_ready)
			;
	}
#endif

	psmx3_wait_thread_enabled = 1;
#if HAVE_LITHE
	lithe_condvar_signal(&psmx3_wait_cond);
#else
	pthread_cond_signal(&psmx3_wait_cond);
#endif
}

static void psmx3_wait_stop_progress(void)
{
	psmx3_wait_thread_enabled = 0;

	while (psmx3_wait_thread_busy)
		;
}

static struct fi_ops_wait *psmx3_wait_ops_save;
static struct fi_ops_wait psmx3_wait_ops;

static int psmx3_wait_wait_wait(struct fid_wait *wait_fid, int timeout)
{
	struct ofi_epollfds_event event;
	struct util_wait_fd *wait;
	uint64_t endtime;
	int ret;

	wait = container_of(wait_fid, struct util_wait_fd, util_wait.wait_fid);
	endtime = ofi_timeout_time(timeout);

	while (1) {
		ret = wait->util_wait.wait_try(&wait->util_wait);
		if (ret) {
			return ret == -FI_EAGAIN ? 0 : ret;
		}

		if (ofi_adjust_timeout(endtime, &timeout))
			return -FI_ETIMEDOUT;

		ret = (wait->util_wait.wait_obj == FI_WAIT_FD) ?
		      ofi_epoll_wait(wait->epoll_fd, &event, 1, 100) :
		      ofi_pollfds_wait(wait->pollfds, &event, 1, 100);
		if (ret > 0)
			return FI_SUCCESS;

		if (ret < 0) {
#if ENABLE_DEBUG
			/* ignore interrupts in order to enable debugging */
			if (ret == -FI_EINTR)
				continue;
#endif
			PSMX3_WARN(wait->util_wait.prov, FI_LOG_FABRIC,
				"poll failed\n");
			return ret;
		}
	}
}

DIRECT_FN
STATIC int psmx3_wait_wait(struct fid_wait *wait, int timeout)
{
	struct util_wait *wait_priv;
	struct psmx3_fid_fabric *fabric;
	int err;

	wait_priv = container_of(wait, struct util_wait, wait_fid);
	fabric = container_of(wait_priv->fabric, struct psmx3_fid_fabric, util_fabric);

	// special fi_wait support for Intel MPI when FI_PSM3_YIELD_MODE=1
	// When used, fi_wait always delays until next PSM3 internal progress event
	// regardless of how the waitset has been constructed.
	// After fi_wait returns, caller must check for OFI CQ and CNTR events of
	// interest and if none, call fi_wait again.  Those OFI CQ and CNTR checks
	// will do progress_all as needed to complete OFI completion delivery.
	// TBD, in future could perhaps build waitset a specific way, such as
	// FI_WAIT_YIELD entries and then use waitset to figure out which EPs
	// are of interest to the given fi_wait call.  Doing so would need more
	// code in this file as the util_fd_wait code used and the fd_pollset
	// seem opaque and hence don't reveal which their waitset contains.  They
	// don't seem extensible and they have no way to determine if they are empty
	// so we depend on FI_PSM3_YIELD_MODE=1 to disable normal waitset handling
	// and allow this simplified use to meet Intel MPI needs.
	if (psmx3_env.yield_mode) {
		switch (psm3_wait(timeout)) {
		case PSM2_OK:
			return FI_SUCCESS;
		case PSM2_TIMEOUT:
			return -FI_ETIMEDOUT;
		default:
			return -FI_EINVAL;
		}
	}

	/* outside of YIELD_MODE, one must explicitly enable support for
	 * fi_wait().  user beware... this is not supported by PSM3 proper,
	 * but instead only plumbed within the PSMX3 provider shim.
	 */
	if (!psmx3_env.wait_enable) {
		PSMX3_WARN(fabric->util_fabric.prov, FI_LOG_FABRIC,
			"fi_wait() not enabled (see FI_PSM3_WAIT_ENABLE)\n");
		return -FI_ENOSYS;
	}

	psmx3_wait_start_progress(fabric);

	err = psmx3_wait_wait_wait(wait, timeout);

	psmx3_wait_stop_progress();

	return err;
}

DIRECT_FN
int psmx3_wait_open(struct fid_fabric *fabric, struct fi_wait_attr *attr,
		   struct fid_wait **waitset)
{
	struct util_fabric *util_fabric = container_of(fabric, struct util_fabric, fabric_fid);
	struct fid_wait *wait;
	int err;

	switch (attr->wait_obj) {
	case FI_WAIT_UNSPEC:
	case FI_WAIT_FD:
	case FI_WAIT_POLLFD:
		err = ofi_wait_fd_open(fabric, attr, &wait);
		if (err)
			return err;

		break;

	case FI_WAIT_YIELD:
		// NOTE: we use the YIELD type only as a special indicator for
		// the Intel MPI yield mode global wait set.  it is otherwise
		// unsupported
		if (!psmx3_env.yield_mode) {
			PSMX3_WARN(util_fabric->prov, FI_LOG_FABRIC,
				"wait object %u not supported outside of yield mode\n",
				attr->wait_obj);
			return -FI_ENOSYS;
		}

		// if not in YIELD_MODE, create a yield wait object.
		//
		// if in YIELD_MODE, we want callers to only ever wait by
		// invoking a top-level fi_wait(), since YIELD_MODE will turn
		// all waits into a global wait.  can also use a yield wait
		// object for this, justified by:
		//
		// - it is not valid to call fi_control(...GETWAIT...) on a
		//   YIELD object, which is what we want to force callers to
		//   wait via fi_wait().
		// - CQ and CNTR won't be allowed to be added to waitset by
		//   an explicit yield mode check in their open() calls, so the
		//   object type shouldn't matter.
		// - in yield mode, fi_wait() will never interact with the wait
		//   set directly, but instead just delegate to psm3_wait(), so
		//   the underlying wait set type doesn't matter.
		//
		err = ofi_wait_yield_open(fabric, attr, &wait);
		if (err)
			return err;

		break;

	case FI_WAIT_MUTEX_COND:
	default:
		PSMX3_WARN(util_fabric->prov, FI_LOG_FABRIC,
			"wait object %u not supported\n",
			attr->wait_obj);
		return -FI_ENOSYS;
	}

	psmx3_wait_ops_save = wait->ops;
	psmx3_wait_ops = *psmx3_wait_ops_save;
	psmx3_wait_ops.wait = psmx3_wait_wait;
	wait->ops = &psmx3_wait_ops;

	*waitset = wait;
	return 0;
}

DIRECT_FN
int psmx3_wait_trywait(struct fid_fabric *fabric, struct fid **fids, int count)
{
	struct psmx3_fid_cq *cq_priv;
	struct util_eq *eq;
	struct util_wait *wait;
	int i, ret;

	for (i = 0; i < count; i++) {
		switch (fids[i]->fclass) {
			case FI_CLASS_CQ:
				cq_priv = container_of(fids[i], struct psmx3_fid_cq, cq);
				wait = cq_priv->wait;
				break;
			case FI_CLASS_EQ:
				eq = container_of(fids[i], struct util_eq, eq_fid.fid);
				wait = eq->wait;
				break;
			case FI_CLASS_CNTR:
				return -FI_ENOSYS;
			case FI_CLASS_WAIT:
				wait = container_of(fids[i], struct util_wait, wait_fid.fid);
				break;
			default:
				return -FI_EINVAL;
		}

		ret = wait->wait_try(wait);
		if (ret)
			return ret;
	}
	return 0;
}

