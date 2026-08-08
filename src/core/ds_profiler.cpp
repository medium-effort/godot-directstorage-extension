#include "ds_profiler.h"
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/variant/callable.hpp>

namespace godot {

DSProfiler *DSProfiler::singleton = nullptr;

DSProfiler *DSProfiler::get_singleton() {
	return singleton;
}

DSProfiler::DSProfiler() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

DSProfiler::~DSProfiler() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

void DSProfiler::record_workload(uint64_t p_bytes, uint64_t p_duration_us) {
	last_bytes = p_bytes;
	last_duration_us = p_duration_us;

	if (p_duration_us > 0) {
		double seconds = static_cast<double>(p_duration_us) / 1000000.0;
		double megabytes = static_cast<double>(p_bytes) / (1024.0 * 1024.0);
		last_throughput_mbps = megabytes / seconds;
	} else {
		last_throughput_mbps = 0.0;
	}
}

double DSProfiler::get_last_throughput_mbps() const {
	return last_throughput_mbps;
}

uint64_t DSProfiler::get_last_bytes() const {
	return last_bytes;
}

uint64_t DSProfiler::get_last_duration_us() const {
	return last_duration_us;
}

void DSProfiler::register_performance_monitors() {
	Performance *perf = Performance::get_singleton();
	if (perf) {
		Callable callable = Callable(this, "get_last_throughput_mbps");
		perf->add_custom_monitor("directstorage/throughput_mbps", callable);
	}
}

void DSProfiler::_bind_methods() {
	ClassDB::bind_method(D_METHOD("record_workload", "bytes", "duration_us"), &DSProfiler::record_workload);
	ClassDB::bind_method(D_METHOD("get_last_throughput_mbps"), &DSProfiler::get_last_throughput_mbps);
	ClassDB::bind_method(D_METHOD("get_last_bytes"), &DSProfiler::get_last_bytes);
	ClassDB::bind_method(D_METHOD("get_last_duration_us"), &DSProfiler::get_last_duration_us);
	ClassDB::bind_method(D_METHOD("register_performance_monitors"), &DSProfiler::register_performance_monitors);
}

} // namespace godot
