#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include "compression/gdeflate_parallel.h"
#include "core/ds_cpu_budget.h"
#include "GDeflate.h"
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
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
//   void/int GDeflateDecompressor::set/get_worker_count() (clamp 1..ds_get_max_cpu_threads())

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

GDEX_TEST(gdeflate, worker_count_clamps_to_cpu_budget) {
    const int ceiling = godot::ds_get_max_cpu_threads();
    GDEX_ASSERT_TRUE(ceiling >= 1);
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    // Default is the resolved CPU budget (fallback 4); assert the clamped range.
    GDEX_EXPECT_GE(decompressor->get_worker_count(), 1);
    GDEX_EXPECT_LE(decompressor->get_worker_count(), ceiling);
    decompressor->set_worker_count(0);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), 1);
    decompressor->set_worker_count(99);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), ceiling);
    decompressor->set_worker_count(2);
    GDEX_EXPECT_EQ(decompressor->get_worker_count(), 2 <= ceiling ? 2 : ceiling);
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

// A container's declared size is what sizes the destination; the stream header is
// the only thing that says how many bytes it can produce. A stream that declares
// fewer bytes than the destination must be rejected, never decoded into a
// partially written buffer that is then reported as success.
GDEX_TEST(gdeflate, short_stream_rejected_for_larger_destination) {
    // One tile of payload: the stream header declares exactly kTileSize bytes.
    godot::PackedByteArray original = make_pattern(kTileSize);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    if (compressed.size() == 0) {
        return;
    }

    // The destination a request hands the codec when its container entry claims
    // two tiles of output for a one-tile payload.
    godot::PackedByteArray destination;
    destination.resize(kTileSize * 2);
    for (int64_t i = 0; i < destination.size(); i++) {
        destination[i] = 0xCD;
    }
    const godot::PackedByteArray canary = destination;

    const bool ok = GDeflate::Decompress(destination.ptrw(),
            static_cast<size_t>(destination.size()), compressed.ptr(),
            static_cast<size_t>(compressed.size()), 1);

    // Discriminator: a stream that cannot fill the destination is rejected.
    GDEX_EXPECT_FALSE(ok);
    // Rejection is up front: nothing was decoded into the destination either.
    GDEX_EXPECT_TRUE(destination == canary);

    // Control: the same stream is accepted into a destination it does fill, so
    // this case cannot pass by rejecting everything.
    godot::PackedByteArray exact;
    exact.resize(original.size());
    GDEX_EXPECT_TRUE(GDeflate::Decompress(exact.ptrw(), static_cast<size_t>(exact.size()),
            compressed.ptr(), static_cast<size_t>(compressed.size()), 1));
    GDEX_EXPECT_TRUE(exact == original);
}

namespace {

struct PoolDecompressJob {
    godot::PackedByteArray *compressed = nullptr;
    godot::PackedByteArray *destination = nullptr;
    bool ok = false;
};

void run_pool_decompress(void *p_user) {
    PoolDecompressJob *job = static_cast<PoolDecompressJob *>(p_user);
    job->ok = GDeflate::Decompress(job->destination->ptrw(),
            static_cast<size_t>(job->destination->size()), job->compressed->ptr(),
            static_cast<size_t>(job->compressed->size()), 4, &godot::ds_gdeflate_parallel_for);
}

} // namespace

// Tile work runs on whatever executor the caller supplies. A caller that is itself a
// worker-pool task is the shape that can stall an engine pool, so it gets a case:
// decompression driven from inside a pool task has to finish and produce the same
// bytes as the inline path.
GDEX_TEST(gdeflate, decompress_runs_inside_worker_thread_pool_task) {
    godot::WorkerThreadPool *pool = godot::WorkerThreadPool::get_singleton();
    if (pool == nullptr) {
        GDEX_SKIP("WorkerThreadPool singleton unavailable");
    }
    // Eight tiles so the codec actually fans out (numItems > 2 * numWorkers).
    godot::PackedByteArray original = make_pattern(kTileSize * 8);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    if (compressed.size() == 0) {
        return;
    }
    godot::PackedByteArray destination;
    destination.resize(original.size());
    PoolDecompressJob job{ &compressed, &destination, false };
    const int64_t task = pool->add_native_task(&run_pool_decompress, &job, false, "ds gdeflate test");
    GDEX_EXPECT_NE(task, static_cast<int64_t>(godot::WorkerThreadPool::INVALID_TASK_ID));
    if (task == static_cast<int64_t>(godot::WorkerThreadPool::INVALID_TASK_ID)) {
        return;
    }
    pool->wait_for_task_completion(task);
    GDEX_EXPECT_TRUE(job.ok);
    GDEX_EXPECT_TRUE(destination == original);
}

// Regression test for libdeflate_gdeflate_compress_bound() omitting the GDeflate
// stream-interleaving slack when HIDE_INTERFACE suppresses deflate_compress.c's
// #ifdef GDEFLATE bound. A 64 KiB tile whose dynamic Huffman cost sits just below
// the 65536-byte two-block uncompressed threshold (~524344 bits) picks
// DYNAMIC_HUFFMAN and writes past 65708 bytes of scratch space.
GDEX_TEST(gdeflate, borderline_incompressible_full_tile_round_trip) {
    godot::PackedByteArray tile;
    tile.resize(kTileSize);
    uint8_t *dst = tile.ptrw();
    uint32_t state = 0x9E3779B9u;
    for (int i = 0; i < kTileSize; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint8_t v = static_cast<uint8_t>((state >> 24) & 0xFFu);
        // ~1.5% bias toward 0x80 puts dynamic Huffman cost right near 524,300 bits
        // on a 65536-byte tile (similar to high-entropy RGTC_RG normal maps).
        dst[i] = ((state & 0x3Fu) == 0u) ? 0x80u : v;
    }

    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();

    for (int level : { 1, 3, 5 }) {
        const godot::PackedByteArray compressed = compressor->compress(tile, level);
        GDEX_EXPECT_GT(compressed.size(), 0);
        if (compressed.size() == 0) {
            continue;
        }
        const godot::PackedByteArray decompressed = decompressor->decompress(compressed, kTileSize);
        GDEX_EXPECT_EQ(decompressed.size(), kTileSize);
        GDEX_EXPECT_TRUE(decompressed == tile);
    }
}

// Audit 3.2: the old 64-tiles-per-worker floor let a 4 MiB (64-tile) mip fan out
// to only 2 workers even on a many-core CPU. Assert the dispatched worker count
// scales with the shared budget, then prove the finer tiling still round-trips.
GDEX_TEST(gdeflate, compress_small_mip_fans_out_across_cpu_budget) {
    if (godot::ds_get_max_cpu_threads() < 4) {
        GDEX_SKIP("Shared CPU budget too small to prove multi-worker fan-out");
    }
    godot::PackedByteArray original = make_pattern(kTileSize * 64);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    godot::ds_reset_parallel_for_workers_for_testing();
    const godot::PackedByteArray compressed = compressor->compress(original, 1);
    GDEX_EXPECT_GT(compressed.size(), 0);
    if (compressed.size() == 0) {
        return;
    }
    GDEX_EXPECT_GT(godot::ds_get_last_parallel_for_workers_for_testing(), 2u);

    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    const godot::PackedByteArray decompressed =
            decompressor->decompress(compressed, static_cast<int>(original.size()));
    GDEX_EXPECT_EQ(decompressed.size(), original.size());
    GDEX_EXPECT_TRUE(decompressed == original);
}

// Audit 3.2: the MSVC /arch:AVX2 compressor must produce byte-identical streams,
// otherwise switching dispatch would silently change archive contents.
GDEX_TEST(gdeflate, avx2_compressor_matches_baseline_bytes) {
    if (!GDeflate::is_avx2_compressor_available()) {
        GDEX_SKIP("AVX2 compressor unavailable on this host");
    }
    const GDeflate::CompressorDispatch prior = GDeflate::get_compressor_dispatch();
    godot::PackedByteArray original = make_pattern(kTileSize * 4);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();

    for (int level : { 1, 3, 5 }) {
        GDeflate::set_compressor_dispatch(GDeflate::CompressorDispatch::Baseline);
        const godot::PackedByteArray baseline = compressor->compress(original, level);
        GDeflate::set_compressor_dispatch(GDeflate::CompressorDispatch::Avx2);
        const godot::PackedByteArray avx2 = compressor->compress(original, level);
        GDeflate::set_compressor_dispatch(prior);

        GDEX_EXPECT_GT(baseline.size(), 0);
        GDEX_EXPECT_EQ(avx2.size(), baseline.size());
        GDEX_EXPECT_TRUE(avx2 == baseline);
        if (avx2.size() == 0) {
            continue;
        }
        godot::Ref<godot::GDeflateDecompressor> decompressor;
        decompressor.instantiate();
        const godot::PackedByteArray decompressed =
                decompressor->decompress(avx2, static_cast<int>(original.size()));
        GDEX_EXPECT_EQ(decompressed.size(), original.size());
        GDEX_EXPECT_TRUE(decompressed == original);
    }
}

// The #0275 bound fix must hold in the AVX2 copy too: a borderline-incompressible
// full tile selects dynamic Huffman and needs the stream-interleaving slack.
GDEX_TEST(gdeflate, avx2_borderline_tile_round_trip) {
    if (!GDeflate::is_avx2_compressor_available()) {
        GDEX_SKIP("AVX2 compressor unavailable on this host");
    }
    godot::PackedByteArray tile;
    tile.resize(kTileSize);
    uint8_t *dst = tile.ptrw();
    uint32_t state = 0x9E3779B9u;
    for (int i = 0; i < kTileSize; ++i) {
        state = state * 1664525u + 1013904223u;
        const uint8_t v = static_cast<uint8_t>((state >> 24) & 0xFFu);
        dst[i] = ((state & 0x3Fu) == 0u) ? 0x80u : v;
    }

    const GDeflate::CompressorDispatch prior = GDeflate::get_compressor_dispatch();
    GDeflate::set_compressor_dispatch(GDeflate::CompressorDispatch::Avx2);
    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    const godot::PackedByteArray compressed = compressor->compress(tile, 3);
    GDeflate::set_compressor_dispatch(prior);

    GDEX_EXPECT_GT(compressed.size(), 0);
    if (compressed.size() == 0) {
        return;
    }
    godot::Ref<godot::GDeflateDecompressor> decompressor;
    decompressor.instantiate();
    const godot::PackedByteArray decompressed = decompressor->decompress(compressed, kTileSize);
    GDEX_EXPECT_EQ(decompressed.size(), kTileSize);
    GDEX_EXPECT_TRUE(decompressed == tile);
}
