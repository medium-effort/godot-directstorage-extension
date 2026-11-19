#include "ds_cpu_budget.h"
#include "ds_settings.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>

namespace godot {

namespace {

// GDeflate supports 31 tile workers plus the calling thread.
constexpr int kMaxBudget = 32;
// Auto stays below the full core count on very large machines: extra fallback
// workers mostly contend with the render thread and the engine worker pool.
constexpr int kAutoCeiling = 16;
constexpr int kLegacyCeiling = 8;

// Written once on the main thread at module init, before any worker exists;
// read-only afterwards (same discipline as the GDeflate pool capacity).
int g_max_cpu_threads = kLegacyCeiling;

} // namespace

int ds_resolve_max_cpu_threads(int64_t p_setting, int p_processors, int p_pool_cap) {
	int budget = 0;
	if (p_setting > 0) {
		budget = static_cast<int>(std::clamp<int64_t>(p_setting, 1, kMaxBudget));
	} else {
		budget = std::clamp(p_processors, 1, kAutoCeiling);
	}
	if (p_pool_cap > 0) {
		budget = std::min(budget, p_pool_cap);
	}
	return std::clamp(budget, 1, kMaxBudget);
}

int64_t ds_get_max_cpu_threads_setting() {
	if (ProjectSettings *ps = ProjectSettings::get_singleton()) {
		const String key = dsgd_settings::key("max_cpu_threads");
		if (ps->has_setting(key)) {
			return static_cast<int64_t>(ps->get_setting(key, Variant(0)));
		}
	}
	return 0;
}

void ds_init_max_cpu_threads(int p_pool_cap) {
	OS *os = OS::get_singleton();
	const int processors = os != nullptr ? std::max(1, os->get_processor_count()) : 1;
	g_max_cpu_threads = ds_resolve_max_cpu_threads(ds_get_max_cpu_threads_setting(), processors, p_pool_cap);
}

int ds_get_max_cpu_threads() {
	return g_max_cpu_threads;
}

} // namespace godot
