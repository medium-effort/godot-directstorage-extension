#ifndef DSPKG_FORMAT_H
#define DSPKG_FORMAT_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace godot {

constexpr uint16_t DSPKG_VERSION_LEGACY = 1;
constexpr uint16_t DSPKG_VERSION_GPU_METADATA = 2;
constexpr uint16_t DSPKG_VERSION_ENTRY_FLAGS = 3;
constexpr uint16_t DSPKG_VERSION_V4 = 4;

constexpr uint16_t DSPKG_FLAG_GPU_MIP_METADATA = (1 << 0);
constexpr uint16_t DSPKG_FLAG_ENCRYPTED_TOC = (1 << 1);

constexpr uint64_t DSPKG_ENTRY_PREFIX_SIZE = 10;
constexpr uint64_t DSPKG_ENTRY_FIELDS_SIZE_LEGACY = 40;
constexpr uint64_t DSPKG_ENTRY_FIELDS_SIZE_V3 = 44;
constexpr uint64_t DSPKG_FIXED_ENTRY_SIZE_LEGACY = 50;
constexpr uint64_t DSPKG_FIXED_ENTRY_SIZE_V3 = 54;

constexpr uint64_t DSPKG_MAX_TOC_SIZE = 16ULL * 1024ULL * 1024ULL;
constexpr uint64_t DSPKG_HEADER_SIZE_LEGACY = 28;
constexpr uint64_t DSPKG_HEADER_SIZE_V4 = 64;
constexpr uint64_t DSPKG_ENCRYPTION_MAC_SIZE = 16;
constexpr uint64_t DSPKG_ENCRYPTION_SALT_SIZE = 16;
constexpr uint64_t DSPKG_ENCRYPTION_NONCE_SIZE = 24;

#pragma pack(push, 1)
struct DSPkgHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t flags;
	uint32_t entry_count;
	uint64_t data_offset;
	uint64_t archive_fingerprint; // archive id, compared as a full 64-bit value
	uint32_t compression_codec;
	uint8_t archive_salt[16];
	uint32_t min_reader_version;
	uint64_t key_id;
	uint8_t reserved[4];
};
#pragma pack(pop)

static_assert(sizeof(DSPkgHeader) == 64, "DSPkgHeader v4 must be exactly 64 bytes");
static_assert(offsetof(DSPkgHeader, compression_codec) == 28, "compression_codec offset mismatch");
static_assert(offsetof(DSPkgHeader, archive_salt) == 32, "archive_salt offset mismatch");
static_assert(offsetof(DSPkgHeader, min_reader_version) == 48, "min_reader_version offset mismatch");
static_assert(offsetof(DSPkgHeader, key_id) == 52, "key_id offset mismatch");
static_assert(offsetof(DSPkgHeader, reserved) == 60, "reserved offset mismatch");

#pragma pack(push, 1)
struct DSPkgMipEntry {
	uint64_t offset;            // byte offset from archive start to compressed mip data
	uint32_t compressed_size;   // compressed mip payload size
	uint32_t uncompressed_size; // D3D12 footprint size in bytes
	uint32_t row_pitch;         // D3D12 256-byte aligned row pitch
	uint32_t reserved;          // reserved alignment padding
};
#pragma pack(pop)

static_assert(sizeof(DSPkgMipEntry) == 24, "DSPkgMipEntry must match DSTexMipEntryV2");

struct DSPkgEntry {
	String path;
	uint64_t path_hash = 0;
	uint64_t data_offset = 0;
	uint64_t compressed_size = 0;
	uint64_t original_size = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pixel_format = 0;
	uint32_t mip_count = 0;
	uint32_t flags = 0;
	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	bool has_gpu_metadata = false;
};

// Builder staging entry: metadata captured by add_texture() only. The payload
// is never buffered; finalize() re-stats the source and streams it in bounded
// chunks, so the builder's footprint is the entry metadata plus one payload
// instead of the sum of every staged texture.
struct DSPkgStagedEntry {
	String resource_path; // archive entry name (resource or alias path)
	String dstex_path;    // source .dstex path
	uint64_t source_file_length = 0;
	uint64_t source_modified_time = 0;
	uint16_t source_version = 0;          // .dstex container version, selects the payload layout
	uint64_t source_compressed_size = 0;  // header compressed_size at add time
	uint64_t payload_size = 0;            // bytes the payload occupies, as counted at add time
	uint64_t payload_offset = 0;          // v1 only: start of the contiguous payload inside the .dstex
	uint64_t original_size = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pixel_format = 0;
	uint32_t mip_count = 0;
	uint32_t flags = 0;
	bool gpu_metadata = false;
	uint64_t gpu_footprint = 0;           // sum of mip uncompressed sizes
	PackedInt64Array source_mip_offsets;  // per-mip payload offsets inside the .dstex
	PackedInt32Array source_mip_sizes;    // per-mip payload sizes inside the .dstex
	PackedInt32Array mip_sizes;           // TOC compressed mip sizes
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
};

class DSPkg : public Resource {
	GDCLASS(DSPkg, Resource);

private:
	// Builder state
	String build_output_path;
	uint64_t build_archive_fingerprint = 0;
	uint64_t payload_alignment = 512;
	bool bin_packing_enabled = false;
	std::vector<DSPkgStagedEntry> staging_entries;
	bool is_building = false;
	bool m_encryption_enabled = false;
	std::array<uint8_t, 32> m_encryption_key = {};
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	PackedByteArray m_test_salt_override;
	// -1 disables the injection. A non-negative value silently drops archive
	// writes past this byte offset while the builder still reports success, so
	// the final size verification can be exercised without a full disk.
	int64_t m_test_write_truncate_after_bytes = -1;
#endif

	// Reader state
	String opened_path;
	DSPkgHeader loaded_header{};
	HashMap<String, DSPkgEntry> entries_by_path;
	HashMap<uint64_t, Vector<DSPkgEntry>> entries_by_hash;
	HashMap<uint64_t, Vector<DSPkgMipEntry>> entries_by_offset;
	bool is_opened = false;

	const DSPkgEntry *find_entry_for_path(const String &p_resource_path) const;

	// Installs a key that open() has already proven: the candidate reproduced the
	// archive header's key_id and decrypted the TOC MAC. No other caller exists,
	// so an unvalidated candidate can never become the active key.
	static void set_global_encryption_key_raw(const uint8_t *p_key_bytes, size_t p_size);

protected:
	static void _bind_methods();

public:
	static constexpr uint64_t DEFAULT_PAYLOAD_ALIGNMENT = 512;
	static constexpr uint64_t STEAMPIPE_1MB_ALIGNMENT = 1048576;

	DSPkg();
	~DSPkg();

	// Archive Builder
	Error create(const String &p_output_path, uint64_t p_archive_fingerprint = 0);
	void close_write();
	void set_archive_fingerprint(uint64_t p_archive_fingerprint);
	void set_payload_alignment(uint64_t p_alignment);
	uint64_t get_payload_alignment() const;
	void set_bin_packing_enabled(bool p_enabled);
	bool is_bin_packing_enabled() const;
	Error add_texture(const String &p_resource_path, const String &p_dstex_path, const Dictionary &p_mip_metadata = Dictionary());
	Error set_encryption_key(const String &p_key_hex);
	Error finalize();

	// Runtime static key API
	static Error set_global_encryption_key(const String &p_key_hex);
	static String get_global_encryption_key_id();
	static String compute_key_id(const String &p_key_hex);

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
public:
	void _set_archive_salt_for_testing(const PackedByteArray &p_salt);
	void _set_write_truncate_for_testing(int64_t p_bytes);
#endif

	// Archive Reader
	Error open(const String &p_path, uint64_t p_expected_id = 0);
	uint64_t get_archive_fingerprint() const;
	bool has_texture(const String &p_resource_path) const;
	Dictionary read_texture(const String &p_resource_path) const;
	Dictionary get_entry_info(const String &p_resource_path) const;
	bool get_mip_entries_for_path(const String &p_path, Vector<DSPkgMipEntry> &r_entries) const;
	bool get_mip_entries_for_offset(uint64_t p_offset, Vector<DSPkgMipEntry> &r_entries) const;
	Array get_mip_entries_for_offset(uint64_t p_offset) const;
	int get_entry_count() const;
	Array get_entries() const;
};

} // namespace godot

#endif // DSPKG_FORMAT_H
