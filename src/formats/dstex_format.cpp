#include "dstex_format.h"
#include "compression/gdeflate_compressor.h"
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

namespace godot {

void DSTex::_bind_methods() {
	ClassDB::bind_method(D_METHOD("write_to_file", "path", "raw_data", "width", "height", "format", "mip_count", "compression_level"), &DSTex::write_to_file, DEFVAL(1), DEFVAL(1));
	ClassDB::bind_method(D_METHOD("read_header", "path"), &DSTex::read_header);
	ClassDB::bind_method(D_METHOD("read_from_file", "path"), &DSTex::read_from_file);
	ClassDB::bind_method(D_METHOD("get_file_info", "path"), &DSTex::get_file_info);

	ClassDB::bind_static_method("DSTex", D_METHOD("godot_format_to_rd_format", "image_format"), &DSTex::godot_format_to_rd_format);
	ClassDB::bind_static_method("DSTex", D_METHOD("rd_format_to_godot_format", "rd_format"), &DSTex::rd_format_to_godot_format);
}

DSTex::DSTex() {
}

DSTex::~DSTex() {
}

int DSTex::godot_format_to_rd_format(int p_image_format) {
	if (p_image_format >= 48) {
		return p_image_format;
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
			return Image::FORMAT_R8;
		case RenderingDevice::DATA_FORMAT_R8G8_UNORM:
			return Image::FORMAT_RG8;
		case RenderingDevice::DATA_FORMAT_R8G8B8_UNORM:
			return Image::FORMAT_RGB8;
		case RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM:
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
			return Image::FORMAT_DXT1;
		case RenderingDevice::DATA_FORMAT_BC2_UNORM_BLOCK:
			return Image::FORMAT_DXT3;
		case RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK:
			return Image::FORMAT_DXT5;
		case RenderingDevice::DATA_FORMAT_BC4_UNORM_BLOCK:
			return Image::FORMAT_RGTC_R;
		case RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK:
			return Image::FORMAT_RGTC_RG;
		case RenderingDevice::DATA_FORMAT_BC7_UNORM_BLOCK:
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

Error DSTex::write_to_file(const String &p_path, const PackedByteArray &p_raw_data, int p_width, int p_height, int p_format, int p_mip_count, int p_compression_level) {
	if (p_raw_data.size() == 0 || p_width <= 0 || p_height <= 0) {
		return ERR_INVALID_PARAMETER;
	}

	Ref<GDeflateCompressor> compressor;
	compressor.instantiate();
	PackedByteArray compressed_data = compressor->compress(p_raw_data, p_compression_level);
	if (compressed_data.size() == 0) {
		UtilityFunctions::push_error("DSTex: Failed to compress texture data.");
		return ERR_INVALID_DATA;
	}

	int mip_count = p_mip_count > 0 ? p_mip_count : 1;
	uint32_t rd_format = static_cast<uint32_t>(godot_format_to_rd_format(p_format));

	DSTexHeader header;
	header.magic = 0x58455444; // "DTEX"
	header.version = 1;
	header.flags = 0;
	header.width = static_cast<uint32_t>(p_width);
	header.height = static_cast<uint32_t>(p_height);
	header.pixel_format = rd_format;
	header.mip_count = static_cast<uint32_t>(mip_count);
	header.original_size = static_cast<uint64_t>(p_raw_data.size());
	header.compressed_size = static_cast<uint64_t>(compressed_data.size());
	header.reserved = 0;

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSTex: Could not open file for writing: ", p_path);
		return FileAccess::get_open_error();
	}

	file->store_buffer(reinterpret_cast<const uint8_t *>(&header), sizeof(DSTexHeader));

	uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntry) * mip_count);
	for (int i = 0; i < mip_count; ++i) {
		DSTexMipEntry mip_entry;
		mip_entry.offset = data_offset;
		mip_entry.size = (i == 0) ? static_cast<uint32_t>(compressed_data.size()) : 0;
		file->store_buffer(reinterpret_cast<const uint8_t *>(&mip_entry), sizeof(DSTexMipEntry));
	}

	file->store_buffer(compressed_data.ptr(), compressed_data.size());
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
	mip_offsets.resize(header.mip_count);
	mip_sizes.resize(header.mip_count);

	for (uint32_t i = 0; i < header.mip_count; ++i) {
		DSTexMipEntry mip_entry;
		PackedByteArray mip_bytes = file->get_buffer(sizeof(DSTexMipEntry));
		if (mip_bytes.size() == sizeof(DSTexMipEntry)) {
			memcpy(&mip_entry, mip_bytes.ptr(), sizeof(DSTexMipEntry));
			mip_offsets[i] = static_cast<int64_t>(mip_entry.offset);
			mip_sizes[i] = static_cast<int32_t>(mip_entry.size);
		}
	}

	uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntry) * header.mip_count);
	file->seek(data_offset);
	PackedByteArray compressed_data = file->get_buffer(static_cast<int64_t>(header.compressed_size));

	result["header"] = header_dict;
	result["compressed_data"] = compressed_data;
	result["mip_offsets"] = mip_offsets;
	result["mip_sizes"] = mip_sizes;

	file->close();
	return result;
}

Dictionary DSTex::get_file_info(const String &p_path) {
	return read_header(p_path);
}

} // namespace godot
