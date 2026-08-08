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
#pragma pack(pop)

class DSTex : public RefCounted {
	GDCLASS(DSTex, RefCounted);

protected:
	static void _bind_methods();

public:
	DSTex();
	~DSTex();

	Error write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count = 1, int p_compression_level = 1);
	Dictionary read_header(const String &p_path);
	Dictionary read_from_file(const String &p_path);
	Dictionary get_file_info(const String &p_path);

	static int godot_format_to_rd_format(int p_image_format);
	static int rd_format_to_godot_format(int p_rd_format);
};

} // namespace godot

#endif // DSTEX_FORMAT_H
