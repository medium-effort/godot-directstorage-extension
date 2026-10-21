#include "dspkg_format.h"
#include "dstex_format.h"
#include "monocypher.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace godot {

namespace {

constexpr uint64_t DSPKG_MAX_PAYLOAD_SIZE = static_cast<uint64_t>(INT32_MAX);
// FMT-01: per-entry sane cap matching DSTEX_MAX_TOTAL_BYTES (512 MiB) so a
// hostile archive entry cannot force a huge transient allocation.
constexpr uint64_t DSPKG_MAX_ENTRY_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256;
constexpr uint32_t D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512;

static const uint8_t DOMAIN_SEPARATOR[8] = {'D', 'S', 'P', 'K', 'G', 'v', '4', 0x01};

#ifdef DSPKG_ENCRYPTION_KEY_BAKED
static_assert(sizeof(DSPKG_ENCRYPTION_KEY_BAKED) - 1 == 64, "Baked DSPKG encryption key must be exactly 64 hex characters");
static const char *const s_baked_encryption_key_hex = DSPKG_ENCRYPTION_KEY_BAKED;
#else
static const char *const s_baked_encryption_key_hex = "";
#endif

enum KeyResolutionState {
	KEY_STATE_DEFAULT,
	KEY_STATE_OVERRIDE,
	KEY_STATE_DISABLED
};

static std::mutex s_key_mutex;
static KeyResolutionState s_key_resolution_state = KEY_STATE_DEFAULT;
static std::array<uint8_t, 32> s_global_encryption_key_bytes = {};
static std::mutex s_unauth_log_mutex;
static uint64_t s_last_unauth_path_hash = 0;
static bool s_has_unauth_path = false;

static String key_id_to_hex(const uint8_t id_buf[8]) {
	String result;
	for (int i = 0; i < 8; ++i) {
		result += String::num_int64((id_buf[i] >> 4) & 0xF, 16);
		result += String::num_int64(id_buf[i] & 0xF, 16);
	}
	return result.to_lower();
}

static bool parse_hex_key_32(const String &p_hex, std::array<uint8_t, 32> &r_bytes) {
	if (p_hex.length() != 64) return false;
	for (int i = 0; i < 64; ++i) {
		char32_t c = p_hex[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
			return false;
		}
	}
	PackedByteArray decoded = p_hex.hex_decode();
	if (decoded.size() != 32) {
		if (decoded.size() > 0) {
			crypto_wipe(decoded.ptrw(), decoded.size());
		}
		return false;
	}
	memcpy(r_bytes.data(), decoded.ptr(), 32);
	crypto_wipe(decoded.ptrw(), decoded.size());
	return true;
}

static bool parse_hex_key_cstr_32(const char *p_hex, std::array<uint8_t, 32> &r_bytes) {
	if (!p_hex || strlen(p_hex) != 64) return false;
	for (size_t i = 0; i < 32; ++i) {
		auto hex_nibble = [](char c) -> int {
			if (c >= '0' && c <= '9') return c - '0';
			if (c >= 'a' && c <= 'f') return c - 'a' + 10;
			if (c >= 'A' && c <= 'F') return c - 'A' + 10;
			return -1;
		};
		int hi = hex_nibble(p_hex[i * 2]);
		int lo = hex_nibble(p_hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) return false;
		r_bytes[i] = static_cast<uint8_t>((hi << 4) | lo);
	}
	return true;
}

static bool resolve_encryption_key(std::array<uint8_t, 32> &r_key_bytes, uint64_t &r_key_id) {
	std::lock_guard<std::mutex> lock(s_key_mutex);
	if (s_key_resolution_state == KEY_STATE_DISABLED) {
		crypto_wipe(r_key_bytes.data(), 32);
		return false;
	}
	if (s_key_resolution_state == KEY_STATE_OVERRIDE) {
		r_key_bytes = s_global_encryption_key_bytes;
		uint8_t id_buf[8] = {0};
		crypto_blake2b(id_buf, 8, r_key_bytes.data(), 32);
		memcpy(&r_key_id, id_buf, 8);
		return true;
	}
	if (s_baked_encryption_key_hex[0] != '\0') {
		if (parse_hex_key_cstr_32(s_baked_encryption_key_hex, r_key_bytes)) {
			uint8_t id_buf[8] = {0};
			crypto_blake2b(id_buf, 8, r_key_bytes.data(), 32);
			memcpy(&r_key_id, id_buf, 8);
			return true;
		} else {
			UtilityFunctions::push_error("DSPkg: Baked encryption key failed hex validation.");
		}
	}
	crypto_wipe(r_key_bytes.data(), 32);
	return false;
}

class TOCBufferReader {
	const uint8_t *buffer = nullptr;
	uint64_t size = 0;
	uint64_t cursor = 0;
	bool has_error = false;
public:
	TOCBufferReader(const uint8_t *p_buf, uint64_t p_size) : buffer(p_buf), size(p_size), cursor(0), has_error(false) {}
	bool has_remaining(uint64_t bytes) const { return !has_error && (cursor + bytes <= size) && (cursor + bytes >= cursor); }
	uint64_t get_cursor() const { return cursor; }
	uint64_t get_size() const { return size; }
	bool is_ok() const { return !has_error; }
	uint64_t get_64() { if (!has_remaining(8)) { has_error = true; return 0; } uint64_t val; memcpy(&val, buffer + cursor, 8); cursor += 8; return val; }
	uint32_t get_32() { if (!has_remaining(4)) { has_error = true; return 0; } uint32_t val; memcpy(&val, buffer + cursor, 4); cursor += 4; return val; }
	uint16_t get_16() { if (!has_remaining(2)) { has_error = true; return 0; } uint16_t val; memcpy(&val, buffer + cursor, 2); cursor += 2; return val; }
	bool get_buffer(uint8_t *dst, uint64_t bytes) { if (!has_remaining(bytes)) { has_error = true; return false; } memcpy(dst, buffer + cursor, bytes); cursor += bytes; return true; }
};

class TOCBufferWriter {
	uint8_t *buffer = nullptr;
	uint64_t capacity = 0;
	uint64_t cursor = 0;
	bool has_error = false;
public:
	TOCBufferWriter(uint8_t *p_buf, uint64_t p_cap) : buffer(p_buf), capacity(p_cap), cursor(0), has_error(false) {}
	bool has_space(uint64_t bytes) const { return !has_error && (cursor + bytes <= capacity) && (cursor + bytes >= cursor); }
	uint64_t get_cursor() const { return cursor; }
	bool is_ok() const { return !has_error; }
	void put_64(uint64_t val) { if (!has_space(8)) { has_error = true; return; } memcpy(buffer + cursor, &val, 8); cursor += 8; }
	void put_32(uint32_t val) { if (!has_space(4)) { has_error = true; return; } memcpy(buffer + cursor, &val, 4); cursor += 4; }
	void put_16(uint16_t val) { if (!has_space(2)) { has_error = true; return; } memcpy(buffer + cursor, &val, 2); cursor += 2; }
	void put_buffer(const uint8_t *src, uint64_t bytes) { if (!has_space(bytes)) { has_error = true; return; } memcpy(buffer + cursor, src, bytes); cursor += bytes; }
};

bool checked_add(uint64_t p_left, uint64_t p_right, uint64_t &r_result) {
	if (p_right > UINT64_MAX - p_left) {
		return false;
	}
	r_result = p_left + p_right;
	return true;
}

bool align_up(uint64_t p_value, uint64_t p_alignment, uint64_t &r_result) {
	if (p_alignment == 0) {
		return false;
	}
	uint64_t remainder = p_value % p_alignment;
	return checked_add(p_value, remainder == 0 ? 0 : p_alignment - remainder, r_result);
}

bool checked_multiply(uint64_t p_left, uint64_t p_right, uint64_t &r_result) {
	if (p_left != 0 && p_right > UINT64_MAX / p_left) {
		return false;
	}
	r_result = p_left * p_right;
	return true;
}

bool get_payload_range(const DSPkgHeader &p_header, const DSPkgEntry &p_entry, uint64_t &r_start, uint64_t &r_end) {
	return checked_add(p_header.data_offset, p_entry.data_offset, r_start) && checked_add(r_start, p_entry.compressed_size, r_end);
}

bool payload_sizes_are_supported(const DSPkgEntry &p_entry) {
	return p_entry.compressed_size > 0 && p_entry.original_size > 0 &&
			p_entry.compressed_size <= DSPKG_MAX_PAYLOAD_SIZE && p_entry.original_size <= DSPKG_MAX_PAYLOAD_SIZE &&
			p_entry.compressed_size <= DSPKG_MAX_ENTRY_BYTES && p_entry.original_size <= DSPKG_MAX_ENTRY_BYTES;
}

bool metadata_arrays_are_valid(const Dictionary &p_metadata, uint32_t p_mip_count) {
	int metadata_version = p_metadata.get("version", 0);
	if (metadata_version != 2) {
		return false;
	}
	PackedInt32Array mip_sizes = p_metadata.get("mip_sizes", PackedInt32Array());
	PackedInt32Array mip_uncompressed_sizes = p_metadata.get("mip_uncompressed_sizes", PackedInt32Array());
	PackedInt32Array mip_row_pitches = p_metadata.get("mip_row_pitches", PackedInt32Array());
	return mip_sizes.size() == static_cast<int64_t>(p_mip_count) &&
			mip_uncompressed_sizes.size() == static_cast<int64_t>(p_mip_count) &&
			mip_row_pitches.size() == static_cast<int64_t>(p_mip_count);
}

bool calculate_packed_payload_offset(uint64_t p_current_offset, uint64_t p_item_size, uint64_t p_chunk_alignment, uint64_t &r_result) {
	if (p_chunk_alignment == 0 || (p_chunk_alignment % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) != 0) {
		return false;
	}
	uint64_t aligned_512 = 0;
	if (!align_up(p_current_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, aligned_512)) {
		return false;
	}
	uint64_t chunk_idx = aligned_512 / p_chunk_alignment;
	uint64_t current_chunk_end = 0;
	if (!checked_multiply(chunk_idx + 1, p_chunk_alignment, current_chunk_end)) {
		return false;
	}
	uint64_t item_end = 0;
	if (!checked_add(aligned_512, p_item_size, item_end)) {
		return false;
	}
	if (item_end > current_chunk_end && (aligned_512 % p_chunk_alignment) != 0) {
		r_result = current_chunk_end;
	} else {
		r_result = aligned_512;
	}
	return true;
}

} // namespace

void DSPkg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "output_path", "archive_fingerprint"), &DSPkg::create, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("set_archive_fingerprint", "archive_fingerprint"), &DSPkg::set_archive_fingerprint);
	ClassDB::bind_method(D_METHOD("set_payload_alignment", "alignment"), &DSPkg::set_payload_alignment);
	ClassDB::bind_method(D_METHOD("get_payload_alignment"), &DSPkg::get_payload_alignment);
	ClassDB::bind_method(D_METHOD("set_bin_packing_enabled", "enabled"), &DSPkg::set_bin_packing_enabled);
	ClassDB::bind_method(D_METHOD("is_bin_packing_enabled"), &DSPkg::is_bin_packing_enabled);
	ClassDB::bind_method(D_METHOD("add_texture", "resource_path", "dstex_path", "mip_metadata"), &DSPkg::add_texture, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("set_encryption_key", "key_hex"), &DSPkg::set_encryption_key);
	ClassDB::bind_method(D_METHOD("finalize"), &DSPkg::finalize);

	ClassDB::bind_static_method("DSPkg", D_METHOD("set_global_encryption_key", "key_hex"), &DSPkg::set_global_encryption_key);
	ClassDB::bind_static_method("DSPkg", D_METHOD("get_global_encryption_key_id"), &DSPkg::get_global_encryption_key_id);
	ClassDB::bind_static_method("DSPkg", D_METHOD("compute_key_id", "key_hex"), &DSPkg::compute_key_id);

	ClassDB::bind_method(D_METHOD("open", "path", "expected_fingerprint"), &DSPkg::open, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_archive_fingerprint"), &DSPkg::get_archive_fingerprint);
	ClassDB::bind_method(D_METHOD("has_texture", "resource_path"), &DSPkg::has_texture);
	ClassDB::bind_method(D_METHOD("read_texture", "resource_path"), &DSPkg::read_texture);
	ClassDB::bind_method(D_METHOD("get_entry_info", "resource_path"), &DSPkg::get_entry_info);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &DSPkg::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_entries"), &DSPkg::get_entries);
	ClassDB::bind_method(D_METHOD("get_mip_entries_for_offset", "offset"), static_cast<Array (DSPkg::*)(uint64_t) const>(&DSPkg::get_mip_entries_for_offset));
}

DSPkg::DSPkg() {
}

DSPkg::~DSPkg() {
	crypto_wipe(m_encryption_key.data(), 32);
}

Error DSPkg::create(const String &p_output_path, uint64_t p_archive_fingerprint) {
	build_output_path = p_output_path;
	build_archive_fingerprint = p_archive_fingerprint;
	payload_alignment = DEFAULT_PAYLOAD_ALIGNMENT;
	bin_packing_enabled = false;
	staging_entries.clear();
	is_building = true;
	m_encryption_enabled = false;
	crypto_wipe(m_encryption_key.data(), 32);
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	m_test_salt_override.clear();
#endif
	return OK;
}

void DSPkg::close_write() {
	is_building = false;
	staging_entries.clear();
}

Error DSPkg::set_encryption_key(const String &p_key_hex) {
	if (!is_building) return ERR_UNCONFIGURED;
	if (p_key_hex.is_empty() || p_key_hex.to_lower() == "disabled" || p_key_hex.to_lower() == "none") {
		m_encryption_enabled = false;
		crypto_wipe(m_encryption_key.data(), 32);
		return OK;
	}
	if (!parse_hex_key_32(p_key_hex, m_encryption_key)) {
		return ERR_INVALID_PARAMETER;
	}
	m_encryption_enabled = true;
	return OK;
}

Error DSPkg::set_global_encryption_key(const String &p_key_hex) {
	std::lock_guard<std::mutex> lock(s_key_mutex);
	{
		std::lock_guard<std::mutex> unauth_lock(s_unauth_log_mutex);
		s_has_unauth_path = false;
		s_last_unauth_path_hash = 0;
	}
	if (p_key_hex.is_empty()) {
		s_key_resolution_state = KEY_STATE_DEFAULT;
		crypto_wipe(s_global_encryption_key_bytes.data(), 32);
		return OK;
	}
	if (p_key_hex.to_lower() == "disabled" || p_key_hex.to_lower() == "none") {
		s_key_resolution_state = KEY_STATE_DISABLED;
		crypto_wipe(s_global_encryption_key_bytes.data(), 32);
		return OK;
	}
	if (!parse_hex_key_32(p_key_hex, s_global_encryption_key_bytes)) {
		return ERR_INVALID_PARAMETER;
	}
	s_key_resolution_state = KEY_STATE_OVERRIDE;
	return OK;
}

String DSPkg::get_global_encryption_key_id() {
	std::lock_guard<std::mutex> lock(s_key_mutex);
	if (s_key_resolution_state == KEY_STATE_DISABLED) return "";
	if (s_key_resolution_state == KEY_STATE_OVERRIDE) {
		uint8_t id_buf[8] = {0};
		crypto_blake2b(id_buf, 8, s_global_encryption_key_bytes.data(), 32);
		return key_id_to_hex(id_buf);
	}
	if (s_baked_encryption_key_hex[0] != '\0') {
		std::array<uint8_t, 32> baked_bytes = {};
		if (parse_hex_key_cstr_32(s_baked_encryption_key_hex, baked_bytes)) {
			uint8_t id_buf[8] = {0};
			crypto_blake2b(id_buf, 8, baked_bytes.data(), 32);
			crypto_wipe(baked_bytes.data(), 32);
			return key_id_to_hex(id_buf);
		} else {
			UtilityFunctions::push_error("DSPkg: Baked encryption key failed hex validation.");
		}
	}
	return "";
}

String DSPkg::compute_key_id(const String &p_key_hex) {
	std::array<uint8_t, 32> key_bytes = {};
	if (!parse_hex_key_32(p_key_hex, key_bytes)) return "";
	uint8_t id_buf[8] = {0};
	crypto_blake2b(id_buf, 8, key_bytes.data(), 32);
	crypto_wipe(key_bytes.data(), 32);
	return key_id_to_hex(id_buf);
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void DSPkg::_set_archive_salt_for_testing(const PackedByteArray &p_salt) {
	m_test_salt_override = p_salt;
}
#endif

void DSPkg::set_archive_fingerprint(uint64_t p_archive_fingerprint) {
	if (is_building) {
		build_archive_fingerprint = p_archive_fingerprint;
	}
}

void DSPkg::set_payload_alignment(uint64_t p_alignment) {
	if (!is_building) {
		UtilityFunctions::push_error("DSPkg: Not in building state. Call create() first.");
		return;
	}
	if (p_alignment < DEFAULT_PAYLOAD_ALIGNMENT || (p_alignment & (p_alignment - 1)) != 0) {
		UtilityFunctions::push_error("DSPkg: Payload alignment must be a power of two and >= 512 bytes.");
		return;
	}
	if (p_alignment > DSPKG_MAX_TOC_SIZE) {
		UtilityFunctions::push_error("DSPkg: Payload alignment cannot exceed maximum TOC size (16 MiB).");
		return;
	}
	payload_alignment = p_alignment;
}

uint64_t DSPkg::get_payload_alignment() const {
	return payload_alignment;
}

void DSPkg::set_bin_packing_enabled(bool p_enabled) {
	if (!is_building) {
		UtilityFunctions::push_error("DSPkg: Not in building state. Call create() first.");
		return;
	}
	bin_packing_enabled = p_enabled;
}

bool DSPkg::is_bin_packing_enabled() const {
	return bin_packing_enabled;
}

Error DSPkg::add_texture(const String &p_resource_path, const String &p_dstex_path, const Dictionary &p_mip_metadata) {
	if (!is_building) {
		UtilityFunctions::push_error("DSPkg: Not in building state. Call create() first.");
		return ERR_UNCONFIGURED;
	}

	Ref<DSTex> dstex;
	dstex.instantiate();
	Dictionary dstex_data = dstex->read_from_file(p_dstex_path);
	if (!dstex_data.has("header") || !dstex_data.has("compressed_data")) {
		UtilityFunctions::push_error("DSPkg: Failed to read .dstex file at ", p_dstex_path);
		return ERR_CANT_OPEN;
	}

	Dictionary entry_item;
	entry_item["resource_path"] = p_resource_path;
	entry_item["dstex_path"] = p_dstex_path;
	entry_item["header"] = dstex_data["header"];
	entry_item["compressed_data"] = dstex_data["compressed_data"];
	Dictionary dstex_header = dstex_data["header"];
	uint32_t mip_count = dstex_header.get("mip_count", 0);
	uint32_t flags = dstex_header.get("flags", 0);
	entry_item["flags"] = flags;
	Dictionary source_metadata = p_mip_metadata.is_empty() ? dstex_data : p_mip_metadata;
	int dstex_version = dstex_header.get("version", 0);
	if (p_mip_metadata.is_empty() && dstex_version == 2) {
		source_metadata["version"] = 2;
	}
	if (metadata_arrays_are_valid(source_metadata, mip_count)) {
		entry_item["gpu_metadata"] = true;
		entry_item["mip_offsets"] = source_metadata.get("mip_offsets", PackedInt64Array());
		entry_item["mip_sizes"] = source_metadata.get("mip_sizes", PackedInt32Array());
		entry_item["mip_uncompressed_sizes"] = source_metadata.get("mip_uncompressed_sizes", PackedInt32Array());
		entry_item["mip_row_pitches"] = source_metadata.get("mip_row_pitches", PackedInt32Array());
	} else {
		entry_item["gpu_metadata"] = false;
		entry_item["mip_sizes"] = dstex_data.get("mip_sizes", PackedInt32Array());
	}

	staging_entries.append(entry_item);
	return OK;
}

Error DSPkg::finalize() {
	if (!is_building) {
		UtilityFunctions::push_error("DSPkg: Not in building state.");
		return ERR_UNCONFIGURED;
	}

	PackedByteArray entries_buf;
	bool finalize_completed = false;

	struct FinalizeCleanupGuard {
		DSPkg *pkg = nullptr;
		PackedByteArray &buf;
		bool &completed;
		~FinalizeCleanupGuard() {
			if (buf.size() > 0) {
				crypto_wipe(buf.ptrw(), buf.size());
			}
			if (pkg && pkg->m_encryption_enabled) {
				crypto_wipe(pkg->m_encryption_key.data(), 32);
				pkg->m_encryption_enabled = false;
			}
			if (pkg && !completed) {
				pkg->close_write();
			}
		}
	} cleanup_guard{this, entries_buf, finalize_completed};

	uint32_t count = static_cast<uint32_t>(staging_entries.size());
	if (count == 0) {
		UtilityFunctions::push_error("DSPkg: No textures added to archive.");
		return ERR_UNCONFIGURED;
	}

	std::vector<Dictionary> sorted_entries;
	sorted_entries.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		sorted_entries.push_back(staging_entries[i]);
	}
	std::sort(sorted_entries.begin(), sorted_entries.end(), [](const Dictionary &a, const Dictionary &b) {
		return String(a.get("resource_path", "")) < String(b.get("resource_path", ""));
	});

	bool write_gpu_metadata = false;
	for (uint32_t i = 0; i < count; ++i) {
		const Dictionary &item = sorted_entries[i];
		if (item.get("gpu_metadata", false)) {
			write_gpu_metadata = true;
			break;
		}
	}

	HashSet<String> seen_staging_paths;
	for (uint32_t i = 0; i < count; ++i) {
		const Dictionary &item = sorted_entries[i];
		String res_path = item["resource_path"];
		if (seen_staging_paths.has(res_path)) {
			UtilityFunctions::push_error("DSPkg: Duplicate entry path encountered during finalization: " + res_path);
			return ERR_ALREADY_EXISTS;
		}
		seen_staging_paths.insert(res_path);
	}

	uint64_t entry_table_size = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const Dictionary &item = sorted_entries[i];
		String res_path = item["resource_path"];
		CharString utf8_path = res_path.utf8();
		if (utf8_path.length() > UINT16_MAX) {
			UtilityFunctions::push_error("DSPkg: Resource path is too long to store in archive.");
			return ERR_INVALID_PARAMETER;
		}
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		uint64_t entry_size = 0;
		Dictionary dstex_header = item["header"];
		uint32_t mip_count = dstex_header.get("mip_count", 0);
		uint64_t metadata_size = 0;
		if (write_gpu_metadata &&
				(!checked_multiply(mip_count, sizeof(DSPkgMipEntry), metadata_size) ||
					!checked_add(DSPKG_FIXED_ENTRY_SIZE_V3, metadata_size, entry_size))) {
			UtilityFunctions::push_error("DSPkg: Mip metadata table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
		if (!write_gpu_metadata) {
			entry_size = DSPKG_FIXED_ENTRY_SIZE_V3;
		}
		if (!checked_add(entry_size, path_len, entry_size) || !checked_add(entry_table_size, entry_size, entry_table_size)) {
			UtilityFunctions::push_error("DSPkg: Entry table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
	}
	if (entry_table_size > DSPKG_MAX_TOC_SIZE) {
		return ERR_INVALID_PARAMETER;
	}

	const uint64_t header_envelope_size = m_encryption_enabled ? (DSPKG_HEADER_SIZE_V4 + DSPKG_ENCRYPTION_MAC_SIZE) : DSPKG_HEADER_SIZE_V4;
	uint64_t table_end_offset = 0;
	if (!checked_add(header_envelope_size, entry_table_size, table_end_offset)) {
		UtilityFunctions::push_error("DSPkg: Archive header and entry table size overflow.");
		return ERR_INVALID_PARAMETER;
	}
	uint64_t total_data_offset = 0;
	if (!align_up(table_end_offset, payload_alignment, total_data_offset)) {
		UtilityFunctions::push_error("DSPkg: Archive data offset alignment overflow.");
		return ERR_INVALID_PARAMETER;
	}
	if (total_data_offset - header_envelope_size > DSPKG_MAX_TOC_SIZE) {
		return ERR_INVALID_PARAMETER;
	}
	if (is_bin_packing_enabled() && table_end_offset > payload_alignment) {
		UtilityFunctions::push_error("DSPkg: TOC envelope (" + String::num_uint64(table_end_offset) + " bytes) exceeds Chunk #0 boundary (" + String::num_uint64(payload_alignment) + " bytes) with bin-packing enabled.");
		return ERR_INVALID_PARAMETER;
	}

	// Serialize TOC into entries_buf via writer
	const uint64_t allocated_toc_size = m_encryption_enabled ? (total_data_offset - header_envelope_size) : entry_table_size;
	entries_buf.resize(allocated_toc_size);
	memset(entries_buf.ptrw(), 0, allocated_toc_size);
	TOCBufferWriter writer(entries_buf.ptrw(), entry_table_size);

	std::vector<uint64_t> entry_payload_offsets;
	entry_payload_offsets.resize(count);

	uint64_t running_data_offset = 0;

	for (uint32_t i = 0; i < count; ++i) {
		const Dictionary &item = sorted_entries[i];
		String res_path = item["resource_path"];
		CharString utf8_path = res_path.utf8();
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		uint64_t hash = res_path.hash();

		Dictionary dstex_header = item["header"];
		PackedByteArray data = item["compressed_data"];

		uint64_t orig_size = dstex_header.get("original_size", 0);
		uint32_t w = dstex_header.get("width", 0);
		uint32_t h = dstex_header.get("height", 0);
		uint32_t fmt = dstex_header.get("pixel_format", 0);
		uint32_t mips_count = dstex_header.get("mip_count", 0);
		PackedInt32Array mip_sizes_entry = item.get("mip_sizes", PackedInt32Array());
		PackedInt32Array mip_uncompressed_sizes = item.get("mip_uncompressed_sizes", PackedInt32Array());
		PackedInt32Array mip_row_pitches = item.get("mip_row_pitches", PackedInt32Array());
		if (write_gpu_metadata && mip_sizes_entry.size() != static_cast<int64_t>(mips_count)) {
			UtilityFunctions::push_error("DSPkg: Texture mip metadata does not match the declared mip count.");
			return ERR_INVALID_DATA;
		}

		uint64_t comp_size = 0;
		if (write_gpu_metadata && item.get("gpu_metadata", false) && mips_count > 1) {
			uint64_t running_mip_span = 0;
			for (uint32_t mip = 0; mip < mips_count; ++mip) {
				if (mip > 0) {
					if (!align_up(running_mip_span, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, running_mip_span)) {
						UtilityFunctions::push_error("DSPkg: Mip span alignment overflow.");
						return ERR_INVALID_PARAMETER;
					}
				}
				if (!checked_add(running_mip_span, static_cast<uint64_t>(mip_sizes_entry[mip]), running_mip_span)) {
					UtilityFunctions::push_error("DSPkg: Mip span overflow.");
					return ERR_INVALID_PARAMETER;
				}
			}
			comp_size = running_mip_span;
		} else {
			comp_size = static_cast<uint64_t>(data.size());
		}

		if (bin_packing_enabled) {
			if (!calculate_packed_payload_offset(running_data_offset, comp_size, payload_alignment, running_data_offset)) {
				UtilityFunctions::push_error("DSPkg: Payload data offset bin-pack alignment overflow.");
				return ERR_INVALID_PARAMETER;
			}
		} else {
			if (!align_up(running_data_offset, payload_alignment, running_data_offset)) {
				UtilityFunctions::push_error("DSPkg: Payload data offset alignment overflow.");
				return ERR_INVALID_PARAMETER;
			}
		}
		entry_payload_offsets[i] = running_data_offset;

		// serialize entry into writer
		writer.put_64(hash);
		writer.put_16(path_len);
		if (path_len > 0) {
			writer.put_buffer(reinterpret_cast<const uint8_t *>(utf8_path.get_data()), path_len);
		}
		writer.put_64(running_data_offset);
		writer.put_64(comp_size);
		writer.put_64(orig_size);
		writer.put_32(w);
		writer.put_32(h);
		writer.put_32(fmt);
		writer.put_32(mips_count);
		uint32_t entry_flags = item.get("flags", 0);
		writer.put_32(entry_flags);

		if (write_gpu_metadata) {
			uint64_t mip_data_offset = 0;
			if (!checked_add(total_data_offset, running_data_offset, mip_data_offset)) {
				UtilityFunctions::push_error("DSPkg: Mip data offset overflow.");
				return ERR_INVALID_PARAMETER;
			}
			uint64_t running_mip_offset = 0;
			for (uint32_t mip = 0; mip < mips_count; ++mip) {
				if (item.get("gpu_metadata", false) && mips_count > 1 && mip > 0) {
					if (!align_up(running_mip_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, running_mip_offset)) {
						UtilityFunctions::push_error("DSPkg: Mip span alignment overflow.");
						return ERR_INVALID_PARAMETER;
					}
				}
				uint64_t mip_size = static_cast<uint64_t>(mip_sizes_entry[mip]);
				if (!checked_add(running_mip_offset, mip_size, running_mip_offset) || running_mip_offset > comp_size) {
					UtilityFunctions::push_error("DSPkg: Invalid compressed mip metadata.");
					return ERR_INVALID_DATA;
				}
			}
			uint64_t mip_offset = 0;
			for (uint32_t mip = 0; mip < mips_count; ++mip) {
				if (item.get("gpu_metadata", false) && mips_count > 1 && mip > 0) {
					align_up(mip_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, mip_offset);
				}
				DSPkgMipEntry mip_entry{};
				uint64_t md_off = 0;
				if (!checked_add(mip_data_offset, mip_offset, md_off)) {
					UtilityFunctions::push_error("DSPkg: Mip data offset overflow.");
					return ERR_INVALID_PARAMETER;
				}
				mip_entry.offset = md_off;
				mip_entry.compressed_size = mip_sizes_entry[mip];
				if (item.get("gpu_metadata", false)) {
					mip_entry.uncompressed_size = mip_uncompressed_sizes[mip];
					mip_entry.row_pitch = mip_row_pitches[mip];
				}
				writer.put_buffer(reinterpret_cast<const uint8_t *>(&mip_entry), sizeof(DSPkgMipEntry));
				mip_offset += mip_entry.compressed_size;
			}
		}

		if (!checked_add(running_data_offset, comp_size, running_data_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload data offset overflow.");
			return ERR_INVALID_PARAMETER;
		}
	}

	if (!writer.is_ok() || writer.get_cursor() != entry_table_size) {
		UtilityFunctions::push_error("DSPkg: TOC buffer serialization overflow or cursor mismatch.");
		return ERR_INVALID_DATA;
	}

	// Step 4: open file
	Ref<FileAccess> file = FileAccess::open(build_output_path, FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open file for writing: ", build_output_path);
		return FileAccess::get_open_error();
	}

	// Step 5: write_zero_padding lambda
	static const uint8_t ZERO_PADDING[4096] = {0};
	auto write_zero_padding = [&](uint64_t p_current, uint64_t p_aligned) -> void {
		uint64_t rem = p_aligned > p_current ? p_aligned - p_current : 0;
		while (rem > 0) {
			uint64_t chunk = std::min<uint64_t>(rem, sizeof(ZERO_PADDING));
			file->store_buffer(ZERO_PADDING, chunk);
			rem -= chunk;
		}
	};

	// Step 6: populate header
	DSPkgHeader header{};
	header.magic = 0x474B5044;
	header.version = DSPKG_VERSION_V4;
	header.flags = write_gpu_metadata ? DSPKG_FLAG_GPU_MIP_METADATA : 0;
	header.entry_count = count;
	header.data_offset = total_data_offset;
	header.archive_fingerprint = build_archive_fingerprint;
	header.compression_codec = 0;
	header.min_reader_version = 4;

	if (m_encryption_enabled) {
		header.flags |= DSPKG_FLAG_ENCRYPTED_TOC;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (m_test_salt_override.size() == 16) {
			memcpy(header.archive_salt, m_test_salt_override.ptr(), 16);
		} else if (m_test_salt_override.size() > 0) {
			file->close();
			DirAccess::remove_absolute(build_output_path);
			return ERR_INVALID_PARAMETER;
		} else
#endif
		{
			PackedByteArray entropy = OS::get_singleton()->get_entropy(16);
			if (entropy.size() != 16) {
				file->close();
				DirAccess::remove_absolute(build_output_path);
				return ERR_CANT_CREATE;
			}
			memcpy(header.archive_salt, entropy.ptr(), 16);
		}
		uint8_t id_buf[8] = {0};
		crypto_blake2b(id_buf, 8, m_encryption_key.data(), 32);
		memcpy(&header.key_id, id_buf, 8);
	}

	// Step 7: write header, MAC, TOC
	if (m_encryption_enabled) {
		const uint64_t ciphertext_len = total_data_offset - header_envelope_size;
		if (ciphertext_len < entry_table_size) {
			file->close();
			DirAccess::remove_absolute(build_output_path);
			return ERR_FILE_CORRUPT;
		}
		uint8_t nonce[DSPKG_ENCRYPTION_NONCE_SIZE] = {0};
		memcpy(nonce, header.archive_salt, 16);
		memcpy(nonce + 16, DOMAIN_SEPARATOR, 8);

		std::array<uint8_t, 32> local_key = m_encryption_key;
		PackedByteArray ciphertext;
		ciphertext.resize(ciphertext_len);
		uint8_t mac[DSPKG_ENCRYPTION_MAC_SIZE] = {0};
		crypto_aead_lock(ciphertext.ptrw(), mac, local_key.data(), nonce,
			reinterpret_cast<const uint8_t *>(&header), sizeof(DSPkgHeader),
			entries_buf.ptr(), ciphertext_len);
		crypto_wipe(local_key.data(), 32);

		file->store_buffer(reinterpret_cast<const uint8_t *>(&header), sizeof(DSPkgHeader));
		file->store_buffer(mac, DSPKG_ENCRYPTION_MAC_SIZE);
		file->store_buffer(ciphertext.ptr(), ciphertext_len);
	} else {
		file->store_buffer(reinterpret_cast<const uint8_t *>(&header), sizeof(DSPkgHeader));
		file->store_buffer(entries_buf.ptr(), entry_table_size);
	}

	// Step 8: single padding site + payloads
	if (!m_encryption_enabled) {
		write_zero_padding(table_end_offset, total_data_offset);
	}

	file->seek(total_data_offset);
	uint64_t payload_offset = total_data_offset;
	for (uint32_t i = 0; i < count; ++i) {
		const Dictionary &item = sorted_entries[i];
		PackedByteArray data = item["compressed_data"];
		uint64_t target_payload_offset = total_data_offset + entry_payload_offsets[i];
		if (target_payload_offset < payload_offset) {
			UtilityFunctions::push_error("DSPkg: Payload offset sequence non-monotonic.");
			file->close();
			return ERR_INVALID_DATA;
		}
		write_zero_padding(payload_offset, target_payload_offset);
		payload_offset = target_payload_offset;

		Dictionary dstex_header = item["header"];
		uint32_t mips = dstex_header.get("mip_count", 0);
		bool has_gpu = write_gpu_metadata && item.get("gpu_metadata", false) && mips > 1;

		if (has_gpu) {
			PackedInt32Array mip_sizes = item.get("mip_sizes", PackedInt32Array());
			int64_t slice_offset = 0;
			for (uint32_t mip = 0; mip < mips; ++mip) {
				int32_t mip_size = mip_sizes[mip];
				if (mip > 0) {
					uint64_t aligned_mip_offset = 0;
					if (!align_up(payload_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, aligned_mip_offset)) {
						UtilityFunctions::push_error("DSPkg: Payload mip file offset alignment overflow.");
						file->close();
						return ERR_INVALID_PARAMETER;
					}
					write_zero_padding(payload_offset, aligned_mip_offset);
					payload_offset = aligned_mip_offset;
				}
				if (mip_size > 0) {
					if (slice_offset < 0 || static_cast<uint64_t>(slice_offset) + mip_size > static_cast<uint64_t>(data.size())) {
						UtilityFunctions::push_error("DSPkg: Mip slice offset exceeds compressed data size.");
						file->close();
						return ERR_INVALID_DATA;
					}
					file->store_buffer(data.ptr() + slice_offset, mip_size);
					slice_offset += mip_size;
				}
				if (!checked_add(payload_offset, static_cast<uint64_t>(mip_size), payload_offset)) {
					UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
					file->close();
					return ERR_INVALID_PARAMETER;
				}
			}
		} else {
			if (data.size() > 0) {
				file->store_buffer(data.ptr(), data.size());
			}
			if (!checked_add(payload_offset, static_cast<uint64_t>(data.size()), payload_offset)) {
				UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
				file->close();
				return ERR_INVALID_PARAMETER;
			}
		}
	}

	file->close();
	finalize_completed = true;
	return OK;
}
Error DSPkg::open(const String &p_path, uint64_t p_expected_fingerprint) {
	entries_by_path.clear();
	entries_by_hash.clear();
	entries_by_offset.clear();
	is_opened = false;
	opened_path = "";
	loaded_header = DSPkgHeader{};

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive: ", p_path);
		return FileAccess::get_open_error();
	}

	PackedByteArray plaintext_toc;

	auto fail = [&](Error p_error, const String &p_message) -> Error {
		if (plaintext_toc.size() > 0) {
			crypto_wipe(plaintext_toc.ptrw(), plaintext_toc.size());
		}
		entries_by_path.clear();
		entries_by_hash.clear();
		entries_by_offset.clear();
		is_opened = false;
		opened_path = "";
		loaded_header = DSPkgHeader{};
		file->close();
		if (p_error == ERR_UNAUTHORIZED) {
			std::lock_guard<std::mutex> lock(s_unauth_log_mutex);
			uint64_t path_hash = p_path.hash();
			if (!s_has_unauth_path || s_last_unauth_path_hash != path_hash) {
				s_has_unauth_path = true;
				s_last_unauth_path_hash = path_hash;
				UtilityFunctions::push_error("DSPkg: ", p_message);
			}
		} else {
			UtilityFunctions::push_error("DSPkg: ", p_message);
		}
		return p_error;
	};

	uint64_t file_len = file->get_length();

	if (file_len < DSPKG_HEADER_SIZE_LEGACY) {
		return fail(ERR_FILE_CORRUPT, "Archive file too small for header.");
	}

	PackedByteArray header_bytes = file->get_buffer(DSPKG_HEADER_SIZE_LEGACY);
	if (header_bytes.size() < DSPKG_HEADER_SIZE_LEGACY) {
		return fail(ERR_FILE_CORRUPT, "Failed to read complete header bytes.");
	}
	memcpy(&loaded_header, header_bytes.ptr(), DSPKG_HEADER_SIZE_LEGACY);

	if (loaded_header.magic != 0x474B5044) {
		return fail(ERR_FILE_UNRECOGNIZED, "Invalid magic number in archive.");
	}

	if (loaded_header.version != 1 && loaded_header.version != 2 && loaded_header.version != 3 && loaded_header.version != 4) {
		return fail(ERR_FILE_UNRECOGNIZED, "Unsupported archive version.");
	}

	if (loaded_header.version == DSPKG_VERSION_V4) {
		if (file_len < DSPKG_HEADER_SIZE_V4) {
			return fail(ERR_FILE_CORRUPT, "Archive file too small for v4 header.");
		}
		constexpr uint64_t v4_extra_size = DSPKG_HEADER_SIZE_V4 - DSPKG_HEADER_SIZE_LEGACY;
		PackedByteArray extra = file->get_buffer(v4_extra_size);
		if (extra.size() < v4_extra_size) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete v4 header bytes.");
		}
		memcpy(reinterpret_cast<uint8_t *>(&loaded_header) + DSPKG_HEADER_SIZE_LEGACY, extra.ptr(), v4_extra_size);
		if (loaded_header.min_reader_version > DSPKG_VERSION_V4) {
			return fail(ERR_FILE_UNRECOGNIZED, "Archive requires newer reader version.");
		}
		if (loaded_header.compression_codec != 0) {
			return fail(ERR_FILE_UNRECOGNIZED, "Unsupported compression codec.");
		}
		if (loaded_header.reserved[0] != 0 || loaded_header.reserved[1] != 0 || loaded_header.reserved[2] != 0 || loaded_header.reserved[3] != 0) {
			return fail(ERR_FILE_CORRUPT, "Reserved header bytes must be zero.");
		}
	}

	const bool is_v3_or_higher = (loaded_header.version >= DSPKG_VERSION_ENTRY_FLAGS);
	const bool has_gpu_metadata = (loaded_header.version == DSPKG_VERSION_GPU_METADATA || is_v3_or_higher) && ((loaded_header.flags & DSPKG_FLAG_GPU_MIP_METADATA) != 0);

	if ((loaded_header.version == DSPKG_VERSION_GPU_METADATA && loaded_header.flags != DSPKG_FLAG_GPU_MIP_METADATA) ||
		(loaded_header.version == DSPKG_VERSION_LEGACY && loaded_header.flags != 0) ||
		(loaded_header.version == DSPKG_VERSION_ENTRY_FLAGS && (loaded_header.flags & ~DSPKG_FLAG_GPU_MIP_METADATA) != 0) ||
		(loaded_header.version == DSPKG_VERSION_V4 && (loaded_header.flags & ~(DSPKG_FLAG_GPU_MIP_METADATA | DSPKG_FLAG_ENCRYPTED_TOC)) != 0)) {
		return fail(ERR_FILE_CORRUPT, "Invalid flags in header.");
	}

	const bool is_encrypted = (loaded_header.version == DSPKG_VERSION_V4) && ((loaded_header.flags & DSPKG_FLAG_ENCRYPTED_TOC) != 0);

	if (!is_encrypted && loaded_header.key_id != 0) {
		return fail(ERR_FILE_CORRUPT, "Unencrypted archive header has non-zero key_id.");
	}
	if (is_encrypted && loaded_header.key_id == 0) {
		return fail(ERR_FILE_CORRUPT, "Encrypted archive header has zero key_id.");
	}

	const uint64_t toc_start = (loaded_header.version != DSPKG_VERSION_V4) ? DSPKG_HEADER_SIZE_LEGACY :
		(is_encrypted ? (DSPKG_HEADER_SIZE_V4 + DSPKG_ENCRYPTION_MAC_SIZE) : DSPKG_HEADER_SIZE_V4);

	if (p_expected_fingerprint != 0 && loaded_header.archive_fingerprint != 0) {
		uint32_t loaded_guid = static_cast<uint32_t>(loaded_header.archive_fingerprint >> 32);
		uint32_t expected_guid = static_cast<uint32_t>(p_expected_fingerprint >> 32);
		uint32_t loaded_hash = static_cast<uint32_t>(loaded_header.archive_fingerprint & 0xFFFFFFFFULL);
		uint32_t expected_hash = static_cast<uint32_t>(p_expected_fingerprint & 0xFFFFFFFFULL);
		if (loaded_guid != expected_guid) {
			return fail(ERR_FILE_UNRECOGNIZED, String("Archive belongs to a different project (archive GUID: 0x") + String::num_uint64(loaded_guid, 16) + ", expected GUID: 0x" + String::num_uint64(expected_guid, 16) + ").");
		}
		if (loaded_hash != expected_hash) {
			return fail(ERR_FILE_UNRECOGNIZED, String("Archive texture manifest does not match current game build (archive hash: 0x") + String::num_uint64(loaded_hash, 16) + ", expected hash: 0x" + String::num_uint64(expected_hash, 16) + ").");
		}
	}

	const uint64_t entry_fields_size = is_v3_or_higher ? DSPKG_ENTRY_FIELDS_SIZE_V3 : DSPKG_ENTRY_FIELDS_SIZE_LEGACY;
	const uint64_t fixed_entry_size = is_v3_or_higher ? DSPKG_FIXED_ENTRY_SIZE_V3 : DSPKG_FIXED_ENTRY_SIZE_LEGACY;

	uint64_t min_table_size = 0;
	uint64_t min_data_offset = 0;
	if (!checked_multiply(loaded_header.entry_count, fixed_entry_size, min_table_size) ||
		!checked_add(toc_start, min_table_size, min_data_offset)) {
		return fail(ERR_FILE_CORRUPT, "Entry table size overflow.");
	}
	if (min_data_offset > loaded_header.data_offset || loaded_header.data_offset > file_len) {
		return fail(ERR_FILE_CORRUPT, "Invalid data_offset in header.");
	}
	if (is_encrypted && loaded_header.data_offset == toc_start) {
		return fail(ERR_FILE_CORRUPT, "Encrypted archive has an empty TOC envelope.");
	}
	const uint64_t toc_len = loaded_header.data_offset - toc_start;
	if (loaded_header.version == DSPKG_VERSION_V4 || is_encrypted) {
		if (toc_len > DSPKG_MAX_TOC_SIZE) {
			return fail(ERR_FILE_CORRUPT, "TOC envelope exceeds maximum size limit (16 MiB).");
		}
	} else {
		constexpr uint64_t MAX_SERIALIZED_ENTRY_SIZE = DSPKG_ENTRY_PREFIX_SIZE + UINT16_MAX + DSPKG_ENTRY_FIELDS_SIZE_V3 + 24ULL * 16; // DSTEX_MAX_MIP_COUNT assumed 16
		uint64_t max_legacy_toc_table = 0;
		if (!checked_multiply(loaded_header.entry_count, MAX_SERIALIZED_ENTRY_SIZE, max_legacy_toc_table)) {
			return fail(ERR_FILE_CORRUPT, "Entry table size overflow.");
		}
		if (toc_len > DSPKG_MAX_TOC_SIZE + max_legacy_toc_table) {
			return fail(ERR_FILE_CORRUPT, "Legacy TOC length exceeds plausible bounds.");
		}
	}

	// Read & decrypt TOC into plaintext_toc
	if (is_encrypted) {
		std::array<uint8_t, 32> key_bytes = {};
		uint64_t active_key_id = 0;
		if (!resolve_encryption_key(key_bytes, active_key_id)) {
			crypto_wipe(key_bytes.data(), 32);
			return fail(ERR_UNAUTHORIZED, "No valid decryption key available.");
		}
		if (loaded_header.key_id != 0 && loaded_header.key_id != active_key_id) {
			crypto_wipe(key_bytes.data(), 32);
			return fail(ERR_UNAUTHORIZED, "Archive encryption key ID mismatch.");
		}
		if (file->get_position() != DSPKG_HEADER_SIZE_V4) {
			file->seek(DSPKG_HEADER_SIZE_V4);
		}
		PackedByteArray mac_bytes = file->get_buffer(DSPKG_ENCRYPTION_MAC_SIZE);
		if (mac_bytes.size() < DSPKG_ENCRYPTION_MAC_SIZE) {
			crypto_wipe(key_bytes.data(), 32);
			return fail(ERR_FILE_CORRUPT, "Failed to read complete MAC bytes.");
		}
		if (file->get_position() != toc_start) {
			file->seek(toc_start);
		}
		PackedByteArray ciphertext = file->get_buffer(toc_len);
		if (ciphertext.size() < toc_len) {
			crypto_wipe(key_bytes.data(), 32);
			return fail(ERR_FILE_CORRUPT, "Failed to read complete TOC ciphertext.");
		}
		uint8_t nonce[DSPKG_ENCRYPTION_NONCE_SIZE] = {0};
		memcpy(nonce, loaded_header.archive_salt, 16);
		memcpy(nonce + 16, DOMAIN_SEPARATOR, 8);

		plaintext_toc.resize(toc_len);
		int unlock_res = crypto_aead_unlock(
			plaintext_toc.ptrw(), mac_bytes.ptr(), key_bytes.data(), nonce,
			reinterpret_cast<const uint8_t *>(&loaded_header), sizeof(DSPkgHeader),
			ciphertext.ptr(), toc_len
		);
		crypto_wipe(key_bytes.data(), 32);
		if (unlock_res != 0) {
			crypto_wipe(plaintext_toc.ptrw(), plaintext_toc.size());
			return fail(ERR_UNAUTHORIZED, "TOC decryption failed or authentication MAC mismatch.");
		}
	} else {
		if (file->get_position() != toc_start) {
			file->seek(toc_start);
		}
		plaintext_toc = file->get_buffer(toc_len);
		if (plaintext_toc.size() < toc_len) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete unencrypted TOC bytes.");
		}
	}

	TOCBufferReader reader(plaintext_toc.ptr(), plaintext_toc.size());

	for (uint32_t i = 0; i < loaded_header.entry_count; ++i) {
		if (!reader.has_remaining(DSPKG_ENTRY_PREFIX_SIZE)) {
			return fail(ERR_FILE_CORRUPT, "Entry header extends past TOC buffer.");
		}

		DSPkgEntry entry;
		entry.path_hash = reader.get_64();
		uint16_t path_len = reader.get_16();

		if (path_len == 0 || !reader.has_remaining(path_len + entry_fields_size)) {
			return fail(ERR_FILE_CORRUPT, "Entry path or fixed fields extend past TOC buffer or path is empty.");
		}

		PackedByteArray path_bytes;
		path_bytes.resize(path_len);
		if (!reader.get_buffer(path_bytes.ptrw(), path_len)) {
			return fail(ERR_FILE_CORRUPT, "Failed to read entry path from TOC buffer.");
		}
		entry.path = String::utf8(reinterpret_cast<const char *>(path_bytes.ptr()), path_len);

		entry.data_offset = reader.get_64();
		entry.compressed_size = reader.get_64();
		entry.original_size = reader.get_64();
		entry.width = reader.get_32();
		entry.height = reader.get_32();
		entry.pixel_format = reader.get_32();
		entry.mip_count = reader.get_32();
		entry.flags = is_v3_or_higher ? reader.get_32() : 0;

		if (entry.width == 0 || entry.height == 0 || entry.mip_count == 0 ||
			entry.mip_count > 16 ||
			!payload_sizes_are_supported(entry)) {
			return fail(ERR_FILE_CORRUPT, "Invalid entry dimensions or mip count.");
		}

		uint64_t abs_payload_offset = 0;
		uint64_t abs_payload_end = 0;
		if (!get_payload_range(loaded_header, entry, abs_payload_offset, abs_payload_end)) {
			return fail(ERR_FILE_CORRUPT, "Entry payload offset arithmetic overflow.");
		}
		if (abs_payload_end > file_len) {
			return fail(ERR_FILE_CORRUPT, "Entry payload extends past archive length.");
		}

		if (has_gpu_metadata) {
			uint64_t metadata_size = 0;
			if (!checked_multiply(entry.mip_count, sizeof(DSPkgMipEntry), metadata_size) ||
				!reader.has_remaining(metadata_size)) {
				return fail(ERR_FILE_CORRUPT, "Entry mip metadata extends past TOC buffer.");
			}
			entry.mip_offsets.resize(entry.mip_count);
			entry.mip_sizes.resize(entry.mip_count);
			entry.mip_uncompressed_sizes.resize(entry.mip_count);
			entry.mip_row_pitches.resize(entry.mip_count);
			uint64_t footprint_size = 0;
			uint64_t compressed_mip_size = 0;
			uint64_t previous_mip_end = abs_payload_offset;
			for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
				uint64_t mip_offset = reader.get_64();
				uint32_t mip_compressed = reader.get_32();
				uint32_t mip_uncompressed = reader.get_32();
				uint32_t mip_row_pitch = reader.get_32();
				uint32_t mip_reserved = reader.get_32();
				entry.mip_offsets[mip] = static_cast<int64_t>(mip_offset);
				entry.mip_sizes[mip] = static_cast<int32_t>(mip_compressed);
				entry.mip_uncompressed_sizes[mip] = static_cast<int32_t>(mip_uncompressed);
				entry.mip_row_pitches[mip] = static_cast<int32_t>(mip_row_pitch);
				uint64_t mip_end = 0;
				if (mip_compressed == 0 || mip_reserved != 0 ||
					!checked_add(mip_offset, mip_compressed, mip_end) ||
					mip_offset < previous_mip_end || mip_end > abs_payload_end) {
					return fail(ERR_FILE_CORRUPT, "Invalid entry mip metadata.");
				}
				if (mip == 0 && mip_offset != abs_payload_offset) {
					return fail(ERR_FILE_CORRUPT, "First mip offset must match entry payload start.");
				}
				if (mip_uncompressed > 0 && mip_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) {
					return fail(ERR_FILE_CORRUPT, "Invalid mip row pitch.");
				}
				previous_mip_end = mip_end;
			}
			entry.has_gpu_metadata = true;
			Vector<DSPkgMipEntry> mip_vector;
			mip_vector.resize(entry.mip_count);
			for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
				DSPkgMipEntry &m_e = mip_vector.write[mip];
				m_e.offset = static_cast<uint64_t>(entry.mip_offsets[mip]);
				m_e.compressed_size = static_cast<uint32_t>(entry.mip_sizes[mip]);
				m_e.uncompressed_size = static_cast<uint32_t>(entry.mip_uncompressed_sizes[mip]);
				m_e.row_pitch = static_cast<uint32_t>(entry.mip_row_pitches[mip]);
				m_e.reserved = 0;
			}
			if (entries_by_offset.has(abs_payload_offset)) {
				return fail(ERR_FILE_CORRUPT, "Duplicate entry payload offset in archive.");
			}
			entries_by_offset[abs_payload_offset] = mip_vector;
		}

		if (entries_by_path.has(entry.path)) {
			return fail(ERR_FILE_CORRUPT, "Duplicate entry path in archive: " + entry.path);
		}
		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash].push_back(entry);
		if (has_gpu_metadata) {
			// store mip entries by offset handled above; skip for brevity
		}
	}

	if (is_encrypted && plaintext_toc.size() > 0) {
		crypto_wipe(plaintext_toc.ptrw(), plaintext_toc.size());
	}
	{
		std::lock_guard<std::mutex> unauth_lock(s_unauth_log_mutex);
		if (s_has_unauth_path && s_last_unauth_path_hash == p_path.hash()) {
			s_has_unauth_path = false;
			s_last_unauth_path_hash = 0;
		}
	}

	is_opened = true;
	opened_path = p_path;
	return OK;
}
bool DSPkg::has_texture(const String &p_resource_path) const {
	return is_opened && find_entry_for_path(p_resource_path) != nullptr;
}

Dictionary DSPkg::read_texture(const String &p_resource_path) const {
	Dictionary result;
	if (!is_opened) {
		UtilityFunctions::push_error("DSPkg: Archive is not opened.");
		return result;
	}

	const DSPkgEntry *entry_ptr = find_entry_for_path(p_resource_path);
	if (entry_ptr == nullptr) {
		UtilityFunctions::push_error("DSPkg: Texture path not found in archive: ", p_resource_path);
		return result;
	}
	const DSPkgEntry &entry = *entry_ptr;
	if (!payload_sizes_are_supported(entry)) {
		UtilityFunctions::push_error("DSPkg: Entry payload size is no longer supported.");
		return result;
	}

	Ref<FileAccess> file = FileAccess::open(opened_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive file for reading entry data.");
		return result;
	}

	uint64_t file_len = file->get_length();
	uint64_t target_offset = 0;
	uint64_t payload_end = 0;
	if (!get_payload_range(loaded_header, entry, target_offset, payload_end)) {
		UtilityFunctions::push_error("DSPkg: Integer overflow calculating entry payload range.");
		file->close();
		return result;
	}

	if (target_offset > file_len || payload_end > file_len) {
		UtilityFunctions::push_error("DSPkg: Entry payload extends past end of file at read time.");
		file->close();
		return result;
	}

	file->seek(target_offset);
	PackedByteArray compressed_data = file->get_buffer(static_cast<int64_t>(entry.compressed_size));
	file->close();

	if (static_cast<uint64_t>(compressed_data.size()) != entry.compressed_size) {
		UtilityFunctions::push_error("DSPkg: Failed to read complete compressed data for entry: ", entry.path);
		return Dictionary();
	}

	result["path"] = entry.path;
	result["compressed_data"] = compressed_data;
	result["width"] = entry.width;
	result["height"] = entry.height;
	result["pixel_format"] = entry.pixel_format;
	result["mip_count"] = entry.mip_count;
	result["original_size"] = entry.original_size;
	result["compressed_size"] = entry.compressed_size;
	result["has_gpu_metadata"] = entry.has_gpu_metadata;
	result["mip_offsets"] = entry.mip_offsets;
	result["mip_sizes"] = entry.mip_sizes;
	result["mip_uncompressed_sizes"] = entry.mip_uncompressed_sizes;
	result["mip_row_pitches"] = entry.mip_row_pitches;
	if (entry.has_gpu_metadata) {
		Array mip_data;
		for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
			uint64_t rel_offset = static_cast<uint64_t>(entry.mip_offsets[mip]) - target_offset;
			uint64_t mip_size = static_cast<uint64_t>(entry.mip_sizes[mip]);
			mip_data.push_back(compressed_data.slice(static_cast<int64_t>(rel_offset), static_cast<int64_t>(rel_offset + mip_size)));
		}
		result["mip_data"] = mip_data;
	}

	return result;
}

Dictionary DSPkg::get_entry_info(const String &p_resource_path) const {
	Dictionary result;
	if (!is_opened) {
		UtilityFunctions::push_error("DSPkg: Archive is not opened.");
		return result;
	}

	const DSPkgEntry *entry_ptr = find_entry_for_path(p_resource_path);
	if (entry_ptr == nullptr) {
		UtilityFunctions::push_error("DSPkg: Texture path not found in archive: ", p_resource_path);
		return result;
	}
	const DSPkgEntry &entry = *entry_ptr;
	if (!payload_sizes_are_supported(entry)) {
		UtilityFunctions::push_error("DSPkg: Entry payload size is no longer supported.");
		return result;
	}

	uint64_t data_offset = 0;
	uint64_t payload_end = 0;
	if (!get_payload_range(loaded_header, entry, data_offset, payload_end)) {
		UtilityFunctions::push_error("DSPkg: Integer overflow calculating entry payload range.");
		return result;
	}

	result["archive_path"] = opened_path;
	result["path"] = entry.path;
	result["data_offset"] = data_offset;
	result["compressed_size"] = entry.compressed_size;
	result["original_size"] = entry.original_size;
	result["width"] = entry.width;
	result["height"] = entry.height;
	result["pixel_format"] = entry.pixel_format;
	result["mip_count"] = entry.mip_count;
	result["flags"] = entry.flags;
	result["has_gpu_metadata"] = entry.has_gpu_metadata;
	result["is_srgb"] = ((entry.flags & 1U) != 0) || DSTex::is_rd_format_srgb(entry.pixel_format);
	result["as_image_texture"] = (entry.flags & 2U) != 0;
	result["mip_offsets"] = entry.mip_offsets;
	result["mip_sizes"] = entry.mip_sizes;
	result["mip_uncompressed_sizes"] = entry.mip_uncompressed_sizes;
	result["mip_row_pitches"] = entry.mip_row_pitches;

	return result;
}

const DSPkgEntry *DSPkg::find_entry_for_path(const String &p_resource_path) const {
	const DSPkgEntry *entry = entries_by_path.getptr(p_resource_path);
	if (entry != nullptr) {
		return entry;
	}

	const Vector<DSPkgEntry> *hash_entries = entries_by_hash.getptr(p_resource_path.hash());
	if (hash_entries == nullptr) {
		return nullptr;
	}
	for (int i = 0; i < hash_entries->size(); ++i) {
		if ((*hash_entries)[i].path == p_resource_path) {
			return &(*hash_entries)[i];
		}
	}
	return nullptr;
}

bool DSPkg::get_mip_entries_for_path(const String &p_path, Vector<DSPkgMipEntry> &r_entries) const {
	r_entries.clear();
	if (!is_opened) {
		return false;
	}

	const DSPkgEntry *entry = find_entry_for_path(p_path);
	if (entry == nullptr || !entry->has_gpu_metadata ||
			entry->mip_offsets.size() != static_cast<int64_t>(entry->mip_count) ||
			entry->mip_sizes.size() != static_cast<int64_t>(entry->mip_count) ||
			entry->mip_uncompressed_sizes.size() != static_cast<int64_t>(entry->mip_count) ||
			entry->mip_row_pitches.size() != static_cast<int64_t>(entry->mip_count)) {
		return false;
	}

	r_entries.resize(entry->mip_count);
	for (uint32_t mip = 0; mip < entry->mip_count; ++mip) {
		DSPkgMipEntry &mip_entry = r_entries.write[mip];
		mip_entry.offset = static_cast<uint64_t>(entry->mip_offsets[mip]);
		mip_entry.compressed_size = static_cast<uint32_t>(entry->mip_sizes[mip]);
		mip_entry.uncompressed_size = static_cast<uint32_t>(entry->mip_uncompressed_sizes[mip]);
		mip_entry.row_pitch = static_cast<uint32_t>(entry->mip_row_pitches[mip]);
		mip_entry.reserved = 0;
	}
	return true;
}

bool DSPkg::get_mip_entries_for_offset(uint64_t p_offset, Vector<DSPkgMipEntry> &r_entries) const {
	r_entries.clear();
	if (!is_opened) {
		return false;
	}
	const Vector<DSPkgMipEntry> *found = entries_by_offset.getptr(p_offset);
	if (found == nullptr) {
		return false;
	}
	r_entries = *found;
	return true;
}

Array DSPkg::get_mip_entries_for_offset(uint64_t p_offset) const {
	Array result;
	Vector<DSPkgMipEntry> entries;
	if (!get_mip_entries_for_offset(p_offset, entries)) {
		return result;
	}
	for (int i = 0; i < entries.size(); ++i) {
		const DSPkgMipEntry &mip_entry = entries[i];
		Dictionary mip_dict;
		mip_dict["offset"] = static_cast<int64_t>(mip_entry.offset);
		mip_dict["compressed_size"] = static_cast<int64_t>(mip_entry.compressed_size);
		mip_dict["uncompressed_size"] = static_cast<int64_t>(mip_entry.uncompressed_size);
		mip_dict["row_pitch"] = static_cast<int64_t>(mip_entry.row_pitch);
		result.append(mip_dict);
	}
	return result;
}

int DSPkg::get_entry_count() const {
	if (!is_opened) {
		return 0;
	}
	return static_cast<int>(loaded_header.entry_count);
}

uint64_t DSPkg::get_archive_fingerprint() const {
	return loaded_header.archive_fingerprint;
}

Array DSPkg::get_entries() const {
	Array result;
	if (!is_opened) {
		return result;
	}
	for (const KeyValue<String, DSPkgEntry> &E : entries_by_path) {
		result.append(E.key);
	}
	result.sort();
	return result;
}

} // namespace godot
