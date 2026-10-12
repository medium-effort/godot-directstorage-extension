#ifndef DS_CONTEXT_H
#define DS_CONTEXT_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <cstdint>
#include <algorithm>
#include <deque>
#include <list>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include "ds_profiler.h"
#include "ds_request.h"
#include "../platform/platform_backend.h"

namespace godot {

class DSContext : public Object {
	GDCLASS(DSContext, Object);

public:
	enum InitStatus {
		INIT_STATUS_UNINITIALIZED = 0,
		INIT_STATUS_DIRECTSTORAGE_D3D12 = 1,
		INIT_STATUS_SOFTWARE_FALLBACK = 2,
		INIT_STATUS_FAILED = 3,
	};

private:
	static DSContext *singleton;
	static std::once_flag support_probe_once;
	static bool supported_probe_result;
	std::atomic<bool> initialized{ false };
	std::atomic<InitStatus> init_status{ INIT_STATUS_UNINITIALIZED };
	std::atomic<bool> shut_down{ false };

	uint32_t staging_buffer_size_mb = 128;
	bool disable_gpu_decompression = false;
	uint16_t queue_capacity = 2000;

	PlatformBackend *active_backend = nullptr;
	mutable std::mutex init_mutex;
	mutable std::shared_mutex backend_lifecycle_mutex;
	String initialization_path;
	bool gpu_requests_pending = false;
	uint64_t completion_fence = 0;
	mutable std::mutex context_mutex;
	static constexpr size_t MAX_EXECUTION_REPORTS = 32768;
	Dictionary request_execution_reports;
	std::unordered_map<int64_t, Ref<DSRequest>> tracked_requests;
	std::unordered_set<int64_t> pending_request_ids;
	std::list<int64_t> report_order;
	std::unordered_map<int64_t, std::list<int64_t>::iterator> report_order_map;
	int pending_request_count = 0;
	std::deque<int> pending_batch_request_counts;

	// Async-submit counter cache for the Win32 backend, which exposes no
	// batch counter of its own. Incremented exactly once per successful
	// submit_async() drain on Win32; the fallback path delegates to
	// FallbackBackend::get_async_batches_submitted() directly and never
	// reads this. Guarded by context_mutex. No queue merge; the sync and
	// async queues stay separate.

	uint64_t async_batches_submitted_cache = 0;

	// Async prewarm stream queue. Kept separate from the sync loader path
	// above: sync _load requests must never be stolen by the batch
	// manager's stream flush, and stream requests must never flow through
	// sync flush_batch().
	// Lock discipline: stream_mutex guards stream_queue, stream_tags and
	// next_stream_tag only. It is never held while acquiring context_mutex.
	// The single exception is count_stream_request_terminal(), which runs both
	// inside finalize_completed_reports_locked() (context_mutex held) and
	// standalone; it takes stream_mutex only, so the order is always
	// context_mutex -> stream_mutex, never reversed.
	struct StreamTagInfo {
		uint64_t tag = 0;
		int total = 0; // requests enqueued under this tag
		int completed = 0; // terminal (COMPLETED/FAILED, incl. cancelled)
		int cancelled = 0;
		bool cancel_requested = false;
		bool fixed_total = false;
	};
	mutable std::mutex stream_mutex;
	std::deque<Ref<DSRequest>> stream_queue; // pending, not yet submitted
	std::unordered_map<uint64_t, StreamTagInfo> stream_tags;
	uint64_t next_stream_tag = 1;

	void evict_old_reports_if_needed_locked();
	void remove_report_locked(int64_t p_request_id);
	void finalize_completed_reports_locked();
	void count_stream_request_terminal(const Ref<DSRequest> &p_request);
	bool enqueue_request_internal(Ref<DSRequest> p_request);
	// Stream-flush fast path: same validation + tracking as
	// enqueue_request_internal(), but WITHOUT the per-request
	// collect_completed_results() / finalize_completed_reports_locked()
	// maintenance. flush_stream_async() hoists both once per flush
	// (collect before submit to free batch slots, finalize after),
	// turning up to 256 O(tracked) scans per main-thread frame into one.
	bool enqueue_stream_request_internal(Ref<DSRequest> p_request);
	// Shared validation + tracking for both enqueue paths above.
	// p_skip_maintenance=true selects the stream behavior (no per-request
	// collect/finalize); report bookkeeping is identical either way.
	bool enqueue_request_impl(Ref<DSRequest> p_request, bool p_skip_maintenance);

	bool initialize_ds_locked();
	void shutdown_ds_locked();

protected:
	static void _bind_methods();

public:
	static constexpr uint32_t DEFAULT_STAGING_BUFFER_SIZE_MB = 128;
	static constexpr uint32_t MIN_STAGING_BUFFER_SIZE_MB = 1;
	static constexpr uint32_t MAX_STAGING_BUFFER_SIZE_MB = 2048;
	static constexpr uint16_t DEFAULT_QUEUE_CAPACITY = 2000;
	static constexpr uint16_t MIN_QUEUE_CAPACITY = 128;
	static constexpr uint16_t MAX_QUEUE_CAPACITY = 8192;

	static uint32_t clamp_staging_buffer_size_mb(uint32_t p_size_mb) {
		return std::clamp<uint32_t>(p_size_mb, MIN_STAGING_BUFFER_SIZE_MB, MAX_STAGING_BUFFER_SIZE_MB);
	}
	static uint16_t clamp_queue_capacity(uint32_t p_capacity) {
		return static_cast<uint16_t>(std::clamp<uint32_t>(p_capacity, MIN_QUEUE_CAPACITY, MAX_QUEUE_CAPACITY));
	}

	static DSContext *get_singleton();

	DSContext();
	~DSContext();

	bool is_supported() const;
	bool is_d3d12_backend() const;
	bool is_initialized() const;
	InitStatus get_init_status() const;
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

	bool enqueue_request(Ref<DSRequest> p_request, bool p_from_stream = false);
	int get_pending_request_count() const;
	void flush_batch();
	Error flush_batch_async();
	Error flush_stream_async();
	bool has_stream_pending() const;
	int get_stream_pending_count() const;
	uint64_t prewarm_async(const PackedStringArray &p_paths);
	bool is_prewarm_complete(uint64_t p_tag) const;
	Vector2i get_prewarm_progress(uint64_t p_tag) const;
	void set_prewarm_total(uint64_t p_tag, int p_total);
	void count_stream_terminal_failure(uint64_t p_tag);
	void cancel_prewarm(uint64_t p_tag);
	bool is_tag_cancelled(uint64_t p_tag) const;
	bool is_resource_in_flight(const RID &p_rid) const;
	bool is_batch_complete() const;
	bool has_pending_results() const;
	void collect_completed_results();
	uint64_t get_completion_fence() const;
	bool wait_for_completion(int p_timeout_msec);
	void cancel_requests_with_tag(uint64_t p_tag);
	RID create_destination_texture(const Ref<RDTextureFormat> &p_format, const Ref<RDTextureView> &p_view = Ref<RDTextureView>());
	// GDScript-visible batching counters. No queue merge; the sync and async
	// queues stay separate.
	uint64_t get_async_batches_submitted() const;
	uint64_t get_in_flight_async_batch_count() const;

	// Settings getters/setters
	void set_staging_buffer_size_mb(uint32_t p_size_mb);
	uint32_t get_staging_buffer_size_mb() const;

	void set_disable_gpu_decompression(bool p_disable);
	bool get_disable_gpu_decompression() const;

	void set_queue_capacity(uint16_t p_capacity);
	uint16_t get_queue_capacity() const;
};

} // namespace godot

VARIANT_ENUM_CAST(DSContext::InitStatus);

#endif // DS_CONTEXT_H
