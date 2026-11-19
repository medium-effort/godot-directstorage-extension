// The shared CPU budget is resolved once at module init, so the cross-platform
// unit is the pure mapping. These pin the contract used by all three consumers:
// auto (0) follows the processor count with a ceiling, an explicit value caps
// every path, and Godot's worker-pool capacity is always an upper bound.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_cpu_budget.h"

GDEX_TEST(cpu_budget, auto_follows_processors_with_a_ceiling) {
	// 8 logical CPUs (the common laptop/desktop case) keeps the old budget.
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 8, 8)), static_cast<int64_t>(8));
	// Past 16 logical CPUs auto stops growing: extra fallback workers would
	// mostly contend with the render thread and the engine's own pool.
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 32, 32)), static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 64, 64)), static_cast<int64_t>(16));
	// Degenerate inputs still produce a usable budget.
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 1, 1)), static_cast<int64_t>(1));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 0, 0)), static_cast<int64_t>(1));
}

GDEX_TEST(cpu_budget, explicit_value_caps_every_path) {
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(16, 32, 32)), static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(32, 32, 32)), static_cast<int64_t>(32));
	// Above GDeflate's 31 tile workers + caller, clamp down.
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(64, 32, 32)), static_cast<int64_t>(32));
	// A negative setting is the same as 0 (auto), never a negative budget.
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(-5, 16, 16)), static_cast<int64_t>(16));
}

GDEX_TEST(cpu_budget, worker_pool_capacity_is_an_upper_bound) {
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(32, 32, 8)), static_cast<int64_t>(8));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(0, 32, 4)), static_cast<int64_t>(4));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::ds_resolve_max_cpu_threads(16, 16, 1)), static_cast<int64_t>(1));
}
