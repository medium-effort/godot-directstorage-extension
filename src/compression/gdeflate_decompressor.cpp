#include "gdeflate_decompressor.h"
#include "../core/ds_error_handler.h"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "GDeflate.h"
#include "TileStream.h"

namespace godot {

namespace {

void report_decompression_error(const String &p_message) {
	UtilityFunctions::push_error(p_message);
	DSErrorHandler *error_handler = DSErrorHandler::get_singleton();
	if (error_handler) {
		error_handler->report_error(p_message);
	}
}

bool validate_stream(const PackedByteArray &p_compressed, int p_original_size) {
	if (p_compressed.size() < static_cast<int64_t>(sizeof(GDeflate::TileStream))) {
		return false;
	}

	const GDeflate::TileStream *header = reinterpret_cast<const GDeflate::TileStream *>(p_compressed.ptr());
	if (!header->IsValid() || header->id != GDeflate::kGDeflateId || header->tileSizeIdx != 1 || header->numTiles == 0 ||
			header->lastTileSize > GDeflate::kDefaultTileSize || header->GetUncompressedSize() != static_cast<size_t>(p_original_size)) {
		return false;
	}

	const size_t tile_table_size = sizeof(GDeflate::TileStream) + static_cast<size_t>(header->numTiles) * sizeof(uint32_t);
	if (tile_table_size > static_cast<size_t>(p_compressed.size())) {
		return false;
	}

	const uint32_t *tile_offsets = reinterpret_cast<const uint32_t *>(p_compressed.ptr() + sizeof(GDeflate::TileStream));
	const size_t compressed_data_size = static_cast<size_t>(p_compressed.size()) - tile_table_size;
	for (uint32_t tile_index = 0; tile_index < header->numTiles; tile_index++) {
		const size_t tile_offset = tile_index > 0 ? tile_offsets[tile_index] : 0;
		const size_t next_offset = tile_index + 1 < header->numTiles ? tile_offsets[tile_index + 1] : tile_offsets[0];
		if (next_offset < tile_offset || tile_offset > compressed_data_size || next_offset > compressed_data_size) {
			return false;
		}
	}

	return true;
}

} // namespace

void GDeflateDecompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("decompress", "compressed", "original_size"), &GDeflateDecompressor::decompress);
	ClassDB::bind_method(D_METHOD("set_worker_count", "count"), &GDeflateDecompressor::set_worker_count);
	ClassDB::bind_method(D_METHOD("get_worker_count"), &GDeflateDecompressor::get_worker_count);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "worker_count"), "set_worker_count", "get_worker_count");
}

GDeflateDecompressor::GDeflateDecompressor() {
	OS *os = OS::get_singleton();
	if (os) {
		int cpus = os->get_processor_count();
		if (cpus < 1) {
			worker_count = 1;
		} else if (cpus > 8) {
			worker_count = 8;
		} else {
			worker_count = cpus;
		}
	} else {
		worker_count = 4;
	}
}

GDeflateDecompressor::~GDeflateDecompressor() {
}

void GDeflateDecompressor::set_worker_count(int p_count) {
	if (p_count < 1) {
		worker_count = 1;
	} else if (p_count > 8) {
		worker_count = 8;
	} else {
		worker_count = p_count;
	}
}

int GDeflateDecompressor::get_worker_count() const {
	return worker_count;
}

PackedByteArray GDeflateDecompressor::decompress(const PackedByteArray &p_compressed, int p_original_size) {
	if (p_compressed.size() == 0 || p_original_size <= 0) {
		return PackedByteArray();
	}
	if (!validate_stream(p_compressed, p_original_size)) {
		report_decompression_error("GDeflateDecompressor: Invalid stream or original size.");
		return PackedByteArray();
	}

	PackedByteArray output;
	output.resize(p_original_size);

	bool ok = GDeflate::Decompress(
			output.ptrw(),
			static_cast<size_t>(p_original_size),
			p_compressed.ptr(),
			p_compressed.size(),
			static_cast<uint32_t>(worker_count));

	if (!ok) {
		report_decompression_error("GDeflateDecompressor: Decompression failed.");
		return PackedByteArray();
	}

	return output;
}

} // namespace godot
