#include "ds_profiler.h"
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/variant/callable.hpp>

namespace godot {

DSProfiler *DSProfiler::singleton = nullptr;

namespace {

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

	Ref<DSProfilerResult> result;
	result.instantiate();
	result->compressed_bytes_read = static_cast<int64_t>(metrics.compressed_bytes);
	result->uncompressed_bytes_produced = static_cast<int64_t>(metrics.uncompressed_bytes);
	result->request_count = metrics.request_count;
	result->physical_read_duration_usec = static_cast<int64_t>(p_duration_us);
	result->decompression_duration_usec = metrics.decompression_duration_us;
	result->logical_throughput_bytes_per_sec = p_duration_us > 0 ? static_cast<double>(metrics.uncompressed_bytes) * 1000000.0 / static_cast<double>(p_duration_us) : 0.0;
	result->physical_throughput_bytes_per_sec = p_duration_us > 0 ? static_cast<double>(metrics.compressed_bytes) * 1000000.0 / static_cast<double>(p_duration_us) : 0.0;
	result->error_state = metrics.error_state;
	record_result(result);
}

void DSProfiler::record_result(const Ref<DSProfilerResult> &p_result) {
	if (p_result.is_null()) {
		return;
	}

	last_result = p_result;
	result_history.push_back(p_result);
	last_bytes = p_result->compressed_bytes_read > 0 ? static_cast<uint64_t>(p_result->compressed_bytes_read) : 0;
	last_duration_us = p_result->physical_read_duration_usec > 0 ? static_cast<uint64_t>(p_result->physical_read_duration_usec) : 0;
	last_throughput_mbps = p_result->get_logical_throughput_mbps();
}

Ref<DSProfilerResult> DSProfiler::get_last_result() const {
	return last_result;
}

Array DSProfiler::get_result_history() const {
	return result_history;
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
	ClassDB::register_class<DSProfilerResult>();

	ClassDB::bind_method(D_METHOD("record_workload", "bytes", "duration_us"), &DSProfiler::record_workload);
	ClassDB::bind_method(D_METHOD("record_result", "result"), &DSProfiler::record_result);
	ClassDB::bind_method(D_METHOD("get_last_result"), &DSProfiler::get_last_result);
	ClassDB::bind_method(D_METHOD("get_result_history"), &DSProfiler::get_result_history);
	ClassDB::bind_method(D_METHOD("get_last_throughput_mbps"), &DSProfiler::get_last_throughput_mbps);
	ClassDB::bind_method(D_METHOD("get_last_bytes"), &DSProfiler::get_last_bytes);
	ClassDB::bind_method(D_METHOD("get_last_duration_us"), &DSProfiler::get_last_duration_us);
	ClassDB::bind_method(D_METHOD("register_performance_monitors"), &DSProfiler::register_performance_monitors);
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
