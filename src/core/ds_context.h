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
#include <functional>
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
	// Result of the last backend shutdown drain. False means DMA may still be
	// active, so teardown must not free device resources. Atomic like its
	// neighbours: the teardown getter takes no lock.
	std::atomic<bool> last_shutdown_clean{ true };

	// Atomic: initialize_ds_locked() runs on whichever thread first needs the
	// context (main, resource-loader worker, or the enqueue path), while the
	// setters are callable from any thread. Plain scalars here were a data race;
	// every access below goes through an explicit .load()/.store().
	std::atomic<uint32_t> staging_buffer_size_mb{ 32 };
	std::atomic<bool> disable_gpu_decompression{ false };
	// Independent of the switch above: keeps GPU decompression enabled while
	// forcing the reference implementation instead of the driver's optimized
	// metacommand, so the two can be compared on the same machine.
	std::atomic<bool> disable_gpu_decompression_metacommand{ false };
	// Reduction-only UMA auto-tune (see initialize_ds_locked): when true and
	// the active adapter is an integrated/UMA GPU, effective staging/queue
	// are clamped down to the UMA caps. Explicit low values pass through
	// untouched; set false to use configured values verbatim.
	std::atomic<bool> auto_tune_for_integrated_gpu{ true };
	// Reduction-only low-VRAM auto-tune (see initialize_ds_locked): when true
	// and the active adapter is discrete, effective staging is capped by
	// dedicated VRAM (32 MiB below 2 GiB, 64 MiB below 8 GiB). Independent of
	// auto_tune_for_integrated_gpu, which keeps its integrated/UMA scope.
	std::atomic<bool> auto_tune_for_low_vram_gpu{ true };
	// Must match the ProjectSetting default registered in register_types.cpp
	// ("dsgd_extension/queue_capacity" = 512); the setting wins at init,
	// this is only the no-setting fallback.
	std::atomic<uint16_t> queue_capacity{ 512 };

	PlatformBackend *active_backend = nullptr;
	mutable std::mutex init_mutex;
	mutable std::shared_mutex backend_lifecycle_mutex;
	String initialization_path;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam: invoked inside initialize_ds_locked() at the point where
	// the backend used to be published (after the object exists, before its
	// initialize() runs). It exists so a test can hold that instant open and
	// read the public getters from another thread; the real window is a
	// scheduler race that cannot be produced on demand. Never bound in
	// _bind_methods().
	std::function<void()> initialization_window_hook;
#endif
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
	static constexpr uint32_t DEFAULT_STAGING_BUFFER_SIZE_MB = 32;
	static constexpr uint32_t MIN_STAGING_BUFFER_SIZE_MB = 1;
	static constexpr uint32_t MAX_STAGING_BUFFER_SIZE_MB = 2048;
	// Staging buffer and queue capacity defaults: these stay fixed, user-overridable
	// baseline settings. Auto-tune is two independent, reduction-only policies:
	// auto_tune_for_integrated_gpu clamps an integrated/UMA adapter's effective
	// staging and queue to UMA_MAX_*; auto_tune_for_low_vram_gpu caps a discrete
	// adapter's staging by dedicated VRAM. Both leave lower user settings untouched.
	static constexpr uint16_t DEFAULT_QUEUE_CAPACITY = 512;
	static constexpr uint16_t MIN_QUEUE_CAPACITY = 128;
	static constexpr uint16_t MAX_QUEUE_CAPACITY = 8192;
	// UMA auto-tune caps (reduction-only, applied at init when
	// auto_tune_for_integrated_gpu is true on an integrated GPU).
	static constexpr uint32_t UMA_MAX_STAGING_BUFFER_SIZE_MB = 64;
	static constexpr uint16_t UMA_MAX_QUEUE_CAPACITY = 256;

	static uint32_t clamp_staging_buffer_size_mb(uint32_t p_size_mb) {
		return std::clamp<uint32_t>(p_size_mb, MIN_STAGING_BUFFER_SIZE_MB, MAX_STAGING_BUFFER_SIZE_MB);
	}
	static uint16_t clamp_queue_capacity(uint32_t p_capacity) {
		return static_cast<uint16_t>(std::clamp<uint32_t>(p_capacity, MIN_QUEUE_CAPACITY, MAX_QUEUE_CAPACITY));
	}

	// Reduction-only staging cap (MiB) for DISCRETE adapters by dedicated VRAM;
	// 0 means "no cap". The caller applies it only when the adapter is available
	// and not integrated. 64 MiB still fits the largest common request (a
	// 4096x4096 RGBA8 mip), so the cap never turns a previously working 4K asset
	// into a load failure on a tier that can render one.
	static uint32_t low_vram_staging_cap_mb(int64_t p_dedicated_vram_mb) {
		if (p_dedicated_vram_mb <= 0) {
			return 0;
		}
		if (p_dedicated_vram_mb < 2048) {
			return 32;
		}
		if (p_dedicated_vram_mb < 8192) {
			return 64;
		}
		return 0;
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
	String get_gpu_decompression_queue_type() const;
	bool is_gpu_decompression_optimized() const;
	// DXGI identity of the exact D3D12 adapter Godot runs on (matched by
	// adapter LUID, so multi-GPU reports the right device). Keys:
	// available, adapter_name, adapter_vendor, vendor_id, device_id,
	// dedicated_vram_mb, shared_system_mb, software_adapter,
	// is_uma, is_cache_coherent_uma, integrated_gpu_guess.
	Dictionary get_gpu_adapter_info() const;
	// Hardware/heuristic verdict: true when D3D12 reports UMA (unified memory
	// architecture) or the adapter reports <1 GiB dedicated VRAM.
	bool is_integrated_gpu() const;
	// Direct D3D12 hardware query: true when D3D12_FEATURE_ARCHITECTURE1
	// reports UMA == TRUE (shared system memory architecture).
	bool is_uma() const;
	Dictionary get_capability_report() const;
	// Installed-runtime identity and feature probes without the GPU queries in
	// get_capability_report(). Returns {} off Windows.
	Dictionary get_runtime_info() const;
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

	// False when the last shutdown_ds() could not drain in-flight GPU work.
	// Teardown code uses this to decide whether device RIDs are safe to free.
	bool was_last_shutdown_clean() const { return last_shutdown_clean.load(); }

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

	void set_disable_gpu_decompression_metacommand(bool p_disable);
	bool get_disable_gpu_decompression_metacommand() const;

	void set_auto_tune_for_integrated_gpu(bool p_enable);
	bool get_auto_tune_for_integrated_gpu() const;

	void set_auto_tune_for_low_vram_gpu(bool p_enable);
	bool get_auto_tune_for_low_vram_gpu() const;

	void set_queue_capacity(uint16_t p_capacity);
	uint16_t get_queue_capacity() const;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam: installs (or clears, with an empty function) the hook that
	// initialize_ds_locked() fires at the publication point. Never bound in
	// _bind_methods().
	void _debug_set_initialization_window_hook_for_testing(const std::function<void()> &p_hook);
	// Test-only seam: true when the four configuration fields are declared as
	// std::atomic. A compile-time type probe, not a runtime observation: this
	// pins the structural half of the invariant that no runtime oracle in this
	// toolchain (no ThreadSanitizer: MSVC plus prebuilt DirectStorage and
	// godot-cpp binaries) can observe. Never bound in _bind_methods().
	bool _debug_config_fields_are_atomic_for_testing() const;
	// Test-only seam: pins the Win32 backend's per-queue completion counters so a
	// fence wait can be stalled on demand (a genuinely never-retiring DMA would
	// need a wedged device). Forwards to
	// Win32Backend::_debug_set_queue_fence_counters_for_testing() under the
	// lifecycle reader lock; a no-op when the backend is not Win32 or the context
	// is down. Every queue must be reset to (0, 0) before shutdown_ds(): a pinned
	// counter above what the real fence will reach makes drain_all_queues() spend
	// its whole budget. Never bound in _bind_methods().
	void _debug_set_backend_fence_counters_for_testing(int p_queue_index, uint64_t p_completion_value, uint64_t p_completed_value);
#endif
};

} // namespace godot

VARIANT_ENUM_CAST(DSContext::InitStatus);

#endif // DS_CONTEXT_H
