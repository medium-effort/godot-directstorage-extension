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

bool checked_add(uint64_t p_left, uint64_t p_right, uint64_t &r_result) {
	if (p_right > UINT64_MAX - p_left) {
		return false;
	}
	r_result = p_left + p_right;
	return true;
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

} // namespace

void DSPkg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("create", "output_path"), &DSPkg::create);
	ClassDB::bind_method(D_METHOD("add_texture", "resource_path", "dstex_path"), &DSPkg::add_texture);
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

Error DSPkg::add_texture(const String &p_resource_path, const String &p_dstex_path) {
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
		if (!checked_add(DSPKG_FIXED_ENTRY_SIZE, path_len, entry_size) || !checked_add(entry_table_size, entry_size, entry_table_size)) {
			UtilityFunctions::push_error("DSPkg: Entry table size overflow.");
			return ERR_INVALID_PARAMETER;
		}
	}

	uint64_t total_data_offset = 0;
	if (!checked_add(header_size, entry_table_size, total_data_offset)) {
		UtilityFunctions::push_error("DSPkg: Archive header and entry table size overflow.");
		return ERR_INVALID_PARAMETER;
	}

	Ref<FileAccess> file = FileAccess::open(build_output_path, FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open file for writing: ", build_output_path);
		return FileAccess::get_open_error();
	}

	DSPkgHeader header;
	header.magic = 0x474B5044; // "DPKG"
	header.version = 1;
	header.flags = 0;
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

		running_data_offset += comp_size;
	}

	for (int i = 0; i < count; ++i) {
		Dictionary item = staging_entries[i];
		PackedByteArray data = item["compressed_data"];
		if (data.size() > 0) {
			file->store_buffer(data.ptr(), data.size());
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

	if (loaded_header.version != 1) {
		return fail(ERR_FILE_UNRECOGNIZED, "Unsupported archive version.");
	}

	if (loaded_header.flags != 0 || loaded_header.reserved != 0) {
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

		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash] = entry;
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
	if (!is_opened) {
		return false;
	}
	if (entries_by_path.has(p_resource_path)) {
		return true;
	}
	return entries_by_hash.has(p_resource_path.hash());
}

Dictionary DSPkg::read_texture(const String &p_resource_path) const {
	Dictionary result;
	if (!is_opened) {
		UtilityFunctions::push_error("DSPkg: Archive is not opened.");
		return result;
	}

	DSPkgEntry entry;
	if (entries_by_path.has(p_resource_path)) {
		entry = entries_by_path[p_resource_path];
	} else if (entries_by_hash.has(p_resource_path.hash())) {
		entry = entries_by_hash[p_resource_path.hash()];
	} else {
		UtilityFunctions::push_error("DSPkg: Texture path not found in archive: ", p_resource_path);
		return result;
	}
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

	return result;
}

Dictionary DSPkg::get_entry_info(const String &p_resource_path) const {
	Dictionary result;
	if (!is_opened) {
		UtilityFunctions::push_error("DSPkg: Archive is not opened.");
		return result;
	}

	DSPkgEntry entry;
	if (entries_by_path.has(p_resource_path)) {
		entry = entries_by_path[p_resource_path];
	} else if (entries_by_hash.has(p_resource_path.hash())) {
		entry = entries_by_hash[p_resource_path.hash()];
	} else {
		UtilityFunctions::push_error("DSPkg: Texture path not found in archive: ", p_resource_path);
		return result;
	}
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

	return result;
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
