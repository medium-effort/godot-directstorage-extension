// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef DS_WIN32_BACKEND_H
#define DS_WIN32_BACKEND_H

#include "../platform_backend.h"
#include "../../core/ds_request_budget.h"
#include "../../formats/dspkg_format.h"
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
#include <unordered_set>

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
// Drops every process-wide D3D12 reference (device/queue cache, queued barrier
// handoffs) while the engine is alive and the loader lock is NOT held. Must be
// called from module deinit: a ComPtr destroyed during
// DLL_PROCESS_DETACH releases under the loader lock and deadlocks the driver.
void ds_win32_release_static_d3d12_state();

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// M0 spike seam (never bound in _bind_methods()): submits ONE raw
// DSTORAGE_REQUEST with DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION + GDeflate
// into a row range of one subresource, on a private queue, and reports whether
// the runtime accepted and completed it. The staging buffer size must already be
// configured: it is locked (E_DSTORAGE_STAGING_BUFFER_LOCKED) once a queue
// exists, so the test sets dsgd_extension/staging_buffer_size_mb and re-inits
// DSContext instead of resizing here. Delete with the milestone if the runtime
// refuses sub-mip row ranges.
bool ds_win32_debug_probe_texture_region(const RID &p_texture, uint32_t p_subresource,
		uint32_t p_top, uint32_t p_bottom, const String &p_path, uint64_t p_offset,
		uint32_t p_size, uint32_t p_uncompressed_size, String &r_error);
#endif
#endif

class Win32Backend : public PlatformBackend {
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
	// One failure read from IDStorageQueue::RetrieveErrorRecord(). The record is
	// kept as fields and only rendered by format_message(), so attribution can be
	// filled in when the record is matched to a staged request without rewriting
	// text that was already assembled. Re-attributing the same record is
	// idempotent by construction: every accessor assigns, never appends.
	struct QueueErrorInfo {
		bool present = false;
		// False for a cancellation (ERROR_OPERATION_ABORTED). The record is still
		// consumed and logged verbosely, but the batch is not a failure.
		bool fatal = true;
		// Base line from the record itself: HRESULT, command type, system message.
		// For an injected (test) record this is the injected text verbatim.
		String message;
		String failed_request_name;
		// Filename DirectStorage reported on the record. Empty in practice unless
		// the runtime learned it from the source file, so attribution falls back to
		// the staged request's own path.
		String failed_filename;
		// Filled by enrichment: the staged request's file path (only when the
		// record carried no filename) and a one-line payload summary.
		String resolved_file_path;
		String request_details;
		// Renders base line + "[File: ..., RequestName: ...]" + details. The only
		// place the bracket text is assembled, so it appears exactly once.
		String format_message() const;
	};
	// Everything a retiring batch has to say to the engine once batch_mutex is
	// released: the queue failure (if any) plus the resource-guard warnings
	// collected while validating the batch's requests.
	struct BatchReport {
		QueueErrorInfo error;
		std::vector<String> warnings;
	};
	struct QueueState {
		IDStorageQueue3 *queue = nullptr;
		HANDLE completion_event = nullptr;
		// Queue-owned handle from IDStorageQueue::GetErrorEvent(); the backend
		// waits on it but never CloseHandle()s it. RetrieveErrorRecord may only
		// be called once this event has signalled, so it is the only drain
		// trigger and the completion path no longer reads the record itself.
		HANDLE error_event = nullptr;
		ID3D12Fence *completion_fence = nullptr;
		// Both counters are owned by batch_mutex: submit_async()/submit_batch()
		// advance completion_fence_value under it, wait_for_completion() and the
		// completion thread record completed_fence_value under it, and
		// drain_all_queues() reads and writes them through a snapshot taken under it.
		uint64_t completion_fence_value = 0;
		uint64_t completed_fence_value = 0;
		// Failures captured on error_event that have not reached their completing
		// batch yet, one entry per slot. Guarded by batch_mutex. The completion
		// path must consume these: poll_batch_status() treats "no record" as
		// "batch succeeded", so a record dropped here would turn a failed batch
		// into a completed one. A single shared entry was not enough - both slots
		// of a queue can be in flight (the async path pipelines them), and the
		// second failing batch would then evict the first record. Every staged
		// request has a unique name, so a record belongs to exactly one slot and
		// is stashed on that slot's entry.
		std::array<QueueErrorInfo, BATCH_SLOT_COUNT> pending_errors;
		std::array<BatchSlot, BATCH_SLOT_COUNT> batch_slots;
	};

	IDStorageFactory *factory = nullptr;
	std::array<QueueState, QUEUE_COUNT> priority_queues;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Record of the per-queue watermarks the last successful drain left behind.
	// shutdown() zeroes the live counters, so this is the only post-drain observer.
	// Written under batch_mutex. Never bound in _bind_methods().
	uint64_t debug_last_drain_completion[QUEUE_COUNT] = {};
	uint64_t debug_last_drain_completed[QUEUE_COUNT] = {};
#endif
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
		// DSErrorHandler's error generation when this batch was submitted. The completion thread
		// compares it instead of reading a sticky flag, so a clean batch is never labelled failed
		// by an earlier failure and an error that lands during the batch is still attributed to it.
		uint64_t error_sequence = 0;
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
	};
	std::vector<ZombieBatchSlot> zombie_slots;
	// Instance-owned runtime state. These were file-level statics in the .cpp, so
	// a second Win32Backend could overwrite the first one's identity, clear its
	// in-flight chunk bookkeeping, release its resource guards, or drop its cached
	// archive. Every one of them is still serialised by batch_mutex (or by the
	// caller's lock), exactly as its static predecessor was; only the owner changed.
	//
	// The chunk entries hold Godot Refs, so they must be destroyed while the engine
	// is alive: ~Win32Backend runs from the DSContext shutdown path, which is
	// before module deinit, and the drain-timeout path deliberately leaks the whole
	// instance instead of unreffing under the loader lock.
	struct DSChunkedRequestState {
		Ref<DSRequest> parent;
		Vector<Ref<DSRequest>> children;
		Vector<uint64_t> destination_offsets;
	};
	std::unordered_map<DSRequest *, DSChunkedRequestState> chunked_requests;
	std::unordered_map<DSRequest *, DSRequest *> chunk_parents;
	std::unordered_map<DSRequest *, ID3D12Resource *> resource_guards;
	std::unordered_set<std::string> staging_buffer_warnings;
	// Cached .dspkg for the active archive path. get_texture_mip_entries() runs on
	// every texture enqueue; reopening/reparsing the archive per enqueue costs disk
	// I/O plus full entry-table validation. A path switch drops the old package and
	// opens the new one; an open failure clears the cache. Empty path + null package
	// is the "nothing cached" state (the predecessor was a lazily memnew'd pointer).
	// All access happens under dspkg_cache_mutex: callers do not hold batch_mutex
	// when get_texture_mip_entries() runs, and the archive open stays outside the
	// short critical section so it never re-serializes enqueue disk I/O.
	mutable std::mutex dspkg_cache_mutex;
	struct DSPkgCache {
		String path;
		Ref<DSPkg> package;
	};
	DSPkgCache dspkg_cache;
	HANDLE completion_stop_event = nullptr;
#endif
	std::thread completion_thread;
	std::atomic<bool> accepting_requests{true};
	std::atomic<bool> batch_in_flight{false};
	std::atomic<bool> sync_batch_in_flight{false};
	std::atomic<bool> completion_thread_stop{false};
	// Set by the first shutdown(). A second call must not repeat the teardown:
	// ~Win32Backend runs shutdown() again, and after a drain timeout the first call
	// has already detached the completion thread and returned without tearing
	// anything down. Re-armed by initialize().
	std::atomic<bool> shut_down_once{false};
	std::atomic<int> active_batch_slot{0};
	mutable std::mutex batch_mutex;
	std::condition_variable batch_cv;
	// Result of the last shutdown() drain. False means hardware DMA may still
	// be running; readers must not release device resources in that case.
	// Atomic like the neighbouring flags: the accessor takes no lock.
	std::atomic<bool> last_shutdown_drained{ true };
	// Set when the last shutdown drain could not arm a completion event. Only
	// written by drain_all_queues() and read immediately afterwards by shutdown(),
	// both on the shutdown thread; plain bool for that reason. Not a lock-free
	// reader: nothing else reads it.
	bool last_drain_arm_failed = false;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam: called immediately after submit_async()/submit_batch()
	// release batch_mutex and before anything is handed to DirectStorage. It
	// exists so a test can hold that instant open while another thread calls
	// cancel_requests_with_tag(); the real interleaving is a scheduler race that
	// cannot be produced on demand. Never bound in _bind_methods().
	std::function<void()> enqueue_window_hook;
	// Test-only seam: runs at the point drain_all_queues() touches the per-queue
	// completion counters. A test parks there and asks another thread whether
	// batch_mutex is held at that instant. Never bound in _bind_methods().
	std::function<void()> drain_counter_access_hook;
#ifdef _WIN32
	// Test-only seam: substitutes the per-target event-arm call in
	// drain_all_queues(). Returns the HRESULT the real
	// ID3D12Fence::SetEventOnCompletion() would have returned; an empty hook calls
	// the fence directly. It exists because an arm failure needs a device removed
	// between the GetCompletedValue() query and the arm, which cannot be produced on
	// demand. Never bound in _bind_methods().
	std::function<HRESULT(int p_queue_index, ID3D12Fence *p_fence, uint64_t p_value, HANDLE p_event)> drain_arm_hook;
#endif
#endif

#ifdef _WIN32
	void completion_thread_func();
	bool drain_all_queues(uint32_t p_timeout_ms);
	// Samples the per-queue completion fences and records the watermarks a successful drain has
	// verified, under batch_mutex. Callers invoke it only after the drain has decided whether it
	// needs to block.
	void record_drain_watermarks(ID3D12Fence *const *p_fences);
	// Reads the queue's error record and builds its message. Never calls into the
	// engine and never takes a lock of its own: callers hold batch_mutex, and the
	// result is reported by report_queue_error() after that lock is released.
	static QueueErrorInfo read_queue_error(IDStorageQueue *p_queue);
	// Pushes the record to the engine (push_error/report_error, or print_verbose
	// for cancellations). Must be called with no extension mutex held.
	static void report_queue_error(const QueueErrorInfo &p_error);
	// Reports a retiring batch's warnings and queue failure. Callers invoke this
	// after the batch_mutex block that produced the report has closed.
	static void report_batch_outcome(const BatchReport &p_report);
	void handle_queue_error_event(int p_queue_index);
	// Hands a captured record to the batch that will consume it, or returns it for
	// immediate reporting when no batch on that queue is still unfinished or every
	// slot entry is already claimed.
	void stash_queue_error_locked(QueueState &p_queue, const QueueErrorInfo &p_error, QueueErrorInfo *r_immediate);
	bool has_unfinished_batch_on_queue_locked(const QueueState &p_queue) const;
	// Index of the slot that staged the named request, or -1 when no slot on this
	// queue holds it (a record for an already-retired request, or a record that
	// names no request at all). Caller holds batch_mutex.
	int slot_index_for_error_name_locked(const QueueState &p_queue, const String &p_failed_request_name) const;
	// True when this slot staged the request the record names, or when the record
	// names no request at all (nothing to match on). Caller holds batch_mutex.
	bool slot_owns_error_name_locked(const BatchSlot &p_slot, const String &p_failed_request_name) const;
	// True when any slot of the queue still holds the named request, i.e. the
	// record belongs to a later completion. Caller holds batch_mutex.
	bool queue_has_request_name_locked(const QueueState &p_queue, const String &p_failed_request_name) const;
	// Returns false when the slot's fence has not retired; the caller must not
	// retire the slot or release its resources in that case.
	bool poll_batch_status(QueueState &p_queue, BatchSlot &p_slot, int p_slot_index, uint64_t p_fence_value, BatchReport &r_report);
	void complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &p_batch, BatchReport &r_report);
	bool has_in_flight_batches_locked() const;
	void update_profiler_queue_depth_locked();
	// Container helpers own this instance's state only; they are members so one
	// backend can never read or clear another's bookkeeping.
	bool get_texture_mip_entries(const String &p_path, uint64_t p_offset, Vector<DSTexMipEntryV2> &r_entries, DSCompressionFormat *r_container_format = nullptr, int64_t p_single_mip_index = -1);
	static void enrich_queue_error_from_slot(const BatchSlot &p_slot, QueueErrorInfo &r_error);
	static void enrich_queue_error_from_queue_locked(const QueueState &p_queue, QueueErrorInfo &r_error);
	void release_resource_guards(const Vector<Ref<DSRequest>> &p_requests);
	void release_all_resource_guards();
	bool validate_resource_guard(const Ref<DSRequest> &p_request, String &r_error, std::unordered_map<int64_t, ID3D12Resource *> *p_rid_cache = nullptr);
	DSRequest *get_logical_request(DSRequest *p_request) const;
	int count_logical_requests(const Vector<Ref<DSRequest>> &p_requests) const;
	void complete_chunked_requests();
	void close_slot_files(BatchSlot &p_slot);
	// Clears every staging container and per-batch metadata of a slot that has
	// already been harvested/aborted. Caller holds batch_mutex and has already
	// released resource guards and closed the slot's file handles.
	static void clear_completed_slot_locked(BatchSlot &r_slot);
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
#endif

public:
	Win32Backend() = default;
	~Win32Backend() override { shutdown(); }

	static void apply_dstorage_configuration();
	static bool probe_system_support();
	// Candidate directories that may hold dstorage.dll / dstoragecore.dll,
	// in lookup order. The addon bin dir covers the editor and dev runs;
	// the executable directory covers exported games, where Godot's
	// GDExtension exporter places all DLLs flat next to the game binary.
	static PackedStringArray get_runtime_search_dirs();
	// Version and feature information for the DirectStorage runtime installed
	// next to the addon. Reports what is on disk or already loaded; it never
	// loads a module, so a capability report can call it without side effects.
	// Keys: version (String), version_major/minor/build/revision (int),
	// loaded (bool), capabilities (Dictionary: zstd, zstd_probed, gacl,
	// enqueue_requests, configuration2, creator_id).
	static Dictionary get_runtime_info();

	// Whether a batch slot may stage p_add more requests on top of p_staged.
	// A submitted batch consumes two queue operations beyond its requests
	// (EnqueueSignal + EnqueueSetEvent), so one slot may stage at most
	// capacity - 2 requests. Pure arithmetic: no hardware, no locks, unit-testable.
	static bool admits_requests(uint32_t p_staged, uint32_t p_add, uint32_t p_capacity) {
		if (p_capacity < 2) {
			return false;
		}
		return static_cast<uint64_t>(p_staged) + static_cast<uint64_t>(p_add) + 2ULL <= static_cast<uint64_t>(p_capacity);
	}

	// Two slots may be staged at once, but their combined requests and the two
	// signal operations per submitted batch must never exceed the queue's
	// capacity. Halving the admission budget gives each slot capacity/2 - 2
	// requests, so two full slots consume exactly capacity. Tiny queues keep
	// the single-slot path.
	static uint32_t per_slot_admission_capacity(uint32_t p_capacity) {
		return static_cast<uint32_t>(dsgd_budget::per_slot_admission_capacity(p_capacity));
	}
	static bool can_pipeline(uint32_t p_capacity) { return p_capacity >= 8; }

#if defined(_WIN32)
	// Loads one DirectStorage runtime DLL ("dstoragecore.dll" or "dstorage.dll")
	// from p_dir. The only loader for the runtime: the backend and the context's
	// capability report both call it, so a dependency resolves only from the DLL's
	// own directory, the application directory, or the system directories - never
	// from the current directory or a PATH entry, which the bare loader this
	// replaced did search.
	// Returns null on failure. The caller owns the reference and must FreeLibrary
	// it; nothing here changes the module's lifetime.
	static HMODULE load_runtime_module(const String &p_dir, const char *p_dll_name);
	// Releases a runtime DLL handle returned by load_runtime_module(). Serialised with the
	// loader and with apply_dstorage_configuration(); it never changes which instance is
	// considered configured - the load epoch does that.
	static void release_runtime_module(HMODULE p_module);
#endif

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	bool initialize(const String &p_path);
	void shutdown() override;
	bool was_drain_clean() const override;
	Dictionary get_backend_diagnostics() const override;

	bool enqueue_request(Ref<DSRequest> p_request, PlatformBackend::EnqueueOutcome *r_outcome = nullptr) override;
	void submit_batch() override;
	bool submit_async() override;
	bool has_free_batch_slot() const override;
	uint32_t get_in_flight_batch_count() const;
	bool is_batch_complete() const override;
	void collect_completed_results() override;
	void poll_batch_status();
	bool has_pending_results();
	bool collect_completed_batch(uint64_t &r_fence_value, int &r_completed_count, int &r_error_code, String &r_error_message);
	void cancel_requests_with_tag(uint64_t p_tag) override;
	uint64_t get_completion_fence() const override;
	bool wait_for_completion(uint64_t p_fence_value, uint32_t p_timeout_msec) override;
	bool wait_for_priority_completion(int p_priority, uint32_t p_timeout_msec) override;
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
	// Test-only seam: installs (or, with an empty callback, clears) the hook that
	// runs where drain_all_queues() touches the per-queue completion counters.
	// Never bound in _bind_methods().
	void _debug_set_drain_counter_access_hook_for_testing(const std::function<void()> &p_hook);
	// Test-only seam: largest pending enqueue payload across every slot, read
	// WITHOUT batch_mutex. It must read 0 for the whole window the hook above
	// opens: the payload is then owned by the submitting stack frame, so no other
	// thread can observe, mutate or clear it. Never bound in _bind_methods().
	int _debug_pending_enqueue_count() const;
	// Test-only seam: try-lock batch_mutex and release it again; true when it was free. Used to
	// prove the drain-counter access and the queue-error *report* run under/outside batch_mutex.
	// Never bound in _bind_methods().
	bool _debug_batch_mutex_is_free_for_testing();
	// Test-only seam: per-queue counters as the last successful drain left them. shutdown()
	// zeroes the live counters, so this record is the only post-drain observer. Never bound in
	// _bind_methods().
	void _debug_get_last_drain_counters_for_testing(int p_queue_index, uint64_t &r_completion_value, uint64_t &r_completed_value) const;
	// Test-only seam: one queue's completion fence, so a test can drive it to a value the GPU
	// has not reached on its own. Declared with void* so the declaration stays buildable on
	// hosts that have no ID3D12Fence; the test casts under _WIN32. Never bound in _bind_methods().
	void *_debug_get_queue_completion_fence_for_testing(int p_queue_index) const;
	// Test-only seam: live completion threads process-wide. A detached thread that outlived
	// its object is invisible to that object, so the count has to live outside it. Never bound
	// in _bind_methods().
	static int _debug_live_completion_threads_for_testing();
	// Test-only seam: try to take the queue-error drain lock; true when nothing held it. Used to
	// prove process_queue_errors() is serialised with the completion thread's own error drain.
	// Never bound in _bind_methods().
	bool _debug_queue_error_read_can_enter_for_testing();
	// Test-only seam: installs (or, with an empty callback, clears) the hook that
	// runs immediately before a captured queue error is pushed to the engine. A
	// test parks in it and asks another thread whether batch_mutex is free: the
	// report must happen after the lock, never inside it.
	// Never bound in _bind_methods().
	void _debug_set_queue_error_report_hook_for_testing(const std::function<void()> &p_hook);
	// Test-only seam: number of RetrieveErrorRecord() calls made process-wide since
	// the extension was loaded. Stays flat for a batch that reports no failure,
	// which is exactly what the event-driven contract requires. Compare deltas:
	// sibling cases that cancel requests raise it legitimately.
	// Never bound in _bind_methods().
	int _debug_error_record_read_count() const;
	// Test-only seam: installs a queue failure that the SDK cannot be made to
	// produce on demand (the real out-of-band trigger is device removal), so the
	// "report a failure with no batch in flight" path can be executed. When
	// p_failed_request_name is given, the record is attributed exactly like a real
	// RetrieveErrorRecord() result (stashed for the slot that staged that request)
	// instead of landing on the first completing slot. p_fatal=false reproduces the
	// cancellation record the SDK builds for ERROR_OPERATION_ABORTED, which must be
	// consumed but must not fail the batch. Never bound in _bind_methods().
	void _debug_inject_queue_error_for_testing(int p_queue_index, const String &p_message, const String &p_failed_request_name = String(), bool p_fatal = true);
	// Test-only seam: the request names currently staged on one slot, in staging
	// order. Lets a test inject a record that names a real staged request without
	// reaching into the slot's containers. Never bound in _bind_methods().
	PackedStringArray _debug_staged_request_names_for_testing(int p_queue_index, int p_slot_index) const;
	// Test-only seam: marks both slots of one queue as submitted without real
	// DMA, so has_free_batch_slot() can be tested against the same quantifier
	// submit_async() uses. Caller must restore the flags via teardown/shutdown.
	// Never bound in _bind_methods().
	void _debug_mark_queue_slots_submitted_for_testing(int p_queue_index);
	// Test-only seam: attributes a synthetic queue-error record against the given
	// request twice and returns the rendered text. The queue-error event thread
	// and the batch claim can both attribute one record, so attribution must be
	// idempotent; the test asserts each part appears exactly once instead of
	// trusting that the two call sites never overlap. A non-empty filename
	// reproduces a record the runtime filled in itself, which must win over the
	// request's path. Never bound in _bind_methods().
	static String _debug_render_queue_error_attributed_twice_for_testing(const String &p_failed_request_name,
			const String &p_base_message, const Ref<DSRequest> &p_request, const String &p_failed_filename = String());
	// Test-only seam: how many times DStorageSetConfiguration has actually been
	// applied. The counter is per loaded runtime instance, not per process, so a
	// second load cycle must raise it again; a process-wide latch did not. Never
	// bound in _bind_methods().
	static int _debug_dstorage_config_apply_count();
	// Test-only seam: the process-wide load epoch. It advances only when the
	// own-reference count goes 0 -> 1, i.e. a genuinely new runtime instance
	// cycle. Lets the config tests assert one apply per new instance instead of
	// assuming a full unload/reload in a shared process. Never bound in
	// _bind_methods().
	static uint64_t _debug_dstorage_load_epoch();
	// Test-only seam: the byte count last handed to
	// IDStorageFactory::SetStagingBufferSize (0 before the first successful
	// init). The factory exposes no getter, so this is the only way to prove the
	// effective setting actually reached the runtime. Never bound in
	// _bind_methods().
	static int64_t _debug_last_applied_staging_bytes();
	// Test-only seam: path and search flags of the most recent runtime-DLL load
	// attempt, failed attempts included, so the hardened search policy can be
	// pinned without a second copy of the runtime loaded into the process.
	// Never bound in _bind_methods().
	static void _debug_last_runtime_module_load_for_testing(String &r_path, uint32_t &r_flags);
	// Test-only seam: inserts one synthetic parent/child chunk pair and one
	// synthetic (null) resource guard, so instance isolation can be asserted
	// without staging a real multi-mip GPU request. Never bound in _bind_methods().
	void _debug_seed_instance_state_for_testing();
	// Test-only seam: this instance's container sizes. Pre-conversion these read
	// the file-level statics, which is exactly what the isolation test
	// discriminates on. Never bound in _bind_methods().
	Dictionary _debug_instance_state_for_testing() const;
	// Test-only seam: runs the .dstex/.dspkg mip parser without a live queue or
	// device. The parser is pure file I/O + validation, so this stays
	// headless-safe and lets the v3/v5 guard suite pin forged headers.
	// Never bound in _bind_methods().
#ifdef _WIN32
	// Windows-only: get_texture_mip_entries() is declared (and defined) only in
	// the Win32 build. This inline body is parsed on every platform, so it needs
	// the same guard as the declaration instead of inheriting the DEBUG_ENABLED
	// block it sits in.
	bool _debug_get_texture_mip_entries_for_testing(const String &p_path, uint64_t p_offset, Vector<DSTexMipEntryV2> &r_entries, DSCompressionFormat *r_container_format = nullptr) {
		return get_texture_mip_entries(p_path, p_offset, r_entries, r_container_format);
	}
#endif
	// Test-only seam: runs the standalone-.dstex chunk splitter without a live
	// queue or device, so the per-mip split can be measured headlessly. Empty on
	// rejection, otherwise source_offsets / source_sizes / destination_offsets /
	// destination_sizes (PackedInt64Array), one element per split chunk.
	// Never bound in _bind_methods().
	static Dictionary _debug_get_dstex_mip_chunks_for_testing(const String &p_path, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t p_capacity);

	// Test-only seam: admission/observability counters for the queue contract.
	// rejected_admissions counts batches refused for want of operation budget;
	// peak_staged_ops is the high-water mark of (requests + signal + event) in a
	// slot. Never bound in _bind_methods().
	static Dictionary _debug_get_queue_admission_stats_for_testing();

#ifdef _WIN32
	// Test-only seam: replaces the four capacity-refusal sentences so a test can
	// produce a retryable refusal whose text no longer matches any legacy
	// classifier. Empty restores the defaults. The classification is unaffected,
	// which is the point: the retry decision must not depend on the text.
	// Never bound in _bind_methods().
	static void _set_capacity_reject_text_for_testing(const String &p_text);
	// Test-only seam: installs (or, with an empty callback, clears) the hook that
	// substitutes drain_all_queues()'s per-target SetEventOnCompletion() call, so a
	// test can make an arm fail on a live device. Never bound in _bind_methods().
	void _debug_set_drain_arm_hook_for_testing(const std::function<HRESULT(int, ID3D12Fence *, uint64_t, HANDLE)> &p_hook);
#endif
#endif

	// Media class of the volume that holds an asset path. Decides the
	// DirectStorage I/O mode: rotational media reads through the OS cache
	// (ForceFileBuffering), flash keeps the default unbuffered path.
	enum class StorageMediaClass {
		Unknown,
		Flash,
		Rotational,
	};
	// Pure classification (no device access), kept separate so it is
	// unit-testable: NVMe short-circuits to flash, a known seek-penalty result
	// is authoritative, and TRIM is only a corroborating flash signal.
	static StorageMediaClass classify_storage_media(bool p_is_nvme, bool p_seek_penalty_known,
			bool p_incurs_seek_penalty, bool p_trim_known, bool p_trim_enabled);

	static bool is_path_valid(const String &p_path);
	static Dictionary get_drive_info_for_path(const String &p_path);
	static bool validate_dstex_chunk_range(uint64_t p_source_offset, uint64_t p_source_size, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t &r_resolved_offset);
	static void attribute_batch_error_records(Vector<Ref<DSRequest>> &p_requests, const std::unordered_map<std::string, Ref<DSRequest>> &p_name_map, const String &p_failed_request_name, const String &p_queue_error);
	Ref<DSRequest> get_chunked_child_request(DSRequest *p_parent, int p_index = 0);
	void rollback_staged_requests(Vector<Ref<DSRequest>> &p_slot_requests,
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
	bool stage_single_mip_request(Ref<DSRequest> p_parent, Ref<DSRequest> &r_child);
};

} // namespace godot

#endif // DS_WIN32_BACKEND_H
