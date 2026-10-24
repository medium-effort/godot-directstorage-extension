#include "ds_preloader.h"
#include "ds_texture_cache.h"
#include "../core/ds_context.h"
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
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace godot {

// Upper bound per prewarm request output buffer. The sync loader imposes no
// cap, but prewarm fans out to dozens of speculative buffers, so a corrupt
// header claiming gigabytes must be rejected before resize() runs.
static constexpr uint64_t MAX_PREWARM_REQUEST_BYTES = 512ULL * 1024ULL * 1024ULL;

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

	bool force_resolution = ps && ps->has_setting("directstorage/force_archive_resolution") &&
			static_cast<bool>(ps->get_setting("directstorage/force_archive_resolution"));

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
	if (ps && ps->has_setting("directstorage/archive_name")) {
		String custom_name = ps->get_setting("directstorage/archive_name");
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
		} else {
			Ref<DSPkg> fresh;
			fresh.instantiate();
			uint64_t expected_fingerprint = 0;
			ProjectSettings *ps = ProjectSettings::get_singleton();
			if (ps && ps->has_setting("directstorage/expected_archive_fingerprint")) {
				expected_fingerprint = static_cast<uint64_t>(ps->get_setting("directstorage/expected_archive_fingerprint"));
			}
			Error open_err = fresh->open(archive_path, expected_fingerprint);
			if (open_err != OK) {
				if (open_err == ERR_UNAUTHORIZED) {
					static String s_last_reported_preloader_unauth;
					if (s_last_reported_preloader_unauth != archive_path) {
						s_last_reported_preloader_unauth = archive_path;
						UtilityFunctions::push_error("DirectStorage Preloader: Decryption failed for package '" + archive_path + "'. Ensure valid encryption key is configured via DSPkg::set_global_encryption_key().");
					}
				} else {
					UtilityFunctions::push_error("DirectStorage Preloader: Failed to open DirectStorage package '" + archive_path + "': " + UtilityFunctions::error_string(open_err));
				}
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
	r_out.is_srgb = (bool)info.get("is_srgb", false) || DSTex::is_rd_format_srgb((int)r_out.pixel_format);
	r_out.mip_count = info["mip_count"];
	r_out.is_v2 = (bool)info.get("has_gpu_metadata", false);
	r_out.as_image_texture = (bool)info.get("as_image_texture", false);
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
	// Color-sampled formats only: linear color formats carry an sRGB
	// counterpart path and already-sRGB formats map to self; float and
	// single/dual-channel data formats (which map to themselves) and
	// unknown formats (MAX) are out.
	if (DSTex::is_rd_format_srgb(static_cast<int>(p_payload.pixel_format))) {
		// Already-sRGB color base: GPU uploads it directly.
	} else {
		const int srgb_fmt = DSTex::get_srgb_rd_format(static_cast<int>(p_payload.pixel_format));
		if (srgb_fmt == RenderingDevice::DATA_FORMAT_MAX || srgb_fmt == static_cast<int>(p_payload.pixel_format)) {
			return false;
		}
	}
	const int64_t mips = (int64_t)p_payload.mip_count;
	if (p_payload.mip_offsets.size() < mips || p_payload.mip_sizes.size() < mips ||
			p_payload.mip_uncompressed_sizes.size() < mips) {
		return false;
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
	for (size_t i = 0; i < orphans.size(); ++i) {
		if (orphans[i].is_valid()) {
			rd->free_rid(orphans[i]);
		}
	}
}

uint64_t DSPreloader::prewarm_gpu_async(const PackedStringArray &p_paths, int p_chunk_size, bool p_prefer_gpu) {
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
		UtilityFunctions::print("[DirectStorage] Integrated/UMA GPU detected: prewarm_gpu_async auto-routed to System RAM prewarm to eliminate loading screen hitches (directstorage/auto_tune_for_integrated_gpu=true).");
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
	{
		Vector<Ref<DSRequest>> reqs;
		for (size_t i = 0; i < payloads.size(); ++i) {
			if (gpu_ok && is_gpu_prewarm_eligible(payloads[i])) {
				GpuChunkItem item;
				item.meta = payloads[i];
				item.pending_id = 0; // assigned at publish below
				gpu_items.push_back(item);
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
		// Slice into chunks of at most chunk_size (bounded render-thread
		// time per callback); leftovers ride as a final short chunk.
		for (size_t base = 0; base < chunk.items.size(); base += (size_t)chunk_size) {
			GpuChunk part;
			part.tag = tag;
			size_t end = base + (size_t)chunk_size;
			if (end > chunk.items.size()) {
				end = chunk.items.size();
			}
			part.items.assign(chunk.items.begin() + base, chunk.items.begin() + end);
			s.gpu_chunks.push_back(part);
		}
	}
	ctx->set_prewarm_total(tag, enqueued + (int)gpu_items.size());
	if (enqueued > 0) {
		ctx->flush_stream_async();
	}
	// Kick the first GPU chunk without waiting for a batch-manager tick.
	pump_gpu_prewarm();
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
				ctx->count_stream_terminal_failure(dead_chunk.tag);
			}
		}
		if (!s.gpu_chunk_in_flight && !s.gpu_chunks.empty() && is_gpu_prewarm_available()) {
			s.gpu_current = s.gpu_chunks.front();
			s.gpu_chunks.pop_front();
			s.gpu_current_valid = true;
			s.gpu_chunk_in_flight = true;
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
			s.gpu_current_valid = false;
			s.gpu_chunk_in_flight = false;
		}
	}
	pump_gpu_orphans();
}

void DSPreloader::alloc_gpu_item_on_render_thread(RenderingDevice *p_rd, DSContext *p_ctx, uint64_t p_tag, const GpuChunkItem &p_item) {
	const Payload &m = p_item.meta;
	// Re-validate: availability may have changed between pump and alloc.
	if (!is_gpu_prewarm_eligible(m)) {
		mark_gpu_item_failed(p_item.pending_id);
		if (p_ctx != nullptr) {
			p_ctx->count_stream_terminal_failure(p_tag);
		}
		return;
	}
	const int img_fmt = DSTex::rd_format_to_godot_format((int)m.pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		mark_gpu_item_failed(p_item.pending_id);
		if (p_ctx != nullptr) {
			p_ctx->count_stream_terminal_failure(p_tag);
		}
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
		if (p_ctx != nullptr) {
			p_ctx->count_stream_terminal_failure(p_tag);
		}
		return;
	}
	uint64_t native = p_rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, tex_rid, 0);
	if (native == 0) {
		UtilityFunctions::push_warning("DSPreloader: GPU native format query failed for '", m.engine_path, "'; sync load remains available.");
		p_rd->free_rid(tex_rid);
		mark_gpu_item_failed(p_item.pending_id);
		if (p_ctx != nullptr) {
			p_ctx->count_stream_terminal_failure(p_tag);
		}
		return;
	}

	// Loader-style single request spanning all mips (backend validates the
	// subresource range against the freshly created texture).
	const uint64_t start_offset = static_cast<uint64_t>(m.mip_offsets[0]);
	const uint64_t last_idx = static_cast<uint64_t>(m.mip_count - 1);
	const uint64_t total_span = (static_cast<uint64_t>(m.mip_offsets[last_idx]) + static_cast<uint64_t>(m.mip_sizes[last_idx])) - start_offset;
	uint64_t total_uncomp = 0;
	for (uint32_t i = 0; i < m.mip_count; ++i) {
		total_uncomp += static_cast<uint64_t>(m.mip_uncompressed_sizes[i]);
	}
	if (total_span == 0 || total_span > UINT32_MAX || total_uncomp == 0 || total_uncomp > UINT32_MAX) {
		UtilityFunctions::push_warning("DSPreloader: GPU payload span out of range for '", m.engine_path, "'; sync load remains available.");
		p_rd->free_rid(tex_rid);
		mark_gpu_item_failed(p_item.pending_id);
		if (p_ctx != nullptr) {
			p_ctx->count_stream_terminal_failure(p_tag);
		}
		return;
	}

	Ref<DSRequest> req;
	req.instantiate();
	req->set_file_path(m.file_path);
	req->set_offset(start_offset);
	req->set_size(static_cast<uint32_t>(total_span));
	req->set_uncompressed_size(static_cast<uint32_t>(total_uncomp));
	req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
	req->set_cancellation_tag(p_tag);
	req->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
	req->set_destination_resource(tex_rid);
	req->set_first_subresource(0);
	req->set_subresource_count(m.mip_count);
	req->set_texture_format(static_cast<uint32_t>(native));
	if (!p_ctx->enqueue_request(req, true)) {
		// Cancelled tag (already terminal-counted): release the texture,
		// drop the pending entry via the failed flag.
		p_rd->free_rid(tex_rid);
		mark_gpu_item_failed(p_item.pending_id);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		std::unordered_map<int64_t, Tracked>::iterator it = state().pending.find(p_item.pending_id);
		if (it == state().pending.end()) {
			// Entry vanished (shutdown/rescan): release, nothing to attach.
			p_rd->free_rid(tex_rid);
			return;
		}
		it->second.gpu_tex_rid = tex_rid;
		it->second.requests.push_back(req);
	}
}

void DSPreloader::process_gpu_chunk_on_render_thread() {
	RenderingServer *rs = RenderingServer::get_singleton();
	GpuChunk chunk;
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		if (!s.gpu_current_valid) {
			s.gpu_chunk_in_flight = false;
			return;
		}
		chunk = s.gpu_current;
		s.gpu_current.items.clear();
		s.gpu_current_valid = false;
	}
	if (rs == nullptr || !rs->is_on_render_thread()) {
		// Wrong thread (should not happen: posted via
		// call_on_render_thread). Re-queue at front; pump retries.
		std::lock_guard<std::mutex> lock(state().mutex);
		State &s = state();
		s.gpu_chunks.push_front(chunk);
		s.gpu_chunk_in_flight = false;
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
			ctx->count_stream_terminal_failure(chunk.tag);
		}
		s.gpu_chunk_in_flight = false;
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
				ctx->count_stream_terminal_failure(chunk.tag);
			}
		}
		s.gpu_chunk_in_flight = false;
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
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().gpu_chunk_in_flight = false;
	}
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
	// Ownership gate (#0126): store() refuses when the cache is disabled, and a
	// Texture2DRD has no self-freeing owner, so wrapping an RID the cache will
	// not take would leak it. Release it on the render thread instead and leave
	// the synchronous load available. p_cache cannot be null here: the only
	// caller, poll_completed(), returns early when the singleton is missing.
	if (!p_cache->can_own(DSTextureCache::CACHE_MODE_REUSE)) {
		queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		UtilityFunctions::push_warning("DSPreloader: texture cache will not own GPU prewarm texture '", p_entry.meta.engine_path, "'; releasing its RID (sync load remains available).");
		return;
	}
	Ref<Texture2DRD> tex_rd;
	tex_rd.instantiate();
	tex_rd->set_texture_rd_rid(p_entry.gpu_tex_rid);
	const String alias = (p_entry.meta.payload_key != p_entry.meta.engine_path) ? p_entry.meta.payload_key : String();
	// can_own() is a pre-check, not a reservation. If the cache was disabled
	// between it and store(), store() reports refusal and we still own the RID.
	if (!p_cache->store(p_entry.meta.engine_path, tex_rd, DSTextureCache::CACHE_MODE_REUSE, alias)) {
		queue_gpu_orphan_rid(p_entry.gpu_tex_rid);
		UtilityFunctions::push_warning("DSPreloader: texture cache refused ownership of GPU prewarm texture '", p_entry.meta.engine_path, "' (disabled or raced); releasing its RID.");
		return;
	}
	r_stored++;
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
		return ImageTexture::create_from_image(image);
	}
	return Ref<Texture2D>();
}

int DSPreloader::poll_completed(uint64_t p_tag, int p_max_items, uint32_t p_max_usec) {
	DSTextureCache *cache = DSTextureCache::get_singleton();
	if (cache == nullptr) {
		UtilityFunctions::push_warning("DSPreloader: DSTextureCache singleton unavailable during poll.");
		return 0;
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
			if (cand.is_gpu) {
				// Chunk not yet attached (no requests) or worker poll:
				// leave for the main/render-thread poll. Failed entries
				// are collected on any thread to drop them promptly.
				if (!cand.gpu_failed && (cand.requests.is_empty() || !can_build_gpu)) {
					continue;
				}
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
				if (p_max_items > 0 && (int)(gpu_terminal_ids.size() + cpu_terminal_ids.size() + processed) >= p_max_items) {
					break;
				}
			}
		}
		terminal_ids.reserve(gpu_terminal_ids.size() + cpu_terminal_ids.size());
		terminal_ids.insert(terminal_ids.end(), gpu_terminal_ids.begin(), gpu_terminal_ids.end());
		terminal_ids.insert(terminal_ids.end(), cpu_terminal_ids.begin(), cpu_terminal_ids.end());
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
		if (processed > 0 && slice_bytes_processed >= kMaxBytesPerSlice) {
			break;
		}

		Tracked t;
		bool found = false;
		bool budget_exhausted = false;
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			std::unordered_map<int64_t, Tracked>::iterator it = s.pending.find(terminal_ids[ti]);
			if (it != s.pending.end()) {
				// Tag filter re-check: a concurrent poll may have retagged?
				// Pending entries are immutable once enqueued, but the
				// caller tag may differ per concurrent poll call.
				if (p_tag == 0 || it->second.tag == p_tag) {
					uint64_t est_bytes = estimate_payload_bytes(it->second.meta);
					// For CPU builds, estimate ~50us per MB for memcpy/unpad; GPU is instant (<2us)
					uint64_t est_usec = it->second.is_gpu ? 2 : (est_bytes / (1024ULL * 1024ULL)) * 50ULL;
					uint64_t elapsed = (p_max_usec > 0 && Time::get_singleton()) ? (Time::get_singleton()->get_ticks_usec() - start_usec) : 0;

					// Pre-estimate defer: If adding this texture exceeds byte or time budget, defer it to next slice
					if (processed > 0 && (slice_bytes_processed + est_bytes > kMaxBytesPerSlice || (p_max_usec > 0 && elapsed + est_usec > p_max_usec))) {
						budget_exhausted = true;
					} else {
						t = it->second;
						s.pending.erase(it);
						found = true;
					}
				}
			}
		}
		if (budget_exhausted) {
			break;
		}
		if (!found) {
			continue;
		}

		bool ok = true;
		std::vector<PackedByteArray> buffers;
		if (!t.is_gpu) {
			for (int r = 0; r < t.requests.size(); ++r) {
				if (t.requests[r].is_null() || t.requests[r]->get_status() != DSRequest::STATUS_COMPLETED) {
					ok = false;
					break;
				}
				buffers.push_back(t.requests[r]->get_destination_buffer());
			}
		}

		if (t.is_gpu) {
			complete_gpu_entry(t, cache, stored);
			slice_bytes_processed += estimate_payload_bytes(t.meta);
		} else if (ok) {
			Ref<Texture2D> tex = build_texture(t.meta, buffers);
			if (tex.is_valid()) {
				String alias = (t.meta.payload_key != t.meta.engine_path) ? t.meta.payload_key : String();
				// Report the decision: a refused store (cache disabled or raced)
				// keeps nothing resident, so it must not count as stored.
				if (cache->store(t.meta.engine_path, tex, DSTextureCache::CACHE_MODE_REUSE, alias)) {
					stored++;
				} else {
					UtilityFunctions::print_verbose("DSPreloader: cache refused prewarmed texture '", t.meta.engine_path, "'; not counted as stored.");
				}
				uint64_t entry_bytes = estimate_payload_bytes(t.meta);
				if (entry_bytes == 0) {
					for (size_t b = 0; b < buffers.size(); ++b) {
						entry_bytes += (uint64_t)buffers[b].size();
					}
				}
				slice_bytes_processed += entry_bytes;
			} else {
				UtilityFunctions::push_warning("DSPreloader: Failed to build prewarmed texture for ", t.meta.engine_path, ".");
			}
		} else {
			UtilityFunctions::print_verbose("DSPreloader: Prewarm texture '", t.meta.engine_path, "' did not complete; dropping (sync load remains available).");
		}
		processed++;

		// Safely unblock any worker threads awaiting this specific texture
		{
			std::lock_guard<std::mutex> lock(state().mutex);
			State &s = state();
			if (!t.meta.engine_path.is_empty()) {
				auto it_p = s.pending_paths.find(t.meta.engine_path);
				if (it_p != s.pending_paths.end()) {
					it_p->value--;
					if (it_p->value <= 0) {
						s.pending_paths.erase(t.meta.engine_path);
					}
				}
			}
			if (!t.meta.payload_key.is_empty() && t.meta.payload_key != t.meta.engine_path) {
				auto it_p = s.pending_paths.find(t.meta.payload_key);
				if (it_p != s.pending_paths.end()) {
					it_p->value--;
					if (it_p->value <= 0) {
						s.pending_paths.erase(t.meta.payload_key);
					}
				}
			}
			s.pending_cv.notify_all();
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
		if ((p_tag == 0 || it->second.tag == p_tag) && !it->second.is_gpu) {
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

void DSPreloader::shutdown() {
	std::lock_guard<std::mutex> archive_lock(state().archive_mutex);
	{
		std::lock_guard<std::mutex> lock(state().mutex);
		state().pending.clear();
		state().pending_paths.clear();
		state().gpu_chunks.clear();
		state().gpu_current.items.clear();
		state().gpu_current_valid = false;
		state().gpu_chunk_in_flight = false;
		// RD is tearing down: destination RIDs die with it, drop orphans
		// without freeing.
		state().gpu_orphans.clear();
		state().gpu_orphan_free_posted = false;
		state().pending_cv.notify_all();
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
		poll_completed();
		if (cache != nullptr) {
			return cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
		}
		return Ref<Texture2D>();
	}

	// On a worker thread: wait for the render thread (e.g. DSRuntime::_process)
	// to complete the in-flight stream request and store the texture into DSTextureCache.
	std::unique_lock<std::mutex> lock(state().mutex);
	State &s = state();

	if (!s.pending_paths.has(p_path)) {
		lock.unlock();
		if (cache != nullptr) {
			return cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
		}
		return Ref<Texture2D>();
	}

	s.pending_cv.wait_for(lock, std::chrono::milliseconds(p_timeout_ms), [&s, &p_path]() {
		return !s.pending_paths.has(p_path);
	});
	if (s.pending_paths.has(p_path)) {
		UtilityFunctions::print_verbose("[DirectStorage] await_pending_texture timed out waiting for '", p_path, "' after ", (int64_t)p_timeout_ms, " ms; falling through to cache/CPU path.");
	}

	lock.unlock();
	if (cache != nullptr) {
		return cache->get_cached(p_path, DSTextureCache::CACHE_MODE_REUSE);
	}
	return Ref<Texture2D>();
}

} // namespace godot
