/*
 * Copyright (c) 2013-2016 Intel Corporation. All rights reserved.
 * Copyright (c) 2016 Cisco Systems, Inc.  All rights reserved.
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif /* _GNU_SOURCE */

#include "config.h"

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <execinfo.h>
#include <pthread.h>
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "ofi.h"
#include "ofi_osd.h"
#include "ofi_file.h"
#include "ofi_util.h"

#include "rdma/fi_errno.h"
#include "rdma/providers/fi_log.h"

#if HAVE_LITHE
/* Include Lithe headers at file scope */
/* Save and undefine conflicting macros/types before including parlib */
/* libfabric may have defined these as macros, parlib defines them as functions */
#ifdef LOG2_DOWN
#define _OFI_SAVED_LOG2_DOWN
#undef LOG2_DOWN
#endif
#ifdef LOG2_UP
#define _OFI_SAVED_LOG2_UP
#undef LOG2_UP
#endif
#ifdef ROUNDUPPWR2
#define _OFI_SAVED_ROUNDUPPWR2
#undef ROUNDUPPWR2
#endif

/* Include parlib and lithe headers with atomic symbol renaming */
/* First undefine stdatomic.h's atomic_init if it exists */
#ifdef atomic_init
#undef atomic_init
#endif
/* Rename parlib's atomic_t and atomic_init to avoid conflicts */
#define atomic_t __parlib_atomic_t_renamed
#define atomic_init __parlib_atomic_init_renamed

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wnested-externs"
/* Don't include parlib.h - it has constructors that interfere with parlib's initialization */
/* Only include Lithe headers we actually need */
#include <lithe/lithe.h>
#include <lithe/mutex.h>
#include <lithe/condvar.h>
#include <lithe/fork_join_sched.h>
#pragma GCC diagnostic pop

/* Restore atomic names for libfabric */
#undef atomic_t
#undef atomic_init
#endif /* HAVE_LITHE */

#ifdef __FreeBSD__
typedef cpuset_t ofi_cpu_set_t;
#elif defined __linux__
typedef cpu_set_t ofi_cpu_set_t;
#endif

#if !HAVE_CLOCK_GETTIME
int clock_gettime(clockid_t clk_id, struct timespec *tp) {
	int retval;
	struct timeval tv;

	retval = gettimeofday(&tv, NULL);

	tp->tv_sec = tv.tv_sec;
	tp->tv_nsec = tv.tv_usec * 1000;

	return retval;
}
#endif /* !HAVE_CLOCK_GETTIME */

int fi_fd_nonblock(int fd)
{
	long flags = 0;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0) {
		return -errno;
	}

	if(fcntl(fd, F_SETFL, flags | O_NONBLOCK))
		return -errno;

	return 0;
}

int fi_fd_block(int fd)
{
	long flags = 0;

	flags = fcntl(fd, F_GETFL);
	if (flags < 0) {
		return -errno;
	}

	if(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK))
		return -errno;

	return 0;
}

/* Global flag to track cleanup state */
volatile int ofi_cleanup_in_progress = 0;

#if HAVE_LITHE
/* Our own scheduler - created in constructor, destroyed in atexit handler */
lithe_fork_join_sched_t *ofi_scheduler = NULL;
static int ofi_scheduler_entered = 0;

static int ofi_debug_enabled(void) {
    return getenv("OFI_DEBUG") != NULL;
}

/* Create our own scheduler */
static void ofi_create_scheduler(void) {
    if (ofi_scheduler != NULL) {
        return; /* Already created */
    }
    
    /* Create a fork-join scheduler for libfabric */
    ofi_scheduler = lithe_fork_join_sched_create();
    if (ofi_scheduler == NULL) {
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] Failed to create libfabric scheduler\n");
            fflush(stderr);
        }
        return;
    }
    
    if (ofi_debug_enabled()) {
        fprintf(stderr, "[OFI_DEBUG] Created libfabric scheduler %p\n", (void*)ofi_scheduler);
        fflush(stderr);
    }
}

/* Enter our scheduler if we're in a scheduler context */
static void ofi_enter_scheduler(void) {
    if (ofi_scheduler == NULL) {
        ofi_create_scheduler();
    }
    
    if (ofi_scheduler_entered) {
        return; /* Already entered */
    }
    
    /* Check if we're in a scheduler context (not base scheduler) */
    lithe_sched_t *current_sched = lithe_sched_current();
    if (current_sched != NULL && current_sched->parent != NULL) {
        /* We're in a scheduler - enter our scheduler as a child */
        lithe_sched_enter((lithe_sched_t*)ofi_scheduler);
        ofi_scheduler_entered = 1;
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] Entered libfabric scheduler %p\n", (void*)ofi_scheduler);
            fflush(stderr);
        }
    }
}

/* Get our scheduler - create and enter it if needed */
static lithe_fork_join_sched_t *ofi_get_scheduler(void) {
    if (ofi_scheduler == NULL) {
        ofi_create_scheduler();
    }
    
    /* Try to enter it if we're in a scheduler context */
    ofi_enter_scheduler();
    
    return ofi_scheduler;
}

/* Cleanup handler - destroy our scheduler */
static void ofi_cleanup_handler(void) {
    ofi_cleanup_in_progress = 1;
    if (ofi_scheduler != NULL) {
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] ofi_cleanup_handler: Destroying libfabric scheduler %p\n", (void*)ofi_scheduler);
            fflush(stderr);
        }
        lithe_fork_join_sched_destroy(ofi_scheduler);
        ofi_scheduler = NULL;
        ofi_scheduler_entered = 0;
    }
}

/* Constructor(200): init Lithe locks before any fi_* call. OMPI5 does not call
 * ofi_init_lithe_scheduler, so common_locks and mem_monitor locks must be
 * initialized here. Priority 200 runs after Lithe/parlib (65535). */
static void ofi_init_lithe_locks(void) {
    extern void ofi_init_common_locks(void);
    extern void ofi_init_mem_monitor_locks(void);
    ofi_init_common_locks();
    ofi_init_mem_monitor_locks();
}
__attribute__((constructor(200)))
static void ofi_init_lithe_locks_ctor(void) {
    ofi_init_lithe_locks();
}

/* Initialize libfabric's Lithe scheduler - may be called from OpenMPI for full init */
void ofi_init_lithe_scheduler(void) {
    /* Register cleanup handler */
    atexit(ofi_cleanup_handler);
    
    /* Create our scheduler */
    ofi_create_scheduler();
    
    /* Locks already initialized by constructor; ensure idempotent */
    ofi_init_lithe_locks();
    
    /* Try to enter it if we're in a scheduler context (not base scheduler) */
    lithe_sched_t *current_sched = lithe_sched_current();
    if (current_sched != NULL && current_sched->parent != NULL) {
        /* We're in a scheduler context - enter our scheduler */
        lithe_sched_enter((lithe_sched_t*)ofi_scheduler);
        ofi_scheduler_entered = 1;
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] ofi_init_lithe_scheduler: Entered libfabric scheduler %p\n", (void*)ofi_scheduler);
            fflush(stderr);
        }
    } else if (ofi_debug_enabled()) {
        fprintf(stderr, "[OFI_DEBUG] ofi_init_lithe_scheduler: In base scheduler, will enter scheduler lazily\n");
        fflush(stderr);
    }
}

/* Ensure we're in the scheduler before using mutexes */
/* This is called from other threads/contexts that might not be in the scheduler */
void ofi_ensure_in_scheduler(void) {
    /* Try to enter our scheduler if we're in a scheduler context */
    ofi_enter_scheduler();
}

/* Wrapper for thread function signature conversion */
struct ofi_thread_wrapper {
    void *(*pthread_func)(void *);
    void *arg;
};

static void ofi_thread_wrapper_func(void *arg) {
    struct ofi_thread_wrapper *wrapper = (struct ofi_thread_wrapper *)arg;
    wrapper->pthread_func(wrapper->arg);
    free(wrapper);
}

/* Thread creation using Lithe contexts.
 *
 * IMPORTANT (composition / no-hijack): the background helper context (e.g. the
 * kdreg2 MR-cache evictor, transport progress threads) is created in the
 * CALLER'S CURRENT fork-join scheduler -- normally the process-wide root FJS
 * that the main rank context already runs under. We deliberately do NOT enter
 * a separate libfabric child scheduler here.
 *
 * The old code called ofi_enter_scheduler() -> lithe_sched_enter(ofi_scheduler)
 * with no matching lithe_sched_exit(). lithe_sched_enter() HIJACKS the current
 * context (swaps in child->main_context and stashes the caller as
 * child->parent_context), so the rank's main context was permanently moved into
 * libfabric's child scheduler. The rest of MPI_Init -- including the PMIx fence
 * -- then ran as the CHILD sched's master context. That child master context,
 * when it blocks on the parlib reactor (PMIx/libevent socket wait) and is later
 * woken, is not re-dispatched by the parent because the cross-scheduler
 * hart-grant of a child master context deadlocks: all vcores park idle in the
 * root's reactor drive while the woken context waits for a hart that never
 * arrives (observed as an MPI_Init hang at VCORE_LIMIT=1 and >=2 whenever the
 * kdreg2 evictor is present; disabling the monitor -- and thus never entering
 * the child sched -- ran clean). Creating the helper in the current (root) FJS
 * keeps main in the root, and the helper is a normal worker context that the
 * root already schedules correctly (it parks on the reactor and is woken via
 * the standard uthread_runnable path). */
int ofi_thread_create(ofi_thread_t *thread, void *(*start_routine)(void *), void *arg) {
    /* Initialize thread context to NULL in case we fail early */
    thread->context = NULL;

    /* Create the helper context in the CURRENT fork-join scheduler. Must be a
     * real (non-base) scheduler: the root FJS has parent == base_sched. We do
     * NOT enter a child scheduler (see the no-hijack note above). */
    lithe_sched_t *current_sched = lithe_sched_current();
    if (current_sched == NULL || current_sched->parent == NULL) {
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: not in a schedulable context (current_sched=%p) -- cannot create helper\n",
                    (void *)current_sched);
            fflush(stderr);
        }
        /* thread->context is already NULL from initialization above */
        return -1;
    }

    /* LITHE_PROGRESS_SCHED=child: evictor/progress helpers on dedicated child FJS. */
    lithe_fork_join_sched_t *sched = lithe_fork_join_sched_progress_sched();
    if (sched == NULL)
        sched = (lithe_fork_join_sched_t *)current_sched;
    
    if (ofi_debug_enabled()) {
        fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: In scheduler %p, creating context\n", sched);
        fflush(stderr);
    }
    
    /* Create wrapper to convert pthread-style function to Lithe-style */
    struct ofi_thread_wrapper *wrapper = malloc(sizeof(struct ofi_thread_wrapper));
    if (wrapper == NULL) {
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: Failed to allocate wrapper\n");
            fflush(stderr);
        }
        return -1;
    }
    wrapper->pthread_func = start_routine;
    wrapper->arg = arg;
    
    /* Create a context in the progress or current fork-join scheduler. */
    lithe_fork_join_context_t *ctx = lithe_fork_join_context_create(
        sched,
        FJS_STACK_SIZE,
        ofi_thread_wrapper_func,
        wrapper
    );
    
    if (ctx == NULL) {
        if (ofi_debug_enabled()) {
            fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: Failed to create context\n");
            fflush(stderr);
        }
        free(wrapper);
        thread->context = NULL; /* Ensure it's NULL on failure */
        return -1;
    }
    
    if (ofi_debug_enabled()) {
        fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: Created context %p, yielding to scheduler\n", ctx);
        fflush(stderr);
    }
    thread->context = ctx;
    
    /* lithe_fork_join_context_create automatically enqueues the context to the scheduler */
    /* The context is now in the scheduler's run queue and will be scheduled automatically */
    /* We must yield to allow the scheduler to run the new context */
    /* Keep yielding until we're confident the scheduler has had a chance to run it */
    /* This is critical - without yielding, the new context may never run */
    for (int i = 0; i < 10; i++) {
        lithe_context_yield();
    }
    
    if (ofi_debug_enabled()) {
        fprintf(stderr, "[OFI_DEBUG] ofi_thread_create: Finished yielding, context should be running\n");
        fflush(stderr);
    }
    return 0;
}

int ofi_thread_join(ofi_thread_t *thread, void **retval) {
    if (thread->context == NULL) {
        return -1;
    }
    
    /* Get the current scheduler */
    lithe_fork_join_sched_t *sched = ofi_get_scheduler();
    if (sched == NULL) {
        return -1; /* Not in a scheduler */
    }
    
    /* Join the context - this will block until it completes */
    lithe_fork_join_sched_join_one(sched);
    
    /* Clean up the context */
    lithe_fork_join_context_cleanup(thread->context);
    lithe_fork_join_context_destroy(thread->context);
    thread->context = NULL;
    
    if (retval) {
        *retval = NULL; /* Lithe doesn't return values, but set to NULL for compatibility */
    }
    
    return 0;
}

void ofi_thread_exit(void *retval) {
    /* In Lithe, contexts just return from their function */
    /* The wrapper function will free itself and return */
    (void)retval; /* Unused, but keep for compatibility */
}
#else
/* Register atexit handler to set cleanup flag */
static void ofi_cleanup_handler(void) {
    ofi_cleanup_in_progress = 1;
}

__attribute__((constructor))
static void ofi_init_cleanup_detection(void) {
    atexit(ofi_cleanup_handler);
}
#endif

#if HAVE_LITHE
int ofi_wait_cond(lithe_condvar_t *cond, lithe_mutex_t *mut, int timeout_ms)
#else
int ofi_wait_cond(pthread_cond_t *cond, pthread_mutex_t *mut, int timeout_ms)
#endif
{
#if HAVE_LITHE
	/* No longer referencing parlib symbols - we don't include parlib.h */
	
	/* Don't use Lithe during cleanup - base scheduler contexts cannot block */
	/* The cleanup flag is set by atexit handler before cleanup runs */
	/* Check this FIRST, before any Lithe operations */
	if (ofi_cleanup_in_progress) {
		/* During cleanup, we can't block - return immediately */
		/* This prevents the "Contexts created by the base scheduler should never need to block!" error */
		return 0; /* Return success to avoid breaking cleanup */
	}
	
	lithe_sched_t *sched = lithe_sched_current();
	lithe_context_t *current_ctx = lithe_context_self();
	
	if (sched != NULL) {
		if (current_ctx != NULL) {
			/* Use Lithe's own concurrency primitives directly */
			/* Wait on Lithe condition variable with Lithe mutex */
			/* The mutex should already be locked by the caller using lithe_mutex_lock */
			if (timeout_ms < 0) {
				/* Wait indefinitely using Lithe's condvar */
				return lithe_condvar_wait(cond, mut);
			} else {
				/* Lithe condvar doesn't support timeout, so wait without timeout */
				/* TODO: Implement timeout using lithe_context_block with timer */
				return lithe_condvar_wait(cond, mut);
			}
		}
	}
	/* If not in Lithe context and not in cleanup, this shouldn't happen */
	/* But if we're in cleanup, return success to avoid blocking */
	if (ofi_cleanup_in_progress) {
		return 0;
	}
	return -1;
#else
	/* Fall back to regular pthread condition variables */
	uint64_t t;
	struct timespec ts;

	if (timeout_ms < 0)
		return pthread_cond_wait(cond, mut);

	t = ofi_get_realtime_ms() + timeout_ms;
	ts.tv_sec = t / 1000;
	ts.tv_nsec = (t % 1000) * 1000000;
	return pthread_cond_timedwait(cond, mut, &ts);
#endif
}

int ofi_mmap_anon_pages(void **memptr, size_t size, int flags)
{
	*memptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
	if (OFI_UNLIKELY(*memptr == MAP_FAILED)) {
		return -errno;
	}
	return FI_SUCCESS;
}

int ofi_unmap_anon_pages(void *memptr, size_t size)
{
	if (munmap(memptr, size)) {
		return -errno;
	}
	return FI_SUCCESS;
}

int ofi_shm_map(struct util_shm *shm, const char *name, size_t size,
		int readonly, void **mapped)
{
	char *fname = 0;
	int i, ret = FI_SUCCESS;
	int flags = O_RDWR | (readonly ? 0 : O_CREAT);
	struct stat mapstat;
	int fname_size = 0;

	*mapped = MAP_FAILED;
	memset(shm, 0, sizeof(*shm));

	fname_size = strlen(name) + 2; /* '/' + %s + trailing 0 */
	fname = calloc(1, fname_size);
	if (!fname)
		return -FI_ENOMEM;

	snprintf(fname, fname_size, "/%s", name);
	shm->name = fname;

	for (i = 0; i < strlen(fname); i++) {
		if (fname[i] == ' ')
			fname[i] = '_';
	}

	FI_DBG(&core_prov, FI_LOG_CORE,
		"Creating shm segment :%s (size: %lu)\n", fname, size);

	shm->shared_fd = shm_open(fname, flags, S_IRUSR | S_IWUSR);
	if (shm->shared_fd < 0) {
		FI_WARN(&core_prov, FI_LOG_CORE, "shm_open failed\n");
		ret = -FI_EINVAL;
		goto failed;
	}

	if (fstat(shm->shared_fd, &mapstat)) {
		FI_WARN(&core_prov, FI_LOG_CORE, "failed to do fstat: %s\n",
			strerror(errno));
		ret = -FI_EINVAL;
		goto failed;
	}

	if (mapstat.st_size == 0) {
		if (ftruncate(shm->shared_fd, size)) {
			FI_WARN(&core_prov, FI_LOG_CORE,
				"ftruncate failed: %s\n", strerror(errno));
			ret = -FI_EINVAL;
			goto failed;
		}
	} else if (mapstat.st_size < size) {
		FI_WARN(&core_prov, FI_LOG_CORE, "shm file too small\n");
		ret = -FI_EINVAL;
		goto failed;
	}

	shm->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_SHARED, shm->shared_fd, 0);
	if (shm->ptr == MAP_FAILED) {
		FI_WARN(&core_prov, FI_LOG_CORE,
			"mmap failed: %s\n", strerror(errno));
		ret = -FI_EINVAL;
		goto failed;
	}

	*mapped = shm->ptr;
	shm->size = size;
	return ret;

failed:
	if (shm->shared_fd >= 0) {
		close(shm->shared_fd);
		shm_unlink(fname);
	}
	if (fname)
		free(fname);
	memset(shm, 0, sizeof(*shm));
	return ret;
}

int ofi_shm_unmap(struct util_shm* shm)
{
	if (shm->ptr && shm->ptr != MAP_FAILED) {
		if (munmap(shm->ptr, shm->size)) {
			FI_WARN(&core_prov, FI_LOG_CORE,
				"munmap failed: %s\n", strerror(errno));
		}
	}

	if (shm->shared_fd)
		close(shm->shared_fd);
	if (shm->name) {
		shm_unlink(shm->name);
		free((void*)shm->name);
	}
	memset(shm, 0, sizeof(*shm));
	return FI_SUCCESS;
}

int ofi_set_thread_affinity(const char *s)
{
#if HAVE_LITHE
	/* CPU affinity is not used with Lithe - Lithe manages CPU scheduling */
	(void)s;
	return -FI_ENOSYS;
#else
	/* Original implementation would go here */
	(void)s;
	return -FI_ENOSYS;
#endif
}

int fi_read_file(const char *dir, const char *file, char *buf, size_t size)
{
	char path[PATH_MAX];
	int len = -1;
	FILE *fp;
	int n;

	if (snprintf(path, sizeof(path), "%s/%s", dir, file) >= (int)sizeof(path))
		return -1;

	fp = fopen(path, "r");
	if (!fp)
		return -1;

	if (fgets(buf, size, fp)) {
		n = strlen(buf);
		if (n > 0 && buf[n - 1] == '\n')
			buf[--n] = '\0';
		len = n;
	}

	fclose(fp);
	return len;
}

struct ofi_pollfds_ctx *ofi_pollfds_get_ctx(struct ofi_pollfds *pfds, int fd)
{
	struct ofi_pollfds_ctx *ctx = NULL;
	int i;

	assert(ofi_genlock_held(&pfds->lock));

	/* 0 is signaling fd */
	for (i = 1; i < pfds->size; i++) {
		ctx = &pfds->ctx[i];
		if (ctx->index >= 0 && ctx->index < pfds->nfds &&
		    pfds->fds[ctx->index].fd == fd)
			return ctx;
	}

	return NULL;
}

struct ofi_pollfds_ctx *ofi_pollfds_alloc_ctx(struct ofi_pollfds *pfds, int fd)
{
	struct ofi_pollfds_ctx *ctx;
	int i;

	assert(ofi_genlock_held(&pfds->lock));
	assert(!ofi_pollfds_get_ctx(pfds, fd));
	/* 0 is signaling fd */
	for (i = 1; i < pfds->size; i++) {
		if (pfds->ctx[i].index == -1) {
			ctx = &pfds->ctx[i];
			goto insert;
		}
	}

	if (ofi_pollfds_grow(pfds, pfds->size + 1))
		return NULL;

	ctx = &pfds->ctx[pfds->nfds];
insert:
	assert(ctx->index < 0);
	ctx->index = pfds->nfds++;
	return ctx;
}
