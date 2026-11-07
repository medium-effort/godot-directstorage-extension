#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include <godot_cpp/classes/ref.hpp>

// Expansion-bound cases for GDeflateDecompressor.
//
// The hostile streams are hand-built TileStreams: a header that declares 512 MiB
// of output over a tile table the payload cannot possibly expand to. The decoder
// must reject them before it sizes the destination. The DEBUG-only seam records
// the last resize request, because the return value alone cannot distinguish the
// two states: a stream that is rejected up front and one that allocates first
// and fails in the codec both return an empty array.

namespace {

constexpr int64_t kTileSize = 65536; // GDeflate::kDefaultTileSize
constexpr int64_t kClaimedSize = 512LL * 1024 * 1024; // DSTEX_MAX_TOTAL_BYTES
constexpr uint32_t kClaimedTiles = static_cast<uint32_t>(kClaimedSize / kTileSize); // 8192

godot::PackedByteArray make_tile_stream_header(uint32_t p_tiles, uint32_t p_last_tile_size) {
	godot::PackedByteArray header;
	header.resize(8);
	header.encode_u8(0, 4); // GDeflate::kGDeflateId
	header.encode_u8(1, 4 ^ 0xff); // magic
	header.encode_u16(2, static_cast<uint16_t>(p_tiles));
	header.encode_u32(4, 1u | (p_last_tile_size << 2)); // tileSizeIdx = 1, lastTileSize
	return header;
}

// Every tile offset is zero, so every tile is zero-length. This is the exact
// 32,776-byte shape from the audit finding.
godot::PackedByteArray make_zero_offset_stream() {
	godot::PackedByteArray payload = make_tile_stream_header(kClaimedTiles, 0);
	payload.resize(8 + static_cast<int64_t>(kClaimedTiles) * 4);
	return payload; // resize zero-fills the tile offsets
}

// One payload byte per tile. This passes the zero-length check and still claims
// 512 MiB, so only an expansion-ratio predicate rejects it.
godot::PackedByteArray make_one_byte_tile_stream() {
	godot::PackedByteArray payload = make_tile_stream_header(kClaimedTiles, 0);
	const int64_t table_start = 8;
	payload.resize(table_start + static_cast<int64_t>(kClaimedTiles) * 4 + static_cast<int64_t>(kClaimedTiles));
	// tile_offsets[0] is the size of the last tile; the rest are tile starts.
	payload.encode_u32(table_start, 1);
	for (uint32_t i = 1; i < kClaimedTiles; i++) {
		payload.encode_u32(table_start + static_cast<int64_t>(i) * 4, i);
	}
	return payload; // the 8192 payload bytes are zero
}

} // namespace

GDEX_TEST(gdeflate_expansion, hostile_zero_offset_stream_rejected_before_allocation) {
	godot::PackedByteArray payload = make_zero_offset_stream();
	GDEX_EXPECT_EQ(payload.size(), static_cast<int64_t>(32776));

	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();
	godot::GDeflateDecompressor::_reset_last_resize_request_for_testing();
	const godot::PackedByteArray result =
			decompressor->decompress(payload, static_cast<int>(kClaimedSize));

	GDEX_EXPECT_EQ(result.size(), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(godot::GDeflateDecompressor::_get_last_resize_request_for_testing(),
			static_cast<int64_t>(0));
}

GDEX_TEST(gdeflate_expansion, hostile_one_byte_tiles_rejected_before_allocation) {
	godot::PackedByteArray payload = make_one_byte_tile_stream();
	GDEX_EXPECT_EQ(payload.size(), static_cast<int64_t>(40968));

	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();
	godot::GDeflateDecompressor::_reset_last_resize_request_for_testing();
	const godot::PackedByteArray result =
			decompressor->decompress(payload, static_cast<int>(kClaimedSize));

	GDEX_EXPECT_EQ(result.size(), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(godot::GDeflateDecompressor::_get_last_resize_request_for_testing(),
			static_cast<int64_t>(0));
}

// Anti-over-tightening guard: the most compressible legal tile must still decode.
// The shipped codec emits about 152 bytes for 64 KiB of zeros, far above the
// predicate's 60-byte threshold.
GDEX_TEST(gdeflate_expansion, legal_compressible_tile_still_accepted) {
	godot::PackedByteArray zeros;
	zeros.resize(kTileSize);

	godot::Ref<godot::GDeflateCompressor> compressor;
	compressor.instantiate();
	godot::PackedByteArray compressed = compressor->compress(zeros, 12);
	GDEX_EXPECT_GT(compressed.size(), static_cast<int64_t>(0));
	GDEX_EXPECT_LT(compressed.size(), static_cast<int64_t>(1100));

	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();
	godot::GDeflateDecompressor::_reset_last_resize_request_for_testing();
	const godot::PackedByteArray result =
			decompressor->decompress(compressed, static_cast<int>(kTileSize));

	GDEX_EXPECT_EQ(result.size(), kTileSize);
	GDEX_EXPECT_TRUE(result == zeros);
	GDEX_EXPECT_EQ(godot::GDeflateDecompressor::_get_last_resize_request_for_testing(), kTileSize);
}

// Positive control for the seam: a real stream must record its own resize
// request, so the two "== 0" asserts above cannot pass vacuously.
GDEX_TEST(gdeflate_expansion, real_stream_records_its_resize_request) {
	godot::PackedByteArray original;
	original.resize(kTileSize * 2);
	for (int64_t i = 0; i < original.size(); i++) {
		original[i] = static_cast<uint8_t>((i * 17 + 5) % 256);
	}

	godot::Ref<godot::GDeflateCompressor> compressor;
	compressor.instantiate();
	godot::PackedByteArray compressed = compressor->compress(original, 1);
	GDEX_EXPECT_GT(compressed.size(), static_cast<int64_t>(0));

	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();
	godot::GDeflateDecompressor::_reset_last_resize_request_for_testing();
	const godot::PackedByteArray result =
			decompressor->decompress(compressed, static_cast<int>(original.size()));

	GDEX_EXPECT_TRUE(result == original);
	GDEX_EXPECT_EQ(godot::GDeflateDecompressor::_get_last_resize_request_for_testing(),
			static_cast<int64_t>(kTileSize * 2));
}
