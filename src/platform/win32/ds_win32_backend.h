#ifndef DS_WIN32_BACKEND_H
#define DS_WIN32_BACKEND_H

#include "../platform_backend.h"
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dstorage.h>
#include <wrl/client.h>
#endif

namespace godot {

struct DSTexMipEntryV2;

#ifdef _WIN32
Microsoft::WRL::ComPtr<ID3D12Device> ds_win32_get_d3d12_device();
Microsoft::WRL::ComPtr<ID3D12CommandQueue> ds_win32_get_d3d12_command_queue();
void ds_win32_invalidate_d3d12_device();
#endif

class Win32Backend : public PlatformBackend {
public:
#ifdef _WIN32
	struct GPUTargetTransition {
		Microsoft::WRL::ComPtr<ID3D12Resource> resource;
		D3D12_RESOURCE_STATES original_state = D3D12_RESOURCE_STATE_COMMON;
		uint32_t first_subresource = 0;
		uint32_t subresource_count = 1;
	};
	struct D3D12BarrierContext {
		ID3D12CommandAllocator *cmd_allocator = nullptr;
		ID3D12GraphicsCommandList *cmd_list = nullptr;
		ID3D12Fence *transition_fence = nullptr;
		uint64_t fence_value = 0;
		HANDLE fence_event = nullptr;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> command_queue;
		std::mutex mutex;
	};
#endif

private:
#ifdef _WIN32
	static constexpr int QUEUE_COUNT = 3;
	static constexpr int BATCH_SLOT_COUNT = 2;
	struct BatchSlot {
		HANDLE event = nullptr;
		Vector<Ref<DSRequest>> requests;
		std::vector<std::string> request_names;
		std::vector<DSTORAGE_REQUEST> dstorage_requests;
		std::unordered_map<std::string, Ref<DSRequest>> request_name_map;
		Vector<IDStorageFile *> files;
		Vector<GPUTargetTransition> gpu_transitions;
		// Open-file cache: normalized path -> slot-owned handle. Exported
		// builds funnel ~256 requests per flush through one archive, so
		// opening per request costs hundreds of kernel opens on the calling
		// (main) thread per frame. First open pushes into files (closed at
		// batch completion) and the map; hits reuse either. Guarded by
		// batch_mutex like every other slot member; cleared together with
		// files by close_slot_files() — never cleared or touched elsewhere.
		std::unordered_map<std::wstring, IDStorageFile *> file_cache;
		uint64_t fence_value = 0;
		int completed_count = 0;
		int error_code = 0;
		String error_message;
		bool closing = false;
		bool submitted = false;
		bool completion_ready = false;
		bool is_sync_submission = false;
	};
	struct QueueState {
		IDStorageQueue3 *queue = nullptr;
		HANDLE completion_event = nullptr;
		ID3D12Fence *completion_fence = nullptr;
		uint64_t completion_fence_value = 0;
		uint64_t completed_fence_value = 0;
		std::array<BatchSlot, BATCH_SLOT_COUNT> batch_slots;
	};

	IDStorageFactory *factory = nullptr;
	std::array<QueueState, QUEUE_COUNT> priority_queues;
	// Backend-level open-file cache: normalized path -> handle plus its live
	// owner count. Exported builds funnel a whole batch through one archive,
	// and a per-request OpenFile costs hundreds of kernel opens per frame, so
	// every request in a batch shares one handle.
	//
	// Lifetime: the cache owns the open handle and each caller/slot holds a
	// reference (refs). When the last reference drops - which happens at batch
	// completion - the handle is closed and the entry is erased. A cached
	// handle therefore exists only while a batch is actually using it, which is
	// what lets a replaced file be deleted or renamed between batches; keeping
	// every DMA source open for the life of the context made a replacement
	// report success while the old bytes stayed (issue #0182).
	//
	// Staleness: no stat revalidation is needed and none is done. A path with
	// no entry is opened fresh, and a path with an entry cannot be replaced on
	// disk because the open handle blocks deletion/rename. Nothing is held
	// between batches.
	//
	// Lock order: batch_mutex -> persistent_file_mutex. Never the reverse.
	struct CachedSourceFile {
		IDStorageFile *file = nullptr; // cache-owned; always open while the entry lives
		uint32_t refs = 0; // caller/slot references; cache ownership is implicit
	};
	mutable std::mutex persistent_file_mutex; // const cache-count query locks it
	std::unordered_map<std::wstring, CachedSourceFile> persistent_file_cache;
	HMODULE dstoragecore_module = nullptr;
	HMODULE dstorage_module = nullptr;
	struct BatchMetadata {
		int queue_index = 0;
		int slot_index = 0;
		uint64_t fence_value = 0;
		uint64_t compressed_bytes = 0;
		uint64_t uncompressed_bytes = 0;
		int request_count = 0;
		uint64_t start_time = 0;
		// Per-batch tag accounting for the async prewarm path. Filled by
		// submit_async() only; the sync submit_batch() path leaves this
		// empty. Each entry groups logical requests (chunk children resolve
		// to their parent and count once) by cancellation_tag so
		// get_prewarm_progress(tag) and prewarm_progress signals can
		// attribute batch completion per tag.
		// Carried through completion_queue to the completion thread with the
		// rest of the batch record; one Submit per queue per frame still
		// covers all N textures in the slot.
		struct BatchTagCount {
			uint64_t tag = 0;
			int request_count = 0;
		};
		Vector<BatchTagCount> tag_counts;
	};
	std::deque<BatchMetadata> completion_queue;
	struct ZombieBatchSlot {
		int queue_index = 0;
		uint64_t fence_value = 0;
		Vector<Ref<DSRequest>> requests;
		Vector<IDStorageFile *> files;
		// Path -> handle for the references this zombie owns; released only
		// after the hardware fence retires (same rule as resource guards).
		std::unordered_map<std::wstring, IDStorageFile *> file_cache;
		Vector<GPUTargetTransition> gpu_transitions;
	};
	std::vector<ZombieBatchSlot> zombie_slots;
	HANDLE completion_stop_event = nullptr;
#endif
	std::thread completion_thread;
	std::atomic<bool> accepting_requests{true};
	std::atomic<bool> batch_in_flight{false};
	std::atomic<bool> sync_batch_in_flight{false};
	std::atomic<bool> completion_thread_stop{false};
	std::atomic<int> active_batch_slot{0};
	mutable std::mutex batch_mutex;
	std::condition_variable batch_cv;
	// Result of the last shutdown() drain. False means hardware DMA may still
	// be running; readers must not release device resources in that case.
	// Atomic like the neighbouring flags: the accessor takes no lock.
	std::atomic<bool> last_shutdown_drained{ true };
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam: called immediately after submit_async()/submit_batch()
	// release batch_mutex and before anything is handed to DirectStorage. It
	// exists so a test can hold that instant open while another thread calls
	// cancel_requests_with_tag(); the real interleaving is a scheduler race that
	// cannot be produced on demand. Never bound in _bind_methods().
	std::function<void()> enqueue_window_hook;
#endif

#ifdef _WIN32
	void completion_thread_func();
	bool drain_all_queues(uint32_t p_timeout_ms);
	void poll_batch_status(QueueState &p_queue, BatchSlot &p_slot, uint64_t p_fence_value);
	void poll_batch_status(QueueState &p_queue, Vector<Ref<DSRequest>> &p_requests, uint64_t p_fence_value);
	void complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &p_batch);
	bool has_in_flight_batches_locked() const;
	void update_profiler_queue_depth_locked();
	void close_slot_files(BatchSlot &p_slot);
	void clear_persistent_files();
	IDStorageFile *resolve_source_file(const std::wstring &p_wpath, String &r_error);
	void release_source_file(const std::wstring &p_wpath, IDStorageFile *p_file);
	void register_slot_source_file(BatchSlot &r_slot, const std::wstring &p_wpath, IDStorageFile *p_file);
	// Releases a resolved source-file reference unless the request that owns it
	// was actually staged into a batch slot. enqueue_request has many
	// validation failure paths between resolving the handle and staging the
	// first request; without this, a rejected request would pin the file open.
	class SourceRefGuard {
	public:
		SourceRefGuard(Win32Backend *p_backend, const std::wstring &p_path, IDStorageFile *p_file, bool &r_registered) :
				backend(p_backend), path(p_path), file(p_file), registered(r_registered) {}
		SourceRefGuard(const SourceRefGuard &) = delete;
		SourceRefGuard &operator=(const SourceRefGuard &) = delete;
		~SourceRefGuard() {
			if (file != nullptr && !registered) {
				backend->release_source_file(path, file);
			}
		}

	private:
		Win32Backend *backend = nullptr;
		std::wstring path;
		IDStorageFile *file = nullptr;
		bool &registered;
	};
	static int priority_to_queue_index(DSRequest::DSPriority p_priority);

	D3D12BarrierContext barrier_context;
	bool init_barrier_context(ID3D12Device *p_device, ID3D12CommandQueue *p_queue = nullptr);
	void cleanup_barrier_context();
	bool execute_transitions(const Vector<GPUTargetTransition> &p_transitions, bool p_to_common);
	bool execute_transitions_internal(const Vector<GPUTargetTransition> &p_transitions, bool p_to_common);
	friend void ds_win32_execute_transitions_on_render_thread();
#endif

public:
	Win32Backend() = default;
	~Win32Backend() override { shutdown(); }

	static void apply_dstorage_configuration_once();
	static bool probe_system_support();
	// Candidate directories that may hold dstorage.dll / dstoragecore.dll,
	// in lookup order. The addon bin dir covers the editor and dev runs;
	// the executable directory covers exported games, where Godot's
	// GDExtension exporter places all DLLs flat next to the game binary.
	static PackedStringArray get_runtime_search_dirs();

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	bool initialize(const String &p_path);
	void shutdown() override;
	bool was_drain_clean() const override;

	bool enqueue_request(Ref<DSRequest> p_request) override;
	void submit_batch() override;
	void submit_async() override;
	bool is_batch_complete() const override;
	void collect_completed_results() override;
	void poll_batch_status();
	bool has_pending_results();
	bool collect_completed_batch(uint64_t &r_fence_value, int &r_completed_count, int &r_error_code, String &r_error_message);
	void cancel_requests_with_tag(uint64_t p_tag) override;
	uint64_t get_completion_fence() const;
	bool wait_for_completion(uint64_t p_fence_value, uint32_t p_timeout_msec);
	bool is_resource_in_flight(const RID &p_rid) const override;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam: reproduces the synchronous-flush timeout state (active slot
	// moved into zombie_slots, fence intact) so the in-flight predicates can be
	// asserted deterministically; the real branch needs a >500 ms DMA stall.
	// Never bound in _bind_methods().
	void _debug_zombify_active_batch_for_testing();
	// Test-only seam: number of cached DMA source handles currently held by the
	// backend. Pins the "a handle exists only while a batch owns it" invariant;
	// never bound in _bind_methods().
	int _debug_cached_source_file_count() const;
	// Test-only seam: pins one priority queue's fence counters. The state it
	// reproduces (two queues at different counter values, one of them still in
	// flight) needs two live queues whose DMA has run a different number of
	// times, which cannot be produced on demand. Never bound in _bind_methods().
	void _debug_set_queue_fence_counters_for_testing(int p_queue_index, uint64_t p_completion_value, uint64_t p_completed_value);
	// Test-only seam: installs (or, with an empty callback, clears) the hook that
	// runs inside the batch_mutex-free enqueue window. Never bound in _bind_methods().
	void _debug_set_enqueue_window_hook_for_testing(const std::function<void()> &p_hook);
	// Test-only seam: largest pending enqueue payload across every slot, read
	// WITHOUT batch_mutex. It must read 0 for the whole window the hook above
	// opens: the payload is then owned by the submitting stack frame, so no other
	// thread can observe, mutate or clear it. Never bound in _bind_methods().
	int _debug_pending_enqueue_count() const;
#endif

	static bool is_path_valid(const String &p_path);
	static Dictionary get_drive_info_for_path(const String &p_path);
	static bool validate_dstex_chunk_range(uint64_t p_source_offset, uint64_t p_source_size, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t &r_resolved_offset);
	static void attribute_batch_error_records(Vector<Ref<DSRequest>> &p_requests, const std::unordered_map<std::string, Ref<DSRequest>> &p_name_map, const String &p_failed_request_name, const String &p_queue_error);
	static Ref<DSRequest> get_chunked_child_request(DSRequest *p_parent, int p_index = 0);
	static void rollback_staged_requests(Vector<Ref<DSRequest>> &p_slot_requests,
			std::vector<std::string> &p_slot_request_names,
			std::unordered_map<std::string, Ref<DSRequest>> &p_slot_request_name_map,
			const std::vector<Ref<DSRequest>> &p_staged_children,
			size_t p_failed_index,
			Ref<DSRequest> p_parent
#ifdef _WIN32
			, std::vector<DSTORAGE_REQUEST> *p_dstorage_requests = nullptr
#endif
	);
	static bool build_single_mip_child_request(Ref<DSRequest> p_parent, const DSTexMipEntryV2 &p_mip_entry, Ref<DSRequest> &r_child);
	static bool stage_single_mip_request(Ref<DSRequest> p_parent, Ref<DSRequest> &r_child);
};

} // namespace godot

#endif // DS_WIN32_BACKEND_H
