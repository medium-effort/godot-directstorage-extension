#include "dspkg_format.h"
#include "dstex_format.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

namespace godot {

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
		uint16_t path_len = static_cast<uint16_t>(utf8_path.length());
		entry_table_size += (8 + 2 + path_len + 8 + 8 + 8 + 4 + 4 + 4 + 4);
	}

	uint64_t total_data_offset = header_size + entry_table_size;

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

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive: ", p_path);
		return FileAccess::get_open_error();
	}

	uint64_t file_len = file->get_length();

	if (file_len < sizeof(DSPkgHeader)) {
		UtilityFunctions::push_error("DSPkg: Archive file too small for header (length: ", file_len, ", expected: ", sizeof(DSPkgHeader), ")");
		return ERR_FILE_CORRUPT;
	}

	PackedByteArray header_bytes = file->get_buffer(sizeof(DSPkgHeader));
	if (header_bytes.size() < sizeof(DSPkgHeader)) {
		UtilityFunctions::push_error("DSPkg: Failed to read complete header bytes.");
		return ERR_FILE_CORRUPT;
	}
	memcpy(&loaded_header, header_bytes.ptr(), sizeof(DSPkgHeader));

	if (loaded_header.magic != 0x474B5044) {
		UtilityFunctions::push_error("DSPkg: Invalid magic number in archive.");
		return ERR_FILE_UNRECOGNIZED;
	}

	if (loaded_header.version != 1) {
		UtilityFunctions::push_error("DSPkg: Unsupported archive version: ", loaded_header.version);
		return ERR_FILE_UNRECOGNIZED;
	}

	if (loaded_header.flags != 0 || loaded_header.reserved != 0) {
		UtilityFunctions::push_error("DSPkg: Invalid flags or reserved fields in header.");
		return ERR_FILE_CORRUPT;
	}

	const uint64_t MIN_ENTRY_SIZE = 50;
	uint64_t entry_count = loaded_header.entry_count;

	if (entry_count > 0 && entry_count > (UINT64_MAX - sizeof(DSPkgHeader)) / MIN_ENTRY_SIZE) {
		UtilityFunctions::push_error("DSPkg: Oversized entry_count in header: ", entry_count);
		return ERR_FILE_CORRUPT;
	}

	uint64_t min_table_size = entry_count * MIN_ENTRY_SIZE;
	uint64_t min_data_offset = sizeof(DSPkgHeader) + min_table_size;

	if (file_len < min_data_offset) {
		UtilityFunctions::push_error("DSPkg: Archive truncated for declared entry count.");
		return ERR_FILE_CORRUPT;
	}

	if (loaded_header.data_offset < min_data_offset || loaded_header.data_offset > file_len) {
		UtilityFunctions::push_error("DSPkg: Invalid data_offset (", loaded_header.data_offset, ") in header.");
		return ERR_FILE_CORRUPT;
	}

	uint64_t current_pos = sizeof(DSPkgHeader);

	for (uint32_t i = 0; i < loaded_header.entry_count; ++i) {
		if (current_pos + 10 > loaded_header.data_offset) {
			UtilityFunctions::push_error("DSPkg: Entry ", i, " header extends past archive data_offset.");
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		DSPkgEntry entry;
		entry.path_hash = file->get_64();
		uint16_t path_len = file->get_16();
		current_pos += 10;

		if (path_len > (loaded_header.data_offset - current_pos) || (current_pos + path_len + 40) > loaded_header.data_offset) {
			UtilityFunctions::push_error("DSPkg: Entry ", i, " path or fields extend past archive data_offset.");
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		if (path_len > 0) {
			PackedByteArray path_bytes = file->get_buffer(path_len);
			if (path_bytes.size() < path_len) {
				UtilityFunctions::push_error("DSPkg: Failed to read path bytes for entry ", i);
				entries_by_path.clear();
				entries_by_hash.clear();
				return ERR_FILE_CORRUPT;
			}
			String path_str = String::utf8(reinterpret_cast<const char *>(path_bytes.ptr()), path_len);
			entry.path = path_str;
		}
		current_pos += path_len;

		entry.data_offset = file->get_64();
		entry.compressed_size = file->get_64();
		entry.original_size = file->get_64();
		entry.width = file->get_32();
		entry.height = file->get_32();
		entry.pixel_format = file->get_32();
		entry.mip_count = file->get_32();
		current_pos += 40;

		if (entry.width == 0 || entry.height == 0 || entry.mip_count == 0) {
			UtilityFunctions::push_error("DSPkg: Invalid entry dimensions in entry ", i);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		if (entry.compressed_size == 0 || entry.original_size == 0) {
			UtilityFunctions::push_error("DSPkg: Invalid entry sizes in entry ", i);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		if (entry.original_size > 0x7FFFFFFF) {
			UtilityFunctions::push_error("DSPkg: Entry original_size exceeds maximum allowed size: ", entry.original_size);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		if (entry.data_offset > UINT64_MAX - loaded_header.data_offset) {
			UtilityFunctions::push_error("DSPkg: Entry data_offset overflow in entry ", i);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		uint64_t abs_payload_offset = loaded_header.data_offset + entry.data_offset;
		if (entry.compressed_size > UINT64_MAX - abs_payload_offset) {
			UtilityFunctions::push_error("DSPkg: Entry compressed_size range overflow in entry ", i);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		uint64_t abs_payload_end = abs_payload_offset + entry.compressed_size;
		if (abs_payload_end > file_len) {
			UtilityFunctions::push_error("DSPkg: Entry payload extends past archive length in entry ", i);
			entries_by_path.clear();
			entries_by_hash.clear();
			return ERR_FILE_CORRUPT;
		}

		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash] = entry;
	}

	if (current_pos > loaded_header.data_offset) {
		UtilityFunctions::push_error("DSPkg: Entry table position exceeds data_offset.");
		entries_by_path.clear();
		entries_by_hash.clear();
		return ERR_FILE_CORRUPT;
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

	Ref<FileAccess> file = FileAccess::open(opened_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive file for reading entry data.");
		return result;
	}

	uint64_t file_len = file->get_length();
	if (UINT64_MAX - loaded_header.data_offset < entry.data_offset) {
		UtilityFunctions::push_error("DSPkg: Integer overflow calculating entry target offset.");
		file->close();
		return result;
	}
	uint64_t target_offset = loaded_header.data_offset + entry.data_offset;

	if (target_offset > file_len || entry.compressed_size > file_len - target_offset) {
		UtilityFunctions::push_error("DSPkg: Entry payload extends past end of file at read time.");
		file->close();
		return result;
	}

	file->seek(target_offset);
	PackedByteArray compressed_data = file->get_buffer(static_cast<int64_t>(entry.compressed_size));
	file->close();

	if (static_cast<uint64_t>(compressed_data.size()) < entry.compressed_size) {
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

	result["archive_path"] = opened_path;
	result["path"] = entry.path;
	result["data_offset"] = loaded_header.data_offset + entry.data_offset;
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
