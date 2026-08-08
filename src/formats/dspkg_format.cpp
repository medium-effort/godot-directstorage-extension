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

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		UtilityFunctions::push_error("DSPkg: Could not open archive: ", p_path);
		return FileAccess::get_open_error();
	}

	if (file->get_length() < sizeof(DSPkgHeader)) {
		UtilityFunctions::push_error("DSPkg: Archive file too small for header.");
		return ERR_FILE_CORRUPT;
	}

	PackedByteArray header_bytes = file->get_buffer(sizeof(DSPkgHeader));
	if (header_bytes.size() < sizeof(DSPkgHeader)) {
		return ERR_FILE_CORRUPT;
	}
	memcpy(&loaded_header, header_bytes.ptr(), sizeof(DSPkgHeader));

	if (loaded_header.magic != 0x474B5044) {
		UtilityFunctions::push_error("DSPkg: Invalid magic number in archive.");
		return ERR_FILE_UNRECOGNIZED;
	}

	for (uint32_t i = 0; i < loaded_header.entry_count; ++i) {
		DSPkgEntry entry;
		entry.path_hash = file->get_64();
		uint16_t path_len = file->get_16();

		if (path_len > 0) {
			PackedByteArray path_bytes = file->get_buffer(path_len);
			String path_str = String::utf8(reinterpret_cast<const char *>(path_bytes.ptr()), path_len);
			entry.path = path_str;
		}

		entry.data_offset = file->get_64();
		entry.compressed_size = file->get_64();
		entry.original_size = file->get_64();
		entry.width = file->get_32();
		entry.height = file->get_32();
		entry.pixel_format = file->get_32();
		entry.mip_count = file->get_32();

		entries_by_path[entry.path] = entry;
		entries_by_hash[entry.path_hash] = entry;
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

	uint64_t target_offset = loaded_header.data_offset + entry.data_offset;
	file->seek(target_offset);
	PackedByteArray compressed_data = file->get_buffer(static_cast<int64_t>(entry.compressed_size));
	file->close();

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
