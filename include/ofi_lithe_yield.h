/*
 * Cooperative yield for lithified libfabric providers.
 * Never call sched_yield(2) when Lithe is enabled.
 */
#ifndef OFI_LITHE_YIELD_H
#define OFI_LITHE_YIELD_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if defined(HAVE_LITHE) && HAVE_LITHE
#include <lithe/lithe.h>
/* parlib_can_cooperatively_yield(). Included explicitly rather than relying on
 * lithe.h to transitively provide it. */
#include <parlib/uthread.h>
#include <sched.h>
struct uthread;
extern __thread struct uthread *current_uthread;
/* PRE-SCHEDULER SAFETY (2026-10-14).
 *
 * lithe_context_yield() -> uthread_yield() -> uth_disable_notifs() dereferences
 * `current_uthread`, which is NULL until parlib has promoted a thread to a
 * vcore. Library constructors run BEFORE that, and this helper is reachable
 * from one: UCX's ucs_init (.init_array) -> ucm_mmap_install ->
 * ucm_bistro_patch -> ucm_bistro_apply_patch_atomic, which contains an
 * UNCONDITIONAL 5 ms grace loop calling this function (bistro.c:88). From a
 * constructor that is a guaranteed NULL dereference.
 *
 * Observed as SIGSEGV in _dl_init under ASan, because ASan's mmap/mprotect
 * interceptors change which bistro patch path is selected. The crash is not
 * ASan-specific -- ASan merely makes the path reachable.
 *
 * So: yield cooperatively when there IS a uthread, fall back to sched_yield
 * when there is not. sched_yield is forbidden ON a vcore, which is exactly the
 * case this branch excludes. */
static inline void ofi_coop_yield(void)
{
	if (parlib_can_cooperatively_yield())
		lithe_context_yield();
	else
		sched_yield();
}
#else
#include <sched.h>
static inline void ofi_coop_yield(void)
{
	sched_yield();
}
#endif

#endif /* OFI_LITHE_YIELD_H */
