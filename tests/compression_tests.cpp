#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include <godot_cpp/core/memory.hpp>

// C++ mirror of project/headless_tests/test_gdeflate.gd (authoritative).
// Pure-C++ GDEX_TEST cases; no engine singletons, no consumer_extension.
// Compressor/decompressor symbols compile INTO the test library via
// [gdextest.tests] sources (src/compression/*.cpp + thirdparty gdeflate +
// libdeflate + src/core/ds_error_handler.cpp) -- compile-in linkage decision.
//
// Real APIs under test (src/compression/gdeflate_compressor.h,
// src/compression/gdeflate_decompressor.h -- verified):
//   PackedByteArray GDeflateCompressor::compress(input, level = -1)
//   int GDeflateCompressor::compress_bound(input_size) const
//   void/int GDeflateCompressor::set/get_compression_level() (clamp 1..12, default 1)
//   PackedByteArray GDeflateDecompressor::decompress(compressed, original_size)
//   void/int GDeflateDecompressor::set/get_worker_count() (clamp 1..8)

namespace {

constexpr int64_t kTileSize = 65536; // GDeflate::kDefaultTileSize, thirdparty/gdeflate/config.h

godot::PackedByteArray make_pattern(int64_t p_size) {
    godot::PackedByteArray data;
    data.resize(p_size);
    for (int64_t i = 0; i < p_size; i++) {
        data[i] = static_cast<uint8_t>((i * 17 + 5) % 256);
    }
    return data;
}

// GDeflateCompressor/GDeflateDecompressor derive from RefCounted (a Godot
// "Wrapped" type), so they MUST be managed via godot::Ref and instantiated
// via instantiate(). Never use raw pointers with memdelete(), which bypasses
// RefCounted reference counting and causes heap corruption/SIGSEGV.
bool check_round_trip(const godot::PackedByteArray &p_original, int p_level) {
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(p_original, p_level);
    if (p_original.size() == 0) {
        return compressed.size() == 0;
    }
    if (compressed.size() == 0) {
        return false;
    }
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    godot::PackedByteArray decompressed = decompressor->decompress(compressed, static_cast<int>(p_original.size()));
    return decompressed.size() == p_original.size() && decompressed == p_original;
}

} // namespace

// MUST-pass gate: byte-for-byte round-trip identity, same 1000-byte pattern
// as test_gdeflate.gd.
GDEX_TEST(gdeflate, roundtrip_1000_byte_gdscript_pattern) {
    godot::PackedByteArray original = make_pattern(1000);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 1);
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    godot::PackedByteArray decompressed =
            decompressor->decompress(compressed, static_cast<int>(original.size()));
    GDEX_EXPECT_EQ(decompressed.size(), original.size());
    GDEX_EXPECT_TRUE(decompressed == original);
}

GDEX_TEST(gdeflate, roundtrip_levels_1_6_12) {
    godot::PackedByteArray original = make_pattern(4096);
    const int levels[] = { 1, 6, 12 };
    for (int level : levels) {
        GDEX_EXPECT_TRUE(check_round_trip(original, level));
    }
}

GDEX_TEST(gdeflate, roundtrip_uses_member_level_by_default) {
    godot::PackedByteArray original = make_pattern(2048);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    compressor->set_compression_level(6);
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 6);
    godot::PackedByteArray compressed = compressor->compress(original);
    GDEX_EXPECT_GT(compressed.size(), 0);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    godot::PackedByteArray decompressed =
            decompressor->decompress(compressed, static_cast<int>(original.size()));
    GDEX_EXPECT_TRUE(decompressed == original);
}

GDEX_TEST(gdeflate, roundtrip_highly_compressible_zeros_shrink) {
    godot::PackedByteArray original;
    original.resize(kTileSize);
    // Zero-filled: memset-equivalent loop (PackedByteArray::resize zero-inits,
    // loop makes the intent explicit).
    for (int64_t i = 0; i < original.size(); i++) {
        original[i] = 0;
    }
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    GDEX_EXPECT_LT(compressed.size(), original.size());
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    GDEX_EXPECT_TRUE(
            decompressor->decompress(compressed, static_cast<int>(original.size())) == original);
}

// SHOULD-pass: tile-boundary expectations around kDefaultTileSize (64 KiB).
// Round-trip identity must hold for exact-tile, off-by-one, and multi-tile
// inputs. Report any upstream mismatch verbatim; do not weaken these.
GDEX_TEST(gdeflate, roundtrip_tile_boundary_sizes) {
    const int64_t sizes[] = { kTileSize - 1, kTileSize, kTileSize + 1, 2 * kTileSize };
    for (int64_t size : sizes) {
        godot::PackedByteArray original = make_pattern(size);
        GDEX_EXPECT_TRUE(check_round_trip(original, 1));
    }
}

GDEX_TEST(gdeflate, compress_bound_covers_output) {
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    GDEX_EXPECT_EQ(compressor->compress_bound(0), 0);
    GDEX_EXPECT_EQ(compressor->compress_bound(-5), 0);
    const int64_t sizes[] = { 1, 1000, 4096, kTileSize };
    for (int64_t size : sizes) {
        godot::PackedByteArray original = make_pattern(size);
        godot::PackedByteArray compressed = compressor->compress(original, 1);
        GDEX_EXPECT_GT(compressed.size(), 0);
        GDEX_EXPECT_GE(compressor->compress_bound(static_cast<int>(size)), compressed.size());
    }
}

GDEX_TEST(gdeflate, compression_level_clamps_1_to_12) {
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 1);
    compressor->set_compression_level(0);
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 1);
    compressor->set_compression_level(-3);
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 1);
    compressor->set_compression_level(99);
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 12);
    compressor->set_compression_level(6);
    GDEX_EXPECT_EQ(compressor->get_compression_level(), 6);
}

GDEX_TEST(gdeflate, worker_count_clamps_1_to_8) {
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    // Default is CPU-dependent (fallback 4); only the clamped range is asserted.
    GDEX_EXPECT_GE(decompressor->get_worker_count(), 1);
    GDEX_EXPECT_LE(decompressor->get_worker_count(), 8);
    decompressor->set_worker_count(0);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), 1);
    decompressor->set_worker_count(99);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), 8);
    decompressor->set_worker_count(2);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), 2);
}

GDEX_TEST(gdeflate, empty_input_yields_empty) {
    godot::PackedByteArray empty;
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    GDEX_EXPECT_EQ(compressor->compress(empty, 1).size(), 0);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    GDEX_EXPECT_EQ(decompressor->decompress(empty, 100).size(), 0);
    godot::PackedByteArray original = make_pattern(64);
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    GDEX_EXPECT_EQ(decompressor->decompress(compressed, 0).size(), 0);
}

GDEX_TEST(gdeflate, wrong_original_size_yields_empty) {
    godot::PackedByteArray original = make_pattern(1000);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    GDEX_EXPECT_EQ(
            decompressor->decompress(compressed, static_cast<int>(original.size()) + 1).size(), 0);
    GDEX_EXPECT_EQ(
            decompressor->decompress(compressed, static_cast<int>(original.size()) - 1).size(), 0);
}

// Corrupt the trailing payload byte; header/tile table stay valid.
// GDeflate tiles are raw DEFLATE with no per-stream checksum, so a single
// flipped payload byte is not guaranteed to be detected: validate_stream()
// only checks header/tile-table metadata, and GDeflate::Decompress() may still
// report success. The invariant this suite actually owns is NO CRASH and a
// well-formed result size (either the empty failure sentinel or the full
// requested size) - never a partial/oversized buffer. Payload integrity
// detection is not a property of the current decompressor.
GDEX_TEST(gdeflate, corrupt_payload_no_crash_wellformed_size) {
    godot::PackedByteArray original = make_pattern(4096);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    godot::PackedByteArray corrupted = compressed;
    corrupted[corrupted.size() - 1] = static_cast<uint8_t>(corrupted[corrupted.size() - 1] ^ 0xFF);
    const int64_t result_size =
            decompressor->decompress(corrupted, static_cast<int>(original.size())).size();
    GDEX_EXPECT_TRUE(result_size == 0 || result_size == original.size());
}

GDEX_TEST(gdeflate, truncated_stream_yields_empty_no_crash) {
    godot::PackedByteArray original = make_pattern(4096);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 16);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    godot::PackedByteArray truncated = compressed;
    truncated.resize(compressed.size() / 2);
    GDEX_EXPECT_EQ(
            decompressor->decompress(truncated, static_cast<int>(original.size())).size(), 0);
}
