#ifndef DS_PROFILER_H
#define DS_PROFILER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>

#include <cstdint>

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
	Ref<DSProfilerResult> last_result;
	Array result_history;
	double last_throughput_mbps = 0.0;
	uint64_t last_bytes = 0;
	uint64_t last_duration_us = 0;

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
};

} // namespace godot

#endif // DS_PROFILER_H
