#ifndef DS_PROFILER_H
#define DS_PROFILER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "ds_frame_window.h"

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
	// Serialises the register/unregister transition. No monitor callable takes it, so it cannot
	// close the Q46 inversion cycle that profiler_mutex closes when the engine is called from
	// inside a transition.
	std::mutex monitor_mutex;
	static constexpr size_t MAX_RESULT_HISTORY = 1024;
	// Plain-data sample: no Godot types, safe to write from any thread.
	struct ProfilerSample {
		int64_t compressed_bytes_read = 0;
		int64_t uncompressed_bytes_produced = 0;
		int request_count = 0;
		int64_t physical_read_duration_usec = 0;
		int64_t decompression_duration_usec = -1;
		double logical_throughput_bytes_per_sec = 0.0;
		double physical_throughput_bytes_per_sec = 0.0;
		int error_state = 0;
	};
	ProfilerSample last_sample;
	bool has_last_sample = false;
	std::vector<ProfilerSample> result_history;
	size_t history_head = 0;
	// Frame-time impact of the current/last streaming window. Fed once per frame
	// by DSRuntime; guarded by profiler_mutex like every other sample.
	DSFrameWindowStats frame_window;
	double last_throughput_mbps = 0.0;
	double last_requests_per_second = 0.0;
	double last_average_latency_ms = 0.0;
	int last_queue_depth = 0;
	bool last_gpu_decompression_active = false;
	uint64_t last_bytes = 0;
	uint64_t last_duration_us = 0;
	bool performance_monitors_registered = false;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// See register_performance_monitors(): the hook is copied under its own mutex and invoked
	// with no extension mutex held, so a parked test can never block a setter or a callable.
	std::mutex monitor_hook_mutex;
	std::function<void()> monitor_registration_hook;
	void _debug_invoke_monitor_registration_hook();
#endif
	void _push_sample_locked(const ProfilerSample &p_sample);
	static Ref<DSProfilerResult> _make_result(const ProfilerSample &p_sample);

protected:
	static void _bind_methods();

public:
	static DSProfiler *get_singleton();

	DSProfiler();
	~DSProfiler();

	void record_workload(uint64_t p_bytes, uint64_t p_duration_us);
	void record_result(const Ref<DSProfilerResult> &p_result);
	// Frame-time window (see DSFrameWindowStats). record_frame() is called every
	// frame; begin/end bracket a streaming load so the before/during means are
	// comparable on the same machine.
	void record_frame(double p_delta_seconds);
	void begin_load_window();
	void end_load_window();
	Dictionary get_frame_window_stats() const;
	Ref<DSProfilerResult> get_last_result() const;
	Array get_result_history() const;
	double get_last_throughput_mbps() const;
	uint64_t get_last_bytes() const;
	uint64_t get_last_duration_us() const;
	void set_queue_depth(int p_depth);
	int get_queue_depth() const;

	void register_performance_monitors();
	void unregister_performance_monitors();

	double _get_throughput_monitor() const;
	double _get_requests_per_second_monitor() const;
	double _get_average_latency_monitor() const;
	int _get_queue_depth_monitor() const;
	int _get_gpu_decompression_active_monitor() const;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seams, never bound in _bind_methods():
	// the hook runs inside register_performance_monitors() immediately before the first engine
	// call, so a test can observe from another thread whether profiler_mutex is held there; the
	// probe try-locks profiler_mutex and releases it again, and must be called from a thread
	// other than the observed one (a same-thread try_lock on a non-recursive mutex is UB).
	void _debug_set_monitor_registration_hook_for_testing(const std::function<void()> &p_hook);
	bool _debug_profiler_mutex_is_free_for_testing();
#endif
};

} // namespace godot

#endif // DS_PROFILER_H
