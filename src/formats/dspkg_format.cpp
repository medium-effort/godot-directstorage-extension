#include "dspkg_format.h"
#include "dstex_format.h"
#include "gen/dspkg_encryption_key.h"
#include "monocypher.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

namespace godot {

extern const char *const dspkg_baked_key_id_tag = DSPKG_KEY_ID_TAG;

namespace {

constexpr uint64_t DSPKG_MAX_PAYLOAD_SIZE = static_cast<uint64_t>(INT32_MAX);
// FMT-01: per-entry sane cap matching DSTEX_MAX_TOTAL_BYTES (512 MiB) so a
// hostile archive entry cannot force a huge transient allocation.
constexpr uint64_t DSPKG_MAX_ENTRY_BYTES = 512ULL * 1024ULL * 1024ULL;
constexpr uint32_t D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256;
constexpr uint32_t D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512;

static const uint8_t DOMAIN_SEPARATOR[8] = {'D', 'S', 'P', 'K', 'G', 'v', '4', 0x01};
// v5 uses a distinct separator so a v4 and a v5 ciphertext are never
// interchangeable under the same key/salt.
static const uint8_t DOMAIN_SEPARATOR_V5[8] = {'D', 'S', 'P', 'K', 'G', 'v', '5', 0x01};

// Upper bound for one v5 spill entry (10 prefix + 65535 path + 56 fixed fields
// + 64 KiB metadata + 64 KiB entry TLV + 16 mip entries + 16*4096 chunk
// entries = 1,245,633 bytes). Rounded up to 2 MiB so the reader can reject an
// implausible TOC before allocating or decrypting it.
constexpr uint64_t DSPKG_MAX_SERIALIZED_ENTRY_SIZE_V5 = 2ULL * 1024ULL * 1024ULL;

// Archive-key delivery contract: the exporter injects this PCK entry as
// magic16 || key32. The magic is derived from a public label instead of being
// stored, so no shipped file carries the byte sequence the pack scan searches
// for.
constexpr const char *DSPKG_TOC_KEY_PATH = "res://.dspkg/toc.key";
constexpr size_t DSPKG_TOC_KEY_MAGIC_SIZE = 16;
constexpr int64_t DSPKG_TOC_KEY_FILE_SIZE = 48;
constexpr const char *DSPKG_TOC_KEY_MAGIC_LABEL = "dspkg-toc-key-magic-v1";

static_assert(DSPKG_TOC_KEY_FILE_SIZE == static_cast<int64_t>(DSPKG_TOC_KEY_MAGIC_SIZE) + 32,
		"Key file layout is magic16 || key32");

// Derived once per process; the static initialisation is thread-safe and keeps
// the derivation out of the open() critical path.
static const std::array<uint8_t, DSPKG_TOC_KEY_MAGIC_SIZE> &get_toc_key_magic() {
	static const std::array<uint8_t, DSPKG_TOC_KEY_MAGIC_SIZE> magic = []() {
		std::array<uint8_t, DSPKG_TOC_KEY_MAGIC_SIZE> derived = {};
		const PackedByteArray digest = String(DSPKG_TOC_KEY_MAGIC_LABEL).sha256_buffer();
		if (digest.size() >= static_cast<int64_t>(DSPKG_TOC_KEY_MAGIC_SIZE)) {
			memcpy(derived.data(), digest.ptr(), DSPKG_TOC_KEY_MAGIC_SIZE);
		}
		return derived;
	}();
	return magic;
}

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

#if DSPKG_ENCRYPTION_ENABLED
static void unmask_baked_key(std::array<uint8_t, 32> &r_key_bytes) {
	volatile const unsigned char *p_mask = dspkg_key::MASK;
	volatile const unsigned char *p_masked = dspkg_key::MASKED_KEY;
	for (size_t i = 0; i < 32; ++i) {
		r_key_bytes[i] = p_mask[i] ^ p_masked[i];
	}
}
#endif

static void compute_key_id_from_bytes(const uint8_t *p_key_bytes, uint64_t &r_key_id) {
	uint8_t id_buf[8] = {0};
	crypto_blake2b(id_buf, 8, p_key_bytes, 32);
	memcpy(&r_key_id, id_buf, 8);
}

static void wipe_key_bytes(std::array<uint8_t, 32> &r_key_bytes) {
	crypto_wipe(r_key_bytes.data(), r_key_bytes.size());
}

// Acquires a decryption-key candidate without installing it. Installation is the
// caller's job and happens only after the candidate reproduces the archive
// header's key_id and decrypts the TOC, so a stale or undecryptable key file
// cannot pin the process to a key that a later PCK mount would have replaced.
static bool resolve_encryption_key(std::array<uint8_t, 32> &r_key_bytes, uint64_t &r_key_id) {
	KeyResolutionState state = KEY_STATE_DEFAULT;
	{
		std::lock_guard<std::mutex> lock(s_key_mutex);
		state = s_key_resolution_state;
		if (state == KEY_STATE_OVERRIDE) {
			r_key_bytes = s_global_encryption_key_bytes;
		}
	}
	if (state == KEY_STATE_DISABLED) {
		wipe_key_bytes(r_key_bytes);
		return false;
	}
	// An explicit override is already validated by the public setter.
	if (state == KEY_STATE_OVERRIDE) {
		compute_key_id_from_bytes(r_key_bytes.data(), r_key_id);
		return true;
	}

	// KEY_STATE_DEFAULT: the archive key travels inside the encrypted PCK. The
	// read is kernel I/O, so it stays outside s_key_mutex. A missing file leaves
	// the resolution state untouched: the next open() after a PCK mount retries.
	PackedByteArray key_file;
	if (FileAccess::file_exists(DSPKG_TOC_KEY_PATH)) {
		key_file = FileAccess::get_file_as_bytes(DSPKG_TOC_KEY_PATH);
	}
	const std::array<uint8_t, DSPKG_TOC_KEY_MAGIC_SIZE> &magic = get_toc_key_magic();
	const bool key_file_is_valid = key_file.size() == DSPKG_TOC_KEY_FILE_SIZE &&
			memcmp(key_file.ptr(), magic.data(), DSPKG_TOC_KEY_MAGIC_SIZE) == 0;
	if (key_file_is_valid) {
		memcpy(r_key_bytes.data(), key_file.ptr() + DSPKG_TOC_KEY_MAGIC_SIZE, 32);
	}
	if (key_file.size() > 0) {
		crypto_wipe(key_file.ptrw(), static_cast<size_t>(key_file.size()));
	}
	key_file = PackedByteArray();
	if (key_file_is_valid) {
		compute_key_id_from_bytes(r_key_bytes.data(), r_key_id);
		return true;
	}

#if DSPKG_ENCRYPTION_ENABLED
	// A source build that bakes a key has no PCK delivery, so exactly one key
	// source exists per configuration rather than a fallback chain.
	unmask_baked_key(r_key_bytes);
	compute_key_id_from_bytes(r_key_bytes.data(), r_key_id);
	return true;
#else
	wipe_key_bytes(r_key_bytes);
	return false;
#endif
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
	const uint8_t *get_data() const { return buffer; }
	bool is_ok() const { return !has_error; }
	uint64_t get_64() { if (!has_remaining(8)) { has_error = true; return 0; } uint64_t val; memcpy(&val, buffer + cursor, 8); cursor += 8; return val; }
	uint32_t get_32() { if (!has_remaining(4)) { has_error = true; return 0; } uint32_t val; memcpy(&val, buffer + cursor, 4); cursor += 4; return val; }
	uint16_t get_16() { if (!has_remaining(2)) { has_error = true; return 0; } uint16_t val; memcpy(&val, buffer + cursor, 2); cursor += 2; return val; }
	uint8_t get_8() { if (!has_remaining(1)) { has_error = true; return 0; } uint8_t val; memcpy(&val, buffer + cursor, 1); cursor += 1; return val; }
	bool get_buffer(uint8_t *dst, uint64_t bytes) { if (!has_remaining(bytes)) { has_error = true; return false; } memcpy(dst, buffer + cursor, bytes); cursor += bytes; return true; }
	bool skip(uint64_t bytes) { if (!has_remaining(bytes)) { has_error = true; return false; } cursor += bytes; return true; }
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

// v5 entry-extension sizing for staged NPOT textures (Phase 3 item 2). A 0
// staged dimension means storage; the TLV record is emitted only when the
// logical size actually differs, keeping aligned-source archives v4.
bool staged_entry_needs_logical_tlv(const DSPkgStagedEntry &p_entry) {
	const uint32_t lw = p_entry.logical_width == 0 ? p_entry.width : p_entry.logical_width;
	const uint32_t lh = p_entry.logical_height == 0 ? p_entry.height : p_entry.logical_height;
	return lw != p_entry.width || lh != p_entry.height;
}

uint64_t entry_logical_tlv_size(const DSPkgStagedEntry &p_entry) {
	return staged_entry_needs_logical_tlv(p_entry) ? (DS_TLV_HEADER_SIZE + sizeof(DSPkgTextureLogical)) : 0;
}

struct DSPkgTlvSummary {
	bool has_crypto = false;
	bool has_provenance = false;
	bool has_archive_set = false;
	bool has_delta = false;
};

// Validates one TLV table (header extension or entry extensions). The encoding
// is tag:u32 || size:u32 || payload[size] padded to 8 bytes. Tables must be
// fully consumed: no trailing partial record and no extra zero bytes beyond a
// record's own alignment padding.
bool validate_tlv_table(const uint8_t *p_data, uint64_t p_size, uint64_t p_max_total,
		const char *p_context, DSPkgTlvSummary &r_summary, String &r_error) {
	if (p_size == 0) {
		return true;
	}
	if (p_size > p_max_total) {
		r_error = String(p_context) + " TLV table exceeds its size limit.";
		return false;
	}
	std::vector<uint32_t> seen_tags;
	uint64_t cursor = 0;
	while (cursor < p_size) {
		if (p_size - cursor < DS_TLV_HEADER_SIZE) {
			r_error = String(p_context) + " TLV header extends past the table.";
			return false;
		}
		uint32_t tag = 0;
		uint32_t payload_size = 0;
		memcpy(&tag, p_data + cursor, 4);
		memcpy(&payload_size, p_data + cursor + 4, 4);
		if (tag == 0) {
			r_error = String(p_context) + " TLV tag 0 is invalid.";
			return false;
		}
		if (payload_size > DS_TLV_MAX_PAYLOAD) {
			r_error = String(p_context) + " TLV payload exceeds the per-record limit.";
			return false;
		}
		uint64_t record_size = 0;
		uint64_t padded_size = 0;
		if (!checked_add(DS_TLV_HEADER_SIZE, payload_size, record_size) ||
				!align_up(record_size, 8, padded_size)) {
			r_error = String(p_context) + " TLV record size overflow.";
			return false;
		}
		if (record_size > p_size - cursor || padded_size > p_size - cursor) {
			r_error = String(p_context) + " TLV record extends past the table.";
			return false;
		}
		for (uint64_t pad = record_size; pad < padded_size; ++pad) {
			if (p_data[cursor + pad] != 0) {
				r_error = String(p_context) + " TLV padding bytes must be zero.";
				return false;
			}
		}
		if (std::find(seen_tags.begin(), seen_tags.end(), tag) != seen_tags.end()) {
			r_error = String(p_context) + " TLV contains a duplicate tag.";
			return false;
		}
		seen_tags.push_back(tag);
		switch (tag) {
			case DS_TLV_TAG_CRYPTO:
				r_summary.has_crypto = true;
				break;
			case DS_TLV_TAG_PROVENANCE:
				r_summary.has_provenance = true;
				break;
			case DS_TLV_TAG_ARCHIVE_SET:
				r_summary.has_archive_set = true;
				break;
			case DS_TLV_TAG_DELTA:
				r_summary.has_delta = true;
				break;
			default:
				// Unknown tags are skipped, but their bounds and padding were
				// validated above.
				break;
		}
		cursor += padded_size;
	}
	if (cursor != p_size) {
		r_error = String(p_context) + " TLV table was not consumed exactly.";
		return false;
	}
	return true;
}

// Scans an already-validated entry TLV table for DS_TLV_TAG_TEXTURE_LOGICAL.
// Sets r_found and the values when the record is present. Returns false only
// on a malformed logical record (wrong payload size); an absent record is
// valid and reports r_found = false. Bounds and padding are trusted here —
// the table must have passed validate_tlv_table first.
bool parse_entry_logical_tlv(const uint8_t *p_data, uint64_t p_size, bool &r_found, uint32_t &r_logical_width, uint32_t &r_logical_height) {
	r_found = false;
	uint64_t cursor = 0;
	while (cursor < p_size) {
		uint32_t tag = 0;
		uint32_t payload_size = 0;
		memcpy(&tag, p_data + cursor, 4);
		memcpy(&payload_size, p_data + cursor + 4, 4);
		uint64_t padded_size = 0;
		if (!checked_add(DS_TLV_HEADER_SIZE, payload_size, padded_size) ||
				!align_up(padded_size, 8, padded_size) ||
				padded_size > p_size - cursor) {
			return false;
		}
		if (tag == DS_TLV_TAG_TEXTURE_LOGICAL) {
			if (payload_size != sizeof(DSPkgTextureLogical)) {
				return false;
			}
			DSPkgTextureLogical logical{};
			memcpy(&logical, p_data + cursor + DS_TLV_HEADER_SIZE, sizeof(DSPkgTextureLogical));
			r_logical_width = logical.logical_width;
			r_logical_height = logical.logical_height;
			r_found = true;
			return true;
		}
		cursor += padded_size;
	}
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
	// v3 GDeflate-inline payloads share the v2 24-byte mip-entry layout and
	// 512-byte placement, so the same footprint arrays describe them. v3
	// chunked/Zstd/GACL payloads are rejected by add_texture(), never here.
	if (metadata_version != 2 && metadata_version != 3) {
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

// Compares a staged entry's add-time snapshot with the source as it is now. A
// texture that was modified, resized or replaced between add_texture() and
// finalize() must abort the archive instead of streaming bytes nobody staged.
bool staged_source_is_unchanged(const DSPkgStagedEntry &p_entry, const Dictionary &p_header,
		uint64_t p_file_length, uint64_t p_modified_time, String &r_reason) {
	if (p_modified_time != p_entry.source_modified_time) {
		r_reason = "modification time changed";
		return false;
	}
	if (p_file_length != p_entry.source_file_length) {
		r_reason = "file length changed from " + String::num_uint64(p_entry.source_file_length) + " to " +
				String::num_uint64(p_file_length) + " bytes";
		return false;
	}
	uint16_t version = p_header.get("version", 0);
	if (version != p_entry.source_version) {
		r_reason = "container version changed";
		return false;
	}
	uint64_t compressed_size = p_header.get("compressed_size", 0);
	if (compressed_size != p_entry.source_compressed_size) {
		r_reason = "compressed size changed from " + String::num_uint64(p_entry.source_compressed_size) + " to " +
				String::num_uint64(compressed_size) + " bytes";
		return false;
	}
	uint32_t mip_count = p_header.get("mip_count", 0);
	if (mip_count != p_entry.mip_count) {
		r_reason = "mip count changed from " + String::num_int64(p_entry.mip_count) + " to " +
				String::num_int64(mip_count);
		return false;
	}
	PackedInt32Array mip_sizes = p_header.get("mip_sizes", PackedInt32Array());
	if (!(mip_sizes == p_entry.source_mip_sizes)) {
		r_reason = "mip size table changed";
		return false;
	}
	PackedInt64Array mip_offsets = p_header.get("mip_offsets", PackedInt64Array());
	if (!(mip_offsets == p_entry.source_mip_offsets)) {
		r_reason = "mip offset table changed";
		return false;
	}
	uint32_t logical_width = p_header.get("logical_width", 0);
	uint32_t logical_height = p_header.get("logical_height", 0);
	if (logical_width != p_entry.logical_width || logical_height != p_entry.logical_height) {
		r_reason = "logical dimensions changed";
		return false;
	}
	if (p_entry.gpu_metadata) {
		PackedInt32Array uncompressed_sizes = p_header.get("mip_uncompressed_sizes", PackedInt32Array());
		if (!(uncompressed_sizes == p_entry.mip_uncompressed_sizes)) {
			r_reason = "GPU footprint metadata changed";
			return false;
		}
		uint64_t footprint = 0;
		for (int64_t mip = 0; mip < uncompressed_sizes.size(); ++mip) {
			if (!checked_add(footprint, static_cast<uint32_t>(uncompressed_sizes[mip]), footprint)) {
				r_reason = "GPU footprint size overflow";
				return false;
			}
		}
		if (footprint != p_entry.gpu_footprint) {
			r_reason = "GPU footprint size changed";
			return false;
		}
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

void DSPkg::set_global_encryption_key_raw(const uint8_t *p_key_bytes, size_t p_size) {
	if (p_key_bytes == nullptr || p_size < 32) {
		return;
	}
	std::lock_guard<std::mutex> lock(s_key_mutex);
	memcpy(s_global_encryption_key_bytes.data(), p_key_bytes, 32);
	// The id is derived from these bytes by the same code path the explicit
	// override uses, so no separate id copy is cached here.
	s_key_resolution_state = KEY_STATE_OVERRIDE;
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
	// Volatile pointer qualification and dereference guarantees an observable load under MSVC /O2,
	// preventing /OPT:REF from dead-stripping the anchor tag in release builds for both keyed
	// and keyless ("dspkg-key-id=none") binaries (R7-04, R8-11, R8-15).
	const char *volatile p_tag = dspkg_baked_key_id_tag;
	(void)*p_tag;

	if (s_key_resolution_state == KEY_STATE_DISABLED) return "";
	if (s_key_resolution_state == KEY_STATE_OVERRIDE) {
		uint8_t id_buf[8] = {0};
		crypto_blake2b(id_buf, 8, s_global_encryption_key_bytes.data(), 32);
		return key_id_to_hex(id_buf);
	}
#if DSPKG_ENCRYPTION_ENABLED
	std::array<uint8_t, 32> baked_bytes = {};
	unmask_baked_key(baked_bytes);
	uint8_t id_buf[8] = {0};
	crypto_blake2b(id_buf, 8, baked_bytes.data(), 32);
	crypto_wipe(baked_bytes.data(), 32);
	return key_id_to_hex(id_buf);
#else
	return "";
#endif
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

void DSPkg::_set_write_truncate_for_testing(int64_t p_bytes) {
	m_test_write_truncate_after_bytes = p_bytes;
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

	// Metadata only: stat the source and parse its header and mip table. The
	// payload stays on disk; finalize() re-stats the file and streams it, so a
	// mutated import aborts instead of streaming unexpected bytes.
	Ref<FileAccess> source = FileAccess::open(p_dstex_path, FileAccess::READ);
	if (source.is_null() || !source->is_open()) {
		UtilityFunctions::push_error("DSPkg: Failed to open .dstex file at ", p_dstex_path);
		return ERR_CANT_OPEN;
	}
	DSPkgStagedEntry entry;
	entry.source_file_length = source->get_length();
	entry.source_modified_time = FileAccess::get_modified_time(p_dstex_path);
	source->close();

	Ref<DSTex> dstex;
	dstex.instantiate();
	Dictionary dstex_header = dstex->read_header(p_dstex_path);
	if (dstex_header.is_empty()) {
		UtilityFunctions::push_error("DSPkg: Failed to read .dstex header at ", p_dstex_path);
		return ERR_CANT_OPEN;
	}

	int dstex_version = dstex_header.get("version", 0);
	uint32_t mip_count = dstex_header.get("mip_count", 0);
	PackedInt64Array source_mip_offsets = dstex_header.get("mip_offsets", PackedInt64Array());
	PackedInt32Array source_mip_sizes = dstex_header.get("mip_sizes", PackedInt32Array());
	if (source_mip_offsets.size() != static_cast<int64_t>(mip_count) ||
			source_mip_sizes.size() != static_cast<int64_t>(mip_count)) {
		UtilityFunctions::push_error("DSPkg: .dstex mip table does not match its declared mip count: ", p_dstex_path);
		return ERR_INVALID_DATA;
	}

	Dictionary source_metadata = p_mip_metadata.is_empty() ? dstex_header : p_mip_metadata;
	if (p_mip_metadata.is_empty() && (dstex_version == 2 || dstex_version == 3)) {
		source_metadata["version"] = dstex_version;
	}

	// v4 archives store inline GDeflate mip payloads only. A v3 source with a
	// Zstd/GACL/chunked payload has no v4 representation: fail closed here
	// instead of writing an entry the readers must reject.
	if (dstex_version == 3) {
		const int src_compression = dstex_header.get("compression_format", 1);
		const int src_gacl = dstex_header.get("gacl_transform", 0);
		const bool src_chunked = dstex_header.get("chunked", false);
		if (src_compression != static_cast<int>(DSCompressionFormat::GDEFLATE) ||
				src_gacl != static_cast<int>(DSGaclTransform::NONE) || src_chunked) {
			UtilityFunctions::push_error("DSPkg: v3 .dstex uses Zstd/GACL/chunked payloads that v4 archives cannot represent: ", p_dstex_path);
			return ERR_UNAVAILABLE;
		}
		PackedInt32Array src_chunk_counts = dstex_header.get("mip_chunk_counts", PackedInt32Array());
		for (int64_t ci = 0; ci < src_chunk_counts.size(); ++ci) {
			if (src_chunk_counts[ci] != 1) {
				UtilityFunctions::push_error("DSPkg: v3 .dstex has chunked mips that v4 archives cannot represent: ", p_dstex_path);
				return ERR_UNAVAILABLE;
			}
		}
	}

	entry.gpu_metadata = metadata_arrays_are_valid(source_metadata, mip_count);
	uint64_t gpu_footprint = 0;
	if (entry.gpu_metadata) {
		entry.mip_sizes = source_metadata.get("mip_sizes", PackedInt32Array());
		entry.mip_uncompressed_sizes = source_metadata.get("mip_uncompressed_sizes", PackedInt32Array());
		entry.mip_row_pitches = source_metadata.get("mip_row_pitches", PackedInt32Array());
		// The payload is streamed mip by mip straight from the source table, so
		// the metadata that describes the TOC must match those slices exactly.
		if (!(entry.mip_sizes == source_mip_sizes)) {
			UtilityFunctions::push_error("DSPkg: Supplied mip metadata does not match the .dstex mip table: ", p_dstex_path);
			return ERR_INVALID_DATA;
		}
		for (int64_t mip = 0; mip < entry.mip_uncompressed_sizes.size(); ++mip) {
			uint64_t footprint_size = static_cast<uint32_t>(entry.mip_uncompressed_sizes[mip]);
			if (!checked_add(gpu_footprint, footprint_size, gpu_footprint)) {
				UtilityFunctions::push_error("DSPkg: GPU footprint size overflow: ", p_dstex_path);
				return ERR_INVALID_DATA;
			}
		}
	} else {
		entry.mip_sizes = source_mip_sizes;
	}
	entry.gpu_footprint = gpu_footprint;

	uint64_t payload_size = 0;
	if (dstex_version == 1) {
		payload_size = dstex_header.get("compressed_size", 0);
		entry.payload_offset = static_cast<uint64_t>(sizeof(DSTexHeader)) +
				static_cast<uint64_t>(sizeof(DSTexMipEntry)) * static_cast<uint64_t>(mip_count);
	} else {
		for (int64_t mip = 0; mip < source_mip_sizes.size(); ++mip) {
			uint64_t mip_size = static_cast<uint32_t>(source_mip_sizes[mip]);
			if (!checked_add(payload_size, mip_size, payload_size)) {
				UtilityFunctions::push_error("DSPkg: .dstex mip sizes overflow the payload length: ", p_dstex_path);
				return ERR_INVALID_DATA;
			}
		}
	}

	entry.resource_path = p_resource_path;
	entry.dstex_path = p_dstex_path;
	entry.source_version = static_cast<uint16_t>(dstex_version);
	entry.source_compressed_size = dstex_header.get("compressed_size", 0);
	entry.payload_size = payload_size;
	entry.original_size = dstex_header.get("original_size", 0);
	entry.width = dstex_header.get("width", 0);
	entry.height = dstex_header.get("height", 0);
	// read_header always reports logical keys (storage fallback), so a v2
	// source stages storage dims and a v3 source stages its display dims.
	entry.logical_width = dstex_header.get("logical_width", entry.width);
	entry.logical_height = dstex_header.get("logical_height", entry.height);
	if (entry.logical_width == 0 || entry.logical_height == 0 ||
			entry.logical_width > entry.width || entry.logical_height > entry.height) {
		UtilityFunctions::push_error("DSPkg: .dstex logical dimensions are invalid: ", p_dstex_path);
		return ERR_INVALID_DATA;
	}
	entry.pixel_format = dstex_header.get("pixel_format", 0);
	entry.mip_count = mip_count;
	entry.flags = dstex_header.get("flags", 0);
	entry.source_mip_offsets = source_mip_offsets;
	entry.source_mip_sizes = source_mip_sizes;

	staging_entries.push_back(entry);
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

	std::vector<const DSPkgStagedEntry *> sorted_entries;
	sorted_entries.reserve(count);
	for (uint32_t i = 0; i < count; ++i) {
		sorted_entries.push_back(&staging_entries[i]);
	}
	std::sort(sorted_entries.begin(), sorted_entries.end(), [](const DSPkgStagedEntry *a, const DSPkgStagedEntry *b) {
		return a->resource_path < b->resource_path;
	});

	bool write_gpu_metadata = false;
	for (uint32_t i = 0; i < count; ++i) {
		if (sorted_entries[i]->gpu_metadata) {
			write_gpu_metadata = true;
			break;
		}
	}

	HashSet<String> seen_staging_paths;
	for (uint32_t i = 0; i < count; ++i) {
		const String &res_path = sorted_entries[i]->resource_path;
		if (seen_staging_paths.has(res_path)) {
			UtilityFunctions::push_error("DSPkg: Duplicate entry path encountered during finalization: " + res_path);
			return ERR_ALREADY_EXISTS;
		}
		seen_staging_paths.insert(res_path);
	}

	// v5 is emitted only when an entry needs an entry extension (an NPOT
	// logical-size TLV). Archives without one stay v4 byte-identical.
	bool write_v5 = false;
	for (uint32_t i = 0; i < count; ++i) {
		if (entry_logical_tlv_size(*sorted_entries[i]) > 0) {
			write_v5 = true;
			break;
		}
	}
	const uint64_t fixed_entry_size = write_v5 ? DSPKG_FIXED_ENTRY_SIZE_V5 : DSPKG_FIXED_ENTRY_SIZE_V3;

	uint64_t entry_table_size = 0;
	for (uint32_t i = 0; i < count; ++i) {
		const DSPkgStagedEntry &item = *sorted_entries[i];
		CharString utf8_path = item.resource_path.utf8();
		if (utf8_path.length() > UINT16_MAX) {
			UtilityFunctions::push_error("DSPkg: Resource path is too long to store in archive.");
			return ERR_INVALID_PARAMETER;
		}
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		uint64_t entry_size = 0;
		uint32_t mip_count = item.mip_count;
		uint64_t metadata_size = 0;
		if (write_gpu_metadata &&
				(!checked_multiply(mip_count, sizeof(DSPkgMipEntry), metadata_size) ||
					!checked_add(fixed_entry_size, metadata_size, entry_size))) {
			UtilityFunctions::push_error("DSPkg: Mip metadata table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
		if (!write_gpu_metadata) {
			entry_size = fixed_entry_size;
		}
		if (write_v5) {
			// Texture entries carry no entry metadata, only the extension
			// table (logical-size TLV when the staged size differs).
			uint64_t ext_size = entry_logical_tlv_size(item);
			if (!checked_add(entry_size, ext_size, entry_size)) {
				UtilityFunctions::push_error("DSPkg: Entry extension size overflow.");
				return ERR_INVALID_PARAMETER;
			}
		}
		if (!checked_add(entry_size, path_len, entry_size) || !checked_add(entry_table_size, entry_size, entry_table_size)) {
			UtilityFunctions::push_error("DSPkg: Entry table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
	}
	if (entry_table_size > (write_v5 ? DSPKG_MAX_TOC_SIZE_V5 : DSPKG_MAX_TOC_SIZE)) {
		return ERR_INVALID_PARAMETER;
	}

	const uint64_t header_size = write_v5 ? DSPKG_HEADER_SIZE_V5 : DSPKG_HEADER_SIZE_V4;
	const uint64_t header_envelope_size = m_encryption_enabled ? (header_size + DSPKG_ENCRYPTION_MAC_SIZE) : header_size;
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
	if (total_data_offset - header_envelope_size > (write_v5 ? DSPKG_MAX_TOC_SIZE_V5 : DSPKG_MAX_TOC_SIZE)) {
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
		const DSPkgStagedEntry &item = *sorted_entries[i];
		CharString utf8_path = item.resource_path.utf8();
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		uint64_t hash = item.resource_path.hash();

		uint64_t orig_size = item.original_size;
		uint32_t w = item.width;
		uint32_t h = item.height;
		uint32_t fmt = item.pixel_format;
		uint32_t mips_count = item.mip_count;
		const PackedInt32Array &mip_sizes_entry = item.mip_sizes;
		const PackedInt32Array &mip_uncompressed_sizes = item.mip_uncompressed_sizes;
		const PackedInt32Array &mip_row_pitches = item.mip_row_pitches;
		if (write_gpu_metadata && mip_sizes_entry.size() != static_cast<int64_t>(mips_count)) {
			UtilityFunctions::push_error("DSPkg: Texture mip metadata does not match the declared mip count.");
			return ERR_INVALID_DATA;
		}

		const bool per_mip_payload = write_gpu_metadata && item.gpu_metadata && mips_count > 1;
		uint64_t comp_size = 0;
		if (per_mip_payload) {
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
			comp_size = item.payload_size;
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
		uint32_t entry_flags = item.flags;
		writer.put_32(entry_flags);

		if (write_v5) {
			// v5 descriptor: texture kind, inline GDeflate payload, no entry
			// metadata, then the extension table (logical-size TLV when the
			// staged size differs), then the mip table below. Descriptor bytes
			// pack little-endian to match the reader's get_8 sequence.
			const uint32_t descriptor = static_cast<uint32_t>(DSPkgEntryKind::TEXTURE) |
					(static_cast<uint32_t>(DSCompressionFormat::GDEFLATE) << 8) |
					(static_cast<uint32_t>(DSGaclTransform::NONE) << 16);
			writer.put_32(descriptor);
			writer.put_32(0); // metadata_size: texture entries carry none
			const uint64_t ext_size = entry_logical_tlv_size(item);
			writer.put_32(static_cast<uint32_t>(ext_size));
			if (ext_size > 0) {
				const uint32_t lw = item.logical_width == 0 ? item.width : item.logical_width;
				const uint32_t lh = item.logical_height == 0 ? item.height : item.logical_height;
				writer.put_32(DS_TLV_TAG_TEXTURE_LOGICAL);
				writer.put_32(sizeof(DSPkgTextureLogical));
				writer.put_32(lw);
				writer.put_32(lh);
			}
		}

		if (write_gpu_metadata) {
			uint64_t mip_data_offset = 0;
			if (!checked_add(total_data_offset, running_data_offset, mip_data_offset)) {
				UtilityFunctions::push_error("DSPkg: Mip data offset overflow.");
				return ERR_INVALID_PARAMETER;
			}
			uint64_t running_mip_offset = 0;
			for (uint32_t mip = 0; mip < mips_count; ++mip) {
				if (per_mip_payload && mip > 0) {
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
				if (per_mip_payload && mip > 0) {
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
				if (item.gpu_metadata) {
					mip_entry.uncompressed_size = mip_uncompressed_sizes[mip];
					mip_entry.row_pitch = mip_row_pitches[mip];
				}
				// v5 reuses the reserved word as chunk_count: 1 = inline. This
				// writer only stages inline payloads (chunked v3 sources are
				// rejected by add_texture), so v5 entries set 1 while v4
				// entries keep the reserved zero.
				mip_entry.chunk_count = write_v5 ? 1 : 0;
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

	// Step 5: checked write helpers. godot-cpp's store_buffer() returns void, so
	// a full disk surfaces through FileAccess::get_error(); every write is
	// inspected because a truncated archive would still parse from its header
	// and TOC (both written first).
	uint64_t written_bytes = 0;
	bool write_failed = false;
	auto write_bytes = [&](const uint8_t *p_data, uint64_t p_size) -> bool {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (m_test_write_truncate_after_bytes >= 0) {
			const uint64_t allowed = static_cast<uint64_t>(m_test_write_truncate_after_bytes);
			if (written_bytes >= allowed) {
				// Pretend the bytes landed: the final size check must catch it.
				written_bytes += p_size;
				return true;
			}
			if (p_size > allowed - written_bytes) {
				const uint64_t keep = allowed - written_bytes;
				if (keep > 0) {
					file->store_buffer(p_data, static_cast<int64_t>(keep));
					if (file->get_error() != OK) {
						return false;
					}
				}
				written_bytes += p_size;
				return true;
			}
		}
#endif
		if (p_size > 0) {
			file->store_buffer(p_data, static_cast<int64_t>(p_size));
			if (file->get_error() != OK) {
				return false;
			}
		}
		written_bytes += p_size;
		return true;
	};
	auto fail_write = [&](const char *p_stage) -> Error {
		UtilityFunctions::push_error(String("DSPkg: Failed to write the archive ") + String(p_stage) +
				" (out of disk space?). The partial file was removed: " + build_output_path);
		file->close();
		DirAccess::remove_absolute(build_output_path);
		return ERR_FILE_CANT_WRITE;
	};
	static const uint8_t ZERO_PADDING[4096] = {0};
	auto write_zero_padding = [&](uint64_t p_current, uint64_t p_aligned) -> bool {
		uint64_t rem = p_aligned > p_current ? p_aligned - p_current : 0;
		while (rem > 0) {
			uint64_t chunk = std::min<uint64_t>(rem, sizeof(ZERO_PADDING));
			if (!write_bytes(ZERO_PADDING, chunk)) {
				return false;
			}
			rem -= chunk;
		}
		return true;
	};

	// Step 6: populate header. v5 is emitted only when an entry needs an
	// entry extension; otherwise the v4 bytes are untouched.
	DSPkgHeader header{};
	DSPkgHeaderV5 header_v5{};
	const uint8_t *header_bytes = nullptr;
	uint64_t header_len = 0;
	if (write_v5) {
		header_v5.magic = 0x474B5044;
		header_v5.version = DSPKG_VERSION_V5;
		header_v5.flags = write_gpu_metadata ? DSPKG_FLAG_GPU_MIP_METADATA : 0;
		header_v5.entry_count = count;
		header_v5.data_offset = total_data_offset;
		header_v5.archive_fingerprint = build_archive_fingerprint;
		header_v5.compression_codec = 0;
		header_v5.min_reader_version = DSPKG_VERSION_V5;
		header_v5.reserved0 = 0;
		header_v5.header_size = DSPKG_HEADER_SIZE_V5;
		header_v5.toc_entry_size = DSPKG_FIXED_ENTRY_SIZE_V5;
		header_v5.capabilities = 0;
		header_v5.extension_offset = 0;
		header_v5.extension_size = 0;
		header_v5.reserved2 = 0;
		header_bytes = reinterpret_cast<const uint8_t *>(&header_v5);
		header_len = sizeof(DSPkgHeaderV5);
	} else {
		header.magic = 0x474B5044;
		header.version = DSPKG_VERSION_V4;
		header.flags = write_gpu_metadata ? DSPKG_FLAG_GPU_MIP_METADATA : 0;
		header.entry_count = count;
		header.data_offset = total_data_offset;
		header.archive_fingerprint = build_archive_fingerprint;
		header.compression_codec = 0;
		header.min_reader_version = 4;
		header_bytes = reinterpret_cast<const uint8_t *>(&header);
		header_len = sizeof(DSPkgHeader);
	}

	uint8_t *header_salt = write_v5 ? header_v5.archive_salt : header.archive_salt;
	uint64_t *header_key_id = write_v5 ? &header_v5.key_id : &header.key_id;
	uint16_t *header_flags = write_v5 ? &header_v5.flags : &header.flags;

	if (m_encryption_enabled) {
		*header_flags |= DSPKG_FLAG_ENCRYPTED_TOC;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (m_test_salt_override.size() == 16) {
			memcpy(header_salt, m_test_salt_override.ptr(), 16);
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
			memcpy(header_salt, entropy.ptr(), 16);
		}
		uint8_t id_buf[8] = {0};
		crypto_blake2b(id_buf, 8, m_encryption_key.data(), 32);
		memcpy(header_key_id, id_buf, 8);
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
		const uint8_t *nonce_salt = write_v5 ? header_v5.archive_salt : header.archive_salt;
		memcpy(nonce, nonce_salt, 16);
		memcpy(nonce + 16, write_v5 ? DOMAIN_SEPARATOR_V5 : DOMAIN_SEPARATOR, 8);

		std::array<uint8_t, 32> local_key = m_encryption_key;
		PackedByteArray ciphertext;
		ciphertext.resize(ciphertext_len);
		uint8_t mac[DSPKG_ENCRYPTION_MAC_SIZE] = {0};
		crypto_aead_lock(ciphertext.ptrw(), mac, local_key.data(), nonce,
			header_bytes, header_len,
			entries_buf.ptr(), ciphertext_len);
		crypto_wipe(local_key.data(), 32);

		if (!write_bytes(header_bytes, header_len) ||
				!write_bytes(mac, DSPKG_ENCRYPTION_MAC_SIZE) ||
				!write_bytes(ciphertext.ptr(), ciphertext_len)) {
			return fail_write("header");
		}
	} else {
		if (!write_bytes(header_bytes, header_len) ||
				!write_bytes(entries_buf.ptr(), entry_table_size)) {
			return fail_write("header");
		}
	}

	// Step 8: single padding site + payloads
	if (!m_encryption_enabled) {
		if (!write_zero_padding(table_end_offset, total_data_offset)) {
			return fail_write("padding");
		}
	}

	file->seek(static_cast<int64_t>(total_data_offset));
	uint64_t payload_offset = total_data_offset;

	// Payloads are streamed in bounded chunks straight from the source, so the
	// builder's peak memory is the entry metadata plus one chunk, never the
	// whole project's compressed texture bytes.
	constexpr uint64_t STREAM_CHUNK_SIZE = 1024ULL * 1024ULL;
	auto stream_source_range = [&](FileAccess *p_source, uint64_t p_source_offset, uint64_t p_size) -> bool {
		if (p_size == 0) {
			return true;
		}
		if (p_source_offset > static_cast<uint64_t>(INT64_MAX)) {
			UtilityFunctions::push_error("DSPkg: Source payload offset exceeds the storable range.");
			return false;
		}
		p_source->seek(static_cast<int64_t>(p_source_offset));
		uint64_t remaining = p_size;
		while (remaining > 0) {
			const uint64_t chunk_size = std::min<uint64_t>(remaining, STREAM_CHUNK_SIZE);
			PackedByteArray chunk = p_source->get_buffer(static_cast<int64_t>(chunk_size));
			if (static_cast<uint64_t>(chunk.size()) != chunk_size) {
				UtilityFunctions::push_error("DSPkg: Source texture ended while streaming its payload.");
				return false;
			}
			if (!write_bytes(chunk.ptr(), chunk_size)) {
				write_failed = true;
				return false;
			}
			remaining -= chunk_size;
		}
		return true;
	};

	for (uint32_t i = 0; i < count; ++i) {
		const DSPkgStagedEntry &item = *sorted_entries[i];
		uint64_t target_payload_offset = 0;
		if (!checked_add(total_data_offset, entry_payload_offsets[i], target_payload_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
			file->close();
			return ERR_INVALID_PARAMETER;
		}
		if (target_payload_offset < payload_offset) {
			UtilityFunctions::push_error("DSPkg: Payload offset sequence non-monotonic.");
			file->close();
			return ERR_INVALID_DATA;
		}
		write_zero_padding(payload_offset, target_payload_offset);
		payload_offset = target_payload_offset;

		// Re-stat the source and compare it with the add-time snapshot. A
		// mutated, resized or replaced import aborts the archive instead of
		// streaming bytes that were never staged.
		Ref<FileAccess> source = FileAccess::open(item.dstex_path, FileAccess::READ);
		if (source.is_null() || !source->is_open()) {
			UtilityFunctions::push_error("DSPkg: Source texture was removed after it was added to the archive: ", item.dstex_path);
			file->close();
			return ERR_CANT_OPEN;
		}
		const uint64_t source_length = source->get_length();
		const uint64_t source_modified_time = FileAccess::get_modified_time(item.dstex_path);
		Ref<DSTex> dstex_reader;
		dstex_reader.instantiate();
		Dictionary current_header = dstex_reader->read_header(item.dstex_path);
		String mismatch_reason;
		const bool source_is_current = !current_header.is_empty() &&
				staged_source_is_unchanged(item, current_header, source_length, source_modified_time, mismatch_reason);
		if (!source_is_current) {
			if (current_header.is_empty()) {
				mismatch_reason = "header is no longer readable";
			}
			UtilityFunctions::push_error("DSPkg: Source texture changed after it was added to the archive (" + mismatch_reason + "); aborting instead of streaming unexpected bytes: " + item.dstex_path);
			source->close();
			file->close();
			return ERR_FILE_CORRUPT;
		}

		const uint32_t mips = item.mip_count;
		if (write_gpu_metadata && item.gpu_metadata && mips > 1) {
			// v1 containers keep one contiguous payload, so mip slices are read
			// at cumulative offsets; v2/v3-inline containers point at each mip directly.
			uint64_t contiguous_cursor = 0;
			for (uint32_t mip = 0; mip < mips; ++mip) {
				if (mip > 0) {
					uint64_t aligned_mip_offset = 0;
					if (!align_up(payload_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, aligned_mip_offset)) {
						UtilityFunctions::push_error("DSPkg: Payload mip file offset alignment overflow.");
						source->close();
						file->close();
						return ERR_INVALID_PARAMETER;
					}
					write_zero_padding(payload_offset, aligned_mip_offset);
					payload_offset = aligned_mip_offset;
				}
				const uint64_t mip_size = static_cast<uint32_t>(item.mip_sizes[mip]);
				uint64_t slice_offset = 0;
				if (item.source_version == 2 || item.source_version == 3) {
					slice_offset = static_cast<uint64_t>(item.source_mip_offsets[mip]);
				} else {
					if (!checked_add(item.payload_offset, contiguous_cursor, slice_offset) ||
							contiguous_cursor > item.payload_size || mip_size > item.payload_size - contiguous_cursor) {
						UtilityFunctions::push_error("DSPkg: Mip slice offset exceeds the staged payload size.");
						source->close();
						file->close();
						return ERR_INVALID_DATA;
					}
					contiguous_cursor += mip_size;
				}
				if (!stream_source_range(source.ptr(), slice_offset, mip_size)) {
					source->close();
					if (write_failed) {
						return fail_write("payload");
					}
					file->close();
					return ERR_FILE_CORRUPT;
				}
				if (!checked_add(payload_offset, mip_size, payload_offset)) {
					UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
					source->close();
					file->close();
					return ERR_INVALID_PARAMETER;
				}
			}
		} else {
			const uint32_t slice_count = (item.source_version == 2 || item.source_version == 3) ? mips : 1;
			for (uint32_t slice = 0; slice < slice_count; ++slice) {
				uint64_t slice_offset = item.payload_offset;
				uint64_t slice_size = item.payload_size;
				if (item.source_version == 2 || item.source_version == 3) {
					slice_offset = static_cast<uint64_t>(item.source_mip_offsets[slice]);
					slice_size = static_cast<uint64_t>(static_cast<uint32_t>(item.source_mip_sizes[slice]));
				}
				if (!stream_source_range(source.ptr(), slice_offset, slice_size)) {
					source->close();
					if (write_failed) {
						return fail_write("payload");
					}
					file->close();
					return ERR_FILE_CORRUPT;
				}
				if (!checked_add(payload_offset, slice_size, payload_offset)) {
					UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
					source->close();
					file->close();
					return ERR_INVALID_PARAMETER;
				}
			}
		}
		source->close();
	}

	// A short write that store_buffer() did not report would still ship a
	// truncated archive whose header and TOC parse fine. Verify the exact
	// expected size before declaring success.
	file->seek_end();
	const uint64_t actual_total = static_cast<uint64_t>(file->get_position());
	if (actual_total != payload_offset || file->get_error() != OK) {
		UtilityFunctions::push_error(String("DSPkg: Archive size mismatch after writing (expected ") +
				String::num_uint64(payload_offset) + " bytes, got " + String::num_uint64(actual_total) +
				"); the partial file was removed: " + build_output_path);
		file->close();
		DirAccess::remove_absolute(build_output_path);
		return ERR_FILE_CANT_WRITE;
	}

	file->close();
	finalize_completed = true;
	return OK;
}
Error DSPkg::open(const String &p_path, uint64_t p_expected_id) {
	entries_by_path.clear();
	entries_by_hash.clear();
	entries_by_offset.clear();
	is_opened = false;
	opened_path = "";
	loaded_header = DSPkgHeader{};
	loaded_header_size = DSPKG_HEADER_SIZE_V4;
	loaded_toc_entry_size = DSPKG_FIXED_ENTRY_SIZE_V3;
	loaded_capabilities = 0;
	loaded_extension_offset = 0;
	loaded_extension_size = 0;

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
		loaded_header_size = DSPKG_HEADER_SIZE_V4;
		loaded_toc_entry_size = DSPKG_FIXED_ENTRY_SIZE_V3;
		loaded_capabilities = 0;
		loaded_extension_offset = 0;
		loaded_extension_size = 0;
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

	if (loaded_header.version != 1 && loaded_header.version != 2 && loaded_header.version != 3 &&
			loaded_header.version != DSPKG_VERSION_V4 && loaded_header.version != DSPKG_VERSION_V5) {
		return fail(ERR_FILE_UNRECOGNIZED, "Unsupported archive version.");
	}

	const bool is_v5 = loaded_header.version == DSPKG_VERSION_V5;
	PackedByteArray v5_header_bytes;
	DSPkgHeaderV5 header_v5{};

	if (is_v5) {
		// header_size lives at offset 64 in the v5 header, so read just enough
		// to validate it before reading the rest. Bytes 0..63 keep the v4 field
		// layout, which is what loaded_header continues to carry.
		if (file_len < DSPKG_HEADER_SIZE_V4 + 2) {
			return fail(ERR_FILE_CORRUPT, "Archive file too small for v5 header.");
		}
		file->seek(DSPKG_HEADER_SIZE_V4);
		PackedByteArray hs_bytes = file->get_buffer(2);
		if (hs_bytes.size() < 2) {
			return fail(ERR_FILE_CORRUPT, "Failed to read v5 header_size.");
		}
		uint16_t v5_header_size = 0;
		memcpy(&v5_header_size, hs_bytes.ptr(), 2);
		if (v5_header_size < DSPKG_HEADER_SIZE_V5 || v5_header_size > 4096 ||
				v5_header_size % 8 != 0) {
			return fail(ERR_FILE_CORRUPT, "v5 header_size is out of range or misaligned.");
		}
		if (file_len < v5_header_size) {
			return fail(ERR_FILE_CORRUPT, "Archive file too small for the complete v5 header.");
		}
		file->seek(0);
		v5_header_bytes = file->get_buffer(v5_header_size);
		if (v5_header_bytes.size() < v5_header_size) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete v5 header bytes.");
		}
		memcpy(&loaded_header, v5_header_bytes.ptr(), DSPKG_HEADER_SIZE_V4);
		memcpy(&header_v5, v5_header_bytes.ptr(), sizeof(DSPkgHeaderV5));
		loaded_header_size = v5_header_size;
		loaded_toc_entry_size = header_v5.toc_entry_size;
		loaded_capabilities = header_v5.capabilities;
		loaded_extension_offset = header_v5.extension_offset;
		loaded_extension_size = header_v5.extension_size;
	} else if (loaded_header.version == DSPKG_VERSION_V4) {
		if (file_len < DSPKG_HEADER_SIZE_V4) {
			return fail(ERR_FILE_CORRUPT, "Archive file too small for v4 header.");
		}
		constexpr uint64_t v4_extra_size = DSPKG_HEADER_SIZE_V4 - DSPKG_HEADER_SIZE_LEGACY;
		PackedByteArray extra = file->get_buffer(v4_extra_size);
		if (extra.size() < v4_extra_size) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete v4 header bytes.");
		}
		memcpy(reinterpret_cast<uint8_t *>(&loaded_header) + DSPKG_HEADER_SIZE_LEGACY, extra.ptr(), v4_extra_size);
	}

	const bool is_v3_or_higher = (loaded_header.version >= DSPKG_VERSION_ENTRY_FLAGS);
	const bool has_gpu_metadata = (loaded_header.version == DSPKG_VERSION_GPU_METADATA || is_v3_or_higher) && ((loaded_header.flags & DSPKG_FLAG_GPU_MIP_METADATA) != 0);

	const bool is_encrypted = (loaded_header.version == DSPKG_VERSION_V4 || is_v5) &&
			((loaded_header.flags & DSPKG_FLAG_ENCRYPTED_TOC) != 0);

	// Framing check only: the MAC lives at header_size, so the envelope must
	// leave room for it. Semantic header fields are validated after the MAC.
	if (is_v5 && is_encrypted &&
			(static_cast<uint64_t>(loaded_header_size) + DSPKG_ENCRYPTION_MAC_SIZE > loaded_header.data_offset)) {
		return fail(ERR_FILE_CORRUPT, "v5 encrypted header envelope overlaps data_offset.");
	}

	// The encryption requirement comes from the build, never from the file. A
	// protected export publishes directstorage/export/protected into its own
	// settings, and the header flag that would otherwise decide whether the TOC
	// is authenticated is attacker-controlled: reading it alone lets a plaintext
	// archive with a copied archive id open in a build that advertises an
	// authenticated TOC. Source and editor builds never publish the setting, so
	// they keep accepting plaintext archives.
	bool require_encrypted_toc = false;
	const ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps != nullptr && ps->has_setting("directstorage/export/protected")) {
		require_encrypted_toc = static_cast<bool>(ps->get_setting("directstorage/export/protected"));
	}
	if (require_encrypted_toc && !is_encrypted) {
		// A v4/v5 header that simply cleared the flag is a downgrade; anything
		// older cannot carry the flag at all, so it is a mismatch rather than a
		// downgrade. The two map to different sentinel reasons in ds_runtime.gd.
		if (loaded_header.version == DSPKG_VERSION_V4 || is_v5) {
			return fail(ERR_UNAUTHORIZED, "Protected build requires an authenticated TOC; archive is not encrypted.");
		}
		return fail(ERR_FILE_UNRECOGNIZED, "Protected build requires a v4/v5 encrypted TOC; archive version is not acceptable.");
	}

	if (is_encrypted && loaded_header.key_id == 0) {
		return fail(ERR_UNAUTHORIZED, "Encrypted archive header has zero key_id.");
	}

	uint64_t toc_start = DSPKG_HEADER_SIZE_LEGACY;
	if (is_v5) {
		if (static_cast<uint64_t>(loaded_header_size) > loaded_header.data_offset) {
			return fail(ERR_FILE_CORRUPT, "v5 header_size exceeds data_offset.");
		}
		toc_start = static_cast<uint64_t>(loaded_header_size) +
				(is_encrypted ? DSPKG_ENCRYPTION_MAC_SIZE : 0);
	} else if (loaded_header.version == DSPKG_VERSION_V4) {
		toc_start = DSPKG_HEADER_SIZE_V4 + (is_encrypted ? DSPKG_ENCRYPTION_MAC_SIZE : 0);
	}

	// Pre-MAC TOC bounds. Use the fixed minimum entry size, not the
	// unauthenticated toc_entry_size, so no semantic header field drives the
	// allocation before the MAC verifies. The extension-size cap is needed for
	// the plausible-bound arithmetic.
	if (is_v5 && loaded_extension_size > DS_HEADER_TLV_MAX_TOTAL) {
		return fail(ERR_FILE_CORRUPT, "v5 header extension size is invalid.");
	}
	const uint64_t pre_mac_entry_size = is_v5 ? DSPKG_FIXED_ENTRY_SIZE_V5 :
			(is_v3_or_higher ? DSPKG_FIXED_ENTRY_SIZE_V3 : DSPKG_FIXED_ENTRY_SIZE_LEGACY);
	uint64_t min_table_size = 0;
	uint64_t min_data_offset = 0;
	if (!checked_multiply(loaded_header.entry_count, pre_mac_entry_size, min_table_size) ||
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
	if (is_v5) {
		// Enforce both bounds before allocating or decrypting the TOC: the flat
		// cap alone lets a one-entry archive with data_offset = 32 MiB force a
		// 32 MiB allocation.
		if (toc_len > DSPKG_MAX_TOC_SIZE_V5) {
			return fail(ERR_FILE_CORRUPT, "v5 TOC envelope exceeds the 64 MiB limit.");
		}
		uint64_t max_serialized = 0;
		uint64_t max_toc = 0;
		if (!checked_multiply(loaded_header.entry_count, DSPKG_MAX_SERIALIZED_ENTRY_SIZE_V5, max_serialized) ||
				!checked_add(max_serialized, loaded_extension_size, max_serialized) ||
				!align_up(max_serialized, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, max_toc)) {
			return fail(ERR_FILE_CORRUPT, "v5 TOC bound overflow.");
		}
		if (toc_len > max_toc) {
			return fail(ERR_FILE_CORRUPT, "v5 TOC length exceeds plausible bounds.");
		}
	} else if (loaded_header.version == DSPKG_VERSION_V4 || is_encrypted) {
		if (toc_len > DSPKG_MAX_TOC_SIZE) {
			return fail(ERR_FILE_CORRUPT, "TOC envelope exceeds maximum size limit (16 MiB).");
		}
	} else {
		constexpr uint64_t MAX_SERIALIZED_ENTRY_SIZE = DSPKG_ENTRY_PREFIX_SIZE + UINT16_MAX + DSPKG_ENTRY_FIELDS_SIZE_V3 + 24ULL * DSTEX_MAX_MIP_COUNT;
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
		std::array<uint8_t, 32> candidate_key = {};
		uint64_t candidate_key_id = 0;
		if (!resolve_encryption_key(candidate_key, candidate_key_id)) {
			wipe_key_bytes(candidate_key);
			return fail(ERR_UNAUTHORIZED, "No decryption key available. Expected res://.dspkg/toc.key inside an encrypted PCK.");
		}
		if (loaded_header.key_id != candidate_key_id) {
			wipe_key_bytes(candidate_key);
			return fail(ERR_UNAUTHORIZED, "Archive encryption key ID mismatch.");
		}
		const uint64_t mac_offset = is_v5 ? loaded_header_size : DSPKG_HEADER_SIZE_V4;
		if (file->get_position() != mac_offset) {
			file->seek(mac_offset);
		}
		PackedByteArray mac_bytes = file->get_buffer(DSPKG_ENCRYPTION_MAC_SIZE);
		if (mac_bytes.size() < DSPKG_ENCRYPTION_MAC_SIZE) {
			wipe_key_bytes(candidate_key);
			return fail(ERR_FILE_CORRUPT, "Failed to read complete MAC bytes.");
		}
		if (file->get_position() != toc_start) {
			file->seek(toc_start);
		}
		PackedByteArray ciphertext = file->get_buffer(toc_len);
		if (ciphertext.size() < toc_len) {
			wipe_key_bytes(candidate_key);
			return fail(ERR_FILE_CORRUPT, "Failed to read complete TOC ciphertext.");
		}
		uint8_t nonce[DSPKG_ENCRYPTION_NONCE_SIZE] = {0};
		memcpy(nonce, loaded_header.archive_salt, 16);
		memcpy(nonce + 16, is_v5 ? DOMAIN_SEPARATOR_V5 : DOMAIN_SEPARATOR, 8);

		plaintext_toc.resize(toc_len);
		const uint8_t *aad = is_v5 ? v5_header_bytes.ptr() :
				reinterpret_cast<const uint8_t *>(&loaded_header);
		const uint64_t aad_len = is_v5 ? loaded_header_size : sizeof(DSPkgHeader);
		int unlock_res = crypto_aead_unlock(
			plaintext_toc.ptrw(), mac_bytes.ptr(), candidate_key.data(), nonce,
			aad, aad_len,
			ciphertext.ptr(), toc_len
		);
		if (unlock_res != 0) {
			wipe_key_bytes(candidate_key);
			crypto_wipe(plaintext_toc.ptrw(), plaintext_toc.size());
			return fail(ERR_UNAUTHORIZED, "TOC decryption failed or authentication MAC mismatch.");
		}
		// Installation waits for both proofs: a candidate that reproduced the
		// header id and decrypted the TOC is the archive's key. Anything earlier
		// would let a wrong key file pin the process and defeat the retry after a
		// later PCK mount.
		set_global_encryption_key_raw(candidate_key.data(), candidate_key.size());
		wipe_key_bytes(candidate_key);
	} else {
		if (file->get_position() != toc_start) {
			file->seek(toc_start);
		}
		plaintext_toc = file->get_buffer(toc_len);
		if (plaintext_toc.size() < toc_len) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete unencrypted TOC bytes.");
		}
	}

	// Header semantics are validated only after the encrypted TOC MAC has
	// verified. For plaintext archives there is no MAC, so this is the first
	// semantic pass either way. Only framing/DoS bounds and key selection ran
	// before the AEAD above.
	auto validate_header_semantics = [&]() -> Error {
		if (is_v5) {
			if (header_v5.toc_entry_size < DSPKG_FIXED_ENTRY_SIZE_V5 ||
					header_v5.toc_entry_size > 1024) {
				return fail(ERR_FILE_CORRUPT, "v5 toc_entry_size is out of range.");
			}
			if (header_v5.reserved0 != 0 || header_v5.reserved2 != 0) {
				return fail(ERR_FILE_CORRUPT, "Reserved v5 header fields must be zero.");
			}
			if (header_v5.compression_codec != 0) {
				return fail(ERR_FILE_CORRUPT, "v5 header compression_codec must be zero.");
			}
			if (header_v5.min_reader_version < DSPKG_VERSION_V5) {
				return fail(ERR_FILE_CORRUPT, "v5 min_reader_version must be at least 5.");
			}
			if (header_v5.min_reader_version > DSPKG_VERSION_V5) {
				return fail(ERR_FILE_UNRECOGNIZED, "Archive requires newer reader version.");
			}
			if ((header_v5.capabilities & ~DSPKG_CAP_KNOWN_MASK) != 0) {
				return fail(ERR_FILE_UNRECOGNIZED, "Archive requires unsupported feature bits.");
			}
			if (header_v5.extension_size % 8 != 0) {
				return fail(ERR_FILE_CORRUPT, "v5 header extension size is invalid.");
			}
			if (header_v5.extension_size == 0) {
				if (header_v5.extension_offset != 0) {
					return fail(ERR_FILE_CORRUPT, "v5 extension_offset must be zero when extension_size is zero.");
				}
			} else if (header_v5.extension_offset == 0 || header_v5.extension_offset % 8 != 0) {
				return fail(ERR_FILE_CORRUPT, "v5 extension_offset is invalid.");
			}
			const bool declares_extension = (loaded_header.flags & DSPKG_FLAG_EXTENSION_TABLE) != 0;
			if (declares_extension != (loaded_extension_size > 0)) {
				return fail(ERR_FILE_CORRUPT, "v5 extension flag does not match extension_size.");
			}
			if (loaded_header.data_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) {
				return fail(ERR_FILE_CORRUPT, "v5 data_offset must be 512-byte aligned.");
			}
			if (!is_encrypted) {
				for (int i = 0; i < 16; ++i) {
					if (loaded_header.archive_salt[i] != 0) {
						return fail(ERR_FILE_CORRUPT, "Unencrypted v5 archive has a non-zero archive_salt.");
					}
				}
			}
		} else if (loaded_header.version == DSPKG_VERSION_V4) {
			if (loaded_header.min_reader_version > DSPKG_VERSION_V4) {
				return fail(ERR_FILE_UNRECOGNIZED, "Archive requires newer reader version.");
			}
			if (loaded_header.compression_codec != 0) {
				return fail(ERR_FILE_UNRECOGNIZED, "Unsupported compression codec.");
			}
			if (loaded_header.reserved[0] != 0 || loaded_header.reserved[1] != 0 ||
					loaded_header.reserved[2] != 0 || loaded_header.reserved[3] != 0) {
				return fail(ERR_FILE_CORRUPT, "Reserved header bytes must be zero.");
			}
		}

		if ((loaded_header.version == DSPKG_VERSION_GPU_METADATA && loaded_header.flags != DSPKG_FLAG_GPU_MIP_METADATA) ||
				(loaded_header.version == DSPKG_VERSION_LEGACY && loaded_header.flags != 0) ||
				(loaded_header.version == DSPKG_VERSION_ENTRY_FLAGS && (loaded_header.flags & ~DSPKG_FLAG_GPU_MIP_METADATA) != 0) ||
				(loaded_header.version == DSPKG_VERSION_V4 && (loaded_header.flags & ~(DSPKG_FLAG_GPU_MIP_METADATA | DSPKG_FLAG_ENCRYPTED_TOC)) != 0) ||
				(is_v5 && (loaded_header.flags & ~(DSPKG_FLAG_GPU_MIP_METADATA | DSPKG_FLAG_ENCRYPTED_TOC | DSPKG_FLAG_EXTENSION_TABLE)) != 0)) {
			return fail(ERR_FILE_CORRUPT, "Invalid flags in header.");
		}
		if (!is_encrypted && loaded_header.key_id != 0) {
			return fail(ERR_FILE_CORRUPT, "Unencrypted archive header has non-zero key_id.");
		}
		// Pairing is authenticated by the MAC for encrypted archives.
		if (p_expected_id != 0 && p_expected_id != loaded_header.archive_fingerprint) {
			return fail(ERR_FILE_UNRECOGNIZED, String("Archive id does not match this build (archive id: 0x") + String::num_uint64(loaded_header.archive_fingerprint, 16) + ", expected: 0x" + String::num_uint64(p_expected_id, 16) + ").");
		}
		return OK;
	};
	Error semantics_error = validate_header_semantics();
	if (semantics_error != OK) {
		return semantics_error;
	}

	const uint64_t entry_fields_size = is_v5 ? DSPKG_ENTRY_FIELDS_SIZE_V5 :
			(is_v3_or_higher ? DSPKG_ENTRY_FIELDS_SIZE_V3 : DSPKG_ENTRY_FIELDS_SIZE_LEGACY);
	const uint64_t fixed_entry_size = is_v5 ? loaded_toc_entry_size :
			(is_v3_or_higher ? DSPKG_FIXED_ENTRY_SIZE_V3 : DSPKG_FIXED_ENTRY_SIZE_LEGACY);

	TOCBufferReader reader(plaintext_toc.ptr(), plaintext_toc.size());

	// Absolute end of the previous entry's payload. finalize() writes entries in
	// resource-path order over one running payload offset, so writer output is
	// ordered and disjoint; this is the boundary every following entry must
	// start at or after.
	uint64_t previous_payload_end = loaded_header.data_offset;

	bool used_zstd = false;
	bool used_gacl = false;
	bool used_chunked = false;
	bool used_mesh = false;
	bool used_content_hash = false;
	DSPkgTlvSummary entry_tlv_summary;
	DSPkgTlvSummary header_tlv_summary;

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

		// The TOC stores a hash of the resource path beside the path itself
		// (finalize() writes path.hash()), and entries_by_hash is keyed by that
		// stored value. A stored hash that does not reproduce from the decoded
		// path can only come from a rewritten or corrupted TOC: accept it and
		// find_entry_for_path() keeps serving the entry from entries_by_path
		// while the hash index stays keyed to a value no lookup can produce.
		if (entry.path_hash != static_cast<uint64_t>(entry.path.hash())) {
			return fail(ERR_FILE_CORRUPT, "Entry path hash does not match its path.");
		}

		entry.data_offset = reader.get_64();
		entry.compressed_size = reader.get_64();
		entry.original_size = reader.get_64();
		entry.width = reader.get_32();
		entry.height = reader.get_32();
		entry.pixel_format = reader.get_32();
		entry.mip_count = reader.get_32();
		entry.flags = is_v3_or_higher ? reader.get_32() : 0;

		if (is_v5) {
			entry.entry_kind = reader.get_8();
			entry.compression_format = reader.get_8();
			entry.gacl_transform = reader.get_8();
			entry.descriptor_flags = reader.get_8();
			entry.metadata_size = reader.get_32();
			entry.extensions_size = reader.get_32();
			if (!reader.is_ok()) {
				return fail(ERR_FILE_CORRUPT, "Entry v5 descriptor extends past TOC buffer.");
			}
			// Future v5.x readers may append fixed fields; toc_entry_size covers
			// them without changing the fields we decode.
			if (loaded_toc_entry_size > DSPKG_FIXED_ENTRY_SIZE_V5 &&
					!reader.skip(loaded_toc_entry_size - DSPKG_FIXED_ENTRY_SIZE_V5)) {
				return fail(ERR_FILE_CORRUPT, "Entry trailing fixed fields extend past TOC buffer.");
			}
		} else {
			entry.entry_kind = static_cast<uint8_t>(DSPkgEntryKind::TEXTURE);
			entry.compression_format = static_cast<uint8_t>(DSCompressionFormat::GDEFLATE);
			entry.gacl_transform = static_cast<uint8_t>(DSGaclTransform::NONE);
		}

		if (is_v5) {
			if (!ds_entry_kind_is_valid(entry.entry_kind)) {
				return fail(ERR_FILE_UNRECOGNIZED, "Unsupported entry kind.");
			}
			if (!ds_compression_format_is_valid(entry.compression_format)) {
				return fail(ERR_FILE_UNRECOGNIZED, "Unsupported entry compression format.");
			}
			if (!ds_gacl_transform_is_valid(entry.gacl_transform)) {
				return fail(ERR_FILE_UNRECOGNIZED, "Unsupported entry GACL transform.");
			}
			if ((entry.descriptor_flags & ~1U) != 0) {
				return fail(ERR_FILE_CORRUPT, "Entry descriptor_flags has reserved bits set.");
			}
			if (entry.metadata_size > DSPKG_MAX_ENTRY_METADATA ||
					entry.extensions_size > DSPKG_MAX_ENTRY_EXTENSIONS ||
					entry.extensions_size % 8 != 0) {
				return fail(ERR_FILE_CORRUPT, "Entry metadata or extension size is invalid.");
			}
			if (entry.entry_kind == static_cast<uint8_t>(DSPkgEntryKind::TEXTURE)) {
				if ((entry.flags & ~3U) != 0) {
					return fail(ERR_FILE_CORRUPT, "Texture entry flags have reserved bits set.");
				}
				if (entry.width == 0 || entry.height == 0 || entry.mip_count == 0 ||
						entry.mip_count > DSTEX_MAX_MIP_COUNT ||
						!dstex_sizes_are_sane(entry.width, entry.height, entry.original_size, entry.compressed_size)) {
					return fail(ERR_FILE_CORRUPT, "Invalid entry dimensions or mip count.");
				}
				if (entry.metadata_size != 0) {
					return fail(ERR_FILE_CORRUPT, "Texture entry metadata_size must be zero.");
				}
				if (!has_gpu_metadata) {
					return fail(ERR_FILE_CORRUPT, "Texture entry has no mip table.");
				}
			} else {
				if (entry.width != 0 || entry.height != 0 || entry.pixel_format != 0 ||
						entry.mip_count != 0 || entry.flags != 0) {
					return fail(ERR_FILE_CORRUPT, "Mesh/blob/audio entry carries texture fields.");
				}
				if (entry.metadata_size == 0) {
					return fail(ERR_FILE_CORRUPT, "Mesh/blob/audio entry metadata is missing.");
				}
				if (entry.entry_kind == static_cast<uint8_t>(DSPkgEntryKind::AUDIO) &&
						entry.compression_format != static_cast<uint8_t>(DSCompressionFormat::NONE)) {
					return fail(ERR_FILE_CORRUPT, "Audio entry must use compression_format NONE.");
				}
			}
			if (entry.compression_format == static_cast<uint8_t>(DSCompressionFormat::NONE) &&
					entry.compressed_size != entry.original_size) {
				return fail(ERR_FILE_CORRUPT, "NONE-compressed entry sizes must match.");
			}
			if (entry.gacl_transform != static_cast<uint8_t>(DSGaclTransform::NONE)) {
				if (entry.entry_kind != static_cast<uint8_t>(DSPkgEntryKind::TEXTURE) ||
						entry.compression_format == static_cast<uint8_t>(DSCompressionFormat::NONE) ||
						!DSTex::gacl_transform_matches_pixel_format(
								static_cast<DSGaclTransform>(entry.gacl_transform),
								static_cast<int>(entry.pixel_format))) {
					return fail(ERR_FILE_CORRUPT, "GACL transform does not match the entry pixel format.");
				}
			}
		} else if (entry.width == 0 || entry.height == 0 || entry.mip_count == 0 ||
				entry.mip_count > DSTEX_MAX_MIP_COUNT ||
				!dstex_sizes_are_sane(entry.width, entry.height, entry.original_size, entry.compressed_size)) {
			return fail(ERR_FILE_CORRUPT, "Invalid entry dimensions or mip count.");
		}

		if (!payload_sizes_are_supported(entry)) {
			return fail(ERR_FILE_CORRUPT, "Entry payload size is zero or exceeds the supported limit.");
		}

		uint64_t abs_payload_offset = 0;
		uint64_t abs_payload_end = 0;
		if (!get_payload_range(loaded_header, entry, abs_payload_offset, abs_payload_end)) {
			return fail(ERR_FILE_CORRUPT, "Entry payload offset arithmetic overflow.");
		}
		if (abs_payload_end > file_len) {
			return fail(ERR_FILE_CORRUPT, "Entry payload extends past archive length.");
		}

		// Writer output is ordered and disjoint (see previous_payload_end above),
		// and the exact-duplicate check below is keyed by absolute offset and only
		// runs for entries carrying GPU mip metadata, so a partially overlapping
		// span would otherwise pass unremarked.
		if (abs_payload_offset < previous_payload_end) {
			return fail(ERR_FILE_CORRUPT, "Entry payload spans overlap a previous entry.");
		}
		previous_payload_end = abs_payload_end;

		if (is_v5) {
			// Entry payload sections after the fixed fields: metadata, then the
			// entry TLV, then (for textures) the mip table and chunk tables.
			if (entry.metadata_size > 0 && !reader.skip(entry.metadata_size)) {
				return fail(ERR_FILE_CORRUPT, "Entry metadata extends past TOC buffer.");
			}
			if (entry.extensions_size > 0) {
				DSPkgTlvSummary summary;
				String tlv_error;
				const uint8_t *tlv_data = reader.get_data() + reader.get_cursor();
				if (!validate_tlv_table(tlv_data, entry.extensions_size,
							DS_ENTRY_TLV_MAX_TOTAL, "Entry", summary, tlv_error)) {
					return fail(ERR_FILE_CORRUPT, tlv_error);
				}
				if (entry.entry_kind == static_cast<uint8_t>(DSPkgEntryKind::TEXTURE)) {
					bool has_logical = false;
					uint32_t logical_w = 0;
					uint32_t logical_h = 0;
					if (!parse_entry_logical_tlv(tlv_data, entry.extensions_size, has_logical, logical_w, logical_h)) {
						return fail(ERR_FILE_CORRUPT, "Entry logical-size record is malformed.");
					}
					if (has_logical) {
						// 0 means storage (tolerant read); anything past
						// storage is a hostile archive: fail closed.
						if ((logical_w != 0 && logical_w > entry.width) ||
								(logical_h != 0 && logical_h > entry.height)) {
							return fail(ERR_FILE_CORRUPT, "Entry logical dimensions exceed storage dimensions.");
						}
						entry.logical_width = logical_w;
						entry.logical_height = logical_h;
					}
				}
				if (!reader.skip(entry.extensions_size)) {
					return fail(ERR_FILE_CORRUPT, "Entry extension table extends past TOC buffer.");
				}
				entry_tlv_summary.has_crypto = entry_tlv_summary.has_crypto || summary.has_crypto;
				entry_tlv_summary.has_provenance = entry_tlv_summary.has_provenance || summary.has_provenance;
				entry_tlv_summary.has_archive_set = entry_tlv_summary.has_archive_set || summary.has_archive_set;
				entry_tlv_summary.has_delta = entry_tlv_summary.has_delta || summary.has_delta;
			}
			if (entry.compression_format == static_cast<uint8_t>(DSCompressionFormat::ZSTD)) {
				used_zstd = true;
			}
			if (entry.gacl_transform != static_cast<uint8_t>(DSGaclTransform::NONE)) {
				used_gacl = true;
			}
			if (entry.entry_kind == static_cast<uint8_t>(DSPkgEntryKind::MESH)) {
				used_mesh = true;
			}
			if ((entry.descriptor_flags & 1U) != 0) {
				used_content_hash = true;
			}
		}

		if (has_gpu_metadata && entry.entry_kind == static_cast<uint8_t>(DSPkgEntryKind::TEXTURE)) {
			uint64_t metadata_size = 0;
			if (!checked_multiply(entry.mip_count, sizeof(DSPkgMipEntry), metadata_size) ||
				!reader.has_remaining(metadata_size)) {
				return fail(ERR_FILE_CORRUPT, "Entry mip metadata extends past TOC buffer.");
			}
			entry.mip_offsets.resize(entry.mip_count);
			entry.mip_sizes.resize(entry.mip_count);
			entry.mip_uncompressed_sizes.resize(entry.mip_count);
			entry.mip_row_pitches.resize(entry.mip_count);
			if (is_v5) {
				entry.mip_chunk_counts.resize(entry.mip_count);
			}
			uint64_t footprint_size = 0;
			uint64_t compressed_mip_size = 0;
			uint64_t previous_mip_end = abs_payload_offset;
			for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
				uint64_t mip_offset = reader.get_64();
				uint32_t mip_compressed = reader.get_32();
				uint32_t mip_uncompressed = reader.get_32();
				uint32_t mip_row_pitch = reader.get_32();
				uint32_t mip_tail = reader.get_32();
				uint32_t mip_chunk_count = 1;
				if (is_v5) {
					mip_chunk_count = mip_tail;
					if (mip_chunk_count == 0 || mip_chunk_count > DSPKG_MAX_CHUNKS_PER_MIP) {
						return fail(ERR_FILE_CORRUPT, "Invalid v5 mip chunk_count.");
					}
					entry.mip_chunk_counts[mip] = static_cast<int32_t>(mip_chunk_count);
					if (mip_chunk_count > 1) {
						used_chunked = true;
					}
				} else if (mip_tail != 0) {
					return fail(ERR_FILE_CORRUPT, "Mip reserved field must be zero.");
				}
				entry.mip_offsets[mip] = static_cast<int64_t>(mip_offset);
				entry.mip_sizes[mip] = static_cast<int32_t>(mip_compressed);
				entry.mip_uncompressed_sizes[mip] = static_cast<int32_t>(mip_uncompressed);
				entry.mip_row_pitches[mip] = static_cast<int32_t>(mip_row_pitch);
				uint64_t mip_end = 0;
				if (mip_compressed == 0 ||
					!checked_add(mip_offset, mip_compressed, mip_end) ||
					(!is_v5 && mip_offset < previous_mip_end) ||
					mip_end > abs_payload_end ||
					!checked_add(compressed_mip_size, mip_compressed, compressed_mip_size) ||
					(mip_uncompressed > 0 &&
						(mip_row_pitch == 0 || mip_row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0)) ||
					(mip_uncompressed > 0 &&
						!checked_add(footprint_size, mip_uncompressed, footprint_size))) {
					return fail(ERR_FILE_CORRUPT, "Invalid entry mip metadata.");
				}
				if (mip == 0 && mip_offset != abs_payload_offset) {
					return fail(ERR_FILE_CORRUPT, "First mip offset must match entry payload start.");
				}
				if (mip_uncompressed > 0 && mip_offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT != 0) {
					return fail(ERR_FILE_CORRUPT, "Mip offset must be aligned to 512 bytes for GPU footprint.");
				}
				previous_mip_end = mip_end;
			}

			if (is_v5) {
				// Chunk tables follow the whole mip table, in mip order.
				uint64_t previous_chunk_end = abs_payload_offset;
				for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
					const uint32_t chunk_count = static_cast<uint32_t>(entry.mip_chunk_counts[mip]);
					const uint32_t row_pitch = static_cast<uint32_t>(entry.mip_row_pitches[mip]);
					const uint64_t mip_offset = static_cast<uint64_t>(entry.mip_offsets[mip]);
					const uint64_t mip_compressed = static_cast<uint64_t>(static_cast<uint32_t>(entry.mip_sizes[mip]));
					const uint64_t mip_uncompressed = static_cast<uint64_t>(static_cast<uint32_t>(entry.mip_uncompressed_sizes[mip]));
					if (chunk_count == 1) {
						if (mip_offset < previous_chunk_end || mip_offset + mip_compressed > abs_payload_end) {
							return fail(ERR_FILE_CORRUPT, "v5 mip payload overlaps or exceeds entry bounds.");
						}
						if (entry.compression_format == static_cast<uint8_t>(DSCompressionFormat::NONE) &&
								mip_compressed != mip_uncompressed) {
							return fail(ERR_FILE_CORRUPT, "NONE-compressed mip sizes must match.");
						}
						previous_chunk_end = mip_offset + mip_compressed;
						continue;
					}
					if (row_pitch == 0 || mip_uncompressed % row_pitch != 0) {
						return fail(ERR_FILE_CORRUPT, "v5 chunked mip is not a whole number of rows.");
					}
					uint64_t sum_compressed = 0;
					uint64_t sum_uncompressed = 0;
					for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
						if (!reader.has_remaining(sizeof(DSChunkEntry))) {
							return fail(ERR_FILE_CORRUPT, "v5 chunk table extends past TOC buffer.");
						}
						uint64_t chunk_offset = reader.get_64();
						uint32_t chunk_compressed = reader.get_32();
						uint32_t chunk_uncompressed = reader.get_32();
						uint64_t chunk_end = 0;
						if (chunk_compressed == 0 || chunk_uncompressed == 0 ||
								chunk_uncompressed % row_pitch != 0 ||
								!checked_add(chunk_offset, chunk_compressed, chunk_end) ||
								chunk_offset < previous_chunk_end || chunk_end > abs_payload_end ||
								!checked_add(sum_compressed, chunk_compressed, sum_compressed) ||
								!checked_add(sum_uncompressed, chunk_uncompressed, sum_uncompressed)) {
							return fail(ERR_FILE_CORRUPT, "Invalid v5 chunk entry.");
						}
						if (chunk_index == 0 && chunk_offset != mip_offset) {
							return fail(ERR_FILE_CORRUPT, "v5 first chunk must match the mip offset.");
						}
						if (chunk_offset % (chunk_index == 0 ? D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT : 8) != 0) {
							return fail(ERR_FILE_CORRUPT, "v5 chunk offset alignment mismatch.");
						}
						if (entry.compression_format == static_cast<uint8_t>(DSCompressionFormat::NONE) &&
								chunk_compressed != chunk_uncompressed) {
							return fail(ERR_FILE_CORRUPT, "NONE-compressed chunk sizes must match.");
						}
						entry.chunk_offsets.push_back(static_cast<int64_t>(chunk_offset));
						entry.chunk_compressed_sizes.push_back(static_cast<int32_t>(chunk_compressed));
						entry.chunk_uncompressed_sizes.push_back(static_cast<int32_t>(chunk_uncompressed));
						previous_chunk_end = chunk_end;
					}
					if (sum_compressed != mip_compressed || sum_uncompressed != mip_uncompressed) {
						return fail(ERR_FILE_CORRUPT, "v5 chunk sums do not match the mip.");
					}
				}
				entry.total_chunk_count = static_cast<uint32_t>(entry.chunk_compressed_sizes.size());
				entry.chunked = entry.total_chunk_count > 0;
				// entry.compressed_size is the payload span (it includes the
				// 512-byte placement padding between mips, exactly like v4),
				// so only the final payload end can be compared to it.
				if (compressed_mip_size > entry.compressed_size ||
						previous_chunk_end != abs_payload_end) {
					return fail(ERR_FILE_CORRUPT, "v5 entry payload does not match its mip/chunk table.");
				}
			} else {
				if (compressed_mip_size > entry.compressed_size) {
					return fail(ERR_FILE_CORRUPT, "Entry mip sizes exceed compressed size.");
				}
				if (entry.mip_count == 1 && compressed_mip_size != entry.compressed_size) {
					return fail(ERR_FILE_CORRUPT, "Entry mip size does not match compressed size.");
				}
				if (entry.mip_count > 1 && previous_mip_end != abs_payload_end) {
					return fail(ERR_FILE_CORRUPT, "Last mip does not end at entry compressed payload boundary.");
				}
			}
			entry.has_gpu_metadata = footprint_size > 0;
			if (entry.has_gpu_metadata && footprint_size != entry.original_size) {
				return fail(ERR_FILE_CORRUPT, "Entry footprint metadata does not match original size.");
			}
			if (entry.has_gpu_metadata) {
				Vector<DSPkgMipEntry> mip_vector;
				mip_vector.resize(entry.mip_count);
				for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
					DSPkgMipEntry &m_e = mip_vector.write[mip];
					m_e.offset = static_cast<uint64_t>(entry.mip_offsets[mip]);
					m_e.compressed_size = static_cast<uint32_t>(entry.mip_sizes[mip]);
					m_e.uncompressed_size = static_cast<uint32_t>(entry.mip_uncompressed_sizes[mip]);
					m_e.row_pitch = static_cast<uint32_t>(entry.mip_row_pitches[mip]);
					m_e.chunk_count = is_v5 ? static_cast<uint32_t>(entry.mip_chunk_counts[mip]) : 1;
					if (!is_v5) {
						m_e.reserved = 0;
					}
				}
				if (entries_by_offset.has(abs_payload_offset)) {
					return fail(ERR_FILE_CORRUPT, "Duplicate entry payload offset in archive.");
				}
				DSPkgOffsetEntry offset_entry;
				offset_entry.mips = mip_vector;
				offset_entry.compression_format = entry.compression_format;
				offset_entry.gacl_transform = entry.gacl_transform;
				offset_entry.chunked = entry.chunked;
				entries_by_offset[abs_payload_offset] = offset_entry;
			}
		}

		if (entries_by_path.has(entry.path)) {
			return fail(ERR_FILE_CORRUPT, "Duplicate entry path in archive: " + entry.path);
		}
		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash].push_back(entry);
	}

	if (is_v5) {
		const uint64_t entries_end_abs = toc_start + reader.get_cursor();
		if (entries_end_abs > loaded_header.data_offset) {
			return fail(ERR_FILE_CORRUPT, "v5 entry table extends past data_offset.");
		}
		const uint64_t toc_buffer_size = plaintext_toc.size();
		if (loaded_extension_size > 0) {
			uint64_t aligned_entries_end = 0;
			uint64_t extension_end = 0;
			if (!align_up(entries_end_abs, 8, aligned_entries_end) ||
					loaded_extension_offset < aligned_entries_end ||
					!checked_add(loaded_extension_offset, loaded_extension_size, extension_end) ||
					extension_end > loaded_header.data_offset ||
					loaded_extension_offset < toc_start) {
				return fail(ERR_FILE_CORRUPT, "v5 header extension placement is invalid.");
			}
			const uint64_t extension_index = loaded_extension_offset - toc_start;
			if (extension_index < reader.get_cursor() ||
					extension_index > toc_buffer_size ||
					loaded_extension_size > toc_buffer_size - extension_index) {
				return fail(ERR_FILE_CORRUPT, "v5 header extension is outside the TOC envelope.");
			}
			for (uint64_t p = reader.get_cursor(); p < extension_index; ++p) {
				if (plaintext_toc.ptr()[p] != 0) {
					return fail(ERR_FILE_CORRUPT, "v5 TOC alignment padding must be zero.");
				}
			}
			String tlv_error;
			if (!validate_tlv_table(plaintext_toc.ptr() + extension_index, loaded_extension_size,
						DS_HEADER_TLV_MAX_TOTAL, "Header", header_tlv_summary, tlv_error)) {
				return fail(ERR_FILE_CORRUPT, tlv_error);
			}
			for (uint64_t p = extension_index + loaded_extension_size; p < toc_buffer_size; ++p) {
				if (plaintext_toc.ptr()[p] != 0) {
					return fail(ERR_FILE_CORRUPT, "v5 TOC trailing padding must be zero.");
				}
			}
		} else {
			if (loaded_extension_offset != 0) {
				return fail(ERR_FILE_CORRUPT, "v5 extension_offset must be zero when extension_size is zero.");
			}
			for (uint64_t p = reader.get_cursor(); p < toc_buffer_size; ++p) {
				if (plaintext_toc.ptr()[p] != 0) {
					return fail(ERR_FILE_CORRUPT, "v5 TOC padding must be zero.");
				}
			}
		}

		const bool has_crypto_tlv = header_tlv_summary.has_crypto || entry_tlv_summary.has_crypto;
		const bool has_archive_set_tlv = header_tlv_summary.has_archive_set || entry_tlv_summary.has_archive_set;
		struct CapabilityUse {
			uint64_t bit;
			bool used;
		} capability_uses[] = {
			{ DSPKG_CAP_ZSTD, used_zstd },
			{ DSPKG_CAP_GACL, used_gacl },
			{ DSPKG_CAP_CHUNKED, used_chunked },
			{ DSPKG_CAP_CONTENT_HASH, used_content_hash },
			{ DSPKG_CAP_MESH, used_mesh },
			{ DSPKG_CAP_CRYPTO_V2, has_crypto_tlv },
			{ DSPKG_CAP_MULTI_ARCHIVE, has_archive_set_tlv },
		};
		for (const CapabilityUse &use : capability_uses) {
			if (((loaded_capabilities & use.bit) != 0) != use.used) {
				return fail(ERR_FILE_CORRUPT, "v5 capabilities do not match the archive contents.");
			}
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
	if (loaded_header.version == DSPKG_VERSION_V5) {
		if (entry.entry_kind != static_cast<uint8_t>(DSPkgEntryKind::TEXTURE)) {
			UtilityFunctions::push_error("DSPkg: Entry is not a texture: ", p_resource_path);
			return result;
		}
		// The CPU/read_texture path only decodes GDeflate, single-frame,
		// non-shuffled payloads until Step 5. Never reinterpret Zstd/GACL bytes
		// as GDeflate and never ignore a chunk table.
		if (entry.compression_format != static_cast<uint8_t>(DSCompressionFormat::GDEFLATE) ||
				entry.gacl_transform != static_cast<uint8_t>(DSGaclTransform::NONE) ||
				entry.chunked) {
			UtilityFunctions::push_error("DSPkg: v5 Zstd/GACL/chunked entries require the GPU path or Step 5 CPU support: ", p_resource_path);
			return result;
		}
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
	result["logical_width"] = entry.logical_width > 0 ? entry.logical_width : entry.width;
	result["logical_height"] = entry.logical_height > 0 ? entry.logical_height : entry.height;
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
	result["format_version"] = loaded_header.version;
	result["data_offset"] = data_offset;
	result["compressed_size"] = entry.compressed_size;
	result["original_size"] = entry.original_size;
	result["width"] = entry.width;
	result["height"] = entry.height;
	result["logical_width"] = entry.logical_width > 0 ? entry.logical_width : entry.width;
	result["logical_height"] = entry.logical_height > 0 ? entry.logical_height : entry.height;
	result["pixel_format"] = entry.pixel_format;
	result["mip_count"] = entry.mip_count;
	result["flags"] = entry.flags;
	result["entry_kind"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int>(entry.entry_kind) : static_cast<int>(DSPkgEntryKind::TEXTURE);
	result["compression_format"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int>(entry.compression_format) : static_cast<int>(DSCompressionFormat::GDEFLATE);
	result["gacl_transform"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int>(entry.gacl_transform) : static_cast<int>(DSGaclTransform::NONE);
	result["descriptor_flags"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int>(entry.descriptor_flags) : 0;
	result["metadata_size"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int64_t>(entry.metadata_size) : 0;
	result["extensions_size"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int64_t>(entry.extensions_size) : 0;
	result["capabilities"] = loaded_header.version == DSPKG_VERSION_V5 ?
			static_cast<int64_t>(loaded_capabilities) : 0;
	result["has_gpu_metadata"] = entry.has_gpu_metadata;
	result["is_srgb"] = ((entry.flags & 1U) != 0) || DSTex::is_rd_format_srgb(entry.pixel_format);
	result["as_image_texture"] = (entry.flags & 2U) != 0;
	result["mip_offsets"] = entry.mip_offsets;
	result["mip_sizes"] = entry.mip_sizes;
	result["mip_uncompressed_sizes"] = entry.mip_uncompressed_sizes;
	result["mip_row_pitches"] = entry.mip_row_pitches;
	if (loaded_header.version == DSPKG_VERSION_V5) {
		result["mip_chunk_counts"] = entry.mip_chunk_counts;
		result["chunk_offsets"] = entry.chunk_offsets;
		result["chunk_compressed_sizes"] = entry.chunk_compressed_sizes;
		result["chunk_uncompressed_sizes"] = entry.chunk_uncompressed_sizes;
		result["chunked"] = entry.chunked;
		result["total_chunk_count"] = static_cast<int64_t>(entry.total_chunk_count);
	} else {
		PackedInt32Array chunk_counts;
		chunk_counts.resize(entry.mip_count);
		for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
			chunk_counts[mip] = 1;
		}
		result["mip_chunk_counts"] = chunk_counts;
		result["chunk_offsets"] = PackedInt64Array();
		result["chunk_compressed_sizes"] = PackedInt32Array();
		result["chunk_uncompressed_sizes"] = PackedInt32Array();
		result["chunked"] = false;
		result["total_chunk_count"] = static_cast<int64_t>(entry.mip_count);
	}

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
			entry->mip_row_pitches.size() != static_cast<int64_t>(entry->mip_count) ||
			(loaded_header.version == DSPKG_VERSION_V5 &&
					entry->mip_chunk_counts.size() != static_cast<int64_t>(entry->mip_count))) {
		return false;
	}

	r_entries.resize(entry->mip_count);
	for (uint32_t mip = 0; mip < entry->mip_count; ++mip) {
		DSPkgMipEntry &mip_entry = r_entries.write[mip];
		mip_entry.offset = static_cast<uint64_t>(entry->mip_offsets[mip]);
		mip_entry.compressed_size = static_cast<uint32_t>(entry->mip_sizes[mip]);
		mip_entry.uncompressed_size = static_cast<uint32_t>(entry->mip_uncompressed_sizes[mip]);
		mip_entry.row_pitch = static_cast<uint32_t>(entry->mip_row_pitches[mip]);
		if (loaded_header.version == DSPKG_VERSION_V5) {
			mip_entry.chunk_count = static_cast<uint32_t>(entry->mip_chunk_counts[mip]);
		} else {
			mip_entry.reserved = 0;
		}
	}
	return true;
}

bool DSPkg::get_mip_entries_for_offset(uint64_t p_offset, Vector<DSPkgMipEntry> &r_entries) const {
	r_entries.clear();
	if (!is_opened) {
		return false;
	}
	const DSPkgOffsetEntry *found = entries_by_offset.getptr(p_offset);
	if (found == nullptr) {
		return false;
	}
	r_entries = found->mips;
	return true;
}

bool DSPkg::get_entry_features_for_offset(uint64_t p_offset, uint8_t &r_compression_format,
		uint8_t &r_gacl_transform, bool &r_chunked) const {
	if (!is_opened) {
		return false;
	}
	const DSPkgOffsetEntry *found = entries_by_offset.getptr(p_offset);
	if (found == nullptr) {
		return false;
	}
	r_compression_format = found->compression_format;
	r_gacl_transform = found->gacl_transform;
	r_chunked = found->chunked;
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
		mip_dict["chunk_count"] = loaded_header.version == DSPKG_VERSION_V5 ?
				static_cast<int64_t>(mip_entry.chunk_count) : 1;
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
