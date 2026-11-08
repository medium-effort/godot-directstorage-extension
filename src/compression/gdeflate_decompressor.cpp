#include "gdeflate_decompressor.h"
#include "gdeflate_parallel.h"
#include "../core/ds_error_handler.h"
#include "../formats/dstex_format.h"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <atomic>
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

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
std::atomic<int64_t> s_test_last_resize_request{ 0 };
#endif

// DEFLATE's maximum expansion is 1032:1: a 258-byte match costs two symbols,
// each at least one bit, so one compressed byte can produce at most 1032 bytes.
// A full 64 KiB tile therefore needs at least ceil(65536 / 1032) = 64 compressed
// bytes. 1100 is that bound with margin for block framing, so it admits every
// legal tile (threshold 60 <= 64) while refusing a payload that claims output
// it cannot encode.
constexpr size_t kMaxGDeflateExpansion = 1100;

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

bool GDeflateDecompressor::validate_stream(const PackedByteArray &p_compressed, int p_original_size) {
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
		const size_t next_offset = tile_index + 1 < header->numTiles ? tile_offsets[tile_index + 1] : (tile_offset + tile_offsets[0]);
		if (next_offset < tile_offset || tile_offset > compressed_data_size || next_offset > compressed_data_size) {
			return false;
		}
		// The header already fixes how many bytes this tile must produce. Reject
		// a tile that cannot encode them: a zero-length tile produces nothing,
		// and a payload may not claim more output than its compressed bytes can
		// expand to. This runs before any caller resizes a destination.
		const size_t tile_compressed = next_offset - tile_offset;
		if (tile_compressed == 0) {
			return false;
		}
		const size_t tile_output = std::min<size_t>(GDeflate::kDefaultTileSize,
				static_cast<size_t>(p_original_size) - static_cast<size_t>(tile_index) * GDeflate::kDefaultTileSize);
		const size_t min_compressed = (tile_output + kMaxGDeflateExpansion - 1) / kMaxGDeflateExpansion;
		if (tile_compressed < min_compressed) {
			return false;
		}
	}

	return true;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
int64_t GDeflateDecompressor::_get_last_resize_request_for_testing() {
	return s_test_last_resize_request.load();
}

void GDeflateDecompressor::_reset_last_resize_request_for_testing() {
	s_test_last_resize_request.store(0);
}
#endif

PackedByteArray GDeflateDecompressor::decompress(const PackedByteArray &p_compressed, int p_original_size) {
	if (p_compressed.size() == 0 || p_original_size <= 0) {
		return PackedByteArray();
	}
	if (static_cast<uint64_t>(p_original_size) > DSTEX_MAX_TOTAL_BYTES) {
		report_decompression_error("GDeflateDecompressor: Original size exceeds maximum allowed size (512 MiB).");
		return PackedByteArray();
	}
	if (!validate_stream(p_compressed, p_original_size)) {
		report_decompression_error("GDeflateDecompressor: Invalid stream, original size, or impossible expansion.");
		return PackedByteArray();
	}

	PackedByteArray output;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	s_test_last_resize_request.store(p_original_size);
#endif
	output.resize(p_original_size);

	const GDeflate::TileStream *header = reinterpret_cast<const GDeflate::TileStream *>(p_compressed.ptr());
	uint32_t effective_workers = 1;
	if (header && header->numTiles > 2) {
		effective_workers = std::min<uint32_t>(static_cast<uint32_t>(worker_count), header->numTiles / 2u);
	}

	bool ok = GDeflate::Decompress(
			output.ptrw(),
			static_cast<size_t>(p_original_size),
			p_compressed.ptr(),
			p_compressed.size(),
			effective_workers,
			&ds_gdeflate_parallel_for);

	if (!ok) {
		report_decompression_error("GDeflateDecompressor: Decompression failed.");
		return PackedByteArray();
	}

	return output;
}

} // namespace godot
