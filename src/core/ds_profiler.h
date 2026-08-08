#ifndef DS_PROFILER_H
#define DS_PROFILER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class DSProfiler : public Object {
	GDCLASS(DSProfiler, Object);

private:
	static DSProfiler *singleton;
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
	double get_last_throughput_mbps() const;
	uint64_t get_last_bytes() const;
	uint64_t get_last_duration_us() const;

	void register_performance_monitors();
};

} // namespace godot

#endif // DS_PROFILER_H
