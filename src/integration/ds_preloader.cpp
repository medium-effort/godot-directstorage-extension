#include "ds_preloader.h"
#include "ds_texture_cache.h"
#include "../core/ds_context.h"
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/config_file.hpp>
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
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>

namespace godot {

// Upper bound per prewarm request output buffer. The sync loader imposes no
// cap, but prewarm fans out to dozens of speculative buffers, so a corrupt
// header claiming gigabytes must be rejected before resize() runs.
static constexpr uint64_t MAX_PREWARM_REQUEST_BYTES = 512ULL * 1024ULL * 1024ULL;

DSPreloader::State &DSPreloader::state() {
	static State s;
	return s;
}

bool DSPreloader::is_engine_source_path(const String &p_path) {
	String ext = p_path.get_extension().to_lower();
	return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp";
}

String DSPreloader::resolve_archive_path() {
	// Cached resolution: the 3-candidate probe below (res://textures.dspkg,
	// exe-dir join, globalized) used to run once per prewarmed path — 300
	// FileAccess::file_exists calls for a 100-path prewarm with only
	// unresolvable names and no archive on disk. The result is now resolved
	// once per generation and served from resolved_archive_path under
	// archive_mutex: a cached miss returns empty with zero probes
	// (false-fast), a cached hit skips straight to the open-cache check in
	// fill_from_dspkg.
	// Staleness contract: a textures.dspkg created/imported after a miss
	// was cached is picked up only via rescan_archive() (or shutdown()).
	{
		std::lock_guard<std::mutex> lock(state().archive_mutex);
		State &s = state();
		if (s.resolved_archive_probed) {
			return s.resolved_archive_path;
		}
	}
	const String logical = "res://textures.dspkg";
	String found;
	if (FileAccess::file_exists(logical)) {
		found = logical;
	} else {
		OS *os = OS::get_singleton();
		if (os) {
			String executable_archive = os->get_executable_path().get_base_dir().path_join("textures.dspkg");
			if (FileAccess::file_exists(executable_archive)) {
				found = executable_archive;
			}
		}
		if (found.is_empty()) {
			ProjectSettings *ps = ProjectSettings::get_singleton();
			if (ps) {
				String globalized = ps->globalize_path(logical);
				if (FileAccess::file_exists(globalized)) {
					found = globalized;
				}
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(state().archive_mutex);
		State &s = state();
		if (!s.resolved_archive_probed) {
			// First publisher wins; probes after this hit the fast path.
			s.resolved_archive_path = found;
			s.resolved_archive_probed = true;
		} else {
			// Lost a probe race while unlocked: honor the winner so every
			// caller in this generation agrees on one resolution.
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
		} else {
			Ref<DSPkg> fresh;
			fresh.instantiate();
			if (fresh->open(archive_path) != OK) {
				return false;
			}
			s.archive_path = archive_path;
			s.archive = fresh;
			archive = fresh;
		}
	}
	if (!archive.is_valid() || !archive->has_texture(p_engine_path)) {
		return false;
	}
	Dictionary info = archive->get_entry_info(p_engine_path);
	if (info.is_empty()) {
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
	r_out.pixel_format = info["pixel_format"];
	r_out.is_srgb = DSTex::is_rd_format_srgb((int)r_out.pixel_format);
	r_out.mip_count = info["mip_count"];
	r_out.is_v2 = (bool)info.get("has_gpu_metadata", false);
	// DSPkg entries carry no as_image_texture marker (get_entry_info has no
	// such key); projector/decal filtering happens in the scene-texture
	// collector via the .import option, never here.
	r_out.as_image_texture = false;
	r_out.data_offset = (uint64_t)info.get("data_offset", 0);
	r_out.compressed_size = (uint64_t)info.get("compressed_size", 0);
	r_out.original_size = (uint64_t)info.get("original_size", 0);
	// Missing key -> shared empty array default; the Variant->Packed cast is
	// the same conversion the explicit branch performed, now after one probe.
	r_out.mip_offsets = (PackedInt64Array)info.get("mip_offsets", PackedInt64Array());
	r_out.mip_sizes = (PackedInt32Array)info.get("mip_sizes", PackedInt32Array());
	r_out.mip_uncompressed_sizes = (PackedInt32Array)info.get("mip_uncompressed_sizes", PackedInt32Array());
	r_out.mip_row_pitches = (PackedInt32Array)info.get("mip_row_pitches", PackedInt32Array());
	return true;
}

void DSPreloader::invalidate_header_cache() {
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	s.header_cache_generation++;
	s.header_cache.clear();
	// Combined index shares the generation: bump + clear together so
	// shutdown()/rescan_archive() drop stale bundle headers with the same
	// single invalidation both caches observe.
	s.combined_index.clear();
	s.combined_index_path = String();
	s.combined_index_probed = false;
	s.combined_index_available = false;
}

uint32_t DSPreloader::crc32_ieee(const uint8_t *p_data, uint64_t p_len) {
	// IEEE CRC32 (poly 0xEDB88320, init 0xFFFFFFFF, xorout 0xFFFFFFFF).
	// Bitwise (no table) to keep the helper dependency-free; index payloads
	// are tens of KB so the per-bit cost is negligible. The GDScript
	// exporter writer mirrors this exact variant when emitting
	// textures.headers.idx.
	uint32_t crc = 0xFFFFFFFFU;
	for (uint64_t i = 0; i < p_len; ++i) {
		crc ^= (uint32_t)p_data[i];
		for (int b = 0; b < 8; ++b) {
			crc = (crc & 1U) ? (0xEDB88320U ^ (crc >> 1)) : (crc >> 1);
		}
	}
	return crc ^ 0xFFFFFFFFU;
}

String DSPreloader::resolve_combined_index_path() {
	// Sidecar sibling of the resolved archive (textures.dspkg ->
	// textures.headers.idx), falling back to res://textures.headers.idx so
	// editor dstex-direct runs without a dspkg still benefit. Probing here
	// is amortized: load_combined_index() calls this once per generation,
	// never per-path inside the hot loop.
	String archive_path = resolve_archive_path();
	if (!archive_path.is_empty()) {
		String dir = archive_path.get_base_dir();
		String file = archive_path.get_file();
		String base = file.get_basename();
		String sibling = dir.path_join(base + ".headers.idx");
		if (FileAccess::file_exists(sibling)) {
			return sibling;
		}
		// Archive-named fallback (e.g. custom bundle names): only when the
		// sibling above is missing, try the canonical res:// name.
	}
	const String canonical = "res://textures.headers.idx";
	if (FileAccess::file_exists(canonical)) {
		return canonical;
	}
	return String();
}

bool DSPreloader::load_combined_index() {
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		if (state().combined_index_probed) {
			return state().combined_index_available;
		}
	}
	// Resolve + open + bulk read outside the critical section: the index
	// for 100 entries is ~20KB, a single open + single bulk.
	String idx_path = resolve_combined_index_path();
	if (idx_path.is_empty()) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
			s.combined_index_path = String();
		}
		return false;
	}
	Ref<FileAccess> file = FileAccess::open(idx_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	uint64_t file_len = file->get_length();
	if (file_len < 16 || file_len > COMBINED_INDEX_MAX_BYTES) {
		file->close();
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	PackedByteArray bulk = file->get_buffer((int64_t)file_len);
	file->close();
	if ((uint64_t)bulk.size() != file_len || file_len < 16) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	const uint8_t *ptr = bulk.ptr();
	uint32_t magic = 0;
	uint16_t idx_version = 0;
	uint32_t entry_count = 0;
	uint32_t stored_crc = 0;
	memcpy(&magic, ptr, 4);
	memcpy(&idx_version, ptr + 4, 2);
	memcpy(&entry_count, ptr + 8, 4);
	memcpy(&stored_crc, ptr + 12, 4);
	if (magic != COMBINED_INDEX_MAGIC || idx_version != COMBINED_INDEX_VERSION ||
			entry_count == 0 || entry_count > COMBINED_INDEX_MAX_ENTRIES) {
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	// CRC covers all entry bytes after the 16-byte header.
	uint32_t calc = crc32_ieee(ptr + 16, file_len - 16);
	if (calc != stored_crc) {
		UtilityFunctions::push_warning("DSPreloader: Combined header index CRC mismatch; ignoring index.");
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	// Parse entries into a local map, then publish first-publisher-wins.
	HashMap<String, PayloadHeader> parsed;
	uint64_t off = 16;
	uint64_t gen = 0;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		gen = state().header_cache_generation;
	}
	uint32_t parsed_count = 0;
	for (uint32_t e = 0; e < entry_count; ++e) {
		if (off + 4 > file_len) {
			break;
		}
		uint32_t key_len = 0;
		memcpy(&key_len, ptr + off, 4);
		off += 4;
		if (key_len == 0 || key_len > 1024 || off + key_len > file_len) {
			break;
		}
		String key;
		{
			PackedByteArray key_bytes;
			key_bytes.resize((int64_t)key_len);
			memcpy(key_bytes.ptrw(), ptr + off, key_len);
			key = key_bytes.get_string_from_utf8();
		}
		off += key_len;
		// Fixed tail: 4*4 (w/h/fmt/flags) + 2+2 (version/mips) + 4*8
		// (orig/comp/mtime/file_len) + 16*8 + 16*4*3 = 16+4+32+128+192.
		constexpr uint64_t TAIL = 16ULL + 4ULL + 32ULL + 128ULL + 192ULL;
		bool entry_ok = !key.is_empty() && off + TAIL <= file_len;
		PayloadHeader h;
		uint32_t w = 0, hh = 0, fmt = 0, flags = 0;
		uint16_t dver = 0, mips = 0;
		if (entry_ok) {
			memcpy(&w, ptr + off, 4);
			memcpy(&hh, ptr + off + 4, 4);
			memcpy(&fmt, ptr + off + 8, 4);
			memcpy(&flags, ptr + off + 12, 4);
			memcpy(&dver, ptr + off + 16, 2);
			memcpy(&mips, ptr + off + 18, 2);
			off += 20;
			memcpy(&h.original_size, ptr + off, 8);
			memcpy(&h.compressed_size, ptr + off + 8, 8);
			memcpy(&h.mtime, ptr + off + 16, 8);
			memcpy(&h.file_len, ptr + off + 24, 8);
			off += 32;
			entry_ok = w != 0 && hh != 0 && mips != 0 && mips <= DSTEX_MAX_MIP_COUNT && (dver == 1 || dver == 2);
		}
		if (!entry_ok) {
			UtilityFunctions::push_warning("DSPreloader: Combined header index entry corrupt; ignoring index.");
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			if (!s.combined_index_probed) {
				s.combined_index_probed = true;
				s.combined_index_available = false;
			}
			return false;
		}
		h.width = w;
		h.height = hh;
		h.pixel_format = fmt;
		h.flags = flags;
		h.version = dver;
		h.mip_count = mips;
		h.open_failed = false;
		h.generation = gen;
		memcpy(h.mip_offsets, ptr + off, 16 * sizeof(int64_t));
		off += 128;
		memcpy(h.mip_sizes, ptr + off, 16 * sizeof(int32_t));
		off += 64;
		memcpy(h.mip_uncompressed_sizes, ptr + off, 16 * sizeof(int32_t));
		off += 64;
		memcpy(h.mip_row_pitches, ptr + off, 16 * sizeof(int32_t));
		off += 64;
		if (!parsed.has(key)) {
			parsed[key] = h;
		}
		parsed_count++;
	}
	if (parsed_count != entry_count) {
		UtilityFunctions::push_warning("DSPreloader: Combined header index truncated; ignoring index.");
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.combined_index_probed) {
			s.combined_index_probed = true;
			s.combined_index_available = false;
		}
		return false;
	}
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	if (s.combined_index_probed) {
		return s.combined_index_available;
	}
	if (gen != s.header_cache_generation) {
		// Generation bumped (shutdown/rescan) during our unlocked I/O:
		// parsed entries carry a stale stamp. Discard without marking so
		// the next serve retries under the new generation.
		return false;
	}
	// Entries already stamped with gen == current; publish first-wins.
	s.combined_index = parsed;
	s.combined_index_path = idx_path;
	s.combined_index_probed = true;
	s.combined_index_available = true;
	return true;
}

bool DSPreloader::serve_from_index(const String &p_dstex_path, PayloadHeader &r_out) {
	// Lazy ensure: first caller in a generation pays the single amortized
	// bulk load; the rest hit the probed map. No per-path open by design.
	if (!load_combined_index()) {
		return false;
	}
	// Freshness stat outside the lock (mirrors try_get_cached_header):
	// 1 stat, 0 opens. Mtime drift (re-imported dstex after bundle) falls
	// through to the header-cache/miss path instead of serving stale bytes.
	uint64_t mtime = FileAccess::get_modified_time(p_dstex_path);
	std::lock_guard<std::mutex> lock(state().mutex);
	State &s = state();
	const PayloadHeader *stored = s.combined_index.getptr(p_dstex_path);
	if (stored == nullptr || stored->generation != s.header_cache_generation || stored->mtime != mtime) {
		return false;
	}
	if (stored->open_failed) {
		return false;
	}
	r_out = *stored;
	return true;
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
	// (Covers every valid file: required = 48 + count * entry <=
	// 48 + 16 * 24 = MAX_BULK, so the buffer always spans the table
	// whenever required <= file_len.)
	constexpr uint64_t MAX_TABLE = (uint64_t)DSTEX_MAX_MIP_COUNT * sizeof(DSTexMipEntryV2);
	constexpr uint64_t MAX_BULK = sizeof(DSTexHeader) + MAX_TABLE;
	uint64_t want = file_len < MAX_BULK ? file_len : MAX_BULK;
	PackedByteArray bulk = file->get_buffer((int64_t)want);
	file->close();
	if (bulk.size() < (int64_t)sizeof(DSTexHeader)) {
		return false;
	}
	DSTexHeader header;
	memcpy(&header, bulk.ptr(), sizeof(DSTexHeader));

	// Validation mirrors DSTex::read_header exactly (magic, dimensions,
	// 1..16 mips, mip-table bounds) so the cache never accepts a file the
	// Dictionary path would reject.
	if (header.magic != 0x58455444) {
		UtilityFunctions::push_error("DSTex: Invalid magic number.");
		return false;
	}
	if (header.width == 0 || header.height == 0) {
		UtilityFunctions::push_error("DSTex: Invalid texture dimensions.");
		return false;
	}
	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
		return false;
	}
	uint64_t entry_size = (header.version == 1) ? sizeof(DSTexMipEntry) : sizeof(DSTexMipEntryV2);
	uint64_t mip_table_size = static_cast<uint64_t>(header.mip_count) * entry_size;
	{
		if (entry_size != 0 && mip_table_size / entry_size != static_cast<uint64_t>(header.mip_count)) {
			UtilityFunctions::push_error("DSTex: Mip table size overflow.");
			return false;
		}
		uint64_t required = sizeof(DSTexHeader) + mip_table_size;
		if (required < sizeof(DSTexHeader) || required > file_len) {
			UtilityFunctions::push_error("DSTex: Mip table exceeds file bounds.");
			return false;
		}
		if ((uint64_t)bulk.size() < required) {
			return false; // Short bulk read; treat as I/O miss.
		}
	}
	const uint8_t *table_ptr = bulk.ptr() + sizeof(DSTexHeader);

	PayloadHeader filled;
	filled.width = header.width;
	filled.height = header.height;
	filled.pixel_format = header.pixel_format;
	filled.mip_count = header.mip_count;
	filled.flags = header.flags;
	filled.version = header.version;
	filled.original_size = header.original_size;
	filled.compressed_size = header.compressed_size;
	filled.mtime = mtime;
	filled.file_len = file_len;
	filled.open_failed = false;

	if (header.version == 1) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			memcpy(&mip_entry, table_ptr + i * sizeof(DSTexMipEntry), sizeof(DSTexMipEntry));
			if (mip_entry.offset > file_len || mip_entry.size > file_len - mip_entry.offset ||
					(mip_entry.size != 0 && mip_entry.offset + mip_entry.size < mip_entry.offset)) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds or overflows.");
				return false;
			}
			filled.mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			filled.mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
	} else if (header.version == 2) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			memcpy(&mip_entry, table_ptr + i * sizeof(DSTexMipEntryV2), sizeof(DSTexMipEntryV2));
			if (mip_entry.offset > file_len || mip_entry.compressed_size > file_len - mip_entry.offset ||
					(mip_entry.compressed_size != 0 && mip_entry.offset + mip_entry.compressed_size < mip_entry.offset)) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds or overflows.");
				return false;
			}
			filled.mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			filled.mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			filled.mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			filled.mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
		}
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

void DSPreloader::payload_from_header(const String &p_engine_path, const String &p_dstex_path, const PayloadHeader &p_header, Payload &r_out) {
	r_out = Payload();
	r_out.engine_path = p_engine_path;
	r_out.payload_key = p_dstex_path;
	r_out.in_dspkg = false;
	r_out.file_path = p_dstex_path;
	r_out.width = p_header.width;
	r_out.height = p_header.height;
	r_out.pixel_format = p_header.pixel_format;
	// read_header reports is_srgb as flags-bit OR rd-format probe, and
	// as_image_texture as flags-bit (== its own as_image_texture key).
	r_out.is_srgb = ((p_header.flags & 1U) != 0) || DSTex::is_rd_format_srgb((int)p_header.pixel_format);
	r_out.as_image_texture = ((p_header.flags & 2U) != 0);
	r_out.mip_count = p_header.mip_count;
	r_out.original_size = p_header.original_size;
	r_out.compressed_size = p_header.compressed_size;
	r_out.is_v2 = p_header.version == 2;
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
	// for v2. Mirrors DSResourceFormatLoader::_load's dstex branch exactly.
	uint64_t data_offset = sizeof(DSTexHeader) + ((r_out.is_v2 ? sizeof(DSTexMipEntryV2) : sizeof(DSTexMipEntry)) * (uint64_t)r_out.mip_count);
	if (r_out.is_v2) {
		data_offset = (data_offset + 511) & ~uint64_t(511);
	}
	r_out.data_offset = data_offset;
}

bool DSPreloader::fill_from_dstex_file(const String &p_engine_path, const String &p_dstex_path, Payload &r_out) {
	// Resolve order: combined-index hit zero-open -> header-cache hit
	// 1 stat/0 opens -> miss single open + single bulk. The
	// package hit stays ahead of ALL of this in resolve_payload(), so
	// package-served textures never pay an index open.
	// Index freshness mirrors the header cache (generation + mtime match,
	// 1 stat, 0 opens); mtime drift falls through instead of serving stale.
	PayloadHeader indexed;
	if (serve_from_index(p_dstex_path, indexed)) {
		payload_from_header(p_engine_path, p_dstex_path, indexed, r_out);
		return true;
	}
	// Fast path: generation-keyed header cache. Positive hit costs one stat
	// (get_modified_time) and zero opens, zero Dictionary allocs. Failure
	// hit (open_failed) costs the same stat and fails without opening —
	// repeat unresolvable/removed dstex paths never pay a second open.
	PayloadHeader cached;
	if (try_get_cached_header(p_dstex_path, cached)) {
		if (cached.open_failed) {
			return false;
		}
		payload_from_header(p_engine_path, p_dstex_path, cached, r_out);
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
	payload_from_header(p_engine_path, p_dstex_path, fresh, r_out);
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
		req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
		req->set_cancellation_tag(p_tag);
		req->get_destination_buffer_ref().resize((int64_t)p_payload.original_size);
		r_out.push_back(req);
		return true;
	}
	r_out.reserve((int64_t)p_payload.mip_count);
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
		req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
		req->set_cancellation_tag(p_tag);
		req->get_destination_buffer_ref().resize((int64_t)uncomp);
		r_out.push_back(req);
	}
	return r_out.size() > 0;
}

uint64_t DSPreloader::prewarm_async(const PackedStringArray &p_paths) {
	DSContext *ctx = DSContext::get_singleton();
	if (ctx == nullptr) {
		UtilityFunctions::push_error("DSPreloader: DSContext singleton unavailable; prewarm_async aborted.");
		return 0;
	}
	DSTextureCache *cache = DSTextureCache::get_singleton();
	ResourceLoader *rl = ResourceLoader::get_singleton();

	// Boot warmer: ONE amortized combined-index load (1 open + 1 bulk for
	// up to MAX entries) before the resolve loop, instead of N cold
	// per-file opens on the file-direct path. Called once per prewarm,
	// never per-path inside the hot loop, so package-served hits stay
	// zero-stat/zero-open. prefetch_headers() (file-path batch warmer for
	// test/scene-collector callers) is deliberately NOT called here
	// per-path for the same reason — its upfront per-engine-path stats
	// would tax every package-served texture.
	load_combined_index();

	HashMap<String, bool> seen;
	std::vector<Payload> payloads;
	payloads.reserve((size_t)p_paths.size());
	int skipped_cached = 0;
	int skipped_unresolvable = 0;
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
				skipped_cached++;
				continue;
			}
		}
		if (rl && rl->has_cached(path)) {
			skipped_cached++;
			continue;
		}
		Payload payload;
		if (!resolve_payload(path, payload) || !validate_payload(payload)) {
			skipped_unresolvable++;
			continue;
		}
		// Payload bytes already cached under the remapped key (e.g. a sync
		// _load stored the .dstex key but the engine path alias is missing):
		// alias the engine path without spending a stream request.
		if (cache && payload.payload_key != payload.engine_path) {
			Ref<Texture2D> payload_hit = cache->get_cached(payload.payload_key, DSTextureCache::CACHE_MODE_REUSE);
			if (payload_hit.is_valid()) {
				cache->store(payload.engine_path, payload_hit, DSTextureCache::CACHE_MODE_REUSE);
				skipped_cached++;
				continue;
			}
		}
		payloads.push_back(payload);
	}

	// Tag first with an empty list (total 0); each stream enqueue below bumps
	// the tag total by exactly one, so total == tracked request count and
	// is_prewarm_complete() settles exactly when every request is finished.
	uint64_t tag = ctx->prewarm_async(PackedStringArray());
	int enqueued = 0;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		// Reused scratch vector: expand_requests() starts with
		// clear()+reserve(), so one buffer serves all payloads and only
		// the largest mip_count reallocates. tracked.requests = reqs below
		// copies the Refs out (pending holds them), so reuse is
		// use-after-clear safe. enqueue_request(req, true) semantics and
		// the all-or-nothing per-payload failure path are untouched.
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
			int64_t id = s.next_id++;
			if (s.next_id == 0) {
				s.next_id = 1;
			}
			s.pending[id] = tracked;
			enqueued += reqs.size();
		}
	}
	UtilityFunctions::print_verbose("DSPreloader: prewarm tag ", String::num_uint64(tag), " tracking ", (int64_t)payloads.size(), " textures (", (int64_t)enqueued, " stream requests, ", (int64_t)skipped_cached, " cached, ", (int64_t)skipped_unresolvable, " skipped).");
	return tag;
}

bool DSPreloader::is_terminal(const Ref<DSRequest> &p_request) {
	if (p_request.is_null() || !p_request.is_valid()) {
		return true;
	}
	DSRequest::Status status = p_request->get_status();
	return status == DSRequest::STATUS_COMPLETED || status == DSRequest::STATUS_FAILED;
}

Ref<Texture2D> DSPreloader::build_texture(const Payload &p_meta, const std::vector<PackedByteArray> &p_buffers) {
	if (p_meta.width == 0 || p_meta.height == 0 || p_meta.mip_count == 0 || p_buffers.empty()) {
		return Ref<Texture2D>();
	}
	const int img_fmt = DSTex::rd_format_to_godot_format((int)p_meta.pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		return Ref<Texture2D>();
	}

	// Assemble natural (unpadded) mip data. v2 buffers carry D3D12
	// footprints (256-byte row pitch); unpad per mip exactly like the sync
	// loader CPU path. v1 buffers are already natural.
	std::vector<PackedByteArray> natural_mips;
	uint32_t cur_w = p_meta.width;
	uint32_t cur_h = p_meta.height;
	size_t expected_mips = p_meta.is_v2 ? (size_t)p_meta.mip_count : 1;
	if (p_buffers.size() != expected_mips) {
		UtilityFunctions::push_warning("DSPreloader: Request/buffer count mismatch for '", p_meta.engine_path, "'; dropping prewarmed texture.");
		return Ref<Texture2D>();
	}
	for (size_t i = 0; i < p_buffers.size(); ++i) {
		if (p_buffers[i].size() == 0) {
			UtilityFunctions::push_warning("DSPreloader: Empty completed payload for '", p_meta.engine_path, "'; dropping prewarmed texture.");
			return Ref<Texture2D>();
		}
		if (p_meta.is_v2) {
			PackedByteArray natural = DSTex::unpad_footprint_mip(p_buffers[i], cur_w, cur_h, img_fmt);
			if (natural.size() == 0) {
				return Ref<Texture2D>();
			}
			natural_mips.push_back(natural);
		} else {
			natural_mips.push_back(p_buffers[i]);
		}
		cur_w = std::max<uint32_t>(1, cur_w / 2);
		cur_h = std::max<uint32_t>(1, cur_h / 2);
	}

	DSContext *ds_ctx = DSContext::get_singleton();
	bool gpu_active = ds_ctx != nullptr && ds_ctx->is_gpu_decompression_enabled();
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs != nullptr ? rs->get_rendering_device() : nullptr;

	// Thread-affinity gate for Texture2DRD construction. Mirrors the sync
	// loader: Texture2DRD (texture_create + get_driver_resource + free_rid)
	// asserts is_on_render_thread(), so off-render-thread completion
	// (main thread under multi-threaded rendering, ResourceLoader workers,
	// foreign backend threads) MUST take the CPU ImageTexture path, which
	// is worker-safe by construction. as_image_texture sources
	// (Light3D projector / Decal compatible) bypass the GPU path even
	// on the render thread.
	bool can_attempt_gpu = false;
	if (p_meta.as_image_texture) {
		UtilityFunctions::print_verbose("DSPreloader: Texture requested as ImageTexture for '" + p_meta.engine_path + "'; bypassing GPU upload.");
	} else if (gpu_active && p_meta.is_v2) {
		if (rs == nullptr || !rs->is_on_render_thread()) {
			UtilityFunctions::print_verbose("DSPreloader: GPU upload unavailable off render thread for '" + p_meta.engine_path + "'; using ImageTexture.");
		} else if (rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			UtilityFunctions::print_verbose("DSPreloader: GPU upload requires D3D12 driver for '" + p_meta.engine_path + "'; using ImageTexture.");
		} else if (rd == nullptr) {
			UtilityFunctions::print_verbose("DSPreloader: GPU upload unavailable (no RenderingDevice) for '" + p_meta.engine_path + "'; using ImageTexture.");
		} else {
			can_attempt_gpu = true;
		}
	}

	Ref<Texture2D> result;
	if (can_attempt_gpu && rd != nullptr) {
		Ref<RDTextureFormat> rd_fmt;
		rd_fmt.instantiate();
		rd_fmt->set_width((int32_t)p_meta.width);
		rd_fmt->set_height((int32_t)p_meta.height);
		rd_fmt->set_format((RenderingDevice::DataFormat)p_meta.pixel_format);
		rd_fmt->set_mipmaps((int32_t)(p_meta.mip_count > 0 ? p_meta.mip_count : 1));
		rd_fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		rd_fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);

		Ref<RDTextureView> rd_view;
		rd_view.instantiate();

		// Single-blob packing for TEXTURE_TYPE_2D (layers == 1).
		// RenderingDevice requires data.size() == array_layers
		// (rendering_device.cpp:1636-1638) with data[0].size() ==
		// get_image_format_required_size(...) (commons 761-799): one tight
		// blob with all mips concatenated, no inter-mip padding. Pitch math
		// (S3TC block vs uncompressed byte) lives in
		// DSTex::get_mip_layout/unpad_footprint_mip above; packing shape
		// stays single-blob for both. The v2 256-byte row-pitch /
		// 512-byte placement footprint metadata is consumed during unpad
		// (file offsets), not as padding inside this init blob.
		if (p_meta.mip_count == 0 || p_meta.mip_count > 16) {
			UtilityFunctions::push_warning("DSPreloader: Invalid mip count for '", p_meta.engine_path, "'; falling back to ImageTexture.");
			can_attempt_gpu = false;
		}
		uint64_t total_init_size = 0;
		if (can_attempt_gpu) {
			for (size_t i = 0; i < natural_mips.size(); ++i) {
				uint64_t mip_size = (uint64_t)natural_mips[i].size();
				if (mip_size == 0 || mip_size > UINT32_MAX || total_init_size > UINT32_MAX - mip_size) {
					UtilityFunctions::push_warning("DSPreloader: Invalid mip payload size for '", p_meta.engine_path, "'; falling back to ImageTexture.");
					can_attempt_gpu = false;
					break;
				}
				total_init_size += mip_size;
			}
		}
		PackedByteArray packed_init;
		if (can_attempt_gpu) {
			packed_init.resize((int64_t)total_init_size);
			uint8_t *dst = packed_init.ptrw();
			uint64_t dst_off = 0;
			for (size_t i = 0; i < natural_mips.size(); ++i) {
				int64_t mip_size = natural_mips[i].size();
				if (mip_size > 0) {
					memcpy(dst + dst_off, natural_mips[i].ptr(), (size_t)mip_size);
					dst_off += (uint64_t)mip_size;
				}
			}
		}
		TypedArray<PackedByteArray> init_data;
		if (can_attempt_gpu) {
			init_data.append(packed_init);
		}
		if (!can_attempt_gpu) {
			// Validation rejected the GPU init blob above; skip
			// texture_create and fall through to the CPU ImageTexture path.
		} else {
			RID tex_rid = rd->texture_create(rd_fmt, rd_view, init_data);
			if (tex_rid.is_valid()) {
				uint64_t native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, tex_rid, 0);
				if (native_format != 0) {
					Ref<Texture2DRD> tex_rd;
					tex_rd.instantiate();
					tex_rd->set_texture_rd_rid(tex_rid);
					result = tex_rd;
				} else {
					UtilityFunctions::push_warning("DSPreloader: Could not retrieve the native texture format for ", p_meta.engine_path, "; falling back to ImageTexture.");
					rd->free_rid(tex_rid);
				}
			} else {
				UtilityFunctions::push_warning("DSPreloader: GPU texture upload failed for ", p_meta.engine_path, "; falling back to ImageTexture.");
			}
		} // else: validated single-blob upload attempt above
	}

	if (result.is_null()) {
		PackedByteArray natural_data;
		for (size_t i = 0; i < natural_mips.size(); ++i) {
			natural_data.append_array(natural_mips[i]);
		}
		Ref<Image> image = Image::create_from_data((int)p_meta.width, (int)p_meta.height, p_meta.mip_count > 1, (Image::Format)img_fmt, natural_data);
		if (image.is_valid() && !image->is_empty()) {
			// sRGB fallback mirrors the sync loader: drill to an 8-bit
			// format and linearize so CPU-sampled colors match GPU sampling.
			if (p_meta.is_srgb) {
				if (image->get_format() != Image::FORMAT_RGB8 && image->get_format() != Image::FORMAT_RGBA8) {
					image->decompress();
					if (image->get_format() != Image::FORMAT_RGB8 && image->get_format() != Image::FORMAT_RGBA8) {
						image->convert(Image::FORMAT_RGBA8);
					}
				}
				if (image->get_format() == Image::FORMAT_RGB8 || image->get_format() == Image::FORMAT_RGBA8) {
					image->srgb_to_linear();
				}
			}
			result = ImageTexture::create_from_image(image);
		}
	}
	return result;
}

int DSPreloader::poll_completed(uint64_t p_tag) {
	std::vector<Tracked> ready;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		for (std::unordered_map<int64_t, Tracked>::iterator it = s.pending.begin(); it != s.pending.end();) {
			const Tracked &t = it->second;
			if (p_tag != 0 && t.tag != p_tag) {
				++it;
				continue;
			}
			bool all_terminal = true;
			for (int i = 0; i < t.requests.size(); ++i) {
				if (!is_terminal(t.requests[i])) {
					all_terminal = false;
					break;
				}
			}
			if (!all_terminal) {
				++it;
				continue;
			}
			ready.push_back(t); // Ref copies keep requests alive past erase
			it = s.pending.erase(it);
		}
	}
	if (ready.empty()) {
		return 0;
	}
	DSTextureCache *cache = DSTextureCache::get_singleton();
	if (cache == nullptr) {
		UtilityFunctions::push_warning("DSPreloader: DSTextureCache singleton unavailable during poll; dropping ", (int64_t)ready.size(), " completed prewarms.");
		return 0;
	}
	int stored = 0;
	for (size_t i = 0; i < ready.size(); ++i) {
		const Tracked &t = ready[i];
		bool ok = true;
		std::vector<PackedByteArray> buffers;
		for (int r = 0; r < t.requests.size(); ++r) {
			if (t.requests[r].is_null() || t.requests[r]->get_status() != DSRequest::STATUS_COMPLETED) {
				ok = false;
				break;
			}
			buffers.push_back(t.requests[r]->get_destination_buffer());
		}
		if (!ok) {
			UtilityFunctions::print_verbose("DSPreloader: Prewarm texture '", t.meta.engine_path, "' did not complete; dropping (sync load remains available).");
			continue;
		}
		Ref<Texture2D> tex = build_texture(t.meta, buffers);
		if (tex.is_null()) {
			UtilityFunctions::push_warning("DSPreloader: Failed to build prewarmed texture for ", t.meta.engine_path, ".");
			continue;
		}
		// Store under BOTH keys so the later sync scene load binds
		// pointer-identical textures whether the engine queries the public
		// engine path or the remapped payload path (#14). Engine
		// ResourceCache residency is handled inside store() via
		// take_over_path, so ResourceLoader.load(..., CACHE_MODE_REUSE)
		// binds without a disk re-read.
		cache->store(t.meta.engine_path, tex, DSTextureCache::CACHE_MODE_REUSE);
		if (t.meta.payload_key != t.meta.engine_path) {
			cache->store(t.meta.payload_key, tex, DSTextureCache::CACHE_MODE_REUSE);
		}
		stored++;
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

void DSPreloader::shutdown() {
	std::lock_guard<std::mutex> archive_lock(state().archive_mutex);
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().pending.clear();
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

} // namespace godot
