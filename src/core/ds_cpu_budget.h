/**************************************************************************/
/*  ds_cpu_budget.h                                                         */
/**************************************************************************/
#pragma once

#include <cstdint>

namespace godot {

// Pure mapping from the project setting, hardware concurrency and Godot's
// worker-pool capacity to one budget shared by every DirectStorage CPU path.
// p_setting <= 0 selects auto; p_pool_cap <= 0 means no pool limit.
int ds_resolve_max_cpu_threads(int64_t p_setting, int p_processors, int p_pool_cap);

// Raw project setting (0 = auto). Win32 passes this through to
// DSTORAGE_CONFIGURATION so that 0 keeps DirectStorage's own best guess;
// the resolved budget below is for the extension's own CPU workers.
int64_t ds_get_max_cpu_threads_setting();

// Called once on the main thread at module init, AFTER
// ds_gdeflate_init_pool_capacity(): p_pool_cap is that resolved capacity and
// bounds the budget. Stores a snapshot.
void ds_init_max_cpu_threads(int p_pool_cap);

// Snapshot read by FallbackBackend, GDeflateDecompressor and DSPreloader.
// Before init it returns the legacy ceiling (8) so early construction is safe.
int ds_get_max_cpu_threads();

} // namespace godot
