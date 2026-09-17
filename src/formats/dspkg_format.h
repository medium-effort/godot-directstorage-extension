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

#include <cstdint>

namespace godot {

#pragma pack(push, 1)
struct DSPkgHeader {
	uint32_t magic;       // 0x474B5044 ("DPKG" little-endian)
	uint16_t version;     // 1 for legacy archives, 2 for GPU mip metadata
	uint16_t flags;       // DSPKG_FLAG_GPU_MIP_METADATA for version 2
	uint32_t entry_count; // Number of entries
	uint64_t data_offset; // Byte offset to start of data blob
	uint64_t reserved;    // 0
};
#pragma pack(pop)

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
	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	bool has_gpu_metadata = false;
};

class DSPkg : public Resource {
	GDCLASS(DSPkg, Resource);

private:
	// Builder state
	String build_output_path;
	Array staging_entries;
	bool is_building = false;

	// Reader state
	String opened_path;
	DSPkgHeader loaded_header{};
	HashMap<String, DSPkgEntry> entries_by_path;
	HashMap<uint64_t, Vector<DSPkgEntry>> entries_by_hash;
	HashMap<uint64_t, Vector<DSPkgMipEntry>> entries_by_offset;
	bool is_opened = false;

	const DSPkgEntry *find_entry_for_path(const String &p_resource_path) const;

protected:
	static void _bind_methods();

public:
	DSPkg();
	~DSPkg();

	// Archive Builder
	Error create(const String &p_output_path);
	Error add_texture(const String &p_resource_path, const String &p_dstex_path, const Dictionary &p_mip_metadata = Dictionary());
	Error finalize();

	// Archive Reader
	Error open(const String &p_path);
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
