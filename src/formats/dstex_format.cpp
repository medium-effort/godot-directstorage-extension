#include "dstex_format.h"
#include "compression/gdeflate_compressor.h"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace godot {

namespace {

constexpr uint64_t D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256;
constexpr uint64_t D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512;

bool align_up(uint64_t p_value, uint64_t p_alignment, uint64_t &r_result) {
	const uint64_t remainder = p_value % p_alignment;
	const uint64_t padding = remainder == 0 ? 0 : p_alignment - remainder;
	if (p_value > std::numeric_limits<uint64_t>::max() - padding) {
		return false;
	}
	r_result = p_value + padding;
	return true;
}

} // namespace

DSTexMipLayout DSTex::get_mip_layout(uint32_t p_width, uint32_t p_height, int p_format) {
	if (p_width == 0 || p_height == 0) {
		return DSTexMipLayout{};
	}

	uint64_t row_bytes = 0;
	uint64_t row_count = p_height;
	uint32_t block_bytes = 0;

	switch (p_format) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			row_bytes = p_width;
			break;
		case Image::FORMAT_LA8:
		case Image::FORMAT_RG8:
			row_bytes = static_cast<uint64_t>(p_width) * 2;
			break;
		case Image::FORMAT_RGB8:
			row_bytes = static_cast<uint64_t>(p_width) * 3;
			break;
		case Image::FORMAT_RGBA8:
			row_bytes = static_cast<uint64_t>(p_width) * 4;
			break;
		case Image::FORMAT_RF:
			row_bytes = static_cast<uint64_t>(p_width) * 4;
			break;
		case Image::FORMAT_RGF:
			row_bytes = static_cast<uint64_t>(p_width) * 8;
			break;
		case Image::FORMAT_RGBF:
			row_bytes = static_cast<uint64_t>(p_width) * 12;
			break;
		case Image::FORMAT_RGBAF:
			row_bytes = static_cast<uint64_t>(p_width) * 16;
			break;
		case Image::FORMAT_RH:
			row_bytes = static_cast<uint64_t>(p_width) * 2;
			break;
		case Image::FORMAT_RGH:
			row_bytes = static_cast<uint64_t>(p_width) * 4;
			break;
		case Image::FORMAT_RGBH:
			row_bytes = static_cast<uint64_t>(p_width) * 6;
			break;
		case Image::FORMAT_RGBAH:
			row_bytes = static_cast<uint64_t>(p_width) * 8;
			break;
		case Image::FORMAT_R16:
			row_bytes = static_cast<uint64_t>(p_width) * 2;
			break;
		case Image::FORMAT_RG16:
			row_bytes = static_cast<uint64_t>(p_width) * 4;
			break;
		case Image::FORMAT_RGB16:
			row_bytes = static_cast<uint64_t>(p_width) * 6;
			break;
		case Image::FORMAT_RGBA16:
			row_bytes = static_cast<uint64_t>(p_width) * 8;
			break;
		case Image::FORMAT_DXT1:
		case Image::FORMAT_RGTC_R:
			block_bytes = 8;
			break;
		case Image::FORMAT_DXT3:
		case Image::FORMAT_DXT5:
		case Image::FORMAT_RGTC_RG:
		case Image::FORMAT_BPTC_RGBA:
		case Image::FORMAT_BPTC_RGBF:
		case Image::FORMAT_BPTC_RGBFU:
			block_bytes = 16;
			break;
		default:
			UtilityFunctions::push_warning("DSTex: Unsupported image format for mip layout: ", p_format);
			return DSTexMipLayout{};
	}

	if (block_bytes != 0) {
		const uint64_t block_width = std::max<uint64_t>(1, (static_cast<uint64_t>(p_width) + 3) / 4);
		const uint64_t block_height = std::max<uint64_t>(1, (static_cast<uint64_t>(p_height) + 3) / 4);
		row_bytes = block_width * block_bytes;
		row_count = block_height;
	}

	DSTexMipLayout layout;
	layout.source_row_bytes = row_bytes;
	layout.row_count = row_count;
	if (!align_up(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, layout.row_pitch) ||
			layout.row_pitch > std::numeric_limits<uint32_t>::max() ||
			layout.row_pitch > std::numeric_limits<uint64_t>::max() / row_count) {
		return DSTexMipLayout{};
	}
	layout.footprint_size = layout.row_pitch * row_count;
	layout.source_size = row_bytes * row_count;
	return layout;
}

// v2 mip-table contract, shared by read_header() and read_from_file().
//
// Three things must hold, and the third is what the header-level heuristic in
// dstex_sizes_are_sane() cannot express:
//   1. every entry declares a plausible uncompressed footprint (non-zero and
//      within the total-byte cap), so no uint32 -> int32 underflow reaches
//      mip_uncompressed_sizes and no single mip can request gigabytes;
//   2. the table adds up to the header totals, so a header cannot declare a
//      small original_size while its mips claim gigabytes of destination;
//   3. when the pixel format maps to a known image format, each entry equals the
//      padded footprint of its own mip level. That is the v2 replacement for the
//      width*height*32 aggregate: the 256-byte row pitch makes a full chain cost
//      more than px*32 for small or narrow textures, while the exact per-level
//      comparison still rejects a table that does not describe the declared
//      dimensions.
bool DSTex::mip_table_is_sane(const DSTexHeader &p_header, const DSTexMipEntryV2 *p_entries) {
	const int img_fmt = DSTex::rd_format_to_godot_format(static_cast<int>(p_header.pixel_format));
	const bool check_layout = img_fmt != Image::FORMAT_MAX;
	uint32_t w = p_header.width;
	uint32_t h = p_header.height;
	uint64_t sum_compressed = 0;
	uint64_t sum_uncompressed = 0;
	for (uint32_t i = 0; i < p_header.mip_count; ++i) {
		const DSTexMipEntryV2 &entry = p_entries[i];
		if (entry.uncompressed_size == 0 || entry.uncompressed_size > DSTEX_MAX_TOTAL_BYTES) {
			return false;
		}
		if (check_layout) {
			const DSTexMipLayout layout = DSTex::get_mip_layout(w, h, img_fmt);
			// A zero footprint means the layout is unknown for this format; the
			// sum and cap checks above still apply, so skip only the exactness.
			if (layout.footprint_size != 0 && layout.footprint_size != entry.uncompressed_size) {
				return false;
			}
		}
		sum_compressed += entry.compressed_size;
		sum_uncompressed += entry.uncompressed_size;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	return sum_compressed == p_header.compressed_size && sum_uncompressed == p_header.original_size;
}

// Payload spans must be ordered, non-overlapping, and start at or after the
// aligned end of the header + mip table. This mirrors the Win32 enqueue path and
// prevents an offset-0 or overlapping mip from aliasing the container metadata.
static bool ds_v2_mip_payload_order_is_sane(const DSTexHeader &p_header, const DSTexMipEntryV2 *p_entries, String &r_error) {
	uint64_t first_payload = dstex_header_size(p_header.version) +
			static_cast<uint64_t>(p_header.mip_count) * sizeof(DSTexMipEntryV2);
	if (!align_up(first_payload, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, first_payload)) {
		r_error = "DSTex: Mip table end alignment overflow.";
		return false;
	}
	uint64_t previous_end = first_payload;
	for (uint32_t i = 0; i < p_header.mip_count; ++i) {
		const DSTexMipEntryV2 &entry = p_entries[i];
		if (entry.offset < previous_end ||
				entry.offset > UINT64_MAX - entry.compressed_size) {
			r_error = "DSTex: Mip payloads overlap or start inside the header/tables.";
			return false;
		}
		previous_end = entry.offset + entry.compressed_size;
	}
	return true;
}

static bool ds_v1_mip_payload_order_is_sane(const DSTexHeader &p_header, const DSTexMipEntry *p_entries, String &r_error) {
	uint64_t previous_end = sizeof(DSTexHeader) +
			static_cast<uint64_t>(p_header.mip_count) * sizeof(DSTexMipEntry);
	for (uint32_t i = 0; i < p_header.mip_count; ++i) {
		const DSTexMipEntry &entry = p_entries[i];
		if (entry.offset < previous_end || entry.offset > UINT64_MAX - entry.size) {
			r_error = "DSTex: v1 mip payloads overlap or start inside the header/tables.";
			return false;
		}
		previous_end = entry.offset + entry.size;
	}
	return true;
}

// Reads the v3 chunk tables that follow the mip table, validates them, and
// returns the flat chunk arrays. The caller must have positioned the file at
// sizeof(DSTexHeader) + mip_count * sizeof(DSTexMipEntryV2).
static bool ds_parse_v3_chunks(Ref<FileAccess> p_file, const DSTexHeader &p_header,
		const DSTexMipEntryV2 *p_entries, PackedInt32Array &r_chunk_counts,
		PackedInt64Array &r_offsets, PackedInt32Array &r_compressed,
		PackedInt32Array &r_uncompressed, bool &r_chunked, String &r_error) {
	const uint64_t file_len = p_file->get_length();
	r_chunked = false;
	uint64_t previous_chunk_end = 0;
	for (uint32_t mip = 0; mip < p_header.mip_count; ++mip) {
		const DSTexMipEntryV2 &mip_entry = p_entries[mip];
		const uint32_t chunk_count = mip_entry.chunk_count;
		if (chunk_count == 0 || chunk_count > DSTEX_MAX_CHUNKS_PER_MIP) {
			r_error = "DSTex: Invalid v3 chunk count.";
			return false;
		}
		if (mip_entry.offset % 512 != 0) {
			r_error = "DSTex: v3 mip offset must be 512-byte aligned.";
			return false;
		}
		r_chunk_counts.push_back(static_cast<int32_t>(chunk_count));
		if (chunk_count == 1) {
			continue;
		}
		r_chunked = true;
		if (mip_entry.row_pitch == 0 || mip_entry.uncompressed_size % mip_entry.row_pitch != 0) {
			r_error = "DSTex: v3 chunked mip is not a whole number of rows.";
			return false;
		}
		uint64_t sum_compressed = 0;
		uint64_t sum_uncompressed = 0;
		for (uint32_t chunk_index = 0; chunk_index < chunk_count; ++chunk_index) {
			if (p_file->get_position() > file_len ||
					file_len - p_file->get_position() < sizeof(DSChunkEntry)) {
				r_error = "DSTex: v3 chunk table extends past file.";
				return false;
			}
			PackedByteArray bytes = p_file->get_buffer(sizeof(DSChunkEntry));
			if (bytes.size() != static_cast<int64_t>(sizeof(DSChunkEntry))) {
				r_error = "DSTex: Failed to read v3 chunk entry.";
				return false;
			}
			DSChunkEntry chunk;
			memcpy(&chunk, bytes.ptr(), sizeof(chunk));
			if (chunk.compressed_size == 0 || chunk.uncompressed_size == 0 ||
					chunk.uncompressed_size % mip_entry.row_pitch != 0) {
				r_error = "DSTex: Invalid v3 chunk size or row alignment.";
				return false;
			}
			if (DSTex::get_compression_format(p_header.reserved) == DSCompressionFormat::NONE &&
					chunk.compressed_size != chunk.uncompressed_size) {
				r_error = "DSTex: NONE-compressed chunk sizes must match.";
				return false;
			}
			if (chunk.offset > file_len || chunk.compressed_size > file_len - chunk.offset) {
				r_error = "DSTex: v3 chunk exceeds file bounds.";
				return false;
			}
			if (chunk_index == 0 && chunk.offset != mip_entry.offset) {
				r_error = "DSTex: v3 first chunk must match the mip offset.";
				return false;
			}
			if (chunk.offset % (chunk_index == 0 ? 512 : 8) != 0) {
				r_error = "DSTex: v3 chunk offset alignment mismatch.";
				return false;
			}
			if (chunk.offset < previous_chunk_end) {
				r_error = "DSTex: v3 chunks overlap.";
				return false;
			}
			previous_chunk_end = chunk.offset + chunk.compressed_size;
			sum_compressed += chunk.compressed_size;
			sum_uncompressed += chunk.uncompressed_size;
			r_offsets.push_back(static_cast<int64_t>(chunk.offset));
			r_compressed.push_back(static_cast<int32_t>(chunk.compressed_size));
			r_uncompressed.push_back(static_cast<int32_t>(chunk.uncompressed_size));
		}
		if (sum_compressed != mip_entry.compressed_size || sum_uncompressed != mip_entry.uncompressed_size) {
			r_error = "DSTex: v3 chunk sums do not match the mip.";
			return false;
		}
	}
	if ((DSTex::is_chunked(p_header.reserved)) != r_chunked) {
		r_error = "DSTex: v3 CAP_CHUNKED does not match the mip table.";
		return false;
	}
	uint64_t first_payload = p_file->get_position();
	if (!align_up(first_payload, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, first_payload)) {
		r_error = "DSTex: v3 payload alignment overflow.";
		return false;
	}
	uint64_t previous_end = first_payload;
	int64_t chunk_cursor = 0;
	for (uint32_t mip = 0; mip < p_header.mip_count; ++mip) {
		const DSTexMipEntryV2 &mip_entry = p_entries[mip];
		if (mip_entry.chunk_count == 1) {
			if (mip_entry.offset < previous_end ||
					mip_entry.offset > UINT64_MAX - mip_entry.compressed_size) {
				r_error = "DSTex: v3 mip payloads overlap or start inside the chunk tables.";
				return false;
			}
			previous_end = mip_entry.offset + mip_entry.compressed_size;
			continue;
		}
		for (uint32_t chunk_index = 0; chunk_index < mip_entry.chunk_count; ++chunk_index) {
			const uint64_t chunk_offset = static_cast<uint64_t>(r_offsets[chunk_cursor]);
			const uint32_t chunk_compressed = static_cast<uint32_t>(r_compressed[chunk_cursor]);
			if (chunk_offset < previous_end ||
					chunk_offset > UINT64_MAX - chunk_compressed) {
				r_error = "DSTex: v3 chunk payloads overlap or start inside the chunk tables.";
				return false;
			}
			previous_end = chunk_offset + chunk_compressed;
			++chunk_cursor;
		}
	}
	if (chunk_cursor != r_offsets.size()) {
		r_error = "DSTex: v3 chunk cursor did not consume the table.";
		return false;
	}
	return true;
}


PackedByteArray DSTex::unpad_footprint_mip(const PackedByteArray &p_footprint, uint32_t p_width, uint32_t p_height, int p_image_format) {
	if (p_footprint.size() == 0 || p_width == 0 || p_height == 0) {
		return PackedByteArray();
	}
	DSTexMipLayout layout = get_mip_layout(p_width, p_height, p_image_format);
	if (layout.row_pitch == 0 || layout.source_row_bytes == 0 || layout.source_size == 0) {
		return p_footprint;
	}
	if (p_footprint.size() == static_cast<int64_t>(layout.source_size)) {
		return p_footprint;
	}
	if (p_footprint.size() != static_cast<int64_t>(layout.footprint_size)) {
		return p_footprint;
	}
	PackedByteArray unpadded;
	unpadded.resize(static_cast<int64_t>(layout.source_size));
	uint8_t *dst = unpadded.ptrw();
	const uint8_t *src = p_footprint.ptr();
	for (uint64_t r = 0; r < layout.row_count; ++r) {
		memcpy(dst + r * layout.source_row_bytes, src + r * layout.row_pitch, static_cast<size_t>(layout.source_row_bytes));
	}
	return unpadded;
}

void DSTex::normalize_image(const Ref<Image> &p_image) {
	if (p_image.is_null() || p_image->is_empty() || p_image->is_compressed()) {
		return;
	}
	const int width = p_image->get_width();
	const int height = p_image->get_height();
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			Color color = p_image->get_pixel(x, y);
			float nx = color.r * 2.0f - 1.0f;
			float ny = color.g * 2.0f - 1.0f;
			float nz = color.b * 2.0f - 1.0f;
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length > 0.0f) {
				nx /= length;
				ny /= length;
				nz /= length;
			} else {
				nx = 0.0f;
				ny = 0.0f;
				nz = 0.0f;
			}
			color.r = nx * 0.5f + 0.5f;
			color.g = ny * 0.5f + 0.5f;
			color.b = nz * 0.5f + 0.5f;
			p_image->set_pixel(x, y, color);
		}
	}
}

void DSTex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("write_to_file", "path", "raw_data", "width", "height", "format", "mip_count", "compression_level", "is_srgb", "as_image_texture"), &DSTex::write_to_file, DEFVAL(1), DEFVAL(1), DEFVAL(false), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("read_header", "path"), &DSTex::read_header);
	ClassDB::bind_method(D_METHOD("read_from_file", "path"), &DSTex::read_from_file);
	ClassDB::bind_method(D_METHOD("get_file_info", "path"), &DSTex::get_file_info);

	ClassDB::bind_static_method("DSTex", D_METHOD("is_format_supported", "image_format"), &DSTex::is_format_supported);
	ClassDB::bind_static_method("DSTex", D_METHOD("godot_format_to_rd_format", "image_format", "is_srgb"), &DSTex::godot_format_to_rd_format, DEFVAL(false));
	ClassDB::bind_static_method("DSTex", D_METHOD("rd_format_to_godot_format", "rd_format"), &DSTex::rd_format_to_godot_format);
	ClassDB::bind_static_method("DSTex", D_METHOD("is_rd_format_srgb", "rd_format"), &DSTex::is_rd_format_srgb);
	ClassDB::bind_static_method("DSTex", D_METHOD("get_srgb_rd_format", "rd_format"), &DSTex::get_srgb_rd_format);
	ClassDB::bind_static_method("DSTex", D_METHOD("normalize_image", "image"), &DSTex::normalize_image);
}

DSTex::DSTex() {
}

DSTex::~DSTex() {
}

bool DSTex::is_format_supported(int p_image_format) {
	switch (p_image_format) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
		case Image::FORMAT_LA8:
		case Image::FORMAT_RG8:
		case Image::FORMAT_RGB8:
		case Image::FORMAT_RGBA8:
		case Image::FORMAT_RF:
		case Image::FORMAT_RGF:
		case Image::FORMAT_RGBF:
		case Image::FORMAT_RGBAF:
		case Image::FORMAT_RH:
		case Image::FORMAT_RGH:
		case Image::FORMAT_RGBH:
		case Image::FORMAT_RGBAH:
		case Image::FORMAT_DXT1:
		case Image::FORMAT_DXT3:
		case Image::FORMAT_DXT5:
		case Image::FORMAT_RGTC_R:
		case Image::FORMAT_RGTC_RG:
		case Image::FORMAT_BPTC_RGBA:
		case Image::FORMAT_BPTC_RGBF:
		case Image::FORMAT_BPTC_RGBFU:
		case Image::FORMAT_R16:
		case Image::FORMAT_RG16:
		case Image::FORMAT_RGB16:
		case Image::FORMAT_RGBA16:
			return true;
		default:
			return false;
	}
}

int DSTex::godot_format_to_rd_format(int p_image_format, bool p_is_srgb) {
	int rd_format = RenderingDevice::DATA_FORMAT_MAX;
	switch (p_image_format) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			rd_format = RenderingDevice::DATA_FORMAT_R8_UNORM;
			break;
		case Image::FORMAT_LA8:
		case Image::FORMAT_RG8:
			rd_format = RenderingDevice::DATA_FORMAT_R8G8_UNORM;
			break;
		case Image::FORMAT_RGB8:
			rd_format = RenderingDevice::DATA_FORMAT_R8G8B8_UNORM;
			break;
		case Image::FORMAT_RGBA8:
			rd_format = RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
			break;
		case Image::FORMAT_RF:
			rd_format = RenderingDevice::DATA_FORMAT_R32_SFLOAT;
			break;
		case Image::FORMAT_RGF:
			rd_format = RenderingDevice::DATA_FORMAT_R32G32_SFLOAT;
			break;
		case Image::FORMAT_RGBF:
			rd_format = RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
			break;
		case Image::FORMAT_RGBAF:
			rd_format = RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT;
			break;
		case Image::FORMAT_RH:
			rd_format = RenderingDevice::DATA_FORMAT_R16_SFLOAT;
			break;
		case Image::FORMAT_RGH:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16_SFLOAT;
			break;
		case Image::FORMAT_RGBH:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16B16_SFLOAT;
			break;
		case Image::FORMAT_RGBAH:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
			break;
		case Image::FORMAT_DXT1:
			rd_format = RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK;
			break;
		case Image::FORMAT_DXT3:
			rd_format = RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK;
			break;
		case Image::FORMAT_DXT5:
			rd_format = RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK;
			break;
		case Image::FORMAT_RGTC_R:
			rd_format = RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK;
			break;
		case Image::FORMAT_RGTC_RG:
			rd_format = RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
			break;
		case Image::FORMAT_BPTC_RGBA:
			rd_format = RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK;
			break;
		case Image::FORMAT_BPTC_RGBF:
			rd_format = RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK;
			break;
		case Image::FORMAT_BPTC_RGBFU:
			rd_format = RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK;
			break;
		case Image::FORMAT_R16:
			rd_format = RenderingDevice::DATA_FORMAT_R16_UNORM;
			break;
		case Image::FORMAT_RG16:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16_UNORM;
			break;
		case Image::FORMAT_RGB16:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16B16_UNORM;
			break;
		case Image::FORMAT_RGBA16:
			rd_format = RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM;
			break;
		default:
			UtilityFunctions::push_error("DSTex: Unsupported Godot image format: ", p_image_format);
			return RenderingDevice::DATA_FORMAT_MAX;
	}

	return p_is_srgb ? get_srgb_rd_format(rd_format) : rd_format;
}

int DSTex::rd_format_to_godot_format(int p_rd_format) {
	switch (p_rd_format) {
		case RenderingDevice::DATA_FORMAT_R8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8_SRGB:
			return Image::FORMAT_R8;
		case RenderingDevice::DATA_FORMAT_R8G8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8G8_SRGB:
			return Image::FORMAT_RG8;
		case RenderingDevice::DATA_FORMAT_R8G8B8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8G8B8_SRGB:
			return Image::FORMAT_RGB8;
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB:
			return Image::FORMAT_RGBA8;
		case RenderingDevice::DATA_FORMAT_R32_SFLOAT:
			return Image::FORMAT_RF;
		case RenderingDevice::DATA_FORMAT_R32G32_SFLOAT:
			return Image::FORMAT_RGF;
		case RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT:
			return Image::FORMAT_RGBF;
		case RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT:
			return Image::FORMAT_RGBAF;
		case RenderingDevice::DATA_FORMAT_R16_SFLOAT:
			return Image::FORMAT_RH;
		case RenderingDevice::DATA_FORMAT_R16G16_SFLOAT:
			return Image::FORMAT_RGH;
		case RenderingDevice::DATA_FORMAT_R16G16B16_SFLOAT:
			return Image::FORMAT_RGBH;
		case RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT:
			return Image::FORMAT_RGBAH;
		case RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
			return Image::FORMAT_DXT1;
		case RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK:
			return Image::FORMAT_DXT3;
		case RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK:
			return Image::FORMAT_DXT5;
		case RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK:
			return Image::FORMAT_RGTC_R;
		case RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK:
			return Image::FORMAT_RGTC_RG;
		case RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK:
			return Image::FORMAT_BPTC_RGBA;
		case RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK:
			return Image::FORMAT_BPTC_RGBF;
		case RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK:
			return Image::FORMAT_BPTC_RGBFU;
		case RenderingDevice::DATA_FORMAT_R16_UNORM:
			return Image::FORMAT_R16;
		case RenderingDevice::DATA_FORMAT_R16G16_UNORM:
			return Image::FORMAT_RG16;
		case RenderingDevice::DATA_FORMAT_R16G16B16_UNORM:
			return Image::FORMAT_RGB16;
		case RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM:
			return Image::FORMAT_RGBA16;
		default:
			return Image::FORMAT_MAX;
	}
}

bool DSTex::is_rd_format_srgb(int p_rd_format) {
	switch (p_rd_format) {
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB:
		case RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB:
		case RenderingDevice::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
		case RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
			return true;
		default:
			return false;
	}
}

int DSTex::get_srgb_rd_format(int p_rd_format) {
	switch (p_rd_format) {
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB;
		case RenderingDevice::DATA_FORMAT_B8G8R8A8_UNORM:
			return RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB;
		case RenderingDevice::DATA_FORMAT_A8B8G8R8_UNORM_PACK32:
			return RenderingDevice::DATA_FORMAT_A8B8G8R8_SRGB_PACK32;
		case RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
			return RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK;
		case RenderingDevice::DATA_FORMAT_R8_SRGB:
		case RenderingDevice::DATA_FORMAT_R8G8_SRGB:
		case RenderingDevice::DATA_FORMAT_R8G8B8_SRGB:
		case RenderingDevice::DATA_FORMAT_B8G8R8_SRGB:
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB:
		case RenderingDevice::DATA_FORMAT_B8G8R8A8_SRGB:
		case RenderingDevice::DATA_FORMAT_A8B8G8R8_SRGB_PACK32:
		case RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
		case RenderingDevice::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
			return p_rd_format;
		// Non-color / linear data formats: single/dual-channel (R8, RG8), floats,
		// BC4/BC5/BC6, and R16 have no D3D12 sRGB variants (Q44) and return self
		// so callers skip adding invalid shareable formats.
		case RenderingDevice::DATA_FORMAT_R8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8G8_UNORM:
		case RenderingDevice::DATA_FORMAT_R8G8B8_UNORM:
		case RenderingDevice::DATA_FORMAT_B8G8R8_UNORM:
		case RenderingDevice::DATA_FORMAT_R32_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R32G32_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R16_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R16G16_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R16G16B16_SFLOAT:
		case RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT:
		case RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK:
		case RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK:
		case RenderingDevice::DATA_FORMAT_R16_UNORM:
		case RenderingDevice::DATA_FORMAT_R16G16_UNORM:
		case RenderingDevice::DATA_FORMAT_R16G16B16_UNORM:
		case RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM:
			return p_rd_format;
		default:
			return RenderingDevice::DATA_FORMAT_MAX;
	}
}

DSCompressionFormat DSTex::get_compression_format(uint64_t p_capabilities) {
	return static_cast<DSCompressionFormat>(p_capabilities & DSTEX_CAP_COMPRESSION_MASK);
}

DSGaclTransform DSTex::get_gacl_transform(uint64_t p_capabilities) {
	return static_cast<DSGaclTransform>((p_capabilities & DSTEX_CAP_GACL_MASK) >> DSTEX_CAP_GACL_SHIFT);
}

bool DSTex::is_chunked(uint64_t p_capabilities) {
	return (p_capabilities & DSTEX_CAP_CHUNKED) != 0;
}

bool DSTex::gacl_transform_matches_pixel_format(DSGaclTransform p_transform, int p_rd_format) {
	switch (p_transform) {
		case DSGaclTransform::NONE:
			return true;
		case DSGaclTransform::BC1:
			return p_rd_format == RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK ||
					p_rd_format == RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK ||
					p_rd_format == RenderingDevice::DATA_FORMAT_BC1_RGBA_UNORM_BLOCK ||
					p_rd_format == RenderingDevice::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK;
		case DSGaclTransform::BC3:
			return p_rd_format == RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK ||
					p_rd_format == RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK;
		case DSGaclTransform::BC4:
			// SNORM is not mapped by rd_format_to_godot_format yet; keep the
			// accepted set aligned with the engine conversion.
			return p_rd_format == RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK;
		case DSGaclTransform::BC5:
			return p_rd_format == RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
		default:
			return false;
	}
}

Error DSTex::write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count, int p_compression_level, bool p_is_srgb, bool p_as_image_texture) {
	if (p_raw_data.size() == 0 || p_width <= 0 || p_height <= 0 || p_mip_count <= 0 || p_mip_count > static_cast<int>(DSTEX_MAX_MIP_COUNT)) {
		return ERR_INVALID_PARAMETER;
	}
	if (p_width > static_cast<int>(DSTEX_MAX_DIMENSION) || p_height > static_cast<int>(DSTEX_MAX_DIMENSION)) {
		UtilityFunctions::push_error("DSTex: Texture dimensions exceed the container limit of ", DSTEX_MAX_DIMENSION, " pixels.");
		return ERR_INVALID_DATA;
	}

	const int rd_format = godot_format_to_rd_format(p_format, p_is_srgb);
	if (rd_format == RenderingDevice::DATA_FORMAT_MAX) {
		return ERR_UNAVAILABLE;
	}

	const int mip_count = p_mip_count;
	std::vector<DSTexMipLayout> layouts;
	layouts.reserve(mip_count);
	uint64_t natural_size = 0;
	uint64_t footprint_size = 0;
	uint32_t mip_width = static_cast<uint32_t>(p_width);
	uint32_t mip_height = static_cast<uint32_t>(p_height);
	for (int i = 0; i < mip_count; ++i) {
		DSTexMipLayout layout = get_mip_layout(mip_width, mip_height, p_format);
		if (layout.row_pitch == 0 || layout.source_size == 0 || layout.footprint_size > std::numeric_limits<uint32_t>::max() ||
				layout.source_size > std::numeric_limits<uint64_t>::max() - natural_size ||
				layout.footprint_size > std::numeric_limits<uint64_t>::max() - footprint_size) {
			return ERR_INVALID_DATA;
		}
		layouts.push_back(layout);
		natural_size += layout.source_size;
		footprint_size += layout.footprint_size;
		mip_width = std::max<uint32_t>(1, mip_width / 2);
		mip_height = std::max<uint32_t>(1, mip_height / 2);
	}

	if (footprint_size > DSTEX_MAX_TOTAL_BYTES) {
		UtilityFunctions::push_error("DSTex: Texture footprint exceeds the container limit of ", DSTEX_MAX_TOTAL_BYTES, " bytes.");
		return ERR_INVALID_DATA;
	}

	const uint64_t input_size = static_cast<uint64_t>(p_raw_data.size());
	const bool input_is_footprint_aligned = input_size == footprint_size;
	if (!input_is_footprint_aligned && input_size != natural_size) {
		UtilityFunctions::push_error("DSTex: Raw texture data does not match the declared mip layout.");
		return ERR_INVALID_DATA;
	}

	Ref<GDeflateCompressor> compressor;
	compressor.instantiate();
	std::vector<PackedByteArray> compressed_mips;
	compressed_mips.reserve(mip_count);
	uint64_t source_offset = 0;
	uint64_t total_compressed_size = 0;
	for (const DSTexMipLayout &layout : layouts) {
		const uint64_t source_mip_size = input_is_footprint_aligned ? layout.footprint_size : layout.source_size;
		if (source_mip_size > input_size || source_offset > input_size - source_mip_size) {
			return ERR_INVALID_DATA;
		}

		PackedByteArray footprint;
		footprint.resize(static_cast<int64_t>(layout.footprint_size));
		memset(footprint.ptrw(), 0, static_cast<size_t>(layout.footprint_size));
		const uint64_t source_row_pitch = input_is_footprint_aligned ? layout.row_pitch : layout.source_row_bytes;
		for (uint64_t row = 0; row < layout.row_count; ++row) {
			memcpy(footprint.ptrw() + row * layout.row_pitch,
					p_raw_data.ptr() + source_offset + row * source_row_pitch,
					static_cast<size_t>(layout.source_row_bytes));
		}

		PackedByteArray compressed_mip = compressor->compress(footprint, p_compression_level);
		if (compressed_mip.size() == 0 || compressed_mip.size() > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()) ||
				total_compressed_size > std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(compressed_mip.size())) {
			UtilityFunctions::push_error("DSTex: Failed to compress texture mip.");
			return ERR_INVALID_DATA;
		}
		compressed_mips.push_back(compressed_mip);
		total_compressed_size += static_cast<uint64_t>(compressed_mip.size());
		source_offset += source_mip_size;
	}

	if (total_compressed_size > DSTEX_MAX_TOTAL_BYTES) {
		UtilityFunctions::push_error("DSTex: Compressed texture exceeds the container limit of ", DSTEX_MAX_TOTAL_BYTES, " bytes.");
		return ERR_INVALID_DATA;
	}

	DSTexHeader header;
	header.magic = 0x58455444; // "DTEX"
	header.version = 2;
	header.flags = (p_is_srgb ? DSTEX_FLAG_SRGB : 0) | (p_as_image_texture ? DSTEX_FLAG_AS_IMAGE_TEXTURE : 0);
	header.width = static_cast<uint32_t>(p_width);
	header.height = static_cast<uint32_t>(p_height);
	header.pixel_format = static_cast<uint32_t>(rd_format);
	header.mip_count = static_cast<uint32_t>(mip_count);
	header.original_size = footprint_size;
	header.compressed_size = total_compressed_size;
	header.reserved = 0;

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSTex: Could not open file for writing: ", p_path);
		return FileAccess::get_open_error();
	}

	file->store_buffer(reinterpret_cast<const uint8_t *>(&header), sizeof(DSTexHeader));

	uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntryV2) * static_cast<uint64_t>(mip_count));
	if (!align_up(data_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, data_offset)) {
		file->close();
		return ERR_INVALID_DATA;
	}
	std::vector<DSTexMipEntryV2> mip_entries;
	mip_entries.reserve(mip_count);
	uint64_t running_offset = data_offset;
	for (int i = 0; i < mip_count; ++i) {
		DSTexMipEntryV2 mip_entry{};
		mip_entry.offset = running_offset;
		mip_entry.compressed_size = static_cast<uint32_t>(compressed_mips[i].size());
		mip_entry.uncompressed_size = static_cast<uint32_t>(layouts[i].footprint_size);
		mip_entry.row_pitch = static_cast<uint32_t>(layouts[i].row_pitch);
		mip_entries.push_back(mip_entry);
		if (running_offset > std::numeric_limits<uint64_t>::max() - compressed_mips[i].size() ||
				!align_up(running_offset + compressed_mips[i].size(), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, running_offset)) {
			file->close();
			return ERR_INVALID_DATA;
		}
	}
	for (const DSTexMipEntryV2 &mip_entry : mip_entries) {
		file->store_buffer(reinterpret_cast<const uint8_t *>(&mip_entry), sizeof(DSTexMipEntryV2));
	}
	for (int i = 0; i < mip_count; ++i) {
		file->seek(mip_entries[i].offset);
		file->store_buffer(compressed_mips[i].ptr(), compressed_mips[i].size());
	}
	file->close();

	return OK;
}

Dictionary DSTex::read_header(const String &p_path) {
	Dictionary result;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSTex: Could not open file for reading header: ", p_path);
		return result;
	}

	if (file->get_length() < sizeof(DSTexHeader)) {
		UtilityFunctions::push_error("DSTex: File too small for header.");
		return result;
	}

	DSTexHeader header;
	PackedByteArray header_bytes = file->get_buffer(sizeof(DSTexHeader));
	if (header_bytes.size() < sizeof(DSTexHeader)) {
		return result;
	}
	memcpy(&header, header_bytes.ptr(), sizeof(DSTexHeader));

	if (header.magic != 0x58455444) {
		UtilityFunctions::push_error("DSTex: Invalid magic number.");
		return result;
	}

	if (header.version != 1 && header.version != 2 && header.version != 3) {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		return result;
	}
	if ((header.flags & ~(DSTEX_FLAG_SRGB | DSTEX_FLAG_AS_IMAGE_TEXTURE)) != 0) {
		UtilityFunctions::push_error("DSTex: Reserved texture flags must be zero.");
		return result;
	}
	if (header.version <= 2 && header.reserved != 0) {
		UtilityFunctions::push_error("DSTex: v1/v2 reserved field must be zero.");
		return result;
	}
	DSTexHeaderV3Ext v3_ext = {};
	if (header.version == 3) {
		if (file->get_length() < sizeof(DSTexHeaderV3)) {
			UtilityFunctions::push_error("DSTex: File too small for v3 header.");
			return result;
		}
		PackedByteArray ext_bytes = file->get_buffer(sizeof(DSTexHeaderV3Ext));
		if (ext_bytes.size() < static_cast<int64_t>(sizeof(DSTexHeaderV3Ext))) {
			return result;
		}
		memcpy(&v3_ext, ext_bytes.ptr(), sizeof(DSTexHeaderV3Ext));
		if (v3_ext.reserved0 != 0) {
			UtilityFunctions::push_error("DSTex: v3 header reserved field must be zero.");
			return result;
		}
		if (v3_ext.logical_width > header.width || v3_ext.logical_height > header.height) {
			UtilityFunctions::push_error("DSTex: v3 logical dimensions exceed storage dimensions.");
			return result;
		}
		const uint64_t capabilities = header.reserved;
		if ((capabilities & ~DSTEX_CAP_KNOWN_MASK) != 0) {
			UtilityFunctions::push_error("DSTex: Unknown v3 capability bits.");
			return result;
		}
		const DSCompressionFormat format = get_compression_format(capabilities);
		const DSGaclTransform gacl = get_gacl_transform(capabilities);
		if (!ds_compression_format_is_valid(static_cast<uint8_t>(format)) ||
				!ds_gacl_transform_is_valid(static_cast<uint8_t>(gacl))) {
			UtilityFunctions::push_error("DSTex: Invalid v3 compression format or GACL transform.");
			return result;
		}
		if (!gacl_transform_matches_pixel_format(gacl, static_cast<int>(header.pixel_format))) {
			UtilityFunctions::push_error("DSTex: GACL transform does not match the pixel format family.");
			return result;
		}
		if (gacl != DSGaclTransform::NONE && format == DSCompressionFormat::NONE) {
			UtilityFunctions::push_error("DSTex: GACL requires a compressed payload.");
			return result;
		}
		if (format == DSCompressionFormat::NONE && header.compressed_size != header.original_size) {
			UtilityFunctions::push_error("DSTex: NONE-compressed header sizes must match.");
			return result;
		}
	}

	if (header.width == 0 || header.height == 0) {
		UtilityFunctions::push_error("DSTex: Invalid texture dimensions.");
		return Dictionary();
	}
	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
		return Dictionary();
	}
	if (!dstex_sizes_are_sane(header.width, header.height, header.original_size, header.compressed_size)) {
		UtilityFunctions::push_error("DSTex: Header sizes exceed sane texture bounds (FMT-01).");
		return Dictionary();
	}
	{
		uint64_t entry_size = (header.version == 1) ? sizeof(DSTexMipEntry) : sizeof(DSTexMipEntryV2);
		uint64_t file_len = file->get_length();
		uint64_t mip_table_size = static_cast<uint64_t>(header.mip_count) * entry_size;
		if (entry_size != 0 && mip_table_size / entry_size != static_cast<uint64_t>(header.mip_count)) {
			UtilityFunctions::push_error("DSTex: Mip table size overflow.");
			return Dictionary();
		}
		uint64_t header_size = dstex_header_size(header.version);
		uint64_t required = header_size + mip_table_size;
		if (required < header_size || required > file_len) {
			UtilityFunctions::push_error("DSTex: Mip table exceeds file bounds.");
			return Dictionary();
		}
	}

	result["magic"] = header.magic;
	result["version"] = header.version;
	result["flags"] = header.flags;
	result["width"] = header.width;
	result["height"] = header.height;
	result["pixel_format"] = header.pixel_format;
	result["mip_count"] = header.mip_count;
	result["original_size"] = header.original_size;
	result["compressed_size"] = header.compressed_size;
	result["reserved"] = header.reserved;
	result["logical_width"] = (header.version == 3 && v3_ext.logical_width > 0) ? v3_ext.logical_width : header.width;
	result["logical_height"] = (header.version == 3 && v3_ext.logical_height > 0) ? v3_ext.logical_height : header.height;
	result["depth_or_layers"] = (header.version == 3 && v3_ext.depth_or_layers > 0) ? v3_ext.depth_or_layers : 1u;
	result["is_srgb"] = (header.flags & DSTEX_FLAG_SRGB) != 0 || is_rd_format_srgb(static_cast<int>(header.pixel_format));
	result["as_image_texture"] = (header.flags & DSTEX_FLAG_AS_IMAGE_TEXTURE) != 0;

	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	PackedInt32Array mip_chunk_counts;
	PackedInt64Array chunk_offsets;
	PackedInt32Array chunk_compressed_sizes;
	PackedInt32Array chunk_uncompressed_sizes;
	bool chunked = false;
	mip_offsets.resize(header.mip_count);
	mip_sizes.resize(header.mip_count);
	mip_uncompressed_sizes.resize(header.mip_count);
	mip_row_pitches.resize(header.mip_count);
	if (header.version == 1) {
		uint64_t file_len = file->get_length();
		DSTexMipEntry v1_entries[DSTEX_MAX_MIP_COUNT];
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntry));
			if (mip_bytes.size() != sizeof(DSTexMipEntry)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntry));
			if (mip_entry.offset > file_len || mip_entry.size > file_len - mip_entry.offset ||
					(mip_entry.size != 0 && mip_entry.offset + mip_entry.size < mip_entry.offset)) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds or overflows.");
				file->close();
				return Dictionary();
			}
			v1_entries[i] = mip_entry;
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
		String order_error;
		if (!ds_v1_mip_payload_order_is_sane(header, v1_entries, order_error)) {
			UtilityFunctions::push_error(order_error);
			file->close();
			return Dictionary();
		}
	} else if (header.version == 2 || header.version == 3) {
		uint64_t file_len = file->get_length();
		DSTexMipEntryV2 entries[DSTEX_MAX_MIP_COUNT];
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (mip_bytes.size() != sizeof(DSTexMipEntryV2)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntryV2));
			if (mip_entry.offset > file_len || mip_entry.compressed_size > file_len - mip_entry.offset ||
					(mip_entry.compressed_size != 0 && mip_entry.offset + mip_entry.compressed_size < mip_entry.offset)) {
				UtilityFunctions::push_error("DSTex: Mip entry exceeds file bounds or overflows.");
				file->close();
				return Dictionary();
			}
			if (header.version == 2 && mip_entry.reserved != 0) {
				UtilityFunctions::push_error("DSTex: v2 mip reserved field must be zero.");
				file->close();
				return Dictionary();
			}
			if (header.version == 3) {
				if (mip_entry.chunk_count == 0 || mip_entry.chunk_count > DSTEX_MAX_CHUNKS_PER_MIP) {
					UtilityFunctions::push_error("DSTex: Invalid v3 chunk count.");
					file->close();
					return Dictionary();
				}
				if (mip_entry.offset % 512 != 0) {
					UtilityFunctions::push_error("DSTex: v3 mip offset must be 512-byte aligned.");
					file->close();
					return Dictionary();
				}
				if (mip_entry.row_pitch == 0 || mip_entry.row_pitch % 256 != 0) {
					UtilityFunctions::push_error("DSTex: v3 mip row pitch must be 256-byte aligned.");
					file->close();
					return Dictionary();
				}
				if (get_compression_format(header.reserved) == DSCompressionFormat::NONE &&
						mip_entry.compressed_size != mip_entry.uncompressed_size) {
					UtilityFunctions::push_error("DSTex: NONE-compressed mip sizes must match.");
					file->close();
					return Dictionary();
				}
			}
			entries[i] = mip_entry;
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
		}
		if (!DSTex::mip_table_is_sane(header, entries)) {
			UtilityFunctions::push_error("DSTex: Mip table does not match the header totals or the declared mip layout.");
			file->close();
			return Dictionary();
		}
		if (header.version == 2) {
			String order_error;
			if (!ds_v2_mip_payload_order_is_sane(header, entries, order_error)) {
				UtilityFunctions::push_error(order_error);
				file->close();
				return Dictionary();
			}
		}
		if (header.version == 3) {
			String chunk_error;
			if (!ds_parse_v3_chunks(file, header, entries, mip_chunk_counts, chunk_offsets,
						chunk_compressed_sizes, chunk_uncompressed_sizes, chunked, chunk_error)) {
				UtilityFunctions::push_error(chunk_error);
				file->close();
				return Dictionary();
			}
		}
	} else {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		file->close();
		return Dictionary();
	}
	result["mip_offsets"] = mip_offsets;
	result["mip_sizes"] = mip_sizes;
	result["mip_uncompressed_sizes"] = mip_uncompressed_sizes;
	result["mip_row_pitches"] = mip_row_pitches;
	result["mip_chunk_counts"] = mip_chunk_counts;
	result["chunk_offsets"] = chunk_offsets;
	result["chunk_compressed_sizes"] = chunk_compressed_sizes;
	result["chunk_uncompressed_sizes"] = chunk_uncompressed_sizes;
	result["chunked"] = chunked;
	result["capabilities"] = header.version == 3 ? header.reserved : 0;
	result["compression_format"] = header.version == 3 ?
			static_cast<int>(get_compression_format(header.reserved)) :
			static_cast<int>(DSCompressionFormat::GDEFLATE);
	result["gacl_transform"] = header.version == 3 ?
			static_cast<int>(get_gacl_transform(header.reserved)) :
			static_cast<int>(DSGaclTransform::NONE);

	file->close();
	return result;
}

Dictionary DSTex::read_from_file(const String &p_path) {
	Dictionary result;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSTex: Could not open file for reading: ", p_path);
		return result;
	}

	if (file->get_length() < sizeof(DSTexHeader)) {
		UtilityFunctions::push_error("DSTex: File too small for header.");
		return result;
	}

	DSTexHeader header;
	PackedByteArray header_bytes = file->get_buffer(sizeof(DSTexHeader));
	if (header_bytes.size() < sizeof(DSTexHeader)) {
		return result;
	}
	memcpy(&header, header_bytes.ptr(), sizeof(DSTexHeader));

	if (header.magic != 0x58455444) {
		UtilityFunctions::push_error("DSTex: Invalid magic number.");
		return result;
	}

	if (header.version != 1 && header.version != 2 && header.version != 3) {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		return result;
	}
	if ((header.flags & ~(DSTEX_FLAG_SRGB | DSTEX_FLAG_AS_IMAGE_TEXTURE)) != 0) {
		UtilityFunctions::push_error("DSTex: Reserved texture flags must be zero.");
		return result;
	}
	if (header.version <= 2 && header.reserved != 0) {
		UtilityFunctions::push_error("DSTex: v1/v2 reserved field must be zero.");
		return result;
	}
	DSTexHeaderV3Ext v3_ext = {};
	if (header.version == 3) {
		if (file->get_length() < sizeof(DSTexHeaderV3)) {
			UtilityFunctions::push_error("DSTex: File too small for v3 header.");
			return result;
		}
		PackedByteArray ext_bytes = file->get_buffer(sizeof(DSTexHeaderV3Ext));
		if (ext_bytes.size() < static_cast<int64_t>(sizeof(DSTexHeaderV3Ext))) {
			return result;
		}
		memcpy(&v3_ext, ext_bytes.ptr(), sizeof(DSTexHeaderV3Ext));
		if (v3_ext.reserved0 != 0) {
			UtilityFunctions::push_error("DSTex: v3 header reserved field must be zero.");
			return result;
		}
		if (v3_ext.logical_width > header.width || v3_ext.logical_height > header.height) {
			UtilityFunctions::push_error("DSTex: v3 logical dimensions exceed storage dimensions.");
			return result;
		}
		const uint64_t capabilities = header.reserved;
		if ((capabilities & ~DSTEX_CAP_KNOWN_MASK) != 0) {
			UtilityFunctions::push_error("DSTex: Unknown v3 capability bits.");
			return result;
		}
		const DSCompressionFormat format = get_compression_format(capabilities);
		const DSGaclTransform gacl = get_gacl_transform(capabilities);
		if (!ds_compression_format_is_valid(static_cast<uint8_t>(format)) ||
				!ds_gacl_transform_is_valid(static_cast<uint8_t>(gacl))) {
			UtilityFunctions::push_error("DSTex: Invalid v3 compression format or GACL transform.");
			return result;
		}
		if (!gacl_transform_matches_pixel_format(gacl, static_cast<int>(header.pixel_format))) {
			UtilityFunctions::push_error("DSTex: GACL transform does not match the pixel format family.");
			return result;
		}
		if (gacl != DSGaclTransform::NONE && format == DSCompressionFormat::NONE) {
			UtilityFunctions::push_error("DSTex: GACL requires a compressed payload.");
			return result;
		}
		if (format == DSCompressionFormat::NONE && header.compressed_size != header.original_size) {
			UtilityFunctions::push_error("DSTex: NONE-compressed header sizes must match.");
			return result;
		}
	}

	if (header.width == 0 || header.height == 0) {
		UtilityFunctions::push_error("DSTex: Invalid texture dimensions.");
		return Dictionary();
	}
	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
		return Dictionary();
	}
	if (!dstex_sizes_are_sane(header.width, header.height, header.original_size, header.compressed_size)) {
		UtilityFunctions::push_error("DSTex: Header sizes exceed sane texture bounds (FMT-01).");
		return Dictionary();
	}
	{
		uint64_t entry_size = (header.version == 1) ? sizeof(DSTexMipEntry) : sizeof(DSTexMipEntryV2);
		uint64_t file_len = file->get_length();
		uint64_t mip_table_size = static_cast<uint64_t>(header.mip_count) * entry_size;
		if (entry_size != 0 && mip_table_size / entry_size != static_cast<uint64_t>(header.mip_count)) {
			UtilityFunctions::push_error("DSTex: Mip table size overflow.");
			return Dictionary();
		}
		uint64_t header_size = dstex_header_size(header.version);
		uint64_t required = header_size + mip_table_size;
		if (required < header_size || required > file_len) {
			UtilityFunctions::push_error("DSTex: Mip table exceeds file bounds.");
			return Dictionary();
		}
	}

	Dictionary header_dict;
	header_dict["magic"] = header.magic;
	header_dict["version"] = header.version;
	header_dict["flags"] = header.flags;
	header_dict["width"] = header.width;
	header_dict["height"] = header.height;
	header_dict["pixel_format"] = header.pixel_format;
	header_dict["mip_count"] = header.mip_count;
	header_dict["original_size"] = header.original_size;
	header_dict["compressed_size"] = header.compressed_size;
	header_dict["reserved"] = header.reserved;
	header_dict["logical_width"] = (header.version == 3 && v3_ext.logical_width > 0) ? v3_ext.logical_width : header.width;
	header_dict["logical_height"] = (header.version == 3 && v3_ext.logical_height > 0) ? v3_ext.logical_height : header.height;
	header_dict["depth_or_layers"] = (header.version == 3 && v3_ext.depth_or_layers > 0) ? v3_ext.depth_or_layers : 1u;
	header_dict["capabilities"] = header.version == 3 ? header.reserved : 0;
	header_dict["compression_format"] = header.version == 3 ?
			static_cast<int>(get_compression_format(header.reserved)) :
			static_cast<int>(DSCompressionFormat::GDEFLATE);
	header_dict["gacl_transform"] = header.version == 3 ?
			static_cast<int>(get_gacl_transform(header.reserved)) :
			static_cast<int>(DSGaclTransform::NONE);
	header_dict["chunked"] = header.version == 3 ? is_chunked(header.reserved) : false;
	header_dict["is_srgb"] = (header.flags & DSTEX_FLAG_SRGB) != 0 || is_rd_format_srgb(static_cast<int>(header.pixel_format));
	header_dict["as_image_texture"] = (header.flags & DSTEX_FLAG_AS_IMAGE_TEXTURE) != 0;

	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	mip_offsets.resize(header.mip_count);
	mip_sizes.resize(header.mip_count);
	mip_uncompressed_sizes.resize(header.mip_count);
	mip_row_pitches.resize(header.mip_count);

	if (header.version == 1) {
		DSTexMipEntry v1_entries[DSTEX_MAX_MIP_COUNT];
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntry));
			if (mip_bytes.size() != sizeof(DSTexMipEntry)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntry));
			v1_entries[i] = mip_entry;
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
		String order_error;
		if (!ds_v1_mip_payload_order_is_sane(header, v1_entries, order_error)) {
			UtilityFunctions::push_error(order_error);
			file->close();
			return Dictionary();
		}
	} else if (header.version == 2 || header.version == 3) {
		DSTexMipEntryV2 entries[DSTEX_MAX_MIP_COUNT];
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (mip_bytes.size() != sizeof(DSTexMipEntryV2)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntryV2));
			if (header.version == 2 && mip_entry.reserved != 0) {
				UtilityFunctions::push_error("DSTex: v2 mip reserved field must be zero.");
				file->close();
				return Dictionary();
			}
			if (header.version == 3 && (mip_entry.chunk_count == 0 ||
					mip_entry.chunk_count > DSTEX_MAX_CHUNKS_PER_MIP)) {
				UtilityFunctions::push_error("DSTex: Invalid v3 chunk count.");
				file->close();
				return Dictionary();
			}
			if (header.version == 3 && mip_entry.offset % 512 != 0) {
				UtilityFunctions::push_error("DSTex: v3 mip offset must be 512-byte aligned.");
				file->close();
				return Dictionary();
			}
			if (header.version == 3 && (mip_entry.row_pitch == 0 || mip_entry.row_pitch % 256 != 0)) {
				UtilityFunctions::push_error("DSTex: v3 mip row pitch must be 256-byte aligned.");
				file->close();
				return Dictionary();
			}
			if (header.version == 3 &&
					get_compression_format(header.reserved) == DSCompressionFormat::NONE &&
					mip_entry.compressed_size != mip_entry.uncompressed_size) {
				UtilityFunctions::push_error("DSTex: NONE-compressed mip sizes must match.");
				file->close();
				return Dictionary();
			}
			entries[i] = mip_entry;
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
		}
		if (!DSTex::mip_table_is_sane(header, entries)) {
			UtilityFunctions::push_error("DSTex: Mip table does not match the header totals or the declared mip layout.");
			file->close();
			return Dictionary();
		}
		if (header.version == 2) {
			String order_error;
			if (!ds_v2_mip_payload_order_is_sane(header, entries, order_error)) {
				UtilityFunctions::push_error(order_error);
				file->close();
				return Dictionary();
			}
		}
		if (header.version == 3) {
			PackedInt32Array chunk_counts;
			PackedInt64Array chunk_offsets;
			PackedInt32Array chunk_compressed;
			PackedInt32Array chunk_uncompressed;
			bool chunked = false;
			String chunk_error;
			if (!ds_parse_v3_chunks(file, header, entries, chunk_counts, chunk_offsets,
						chunk_compressed, chunk_uncompressed, chunked, chunk_error)) {
				UtilityFunctions::push_error(chunk_error);
				file->close();
				return Dictionary();
			}
			// The CPU/ImageTexture path cannot decode Zstd, GACL-shuffled data or
			// chunked mips until Step 5; fail closed instead of misreading them.
			const DSCompressionFormat format = get_compression_format(header.reserved);
			const DSGaclTransform gacl = get_gacl_transform(header.reserved);
			if (format != DSCompressionFormat::GDEFLATE || gacl != DSGaclTransform::NONE || chunked) {
				UtilityFunctions::push_error("DSTex: v3 Zstd/GACL/chunked payloads require the GPU path or Step 5 CPU support.");
				file->close();
				return Dictionary();
			}
		}
	} else {
		UtilityFunctions::push_error("DSTex: Unsupported texture version.");
		file->close();
		return Dictionary();
	}

	PackedByteArray compressed_data;
	Array mip_data;
	if (header.version == 1) {
		uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntry) * static_cast<uint64_t>(header.mip_count));
		uint64_t file_len = file->get_length();
		if (header.compressed_size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
			UtilityFunctions::push_error("DSTex: Compressed size exceeds maximum buffer size.");
			file->close();
			return Dictionary();
		}
		if (data_offset > file_len || header.compressed_size > file_len - data_offset) {
			UtilityFunctions::push_error("DSTex: Compressed data exceeds file bounds.");
			file->close();
			return Dictionary();
		}
		if (header.compressed_size != 0 && data_offset + header.compressed_size < data_offset) {
			UtilityFunctions::push_error("DSTex: Compressed data offset overflow.");
			file->close();
			return Dictionary();
		}
		file->seek(data_offset);
		compressed_data = file->get_buffer(static_cast<int64_t>(header.compressed_size));
	} else {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			if (mip_offsets[i] < 0 || mip_sizes[i] < 0) {
				UtilityFunctions::push_error("DSTex: Invalid mip offset/size (negative).");
				file->close();
				return Dictionary();
			}
			uint64_t offset = static_cast<uint64_t>(mip_offsets[i]);
			uint64_t size = static_cast<uint64_t>(static_cast<uint32_t>(mip_sizes[i]));
			uint64_t file_len = file->get_length();
			if (offset > file_len || size > file_len - offset) {
				UtilityFunctions::push_error("DSTex: Mip payload exceeds file bounds.");
				file->close();
				return Dictionary();
			}
			if (size != 0 && offset + size < offset) {
				UtilityFunctions::push_error("DSTex: Mip payload offset overflow.");
				file->close();
				return Dictionary();
			}
			file->seek(offset);
			PackedByteArray mip_payload = file->get_buffer(mip_sizes[i]);
			if (mip_payload.size() != mip_sizes[i]) {
				file->close();
				return Dictionary();
			}
			compressed_data.append_array(mip_payload);
			mip_data.push_back(mip_payload);
		}
	}

	result["header"] = header_dict;
	result["compressed_data"] = compressed_data;
	result["mip_offsets"] = mip_offsets;
	result["mip_sizes"] = mip_sizes;
	result["mip_uncompressed_sizes"] = mip_uncompressed_sizes;
	result["mip_row_pitches"] = mip_row_pitches;
	if (header.version == 2 || header.version == 3) {
		result["mip_data"] = mip_data;
	}

	file->close();
	return result;
}

// Keep the legacy file-info API as an explicit convenience alias for read_header.
Dictionary DSTex::get_file_info(const String &p_path) {
	return read_header(p_path);
}

} // namespace godot
