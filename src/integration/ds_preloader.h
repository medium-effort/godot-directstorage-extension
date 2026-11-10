#ifndef DS_PRELOADER_H
#define DS_PRELOADER_H

// Async prewarm core.
//
// Fills the gap between DSContext's tag/stream-queue plumbing
// (stream_queue + prewarm_async/progress/cancel in ds_context.h/.cpp) and the
// texture cache: engine-path remap resolution, ResourceCache dedup, stream
// enqueue, and render-thread-gated completion -> texture -> store.
//
// Design constraints (do not relax):
// - Public API accepts ENGINE paths only (.png/.jpg/.webp). The
//   .import -> .godot/imported/*.dstex remap and the textures.dspkg
//   lookup are resolved INTERNALLY. There is intentionally no public
//   load-a-.dstex entry point.
// - Completed payloads keep their v2 D3D12 footprint metadata
//   (mip_offsets/sizes/uncompressed/row_pitch) from enqueue through to the
//   build step ("v2 footprint passthrough"); the build unpads rows via
//   DSTex::unpad_footprint_mip exactly like the sync loader CPU path.
// - Texture OBJECT construction is ImageTexture-only on the memory path
//   (worker-safe by construction, no RenderingDevice thread affinity):
//   build_texture() produces ImageTexture::create_from_image() on every
//   thread. The GPU path (prewarm_gpu_async, D3D12 only) allocates
//   destination textures on the render thread and wraps them as Texture2DRD
//   on completion; GPU entries are never built on worker threads.
//   as_image_texture sources take the ImageTexture path on both paths.
// - Each enqueued DSRequest Ref is held in the pending table until its status
//   is finished (COMPLETED/FAILED, including cancelled) and poll_completed()
//   has consumed it, so completion threads can never touch a freed request.
// - Tag accounting relies on DSContext: prewarm_async() issues a tag with an
//   EMPTY path list (total 0), then every stream enqueue bumps that tag's
//   total by exactly one, so total always equals tracked request count.
//   Requests are EXPECTED to be counted finished exactly once by the context
//   (flush_stream_async reject path / finalize_completed_reports_locked /
//   cancel_prewarm). This class never touches tag counters directly.
// - GDScript reachability: this class is deliberately NOT a registered
//   Godot class. The GDScript-visible entry points live on the
//   DSTextureCache singleton (prewarm_async / prewarm_gpu_async /
//   pump_gpu_prewarm / poll_completed) and delegate here; tag
//   progress/cancel stay on DSContext's existing bindings.

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace godot {

class Texture2D;
class RenderingDevice;
class DSContext;
class DSTextureCache;

class DSPreloader {
public:
	// Resolved prewarm payload for ONE engine texture path. Keeps every field
	// the completion build needs so v2 footprint metadata survives the
	// enqueue -> backend -> completion round-trip (passthrough).
	struct Payload {
		String engine_path; // public cache key A (what scenes load)
		String payload_key; // cache key B (.dstex res path or dspkg entry path)
		bool in_dspkg = false;
		String file_path; // physical file handed to DSRequest (dspkg or dstex)
		uint32_t width = 0;
		uint32_t height = 0;
		// NPOT display size (Phase 3 item 2). 0 = same as storage; the build
		// step applies RenderingServer::texture_set_size_override when set.
		uint32_t logical_width = 0;
		uint32_t logical_height = 0;
		uint32_t pixel_format = 0;
		bool is_srgb = false;
		uint32_t mip_count = 0;
		bool is_v2 = false;
		bool as_image_texture = false;
		// Payload codec as declared by the container (DSCompressionFormat).
		// Defaults to GDeflate, which is what every v1/v2 file carries.
		uint8_t compression_format = static_cast<uint8_t>(DSCompressionFormat::GDEFLATE);
		uint64_t data_offset = 0; // v1 single-request start
		uint64_t compressed_size = 0; // v1 single-request span
		uint64_t original_size = 0; // v1 single-request output
		PackedInt64Array mip_offsets; // v2 per-mip archive/file offsets
		PackedInt32Array mip_sizes; // v2 per-mip compressed sizes
		PackedInt32Array mip_uncompressed_sizes; // v2 per-mip footprint sizes
		PackedInt32Array mip_row_pitches; // v2 per-mip 256B pitches
	};

	// Header cache entry for one .dstex file. Plain struct — no Dictionary,
	// Packed* containers — so cache hits serve with zero Variant/Godot-
	// container allocation. Fixed 16-slot mip tables
	// (DSTEX_MAX_MIP_COUNT); only [0, mip_count) are valid. Freshness key:
	// dstex res path + mtime + file_len + generation. Entries with
	// open_failed=true are failure entries (open failed or validation
	// rejected the file): repeat prewarms serve the failure with one stat
	// and zero opens. Missing-file failure entries store mtime 0, so a file
	// created after the miss carries a new mtime and re-opens — only
	// shutdown()/rescan_archive() (generation bump + clear) drops
	// same-mtime failure entries, mirroring the archive-miss contract.
	struct PayloadHeader {
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t logical_width = 0;
		uint32_t logical_height = 0;
		uint32_t pixel_format = 0;
		uint32_t mip_count = 0;
		uint32_t flags = 0;
		uint16_t version = 0;
		uint8_t compression_format = static_cast<uint8_t>(DSCompressionFormat::GDEFLATE);
		uint64_t original_size = 0;
		uint64_t compressed_size = 0;
		uint64_t mtime = 0; // FileAccess::get_modified_time at fill (0 = unstatable)
		uint64_t file_len = 0; // FileAccess::get_length at fill (0 = unknown/missing)
		uint64_t generation = 0; // header_cache_generation at publish
		bool open_failed = false; // true = failure entry, served without opening
		int64_t mip_offsets[16] = {};
		int32_t mip_sizes[16] = {};
		int32_t mip_uncompressed_sizes[16] = {};
		int32_t mip_row_pitches[16] = {};
	};



	// Resolve + dedup + enqueue. Returns the DSContext stream tag, or 0 when
	// DSContext is unavailable. Never blocks: requests sit in stream_queue
	// until the batch manager drains them via flush_stream_async().
	static uint64_t prewarm_async(const PackedStringArray &p_paths);

	// GPU prewarm: same resolve/dedup/tag contract as prewarm_async, but
	// GPU-eligible payloads (is_gpu_prewarm_eligible, D3D12 + GPU
	// decompression active) are allocated as RenderingDevice textures on
	// the render thread (one call_on_render_thread per chunk of
	// p_chunk_size) and filled by DirectStorage DMA straight into VRAM.
	// Ineligible payloads take the memory path under the SAME tag. When the
	// GPU path is unavailable (or on integrated/UMA GPUs when
	// auto_tune_for_integrated_gpu is enabled), everything automatically
	// degrades to the memory path, eliminating loading-screen hitches and
	// making the call safe anywhere prewarm_async is (including headless CI).
	// Completion wraps Texture2DRD on the main/render thread only; worker
	// poll_completed() calls skip GPU entries and leave them pending.
	// Returns the DSContext stream tag, or 0 when DSContext is unavailable.
	static uint64_t prewarm_gpu_async(const PackedStringArray &p_paths, int p_chunk_size = 16, bool p_prefer_gpu = true);

	// True for payloads the GPU path accepts: v2 footprint metadata,
	// worker-safe exclusion (as_image_texture == false), and a format the GPU
	// path uploads - an sRGB-pairable color format, or a linear-only block
	// format that carries 3D material data (BC4/BC5). Uncompressed and float
	// data formats (R8/RG8/RGB8, the R16 family, floats, BC6H) stay on the
	// memory path. Non-multiple-of-4 payloads are eligible; the GPU wrap reports
	// their padded storage size (GODOT_QUIRKS.md Q79), so as_image_texture is
	// the documented way to get authoring dimensions.
	static bool is_gpu_prewarm_eligible(const Payload &p_payload);

	// True when a GPU prewarm can actually run: initialized D3D12 backend
	// with GPU decompression enabled and a live RenderingDevice.
	static bool is_gpu_prewarm_available();

	// Main-thread pump: posts at most one pending GPU chunk to the render
	// thread per call; no-op while idle, in flight, or GPU-unavailable.
	// Called once per frame by the batch manager; also kicked once from
	// prewarm_gpu_async so the first chunk starts without a manager tick.
	static void pump_gpu_prewarm();

	// Build textures for terminal requests and store them.
	// p_tag == 0 consumes pending entries for ALL tags, otherwise only the
	// given tag. Returns the number of textures stored into DSTextureCache
	// (+ engine ResourceCache residency via take_over_path) by this call.
	// Intended to be called from the main thread each frame while a prewarm
	// is in flight (the batch manager calls it from its per-frame hook).
	// When p_max_items > 0, at most p_max_items textures are processed in this call.
	// When p_max_usec > 0, processing yields as soon as elapsed microseconds exceed the budget.
	// Defaults to a 4000 us slice so a caller that passes nothing cannot build a
	// 64 MiB texture inline (~20-27 ms measured); pass 0 explicitly for the legacy
	// unbounded behaviour. CPU entries that do not fit a non-zero budget are built
	// on WorkerThreadPool instead of the calling thread.
	static int poll_completed(uint64_t p_tag = 0, int p_max_items = 0, uint32_t p_max_usec = 4000);

	// Diagnostic: number of tracked textures with at least one
	// unfinished request. p_tag == 0 counts all tags.
	static int get_pending_prewarm_count(uint64_t p_tag = 0);

	// Diagnostic: number of tracked textures still residing in the preloader's
	// pending state awaiting poll_completed(). p_tag == 0 counts all tags.
	static int get_unbuilt_prewarm_count(uint64_t p_tag = 0);
	static int get_unbuilt_cpu_prewarm_count(uint64_t p_tag = 0);
	static int get_unbuilt_gpu_prewarm_count(uint64_t p_tag = 0);

	// Release all held Refs + the archive handle. Called from
	// ~DSTextureCache (module teardown) so no Ref outlives Godot.
	static void shutdown();

	// Invalidate the cached archive-path resolution (positive or negative)
	// and bump the archive generation. Called internally by shutdown() and
	// must also be called by whatever code successfully (re)creates or
	// re-imports textures.dspkg, otherwise a cached miss keeps serving
	// false-fast until the next shutdown. C++-internal only: this class is
	// deliberately not a registered Godot class, so neither knob reaches
	// GDScript.
	static void rescan_archive();
	static uint64_t get_archive_generation();

	// True for engine authoring extensions this preloader accepts.
	static bool is_engine_source_path(const String &p_path);

	// Check if a texture path (engine path or payload key) is currently in-flight in the prewarm stream.
	static bool is_texture_pending(const String &p_path);

	// Await completion of a pending texture in-flight in the prewarm stream (e.g. from ResourceLoader worker threads).
	// On render thread, pumps completions immediately to avoid deadlocks. On worker threads, waits on condition variable.
	static Ref<Texture2D> await_pending_texture(const String &p_path, uint32_t p_timeout_ms = 3000);

	// Check if a request has reached a terminal state (completed or retired from failure/cancellation).
	// Bounded fallback timeout (default 5.0s) protects against GPU hangs / TDR.
	static bool is_terminal(const Ref<DSRequest> &p_request, uint64_t p_timeout_usec = 5000000ULL);

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only: hands the native suite the tracked GPU prewarm's destination RID
	// and its parent request, so the in-flight window can be asserted directly.
	// Never bound in _bind_methods(): the GDScript surface is unchanged.
	static RID _get_gpu_prewarm_rid_for_testing(uint64_t p_tag);
	static Ref<DSRequest> _get_gpu_prewarm_request_for_testing(uint64_t p_tag);
#endif

	friend struct PreloaderUnpackTest;
	friend struct PrewarmBudgetTest;

private:
	struct Tracked {
		uint64_t tag = 0;
		Payload meta;
		Vector<Ref<DSRequest>> requests;
		// GPU-path fields: destination texture allocated on the render
		// thread. is_gpu entries skip the memory build; completion wraps
		// gpu_tex_rid as Texture2DRD. gpu_failed drops the entry (sync
		// load remains available) and orphans the RID for render-thread
		// release when one was allocated.
		RID gpu_tex_rid;
		bool is_gpu = false;
		bool gpu_failed = false;
		// Set while this CPU entry is being built by an offloaded worker task.
		// The entry stays in `pending` (so the unbuilt counts stay honest) but
		// the tag scan and every poll skip it, so it cannot be taken twice.
		bool build_in_flight = false;
	};

	// One CPU build handed to WorkerThreadPool because it did not fit the
	// calling thread's slice. Owned by State::build_jobs until a poll or
	// shutdown reaps the finished task. The entry keeps the request Refs alive
	// while the worker builds, so a completion thread can never free them
	// mid-build.
	struct BuildJob {
		int64_t id = 0;
		Tracked entry;
		uint64_t generation = 0;
		std::atomic<bool> done{false};
	};

	// One GPU chunk: pending-table ids + payloads, allocated + enqueued +
	// flush-attempted inside a single render-thread callback so backend
	// destination queries run inline (zero cross-thread rendezvous).
	struct GpuChunkItem {
		int64_t pending_id = 0;
		Payload meta;
	};
	struct GpuChunk {
		uint64_t tag = 0;
		std::vector<GpuChunkItem> items;
	};

	struct State {
		std::mutex mutex;
		std::condition_variable pending_cv;
		// Offloaded CPU builds: WorkerThreadPool task id -> owned job.
		// in_flight_builds is the drain counter; never call
		// WorkerThreadPool::is_task_completed() after wait_for_task_completion()
		// (the id is retired and the call errors with "Invalid Task ID").
		std::unordered_map<int64_t, BuildJob *> build_jobs;
		int in_flight_builds = 0;
		// Incremented by shutdown(); a job captured an older generation must not
		// store into the cache of the next session (teardown of one test's cache
		// must not poison the next test's offloaded builds).
		uint64_t shutdown_generation = 0;
		std::unordered_map<int64_t, Tracked> pending;
		HashMap<String, int> pending_paths;
		int64_t next_id = 1;
		std::mutex archive_mutex;
		String archive_path;
		Ref<DSPkg> archive;
		// Cached result of the last archive-path resolution, served under
		// archive_mutex. Caches BOTH outcomes: a found path (saves the
		// 1-probe res:// hit per path while the archive stays open) and
		// the empty miss (false-fast: zero FileAccess::file_exists probes
		// on repeat unresolvable-path prewarms). Invalidated by
		// rescan_archive() / shutdown() via resolved_archive_probed=false
		// + generation bump.
		String resolved_archive_path;
		bool resolved_archive_probed = false;
		uint64_t archive_generation = 0;
		// Enqueue fast-path header cache (positive entries + failure
		// entries): dstex res path -> PayloadHeader, served with zero
		// FileAccess::open on mtime match. Positive hits skip the open
		// entirely (1 stat); failure hits skip the re-open. Guarded by
		// State::mutex (already present): first-publisher-wins per
		// generation. Invalidated by invalidate_header_cache() via
		// generation bump + clear (called from shutdown()/rescan_archive()).
		HashMap<String, PayloadHeader> header_cache;
		uint64_t header_cache_generation = 0;

		// GPU prewarm chunk queue: pending chunks awaiting one
		// render-thread callback each (pump_gpu_prewarm posts at most one
		// per frame). gpu_current holds the chunk being processed;
		// gpu_chunk_in_flight prevents pile-up. gpu_orphans holds
		// destination RIDs whose requests failed, released on the render
		// thread (free_rid is render-thread-only).
		std::deque<GpuChunk> gpu_chunks;
		GpuChunk gpu_current;
		bool gpu_current_valid = false;
		bool gpu_chunk_in_flight = false;
		std::vector<RID> gpu_orphans;
		bool gpu_orphan_free_posted = false;
	};

	static State &state();

	static bool resolve_payload(const String &p_engine_path, Payload &r_out);
	static String resolve_archive_path();
	static bool fill_from_dspkg(const String &p_engine_path, Payload &r_out);
	static bool fill_from_import_remap(const String &p_engine_path, Payload &r_out);
	static bool fill_from_dstex_file(const String &p_engine_path, const String &p_dstex_path, Payload &r_out);
	static bool validate_payload(const Payload &p_payload);



	// C++-internal header-cache plumbing (no ClassDB bindings by design —
	// this class is deliberately not a registered Godot class).
	static void invalidate_header_cache();
	// Tri-state lookup: returns true on a generation+mtime match for EITHER
	// a positive entry (r_out.open_failed=false, build from it) or a
	// failure entry (r_out.open_failed=true, fail without opening).
	// Returns false on miss (no entry, stale generation, or mtime drift).
	static bool try_get_cached_header(const String &p_dstex_path, PayloadHeader &r_out);
	static void publish_header_cache(const String &p_dstex_path, const PayloadHeader &p_header);
	// Single-open single-bulk miss path: one FileAccess::open + ONE
	// get_buffer(header + largest possible mip table, 16 V2 entries) with
	// the entries parsed from that same buffer via memcpy — no second I/O,
	// no Dictionary/Packed* churn until Payload construction. Validates
	// magic/version/1..16 mips/bounds exactly as DSTex::read_header does
	// today. On failure returns false with r_out carrying the failure
	// entry (mtime/file_len observed, open_failed=true) for the caller to
	// publish, so repeats skip the re-open.
	static bool read_header_uncached(const String &p_dstex_path, PayloadHeader &r_out);
	// Batch warm helper: best-effort fill of positive + failure entries
	// for known dstex paths in one pass (one stat + at most one open per
	// uncached path, duplicates skipped). C++-internal only; deliberately
	// NOT wired into prewarm_async(), whose package hits must stay
	// zero-stat — prefetching every engine path upfront would add a stat
	// per package-served texture. Intended for callers (tests, scene
	// collectors) that already hold dstex paths.
	static void prefetch_headers(const PackedStringArray &p_dstex_paths);
	// Shared hit/miss Payload construction so both paths stay identical.
	static void payload_from_header(const String &p_engine_path, const String &p_dstex_path, const PayloadHeader &p_header, Payload &r_out);

	// One DSRequest per independently-decompressed unit: exactly one for v1
	// (single continuous stream), one per mip for v2 (individually
	// compressed footprints). Mirrors _load_texture_payload's split.
	static bool expand_requests(const Payload &p_payload, uint64_t p_tag, Vector<Ref<DSRequest>> &r_out);

	static Ref<Texture2D> build_texture(const Payload &p_meta, const std::vector<PackedByteArray> &p_buffers);

	// GPU chunk pipeline (all render-thread-affine RD work happens in
	// process_gpu_chunk_on_render_thread; pump/attach/orphan helpers run
	// on the calling thread under State::mutex).
	// Shared resolve+dedup core for prewarm_async/prewarm_gpu_async:
	// boot-warmer index load, cache/engine dedup, payload resolve +
	// validate. Fills r_payloads, r_skipped_cached, r_skipped_unresolvable.
	static void collect_prewarm_payloads(const PackedStringArray &p_paths, std::vector<Payload> &r_payloads, int &r_skipped_cached, int &r_skipped_unresolvable);
	static void process_gpu_chunk_on_render_thread();
	static void free_gpu_orphans_on_render_thread();
	static void queue_gpu_orphan_rid(const RID &p_rid);
	static bool main_or_render_thread();
	static void alloc_gpu_item_on_render_thread(RenderingDevice *p_rd, DSContext *p_ctx, uint64_t p_tag, const GpuChunkItem &p_item);
	static void mark_gpu_item_failed(int64_t p_pending_id);
	static void complete_gpu_entry(const Tracked &p_entry, DSTextureCache *p_cache, int &r_stored);
	// CPU completion body shared by the inline slice and the offloaded task.
	// r_stored is true only when the cache accepted the texture; r_entry_bytes
	// is the slice byte charge (charged whenever a texture was built).
	static void complete_cpu_entry(const Tracked &p_entry, DSTextureCache *p_cache, int &r_stored, uint64_t &r_entry_bytes);
	// pending_paths decrement + pending_cv notify; caller holds State::mutex.
	static void unblock_pending_paths_locked(const Tracked &p_entry);
	// WorkerThreadPool native task: builds and stores one CPU entry, then
	// retires it from `pending`. Safe on any pool thread (ImageTexture path).
	static void run_offloaded_cpu_build(void *p_user);
	// Caller holds State::mutex. Returns false when no pool is available, in
	// which case the caller builds inline (progress beats a strict bound).
	static bool offload_cpu_build_locked(int64_t p_id);
	static void pump_gpu_orphans();
};

} // namespace godot

#endif // DS_PRELOADER_H
