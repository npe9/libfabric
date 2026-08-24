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
static inline void ofi_coop_yield(void)
{
	lithe_context_yield();
}
#else
#include <sched.h>
static inline void ofi_coop_yield(void)
{
	sched_yield();
}
#endif

#endif /* OFI_LITHE_YIELD_H */
