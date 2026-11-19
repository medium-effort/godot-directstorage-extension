#include "ds_preloader.h"
#include "ds_texture_cache.h"
#include "core/ds_settings.h"
#include "../core/ds_context.h"
#include "../core/ds_cpu_budget.h"
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace godot {

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
static std::atomic<int> s_last_poll_inline_builds{ 0 };
static std::atomic<int> s_last_poll_offloads{ 0 };
#endif

// Upper bound per prewarm request output buffer. The sync loader imposes no
// cap, but prewarm fans out to dozens of speculative buffers, so a corrupt
// header claiming gigabytes must be rejected before resize() runs.
static constexpr uint64_t MAX_PREWARM_REQUEST_BYTES = 512ULL * 1024ULL * 1024ULL;

// Per-callback request cap: one render-thread callback allocates, enqueues and
// flush-attempts every request of its textures, so an 8K chunked texture (tens
// of chunk requests) must not share a callback with dozens of siblings. 64
// keeps the worst case near one queue-budget slice (queue_capacity / 2 = 256 at
// the 512 default) while packing small textures efficiently. The effective cap
// is additionally lowered to half the queue capacity by prewarm_gpu_async().
static constexpr size_t kGpuPrewarmRequestsPerCallback = 64;

// Per-callback VRAM byte cap: bound how much texture_create() + GPU GDeflate
// decompression work a single render-thread callback stages. Whole textures
// are never split: a single oversized texture rides alone in its own chunk.
static constexpr uint64_t kGpuPrewarmBytesPerCallback = 16ULL * 1024ULL * 1024ULL;

static uint64_t estimate_payload_bytes(const DSPreloader::Payload &p_meta);

// Single walk over a chunked payload's chunk table, in mip order. This is the
// ONE definition of the GPU upload shape: eligibility, request counting and
// request construction all consume it, so they cannot drift apart.
//
// Calls p_visit(mip, chunk_index, row, rows) for every upload unit:
//   chunk_index >= 0 - one row-range chunk; `row` is its starting texel row
//                      inside the mip and `rows` its texel height (the units a
//                      DSTORAGE_DESTINATION_TEXTURE_REGION box speaks).
//   chunk_index == -1 - a single-chunk mip, uploaded as a whole-subresource
//                      request (its chunk-table entry does not exist).
//
// Returns false when the table does not exactly cover the payload: array
// lengths disagree, a mip's chunk count overruns or undershoots the table, a
// pitch is unusable, a chunk size is not a whole number of rows, or a mip's
// chunks do not sum to its footprint. Callers treat false as "ineligible".
template <typename Fn>
static bool walk_gpu_upload_units(const DSPreloader::Payload &p_meta, Fn &&p_visit) {
	const int64_t mips = static_cast<int64_t>(p_meta.mip_count);
	if (mips <= 0) {
		return false;
	}
	if (p_meta.mip_chunk_counts.size() < mips || p_meta.mip_row_pitches.size() < mips) {
		return false;
	}
	const int64_t table_size = p_meta.chunk_offsets.size();
	if (p_meta.chunk_compressed_sizes.size() != table_size ||
			p_meta.chunk_uncompressed_sizes.size() != table_size) {
		return false;
	}
	const int img_fmt = DSTex::rd_format_to_godot_format(static_cast<int>(p_meta.pixel_format));
	int64_t cursor = 0;
	for (int64_t mip = 0; mip < mips; ++mip) {
		const int32_t count = p_meta.mip_chunk_counts[mip];
		// Single-chunk mips have no chunk-table entries (same convention as
		// expand_requests and the loader path).
		if (count <= 1) {
			if (!p_visit(static_cast<uint32_t>(mip), static_cast<int64_t>(-1), 0u, 0u)) {
				return false;
			}
			continue;
		}
		const int32_t pitch = p_meta.mip_row_pitches[mip];
		if (pitch <= 0) {
			return false;
		}
		if (cursor + count > table_size) {
			return false;
		}
		// `row` stays in storage rows (block rows for the 4x4 formats) because
		// that is the unit of `pitch` and of the byte-sum check below; the
		// visitor receives texel rows, which is what a D3D12_BOX speaks.
		const uint32_t mip_height = std::max<uint32_t>(1u, p_meta.height >> static_cast<uint32_t>(mip));
		uint32_t row = 0;
		for (int32_t c = 0; c < count; ++c) {
			const int64_t chunk_index = cursor + c;
			const int64_t uncompressed = static_cast<int64_t>(p_meta.chunk_uncompressed_sizes[chunk_index]);
			const int64_t compressed = static_cast<int64_t>(p_meta.chunk_compressed_sizes[chunk_index]);
			if (uncompressed <= 0 || uncompressed > UINT32_MAX || compressed <= 0 || compressed > UINT32_MAX) {
				return false;
			}
			if (uncompressed % pitch != 0) {
				return false;
			}
			const uint32_t rows = static_cast<uint32_t>(uncompressed / pitch);
			uint32_t region_top = 0;
			uint32_t region_bottom = 0;
			if (rows == 0 ||
					!DSTex::get_chunk_texel_region(mip_height, row, rows, img_fmt, region_top, region_bottom)) {
				return false;
			}
			if (!p_visit(static_cast<uint32_t>(mip), chunk_index, region_top, region_bottom - region_top)) {
				return false;
			}
			row += rows;
		}
		if (static_cast<uint64_t>(row) * static_cast<uint64_t>(pitch) !=
				static_cast<uint64_t>(p_meta.mip_uncompressed_sizes[mip])) {
			return false;
		}
		cursor += count;
	}
	return cursor == table_size;
}

// Request fan-out the GPU path will stage for one payload: 1 for plain
// payloads, otherwise one per upload unit. Mirrors walk_gpu_upload_units so the
// per-callback slicer budgets in requests, not textures. A whole texture is
// never split across callbacks (pending entries attach all requests in one
// alloc), so an estimate above the cap still rides as one oversized chunk and
// the render-side queue-budget guard diverts it.
static size_t count_gpu_requests_for_payload(const DSPreloader::Payload &p_meta) {
	if (!p_meta.chunked) {
		return 1;
	}
	size_t total = 0;
	const bool shaped = walk_gpu_upload_units(p_meta,
			[&total](uint32_t, int64_t, uint32_t, uint32_t) {
				++total;
				return true;
			});
	// A malformed table is rejected by is_gpu_prewarm_eligible() before it can be
	// queued; counting it as one keeps a stray entry from stalling the tag.
	return (shaped && total > 0) ? total : 1;
}

// One render-thread callback's worth of GPU prewarm items.
struct GpuChunkSlice {
	size_t first = 0;
	size_t count = 0;
};

// THE definition of the per-callback split, shared by prewarm_gpu_async() and the
// test seam so the budget cannot drift from what it asserts. Consecutive items
// are packed while the chunk stays inside ALL THREE caps; a whole texture is
// never split (an item that does not fit starts the next chunk), so a single
// oversized texture rides alone rather than wedge the queue behind it.
static std::vector<GpuChunkSlice> plan_gpu_chunk_slices(const std::vector<uint64_t> &p_requests_per_item,
		const std::vector<uint64_t> &p_bytes_per_item, size_t p_item_cap, size_t p_request_cap, uint64_t p_byte_cap) {
	std::vector<GpuChunkSlice> slices;
	const size_t item_count = std::min(p_requests_per_item.size(), p_bytes_per_item.size());
	size_t index = 0;
	while (index < item_count) {
		GpuChunkSlice slice;
		slice.first = index;
		uint64_t slice_requests = 0;
		uint64_t slice_bytes = 0;
		while (index < item_count) {
			const uint64_t item_requests = p_requests_per_item[index];
			const uint64_t item_bytes = p_bytes_per_item[index];
			const bool overflows = slice.count > 0 &&
					(slice.count >= p_item_cap ||
							slice_requests + item_requests > p_request_cap ||
							slice_bytes + item_bytes > p_byte_cap);
			if (overflows) {
				break;
			}
			slice_requests += item_requests;
			slice_bytes += item_bytes;
			++slice.count;
			++index;
		}
		slices.push_back(slice);
	}
	return slices;
}

// Terminal accounting for GPU prewarm units the DSContext will never count
// itself: an alloc that failed before enqueueing, or requests abandoned mid
// fan-out. The tag total counts one unit per interned request (#0247), so a
// texture that fans out to N requests must contribute N here as well.
static void count_gpu_shortfall(DSContext *p_ctx, uint64_t p_tag, size_t p_units) {
	if (p_ctx == nullptr) {
		return;
	}
	for (size_t i = 0; i < p_units; ++i) {
		p_ctx->count_stream_terminal_failure(p_tag);
	}
}

// Full Godot mip chain length for Image::create_from_data(use_mipmaps=true).
static uint32_t ds_preloader_full_mip_chain_count(uint32_t p_w, uint32_t p_h) {
	uint32_t n = 1;
	while (p_w > 1 || p_h > 1) {
		if (p_w > 1) {
			p_w /= 2;
		}
		if (p_h > 1) {
			p_h /= 2;
		}
		++n;
	}
	return n;
}

DSPreloader::State &DSPreloader::state() {
	static State s;
	return s;
}

bool DSPreloader::is_engine_source_path(const String &p_path) {
	String ext = p_path.get_extension().to_lower();
	return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp";
}

String DSPreloader::resolve_archive_path() {
	Engine *engine = Engine::get_singleton();
	OS *os = OS::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();

	bool force_resolution = dsgd_settings::has_setting("force_archive_resolution", "force_archive_resolution") &&
			static_cast<bool>(dsgd_settings::get_setting("force_archive_resolution", "force_archive_resolution"));

	if (!force_resolution && ((engine && engine->is_editor_hint()) || (os && os->has_feature("editor")))) {
		return String();
	}

	{
		std::lock_guard<std::mutex> lock(state().archive_mutex);
		State &s = state();
		if (s.resolved_archive_probed) {
			return s.resolved_archive_path;
		}
	}

	String archive_filename = "textures.dspkg";
	if (dsgd_settings::has_setting("archive_name", "archive_name")) {
		String custom_name = dsgd_settings::get_setting("archive_name", "archive_name");
		if (!custom_name.is_empty()) {
			archive_filename = custom_name;
		}
	}
	String logical = "res://" + archive_filename;

	String found;
	if (FileAccess::file_exists(logical)) {
		found = logical;
	} else if (FileAccess::file_exists("res://textures.dspkg")) {
		found = "res://textures.dspkg";
	} else {
		if (os) {
			String exe_dir = os->get_executable_path().get_base_dir();
			String exe_custom = exe_dir.path_join(archive_filename);
			String exe_default = exe_dir.path_join("textures.dspkg");
			if (FileAccess::file_exists(exe_custom)) {
				found = exe_custom;
			} else if (FileAccess::file_exists(exe_default)) {
				found = exe_default;
			}
		}
		if (found.is_empty() && ps) {
			String globalized_custom = ps->globalize_path(logical);
			String globalized_default = ps->globalize_path("res://textures.dspkg");
			if (FileAccess::file_exists(globalized_custom)) {
				found = globalized_custom;
			} else if (FileAccess::file_exists(globalized_default)) {
				found = globalized_default;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(state().archive_mutex);
		State &s = state();
		if (!s.resolved_archive_probed) {
			s.resolved_archive_path = found;
			s.resolved_archive_probed = true;
		} else {
			found = s.resolved_archive_path;
		}
	}
	return found;
}

void DSPreloader::rescan_archive() {
	std::lock_guard<std::mutex> lock(state().archive_mutex);
	State &s = state();
	s.resolved_archive_path = String();
	s.resolved_archive_probed = false;
	s.archive_generation++;
	// Archive set may have changed: dstex files (re)imported alongside the
	// archive must be re-read, so drop cached headers too. Lock order stays
	// archive_mutex -> mutex, matching shutdown().
	invalidate_header_cache();
	// NOTE: the open handle (s.archive) is intentionally kept — the
	// positive open-cache behavior stays identical and the next
	// fill_from_dspkg re-resolves, then reuses the handle when the path
	// is unchanged instead of reopening.
}

uint64_t DSPreloader::get_archive_generation() {
	std::lock_guard<std::mutex> lock(state().archive_mutex);
	return state().archive_generation;
}

bool DSPreloader::fill_from_dspkg(const String &p_engine_path, Payload &r_out) {
	String archive_path = resolve_archive_path();
	if (archive_path.is_empty()) {
		// Negative-cache false-fast: cached miss, zero probes spent.
		return false;
	}
	Ref<DSPkg> archive;
	{
		std::lock_guard<std::mutex> lock(state().archive_mutex);
		State &s = state();
		if (s.archive.is_valid() && s.archive_path == archive_path) {
			archive = s.archive;
		}
	}

	if (archive.is_null()) {
		// Open the archive with no lock held: this mutex also guards the state the
		// main thread reads, so a TOC read and decrypt (up to 64 MiB) must not run
		// inside it. DSPkg::open touches only the instance it is called on.
		Ref<DSPkg> fresh;
		fresh.instantiate();
		// 0 means the archive is unpaired; pairing is the random archive id,
		// compared in full 64-bit width when the expected value is non-zero.
		uint64_t expected_fingerprint = 0;
		if (dsgd_settings::has_setting("archive_id", "export/archive_id")) {
			expected_fingerprint = static_cast<uint64_t>(dsgd_settings::get_setting("archive_id", "export/archive_id"));
		}
		Error open_err = fresh->open(archive_path, expected_fingerprint);
		if (open_err != OK) {
			if (open_err == ERR_UNAUTHORIZED) {
				// Function-local state, so it needs its own guard: two callers may
				// open concurrently now that archive_mutex does not cover the parse.
				static std::mutex s_unauth_report_mutex;
				static String s_last_reported_preloader_unauth;
				std::lock_guard<std::mutex> report_lock(s_unauth_report_mutex);
				if (s_last_reported_preloader_unauth != archive_path) {
					s_last_reported_preloader_unauth = archive_path;
					UtilityFunctions::push_error("DirectStorage Preloader: Decryption unauthorized or key mismatch for package '" + archive_path + "'. The archive key is read from res://.dspkg/toc.key inside the game's encrypted PCK, so the PCK and the archive must ship as a pair.");
				}
			} else {
				UtilityFunctions::push_error("DirectStorage Preloader: Failed to open DirectStorage package '" + archive_path + "': " + UtilityFunctions::error_string(open_err));
			}
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(state().archive_mutex);
			State &s = state();
			// A concurrent opener that parsed this same path may have published
			// first; keep its instance so one archive has one TOC in memory.
			if (s.archive.is_valid() && s.archive_path == archive_path) {
				archive = s.archive;
			} else {
				s.archive_path = archive_path;
				s.archive = fresh;
				archive = fresh;
			}
		}
	}
	if (!archive.is_valid() || !archive->has_texture(p_engine_path)) {
		return false;
	}
	Dictionary info = archive->get_entry_info(p_engine_path);
	if (info.is_empty()) {
		return false;
	}
	// Preload stages GDeflate and raw-NONE requests. Zstd and GACL still fail
	// closed; chunked entries are staged with their chunk table (CPU prewarm
	// expands one request per chunk and validate_payload checks the table).
	const int entry_compression_format = (int)info.get("compression_format", 1);
	if ((int)info.get("entry_kind", 0) != static_cast<int>(DSPkgEntryKind::TEXTURE) ||
			(entry_compression_format != static_cast<int>(DSCompressionFormat::GDEFLATE) &&
					entry_compression_format != static_cast<int>(DSCompressionFormat::NONE)) ||
			(int)info.get("gacl_transform", 0) != static_cast<int>(DSGaclTransform::NONE)) {
		UtilityFunctions::push_error("DirectStorage Preloader: Entry requires a GDeflate/NONE, non-GACL payload; preload skipped: " + p_engine_path);
		return false;
	}
	r_out.engine_path = p_engine_path;
	// Single-lookup Dictionary reads: get() costs one hash probe where
	// has()+operator[] cost two for every optional key.
	// Semantics identical to the prior has/?:/default form (missing key ->
	// default; present-but-empty "path" still resolves to the engine path via
	// the is_empty() ternary, exactly as the old empty-check block did).
	String path_override = info.get("path", p_engine_path);
	r_out.payload_key = path_override.is_empty() ? p_engine_path : path_override;
	r_out.in_dspkg = true;
	// entry_info["archive_path"] is the physical archive this entry was read
	// from (same file we opened above); the backend opens it like the sync
	// loader does, so keep that exact string.
	r_out.file_path = String(info.get("archive_path", archive_path));
	r_out.width = info["width"];
	r_out.height = info["height"];
	// get_entry_info reports logical keys with a storage fallback. Only a
	// differing size is retained; 0 keeps the "same as storage" meaning.
	uint32_t dspkg_logical_w = info.get("logical_width", r_out.width);
	uint32_t dspkg_logical_h = info.get("logical_height", r_out.height);
	r_out.logical_width = (dspkg_logical_w != 0 && dspkg_logical_w != r_out.width) ? dspkg_logical_w : 0;
	r_out.logical_height = (dspkg_logical_h != 0 && dspkg_logical_h != r_out.height) ? dspkg_logical_h : 0;
	r_out.pixel_format = info["pixel_format"];
	r_out.is_srgb = (bool)info.get("is_srgb", false) || DSTex::is_rd_format_srgb((int)r_out.pixel_format);
	r_out.mip_count = info["mip_count"];
	r_out.is_v2 = (bool)info.get("has_gpu_metadata", false);
	r_out.as_image_texture = (bool)info.get("as_image_texture", false);
	r_out.compression_format = static_cast<uint8_t>(entry_compression_format);
	r_out.data_offset = (uint64_t)info.get("data_offset", 0);
	r_out.compressed_size = (uint64_t)info.get("compressed_size", 0);
	r_out.original_size = (uint64_t)info.get("original_size", 0);
	// Missing key -> shared empty array default; the Variant->Packed cast is
	// the same conversion the explicit branch performed, now after one probe.
	r_out.mip_offsets = (PackedInt64Array)info.get("mip_offsets", PackedInt64Array());
	r_out.mip_sizes = (PackedInt32Array)info.get("mip_sizes", PackedInt32Array());
	r_out.mip_uncompressed_sizes = (PackedInt32Array)info.get("mip_uncompressed_sizes", PackedInt32Array());
	r_out.mip_row_pitches = (PackedInt32Array)info.get("mip_row_pitches", PackedInt32Array());
	r_out.chunked = (bool)info.get("chunked", false);
	r_out.mip_chunk_counts = (PackedInt32Array)info.get("mip_chunk_counts", PackedInt32Array());
	r_out.chunk_offsets = (PackedInt64Array)info.get("chunk_offsets", PackedInt64Array());
	r_out.chunk_compressed_sizes = (PackedInt32Array)info.get("chunk_compressed_sizes", PackedInt32Array());
	r_out.chunk_uncompressed_sizes = (PackedInt32Array)info.get("chunk_uncompressed_sizes", PackedInt32Array());
	return true;
}

void DSPreloader::invalidate_header_cache() {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	s.header_cache_generation++;
	s.header_cache.clear();
}

bool DSPreloader::try_get_cached_header(const String &p_dstex_path, PayloadHeader &r_out) {
	// Stat outside the critical section: get_modified_time is a cheap
	// metadata probe (~1-5 us), not an open + parse (~40-140 us).
	// Missing files (mtime 0) also consult the map — a recorded failure
	// entry at this generation skips the re-open below.
	uint64_t mtime = FileAccess::get_modified_time(p_dstex_path);
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	const PayloadHeader *stored = s.header_cache.getptr(p_dstex_path);
	if (stored == nullptr || stored->generation != s.header_cache_generation || stored->mtime != mtime) {
		return false;
	}
	// file_len is trusted on mtime match: NTFS mtime granularity makes a
	// content change without an mtime bump practically impossible, and a
	// static length stat would cost an open — the very tax this cache
	// removes. Length is still validated on every miss-path read. The same
	// discipline covers failure entries: a file created after a missing-file
	// miss carries a new nonzero mtime and re-opens; same-mtime failure
	// entries serve the failure with zero opens until
	// shutdown()/rescan_archive().
	r_out = *stored;
	return true;
}

void DSPreloader::publish_header_cache(const String &p_dstex_path, const PayloadHeader &p_header) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	const PayloadHeader *existing = s.header_cache.getptr(p_dstex_path);
	if (existing != nullptr && existing->generation == s.header_cache_generation &&
			existing->mtime == p_header.mtime && existing->file_len == p_header.file_len) {
		return; // First publisher wins within a generation.
	}
	PayloadHeader copy = p_header;
	copy.generation = s.header_cache_generation;
	s.header_cache[p_dstex_path] = copy;
}

void DSPreloader::mark_header_failed(const String &p_dstex_path) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	PayloadHeader *entry = s.header_cache.getptr(p_dstex_path);
	if (entry == nullptr || entry->generation != s.header_cache_generation) {
		return;
	}
	entry->open_failed = true;
}

bool DSPreloader::read_header_uncached(const String &p_dstex_path, PayloadHeader &r_out) {
	// Failure contract: r_out always carries the observed mtime/file_len
	// observed mtime/file_len with open_failed=true until the final fill
	// below flips it — the caller publishes r_out verbatim on false, so
	// repeats serve the failure from the cache with zero re-opens.
	r_out = PayloadHeader();
	r_out.open_failed = true;
	uint64_t mtime = FileAccess::get_modified_time(p_dstex_path);
	r_out.mtime = mtime;
	Ref<FileAccess> file = FileAccess::open(p_dstex_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	uint64_t file_len = file->get_length();
	r_out.file_len = file_len;
	if (file_len < sizeof(DSTexHeader)) {
		file->close();
		return false;
	}
	// Single bulk read: header + largest possible mip table (16 V2
	// entries) in ONE get_buffer, entries parsed from the same buffer via
	// memcpy — no second I/O. A literal header+actual-table single read is
	// impossible without knowing mip_count first (it lives inside the
	// header); the max-table over-read is ~384 bytes, capped by file_len,
	// and surplus bytes are never parsed. Total per miss: 1 open +
	// 1 buffer + zero Dictionary/Packed* churn until Payload construction.
	// (Covers every valid file: required = dstex_header_size(version) +
	// count * entry <= 64 + 16 * 24 = MAX_BULK, so the buffer always spans the
	// table whenever required <= file_len.)
	constexpr uint64_t MAX_TABLE = (uint64_t)DSTEX_MAX_MIP_COUNT * sizeof(DSTexMipEntryV2);
	constexpr uint64_t MAX_BULK = sizeof(DSTexHeaderV3) + MAX_TABLE;
	uint64_t want = file_len < MAX_BULK ? file_len : MAX_BULK;
	PackedByteArray bulk = file->get_buffer((int64_t)want);
	file->close();
	if (bulk.size() < (int64_t)sizeof(DSTexHeader)) {
		return false;
	}
	DSTexHeader header;
	memcpy(&header, bulk.ptr(), sizeof(DSTexHeader));

	// Validation mirrors DSTex::read_header (magic, dimensions, 1..16 mips, the
	// dstex_sizes_are_sane() size contract, mip-table bounds) and adds the
	// per-mip sum contract that DSPkg::open() and the Win32 enqueue path already
	// enforce, so the cache never accepts a file the Dictionary path, the
	// archive reader or the GPU enqueue path would reject.
	if (header.magic != 0x58455444) {
		UtilityFunctions::push_error("DSTex: Invalid magic number.");
		return false;
	}
	if (header.version < 1 || header.version > 3) {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		return false;
	}
	if ((header.flags & ~(DSTEX_FLAG_SRGB | DSTEX_FLAG_AS_IMAGE_TEXTURE)) != 0) {
		UtilityFunctions::push_error("DSTex: Reserved texture flags must be zero.");
		return false;
	}
	if (header.version <= 2 && header.reserved != 0) {
		UtilityFunctions::push_error("DSTex: v1/v2 reserved field must be zero.");
		return false;
	}
	DSTexHeaderV3Ext v3_ext = {};
	// v1/v2 carry no codec field, so their payload is GDeflate by definition.
	DSCompressionFormat payload_format = DSCompressionFormat::GDEFLATE;
	uint64_t header_capabilities = 0;
	if (header.version == 3) {
		if (bulk.size() < static_cast<int64_t>(sizeof(DSTexHeaderV3))) {
			return false;
		}
		memcpy(&v3_ext, bulk.ptr() + sizeof(DSTexHeader), sizeof(DSTexHeaderV3Ext));
		if (v3_ext.reserved0 != 0) {
			UtilityFunctions::push_error("DSTex: v3 header reserved field must be zero.");
			return false;
		}
		if (v3_ext.logical_width > header.width || v3_ext.logical_height > header.height) {
			UtilityFunctions::push_error("DSTex: v3 logical dimensions exceed storage dimensions.");
			return false;
		}
		if (v3_ext.depth_or_layers > 1) {
			UtilityFunctions::push_error("DSTex: Preload does not support 3D/array textures until wired.");
			return false;
		}
		const uint64_t capabilities = header.reserved;
		if ((capabilities & ~DSTEX_CAP_KNOWN_MASK) != 0) {
			UtilityFunctions::push_error("DSTex: Unknown v3 capability bits.");
			return false;
		}
		const DSCompressionFormat format = DSTex::get_compression_format(capabilities);
		const DSGaclTransform gacl = DSTex::get_gacl_transform(capabilities);
		if (!ds_compression_format_is_valid(static_cast<uint8_t>(format)) ||
				!ds_gacl_transform_is_valid(static_cast<uint8_t>(gacl)) ||
				!DSTex::gacl_transform_matches_pixel_format(gacl, static_cast<int>(header.pixel_format)) ||
				(gacl != DSGaclTransform::NONE && format == DSCompressionFormat::NONE)) {
			UtilityFunctions::push_error("DSTex: Invalid v3 capability word.");
			return false;
		}
		if ((format != DSCompressionFormat::GDEFLATE && format != DSCompressionFormat::NONE) ||
				gacl != DSGaclTransform::NONE) {
			UtilityFunctions::push_error("DSTex: Preload requires a GDeflate or raw NONE payload without GACL until Step 5.");
			return false;
		}
		payload_format = format;
		header_capabilities = capabilities;
		if (format == DSCompressionFormat::NONE && header.compressed_size != header.original_size) {
			UtilityFunctions::push_error("DSTex: NONE-compressed header sizes must match.");
			return false;
		}
	}
	if (header.width == 0 || header.height == 0) {
		UtilityFunctions::push_error("DSTex: Invalid texture dimensions.");
		return false;
	}
	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
		return false;
	}
	if (!dstex_sizes_are_sane(header.width, header.height, header.original_size, header.compressed_size)) {
		UtilityFunctions::push_error("DSTex: Header sizes exceed sane texture bounds (FMT-01).");
		return false;
	}
	uint64_t entry_size = (header.version == 1) ? sizeof(DSTexMipEntry) : sizeof(DSTexMipEntryV2);
	uint64_t mip_table_size = static_cast<uint64_t>(header.mip_count) * entry_size;
	{
		if (entry_size != 0 && mip_table_size / entry_size != static_cast<uint64_t>(header.mip_count)) {
			UtilityFunctions::push_error("DSTex: Mip table size overflow.");
			return false;
		}
		uint64_t header_size = dstex_header_size(header.version);
		uint64_t required = header_size + mip_table_size;
		if (required < header_size || required > file_len) {
			UtilityFunctions::push_error("DSTex: Mip table exceeds file bounds.");
			return false;
		}
		if ((uint64_t)bulk.size() < required) {
			return false; // Short bulk read; treat as I/O miss.
		}
	}
	const uint8_t *table_ptr = bulk.ptr() + dstex_header_size(header.version);

	PayloadHeader filled;
	filled.width = header.width;
	filled.height = header.height;
	if (header.version == 3) {
		// v3_ext was validated above (logical <= storage, 0 = storage).
		// Only a differing size is retained; 0 keeps "same as storage".
		if (v3_ext.logical_width != 0 && v3_ext.logical_width != header.width) {
			filled.logical_width = v3_ext.logical_width;
		}
		if (v3_ext.logical_height != 0 && v3_ext.logical_height != header.height) {
			filled.logical_height = v3_ext.logical_height;
		}
	}
	filled.pixel_format = header.pixel_format;
	filled.mip_count = header.mip_count;
	filled.flags = header.flags;
	filled.version = header.version;
	filled.compression_format = static_cast<uint8_t>(payload_format);
	filled.original_size = header.original_size;
	filled.compressed_size = header.compressed_size;
	filled.mtime = mtime;
	filled.file_len = file_len;
	filled.open_failed = false;

	if (header.version == 1) {
		uint64_t previous_end = sizeof(DSTexHeader) +
				static_cast<uint64_t>(header.mip_count) * sizeof(DSTexMipEntry);
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			memcpy(&mip_entry, table_ptr + i * sizeof(DSTexMipEntry), sizeof(DSTexMipEntry));
			if (mip_entry.offset > file_len || mip_entry.size > file_len - mip_entry.offset ||
					(mip_entry.size != 0 && mip_entry.offset + mip_entry.size < mip_entry.offset) ||
					mip_entry.offset < previous_end) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds, overlaps, or starts inside the tables.");
				return false;
			}
			previous_end = mip_entry.offset + mip_entry.size;
			filled.mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			filled.mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
	} else if (header.version == 2 || header.version == 3) {
		// Each table entry is a uint32 and there are at most 16 of them, so the
		// sums cannot overflow uint64 and need no guard of their own.
		DSTexMipEntryV2 entries[DSTEX_MAX_MIP_COUNT];
		bool any_chunked_mip = false;
		uint64_t previous_end = dstex_header_size(header.version) +
				static_cast<uint64_t>(header.mip_count) * sizeof(DSTexMipEntryV2);
		previous_end = (previous_end + 511) & ~uint64_t(511);
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			memcpy(&mip_entry, table_ptr + i * sizeof(DSTexMipEntryV2), sizeof(DSTexMipEntryV2));
			if (mip_entry.offset > file_len || mip_entry.compressed_size > file_len - mip_entry.offset ||
					(mip_entry.compressed_size != 0 && mip_entry.offset + mip_entry.compressed_size < mip_entry.offset) ||
					mip_entry.offset < previous_end) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds, overlaps, or starts inside the tables.");
				return false;
			}
			previous_end = mip_entry.offset + mip_entry.compressed_size;
			if (header.version == 2 && mip_entry.reserved != 0) {
				UtilityFunctions::push_error("DSTex: v2 mip reserved field must be zero.");
				return false;
			}
			if (header.version == 3) {
				if (mip_entry.chunk_count == 0 || mip_entry.chunk_count > DSTEX_MAX_CHUNKS_PER_MIP) {
					UtilityFunctions::push_error("DSTex: Invalid v3 chunk count.");
					return false;
				}
				if (mip_entry.chunk_count > 1) {
					any_chunked_mip = true;
				}
				if (mip_entry.offset % 512 != 0 ||
						mip_entry.row_pitch == 0 || mip_entry.row_pitch % 256 != 0) {
					UtilityFunctions::push_error("DSTex: Invalid v3 mip alignment.");
					return false;
				}
				if (DSTex::get_compression_format(header.reserved) == DSCompressionFormat::NONE &&
						mip_entry.compressed_size != mip_entry.uncompressed_size) {
					UtilityFunctions::push_error("DSTex: NONE-compressed mip sizes must match.");
					return false;
				}
			}
			entries[i] = mip_entry;
			filled.mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			filled.mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			filled.mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			filled.mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
		}
		// The archive reader (DSPkg::open) and the Win32 enqueue path both require
		// the mip table to add up to the header totals, and the per-mip footprint
		// to match the declared layout. Without it a header can declare a sane
		// total while its mips lie about their shape.
		if (!DSTex::mip_table_is_sane(header, entries)) {
			UtilityFunctions::push_error("DSTex: Mip table does not match the header totals or the declared mip layout.");
			return false;
		}
		// The capability must be set iff a mip is actually chunked (same contract
		// as DSTex::read_header); the chunk tables themselves are read later by
		// the payload builder through DSTex::read_header, and a rejection there
		// fails the payload closed instead of downgrading it to unchunked.
		if (((header_capabilities & DSTEX_CAP_CHUNKED) != 0) != any_chunked_mip) {
			UtilityFunctions::push_error("DSTex: v3 CAP_CHUNKED does not match the mip table.");
			return false;
		}
		filled.capabilities = header_capabilities;
		filled.chunked = any_chunked_mip;
	} else {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		return false;
	}
	r_out = filled;
	return true;
}

void DSPreloader::prefetch_headers(const PackedStringArray &p_dstex_paths) {
	// Best-effort batch warm: each uncached path costs one stat + at most
	// one open, served through the same first-publisher-wins publish path
	// as the miss flow (positives and open-negatives alike). Duplicates
	// within the batch collapse to a single fill via the local seen set.
	HashMap<String, bool> seen;
	for (int i = 0; i < p_dstex_paths.size(); ++i) {
		String path = p_dstex_paths[i];
		if (path.is_empty() || seen.has(path)) {
			continue;
		}
		seen[path] = true;
		PayloadHeader tmp;
		if (try_get_cached_header(path, tmp)) {
			continue; // Positive or failure entry already current; nothing to do.
		}
		// read_header_uncached fills tmp either way: positive on true, the
		// failure entry (observed mtime/file_len, open_failed=true) on
		// false — publishing both skips the re-open on repeats.
		read_header_uncached(path, tmp);
		publish_header_cache(path, tmp);
	}
}

bool DSPreloader::payload_from_header(const String &p_engine_path, const String &p_dstex_path, const PayloadHeader &p_header, Payload &r_out) {
	r_out = Payload();
	r_out.engine_path = p_engine_path;
	r_out.payload_key = p_dstex_path;
	r_out.in_dspkg = false;
	r_out.file_path = p_dstex_path;
	r_out.width = p_header.width;
	r_out.height = p_header.height;
	r_out.logical_width = p_header.logical_width;
	r_out.logical_height = p_header.logical_height;
	r_out.pixel_format = p_header.pixel_format;
	// read_header reports is_srgb as flags-bit OR rd-format probe, and
	// as_image_texture as flags-bit (== its own as_image_texture key).
	r_out.is_srgb = ((p_header.flags & 1U) != 0) || DSTex::is_rd_format_srgb((int)p_header.pixel_format);
	r_out.as_image_texture = ((p_header.flags & 2U) != 0);
	r_out.mip_count = p_header.mip_count;
	r_out.original_size = p_header.original_size;
	r_out.compressed_size = p_header.compressed_size;
	r_out.compression_format = p_header.compression_format;
	r_out.is_v2 = p_header.version >= 2;
	// Bulk mip-table copy: one resize (single zero-fill alloc) + one memcpy
	// per table instead of 4*mip_count bounds-checked operator[] pairs. Array SHAPES unchanged — v1 keeps all
	// four tables sized mip_count (zero-filled tail, exactly as the element
	// loop produced from the zero-initialized PayloadHeader slots), so every
	// downstream consumer (validate/expand/build) observes identical sizes
	// and values. PayloadHeader tables are fixed 16-slot; only
	// [0, mip_count) are valid and only those bytes are copied.
	int64_t mips = (int64_t)p_header.mip_count;
	if (mips > 0) {
		r_out.mip_offsets.resize(mips);
		memcpy(r_out.mip_offsets.ptrw(), p_header.mip_offsets, (size_t)mips * sizeof(int64_t));
		r_out.mip_sizes.resize(mips);
		memcpy(r_out.mip_sizes.ptrw(), p_header.mip_sizes, (size_t)mips * sizeof(int32_t));
		r_out.mip_uncompressed_sizes.resize(mips);
		memcpy(r_out.mip_uncompressed_sizes.ptrw(), p_header.mip_uncompressed_sizes, (size_t)mips * sizeof(int32_t));
		r_out.mip_row_pitches.resize(mips);
		memcpy(r_out.mip_row_pitches.ptrw(), p_header.mip_row_pitches, (size_t)mips * sizeof(int32_t));
	}
	// Byte offset of the first payload byte: header + mip table, 512-aligned
	// for v2. Mirrors DSResourceFormatLoader::_load's dstex branch exactly. The
	// header size comes from the file's version, not the base struct: a v3 file
	// carries its 16-byte extension between the header and the mip table.
	uint64_t data_offset = dstex_header_size(p_header.version) + ((r_out.is_v2 ? sizeof(DSTexMipEntryV2) : sizeof(DSTexMipEntry)) * (uint64_t)r_out.mip_count);
	if (r_out.is_v2) {
		data_offset = (data_offset + 511) & ~uint64_t(511);
	}
	r_out.data_offset = data_offset;

	r_out.chunked = p_header.chunked;
	if (r_out.chunked) {
		// The header cache deliberately keeps only the fixed header + mip table.
		// Chunk tables are variable length, so a chunked payload re-reads them
		// through the validated parser (one extra open+parse per prewarm of a
		// chunked texture; these are the minority and prewarm is batched).
		Ref<DSTex> dstex_reader;
		dstex_reader.instantiate();
		Dictionary header_info = dstex_reader->read_header(p_dstex_path);
		if (header_info.is_empty()) {
			// Corrupt or truncated chunk table: this is NOT an unchunked file.
			// The v3 mip table already carries the first chunk offset and the
			// summed chunk sizes, so a downgraded payload would pass
			// validate_payload() and stage concatenated GDeflate streams as one
			// whole-mip request. Reset so no caller can observe a half-built
			// payload either.
			r_out = Payload();
			return false;
		}
		const PackedInt64Array chunk_offsets = (PackedInt64Array)header_info.get("chunk_offsets", PackedInt64Array());
		if (chunk_offsets.size() == 0) {
			// Invariant guard: a successful parse of a header that carries
			// DSTEX_CAP_CHUNKED always yields at least one chunk entry (the
			// reader rejects the capability/mip-count mismatch), so this can
			// only fire if that contract ever regresses.
			r_out = Payload();
			return false;
		}
		r_out.mip_chunk_counts = (PackedInt32Array)header_info.get("mip_chunk_counts", PackedInt32Array());
		r_out.chunk_offsets = chunk_offsets;
		r_out.chunk_compressed_sizes = (PackedInt32Array)header_info.get("chunk_compressed_sizes", PackedInt32Array());
		r_out.chunk_uncompressed_sizes = (PackedInt32Array)header_info.get("chunk_uncompressed_sizes", PackedInt32Array());
	}
	return true;
}

bool DSPreloader::fill_from_dstex_file(const String &p_engine_path, const String &p_dstex_path, Payload &r_out) {
	// Fast path: generation-keyed header cache. Positive hit costs one stat
	// (get_modified_time) and zero opens, zero Dictionary allocs. Failure
	// hit (open_failed) costs the same stat and fails without opening —
	// repeat unresolvable/removed dstex paths never pay a second open.
	PayloadHeader cached;
	if (try_get_cached_header(p_dstex_path, cached)) {
		if (cached.open_failed) {
			return false;
		}
		if (!payload_from_header(p_engine_path, p_dstex_path, cached, r_out)) {
			mark_header_failed(p_dstex_path);
			return false;
		}
		return true;
	}
	// Miss path: single-open single-bulk read, then publish
	// first-publisher-wins (positive on success, failure entry on failure).
	PayloadHeader fresh;
	if (!read_header_uncached(p_dstex_path, fresh)) {
		publish_header_cache(p_dstex_path, fresh); // fresh is the failure entry.
		return false;
	}
	publish_header_cache(p_dstex_path, fresh);
	if (!payload_from_header(p_engine_path, p_dstex_path, fresh, r_out)) {
		// The fixed header parsed but the payload builder rejected the file
		// (chunk table). Record it so repeats fail with one stat, no open.
		mark_header_failed(p_dstex_path);
		return false;
	}
	return true;
}

bool DSPreloader::fill_from_import_remap(const String &p_engine_path, Payload &r_out) {
	// Import-remap skip (profiled): the .import file is exactly what
	// ConfigFile::load below needs, so test it directly. A missing .import
	// file means the load MUST fail — skipping is outcome-identical to
	// attempting it, at ~7 usec instead of ~40 usec for the failed
	// open+parse. A present .import file proceeds to the load exactly as
	// before, so remap order and all success paths are preserved,
	// including remapped/UID-backed sources (their .import file still
	// exists).
	// NOTE: an earlier gate (source FileAccess::file_exists +
	// ResourceLoader::exists) measured slower on a 100-path test:
	// ResourceLoader::exists() averages ~85 usec/call (full loader
	// iteration per path), worse than the ~40 usec parse it skipped —
	// warm enqueue stayed at ~9 ms. The engine call is dropped in favor
	// of this single .import probe.
	if (!FileAccess::file_exists(p_engine_path + String(".import"))) {
		return false;
	}
	Ref<ConfigFile> import_cfg;
	import_cfg.instantiate();
	if (import_cfg->load(p_engine_path + String(".import")) != OK) {
		return false;
	}
	String remapped = import_cfg->get_value("remap", "path", String());
	if (remapped.is_empty() || remapped.get_extension().to_lower() != "dstex") {
		return false;
	}
	if (!FileAccess::file_exists(remapped)) {
		return false;
	}
	return fill_from_dstex_file(p_engine_path, remapped, r_out);
}

bool DSPreloader::resolve_payload(const String &p_engine_path, Payload &r_out) {
	// Order mirrors runtime reality: exported builds serve every texture
	// from the deployed archive; editor runs serve the importer's .dstex
	// via the .import remap table. The direct-file branch below exists only
	// editor smoke tests that address an imported .dstex path directly keep
	// working; it is NOT a public load-a-.dstex API.
	if (fill_from_dspkg(p_engine_path, r_out)) {
		return true;
	}
	if (is_engine_source_path(p_engine_path) && fill_from_import_remap(p_engine_path, r_out)) {
		return true;
	}
	String ext = p_engine_path.get_extension().to_lower();
	if (ext == "dstex" && FileAccess::file_exists(p_engine_path)) {
		return fill_from_dstex_file(p_engine_path, p_engine_path, r_out);
	}
	return false;
}

bool DSPreloader::validate_payload(const Payload &p_payload) {
	if (p_payload.width == 0 || p_payload.height == 0 || p_payload.mip_count == 0) {
		UtilityFunctions::push_warning("DSPreloader: Invalid dimensions or mip count for '", p_payload.engine_path, "'; skipping prewarm.");
		return false;
	}
	if (!dstex_sizes_are_sane(p_payload.width, p_payload.height, p_payload.original_size, p_payload.compressed_size)) {
		UtilityFunctions::push_warning("DSPreloader: Header sizes exceed sane texture bounds for '", p_payload.engine_path, "'; skipping prewarm.");
		return false;
	}
	if (p_payload.pixel_format >= (uint32_t)RenderingDevice::DATA_FORMAT_MAX) {
		UtilityFunctions::push_warning("DSPreloader: Unsupported texture data format for '", p_payload.engine_path, "'; skipping prewarm.");
		return false;
	}
	const int img_fmt = DSTex::rd_format_to_godot_format((int)p_payload.pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		UtilityFunctions::push_warning("DSPreloader: Texture format cannot be represented as a 2D image for '", p_payload.engine_path, "'; skipping prewarm.");
		return false;
	}
	if (p_payload.is_v2) {
		int64_t mips = (int64_t)p_payload.mip_count;
		if (p_payload.mip_offsets.size() < mips || p_payload.mip_sizes.size() < mips || p_payload.mip_uncompressed_sizes.size() < mips) {
			UtilityFunctions::push_warning("DSPreloader: Incomplete v2 mip metadata for '", p_payload.engine_path, "'; skipping prewarm.");
			return false;
		}
		// expand_requests() resizes one destination buffer per mip from these
		// values, so their SUM is the allocation bound, not each value. A
		// well-formed file sums to original_size, which the check above already
		// caps; this only rejects a header whose mip table contradicts it.
		uint64_t mip_total = 0;
		for (int64_t i = 0; i < mips; ++i) {
			const int64_t uncompressed = (int64_t)p_payload.mip_uncompressed_sizes[i];
			if (uncompressed <= 0) {
				UtilityFunctions::push_warning("DSPreloader: Invalid v2 mip size (mip ", i, ") for '", p_payload.engine_path, "'; skipping prewarm.");
				return false;
			}
			mip_total += (uint64_t)uncompressed;
		}
		if (mip_total > MAX_PREWARM_REQUEST_BYTES) {
			UtilityFunctions::push_warning("DSPreloader: v2 mip footprints exceed the prewarm buffer cap for '", p_payload.engine_path, "'; skipping prewarm.");
			return false;
		}
		if (p_payload.chunked) {
			if (p_payload.mip_chunk_counts.size() < mips ||
					p_payload.chunk_compressed_sizes.size() != p_payload.chunk_offsets.size() ||
					p_payload.chunk_uncompressed_sizes.size() != p_payload.chunk_offsets.size()) {
				UtilityFunctions::push_warning("DSPreloader: Incomplete chunk table for '", p_payload.engine_path, "'; skipping prewarm.");
				return false;
			}
			int64_t chunk_cursor = 0;
			for (int64_t mip = 0; mip < mips; ++mip) {
				const int32_t count = p_payload.mip_chunk_counts[mip];
				if (count < 1) {
					UtilityFunctions::push_warning("DSPreloader: Invalid chunk count for '", p_payload.engine_path, "'; skipping prewarm.");
					return false;
				}
				if (count <= 1) {
					continue;
				}
				uint64_t compressed_sum = 0;
				uint64_t uncompressed_sum = 0;
				for (int32_t c = 0; c < count; ++c, ++chunk_cursor) {
					if (chunk_cursor >= p_payload.chunk_compressed_sizes.size() ||
							chunk_cursor >= p_payload.chunk_uncompressed_sizes.size()) {
						UtilityFunctions::push_warning("DSPreloader: Chunk table ends early for '", p_payload.engine_path, "'; skipping prewarm.");
						return false;
					}
					const int32_t compressed = p_payload.chunk_compressed_sizes[chunk_cursor];
					const int32_t uncompressed = p_payload.chunk_uncompressed_sizes[chunk_cursor];
					if (compressed <= 0 || uncompressed <= 0) {
						UtilityFunctions::push_warning("DSPreloader: Invalid chunk size for '", p_payload.engine_path, "'; skipping prewarm.");
						return false;
					}
					compressed_sum += static_cast<uint64_t>(compressed);
					uncompressed_sum += static_cast<uint64_t>(uncompressed);
				}
				if (compressed_sum != static_cast<uint64_t>(p_payload.mip_sizes[mip]) ||
						uncompressed_sum != static_cast<uint64_t>(p_payload.mip_uncompressed_sizes[mip])) {
					UtilityFunctions::push_warning("DSPreloader: Chunk sums do not match mip ", mip, " for '", p_payload.engine_path, "'; skipping prewarm.");
					return false;
				}
			}
			if (chunk_cursor != p_payload.chunk_offsets.size()) {
				UtilityFunctions::push_warning("DSPreloader: Chunk table overruns its mip counts for '", p_payload.engine_path, "'; skipping prewarm.");
				return false;
			}
		}
	} else {
		if (p_payload.compressed_size == 0 || p_payload.compressed_size > UINT32_MAX || p_payload.original_size == 0 || p_payload.original_size > UINT32_MAX) {
			UtilityFunctions::push_warning("DSPreloader: Invalid v1 payload size for '", p_payload.engine_path, "'; skipping prewarm.");
			return false;
		}
		if (p_payload.original_size > MAX_PREWARM_REQUEST_BYTES) {
			UtilityFunctions::push_warning("DSPreloader: Payload exceeds prewarm buffer cap for '", p_payload.engine_path, "'; skipping prewarm.");
			return false;
		}
	}
	return true;
}

bool DSPreloader::expand_requests(const Payload &p_payload, uint64_t p_tag, Vector<Ref<DSRequest>> &r_out) {
	r_out.clear();
	// Pre-size once: the per-mip push_back loop below must not pay one
	// realloc per mip. The destination output buffer resize() per request
	// is load-bearing (decompression target) and stays.
	if (!p_payload.is_v2) {
		r_out.reserve(1);
		Ref<DSRequest> req;
		req.instantiate();
		req->set_file_path(p_payload.file_path);
		req->set_offset(p_payload.data_offset);
		req->set_size((uint32_t)p_payload.compressed_size);
		req->set_uncompressed_size((uint32_t)p_payload.original_size);
		req->set_compression_format(p_payload.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE)
				? DSRequest::COMPRESSION_GDEFLATE : DSRequest::COMPRESSION_NONE);
		req->set_cancellation_tag(p_tag);
		r_out.push_back(req);
		return true;
	}
	r_out.reserve((int64_t)p_payload.mip_count);
	if (p_payload.chunked) {
		// One request per chunk so each stream gets its own destination buffer;
		// complete_cpu_entry() concatenates the buffers back into one footprint
		// per mip before build_texture() sees them.
		int64_t chunk_cursor = 0;
		for (uint32_t i = 0; i < p_payload.mip_count; ++i) {
			const int32_t count = p_payload.mip_chunk_counts.size() > static_cast<int64_t>(i)
					? p_payload.mip_chunk_counts[i]
					: 1;
			if (count <= 1) {
				Ref<DSRequest> req;
				req.instantiate();
				req->set_file_path(p_payload.file_path);
				req->set_offset(static_cast<uint64_t>(p_payload.mip_offsets[i]));
				req->set_size(static_cast<uint32_t>(p_payload.mip_sizes[i]));
				req->set_uncompressed_size(static_cast<uint32_t>(p_payload.mip_uncompressed_sizes[i]));
				req->set_compression_format(p_payload.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE)
								? DSRequest::COMPRESSION_GDEFLATE
								: DSRequest::COMPRESSION_NONE);
				req->set_cancellation_tag(p_tag);
				r_out.push_back(req);
				continue;
			}
			for (int32_t c = 0; c < count; ++c, ++chunk_cursor) {
				if (chunk_cursor >= p_payload.chunk_offsets.size() ||
						chunk_cursor >= p_payload.chunk_compressed_sizes.size() ||
						chunk_cursor >= p_payload.chunk_uncompressed_sizes.size()) {
					r_out.clear();
					return false;
				}
				Ref<DSRequest> req;
				req.instantiate();
				req->set_file_path(p_payload.file_path);
				req->set_offset(static_cast<uint64_t>(p_payload.chunk_offsets[chunk_cursor]));
				req->set_size(static_cast<uint32_t>(p_payload.chunk_compressed_sizes[chunk_cursor]));
				req->set_uncompressed_size(static_cast<uint32_t>(p_payload.chunk_uncompressed_sizes[chunk_cursor]));
				req->set_compression_format(p_payload.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE)
								? DSRequest::COMPRESSION_GDEFLATE
								: DSRequest::COMPRESSION_NONE);
				req->set_cancellation_tag(p_tag);
				r_out.push_back(req);
			}
		}
		return r_out.size() > 0;
	}
	// Raw table pointers hoisted: per-mip operator[] on Packed*Array pays
	// a bounds check + CoW read-lock probe per access (4 per mip); plain
	// pointer reads return identical values with no per-iteration
	// overhead. Pointers are taken once and the Payload is
	// const through this loop, so no revalidation is needed. Validation
	// bounds are unchanged (same rejects, same warning, same clear+false).
	const int64_t *offsets = p_payload.mip_offsets.ptr();
	const int32_t *sizes = p_payload.mip_sizes.ptr();
	const int32_t *uncomps = p_payload.mip_uncompressed_sizes.ptr();
	for (uint32_t i = 0; i < p_payload.mip_count; ++i) {
		uint64_t size = (uint64_t)(int64_t)sizes[i];
		uint64_t uncomp = (uint64_t)(int64_t)uncomps[i];
		if (size == 0 || size > UINT32_MAX || uncomp == 0 || uncomp > UINT32_MAX || uncomp > MAX_PREWARM_REQUEST_BYTES) {
			UtilityFunctions::push_warning("DSPreloader: Invalid v2 mip payload size (mip ", (int64_t)i, ") for '", p_payload.engine_path, "'; skipping prewarm.");
			r_out.clear();
			return false;
		}
		Ref<DSRequest> req;
		req.instantiate();
		req->set_file_path(p_payload.file_path);
		req->set_offset((uint64_t)offsets[i]);
		req->set_size((uint32_t)size);
		req->set_uncompressed_size((uint32_t)uncomp);
		req->set_compression_format(p_payload.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE)
				? DSRequest::COMPRESSION_GDEFLATE : DSRequest::COMPRESSION_NONE);
		req->set_cancellation_tag(p_tag);
		r_out.push_back(req);
	}
	return r_out.size() > 0;
}

void DSPreloader::collect_prewarm_payloads(const PackedStringArray &p_paths, std::vector<Payload> &r_payloads, int &r_skipped_cached, int &r_skipped_unresolvable) {
	DSTextureCache *cache = DSTextureCache::get_singleton();
	ResourceLoader *rl = ResourceLoader::get_singleton();

	HashMap<String, bool> seen;
	r_payloads.reserve((size_t)p_paths.size());
	for (int i = 0; i < p_paths.size(); ++i) {
		String path = p_paths[i].strip_edges();
		if (path.is_empty() || seen.has(path)) {
			continue;
		}
		seen[path] = true;
		// Dedup against both resident stores before touching the queue, so
		// cached hits never pay enqueue + submit + completion for bytes
		// that are already resident. Probe order (measured):
		// DSTextureCache::get_cached FIRST (local mutex + hash lookup),
		// ResourceLoader::has_cached SECOND (engine call). On an all-miss
		// set both probes pay ~1 usec combined regardless of order, so the
		// order is decided by the hit path: cache-first short-circuits
		// before the engine call, and a texture-cache hit implies engine
		// residency anyway (store() takes over the path), making the
		// engine probe redundant on hit. No lock-nesting change;
		// take_over_path stays outside the cache lock.
		if (cache) {
			Ref<Texture2D> hit = cache->get_cached(path, DSTextureCache::CACHE_MODE_REUSE);
			if (hit.is_valid()) {
				r_skipped_cached++;
				continue;
			}
		}
		if (rl && rl->has_cached(path)) {
			r_skipped_cached++;
			continue;
		}
		Payload payload;
		if (!resolve_payload(path, payload) || !validate_payload(payload)) {
			r_skipped_unresolvable++;
			continue;
		}
		// Payload bytes already cached under the remapped key (e.g. a sync
		// _load stored the .dstex key but the engine path alias is missing):
		// alias the engine path without spending a stream request.
		if (cache && payload.payload_key != payload.engine_path) {
			Ref<Texture2D> payload_hit = cache->get_cached(payload.payload_key, DSTextureCache::CACHE_MODE_REUSE);
			if (payload_hit.is_valid()) {
				cache->add_alias(payload.payload_key, payload.engine_path);
				r_skipped_cached++;
				continue;
			}
		}
		r_payloads.push_back(payload);
	}
}

uint64_t DSPreloader::prewarm_async(const PackedStringArray &p_paths) {
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr) {
		UtilityFunctions::push_error("DSPreloader: DSContext singleton unavailable; prewarm_async aborted.");
		return 0;
	}
	// #0126 symmetry with prewarm_gpu_async(): a prewarm whose textures cannot
	// be stored has no contract value, so it is refused once, before payload
	// resolution, instead of streaming and CPU-decoding textures that are
	// dropped at completion (one verbose line each). Page-cache warming is not
	// a documented promise of this API; a caller that wants it can read the
	// files directly.
	DSTextureCache *prewarm_cache = DSTextureCache::get_singleton();
	if (prewarm_cache == nullptr || !prewarm_cache->can_own(DSTextureCache::CACHE_MODE_REUSE)) {
		UtilityFunctions::push_warning("DSPreloader: texture cache cannot own prewarmed textures (disabled or unavailable); prewarm_async skipped.");
		return 0;
	}
	std::vector<Payload> payloads;
	int skipped_cached = 0;
	int skipped_unresolvable = 0;
	collect_prewarm_payloads(p_paths, payloads, skipped_cached, skipped_unresolvable);

	// Tag first with an empty list (total 0); each stream enqueue below bumps
	// the tag total by exactly one, so total == tracked request count and
	// is_prewarm_complete() settles exactly when every request is finished.
	uint64_t tag = ctx->prewarm_async(PackedStringArray());
	int enqueued = 0;
	// Stutter guard: expand + stream-enqueue run WITHOUT state().mutex held.
	// The old code held the preloader lock across the whole loop (buffer
	// resizes + per-request stream enqueue for every payload), so the batch
	// manager's per-frame get_unbuilt_prewarm_count() on the main thread
	// blocked for the full enqueue duration (~tens of ms -> Frame #3 hitch).
	// Stream enqueue only takes the context's stream_mutex, so no preloader
	// lock is needed here; pending-table publication below is one short
	// critical section with identical visibility semantics.
	// Reused scratch vector: expand_requests() starts with
	// clear()+reserve(), so one buffer serves all payloads and only
	// the largest mip_count reallocates. tracked.requests = reqs below
	// copies the Refs out (pending holds them), so reuse is
	// use-after-clear safe. enqueue_request(req, true) semantics and
	// the all-or-nothing per-payload failure path are untouched.
	std::vector<Tracked> ready;
	ready.reserve(payloads.size());
	{
		Vector<Ref<DSRequest>> reqs;
		for (size_t i = 0; i < payloads.size(); ++i) {
			if (!expand_requests(payloads[i], tag, reqs)) {
				skipped_unresolvable++;
				continue;
			}
			bool all_queued = true;
			for (int r = 0; r < reqs.size(); ++r) {
				if (!ctx->enqueue_request(reqs[r], true)) {
					all_queued = false;
					break;
				}
			}
			if (!all_queued) {
				// from_stream enqueue only fails for a cancelled tag or a
				// null request (neither expected for a fresh tag); mark
				// terminal so poll_completed() drops the entry instead of
				// stalling is_prewarm_complete() on it.
				for (int r = 0; r < reqs.size(); ++r) {
					if (reqs[r].is_valid() && reqs[r]->get_status() != DSRequest::STATUS_COMPLETED && reqs[r]->get_status() != DSRequest::STATUS_FAILED) {
						reqs[r]->set_status(DSRequest::STATUS_FAILED);
						reqs[r]->set_error_message("Prewarm enqueue rejected.");
					}
				}
				UtilityFunctions::push_warning("DSPreloader: Stream enqueue rejected for '", payloads[i].engine_path, "'; skipping.");
				skipped_unresolvable++;
				continue;
			}
			Tracked tracked;
			tracked.tag = tag;
			tracked.meta = payloads[i];
			tracked.requests = reqs;
			ready.push_back(tracked);
			enqueued += reqs.size();
		}
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < ready.size(); ++i) {
			int64_t id = s.next_id++;
			if (s.next_id == 0) {
				s.next_id = 1;
			}
			s.pending[id] = ready[i];
			s.pending_paths[ready[i].meta.engine_path]++;
			if (ready[i].meta.payload_key != ready[i].meta.engine_path) {
				s.pending_paths[ready[i].meta.payload_key]++;
			}
		}
	}
	if (enqueued > 0) {
		ctx->flush_stream_async();
	}
	UtilityFunctions::print_verbose("DSPreloader: prewarm tag ", String::num_uint64(tag), " tracking ", (int64_t)payloads.size(), " textures (", (int64_t)enqueued, " stream requests, ", (int64_t)skipped_cached, " cached, ", (int64_t)skipped_unresolvable, " skipped).");
	return tag;
}

static size_t count_cpu_requests_for_payload(const DSPreloader::Payload &p_meta) {
	if (!p_meta.is_v2 || p_meta.mip_count == 0) {
		return 1;
	}
	if (!p_meta.chunked) {
		return static_cast<size_t>(p_meta.mip_count);
	}
	// Mirror expand_requests(): a chunked v3 payload emits one request per
	// chunk (or one whole-mip request when a mip has <= 1 chunk). This total
	// is locked at resolution time and drives is_prewarm_complete(), so it
	// must count the same units the enqueue loop actually produces.
	size_t requests = 0;
	for (uint32_t i = 0; i < p_meta.mip_count; ++i) {
		const int32_t chunks = p_meta.mip_chunk_counts.size() > static_cast<int64_t>(i)
				? p_meta.mip_chunk_counts[i]
				: 1;
		requests += (chunks > 1) ? static_cast<size_t>(chunks) : 1;
	}
	return requests;
}

uint64_t DSPreloader::prewarm_cpu_paced_async(const PackedStringArray &p_paths) {
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr) {
		UtilityFunctions::push_error("DSPreloader: DSContext singleton unavailable; prewarm_cpu_paced_async aborted.");
		return 0;
	}
	DSTextureCache *prewarm_cache = DSTextureCache::get_singleton();
	if (prewarm_cache == nullptr || !prewarm_cache->can_own(DSTextureCache::CACHE_MODE_REUSE)) {
		UtilityFunctions::push_warning("DSPreloader: texture cache cannot own prewarmed textures (disabled or unavailable); prewarm_cpu_paced_async skipped.");
		return 0;
	}
	std::vector<Payload> payloads;
	int skipped_cached = 0;
	int skipped_unresolvable = 0;
	collect_prewarm_payloads(p_paths, payloads, skipped_cached, skipped_unresolvable);

	uint64_t tag = ctx->prewarm_async(PackedStringArray());
	size_t total_units = 0;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < payloads.size(); ++i) {
			int64_t id = s.next_id++;
			if (s.next_id == 0) {
				s.next_id = 1;
			}
			Tracked t;
			t.tag = tag;
			t.meta = payloads[i];
			t.is_gpu = false;
			t.cpu_queued = true;
			s.pending[id] = t;
			s.pending_paths[t.meta.engine_path]++;
			if (t.meta.payload_key != t.meta.engine_path) {
				s.pending_paths[t.meta.payload_key]++;
			}
			CpuQueueItem item;
			item.pending_id = id;
			item.tag = tag;
			item.meta = payloads[i];
			s.cpu_queue.push_back(item);
			total_units += count_cpu_requests_for_payload(payloads[i]);
		}
	}
	ctx->set_prewarm_total(tag, static_cast<int>(total_units));
	UtilityFunctions::print_verbose("DSPreloader: paced cpu prewarm tag ", String::num_uint64(tag), " queued ", (int64_t)payloads.size(), " textures (", (int64_t)total_units, " units, ", (int64_t)skipped_cached, " cached, ", (int64_t)skipped_unresolvable, " skipped).");
	return tag;
}

void DSPreloader::pump_cpu_prewarm(int p_max_items, int64_t p_max_bytes) {
	if (p_max_items <= 0) {
		return;
	}
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr) {
		return;
	}

	std::vector<CpuQueueItem> batch;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		while (!s.cpu_queue.empty() && ctx->is_tag_cancelled(s.cpu_queue.front().tag)) {
			CpuQueueItem dead = s.cpu_queue.front();
			s.cpu_queue.pop_front();
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(dead.pending_id);
			if (it != s.pending.end()) {
				unblock_pending_paths_locked(it->second);
				s.pending.erase(it);
			}
			count_gpu_shortfall(ctx, dead.tag, count_cpu_requests_for_payload(dead.meta));
		}

		int64_t bytes_accum = 0;
		const int64_t byte_limit = p_max_bytes > 0 ? p_max_bytes : INT64_MAX;
		while (!s.cpu_queue.empty() && static_cast<int>(batch.size()) < p_max_items) {
			const CpuQueueItem &front = s.cpu_queue.front();
			const int64_t item_bytes = static_cast<int64_t>(estimate_payload_bytes(front.meta));
			if (!batch.empty() && bytes_accum + item_bytes > byte_limit) {
				break;
			}
			batch.push_back(front);
			s.cpu_queue.pop_front();
			bytes_accum += item_bytes;
		}
	}

	if (batch.empty()) {
		return;
	}

	struct ExpandedItem {
		int64_t pending_id = 0;
		uint64_t tag = 0;
		size_t expected_units = 1;
		bool ok = false;
		Vector<Ref<DSRequest>> reqs;
	};
	std::vector<ExpandedItem> expanded;
	expanded.resize(batch.size());
	int total_enqueued = 0;

	for (size_t i = 0; i < batch.size(); ++i) {
		ExpandedItem &out = expanded[i];
		out.pending_id = batch[i].pending_id;
		out.tag = batch[i].tag;
		out.expected_units = count_cpu_requests_for_payload(batch[i].meta);
		if (ctx->is_tag_cancelled(out.tag)) {
			continue;
		}
		if (!expand_requests(batch[i].meta, out.tag, out.reqs)) {
			continue;
		}
		bool all_queued = true;
		for (int r = 0; r < out.reqs.size(); ++r) {
			if (!ctx->enqueue_request(out.reqs[r], true)) {
				all_queued = false;
				break;
			}
		}
		if (!all_queued) {
			for (int r = 0; r < out.reqs.size(); ++r) {
				if (out.reqs[r].is_valid() && out.reqs[r]->get_status() != DSRequest::STATUS_COMPLETED && out.reqs[r]->get_status() != DSRequest::STATUS_FAILED) {
					out.reqs[r]->set_status(DSRequest::STATUS_FAILED);
					out.reqs[r]->set_error_message("Paced CPU prewarm enqueue rejected.");
				}
			}
			continue;
		}
		out.ok = true;
		total_enqueued += out.reqs.size();
	}

	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < expanded.size(); ++i) {
			const ExpandedItem &item = expanded[i];
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(item.pending_id);
			if (item.ok && it != s.pending.end()) {
				it->second.requests = item.reqs;
				it->second.cpu_queued = false;
			} else {
				if (it != s.pending.end()) {
					unblock_pending_paths_locked(it->second);
					s.pending.erase(it);
				}
				count_gpu_shortfall(ctx, item.tag, item.expected_units);
			}
		}
	}

	if (total_enqueued > 0) {
		ctx->flush_stream_async();
	}
}

int DSPreloader::get_queued_cpu_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	const State &s = state();
	if (p_tag == 0) {
		return static_cast<int>(s.cpu_queue.size());
	}
	int count = 0;
	for (size_t i = 0; i < s.cpu_queue.size(); ++i) {
		if (s.cpu_queue[i].tag == p_tag) {
			count++;
		}
	}
	return count;
}

int DSPreloader::get_in_flight_cpu_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	const State &s = state();
	int count = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		const Tracked &t = it->second;
		if ((p_tag != 0 && t.tag != p_tag) || t.is_gpu || t.cpu_queued || t.requests.is_empty()) {
			continue;
		}
		for (int i = 0; i < t.requests.size(); ++i) {
			if (!is_terminal(t.requests[i])) {
				count++;
				break;
			}
		}
	}
	return count;
}

int64_t DSPreloader::get_in_flight_cpu_prewarm_bytes(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	const State &s = state();
	int64_t bytes = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		const Tracked &t = it->second;
		if ((p_tag != 0 && t.tag != p_tag) || t.is_gpu || t.cpu_queued || t.requests.is_empty()) {
			continue;
		}
		for (int i = 0; i < t.requests.size(); ++i) {
			if (!is_terminal(t.requests[i])) {
				bytes += static_cast<int64_t>(estimate_payload_bytes(t.meta));
				break;
			}
		}
	}
	return bytes;
}

// --- Phase 2: prewarm-owned GPU path ---------------------------------------
// Design: texture_create + stream-enqueue + flush-attempt all run inside
// one render-thread callback per chunk, so backend destination queries
// run inline (zero cross-thread rendezvous). A main-thread batch-manager
// flush stays as the rare ERR_BUSY fallback (correct, pays rendezvous).

bool DSPreloader::main_or_render_thread() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs != nullptr && rs->is_on_render_thread()) {
		return true;
	}
	return Thread::is_main_thread();
}

// A format the GPU prewarm path accepts. Color formats qualify through the
// sRGB pairing the texture-create path already declares (Q44 shareable-format
// list: base + counterpart, or no list at all). Linear-only *block* formats
// carry 3D material data (normal, roughness, AO, height) and have no sRGB view
// at all, so both paths declare no shareable list - which Q44 explicitly
// allows for exactly these formats. They are worth the GPU path because their
// CPU route is the expensive one: a 4K BC5 normal map is ~21 MiB of CPU
// GDeflate + unpad + engine upload per texture. Uncompressed and float data
// formats (R8/RG8/RGB8, the R16 family, floats, BC6H) stay on the memory path:
// they are cheap to move, are plausible 2D/UI data, and 2D DRD sampling is not
// re-verified yet (.agents/plans/finished_plans/prewarm_owned_gpu_path_plan.md,
// "2D vs 3D asset policy").
static bool ds_prewarm_format_eligible(int p_rd_format) {
	if (DSTex::is_rd_format_srgb(p_rd_format)) {
		return true;
	}
	const int srgb_format = DSTex::get_srgb_rd_format(p_rd_format);
	if (srgb_format != RenderingDevice::DATA_FORMAT_MAX && srgb_format != p_rd_format) {
		return true;
	}
	return p_rd_format == RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK ||
			p_rd_format == RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
}

bool DSPreloader::is_gpu_prewarm_eligible(const Payload &p_payload) {
	if (!p_payload.is_v2) {
		return false;
	}
	if (p_payload.as_image_texture) {
		return false;
	}
	if (p_payload.width == 0 || p_payload.height == 0 || p_payload.mip_count == 0) {
		return false;
	}
	if (p_payload.pixel_format >= (uint32_t)RenderingDevice::DATA_FORMAT_MAX) {
		return false;
	}
	if (!ds_prewarm_format_eligible(static_cast<int>(p_payload.pixel_format))) {
		return false;
	}
	const int64_t mips = (int64_t)p_payload.mip_count;
	if (p_payload.mip_offsets.size() < mips || p_payload.mip_sizes.size() < mips ||
			p_payload.mip_uncompressed_sizes.size() < mips) {
		return false;
	}
	if (p_payload.chunked) {
		// Chunked payloads upload through per-chunk row-range requests (the
		// same shape the loader's sync GPU path emits). The tables were fully
		// validated at collect time; re-check the shape cheaply here because
		// availability may have changed between pump and alloc. Both codecs
		// are eligible: a NONE chunk is a raw row slice the backend uploads
		// without decompression.
		if (p_payload.compression_format != static_cast<uint8_t>(DSCompressionFormat::GDEFLATE) &&
				p_payload.compression_format != static_cast<uint8_t>(DSCompressionFormat::NONE)) {
			return false;
		}
		// The walker is the single definition of a valid upload shape, so it
		// validates the table (lengths, coverage, pitches, row alignment, mip
		// sums) and the alloc path cannot disagree with this decision.
		return walk_gpu_upload_units(p_payload,
				[](uint32_t, int64_t, uint32_t, uint32_t) {
					return true;
				});
	}
	return true;
}

bool DSPreloader::is_gpu_prewarm_available() {
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr || !ctx->is_initialized()) {
		return false;
	}
	if (!ctx->is_d3d12_backend() || !ctx->is_gpu_decompression_enabled()) {
		return false;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return false;
	}
	return rs->get_rendering_device() != nullptr;
}

void DSPreloader::mark_gpu_item_failed(int64_t p_pending_id) {
	std::lock_guard<std::mutex> lock(state().mutex);
	std::unordered_map<int64_t, Tracked>::iterator it = state().pending.find(p_pending_id);
	if (it != state().pending.end()) {
		it->second.gpu_failed = true;
	}
}

void DSPreloader::queue_gpu_orphan_rid(const RID &p_rid) {
	if (!p_rid.is_valid()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().gpu_orphans.push_back(p_rid);
	}
	pump_gpu_orphans();
}

void DSPreloader::pump_gpu_orphans() {
	bool need_post = false;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (s.gpu_orphans.empty() || s.gpu_orphan_free_posted) {
			return;
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs == nullptr) {
			// No RenderingDevice exists on this host (e.g. headless CI):
			// orphan RIDs are meaningless here, drop them.
			s.gpu_orphans.clear();
			return;
		}
		s.gpu_orphan_free_posted = true;
		need_post = true;
	}
	if (need_post) {
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr) {
			rs->call_on_render_thread(callable_mp_static(free_gpu_orphans_on_render_thread));
		}
	}
}

void DSPreloader::free_gpu_orphans_on_render_thread() {
	std::vector<RID> orphans;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		orphans.swap(s.gpu_orphans);
		s.gpu_orphan_free_posted = false;
	}
	if (orphans.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || !rs->is_on_render_thread()) {
		// Wrong thread (should not happen): hand back for a later pump.
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		s.gpu_orphans.insert(s.gpu_orphans.begin(), orphans.begin(), orphans.end());
		return;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	if (rd == nullptr) {
		return;
	}
	// Never free inline. A cancelled GPU prewarm is exactly the case where the
	// destination texture may still be under live DMA: cancel_prewarm() cannot stop
	// a copy the hardware already started, and the parent request only stops
	// reporting is_gpu_in_flight() once its fence retires. release_owned_rid()
	// applies the shared discipline instead - free now on the render thread, or
	// defer to the evict queue and retry on a later frame while
	// DSContext::is_resource_in_flight() still reports the RID in use.
	for (size_t i = 0; i < orphans.size(); ++i) {
		if (orphans[i].is_valid()) {
			DSTextureCache::release_owned_rid(orphans[i]);
		}
	}
}

uint64_t DSPreloader::prewarm_gpu_async(const PackedStringArray &p_paths, int p_chunk_size, bool p_prefer_gpu, int64_t p_max_chunk_bytes) {
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr) {
		UtilityFunctions::push_error("DSPreloader: DSContext singleton unavailable; prewarm_gpu_async aborted.");
		return 0;
	}

	// #0126: with a disabled (or absent) cache nothing can be stored, so the
	// whole call is refused here - not just the VRAM work. That includes this
	// function's internal memory/CPU route (prefer_gpu=false, auto-tuned iGPUs
	// and GPU-ineligible payloads): those textures could not be stored either,
	// so they would only burn I/O and CPU before being dropped. Refusing before
	// payload resolution also avoids one completion warning per texture.
	DSTextureCache *prewarm_cache = DSTextureCache::get_singleton();
	if (prewarm_cache == nullptr || !prewarm_cache->can_own(DSTextureCache::CACHE_MODE_REUSE)) {
		UtilityFunctions::push_warning("DSPreloader: texture cache cannot own prewarmed textures (disabled or unavailable); prewarm_gpu_async skipped.");
		return 0;
	}

	std::vector<Payload> payloads;
	int skipped_cached = 0;
	int skipped_unresolvable = 0;
	collect_prewarm_payloads(p_paths, payloads, skipped_cached, skipped_unresolvable);

	// Evaluated once up front: render-side alloc re-validates per chunk, and
	// anything ineligible (or late-unavailable GPU) stays on the memory path.
	bool prefer_gpu = p_prefer_gpu;
	if (prefer_gpu && ctx->get_auto_tune_for_integrated_gpu() && ctx->is_integrated_gpu()) {
		prefer_gpu = false;
		UtilityFunctions::print("[DirectStorage] Integrated/UMA GPU detected: prewarm_gpu_async auto-routed to System RAM prewarm to eliminate loading screen hitches (dsgd_extension/auto_tune_for_integrated_gpu=true).");
	}
	const bool gpu_ok = prefer_gpu && is_gpu_prewarm_available();
	const int chunk_size = p_chunk_size > 0 ? p_chunk_size : 16;

	// Tag first with an empty list (total 0); each stream enqueue bumps the
	// total by exactly one on BOTH paths, so total == tracked request count.
	uint64_t tag = ctx->prewarm_async(PackedStringArray());
	int enqueued = 0;
	int gpu_queued = 0;

	// Memory path: identical stutter-guarded enqueue to prewarm_async
	// (expand + stream-enqueue WITHOUT state().mutex held; one short
	// publish section below).
	std::vector<Tracked> ready;
	ready.reserve(payloads.size());
	std::vector<GpuChunkItem> gpu_items;
	gpu_items.reserve(payloads.size());
	// Count non-multiple-of-4 payloads that take the GPU path. Their wrap reports
	// the padded storage size (Texture2DRD has no size setter), so surface one
	// batch-level warning instead of one line per texture.
	int npot_gpu = 0;
	{
		Vector<Ref<DSRequest>> reqs;
		for (size_t i = 0; i < payloads.size(); ++i) {
			if (gpu_ok && is_gpu_prewarm_eligible(payloads[i])) {
				GpuChunkItem item;
				item.meta = payloads[i];
				item.pending_id = 0; // assigned at publish below
				gpu_items.push_back(item);
				if (payloads[i].logical_width != 0 || payloads[i].logical_height != 0) {
					npot_gpu++;
				}
				continue;
			}
			if (!expand_requests(payloads[i], tag, reqs)) {
				skipped_unresolvable++;
				continue;
			}
			bool all_queued = true;
			for (int r = 0; r < reqs.size(); ++r) {
				if (!ctx->enqueue_request(reqs[r], true)) {
					all_queued = false;
					break;
				}
			}
			if (!all_queued) {
				for (int r = 0; r < reqs.size(); ++r) {
					if (reqs[r].is_valid() && reqs[r]->get_status() != DSRequest::STATUS_COMPLETED && reqs[r]->get_status() != DSRequest::STATUS_FAILED) {
						reqs[r]->set_status(DSRequest::STATUS_FAILED);
						reqs[r]->set_error_message("Prewarm enqueue rejected.");
					}
				}
				UtilityFunctions::push_warning("DSPreloader: Stream enqueue rejected for '", payloads[i].engine_path, "'; skipping.");
				skipped_unresolvable++;
				continue;
			}
			Tracked tracked;
			tracked.tag = tag;
			tracked.meta = payloads[i];
			tracked.requests = reqs;
			ready.push_back(tracked);
			enqueued += reqs.size();
		}
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < ready.size(); ++i) {
			int64_t id = s.next_id++;
			if (s.next_id == 0) {
				s.next_id = 1;
			}
			s.pending[id] = ready[i];
			s.pending_paths[ready[i].meta.engine_path]++;
			if (ready[i].meta.payload_key != ready[i].meta.engine_path) {
				s.pending_paths[ready[i].meta.payload_key]++;
			}
		}
		// GPU entries are published upfront (no requests yet); the
		// render-thread chunk callback attaches RID + requests by id.
		GpuChunk chunk;
		chunk.tag = tag;
		for (size_t i = 0; i < gpu_items.size(); ++i) {
			int64_t id = s.next_id++;
			if (s.next_id == 0) {
				s.next_id = 1;
			}
			Tracked t;
			t.tag = tag;
			t.meta = gpu_items[i].meta;
			t.is_gpu = true;
			s.pending[id] = t;
			s.pending_paths[t.meta.engine_path]++;
			if (t.meta.payload_key != t.meta.engine_path) {
				s.pending_paths[t.meta.payload_key]++;
			}
			gpu_items[i].pending_id = id;
			chunk.items.push_back(gpu_items[i]);
			gpu_queued++;
		}
		// Slice into callbacks bounded by BOTH item count (chunk_size, the
		// historic bound) and estimated request count. A chunked texture fans
		// out to many requests, so item-count slicing alone would let one
		// callback stage hundreds of uploads on the render thread. Whole
		// textures are never split: an item that would overflow the request
		// cap starts the next chunk (an oversized single texture rides alone
		// and the render-side queue-budget guard diverts it if needed).
		// The cap is the smaller of the fixed per-callback constant and half the
		// queue capacity, so a caller that lowers queue_capacity cannot get a
		// callback that stages the entire queue in one go.
		const size_t callback_request_cap = std::min<size_t>(
				kGpuPrewarmRequestsPerCallback,
				static_cast<size_t>(std::max<uint32_t>(1, ctx->get_queue_capacity() / 2)));
		{
			// Per-item budget inputs first, then the shared planner decides the
			// boundaries (the same function the test seam asserts).
			std::vector<uint64_t> item_requests;
			std::vector<uint64_t> item_bytes;
			item_requests.reserve(chunk.items.size());
			item_bytes.reserve(chunk.items.size());
			for (size_t i = 0; i < chunk.items.size(); ++i) {
				item_requests.push_back(static_cast<uint64_t>(count_gpu_requests_for_payload(chunk.items[i].meta)));
				item_bytes.push_back(estimate_payload_bytes(chunk.items[i].meta));
			}
			const uint64_t bg_chunk_cap = 2ULL * 1024ULL * 1024ULL;
			const uint64_t default_chunk_cap = s.background_pacing.load(std::memory_order_relaxed) ? bg_chunk_cap : kGpuPrewarmBytesPerCallback;
			const uint64_t chunk_byte_cap = p_max_chunk_bytes > 0 ? static_cast<uint64_t>(p_max_chunk_bytes) : default_chunk_cap;
			const std::vector<GpuChunkSlice> slices = plan_gpu_chunk_slices(item_requests, item_bytes,
					static_cast<size_t>(std::max(1, chunk_size)), callback_request_cap, chunk_byte_cap);
			for (size_t slice_index = 0; slice_index < slices.size(); ++slice_index) {
				GpuChunk part;
				part.tag = tag;
				for (size_t i = slices[slice_index].first; i < slices[slice_index].first + slices[slice_index].count; ++i) {
					part.items.push_back(chunk.items[i]);
				}
				if (!part.items.empty()) {
					s.gpu_chunks.push_back(part);
				}
			}
		}
	}
	// Total counts one unit per staged request on BOTH paths, because DSContext
	// counts a request when it finalizes. A chunked GPU texture therefore
	// contributes its whole fan-out, not one: counting textures here made
	// progress clamp to 100% early and let is_prewarm_complete() return true
	// while most of a texture's chunks were still in flight (#0247). Paths that
	// drop a texture before enqueueing add the difference via count_gpu_shortfall().
	int gpu_units = 0;
	for (size_t i = 0; i < gpu_items.size(); ++i) {
		gpu_units += static_cast<int>(count_gpu_requests_for_payload(gpu_items[i].meta));
	}
	ctx->set_prewarm_total(tag, enqueued + gpu_units);
	if (enqueued > 0) {
		ctx->flush_stream_async();
	}
	// Kick the first GPU chunk immediately in loading-screen mode; in background
	// pacing mode let DSRuntime._process() pump on the next frame so initiation
	// never stacks a render-thread alloc/flush onto the same frame.
	if (!state().background_pacing.load(std::memory_order_relaxed)) {
		pump_gpu_prewarm();
	}
	if (npot_gpu > 0) {
		UtilityFunctions::push_warning("DSPreloader: ", npot_gpu, " of ", (int64_t)gpu_items.size(),
				" GPU-prewarmed textures are non-multiple-of-4; Texture2DRD reports their padded storage dimensions. Enable compatibility/as_image_texture to load them as ImageTexture at their logical size.");
	}
	UtilityFunctions::print_verbose("DSPreloader: gpu prewarm tag ", String::num_uint64(tag), " tracking ", (int64_t)payloads.size(), " textures (", (int64_t)enqueued, " memory requests, ", (int64_t)gpu_queued, " gpu textures, ", (int64_t)skipped_cached, " cached, ", (int64_t)skipped_unresolvable, " skipped).");
	return tag;
}

void DSPreloader::pump_gpu_prewarm() {
	bool have_chunk = false;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		DSContext *ctx = DSContext::get_singleton();
		while (!s.gpu_chunks.empty() && ctx != nullptr && ctx->is_tag_cancelled(s.gpu_chunks.front().tag)) {
			GpuChunk dead_chunk = s.gpu_chunks.front();
			s.gpu_chunks.pop_front();
			for (size_t i = 0; i < dead_chunk.items.size(); ++i) {
				std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(dead_chunk.items[i].pending_id);
				if (it != s.pending.end()) {
					it->second.gpu_failed = true;
				}
				count_gpu_shortfall(ctx, dead_chunk.tag,
						count_gpu_requests_for_payload(dead_chunk.items[i].meta));
			}
		}
		const size_t max_in_flight_chunks = s.background_pacing.load(std::memory_order_relaxed) ? 4 : 8;
		if (s.gpu_posted_chunks.size() < max_in_flight_chunks && !s.gpu_chunks.empty() && is_gpu_prewarm_available()) {
			s.gpu_posted_chunks.push_back(s.gpu_chunks.front());
			s.gpu_chunks.pop_front();
			have_chunk = true;
		}
	}
	if (have_chunk) {
		// Post WITHOUT state().mutex held. Beyond classic lock-order
		// hygiene this is load-bearing: posting call_on_render_thread
		// while holding the preloader mutex throws (observed: unknown
		// exception escaping the post on D3D12, green the moment the
		// lock is released first).
		RenderingServer *rs = RenderingServer::get_singleton();
		if (rs != nullptr) {
			Callable gpu_chunk_callable = callable_mp_static(process_gpu_chunk_on_render_thread);
			rs->call_on_render_thread(gpu_chunk_callable);
		} else {
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			if (!s.gpu_posted_chunks.empty()) {
				s.gpu_posted_chunks.pop_back();
			}
		}
	}
	pump_gpu_orphans();
}

void DSPreloader::alloc_gpu_item_on_render_thread(RenderingDevice *p_rd, DSContext *p_ctx, uint64_t p_tag, const GpuChunkItem &p_item) {
	const Payload &m = p_item.meta;
	// Re-validate: availability may have changed between pump and alloc.
	if (!is_gpu_prewarm_eligible(m)) {
		mark_gpu_item_failed(p_item.pending_id);
		count_gpu_shortfall(p_ctx, p_tag, count_gpu_requests_for_payload(m));
		return;
	}
	const int img_fmt = DSTex::rd_format_to_godot_format((int)m.pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		mark_gpu_item_failed(p_item.pending_id);
		count_gpu_shortfall(p_ctx, p_tag, count_gpu_requests_for_payload(m));
		return;
	}

	// Mirror the loader can_attempt_gpu texture_create (Component 1 sRGB
	// share included) so 3D PBR albedo sampling stays valid.
	Ref<RDTextureFormat> rd_fmt;
	rd_fmt.instantiate();
	rd_fmt->set_width(static_cast<int32_t>(m.width));
	rd_fmt->set_height(static_cast<int32_t>(m.height));
	rd_fmt->set_format(static_cast<RenderingDevice::DataFormat>(m.pixel_format));
	rd_fmt->set_mipmaps(static_cast<int32_t>(m.mip_count > 0 ? m.mip_count : 1));
	rd_fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	rd_fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	if (!DSTex::is_rd_format_srgb(static_cast<int>(m.pixel_format))) {
		int srgb_fmt = DSTex::get_srgb_rd_format(static_cast<int>(m.pixel_format));
		if (srgb_fmt != RenderingDevice::DATA_FORMAT_MAX && srgb_fmt != static_cast<int>(m.pixel_format)) {
			rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(m.pixel_format));
			rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(srgb_fmt));
		}
	}

	Ref<RDTextureView> rd_view;
	rd_view.instantiate();

	RID tex_rid = p_rd->texture_create(rd_fmt, rd_view, TypedArray<PackedByteArray>());
	if (!tex_rid.is_valid()) {
		UtilityFunctions::push_warning("DSPreloader: GPU texture_create failed for '", m.engine_path, "'; sync load remains available.");
		mark_gpu_item_failed(p_item.pending_id);
		count_gpu_shortfall(p_ctx, p_tag, count_gpu_requests_for_payload(m));
		return;
	}
	uint64_t native = p_rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, tex_rid, 0);
	if (native == 0) {
		UtilityFunctions::push_warning("DSPreloader: GPU native format query failed for '", m.engine_path, "'; sync load remains available.");
		p_rd->free_rid(tex_rid);
		mark_gpu_item_failed(p_item.pending_id);
		count_gpu_shortfall(p_ctx, p_tag, count_gpu_requests_for_payload(m));
		return;
	}

	// Loader-style GPU upload: a plain payload keeps one request spanning all
	// mips; a chunked payload emits one row-range request per chunk (and a
	// whole-subresource request per single-chunk mip). The backend validates
	// each subresource range against the created texture.
	Vector<Ref<DSRequest>> uploads;
	auto make_upload = [&](uint64_t p_upload_offset, uint32_t p_size, uint32_t p_uncompressed,
			uint32_t p_first_subresource, uint32_t p_subresource_count) -> Ref<DSRequest> {
		Ref<DSRequest> req;
		req.instantiate();
		req->set_file_path(m.file_path);
		req->set_offset(p_upload_offset);
		req->set_size(p_size);
		req->set_uncompressed_size(p_uncompressed);
		req->set_compression_format(m.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE)
				? DSRequest::COMPRESSION_GDEFLATE : DSRequest::COMPRESSION_NONE);
		req->set_cancellation_tag(p_tag);
		req->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
		req->set_destination_resource(tex_rid);
		req->set_first_subresource(p_first_subresource);
		req->set_subresource_count(p_subresource_count);
		req->set_texture_format(static_cast<uint32_t>(native));
		req->set_debug_label(m.engine_path);
		uploads.push_back(req);
		return req;
	};

	bool chunk_layout_ok = true;
	if (m.chunked) {
		// One request per upload unit: a row-range request per chunk, and a
		// whole-subresource request for a single-chunk mip (no table entry).
		chunk_layout_ok = walk_gpu_upload_units(m,
				[&](uint32_t p_mip, int64_t p_chunk_index, uint32_t p_row, uint32_t p_rows) -> bool {
					if (p_chunk_index < 0) {
						// A single-chunk mip has no chunk-table entry and targets ONE
						// subresource: keep it on MULTIPLE_SUBRESOURCES_RANGE (no
						// texture row region) so <4x4 BCn tail mips do not submit
						// non-4-aligned D3D12_BOX extents to TEXTURE_REGION.
						make_upload(static_cast<uint64_t>(m.mip_offsets[p_mip]),
								static_cast<uint32_t>(m.mip_sizes[p_mip]),
								static_cast<uint32_t>(m.mip_uncompressed_sizes[p_mip]), p_mip, 1);
						return true;
					}
					Ref<DSRequest> req = make_upload(static_cast<uint64_t>(m.chunk_offsets[p_chunk_index]),
							static_cast<uint32_t>(m.chunk_compressed_sizes[p_chunk_index]),
							static_cast<uint32_t>(m.chunk_uncompressed_sizes[p_chunk_index]), p_mip, 1);
					req->set_texture_region(p_row, p_row + p_rows);
					return true;
				});
	} else {
		const uint64_t start_offset = static_cast<uint64_t>(m.mip_offsets[0]);
		const uint64_t last_idx = static_cast<uint64_t>(m.mip_count - 1);
		const uint64_t total_span = (static_cast<uint64_t>(m.mip_offsets[last_idx]) + static_cast<uint64_t>(m.mip_sizes[last_idx])) - start_offset;
		uint64_t total_uncomp = 0;
		for (uint32_t i = 0; i < m.mip_count; ++i) {
			total_uncomp += static_cast<uint64_t>(m.mip_uncompressed_sizes[i]);
		}
		if (total_span == 0 || total_span > UINT32_MAX || total_uncomp == 0 || total_uncomp > UINT32_MAX) {
			chunk_layout_ok = false;
		} else {
			make_upload(start_offset, static_cast<uint32_t>(total_span),
					static_cast<uint32_t>(total_uncomp), 0, m.mip_count);
		}
	}
	if (!chunk_layout_ok || uploads.is_empty()) {
		UtilityFunctions::push_warning("DSPreloader: GPU payload layout out of range for '", m.engine_path, "'; sync load remains available.");
		p_rd->free_rid(tex_rid);
		mark_gpu_item_failed(p_item.pending_id);
		// Nothing was enqueued, so the DSContext will never count this texture:
		// charge its whole fan-out to the tag so progress can still settle.
		count_gpu_shortfall(p_ctx, p_tag, count_gpu_requests_for_payload(m));
		return;
	}
	// Queue-budget guard (loader parity): a chunked texture fans out to many
	// requests, and staging them past half the queue risks hardware
	// backpressure on the render thread. Over-budget textures stay on the
	// memory path shape by dropping the GPU entry; sync load stays available.
	if (p_ctx != nullptr) {
		const uint32_t queue_capacity = p_ctx->get_queue_capacity();
		if (uploads.size() > static_cast<int>(std::max<uint32_t>(1, queue_capacity / 2))) {
			UtilityFunctions::push_warning("DSPreloader: ", uploads.size(), " GPU prewarm requests exceed the queue budget (", queue_capacity, ") for '", m.engine_path, "'; sync load remains available.");
			p_rd->free_rid(tex_rid);
			mark_gpu_item_failed(p_item.pending_id);
			count_gpu_shortfall(p_ctx, p_tag, static_cast<size_t>(uploads.size()));
			return;
		}
	}

	// Attaches the accepted requests (and the destination RID) to the pending
	// entry. False means the entry vanished (shutdown / rescan).
	auto attach_uploads = [&](int p_count) -> bool {
		std::lock_guard<std::mutex> lock(state().mutex);
		std::unordered_map<int64_t, Tracked>::iterator it = state().pending.find(p_item.pending_id);
		if (it == state().pending.end()) {
			return false;
		}
		it->second.gpu_tex_rid = tex_rid;
		for (int i = 0; i < p_count; ++i) {
			it->second.requests.push_back(uploads[i]);
		}
		return true;
	};

	// Enqueue the whole fan-out. A failure means the tag was cancelled (or the
	// context shut down) mid-loop.
	int enqueued_uploads = 0;
	while (enqueued_uploads < uploads.size() &&
			p_ctx != nullptr &&
			p_ctx->enqueue_request(uploads[enqueued_uploads], true)) {
		enqueued_uploads++;
	}
	if (enqueued_uploads < uploads.size()) {
		// Requests accepted before the failure are already staged (and may
		// already be DMA-ing into tex_rid), so the RID must survive until they
		// retire: attach them, mark the item failed and let poll_completed()
		// orphan the RID after the last request is terminal (#0246). The units
		// that never reached the queue are charged to the tag here.
		count_gpu_shortfall(p_ctx, p_tag, static_cast<size_t>(uploads.size() - enqueued_uploads));
		mark_gpu_item_failed(p_item.pending_id);
		if (enqueued_uploads == 0) {
			// Nothing references the texture yet, so releasing it is safe.
			p_rd->free_rid(tex_rid);
			return;
		}
		if (!attach_uploads(enqueued_uploads)) {
			UtilityFunctions::push_warning("DSPreloader: prewarmed GPU texture '", m.engine_path, "' lost its pending entry mid fan-out; leaking its RID rather than freeing it under staged DMA.");
		}
		return;
	}
	if (!attach_uploads(static_cast<int>(uploads.size()))) {
		// Entry vanished after the whole fan-out was staged. A staged or
		// submitted request still targets tex_rid, so freeing here would let DMA
		// write into freed VRAM: leak deliberately instead (the documented rule).
		UtilityFunctions::push_warning("DSPreloader: prewarmed GPU texture '", m.engine_path, "' has no pending entry after enqueue; leaking its RID rather than freeing it under staged DMA.");
	}
}

void DSPreloader::process_gpu_chunk_on_render_thread() {
	RenderingServer *rs = RenderingServer::get_singleton();
	GpuChunk chunk;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (s.gpu_posted_chunks.empty()) {
			return;
		}
		chunk = s.gpu_posted_chunks.front();
		s.gpu_posted_chunks.pop_front();
	}
	if (rs == nullptr || !rs->is_on_render_thread()) {
		// Wrong thread (should not happen: posted via
		// call_on_render_thread). Re-queue at front; pump retries.
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		s.gpu_chunks.push_front(chunk);
		return;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	DSContext *ctx = DSContext::get_singleton();
	if (ctx != nullptr && ctx->is_tag_cancelled(chunk.tag)) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < chunk.items.size(); ++i) {
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(chunk.items[i].pending_id);
			if (it != s.pending.end()) {
				it->second.gpu_failed = true;
			}
			count_gpu_shortfall(ctx, chunk.tag, count_gpu_requests_for_payload(chunk.items[i].meta));
		}
		return;
	}
	if (rd == nullptr || ctx == nullptr || !ctx->is_d3d12_backend() || !ctx->is_gpu_decompression_enabled()) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (size_t i = 0; i < chunk.items.size(); ++i) {
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(chunk.items[i].pending_id);
			if (it != s.pending.end()) {
				it->second.gpu_failed = true;
			}
			if (ctx != nullptr) {
				count_gpu_shortfall(ctx, chunk.tag, count_gpu_requests_for_payload(chunk.items[i].meta));
			}
		}
		UtilityFunctions::push_warning("DSPreloader: GPU chunk dropped (D3D12 unavailable at alloc time); sync load remains available.");
		return;
	}
	for (size_t i = 0; i < chunk.items.size(); ++i) {
		alloc_gpu_item_on_render_thread(rd, ctx, chunk.tag, chunk.items[i]);
	}
	// Flush attempt on the render thread: backend destination queries run
	// inline (zero rendezvous). ERR_BUSY keeps requests queued for the
	// batch-manager retry on the main thread (slower rendezvous, rare).
	ctx->flush_stream_async();
}

static uint64_t estimate_payload_bytes(const DSPreloader::Payload &p_meta) {
	uint64_t bytes = 0;
	if (p_meta.is_v2) {
		for (int i = 0; i < p_meta.mip_uncompressed_sizes.size(); ++i) {
			bytes += static_cast<uint64_t>(p_meta.mip_uncompressed_sizes[i]);
		}
	} else {
		bytes = p_meta.original_size;
	}
	if (bytes == 0) {
		bytes = static_cast<uint64_t>(p_meta.width) * static_cast<uint64_t>(p_meta.height) * 4ULL;
	}
	return bytes;
}

// Measured at 368f5cb: a 4096x4096 RGBA8 v2 payload (64 MiB natural) costs
// 20.3-26.9 ms in build_texture() on the gdextest host, and 24 ms warm on a
// GTX 1050 Ti windowed run (14.1 ms for the 64 MiB resize + 8.35 ms for
// ImageTexture::create_from_image; Image::create_from_data is copy-on-write).
// 26.9 ms / 64 MiB = 420 us/MiB, x1.25 safety = 525. The previous 50 us/MiB
// (~20 GB/s) was 6-8x low and let 20-27 ms builds pass a 2-4 ms budget check
// (GODOT_QUIRKS.md quirk 41). Over-estimating is the safe direction: it only
// offloads more builds, never fewer.
static constexpr uint64_t kCpuBuildUsecPerMiB = 525;

// Byte charge for one CPU entry, shared by the inline slice and the offloaded
// task: estimate_payload_bytes() unless the metadata carries no sizes, in
// which case the destination buffers are summed (pre-extraction behaviour).
static uint64_t cpu_entry_bytes(const DSPreloader::Payload &p_meta, const std::vector<PackedByteArray> &p_buffers) {
	uint64_t bytes = estimate_payload_bytes(p_meta);
	if (bytes == 0) {
		for (size_t b = 0; b < p_buffers.size(); ++b) {
			bytes += static_cast<uint64_t>(p_buffers[b].size());
		}
	}
	return bytes;
}

void DSPreloader::complete_gpu_entry(const Tracked &p_entry, DSTextureCache *p_cache, int &r_stored) {
	DSContext *ctx = DSContext::get_singleton();
	bool is_cancelled = (ctx != nullptr && ctx->is_tag_cancelled(p_entry.tag));
	if (!is_cancelled) {
		for (int r = 0; r < p_entry.requests.size(); ++r) {
			if (p_entry.requests[r].is_valid()) {
				String err = p_entry.requests[r]->get_error_message();
				if (err.begins_with("Prewarm cancelled") ||
						err.begins_with("Request cancelled") ||
						err.contains("0x800703e3") ||
						err.to_lower().contains("aborted")) {
					is_cancelled = true;
					break;
				}
			}
		}
	}

	if (is_cancelled) {
		if (p_entry.gpu_tex_rid.is_valid()) {
			queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		}
		UtilityFunctions::print_verbose("DSPreloader: GPU prewarm texture '", p_entry.meta.engine_path, "' cancelled; dropping.");
		return;
	}

	if (p_entry.gpu_failed || !p_entry.gpu_tex_rid.is_valid()) {
		if (p_entry.gpu_tex_rid.is_valid()) {
			queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		}
		// Loud (not verbose): a GPU drop means DMA never filled VRAM and
		// the scene falls back to a sync load; operators need to see it.
		UtilityFunctions::push_warning("DSPreloader: GPU prewarm texture '", p_entry.meta.engine_path, "' did not complete; dropping (sync load remains available).");
		return;
	}
	for (int r = 0; r < p_entry.requests.size(); ++r) {
		if (p_entry.requests[r].is_null() || p_entry.requests[r]->get_status() != DSRequest::STATUS_COMPLETED) {
			queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
			UtilityFunctions::push_warning("DSPreloader: GPU prewarm texture '", p_entry.meta.engine_path, "' did not complete; dropping (sync load remains available).");
			return;
		}
	}
	// Prewarm needs a cache owner: store() refuses when the cache is disabled,
	// and a prewarmed texture nothing holds would be released immediately
	// anyway. Drop the RID on the render thread instead of wrapping it; the
	// synchronous load path now handles the uncached case with its own
	// DSTextureOwner. (#0126) p_cache cannot be null here: the only
	// caller, poll_completed(), returns early when the singleton is missing.
	if (!p_cache->can_own(DSTextureCache::CACHE_MODE_REUSE)) {
		queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		UtilityFunctions::push_warning("DSPreloader: texture cache will not own GPU prewarm texture '", p_entry.meta.engine_path, "'; releasing its RID (sync load remains available).");
		return;
	}
	Ref<Texture2DRD> tex_rd;
	tex_rd.instantiate();
	tex_rd->set_texture_rd_rid(p_entry.gpu_tex_rid);
	// NOTE: no size override here. The wrap above executes asynchronously on
	// the render thread, so the RS wrapper RID is not available yet, and the
	// device RID lives in the RD namespace that texture_set_size_override
	// must never receive. GPU-prewarmed NPOT textures therefore report storage
	// dims; the engine exposes no setter (GODOT_QUIRKS.md Q79). Users who need
	// authoring dimensions enable compatibility/as_image_texture, which routes
	// the texture to the CPU/ImageTexture path with set_size_override.
	const String alias = (p_entry.meta.payload_key != p_entry.meta.engine_path) ? p_entry.meta.payload_key : String();
	// can_own() is a pre-check, not a reservation. If the cache was disabled
	// between it and store(), store() reports refusal and we still own the RID.
	if (!p_cache->store(p_entry.meta.engine_path, tex_rd, DSTextureCache::CACHE_MODE_REUSE, alias, true)) {
		queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		UtilityFunctions::push_warning("DSPreloader: texture cache refused ownership of GPU prewarm texture '", p_entry.meta.engine_path, "' (disabled or raced); releasing its RID.");
		return;
	}
	r_stored++;
}

bool DSPreloader::offload_cpu_build_locked(int64_t p_id) {
	State &s = state();
	std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(p_id);
	if (it == s.pending.end() || it->second.build_in_flight || it->second.is_gpu) {
		return false;
	}
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	if (pool == nullptr) {
		return false;
	}
	BuildJob *job = new BuildJob();
	job->id = p_id;
	job->entry = it->second;
	job->generation = s.shutdown_generation;
	job->job_bytes = estimate_payload_bytes(it->second.meta);
	it->second.build_in_flight = true;
	const int64_t task = pool->add_native_task(&DSPreloader::run_offloaded_cpu_build, job, false, "DS CPU build");
	if (task == static_cast<int64_t>(WorkerThreadPool::INVALID_TASK_ID)) {
		it->second.build_in_flight = false;
		delete job;
		return false;
	}
	s.build_jobs[task] = job;
	s.in_flight_builds++;
	s.in_flight_build_bytes += job->job_bytes;
	return true;
}

void DSPreloader::run_offloaded_cpu_build(void *p_user) {
	BuildJob *job = static_cast<BuildJob *>(p_user);
	DSTextureCache *cache = DSTextureCache::get_singleton();
	bool stale = false;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		// A shutdown between posting and execution means this build belongs to a
		// session whose cache is being (or has been) torn down: drop it. The
		// generation check also makes a deliberately leaked task harmless.
		stale = (job->generation != state().shutdown_generation);
	}
	if (!stale && cache != nullptr) {
		int stored = 0;
		uint64_t entry_bytes = 0;
		complete_cpu_entry(job->entry, cache, stored, entry_bytes);
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		s.pending.erase(job->id);
		unblock_pending_paths_locked(job->entry);
		s.in_flight_builds--;
		s.in_flight_build_bytes = (s.in_flight_build_bytes >= job->job_bytes) ? (s.in_flight_build_bytes - job->job_bytes) : 0;
		job->done.store(true);
	}
}

void DSPreloader::unblock_pending_paths_locked(const Tracked &p_entry) {
	State &s = state();
	if (!p_entry.meta.engine_path.is_empty()) {
		auto it_p = s.pending_paths.find(p_entry.meta.engine_path);
		if (it_p != s.pending_paths.end()) {
			it_p->value--;
			if (it_p->value <= 0) {
				s.pending_paths.erase(p_entry.meta.engine_path);
			}
		}
	}
	if (!p_entry.meta.payload_key.is_empty() && p_entry.meta.payload_key != p_entry.meta.engine_path) {
		auto it_p = s.pending_paths.find(p_entry.meta.payload_key);
		if (it_p != s.pending_paths.end()) {
			it_p->value--;
			if (it_p->value <= 0) {
				s.pending_paths.erase(p_entry.meta.payload_key);
			}
		}
	}
	s.pending_cv.notify_all();
}

void DSPreloader::complete_cpu_entry(const Tracked &p_entry, DSTextureCache *p_cache, int &r_stored, uint64_t &r_entry_bytes) {
	r_stored = 0;
	r_entry_bytes = 0;
	std::vector<PackedByteArray> buffers;
	if (p_entry.meta.chunked) {
		int64_t request_cursor = 0;
		for (uint32_t mip = 0; mip < p_entry.meta.mip_count; ++mip) {
			const int32_t count = p_entry.meta.mip_chunk_counts.size() > static_cast<int64_t>(mip)
					? p_entry.meta.mip_chunk_counts[mip]
					: 1;
			if (count <= 1) {
				if (request_cursor >= p_entry.requests.size() ||
						p_entry.requests[request_cursor].is_null() ||
						p_entry.requests[request_cursor]->get_status() != DSRequest::STATUS_COMPLETED) {
					UtilityFunctions::print_verbose("DSPreloader: Prewarm texture '", p_entry.meta.engine_path, "' did not complete; dropping (sync load remains available).");
					return;
				}
				buffers.push_back(p_entry.requests[request_cursor]->get_destination_buffer());
				++request_cursor;
				continue;
			}
			PackedByteArray footprint;
			footprint.resize(static_cast<int64_t>(p_entry.meta.mip_uncompressed_sizes[mip]));
			uint64_t written = 0;
			for (int32_t c = 0; c < count; ++c, ++request_cursor) {
				if (request_cursor >= p_entry.requests.size() ||
						p_entry.requests[request_cursor].is_null() ||
						p_entry.requests[request_cursor]->get_status() != DSRequest::STATUS_COMPLETED) {
					const int64_t status = (request_cursor < p_entry.requests.size() && p_entry.requests[request_cursor].is_valid())
							? static_cast<int64_t>(p_entry.requests[request_cursor]->get_status())
							: -1;
					const godot::String why = (request_cursor < p_entry.requests.size() && p_entry.requests[request_cursor].is_valid())
							? p_entry.requests[request_cursor]->get_error_message()
							: godot::String("missing request");
					UtilityFunctions::push_warning("DSPreloader: chunked prewarm request did not complete (status ", status,
							", '", why, "') for '", p_entry.meta.engine_path, "'; dropping (sync load remains available).");
					return;
				}
				const PackedByteArray chunk = p_entry.requests[request_cursor]->get_destination_buffer();
				if (chunk.size() <= 0 ||
						written + static_cast<uint64_t>(chunk.size()) > static_cast<uint64_t>(footprint.size())) {
					UtilityFunctions::push_warning("DSPreloader: Chunk buffer does not fit mip ", (int64_t)mip, " for '", p_entry.meta.engine_path, "'; dropping prewarmed texture.");
					return;
				}
				memcpy(footprint.ptrw() + written, chunk.ptr(), static_cast<size_t>(chunk.size()));
				written += static_cast<uint64_t>(chunk.size());
			}
			if (written != static_cast<uint64_t>(footprint.size())) {
				UtilityFunctions::push_warning("DSPreloader: Chunk reassembly size mismatch for '", p_entry.meta.engine_path, "'; dropping prewarmed texture.");
				return;
			}
			buffers.push_back(footprint);
		}
		if (request_cursor != p_entry.requests.size()) {
			UtilityFunctions::push_warning("DSPreloader: Unconsumed prewarm requests for '", p_entry.meta.engine_path, "'; dropping prewarmed texture.");
			return;
		}
	} else {
		for (int r = 0; r < p_entry.requests.size(); ++r) {
			if (p_entry.requests[r].is_null() || p_entry.requests[r]->get_status() != DSRequest::STATUS_COMPLETED) {
				UtilityFunctions::print_verbose("DSPreloader: Prewarm texture '", p_entry.meta.engine_path, "' did not complete; dropping (sync load remains available).");
				return;
			}
			buffers.push_back(p_entry.requests[r]->get_destination_buffer());
		}
	}
	Ref<Texture2D> tex = build_texture(p_entry.meta, buffers);
	if (!tex.is_valid()) {
		UtilityFunctions::push_warning("DSPreloader: Failed to build prewarmed texture for ", p_entry.meta.engine_path, ".");
		return;
	}
	const String alias = (p_entry.meta.payload_key != p_entry.meta.engine_path) ? p_entry.meta.payload_key : String();
	// Report the decision: a refused store (cache disabled or raced) keeps
	// nothing resident, so it must not count as stored.
	if (p_cache->store(p_entry.meta.engine_path, tex, DSTextureCache::CACHE_MODE_REUSE, alias, true)) {
		r_stored = 1;
	} else {
		UtilityFunctions::print_verbose("DSPreloader: cache refused prewarmed texture '", p_entry.meta.engine_path, "'; not counted as stored.");
	}
	r_entry_bytes = cpu_entry_bytes(p_entry.meta, buffers);
}

bool DSPreloader::is_terminal(const Ref<DSRequest> &p_request, uint64_t p_timeout_usec) {
	if (p_request.is_null() || !p_request.is_valid()) {
		return true;
	}
	DSRequest::Status status = p_request->get_status();
	if (status == DSRequest::STATUS_COMPLETED) {
		return true;
	}
	if (status == DSRequest::STATUS_FAILED) {
		if (!p_request->is_gpu_in_flight()) {
			return true;
		}
		// Bounded fallback timeout (default 5.0s) against steady-state GPU hangs / TDR
		uint64_t cancel_time = p_request->get_cancel_timestamp_usec();
		if (cancel_time > 0 && Time::get_singleton()) {
			uint64_t now_us = Time::get_singleton()->get_ticks_usec();
			if (now_us > cancel_time && (now_us - cancel_time) > p_timeout_usec) {
				UtilityFunctions::push_error("[DirectStorage] Preloader: GPU cancellation retirement timed out (>5.0s, GPU hang/TDR). Force-retiring request.");
				p_request->set_gpu_in_flight(false);
				return true;
			}
		}
		return false;
	}
	return false;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
RID DSPreloader::_get_gpu_prewarm_rid_for_testing(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		if (it->second.tag == p_tag && it->second.is_gpu && it->second.gpu_tex_rid.is_valid()) {
			return it->second.gpu_tex_rid;
		}
	}
	return RID();
}

Ref<DSRequest> DSPreloader::_get_gpu_prewarm_request_for_testing(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		if (it->second.tag != p_tag || !it->second.is_gpu || it->second.requests.is_empty()) {
			continue;
		}
		for (int r = 0; r < it->second.requests.size(); ++r) {
			if (it->second.requests[r].is_valid()) {
				return it->second.requests[r];
			}
		}
	}
	return Ref<DSRequest>();
}

int64_t DSPreloader::_count_gpu_requests_for_testing(const Payload &p_payload) {
	return static_cast<int64_t>(count_gpu_requests_for_payload(p_payload));
}

Array DSPreloader::_debug_plan_gpu_chunk_slices_for_testing(const PackedInt32Array &p_item_requests,
		const PackedInt64Array &p_item_bytes, int p_item_cap, int p_request_cap, int64_t p_byte_cap) {
	std::vector<uint64_t> requests;
	requests.reserve(static_cast<size_t>(p_item_requests.size()));
	for (int i = 0; i < p_item_requests.size(); i++) {
		requests.push_back(static_cast<uint64_t>(std::max<int64_t>(0, p_item_requests[i])));
	}
	std::vector<uint64_t> bytes;
	bytes.reserve(static_cast<size_t>(p_item_bytes.size()));
	for (int i = 0; i < p_item_bytes.size(); i++) {
		bytes.push_back(static_cast<uint64_t>(std::max<int64_t>(0, p_item_bytes[i])));
	}
	const std::vector<GpuChunkSlice> slices = plan_gpu_chunk_slices(requests, bytes,
			static_cast<size_t>(std::max(1, p_item_cap)), static_cast<size_t>(std::max(1, p_request_cap)),
			static_cast<uint64_t>(std::max<int64_t>(0, p_byte_cap)));
	Array result;
	for (size_t s = 0; s < slices.size(); s++) {
		uint64_t slice_requests = 0;
		uint64_t slice_bytes = 0;
		for (size_t i = slices[s].first; i < slices[s].first + slices[s].count; i++) {
			slice_requests += requests[i];
			slice_bytes += bytes[i];
		}
		Dictionary entry;
		entry["items"] = static_cast<int64_t>(slices[s].count);
		entry["requests"] = static_cast<int64_t>(slice_requests);
		entry["bytes"] = static_cast<int64_t>(slice_bytes);
		result.push_back(entry);
	}
	return result;
}
#endif

Ref<Texture2D> DSPreloader::build_texture(const Payload &p_meta, const std::vector<PackedByteArray> &p_buffers) {
	if (p_meta.width == 0 || p_meta.height == 0 || p_meta.mip_count == 0 || p_buffers.empty()) {
		return Ref<Texture2D>();
	}
	const int img_fmt = DSTex::rd_format_to_godot_format((int)p_meta.pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		return Ref<Texture2D>();
	}

	size_t expected_mips = p_meta.is_v2 ? (size_t)p_meta.mip_count : 1;
	if (p_buffers.size() != expected_mips) {
		UtilityFunctions::push_warning("DSPreloader: Request/buffer count mismatch for '", p_meta.engine_path, "'; dropping prewarmed texture.");
		return Ref<Texture2D>();
	}

	// 1. Calculate mip layouts and total natural data size upfront
	std::vector<DSTexMipLayout> layouts;
	layouts.reserve(p_buffers.size());
	int64_t total_natural_size = 0;
	uint32_t cur_w = p_meta.width;
	uint32_t cur_h = p_meta.height;
	for (size_t i = 0; i < p_buffers.size(); ++i) {
		if (p_buffers[i].size() == 0) {
			UtilityFunctions::push_warning("DSPreloader: Empty completed payload for '", p_meta.engine_path, "'; dropping prewarmed texture.");
			return Ref<Texture2D>();
		}
		if (p_meta.is_v2) {
			DSTexMipLayout layout = DSTex::get_mip_layout(cur_w, cur_h, img_fmt);
			layouts.push_back(layout);
			total_natural_size += (int64_t)layout.source_size;
		} else {
			total_natural_size += (int64_t)p_buffers[i].size();
		}
		cur_w = std::max<uint32_t>(1, cur_w / 2);
		cur_h = std::max<uint32_t>(1, cur_h / 2);
	}

	// 2. Single pre-allocated buffer: unpad directly into destination without intermediate allocations
	PackedByteArray natural_data;
	natural_data.resize(total_natural_size);
	uint8_t *dst_ptr = natural_data.ptrw();
	int64_t dst_offset = 0;

	for (size_t i = 0; i < p_buffers.size(); ++i) {
		const uint8_t *src = p_buffers[i].ptr();
		if (p_meta.is_v2) {
			const DSTexMipLayout &layout = layouts[i];
			uint8_t *dst = dst_ptr + dst_offset;
			if (layout.row_pitch > 0 && layout.source_row_bytes > 0 &&
					p_buffers[i].size() == static_cast<int64_t>(layout.footprint_size)) {
				for (uint64_t r = 0; r < layout.row_count; ++r) {
					memcpy(dst + r * layout.source_row_bytes, src + r * layout.row_pitch, static_cast<size_t>(layout.source_row_bytes));
				}
			} else {
				size_t copy_bytes = std::min(static_cast<size_t>(p_buffers[i].size()), static_cast<size_t>(layout.source_size));
				if (dst_offset + static_cast<int64_t>(copy_bytes) <= total_natural_size) {
					memcpy(dst, src, copy_bytes);
				} else {
					UtilityFunctions::push_error("DSPreloader: Buffer overrun prevented for '", p_meta.engine_path, "'.");
					return Ref<Texture2D>();
				}
			}
			dst_offset += (int64_t)layout.source_size;
		} else {
			memcpy(dst_ptr + dst_offset, src, p_buffers[i].size());
			dst_offset += p_buffers[i].size();
		}
	}

	// Graceful Base Fallback: partial chains slice mip 0, no mipmaps + warning.
	bool use_mipmaps = p_meta.mip_count > 1;
	PackedByteArray image_data;
	const uint32_t full_chain = ds_preloader_full_mip_chain_count(p_meta.width, p_meta.height);
	if (use_mipmaps && p_meta.mip_count < full_chain) {
		int64_t base_size = 0;
		if (p_meta.is_v2 && !layouts.empty()) {
			base_size = static_cast<int64_t>(layouts[0].source_size);
		} else {
			const DSTexMipLayout base_layout = DSTex::get_mip_layout(p_meta.width, p_meta.height, img_fmt);
			base_size = static_cast<int64_t>(base_layout.source_size);
		}
		if (base_size > 0 && natural_data.size() >= base_size) {
			image_data.resize(base_size);
			memcpy(image_data.ptrw(), natural_data.ptr(), static_cast<size_t>(base_size));
			use_mipmaps = false;
			UtilityFunctions::push_warning("DSPreloader: Partial mip chain (", p_meta.mip_count, " of ", full_chain, ") for '", p_meta.engine_path, "'; using base mip without mipmaps.");
		} else {
			UtilityFunctions::push_error("DSPreloader: Partial mip chain with bad base size for '", p_meta.engine_path, "'.");
		}
	} else {
		image_data = natural_data;
	}
	Ref<Image> image;
	if (image_data.size() > 0) {
		image = Image::create_from_data((int)p_meta.width, (int)p_meta.height, use_mipmaps, (Image::Format)img_fmt, image_data);
	}
	if (image.is_valid() && !image->is_empty()) {
		Ref<ImageTexture> img_tex = ImageTexture::create_from_image(image);
		// NPOT display size via the engine-blessed path: get_width() reports
		// the logical size and the RS override covers render-side queries.
		if (img_tex.is_valid() && p_meta.logical_width > 0 && p_meta.logical_height > 0 &&
				p_meta.logical_width <= p_meta.width && p_meta.logical_height <= p_meta.height &&
				(p_meta.logical_width != p_meta.width || p_meta.logical_height != p_meta.height)) {
			img_tex->set_size_override(Vector2i((int32_t)p_meta.logical_width, (int32_t)p_meta.logical_height));
		}
		Ref<Texture2D> tex = img_tex;
		return tex;
	}
	return Ref<Texture2D>();
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void DSPreloader::_reset_poll_dispatch_counters_for_testing() {
	s_last_poll_inline_builds.store(0);
	s_last_poll_offloads.store(0);
}

int DSPreloader::_get_last_poll_inline_builds_for_testing() {
	return s_last_poll_inline_builds.load();
}

int DSPreloader::_get_last_poll_offloads_for_testing() {
	return s_last_poll_offloads.load();
}
#endif

int DSPreloader::poll_completed(uint64_t p_tag, int p_max_items, uint32_t p_max_usec, const String &p_essential_path, bool p_gpu_only) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	s_last_poll_inline_builds.store(0);
	s_last_poll_offloads.store(0);
#endif
	DSTextureCache *cache = DSTextureCache::get_singleton();
	if (cache == nullptr) {
		UtilityFunctions::push_warning("DSPreloader: DSTextureCache singleton unavailable during poll.");
		return 0;
	}

	// Reap offloaded builds that finished since the last poll. Their store
	// already happened on the worker; this only frees the job so build_jobs
	// stays bounded. Deleting outside the lock keeps the job's Tracked
	// destructor (request Refs) off the pending lock.
	{
		std::vector<BuildJob *> finished;
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			for (std::unordered_map<int64_t, BuildJob *>::iterator it = s.build_jobs.begin(); it != s.build_jobs.end();) {
				if (it->second->done.load()) {
					finished.push_back(it->second);
					it = s.build_jobs.erase(it);
				} else {
					++it;
				}
			}
		}
		for (size_t i = 0; i < finished.size(); ++i) {
			delete finished[i];
		}
	}

	uint64_t start_usec = (p_max_usec > 0 && Time::get_singleton()) ? Time::get_singleton()->get_ticks_usec() : 0;
	int stored = 0;
	// Items consumed by this slice. Drives the item/byte budgets because `stored`
	// only counts cache hits, and a refused store (cache disabled or raced)
	// consumes work without storing anything - budgeting on `stored` would then
	// process the whole backlog in one frame.
	int processed = 0;
	// GPU entries wrap Texture2DRD (main/render-thread-only): worker polls
	// skip them in the scan below and leave them for the main-thread poll.
	const bool can_build_gpu = main_or_render_thread();

	// Mode and offload budgets drive both the scan width and the dispatch
	// split, so resolve them before collecting candidates.
	const bool bg_pacing = state().background_pacing.load(std::memory_order_relaxed);
	const int max_offloads_per_slice = bg_pacing ? 1 : std::max(2, ds_get_max_cpu_threads());
	// Only bounded loading slices seed parallel builds up front: background
	// pacing keeps its 1-offload cap, and unbounded callers keep the fit-first
	// path used by await_pending_texture's essential entry.
	const bool parallel_first = !bg_pacing && p_max_usec > 0 && !p_gpu_only;
	constexpr uint64_t kInFlightBytesPerCpuWorker = 32ULL * 1024ULL * 1024ULL;
	constexpr uint64_t kMaxInFlightBytesLoadingCeiling = 512ULL * 1024ULL * 1024ULL;
	constexpr uint64_t kMaxInFlightBytesBackground = 16ULL * 1024ULL * 1024ULL;
	// Loading screens scale the in-flight byte budget with the same CPU budget
	// that sizes the offload fan-out. Background streaming stays at 16 MiB so
	// large ImageTexture uploads cannot burst RenderingServer::command_queue.
	const uint64_t kMaxInFlightBytesLoading = std::min<uint64_t>(
			kMaxInFlightBytesLoadingCeiling,
			kInFlightBytesPerCpuWorker * static_cast<uint64_t>(std::max(1, ds_get_max_cpu_threads())));
	const uint64_t max_in_flight_bytes = bg_pacing ? kMaxInFlightBytesBackground : kMaxInFlightBytesLoading;
	const int scan_limit = p_max_items > 0 ? p_max_items + (parallel_first ? max_offloads_per_slice : 0) : 0;
	int offloads = 0;

	// Single-scan collect: gather terminal entry IDs under one lock instead
	// of re-scanning the full pending map after every build (O(N^2)).
	// Prioritize GPU entries first: on the main thread, wrapping Texture2DRD
	// takes <2us per texture, draining VRAM DMA instantly without consuming
	// the microsecond budget on heavier CPU builds.
	std::vector<int64_t> terminal_ids;
	{
		std::vector<int64_t> gpu_terminal_ids;
		std::vector<int64_t> cpu_terminal_ids;
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		gpu_terminal_ids.reserve(s.pending.size());
		cpu_terminal_ids.reserve(s.pending.size());
		for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
			const Tracked &cand = it->second;
			if (p_tag != 0 && cand.tag != p_tag) {
				continue;
			}
			// Already being built by an offloaded worker task: it will retire
			// the entry when it finishes, so no poll may take it again.
			if (cand.build_in_flight) {
				continue;
			}
			if (cand.is_gpu) {
				// Chunk not yet attached (no requests) or worker poll:
				// leave for the main/render-thread poll. Failed entries
				// are collected on any thread to drop them promptly.
				if (!cand.gpu_failed && (cand.requests.is_empty() || !can_build_gpu)) {
					continue;
				}
			} else if (p_gpu_only || cand.cpu_queued) {
				continue;
			}
			bool all_terminal = true;
			for (int i = 0; i < cand.requests.size(); ++i) {
				if (!is_terminal(cand.requests[i])) {
					all_terminal = false;
					break;
				}
			}
			if (all_terminal) {
				if (cand.is_gpu) {
					gpu_terminal_ids.push_back(it->first);
				} else {
					cpu_terminal_ids.push_back(it->first);
				}
				if (scan_limit > 0 && (int)(gpu_terminal_ids.size() + cpu_terminal_ids.size()) >= scan_limit) {
					break;
				}
			}
		}
		terminal_ids.reserve(gpu_terminal_ids.size() + cpu_terminal_ids.size());
		terminal_ids.insert(terminal_ids.end(), gpu_terminal_ids.begin(), gpu_terminal_ids.end());
		terminal_ids.insert(terminal_ids.end(), cpu_terminal_ids.begin(), cpu_terminal_ids.end());
	}

	// Loading screens: seed WorkerThreadPool with ready CPU entries BEFORE the
	// single poll thread spends its microsecond slice building textures inline.
	// One offload slot and the named essential entry stay with the main loop.
	if (parallel_first && p_max_items > 0) {
		bool inline_runner_reserved = false;
		for (size_t si = 0; si < terminal_ids.size() && offloads + 1 < max_offloads_per_slice; ++si) {
			const int64_t candidate_id = terminal_ids[si];
			bool should_offload = false;
			{
				std::lock_guard<std::mutex> lock(state().mutex);
				State &s = state();
				std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.find(candidate_id);
				if (it != s.pending.end()) {
					const Tracked &cand = it->second;
					const bool essential = !p_essential_path.is_empty() &&
							(cand.meta.engine_path == p_essential_path ||
									cand.meta.payload_key == p_essential_path);
					if (!cand.is_gpu && !cand.build_in_flight && !essential &&
							(p_tag == 0 || cand.tag == p_tag)) {
						if (!inline_runner_reserved) {
							inline_runner_reserved = true;
						} else {
							const uint64_t est_bytes = estimate_payload_bytes(cand.meta);
							should_offload = (s.in_flight_builds == 0) ||
									(s.in_flight_build_bytes + est_bytes <= max_in_flight_bytes);
						}
					}
				}
			}
			if (should_offload && offload_cpu_build_locked(candidate_id)) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
				s_last_poll_offloads.fetch_add(1);
#endif
				++offloads;
			}
		}
	}

	uint64_t slice_bytes_processed = 0;
	constexpr uint64_t kMaxBytesPerSlice = 64ULL * 1024ULL * 1024ULL; // 64 MiB slice cap

	for (size_t ti = 0; ti < terminal_ids.size(); ++ti) {
		if (p_max_items > 0 && processed >= p_max_items) {
			break;
		}
		if (p_max_usec > 0 && Time::get_singleton()) {
			uint64_t elapsed = Time::get_singleton()->get_ticks_usec() - start_usec;
			if (elapsed >= p_max_usec) {
				break;
			}
		}
		if (slice_bytes_processed >= kMaxBytesPerSlice) {
			break;
		}

		Tracked t;
		int64_t entry_id = 0;
		bool found = false;
		bool budget_exhausted = false;
		bool offloaded = false;
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(terminal_ids[ti]);
			if (it != s.pending.end()) {
				// Tag filter re-check: a concurrent poll may have retagged?
				// Pending entries are immutable once enqueued, but the
				// caller tag may differ per concurrent poll call.
				if ((p_tag == 0 || it->second.tag == p_tag) && !it->second.build_in_flight) {
					const bool is_gpu = it->second.is_gpu;
					uint64_t est_bytes = estimate_payload_bytes(it->second.meta);
					// CPU builds are 20-27 ms for a 64 MiB texture (measured), so
					// the estimate is deliberately conservative; GPU completion is a
					// constant 2 us (RID wrap + store, no data movement).
					uint64_t est_usec = is_gpu ? 2 : std::max<uint64_t>(1ULL, (est_bytes * kCpuBuildUsecPerMiB + (1024ULL * 1024ULL - 1ULL)) / (1024ULL * 1024ULL));
					uint64_t elapsed = (p_max_usec > 0 && Time::get_singleton()) ? (Time::get_singleton()->get_ticks_usec() - start_usec) : 0;

					bool fits;
					if (p_max_usec > 0) {
						// No first-item exemption: the budget is a promise to the
						// calling thread. An oversized CPU entry is offloaded below.
						const uint64_t remaining = (elapsed < p_max_usec) ? (p_max_usec - elapsed) : 0;
						fits = (est_usec <= remaining) && (slice_bytes_processed + est_bytes <= kMaxBytesPerSlice);
					} else {
						// Explicit unbounded caller (p_max_usec == 0): unbounded in
						// time, never in bytes. Only the entry the caller named as the
						// texture it is about to look up (await_pending_texture) may
						// exceed the slice cap: that build is the caller's own request,
						// and deferring it would return a miss and pay for a duplicate
						// synchronous load. Every other CPU entry obeys the same
						// pre-estimate as a bounded slice and is offloaded below when it
						// does not fit.
						const bool essential = !p_essential_path.is_empty() &&
								(it->second.meta.engine_path == p_essential_path ||
										it->second.meta.payload_key == p_essential_path);
						fits = essential || (slice_bytes_processed + est_bytes <= kMaxBytesPerSlice);
					}

					if (fits || is_gpu) {
						t = it->second;
						entry_id = it->first;
						it->second.build_in_flight = true;
						found = true;
					} else if (offloads >= max_offloads_per_slice ||
							(s.in_flight_builds > 0 && (s.in_flight_build_bytes + est_bytes > max_in_flight_bytes))) {
						// Budget for this slice or in-flight byte cap is spent; the next call offloads more.
						budget_exhausted = true;
					} else if (offload_cpu_build_locked(it->first)) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
						s_last_poll_offloads.fetch_add(1);
#endif
						offloaded = true;
					} else {
						// No usable worker pool: build inline so the stream still
						// drains. Progress beats a strict bound.
						t = it->second;
						entry_id = it->first;
						it->second.build_in_flight = true;
						found = true;
					}
				}
			}
		}
		if (offloaded) {
			++offloads;
			continue;
		}
		if (budget_exhausted) {
			break;
		}
		if (!found) {
			continue;
		}

		if (t.is_gpu) {
			complete_gpu_entry(t, cache, stored);
			slice_bytes_processed += estimate_payload_bytes(t.meta);
		} else {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
			s_last_poll_inline_builds.fetch_add(1);
#endif
			int cpu_stored = 0;
			uint64_t entry_bytes = 0;
			complete_cpu_entry(t, cache, cpu_stored, entry_bytes);
			stored += cpu_stored;
			slice_bytes_processed += entry_bytes;
		}
		processed++;

		// Erase from s.pending only AFTER complete_*_entry() stores into
		// DSTextureCache: otherwise a concurrent main-thread poll sees
		// get_unbuilt_prewarm_count() == 0 and fires prewarm_finished() while
		// the last texture is still being built.
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			state().pending.erase(entry_id);
			unblock_pending_paths_locked(t);
		}
	}

	return stored;
}

int DSPreloader::get_pending_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	int count = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		const Tracked &t = it->second;
		if (p_tag != 0 && t.tag != p_tag) {
			continue;
		}
		bool pending = false;
		for (int i = 0; i < t.requests.size(); ++i) {
			if (!is_terminal(t.requests[i])) {
				pending = true;
				break;
			}
		}
		if (pending) {
			count++;
		}
	}
	return count;
}

int DSPreloader::get_unbuilt_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	if (p_tag == 0) {
		return (int)s.pending.size();
	}
	int count = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		if (it->second.tag == p_tag) {
			count++;
		}
	}
	return count;
}

int DSPreloader::get_unbuilt_cpu_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	int count = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		// In-flight offloaded builds are excluded: ds_runtime.gd:523 spawns a
		// worker slice whenever this count is non-zero, and an entry already
		// being built would only make it post a no-op task every frame.
		if ((p_tag == 0 || it->second.tag == p_tag) && !it->second.is_gpu && !it->second.cpu_queued && !it->second.build_in_flight) {
			count++;
		}
	}
	return count;
}

int DSPreloader::get_unbuilt_gpu_prewarm_count(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	int count = 0;
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		if ((p_tag == 0 || it->second.tag == p_tag) && it->second.is_gpu) {
			count++;
		}
	}
	return count;
}

bool DSPreloader::has_gpu_requests_in_flight(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(state().mutex);
	const State &s = state();
	for (std::unordered_map<int64_t, Tracked>::const_iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
		const Tracked &t = it->second;
		if (p_tag != 0 && t.tag != p_tag) {
			continue;
		}
		if (!t.is_gpu || t.gpu_failed || t.requests.is_empty()) {
			continue;
		}
		for (int i = 0; i < t.requests.size(); ++i) {
			if (!is_terminal(t.requests[i])) {
				return true;
			}
		}
	}
	return false;
}

void DSPreloader::set_background_pacing(bool p_enabled) {
	state().background_pacing.store(p_enabled, std::memory_order_relaxed);
}

bool DSPreloader::is_background_pacing() {
	return state().background_pacing.load(std::memory_order_relaxed);
}

void DSPreloader::shutdown() {
	// Offloaded CPU builds run on WorkerThreadPool and may still be inside
	// DSTextureCache::store() when teardown starts. Signal them, then drain
	// before touching the cache or the state containers. The wait is bounded:
	// the largest legal payload is capped at DSTEX_MAX_TOTAL_BYTES (512 MiB), so
	// this waits for a finite build. A job that outlives the deadline is leaked
	// deliberately - never freed under a running task.
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().shutdown_generation++;
		state().pending_cv.notify_all();
	}
	{
		const uint64_t deadline = Time::get_singleton() ? (Time::get_singleton()->get_ticks_usec() + 5000000ULL) : 0;
		for (;;) {
			std::vector<int64_t> ids;
			{
				std::lock_guard<std::mutex> lock(state().mutex);
				if (state().in_flight_builds == 0) {
					break;
				}
				for (std::unordered_map<int64_t, BuildJob *>::const_iterator it = state().build_jobs.begin(); it != state().build_jobs.end(); ++it) {
					ids.push_back(it->first);
				}
			}
			WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
			if (pool == nullptr) {
				UtilityFunctions::push_warning("DSPreloader: shutdown found offloaded CPU builds but no WorkerThreadPool; leaking them.");
				break;
			}
			for (size_t i = 0; i < ids.size(); ++i) {
				// Never is_task_completed() after this: the id is retired and the
				// call errors with "Invalid Task ID" (measured at 368f5cb).
				pool->wait_for_task_completion(ids[i]);
			}
			if (deadline != 0 && Time::get_singleton() && Time::get_singleton()->get_ticks_usec() >= deadline) {
				UtilityFunctions::push_warning("DSPreloader: shutdown timed out waiting for offloaded CPU builds; leaking the remaining job(s).");
				break;
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (std::unordered_map<int64_t, BuildJob *>::iterator it = s.build_jobs.begin(); it != s.build_jobs.end(); ++it) {
			if (it->second->done.load()) {
				delete it->second;
			}
			// else: deliberately leaked (the task may still be inside store()).
		}
		s.build_jobs.clear();
	}

	// Destination RIDs this preloader still owns must leave through
	// DSTextureCache::release_owned_rid(), never by dropping the containers: that
	// is the only path that consults is_resource_in_flight() and defers a free
	// until the governing fence retires. Every entry still in `pending` here is
	// pre-handoff - poll_completed() erases an entry BEFORE complete_gpu_entry()
	// either gives its RID to the cache or queues it as an orphan - so all of
	// these are ours to release.
	std::vector<RID> owned_rids;
	{
		std::lock_guard<std::mutex> archive_lock(state().archive_mutex);
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			for (std::unordered_map<int64_t, Tracked>::iterator it = s.pending.begin(); it != s.pending.end(); ++it) {
				if (it->second.gpu_tex_rid.is_valid()) {
					owned_rids.push_back(it->second.gpu_tex_rid);
				}
			}
			owned_rids.insert(owned_rids.end(), s.gpu_orphans.begin(), s.gpu_orphans.end());
			s.pending.clear();
			s.pending_paths.clear();
			s.gpu_chunks.clear();
			s.cpu_queue.clear();
			s.gpu_posted_chunks.clear();
			s.gpu_orphans.clear();
			s.gpu_orphan_free_posted = false;
			s.pending_cv.notify_all();
		}
		state().archive.unref();
		state().archive_path = String();
		// Invalidate the resolution cache too: a textures.dspkg imported or
		// copied while the module was live (or between sessions) must be
		// re-probed on the next prewarm instead of serving a stale miss.
		state().resolved_archive_path = String();
		state().resolved_archive_probed = false;
		state().archive_generation++;
		// Teardown must not serve stale headers next session: bump the header
		// generation (called outside the pending-lock scope above; order stays
		// archive_mutex -> mutex, matching rescan_archive()).
		invalidate_header_cache();
	}
	// Released after BOTH mutexes are gone: release_owned_rid() takes the
	// backend's batch_mutex through is_resource_in_flight() and may post a
	// render-thread callable, which must never happen under state().mutex (see
	// pump_gpu_prewarm()). Ordering is still correct: shutdown() runs from
	// ~DSTextureCache, i.e. before shutdown_ds(), so a RID whose request is still
	// in flight is deferred to register_types.cpp's teardown flush instead of
	// racing the DMA. Dedupe is defensive: an entry and an orphan cannot name the
	// same RID today, and this keeps a future reordering from double-freeing.
	std::sort(owned_rids.begin(), owned_rids.end(), [](const RID &a, const RID &b) { return a.get_id() < b.get_id(); });
	owned_rids.erase(std::unique(owned_rids.begin(), owned_rids.end(), [](const RID &a, const RID &b) { return a.get_id() == b.get_id(); }), owned_rids.end());
	for (size_t i = 0; i < owned_rids.size(); ++i) {
		if (owned_rids[i].is_valid()) {
			DSTextureCache::release_owned_rid(owned_rids[i]);
		}
	}
}

bool DSPreloader::is_texture_pending(const String &p_path) {
	if (p_path.is_empty()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(state().mutex);
	return state().pending_paths.has(p_path);
}

Ref<Texture2D> DSPreloader::await_pending_texture(const String &p_path, uint32_t p_timeout_ms) {
	if (p_path.is_empty()) {
		return Ref<Texture2D>();
	}

	DSTextureCache *cache = DSTextureCache::get_singleton();
	if (cache != nullptr) {
		Ref<Texture2D> hit = cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
		if (hit.is_valid()) {
			return hit;
		}
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	bool is_render_thread = (rs != nullptr) && rs->is_on_render_thread();

	if (is_render_thread) {
		// On the render thread, we cannot sleep on pending_cv because poll_completed()
		// itself requires the render thread. Pump completions directly to prevent deadlocks:
		DSContext *ctx = DSContext::get_singleton();
		if (ctx != nullptr) {
			if (ctx->has_stream_pending()) {
				ctx->flush_stream_async();
			}
			ctx->collect_completed_results();
		}
		// Explicit unbounded: this is the render-thread deadlock-avoidance pump
		// and the caller is about to look this path up. Time stays unbounded, but
		// the pass is bounded in bytes: p_path is the one entry this call may build
		// over the slice cap, and every other terminal entry is offloaded rather
		// than built here.
		poll_completed(0, 0, 0, p_path);
		if (cache != nullptr) {
			return cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
		}
		return Ref<Texture2D>();
	}

	// On a worker thread: wait for the render thread (e.g. DSRuntime::_process)
	// to complete the in-flight stream request and store the texture into DSTextureCache.
	std::unique_lock<std::mutex> lock(state().mutex);
	State &s = state();
	const uint64_t entry_generation = s.shutdown_generation;

	if (!s.pending_paths.has(p_path)) {
		lock.unlock();
		DSTextureCache *post_cache = DSTextureCache::get_singleton();
		if (post_cache != nullptr) {
			return post_cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
		}
		return Ref<Texture2D>();
	}

	s.pending_cv.wait_for(lock, std::chrono::milliseconds(p_timeout_ms), [&s, &p_path, entry_generation]() {
		return !s.pending_paths.has(p_path) || s.shutdown_generation != entry_generation;
	});
	const bool shut_down = (s.shutdown_generation != entry_generation);
	const bool timed_out = !shut_down && s.pending_paths.has(p_path);

	lock.unlock();
	if (shut_down) {
		return Ref<Texture2D>();
	}
	if (timed_out) {
		UtilityFunctions::print_verbose("[DirectStorage] await_pending_texture timed out waiting for '", p_path, "' after ", (int64_t)p_timeout_ms, " ms; falling through to cache/CPU path.");
	}
	// Re-fetch after sleeping: DSTextureCache may have been replaced or torn
	// down while this thread waited on pending_cv.
	DSTextureCache *post_cache = DSTextureCache::get_singleton();
	if (post_cache != nullptr) {
		return post_cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
	}
	return Ref<Texture2D>();
}

} // namespace godot
