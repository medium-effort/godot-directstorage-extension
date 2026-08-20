#include "dspkg_format.h"
#include "dstex_format.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

namespace godot {

namespace {

constexpr uint64_t DSPKG_FIXED_ENTRY_SIZE = 50;
constexpr uint64_t DSPKG_ENTRY_PREFIX_SIZE = 10;
constexpr uint64_t DSPKG_ENTRY_FIELDS_SIZE = 40;
constexpr uint64_t DSPKG_MAX_PAYLOAD_SIZE = static_cast<uint64_t>(INT32_MAX);
constexpr uint16_t DSPKG_VERSION_LEGACY = 1;
constexpr uint16_t DSPKG_VERSION_GPU_METADATA = 2;
constexpr uint16_t DSPKG_FLAG_GPU_MIP_METADATA = 1;
constexpr uint32_t D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256;
constexpr uint32_t D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT = 512;

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

bool range_is_within(uint64_t p_start, uint64_t p_size, uint64_t p_end) {
	return p_start <= p_end && p_size <= p_end - p_start;
}

bool get_payload_range(const DSPkgHeader &p_header, const DSPkgEntry &p_entry, uint64_t &r_start, uint64_t &r_end) {
	return checked_add(p_header.data_offset, p_entry.data_offset, r_start) && checked_add(r_start, p_entry.compressed_size, r_end);
}

bool payload_sizes_are_supported(const DSPkgEntry &p_entry) {
	return p_entry.compressed_size > 0 && p_entry.original_size > 0 &&
			p_entry.compressed_size <= DSPKG_MAX_PAYLOAD_SIZE && p_entry.original_size <= DSPKG_MAX_PAYLOAD_SIZE;
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

} // namespace

void DSPkg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "output_path"), &DSPkg::create);
	ClassDB::bind_method(D_METHOD("add_texture", "resource_path", "dstex_path", "mip_metadata"), &DSPkg::add_texture, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("finalize"), &DSPkg::finalize);

	ClassDB::bind_method(D_METHOD("open", "path"), &DSPkg::open);
	ClassDB::bind_method(D_METHOD("has_texture", "resource_path"), &DSPkg::has_texture);
	ClassDB::bind_method(D_METHOD("read_texture", "resource_path"), &DSPkg::read_texture);
	ClassDB::bind_method(D_METHOD("get_entry_info", "resource_path"), &DSPkg::get_entry_info);
	ClassDB::bind_method(D_METHOD("get_entry_count"), &DSPkg::get_entry_count);
	ClassDB::bind_method(D_METHOD("get_entries"), &DSPkg::get_entries);
}

DSPkg::DSPkg() {
}

DSPkg::~DSPkg() {
}

Error DSPkg::create(const String &p_output_path) {
	build_output_path = p_output_path;
	staging_entries.clear();
	is_building = true;
	return OK;
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

	uint32_t count = static_cast<uint32_t>(staging_entries.size());
	if (count == 0) {
		UtilityFunctions::push_error("DSPkg: No textures added to archive.");
		return ERR_UNCONFIGURED;
	}

	bool write_gpu_metadata = false;
	for (int i = 0; i < count; ++i) {
		Dictionary item = staging_entries[i];
		if (item.get("gpu_metadata", false)) {
			write_gpu_metadata = true;
			break;
		}
	}

	uint64_t header_size = sizeof(DSPkgHeader);
	uint64_t entry_table_size = 0;

	for (int i = 0; i < count; ++i) {
		Dictionary item = staging_entries[i];
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
					!checked_add(DSPKG_FIXED_ENTRY_SIZE, metadata_size, entry_size))) {
			UtilityFunctions::push_error("DSPkg: Mip metadata table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
		if (!write_gpu_metadata && !checked_add(DSPKG_FIXED_ENTRY_SIZE, 0, entry_size)) {
			UtilityFunctions::push_error("DSPkg: Entry table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
		if (!checked_add(entry_size, path_len, entry_size) || !checked_add(entry_table_size, entry_size, entry_table_size)) {
			UtilityFunctions::push_error("DSPkg: Entry table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
	}

	uint64_t table_end_offset = 0;
	if (!checked_add(header_size, entry_table_size, table_end_offset)) {
		UtilityFunctions::push_error("DSPkg: Archive header and entry table size overflow.");
		return ERR_INVALID_PARAMETER;
	}
	uint64_t total_data_offset = 0;
	if (!align_up(table_end_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, total_data_offset)) {
		UtilityFunctions::push_error("DSPkg: Archive data offset alignment overflow.");
		return ERR_INVALID_PARAMETER;
	}

	Ref<FileAccess> file = FileAccess::open(build_output_path, FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open file for writing: ", build_output_path);
		return FileAccess::get_open_error();
	}

	DSPkgHeader header;
	header.magic = 0x474B5044; // "DPKG"
	header.version = write_gpu_metadata ? DSPKG_VERSION_GPU_METADATA : DSPKG_VERSION_LEGACY;
	header.flags = write_gpu_metadata ? DSPKG_FLAG_GPU_MIP_METADATA : 0;
	header.entry_count = count;
	header.data_offset = total_data_offset;
	header.reserved = 0;

	file->store_buffer(reinterpret_cast<const uint8_t *>(&header), sizeof(DSPkgHeader));

	uint64_t running_data_offset = 0;
	for (int i = 0; i < count; ++i) {
		Dictionary item = staging_entries[i];
		String res_path = item["resource_path"];
		CharString utf8_path = res_path.utf8();
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		uint64_t hash = res_path.hash();

		Dictionary dstex_header = item["header"];
		PackedByteArray data = item["compressed_data"];

		uint64_t comp_size = static_cast<uint64_t>(data.size());
		uint64_t orig_size = dstex_header.get("original_size", 0);
		uint32_t w = dstex_header.get("width", 0);
		uint32_t h = dstex_header.get("height", 0);
		uint32_t fmt = dstex_header.get("pixel_format", 0);
		uint32_t mips = dstex_header.get("mip_count", 0);
		if (!align_up(running_data_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, running_data_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload data offset alignment overflow.");
			file->close();
			return ERR_INVALID_PARAMETER;
		}
		PackedInt32Array mip_sizes = item.get("mip_sizes", PackedInt32Array());
		PackedInt32Array mip_uncompressed_sizes = item.get("mip_uncompressed_sizes", PackedInt32Array());
		PackedInt32Array mip_row_pitches = item.get("mip_row_pitches", PackedInt32Array());
		if (write_gpu_metadata && mip_sizes.size() != static_cast<int64_t>(mips)) {
			UtilityFunctions::push_error("DSPkg: Texture mip metadata does not match the declared mip count.");
			file->close();
			return ERR_INVALID_DATA;
		}
		if (write_gpu_metadata && item.get("gpu_metadata", false) &&
				(mip_uncompressed_sizes.size() != static_cast<int64_t>(mips) ||
					mip_row_pitches.size() != static_cast<int64_t>(mips))) {
			UtilityFunctions::push_error("DSPkg: Incomplete GPU footprint metadata.");
			file->close();
			return ERR_INVALID_DATA;
		}

		file->store_64(hash);
		file->store_16(path_len);
		if (path_len > 0) {
			file->store_buffer(reinterpret_cast<const uint8_t *>(utf8_path.get_data()), path_len);
		}
		file->store_64(running_data_offset);
		file->store_64(comp_size);
		file->store_64(orig_size);
		file->store_32(w);
		file->store_32(h);
		file->store_32(fmt);
		file->store_32(mips);

		if (write_gpu_metadata) {
			uint64_t mip_data_offset = 0;
			if (!checked_add(total_data_offset, running_data_offset, mip_data_offset)) {
				UtilityFunctions::push_error("DSPkg: Mip data offset overflow.");
				file->close();
				return ERR_INVALID_PARAMETER;
			}
			uint64_t mip_offset = 0;
			for (uint32_t mip = 0; mip < mips; ++mip) {
				if (mip_sizes[mip] <= 0) {
					UtilityFunctions::push_error("DSPkg: Invalid compressed mip metadata.");
					file->close();
					return ERR_INVALID_DATA;
				}
				uint32_t mip_size = static_cast<uint32_t>(mip_sizes[mip]);
				if (mip_size == 0 || !checked_add(mip_offset, mip_size, mip_offset) || mip_offset > comp_size) {
					UtilityFunctions::push_error("DSPkg: Invalid compressed mip metadata.");
					file->close();
					return ERR_INVALID_DATA;
				}
			}
			if (mip_offset != comp_size) {
				UtilityFunctions::push_error("DSPkg: Compressed mip sizes do not match the payload size.");
				file->close();
				return ERR_INVALID_DATA;
			}
			mip_offset = 0;
			for (uint32_t mip = 0; mip < mips; ++mip) {
				DSPkgMipEntry mip_entry{};
				mip_entry.offset = mip_data_offset + mip_offset;
				mip_entry.compressed_size = mip_sizes[mip];
				if (item.get("gpu_metadata", false)) {
					mip_entry.uncompressed_size = mip_uncompressed_sizes[mip];
					mip_entry.row_pitch = mip_row_pitches[mip];
					if (mip_entry.uncompressed_size == 0 || mip_entry.row_pitch == 0 ||
							mip_entry.row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0) {
						UtilityFunctions::push_error("DSPkg: Invalid D3D12 footprint metadata.");
						file->close();
						return ERR_INVALID_DATA;
					}
				}
				file->store_buffer(reinterpret_cast<const uint8_t *>(&mip_entry), sizeof(DSPkgMipEntry));
				mip_offset += mip_entry.compressed_size;
			}
		}

		if (!checked_add(running_data_offset, comp_size, running_data_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload data offset overflow.");
			file->close();
			return ERR_INVALID_PARAMETER;
		}
	}

	uint64_t payload_offset = table_end_offset;
	for (int i = 0; i < count; ++i) {
		Dictionary item = staging_entries[i];
		PackedByteArray data = item["compressed_data"];
		uint64_t aligned_payload_offset = 0;
		if (!align_up(payload_offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, aligned_payload_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload file offset alignment overflow.");
			file->close();
			return ERR_INVALID_PARAMETER;
		}
		for (uint64_t padding = payload_offset; padding < aligned_payload_offset; ++padding) {
			file->store_8(0);
		}
		payload_offset = aligned_payload_offset;
		if (data.size() > 0) {
			file->store_buffer(data.ptr(), data.size());
		}
		if (!checked_add(payload_offset, static_cast<uint64_t>(data.size()), payload_offset)) {
			UtilityFunctions::push_error("DSPkg: Payload file offset overflow.");
			file->close();
			return ERR_INVALID_PARAMETER;
		}
	}

	file->close();
	is_building = false;
	staging_entries.clear();

	return OK;
}

Error DSPkg::open(const String &p_path) {
	entries_by_path.clear();
	entries_by_hash.clear();
	is_opened = false;
	opened_path = "";
	loaded_header = DSPkgHeader{};

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive: ", p_path);
		return FileAccess::get_open_error();
	}

	auto fail = [&](Error p_error, const String &p_message) -> Error {
		entries_by_path.clear();
		entries_by_hash.clear();
		is_opened = false;
		opened_path = "";
		loaded_header = DSPkgHeader{};
		file->close();
		UtilityFunctions::push_error("DSPkg: ", p_message);
		return p_error;
	};

	uint64_t file_len = file->get_length();

	if (file_len < sizeof(DSPkgHeader)) {
		return fail(ERR_FILE_CORRUPT, "Archive file too small for header.");
	}

	PackedByteArray header_bytes = file->get_buffer(sizeof(DSPkgHeader));
	if (header_bytes.size() < sizeof(DSPkgHeader)) {
		return fail(ERR_FILE_CORRUPT, "Failed to read complete header bytes.");
	}
	memcpy(&loaded_header, header_bytes.ptr(), sizeof(DSPkgHeader));

	if (loaded_header.magic != 0x474B5044) {
		return fail(ERR_FILE_UNRECOGNIZED, "Invalid magic number in archive.");
	}

	if (loaded_header.version != DSPKG_VERSION_LEGACY && loaded_header.version != DSPKG_VERSION_GPU_METADATA) {
		return fail(ERR_FILE_UNRECOGNIZED, "Unsupported archive version.");
	}

	const bool has_gpu_metadata = loaded_header.version == DSPKG_VERSION_GPU_METADATA;
	if ((has_gpu_metadata && loaded_header.flags != DSPKG_FLAG_GPU_MIP_METADATA) ||
			(!has_gpu_metadata && loaded_header.flags != 0) || loaded_header.reserved != 0) {
		return fail(ERR_FILE_CORRUPT, "Invalid flags or reserved fields in header.");
	}

	uint64_t entry_count = loaded_header.entry_count;
	uint64_t min_table_size = 0;
	uint64_t min_data_offset = 0;
	if (!checked_multiply(entry_count, DSPKG_FIXED_ENTRY_SIZE, min_table_size) ||
			!checked_add(sizeof(DSPkgHeader), min_table_size, min_data_offset)) {
		return fail(ERR_FILE_CORRUPT, "Entry table size overflow.");
	}

	if (file_len < min_data_offset) {
		return fail(ERR_FILE_CORRUPT, "Archive truncated for declared entry count.");
	}

	if (loaded_header.data_offset < min_data_offset || loaded_header.data_offset > file_len) {
		return fail(ERR_FILE_CORRUPT, "Invalid data_offset in header.");
	}

	uint64_t current_pos = sizeof(DSPkgHeader);

	for (uint32_t i = 0; i < loaded_header.entry_count; ++i) {
		if (!range_is_within(current_pos, DSPKG_ENTRY_PREFIX_SIZE, loaded_header.data_offset)) {
			return fail(ERR_FILE_CORRUPT, "Entry header extends past archive data_offset.");
		}

		DSPkgEntry entry;
		entry.path_hash = file->get_64();
		uint16_t path_len = file->get_16();
		if (!checked_add(current_pos, DSPKG_ENTRY_PREFIX_SIZE, current_pos)) {
			return fail(ERR_FILE_CORRUPT, "Entry header offset overflow.");
		}

		if (path_len == 0 || !range_is_within(current_pos, path_len, loaded_header.data_offset)) {
			return fail(ERR_FILE_CORRUPT, "Entry path extends past archive data_offset or is empty.");
		}

		uint64_t path_end = 0;
		if (!checked_add(current_pos, path_len, path_end) || !range_is_within(path_end, DSPKG_ENTRY_FIELDS_SIZE, loaded_header.data_offset)) {
			return fail(ERR_FILE_CORRUPT, "Entry path or fixed fields extend past archive data_offset.");
		}

		PackedByteArray path_bytes = file->get_buffer(path_len);
		if (path_bytes.size() < path_len) {
			return fail(ERR_FILE_CORRUPT, "Failed to read complete entry path.");
		}
		entry.path = String::utf8(reinterpret_cast<const char *>(path_bytes.ptr()), path_len);
		current_pos = path_end;

		entry.data_offset = file->get_64();
		entry.compressed_size = file->get_64();
		entry.original_size = file->get_64();
		entry.width = file->get_32();
		entry.height = file->get_32();
		entry.pixel_format = file->get_32();
		entry.mip_count = file->get_32();
		if (!checked_add(current_pos, DSPKG_ENTRY_FIELDS_SIZE, current_pos)) {
			return fail(ERR_FILE_CORRUPT, "Entry fixed fields offset overflow.");
		}

		if (entry.width == 0 || entry.height == 0 || entry.mip_count == 0) {
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

		if (has_gpu_metadata) {
			uint64_t metadata_size = 0;
			if (!checked_multiply(entry.mip_count, sizeof(DSPkgMipEntry), metadata_size) ||
					!range_is_within(current_pos, metadata_size, loaded_header.data_offset)) {
				return fail(ERR_FILE_CORRUPT, "Entry mip metadata extends past archive data_offset.");
			}
			entry.mip_offsets.resize(entry.mip_count);
			entry.mip_sizes.resize(entry.mip_count);
			entry.mip_uncompressed_sizes.resize(entry.mip_count);
			entry.mip_row_pitches.resize(entry.mip_count);
			uint64_t footprint_size = 0;
			uint64_t compressed_mip_size = 0;
			for (uint32_t mip = 0; mip < entry.mip_count; ++mip) {
				DSPkgMipEntry mip_entry{};
				mip_entry.offset = file->get_64();
				mip_entry.compressed_size = file->get_32();
				mip_entry.uncompressed_size = file->get_32();
				mip_entry.row_pitch = file->get_32();
				mip_entry.reserved = file->get_32();
				entry.mip_offsets[mip] = static_cast<int64_t>(mip_entry.offset);
				entry.mip_sizes[mip] = static_cast<int32_t>(mip_entry.compressed_size);
				entry.mip_uncompressed_sizes[mip] = static_cast<int32_t>(mip_entry.uncompressed_size);
				entry.mip_row_pitches[mip] = static_cast<int32_t>(mip_entry.row_pitch);
				uint64_t mip_end = 0;
				if (mip_entry.compressed_size == 0 ||
						mip_entry.reserved != 0 ||
						!checked_add(mip_entry.offset, mip_entry.compressed_size, mip_end) ||
						mip_entry.offset < abs_payload_offset || mip_end > abs_payload_end ||
						!checked_add(compressed_mip_size, mip_entry.compressed_size, compressed_mip_size) ||
						(mip_entry.uncompressed_size > 0 &&
							(mip_entry.row_pitch == 0 || mip_entry.row_pitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT != 0)) ||
						(mip_entry.uncompressed_size > 0 &&
							!checked_add(footprint_size, mip_entry.uncompressed_size, footprint_size))) {
					return fail(ERR_FILE_CORRUPT, "Invalid entry mip metadata.");
				}
			}
			if (compressed_mip_size != entry.compressed_size) {
				return fail(ERR_FILE_CORRUPT, "Entry mip sizes do not match compressed size.");
			}
			entry.has_gpu_metadata = footprint_size > 0;
			if (entry.has_gpu_metadata && footprint_size != entry.original_size) {
				return fail(ERR_FILE_CORRUPT, "Entry footprint metadata does not match original size.");
			}
			if (!checked_add(current_pos, metadata_size, current_pos)) {
				return fail(ERR_FILE_CORRUPT, "Entry mip metadata offset overflow.");
			}
		}

		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash].push_back(entry);
	}

	if (current_pos > loaded_header.data_offset) {
		return fail(ERR_FILE_CORRUPT, "Entry table position exceeds data_offset.");
	}

	file->close();
	opened_path = p_path;
	is_opened = true;

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
	result["has_gpu_metadata"] = entry.has_gpu_metadata;
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

int DSPkg::get_entry_count() const {
	if (!is_opened) {
		return 0;
	}
	return static_cast<int>(loaded_header.entry_count);
}

Array DSPkg::get_entries() const {
	Array result;
	if (!is_opened) {
		return result;
	}
	for (const KeyValue<String, DSPkgEntry> &E : entries_by_path) {
		result.append(E.key);
	}
	return result;
}

} // namespace godot
