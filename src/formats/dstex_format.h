#ifndef DSTEX_FORMAT_H
#define DSTEX_FORMAT_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

namespace godot {

#pragma pack(push, 1)
struct DSTexHeader {
	uint32_t magic;           // 0x58455444 ("DTEX" little-endian)
	uint16_t version;         // 1
	uint16_t flags;           // 0
	uint32_t width;
	uint32_t height;
	uint32_t pixel_format;    // RenderingDevice::DataFormat enum value
	uint32_t mip_count;
	uint64_t original_size;   // uncompressed total bytes
	uint64_t compressed_size; // compressed total bytes
	uint64_t reserved;        // 0
};

struct DSTexMipEntry {
	uint64_t offset; // byte offset from file start
	uint32_t size;   // compressed mip size in bytes
};

struct DSTexMipEntryV2 {
	uint64_t offset;            // byte offset from file start to compressed mip data
	uint32_t compressed_size;   // compressed mip payload size
	uint32_t uncompressed_size; // footprint-aligned uncompressed size in bytes
	uint32_t row_pitch;         // D3D12 256-byte aligned row pitch
	uint32_t reserved;          // reserved alignment padding
};
#pragma pack(pop)

static_assert(sizeof(DSTexMipEntryV2) == 24, "DSTexMipEntryV2 must be 24 bytes");

// Defensive parsing limits (C8 remediation):
// - Maximum mip count is 16 (covers up to 64k texture resolution, prevents unbounded allocations).
// - Mip table must satisfy: sizeof(DSTexHeader) + mip_count * entry_size <= file_length.
// - For v1, data_offset + compressed_size <= file_length.
// - For v2, each mip_offsets[i] + mip_sizes[i] <= file_length.
static constexpr uint32_t DSTEX_MAX_MIP_COUNT = 16;

struct DSTexMipLayout {
	uint64_t source_row_bytes = 0;
	uint64_t row_count = 0;
	uint64_t row_pitch = 0;
	uint64_t footprint_size = 0;
	uint64_t source_size = 0;
};

class DSTex : public RefCounted {
	GDCLASS(DSTex, RefCounted);

protected:
	static void _bind_methods();

public:
	DSTex();
	~DSTex();

	Error write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count = 1, int p_compression_level = 1, bool p_is_srgb = false);
	Dictionary read_header(const String &p_path);
	Dictionary read_from_file(const String &p_path);
	Dictionary get_file_info(const String &p_path);

	static DSTexMipLayout get_mip_layout(uint32_t p_width, uint32_t p_height, int p_image_format);
	static PackedByteArray unpad_footprint_mip(const PackedByteArray &p_footprint, uint32_t p_width, uint32_t p_height, int p_image_format);

	static int godot_format_to_rd_format(int p_image_format, bool p_is_srgb = false);
	static int rd_format_to_godot_format(int p_rd_format);
};

} // namespace godot

#endif // DSTEX_FORMAT_H
