#include "ds_profiler.h"
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/variant/callable.hpp>

namespace godot {

DSProfiler *DSProfiler::singleton = nullptr;

namespace {

constexpr const char *THROUGHPUT_MONITOR = "DirectStorage/Throughput (MB/s)";
constexpr const char *REQUESTS_PER_SECOND_MONITOR = "DirectStorage/Requests/Second";
constexpr const char *AVERAGE_LATENCY_MONITOR = "DirectStorage/Avg Latency (ms)";
constexpr const char *QUEUE_DEPTH_MONITOR = "DirectStorage/Queue Depth";
constexpr const char *GPU_DECOMPRESSION_MONITOR = "DirectStorage/GPU Decompression Active";

struct PendingBatchMetrics {
	bool valid = false;
	uint64_t compressed_bytes = 0;
	uint64_t uncompressed_bytes = 0;
	int request_count = 0;
	int64_t decompression_duration_us = -1;
	int error_state = 0;
};

thread_local PendingBatchMetrics pending_batch_metrics;

} // namespace

// Internal bridge for batch metrics without changing the legacy public API.
void ds_profiler_set_batch_metrics(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes, int p_request_count, int64_t p_decompression_duration_us, int p_error_state) {
	pending_batch_metrics.valid = true;
	pending_batch_metrics.compressed_bytes = p_compressed_bytes;
	pending_batch_metrics.uncompressed_bytes = p_uncompressed_bytes;
	pending_batch_metrics.request_count = p_request_count;
	pending_batch_metrics.decompression_duration_us = p_decompression_duration_us;
	pending_batch_metrics.error_state = p_error_state;
}

DSProfiler *DSProfiler::get_singleton() {
	return singleton;
}

int64_t DSProfilerResult::get_compressed_bytes_read() const {
	return compressed_bytes_read;
}

int64_t DSProfilerResult::get_uncompressed_bytes_produced() const {
	return uncompressed_bytes_produced;
}

int DSProfilerResult::get_request_count() const {
	return request_count;
}

int64_t DSProfilerResult::get_physical_read_duration_usec() const {
	return physical_read_duration_usec;
}

int64_t DSProfilerResult::get_decompression_duration_usec() const {
	return decompression_duration_usec;
}

int DSProfilerResult::get_error_state() const {
	return error_state;
}

double DSProfilerResult::get_logical_throughput_mbps() const {
	return logical_throughput_bytes_per_sec / 1000000.0;
}

double DSProfilerResult::get_logical_throughput_mibps() const {
	return logical_throughput_bytes_per_sec / (1024.0 * 1024.0);
}

double DSProfilerResult::get_physical_throughput_mbps() const {
	return physical_throughput_bytes_per_sec / 1000000.0;
}

double DSProfilerResult::get_physical_throughput_mibps() const {
	return physical_throughput_bytes_per_sec / (1024.0 * 1024.0);
}

DSProfiler::DSProfiler() {
	result_history.reserve(MAX_RESULT_HISTORY);
	if (singleton == nullptr) {
		singleton = this;
		register_performance_monitors();
	}
}

DSProfiler::~DSProfiler() {
	unregister_performance_monitors();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void DSProfiler::record_workload(uint64_t p_bytes, uint64_t p_duration_us) {
	PendingBatchMetrics metrics = pending_batch_metrics;
	pending_batch_metrics = PendingBatchMetrics();
	if (!metrics.valid) {
		// Legacy callers have no separate compressed size, so both byte counts match.
		metrics.compressed_bytes = p_bytes;
		metrics.uncompressed_bytes = p_bytes;
		metrics.request_count = 1;
		metrics.decompression_duration_us = -1;
		metrics.error_state = 0;
	}

	// Build plain sample only: no Godot allocation on the calling (worker) thread.
	ProfilerSample sample;
	sample.compressed_bytes_read = static_cast<int64_t>(metrics.compressed_bytes);
	sample.uncompressed_bytes_produced = static_cast<int64_t>(metrics.uncompressed_bytes);
	sample.request_count = metrics.request_count;
	sample.physical_read_duration_usec = static_cast<int64_t>(p_duration_us);
	sample.decompression_duration_usec = metrics.decompression_duration_us;
	sample.logical_throughput_bytes_per_sec = p_duration_us > 0 ? static_cast<double>(metrics.uncompressed_bytes) * 1000000.0 / static_cast<double>(p_duration_us) : 0.0;
	sample.physical_throughput_bytes_per_sec = p_duration_us > 0 ? static_cast<double>(metrics.compressed_bytes) * 1000000.0 / static_cast<double>(p_duration_us) : 0.0;
	sample.error_state = metrics.error_state;

	std::lock_guard<std::mutex> lock(profiler_mutex);
	_push_sample_locked(sample);
}

void DSProfiler::_push_sample_locked(const ProfilerSample &p_sample) {
	last_sample = p_sample;
	has_last_sample = true;
	if (result_history.size() < MAX_RESULT_HISTORY) {
		result_history.push_back(p_sample);
	} else {
		// Efficient O(1) ring buffer: overwrite oldest entry and advance head.
		// No reallocation or O(N) shift; size remains capped at MAX_RESULT_HISTORY.
		result_history[history_head] = p_sample;
		history_head = (history_head + 1) % MAX_RESULT_HISTORY;
	}
	last_bytes = p_sample.compressed_bytes_read > 0 ? static_cast<uint64_t>(p_sample.compressed_bytes_read) : 0;
	last_duration_us = p_sample.physical_read_duration_usec > 0 ? static_cast<uint64_t>(p_sample.physical_read_duration_usec) : 0;
	last_throughput_mbps = p_sample.logical_throughput_bytes_per_sec / 1000000.0;
	const int request_count = p_sample.request_count > 0 ? p_sample.request_count : 0;
	last_requests_per_second = last_duration_us > 0 ? static_cast<double>(request_count) * 1000000.0 / static_cast<double>(last_duration_us) : 0.0;
	last_average_latency_ms = request_count > 0 ? static_cast<double>(last_duration_us) / static_cast<double>(request_count) / 1000.0 : 0.0;
	// last_queue_depth is intentionally NOT reset here; it is maintained via
	// set_queue_depth() so backend queues can feed pending depth and the
	// Performance monitor returns the last fed value thread-safely.
	last_gpu_decompression_active = p_sample.decompression_duration_usec >= 0;
}

Ref<DSProfilerResult> DSProfiler::_make_result(const ProfilerSample &p_sample) {
	Ref<DSProfilerResult> result;
	result.instantiate();
	result->compressed_bytes_read = p_sample.compressed_bytes_read;
	result->uncompressed_bytes_produced = p_sample.uncompressed_bytes_produced;
	result->request_count = p_sample.request_count;
	result->physical_read_duration_usec = p_sample.physical_read_duration_usec;
	result->decompression_duration_usec = p_sample.decompression_duration_usec;
	result->logical_throughput_bytes_per_sec = p_sample.logical_throughput_bytes_per_sec;
	result->physical_throughput_bytes_per_sec = p_sample.physical_throughput_bytes_per_sec;
	result->error_state = p_sample.error_state;
	return result;
}

void DSProfiler::record_result(const Ref<DSProfilerResult> &p_result) {
	if (p_result.is_null()) {
		return;
	}

	// Copy out plain values first (caller owns the Ref); store only the sample.
	ProfilerSample sample;
	sample.compressed_bytes_read = p_result->compressed_bytes_read;
	sample.uncompressed_bytes_produced = p_result->uncompressed_bytes_produced;
	sample.request_count = p_result->request_count;
	sample.physical_read_duration_usec = p_result->physical_read_duration_usec;
	sample.decompression_duration_usec = p_result->decompression_duration_usec;
	sample.logical_throughput_bytes_per_sec = p_result->logical_throughput_bytes_per_sec;
	sample.physical_throughput_bytes_per_sec = p_result->physical_throughput_bytes_per_sec;
	sample.error_state = p_result->error_state;

	std::lock_guard<std::mutex> lock(profiler_mutex);
	_push_sample_locked(sample);
}

Ref<DSProfilerResult> DSProfiler::get_last_result() const {
	ProfilerSample sample;
	bool valid = false;
	{
		std::lock_guard<std::mutex> lock(profiler_mutex);
		sample = last_sample;
		valid = has_last_sample;
	}
	if (!valid) {
		return Ref<DSProfilerResult>();
	}
	return _make_result(sample);
}

Array DSProfiler::get_result_history() const {
	std::vector<ProfilerSample> snapshot;
	{
		std::lock_guard<std::mutex> lock(profiler_mutex);
		const size_t count = result_history.size();
		snapshot.reserve(count);
		if (count < MAX_RESULT_HISTORY || history_head == 0) {
			snapshot = result_history;
		} else {
			// Reorder ring buffer into chronological order (oldest -> newest).
			for (size_t i = 0; i < MAX_RESULT_HISTORY; ++i) {
				size_t src_idx = (history_head + i) % MAX_RESULT_HISTORY;
				snapshot.push_back(result_history[src_idx]);
			}
		}
	}
	// Build Godot objects outside the lock: zero cross-thread aliasing,
	// no engine allocation while holding profiler_mutex.
	Array ordered;
	ordered.resize(static_cast<int64_t>(snapshot.size()));
	for (size_t i = 0; i < snapshot.size(); ++i) {
		ordered[static_cast<int64_t>(i)] = _make_result(snapshot[i]);
	}
	return ordered;
}

void DSProfiler::set_queue_depth(int p_depth) {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	last_queue_depth = p_depth < 0 ? 0 : p_depth;
}

int DSProfiler::get_queue_depth() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_queue_depth;
}

double DSProfiler::get_last_throughput_mbps() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_throughput_mbps;
}

uint64_t DSProfiler::get_last_bytes() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_bytes;
}

uint64_t DSProfiler::get_last_duration_us() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_duration_us;
}

void DSProfiler::register_performance_monitors() {
	// monitor_mutex serialises the transition; the engine calls below must not be made under
	// profiler_mutex, because every monitor callable takes it (GODOT_QUIRKS.md Q46).
	std::lock_guard<std::mutex> transition(monitor_mutex);
	{
		std::lock_guard<std::mutex> lock(profiler_mutex);
		if (performance_monitors_registered) {
			return;
		}
	}

	Performance *perf = Performance::get_singleton();
	if (perf == nullptr) {
		return;
	}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	_debug_invoke_monitor_registration_hook();
#endif

	perf->add_custom_monitor(THROUGHPUT_MONITOR, Callable(this, "_get_throughput_monitor"));
	perf->add_custom_monitor(REQUESTS_PER_SECOND_MONITOR, Callable(this, "_get_requests_per_second_monitor"));
	perf->add_custom_monitor(AVERAGE_LATENCY_MONITOR, Callable(this, "_get_average_latency_monitor"));
	perf->add_custom_monitor(QUEUE_DEPTH_MONITOR, Callable(this, "_get_queue_depth_monitor"));
	perf->add_custom_monitor(GPU_DECOMPRESSION_MONITOR, Callable(this, "_get_gpu_decompression_active_monitor"));

	// Written after the engine calls succeed: one write point, no rollback branch. The whole
	// transition is serialised by monitor_mutex, and nothing outside these two functions reads
	// the flag, so "true" can never be observed before the monitors exist.
	std::lock_guard<std::mutex> lock(profiler_mutex);
	performance_monitors_registered = true;
}

void DSProfiler::unregister_performance_monitors() {
	std::lock_guard<std::mutex> transition(monitor_mutex);
	{
		std::lock_guard<std::mutex> lock(profiler_mutex);
		if (!performance_monitors_registered) {
			return;
		}
	}

	Performance *perf = Performance::get_singleton();
	if (perf != nullptr) {
		perf->remove_custom_monitor(THROUGHPUT_MONITOR);
		perf->remove_custom_monitor(REQUESTS_PER_SECOND_MONITOR);
		perf->remove_custom_monitor(AVERAGE_LATENCY_MONITOR);
		perf->remove_custom_monitor(QUEUE_DEPTH_MONITOR);
		perf->remove_custom_monitor(GPU_DECOMPRESSION_MONITOR);
	}

	std::lock_guard<std::mutex> lock(profiler_mutex);
	performance_monitors_registered = false;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void DSProfiler::_debug_set_monitor_registration_hook_for_testing(const std::function<void()> &p_hook) {
	std::lock_guard<std::mutex> lock(monitor_hook_mutex);
	monitor_registration_hook = p_hook;
}

bool DSProfiler::_debug_profiler_mutex_is_free_for_testing() {
	if (profiler_mutex.try_lock()) {
		profiler_mutex.unlock();
		return true;
	}
	return false;
}

void DSProfiler::_debug_invoke_monitor_registration_hook() {
	// Copy under the hook mutex and release it before invoking: a parked hook must never block
	// the setter or a monitor callable, and the hook itself runs with no extension mutex held.
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> lock(monitor_hook_mutex);
		hook = monitor_registration_hook;
	}
	if (hook) {
		hook();
	}
}
#endif

double DSProfiler::_get_throughput_monitor() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_throughput_mbps;
}

double DSProfiler::_get_requests_per_second_monitor() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_requests_per_second;
}

double DSProfiler::_get_average_latency_monitor() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_average_latency_ms;
}

int DSProfiler::_get_queue_depth_monitor() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_queue_depth;
}

int DSProfiler::_get_gpu_decompression_active_monitor() const {
	std::lock_guard<std::mutex> lock(profiler_mutex);
	return last_gpu_decompression_active ? 1 : 0;
}

void DSProfiler::_bind_methods() {
	// NOTE: DSProfilerResult is registered in initialize_directstorage_accelerator_module()
	// in register_types.cpp. Do NOT call ClassDB::register_class<DSProfilerResult>() here:
	// doing so re-entrantly mutates ClassDB::classes while register_class<DSProfiler> is
	// mid-execution, which is undefined behaviour and can corrupt the class registry.

	ClassDB::bind_method(D_METHOD("record_workload", "bytes", "duration_us"), &DSProfiler::record_workload);
	ClassDB::bind_method(D_METHOD("record_result", "result"), &DSProfiler::record_result);
	ClassDB::bind_method(D_METHOD("get_last_result"), &DSProfiler::get_last_result);
	ClassDB::bind_method(D_METHOD("get_result_history"), &DSProfiler::get_result_history);
	ClassDB::bind_method(D_METHOD("get_last_throughput_mbps"), &DSProfiler::get_last_throughput_mbps);
	ClassDB::bind_method(D_METHOD("get_last_bytes"), &DSProfiler::get_last_bytes);
	ClassDB::bind_method(D_METHOD("get_last_duration_us"), &DSProfiler::get_last_duration_us);
	ClassDB::bind_method(D_METHOD("set_queue_depth", "depth"), &DSProfiler::set_queue_depth);
	ClassDB::bind_method(D_METHOD("get_queue_depth"), &DSProfiler::get_queue_depth);
	ClassDB::bind_method(D_METHOD("register_performance_monitors"), &DSProfiler::register_performance_monitors);
	ClassDB::bind_method(D_METHOD("unregister_performance_monitors"), &DSProfiler::unregister_performance_monitors);
	ClassDB::bind_method(D_METHOD("_get_throughput_monitor"), &DSProfiler::_get_throughput_monitor);
	ClassDB::bind_method(D_METHOD("_get_requests_per_second_monitor"), &DSProfiler::_get_requests_per_second_monitor);
	ClassDB::bind_method(D_METHOD("_get_average_latency_monitor"), &DSProfiler::_get_average_latency_monitor);
	ClassDB::bind_method(D_METHOD("_get_queue_depth_monitor"), &DSProfiler::_get_queue_depth_monitor);
	ClassDB::bind_method(D_METHOD("_get_gpu_decompression_active_monitor"), &DSProfiler::_get_gpu_decompression_active_monitor);
}

void DSProfilerResult::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_compressed_bytes_read"), &DSProfilerResult::get_compressed_bytes_read);
	ClassDB::bind_method(D_METHOD("get_uncompressed_bytes_produced"), &DSProfilerResult::get_uncompressed_bytes_produced);
	ClassDB::bind_method(D_METHOD("get_request_count"), &DSProfilerResult::get_request_count);
	ClassDB::bind_method(D_METHOD("get_physical_read_duration_usec"), &DSProfilerResult::get_physical_read_duration_usec);
	ClassDB::bind_method(D_METHOD("get_decompression_duration_usec"), &DSProfilerResult::get_decompression_duration_usec);
	ClassDB::bind_method(D_METHOD("get_logical_throughput_mbps"), &DSProfilerResult::get_logical_throughput_mbps);
	ClassDB::bind_method(D_METHOD("get_logical_throughput_mibps"), &DSProfilerResult::get_logical_throughput_mibps);
	ClassDB::bind_method(D_METHOD("get_physical_throughput_mbps"), &DSProfilerResult::get_physical_throughput_mbps);
	ClassDB::bind_method(D_METHOD("get_physical_throughput_mibps"), &DSProfilerResult::get_physical_throughput_mibps);
	ClassDB::bind_method(D_METHOD("get_error_state"), &DSProfilerResult::get_error_state);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "compressed_bytes_read"), "", "get_compressed_bytes_read");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "uncompressed_bytes_produced"), "", "get_uncompressed_bytes_produced");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "request_count"), "", "get_request_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "physical_read_duration_usec"), "", "get_physical_read_duration_usec");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "decompression_duration_usec"), "", "get_decompression_duration_usec");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "logical_throughput_mbps"), "", "get_logical_throughput_mbps");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "physical_throughput_mbps"), "", "get_physical_throughput_mbps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "error_state"), "", "get_error_state");
}

} // namespace godot
