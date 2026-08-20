#ifndef DS_CONTEXT_H
#define DS_CONTEXT_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <deque>
#include <mutex>
#include <unordered_map>
#include "ds_profiler.h"
#include "ds_request.h"
#include "../platform/platform_backend.h"

namespace godot {

class DSContext : public Object {
	GDCLASS(DSContext, Object);

private:
	static DSContext *singleton;
	bool initialized = false;

	uint32_t staging_buffer_size_mb = 128;
	bool disable_gpu_decompression = false;
	uint16_t queue_capacity = 2000;

	PlatformBackend *active_backend = nullptr;
	String initialization_path;
	bool gpu_requests_pending = false;
	uint64_t completion_fence = 0;
	mutable std::mutex context_mutex;
	static constexpr size_t MAX_EXECUTION_REPORTS = 1024;
	Dictionary request_execution_reports;
	std::unordered_map<int64_t, Ref<DSRequest>> tracked_requests;
	std::deque<int64_t> report_order;
	int pending_request_count = 0;
	std::deque<int> pending_batch_request_counts;

	void evict_old_reports_if_needed_locked();
	void remove_report_locked(int64_t p_request_id);
	void finalize_completed_reports_locked();

protected:
	static void _bind_methods();

public:
	static constexpr uint32_t DEFAULT_STAGING_BUFFER_SIZE_MB = 128;

	static DSContext *get_singleton();

	DSContext();
	~DSContext();

	bool is_supported() const;
	bool is_d3d12_backend() const;
	bool is_initialized() const;
	String get_active_backend_name() const;
	bool is_gpu_decompression_available() const;
	bool is_gpu_decompression_enabled() const;
	int get_gpu_decompression_support_tier() const;
	Dictionary get_capability_report() const;
	Dictionary get_request_execution_report(Ref<DSRequest> p_request) const;
	DSRequest::DSDestinationType get_actual_destination_type(Ref<DSRequest> p_request) const;
	String get_actual_backend(Ref<DSRequest> p_request) const;
	bool was_gpu_decompression_used(Ref<DSRequest> p_request) const;

	bool is_current_drive_supported() const;
	bool is_path_supported(const String &p_path) const;
	Dictionary get_drive_info(const String &p_path = "res://") const;

	bool initialize_ds();
	Error initialize_ds_for_path(const String &p_path);
	void shutdown_ds();

	bool enqueue_request(Ref<DSRequest> p_request);
	void flush_batch();
	void flush_batch_async();
	bool is_batch_complete() const;
	bool has_pending_results() const;
	void collect_completed_results();
	uint64_t get_completion_fence() const;
	bool wait_for_completion(int p_timeout_msec);
	void cancel_requests_with_tag(uint64_t p_tag);

	// Settings getters/setters
	void set_staging_buffer_size_mb(uint32_t p_size_mb);
	uint32_t get_staging_buffer_size_mb() const;

	void set_disable_gpu_decompression(bool p_disable);
	bool get_disable_gpu_decompression() const;

	void set_queue_capacity(uint16_t p_capacity);
	uint16_t get_queue_capacity() const;
};

} // namespace godot

#endif // DS_CONTEXT_H
