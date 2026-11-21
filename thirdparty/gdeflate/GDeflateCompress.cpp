/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corporation. All rights reserved.
 * Modifications Copyright 2026 MediumEffort. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "GDeflate.h"
#include "TileStream.h"
#include "Utils.h"
#include "config.h"
#include "gdeflate_cpu_dispatch.h"

#include <assert.h>
#include <libdeflate.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <thread> // hardware_concurrency() only: this file spawns no threads
#include <vector>

#if defined(_MSC_VER) && defined(_M_X64)
extern "C" {
LIBDEFLATEEXPORT struct libdeflate_gdeflate_compressor* LIBDEFLATEAPI
libdeflate_alloc_gdeflate_compressor_avx2_msvc(int compression_level);

LIBDEFLATEEXPORT void LIBDEFLATEAPI
libdeflate_free_gdeflate_compressor_avx2_msvc(struct libdeflate_gdeflate_compressor* comp);

LIBDEFLATEEXPORT size_t LIBDEFLATEAPI
libdeflate_gdeflate_compress_avx2_msvc(struct libdeflate_gdeflate_compressor* compressor,
        const void* in, size_t in_nbytes,
        struct libdeflate_gdeflate_out_page* out_pages, size_t out_npages);
}
#endif

namespace {

// An AVX2 compressor must be released by the matching AVX2 deallocator.
struct CompressorHandle
{
    libdeflate_gdeflate_compressor* ptr = nullptr;
    bool avx2 = false;

    CompressorHandle() = default;
    CompressorHandle(const CompressorHandle&) = delete;
    CompressorHandle& operator=(const CompressorHandle&) = delete;

    ~CompressorHandle()
    {
        if (ptr == nullptr)
            return;
#if defined(_MSC_VER) && defined(_M_X64)
        if (avx2)
        {
            libdeflate_free_gdeflate_compressor_avx2_msvc(ptr);
            return;
        }
#endif
        libdeflate_free_gdeflate_compressor(ptr);
    }
};

} // namespace

namespace GDeflate
{
    static constexpr uint32_t kMaxWorkers = 31;
    // 64 KiB tiles: 8 tiles = 512 KiB of work per runner, so a 4 MiB mip can
    // use the whole CPU budget instead of the old 2-runner (4 MiB/worker) floor.
    static constexpr uint32_t kMinTilesPerWorker = 8;

    static std::atomic<int> s_compressor_dispatch{ static_cast<int>(CompressorDispatch::Auto) };

    void set_compressor_dispatch(CompressorDispatch p_mode)
    {
        s_compressor_dispatch.store(static_cast<int>(p_mode));
    }

    CompressorDispatch get_compressor_dispatch()
    {
        return static_cast<CompressorDispatch>(s_compressor_dispatch.load());
    }

    bool is_avx2_compressor_available()
    {
#if defined(_MSC_VER) && defined(_M_X64)
        return has_avx2_support();
#else
        return false;
#endif
    }

    // CPU features cannot change at runtime: probe once on first use instead of
    // on every tile-compression job. The dispatch mode itself stays live below.
    static bool avx2_capability_cached()
    {
        static const bool supported = is_avx2_compressor_available();
        return supported;
    }

    static bool should_use_avx2_compressor()
    {
        const CompressorDispatch mode = get_compressor_dispatch();
        if (mode == CompressorDispatch::Baseline)
            return false;
        // Auto and explicit AVX2 both require a runtime-probed capable CPU.
        return avx2_capability_cached();
    }

    struct OutputStreamWrapper
    {
        uint8_t* ptr = nullptr;
        size_t size = 0;
        size_t pos = 0;

        template<typename T>
        void StreamOut(T* d, size_t n = 1)
        {
            const size_t dataSize = sizeof(T) * n;

            if (pos + dataSize > size)
            {
                printf("Fatal: stream overrun!\n");
                return;
            }

            memcpy(ptr + pos, d, dataSize);
            pos += dataSize;
        }

        bool SetPos(size_t n)
        {
            if (n > size)
            {
                printf("Fatal: stream overrun!\n");
                return false;
            }

            pos = n;

            return true;
        }
    };

    struct CompressionContext
    {
        const uint8_t* inputPtr;
        size_t inputSize;

        struct Tile
        {
            std::vector<uint8_t> data;
            size_t uncompressedSize = 0;
        };

        std::vector<Tile> tiles;

        std::atomic_uint32_t globalIndex;
        uint32_t numItems;

        std::atomic_bool failed;
    };

    // Tile work for one worker: claims tiles from the shared index until the stream is
    // exhausted. Free function (not a lambda) so an executor can call it through a
    // plain function pointer.
    static void TileCompressionJob(CompressionContext& context, uint32_t level)
    {
        size_t pageCount = 0;
        const size_t scratchSize = libdeflate_gdeflate_compress_bound(nullptr, kDefaultTileSize, &pageCount);
        // assert() is compiled out of release builds, and a zero-sized or multi-page
        // scratch buffer would be written past by libdeflate on the next call.
        if (scratchSize == 0 || pageCount != 1)
        {
            context.failed = true;
            return;
        }

        // Heap scratch instead of alloca(): a compression worker may run with little
        // remaining stack, and the size is only known here.
        std::vector<uint8_t> scratch(scratchSize);

        const bool use_avx2 = should_use_avx2_compressor();
        CompressorHandle compressor;
        compressor.avx2 = use_avx2;
#if defined(_MSC_VER) && defined(_M_X64)
        compressor.ptr = use_avx2
                ? libdeflate_alloc_gdeflate_compressor_avx2_msvc(level)
                : libdeflate_alloc_gdeflate_compressor(level);
#else
        compressor.ptr = libdeflate_alloc_gdeflate_compressor(level);
#endif
        if (!compressor.ptr)
        {
            context.failed = true;
            return;
        }

        while (true)
        {
            const uint32_t tileIndex = context.globalIndex.fetch_add(1, std::memory_order_relaxed);

            if (tileIndex >= context.numItems)
                break;

            const size_t tilePos = tileIndex * kDefaultTileSize;

            size_t remaining = context.inputSize - tilePos;
            size_t uncompressedSize = std::min<size_t>(remaining, kDefaultTileSize);

            auto& tile = context.tiles[tileIndex];
            tile.uncompressedSize = uncompressedSize;

            libdeflate_gdeflate_out_page compressedPage{scratch.data(), scratchSize};

            size_t result = 0;
#if defined(_MSC_VER) && defined(_M_X64)
            if (use_avx2)
            {
                result = libdeflate_gdeflate_compress_avx2_msvc(
                        compressor.ptr, context.inputPtr + tilePos, uncompressedSize,
                        &compressedPage, 1);
            }
            else
#endif
            {
                result = libdeflate_gdeflate_compress(
                        compressor.ptr, context.inputPtr + tilePos, uncompressedSize,
                        &compressedPage, 1);
            }

            if (result == 0)
            {
                context.failed = true;
                break;
            }

            tile.data.resize(compressedPage.nbytes);
            memcpy(tile.data.data(), compressedPage.data, compressedPage.nbytes);
        }
    }

    struct CompressJob
    {
        CompressionContext* context;
        uint32_t level;
    };

    static void TileCompressionWorker(void* p_user, uint32_t /*p_index*/)
    {
        CompressJob* job = static_cast<CompressJob*>(p_user);
        TileCompressionJob(*job->context, job->level);
    }

    static bool DoCompress(
        uint8_t* output,
        size_t* outputSize,
        const uint8_t* in,
        size_t inSize,
        uint32_t level,
        uint32_t flags,
        ParallelForFn p_parallelFor,
        uint32_t p_max_workers)
    {
        if (outputSize == nullptr || output == nullptr || in == nullptr || inSize == 0)
            return false;

        if (inSize > kDefaultTileSize * TileStream::kMaxTiles)
            return false;

        CompressionContext context{};

        context.inputPtr = in;
        context.inputSize = inSize;
        context.numItems = static_cast<uint32_t>((inSize + kDefaultTileSize - 1) / kDefaultTileSize);
        context.tiles.resize(context.numItems);
        context.failed = false;

        const uint32_t hardwareWorkers = std::min<uint32_t>(
                kMaxWorkers, std::max<uint32_t>(1u, std::thread::hardware_concurrency()));
        const uint32_t maxWorkers = p_max_workers == 0
                ? hardwareWorkers
                : std::min(hardwareWorkers, p_max_workers);

        // The parallel-for executor posts ALL p_workers as pool tasks (no inline
        // runner), so the total must stay within the CPU budget and the tile
        // count: 1 tile -> 1 worker, not upstream's "1 + 1 = 2"; large inputs
        // request at most maxWorkers, not maxWorkers + 1.
        const uint32_t workers = (flags & COMPRESS_SINGLE_THREAD)
            ? 1u
            : std::clamp(
                (context.numItems + kMinTilesPerWorker - 1) / kMinTilesPerWorker,
                1u,
                std::min(std::max(1u, context.numItems), maxWorkers));

        CompressJob job{ &context, level };
        if (workers > 1 && p_parallelFor != nullptr)
        {
            p_parallelFor(&job, &TileCompressionWorker, workers);
        }
        else
        {
            TileCompressionJob(context, level);
        }

        // Compression failed
        if (context.failed)
            return false;

        // Compression is done. Prepare the output stream.

        std::vector<uint32_t> tilePtrs;
        size_t dataPos = 0;

        for (auto const& tile : context.tiles)
        {
            tilePtrs.push_back(static_cast<uint32_t>(dataPos));
            dataPos += tile.data.size();
        }

        // tilePtrs[0] is used to store the size of the last tile; all the other
        // elements are offsets to the tile data.
        tilePtrs[0] = static_cast<uint32_t>(context.tiles.back().data.size());

        assert(tilePtrs.size() <= TileStream::kMaxTiles);
        assert(tilePtrs.size() == context.tiles.size());
        assert(tilePtrs.size() == context.numItems);

        OutputStreamWrapper outputStream;
        outputStream.ptr = output;
        outputStream.size = *outputSize;

        // calculate uncompressed size
        size_t uncompressedSize = tilePtrs.size() * kDefaultTileSize;
        size_t tailSize = context.inputSize - (tilePtrs.size() - 1) * kDefaultTileSize;
        if (tailSize < kDefaultTileSize)
            uncompressedSize -= kDefaultTileSize - tailSize;

        assert(uncompressedSize == inSize);

        TileStream header(uncompressedSize);

        assert(tilePtrs.size() == header.numTiles);

        outputStream.StreamOut(&header);
        outputStream.StreamOut(tilePtrs.data(), tilePtrs.size());

        size_t dataOffset = outputStream.pos;

        for (size_t i = 0; i < tilePtrs.size(); ++i)
        {
            CompressionContext::Tile const& tile = context.tiles[i];
            uint32_t tileOffset = (i == 0) ? 0 : tilePtrs[i];

            outputStream.SetPos(dataOffset + tileOffset);
            outputStream.StreamOut(tile.data.data(), tile.data.size());
        }

        *outputSize = outputStream.pos;

        return true;
    }

    size_t CompressBound(size_t size)
    {
        size_t numTiles = std::min(size_t(TileStream::kMaxTiles), (size + kDefaultTileSize - 1) / kDefaultTileSize);
        numTiles = std::max(size_t(1), numTiles);

        const size_t tileSize = kDefaultTileSize +
                                // Tile header. Ideally need to make it a part of compressor API.
                                (sizeof(uint32_t) + 4 * 208 + 4 * 8);

        return numTiles * tileSize + sizeof(TileStream) + sizeof(uint64_t);
    }

    bool Compress(uint8_t* output, size_t* outputSize, const uint8_t* in, size_t inSize, uint32_t level,
            uint32_t flags, ParallelForFn p_parallelFor, uint32_t p_max_workers)
    {
        return DoCompress(output, outputSize, in, inSize, level, flags, p_parallelFor, p_max_workers);
    }

} // namespace GDeflate
