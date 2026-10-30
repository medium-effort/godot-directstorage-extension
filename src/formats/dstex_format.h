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

enum DSTexFlags {
	DSTEX_FLAG_SRGB = 1 << 0,
	DSTEX_FLAG_AS_IMAGE_TEXTURE = 1 << 1,
};

#pragma pack(push, 1)
struct DSTexHeader {
	uint32_t magic;           // 0x58455444 ("DTEX" little-endian)
	uint16_t version;         // 1
	uint16_t flags;           // bit 0: sRGB, bit 1: as ImageTexture (projector/decal compatible)
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

// Defensive parsing limits (C8 remediation + FMT-01 hardening):
// - Maximum mip count is 16 (covers up to 64k texture resolution, prevents unbounded allocations).
// - Mip table must satisfy: sizeof(DSTexHeader) + mip_count * entry_size <= file_length.
// - For v1, data_offset + compressed_size <= file_length.
// - For v2, each mip_offsets[i] + mip_sizes[i] <= file_length.
// - FMT-01: dimensions and total bytes are additionally capped so a hostile
//   header claiming 2 GiB cannot force a huge transient allocation. The byte
//   cap matches MAX_PREWARM_REQUEST_BYTES (512 MiB) in ds_preloader.cpp.
static constexpr uint32_t DSTEX_MAX_MIP_COUNT = 16;
static constexpr uint32_t DSTEX_MAX_DIMENSION = 16384;
static constexpr uint64_t DSTEX_MAX_TOTAL_BYTES = 512ULL * 1024ULL * 1024ULL;

// Generous upper bound: 16 bytes/px (RGBA32F) x 2 for mips + footprint padding.
// The padding term is load-bearing, not slack: every mip row is rounded up to a
// 256-byte pitch, and a full chain contributes at most 2 x height rows, so a
// legal small or narrow texture costs more than px*32 (an 8x8 RGBA8 chain is
// 3840 bytes against px*32 = 2048). Without it the writer's own output is
// refused by every reader. v2 files are additionally held to their exact
// per-level footprint, which this heuristic cannot express.
inline bool dstex_sizes_are_sane(uint32_t p_width, uint32_t p_height, uint64_t p_original, uint64_t p_compressed) {
	if (p_width == 0 || p_height == 0 || p_width > DSTEX_MAX_DIMENSION || p_height > DSTEX_MAX_DIMENSION) {
		return false;
	}
	if (p_original == 0 || p_original > DSTEX_MAX_TOTAL_BYTES) {
		return false;
	}
	if (p_compressed == 0 || p_compressed > DSTEX_MAX_TOTAL_BYTES) {
		return false;
	}
	uint64_t px = static_cast<uint64_t>(p_width) * static_cast<uint64_t>(p_height);
	if (px > UINT64_MAX / 32ULL) {
		return false;
	}
	const uint64_t natural_allowance = px * 32ULL;
	// Rows over a full chain sum to at most 2 x height; each can waste up to 255
	// bytes of pitch padding, so 512 x height is a safe bound for all of them.
	const uint64_t padding_allowance = 512ULL * static_cast<uint64_t>(p_height);
	if (p_original > natural_allowance + padding_allowance) {
		return false;
	}
	return true;
}

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

	Error write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count = 1, int p_compression_level = 1, bool p_is_srgb = false, bool p_as_image_texture = false);
	Dictionary read_header(const String &p_path);
	Dictionary read_from_file(const String &p_path);
	/// Convenience alias for read_header; returns the same header metadata dictionary.
	Dictionary get_file_info(const String &p_path);

	static DSTexMipLayout get_mip_layout(uint32_t p_width, uint32_t p_height, int p_image_format);
	static PackedByteArray unpad_footprint_mip(const PackedByteArray &p_footprint, uint32_t p_width, uint32_t p_height, int p_image_format);

	static bool is_format_supported(int p_image_format);
	static int godot_format_to_rd_format(int p_image_format, bool p_is_srgb = false);
	static int rd_format_to_godot_format(int p_rd_format);
	static bool is_rd_format_srgb(int p_rd_format);
	static int get_srgb_rd_format(int p_rd_format);
};

} // namespace godot

#endif // DSTEX_FORMAT_H
