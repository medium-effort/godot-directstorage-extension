#pragma once

#include <cstdint>

namespace godot {

// Executor handed to the vendored GDeflate codecs so their tile work runs on Godot's
// worker pool instead of on threads the codec creates itself.
//
// Contract (same signature as GDeflate::ParallelForFn): run p_job(p_user, i) for every
// i in [0, p_workers) and return only after all invocations finished. This function
// never falls back to a private thread; when it cannot use the pool it runs the job on
// the calling thread, which is always a correct (single-worker) execution.
void ds_gdeflate_parallel_for(void *p_user, void (*p_job)(void *, uint32_t), uint32_t p_workers);

// Resolves the pool capacity once, on the main thread, at module init. The worker
// count comes from a project setting and a caller of ds_gdeflate_parallel_for can be
// holding a lock a pool thread might need to make progress, so a pool that could be a
// single worker is never waited on.
void ds_gdeflate_init_pool_capacity();

// Effective pool capacity resolved at module init (a configured -1 means all
// logical processors). Used by the shared CPU budget as an upper bound.
int ds_gdeflate_get_pool_capacity();

// Native-test observation point: the worker count a codec requested on the
// most recent dispatch. Not bound to ClassDB.
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
uint32_t ds_get_last_parallel_for_workers_for_testing();
void ds_reset_parallel_for_workers_for_testing();
#endif

} // namespace godot
