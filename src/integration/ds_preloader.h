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
// - Texture OBJECT construction is ImageTexture-only (worker-safe by
//   construction, no RenderingDevice thread affinity): build_texture()
//   produces ImageTexture::create_from_image() on every thread. GPU-direct
//   Texture2DRD upload was removed as the Q31 decision (CPU -> ImageTexture
//   -> RenderingServer marshal replaces NVMe -> VRAM zero-copy; avoids the
//   per-frame call_on_render_thread drain). as_image_texture sources take
//   the same ImageTexture path.
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
//   Godot class. The two GDScript-visible entry points live on the
//   DSTextureCache singleton (prewarm_async / poll_completed) and delegate
//   here; tag progress/cancel stay on DSContext's existing bindings.

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace godot {

class Texture2D;

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
		uint32_t pixel_format = 0;
		bool is_srgb = false;
		uint32_t mip_count = 0;
		bool is_v2 = false;
		bool as_image_texture = false;
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
		uint32_t pixel_format = 0;
		uint32_t mip_count = 0;
		uint32_t flags = 0;
		uint16_t version = 0;
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

	// Combined header index: one file (e.g. textures.headers.idx) next to
	// textures.dspkg carries the header of every bundled .dstex, so 100
	// texture paths cost 1 open + 1 bulk read instead of 100 cold opens.
	//
	// On-disk layout (all little-endian, written by the export plugin):
	//   u32 magic (COMBINED_INDEX_MAGIC "HIDX"), u16 index_version (1),
	//   u16 reserved (0), u32 entry_count, u32 crc32_ieee(entry payload).
	//   Per entry: u32 key_len (1..1024) + key_len utf8 bytes (dstex res
	//   path, exactly as .import remap yields it) + u32 width + u32 height
	//   + u32 pixel_format + u32 flags + u16 dstex_version + u16 mip_count
	//   (1..16) + u64 original_size + u64 compressed_size + u64 mtime +
	//   u64 file_len + int64 mip_offsets[16] + int32 mip_sizes[16] +
	//   int32 mip_uncompressed[16] + int32 row_pitches[16] (only
	//   [0, mip_count) valid, rest zero). CRC is IEEE poly 0xEDB88320 over
	//   the concatenated entry bytes (see crc32_ieee); the GDScript exporter
	//   writer mirrors it exactly. Freshness mirrors the header-cache
	//   contract: serve validates current mtime == stored mtime +
	//   generation match (1 stat, 0 opens); mtime drift falls through to
	//   the header-cache/miss path. Bundle rotation is picked up via
	//   rescan_archive()/shutdown() (generation bump + clear).
	static constexpr uint32_t COMBINED_INDEX_MAGIC = 0x58444948;
	static constexpr uint16_t COMBINED_INDEX_VERSION = 1;
	static constexpr uint32_t COMBINED_INDEX_MAX_ENTRIES = 8192;
	static constexpr uint64_t COMBINED_INDEX_MAX_BYTES = 32ULL * 1024ULL * 1024ULL;

	// Resolve + dedup + enqueue. Returns the DSContext stream tag, or 0 when
	// DSContext is unavailable. Never blocks: requests sit in stream_queue
	// until the batch manager drains them via flush_stream_async().
	static uint64_t prewarm_async(const PackedStringArray &p_paths);

	// Build textures for terminal requests and store them.
	// p_tag == 0 consumes pending entries for ALL tags, otherwise only the
	// given tag. Returns the number of textures stored into DSTextureCache
	// (+ engine ResourceCache residency via take_over_path) by this call.
	// Intended to be called from the main thread each frame while a prewarm
	// is in flight (the batch manager calls it from its per-frame hook).
	// When p_max_items > 0, at most p_max_items textures are processed in this call.
	// When p_max_usec > 0, processing yields as soon as elapsed microseconds exceed the budget.
	static int poll_completed(uint64_t p_tag = 0, int p_max_items = 0, uint32_t p_max_usec = 0);

	// Diagnostic: number of tracked textures with at least one
	// unfinished request. p_tag == 0 counts all tags.
	static int get_pending_prewarm_count(uint64_t p_tag = 0);

	// Diagnostic: number of tracked textures still residing in the preloader's
	// pending state awaiting poll_completed(). p_tag == 0 counts all tags.
	static int get_unbuilt_prewarm_count(uint64_t p_tag = 0);

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

private:
	struct Tracked {
		uint64_t tag = 0;
		Payload meta;
		Vector<Ref<DSRequest>> requests;
	};

	struct State {
		std::mutex mutex;
		std::condition_variable pending_cv;
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
		// Combined header index: dstex res path -> PayloadHeader populated by
		// ONE bulk load_combined_index() (1 open + 1 bulk). Served with zero
		// opens after warm; per-serve freshness is an mtime + generation
		// match, mirroring the header cache. Guarded by State::mutex. Invalidated together with
		// header_cache by invalidate_header_cache() (generation bump +
		// clear, called from shutdown()/rescan_archive()). probed remembers
		// the miss so a missing index costs zero probes per path.
		HashMap<String, PayloadHeader> combined_index;
		String combined_index_path;
		bool combined_index_probed = false;
		bool combined_index_available = false;
	};

	static State &state();

	static bool resolve_payload(const String &p_engine_path, Payload &r_out);
	static String resolve_archive_path();
	static bool fill_from_dspkg(const String &p_engine_path, Payload &r_out);
	static bool fill_from_import_remap(const String &p_engine_path, Payload &r_out);
	static bool fill_from_dstex_file(const String &p_engine_path, const String &p_dstex_path, Payload &r_out);
	static bool validate_payload(const Payload &p_payload);

	// Combined index reader (writer lives in the export plugin).
	// load_combined_index(): idempotent once-per-generation bulk load
	// (single FileAccess::open + single get_buffer). Returns true when the
	// index is available. serve_from_index(): zero-open lookup (1 stat for
	// mtime freshness, zero opens); true on generation+mtime match for a
	// positive entry (build from it), false otherwise (miss, stale mtime,
	// or failure entry). crc32_ieee(): IEEE (poly 0xEDB88320) helper shared
	// by the loader validator and mirrored by the GDScript writer.
	// C++-internal only, no ClassDB bindings by design.
	static String resolve_combined_index_path();
	static bool load_combined_index();
	static bool serve_from_index(const String &p_dstex_path, PayloadHeader &r_out);
	static uint32_t crc32_ieee(const uint8_t *p_data, uint64_t p_len);

	// C++-internal header-cache plumbing (no ClassDB bindings by design —
	// this class is deliberately not a registered Godot class).
	// invalidate_header_cache() also bumps + clears the combined index
	// (generation shared with header_cache_generation).
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

	static bool is_terminal(const Ref<DSRequest> &p_request);
	static Ref<Texture2D> build_texture(const Payload &p_meta, const std::vector<PackedByteArray> &p_buffers);
};

} // namespace godot

#endif // DS_PRELOADER_H
