#include "dstex_format.h"
#include "compression/gdeflate_compressor.h"
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
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
	uint64_t row_bytes = static_cast<uint64_t>(p_width) * 4;
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
			break;
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

void DSTex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("write_to_file", "path", "raw_data", "width", "height", "format", "mip_count", "compression_level", "is_srgb"), &DSTex::write_to_file, DEFVAL(1), DEFVAL(1), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("read_header", "path"), &DSTex::read_header);
	ClassDB::bind_method(D_METHOD("read_from_file", "path"), &DSTex::read_from_file);
	ClassDB::bind_method(D_METHOD("get_file_info", "path"), &DSTex::get_file_info);

	ClassDB::bind_static_method("DSTex", D_METHOD("godot_format_to_rd_format", "image_format", "is_srgb"), &DSTex::godot_format_to_rd_format, DEFVAL(false));
	ClassDB::bind_static_method("DSTex", D_METHOD("rd_format_to_godot_format", "rd_format"), &DSTex::rd_format_to_godot_format);
}

DSTex::DSTex() {
}

DSTex::~DSTex() {
}

int DSTex::godot_format_to_rd_format(int p_image_format, bool p_is_srgb) {
	if (p_image_format >= 48) {
		return p_image_format;
	}

	if (p_is_srgb) {
		switch (p_image_format) {
			case Image::FORMAT_L8:
			case Image::FORMAT_R8:
				return RenderingDevice::DATA_FORMAT_R8_SRGB;
			case Image::FORMAT_LA8:
			case Image::FORMAT_RG8:
				return RenderingDevice::DATA_FORMAT_R8G8_SRGB;
			case Image::FORMAT_RGB8:
				return RenderingDevice::DATA_FORMAT_R8G8B8_SRGB;
			case Image::FORMAT_RGBA8:
				return RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB;
			case Image::FORMAT_DXT1:
				return RenderingDevice::DATA_FORMAT_BC1_RGB_SRGB_BLOCK;
			case Image::FORMAT_DXT3:
				return RenderingDevice::DATA_FORMAT_BC2_SRGB_BLOCK;
			case Image::FORMAT_DXT5:
				return RenderingDevice::DATA_FORMAT_BC3_SRGB_BLOCK;
			case Image::FORMAT_BPTC_RGBA:
				return RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK;
			default:
				break;
		}
	}

	switch (p_image_format) {
		case Image::FORMAT_L8:
		case Image::FORMAT_R8:
			return RenderingDevice::DATA_FORMAT_R8_UNORM;
		case Image::FORMAT_LA8:
		case Image::FORMAT_RG8:
			return RenderingDevice::DATA_FORMAT_R8G8_UNORM;
		case Image::FORMAT_RGB8:
			return RenderingDevice::DATA_FORMAT_R8G8B8_UNORM;
		case Image::FORMAT_RGBA8:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
		case Image::FORMAT_RF:
			return RenderingDevice::DATA_FORMAT_R32_SFLOAT;
		case Image::FORMAT_RGF:
			return RenderingDevice::DATA_FORMAT_R32G32_SFLOAT;
		case Image::FORMAT_RGBF:
			return RenderingDevice::DATA_FORMAT_R32G32B32_SFLOAT;
		case Image::FORMAT_RGBAF:
			return RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT;
		case Image::FORMAT_RH:
			return RenderingDevice::DATA_FORMAT_R16_SFLOAT;
		case Image::FORMAT_RGH:
			return RenderingDevice::DATA_FORMAT_R16G16_SFLOAT;
		case Image::FORMAT_RGBH:
			return RenderingDevice::DATA_FORMAT_R16G16B16_SFLOAT;
		case Image::FORMAT_RGBAH:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT;
		case Image::FORMAT_DXT1:
			return RenderingDevice::DATA_FORMAT_BC1_RGB_UNORM_BLOCK;
		case Image::FORMAT_DXT3:
			return RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK;
		case Image::FORMAT_DXT5:
			return RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK;
		case Image::FORMAT_RGTC_R:
			return RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK;
		case Image::FORMAT_RGTC_RG:
			return RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK;
		case Image::FORMAT_BPTC_RGBA:
			return RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK;
		case Image::FORMAT_BPTC_RGBF:
			return RenderingDevice::DATA_FORMAT_BC6H_SFLOAT_BLOCK;
		case Image::FORMAT_BPTC_RGBFU:
			return RenderingDevice::DATA_FORMAT_BC6H_UFLOAT_BLOCK;
		case Image::FORMAT_R16:
			return RenderingDevice::DATA_FORMAT_R16_UNORM;
		case Image::FORMAT_RG16:
			return RenderingDevice::DATA_FORMAT_R16G16_UNORM;
		case Image::FORMAT_RGB16:
			return RenderingDevice::DATA_FORMAT_R16G16B16_UNORM;
		case Image::FORMAT_RGBA16:
			return RenderingDevice::DATA_FORMAT_R16G16B16A16_UNORM;
		default:
			return RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM;
	}
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
			return Image::FORMAT_RGBA8;
	}
}

Error DSTex::write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count, int p_compression_level, bool p_is_srgb) {
	if (p_raw_data.size() == 0 || p_width <= 0 || p_height <= 0) {
		return ERR_INVALID_PARAMETER;
	}

	const int mip_count = p_mip_count > 0 ? p_mip_count : 1;
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

	uint32_t rd_format = static_cast<uint32_t>(godot_format_to_rd_format(p_format, p_is_srgb));

	DSTexHeader header;
	header.magic = 0x58455444; // "DTEX"
	header.version = 2;
	header.flags = p_is_srgb ? 1 : 0;
	header.width = static_cast<uint32_t>(p_width);
	header.height = static_cast<uint32_t>(p_height);
	header.pixel_format = rd_format;
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

	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
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
		uint64_t required = sizeof(DSTexHeader) + mip_table_size;
		if (required < sizeof(DSTexHeader) || required > file_len) {
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

	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	mip_offsets.resize(header.mip_count);
	mip_sizes.resize(header.mip_count);
	mip_uncompressed_sizes.resize(header.mip_count);
	mip_row_pitches.resize(header.mip_count);
	if (header.version == 1) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntry));
			if (mip_bytes.size() != sizeof(DSTexMipEntry)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntry));
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
	} else if (header.version == 2) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (mip_bytes.size() != sizeof(DSTexMipEntryV2)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntryV2));
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
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

	if (header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT) {
		UtilityFunctions::push_error("DSTex: Invalid mip count (must be 1..16).");
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
		uint64_t required = sizeof(DSTexHeader) + mip_table_size;
		if (required < sizeof(DSTexHeader) || required > file_len) {
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

	PackedInt64Array mip_offsets;
	PackedInt32Array mip_sizes;
	PackedInt32Array mip_uncompressed_sizes;
	PackedInt32Array mip_row_pitches;
	mip_offsets.resize(header.mip_count);
	mip_sizes.resize(header.mip_count);
	mip_uncompressed_sizes.resize(header.mip_count);
	mip_row_pitches.resize(header.mip_count);

	if (header.version == 1) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntry mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntry));
			if (mip_bytes.size() != sizeof(DSTexMipEntry)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntry));
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
	} else if (header.version == 2) {
		for (uint32_t i = 0; i < header.mip_count; ++i) {
			DSTexMipEntryV2 mip_entry;
			PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (mip_bytes.size() != sizeof(DSTexMipEntryV2)) {
				file->close();
				return Dictionary();
			}
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntryV2));
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.compressed_size);
			mip_uncompressed_sizes[i] = static_cast<int32_t>(mip_entry.uncompressed_size);
			mip_row_pitches[i] = static_cast<int32_t>(mip_entry.row_pitch);
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
	if (header.version == 2) {
		result["mip_data"] = mip_data;
	}

	file->close();
	return result;
}

Dictionary DSTex::get_file_info(const String &p_path) {
	return read_header(p_path);
}

} // namespace godot
