#ifndef DS_PROFILER_H
#define DS_PROFILER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include <cstdint>
#include <mutex>

namespace godot {

class DSProfilerResult : public RefCounted {
	GDCLASS(DSProfilerResult, RefCounted);

	friend class DSProfiler;

private:
	int64_t compressed_bytes_read = 0;
	int64_t uncompressed_bytes_produced = 0;
	int request_count = 0;
	int64_t physical_read_duration_usec = 0;
	int64_t decompression_duration_usec = -1;
	double logical_throughput_bytes_per_sec = 0.0;
	double physical_throughput_bytes_per_sec = 0.0;
	int error_state = 0;

protected:
	static void _bind_methods();

public:
	int64_t get_compressed_bytes_read() const;
	int64_t get_uncompressed_bytes_produced() const;
	int get_request_count() const;
	int64_t get_physical_read_duration_usec() const;
	int64_t get_decompression_duration_usec() const;
	int get_error_state() const;

	double get_logical_throughput_mbps() const;
	double get_logical_throughput_mibps() const;
	double get_physical_throughput_mbps() const;
	double get_physical_throughput_mibps() const;
};

class DSProfiler : public Object {
	GDCLASS(DSProfiler, Object);

private:
	static DSProfiler *singleton;
	mutable std::mutex profiler_mutex;
	static constexpr size_t MAX_RESULT_HISTORY = 1024;
	Ref<DSProfilerResult> last_result;
	Array result_history;
	double last_throughput_mbps = 0.0;
	double last_requests_per_second = 0.0;
	double last_average_latency_ms = 0.0;
	int last_queue_depth = 0;
	bool last_gpu_decompression_active = false;
	uint64_t last_bytes = 0;
	uint64_t last_duration_us = 0;
	bool performance_monitors_registered = false;

protected:
	static void _bind_methods();

public:
	static DSProfiler *get_singleton();

	DSProfiler();
	~DSProfiler();

	void record_workload(uint64_t p_bytes, uint64_t p_duration_us);
	void record_result(const Ref<DSProfilerResult> &p_result);
	Ref<DSProfilerResult> get_last_result() const;
	Array get_result_history() const;
	double get_last_throughput_mbps() const;
	uint64_t get_last_bytes() const;
	uint64_t get_last_duration_us() const;

	void register_performance_monitors();
	void unregister_performance_monitors();

	double _get_throughput_monitor() const;
	double _get_requests_per_second_monitor() const;
	double _get_average_latency_monitor() const;
	int _get_queue_depth_monitor() const;
	int _get_gpu_decompression_active_monitor() const;
};

} // namespace godot

#endif // DS_PROFILER_H
