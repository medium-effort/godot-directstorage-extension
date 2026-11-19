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
#include "gdeflate_cpu_dispatch.h"

#include <libdeflate.h>

#include <algorithm>
#include <atomic>
#include <memory>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>

extern "C" {
struct libdeflate_gdeflate_decompressor* LIBDEFLATEAPI
libdeflate_alloc_gdeflate_decompressor_bmi2_msvc(void);

void LIBDEFLATEAPI
libdeflate_free_gdeflate_decompressor_bmi2_msvc(struct libdeflate_gdeflate_decompressor* d);

enum libdeflate_result LIBDEFLATEAPI
libdeflate_gdeflate_decompress_bmi2_msvc(struct libdeflate_gdeflate_decompressor* d,
                                         struct libdeflate_gdeflate_in_page* in_pages,
                                         size_t in_npages, void* out,
                                         size_t out_nbytes_avail,
                                         size_t* actual_out_nbytes_ret);
}

static bool use_bmi2_msvc_decoder()
{
    static const bool s_supported = GDeflate::has_avx2_bmi2_support();
    return s_supported;
}
#endif

template<>
struct std::default_delete<libdeflate_gdeflate_decompressor>
{
    void operator()(libdeflate_gdeflate_decompressor* p) const
    {
#if defined(_MSC_VER) && defined(_M_X64)
        if (use_bmi2_msvc_decoder())
        {
            libdeflate_free_gdeflate_decompressor_bmi2_msvc(p);
            return;
        }
#endif
        libdeflate_free_gdeflate_decompressor(p);
    }
};

namespace GDeflate
{
    static constexpr uint32_t kMaxWorkers = 31;

    static bool ValidateStream(const TileStream* header)
    {
        if (!header->IsValid())
        {
            return false;
        }

        if (header->id != kGDeflateId)
        {
            return false;
        }

        if (header->tileSizeIdx != 1 || header->numTiles == 0 || header->lastTileSize > kDefaultTileSize)
        {
            return false;
        }

        return true;
    }

    struct DecompressionContext
    {
        const uint8_t* inputPtr;
        size_t inputSize;

        uint8_t* outputPtr;
        size_t outputSize;

        std::atomic_uint32_t globalIndex;
        uint32_t numItems;

        std::atomic_bool failed;
    };

    static void TileDecompressionJob(DecompressionContext& context, uint32_t compressorId)
    {
#if defined(_MSC_VER) && defined(_M_X64)
        const bool use_bmi2 = use_bmi2_msvc_decoder();
        std::unique_ptr<libdeflate_gdeflate_decompressor> decompressor(
            use_bmi2 ? libdeflate_alloc_gdeflate_decompressor_bmi2_msvc()
                     : libdeflate_alloc_gdeflate_decompressor());
#else
        std::unique_ptr<libdeflate_gdeflate_decompressor> decompressor(libdeflate_alloc_gdeflate_decompressor());
#endif
        if (!decompressor)
        {
            // Allocation failure is a clean failure, not a null decompressor handed to
            // libdeflate (which only checks its page arguments, not this pointer).
            context.failed = true;
            return;
        }

        const uint32_t* tileOffsets = reinterpret_cast<const uint32_t*>(context.inputPtr + sizeof(TileStream));
        const uint8_t* inDataPtr = reinterpret_cast<const uint8_t*>(tileOffsets + context.numItems);

        libdeflate_result decompressResult = LIBDEFLATE_SUCCESS;
        while (true)
        {
            const uint32_t tileIndex = context.globalIndex.fetch_add(1, std::memory_order_relaxed);

            if (tileIndex >= context.numItems)
                break;

            const size_t tileOffset = tileIndex > 0 ? tileOffsets[tileIndex] : 0;
            if (tileIndex < context.numItems - 1 && tileOffsets[tileIndex + 1] < tileOffset) {
                context.failed = true;
                break;
            }
            const size_t tileNbytes =
                tileIndex < context.numItems - 1 ? tileOffsets[tileIndex + 1] - tileOffset : tileOffsets[0];

            // Bound the input page: corrupt tile tables must fail, never OOB-read.
            const size_t dataStart = static_cast<size_t>(inDataPtr - context.inputPtr);
            const size_t dataAvail = dataStart <= context.inputSize ? context.inputSize - dataStart : 0;
            if (tileOffset > dataAvail || tileNbytes > dataAvail - tileOffset) {
                context.failed = true;
                break;
            }
            libdeflate_gdeflate_in_page compressedPage{};
            compressedPage.data = inDataPtr + tileOffset;
            compressedPage.nbytes = tileNbytes;

            // Bound the output: the last tile is usually partial, so a full
            // kDefaultTileSize write heap-overflows when corrupt input decodes
            // past the real end (SIGSEGV on macOS; latent on Windows).
            const size_t outputOffset = static_cast<size_t>(tileIndex) * kDefaultTileSize;
            if (outputOffset >= context.outputSize) {
                context.failed = true;
                break;
            }
            const size_t outputAvail = std::min(static_cast<size_t>(kDefaultTileSize),
                    context.outputSize - outputOffset);

#if defined(_MSC_VER) && defined(_M_X64)
            decompressResult = use_bmi2
                ? libdeflate_gdeflate_decompress_bmi2_msvc(
                    decompressor.get(),
                    &compressedPage,
                    1,
                    context.outputPtr + outputOffset,
                    outputAvail,
                    nullptr)
                : libdeflate_gdeflate_decompress(
                    decompressor.get(),
                    &compressedPage,
                    1,
                    context.outputPtr + outputOffset,
                    outputAvail,
                    nullptr);
#else
            decompressResult = libdeflate_gdeflate_decompress(
                decompressor.get(),
                &compressedPage,
                1,
                context.outputPtr + outputOffset,
                outputAvail,
                nullptr);
#endif

            if ( decompressResult != LIBDEFLATE_SUCCESS)
            {
                context.failed = true;
                break;
            }
        }
    }

    struct DecompressJob
    {
        DecompressionContext* context;
        uint32_t compressorId;
    };

    static void TileDecompressionWorker(void* p_user, uint32_t /*p_index*/)
    {
        DecompressJob* job = static_cast<DecompressJob*>(p_user);
        TileDecompressionJob(*job->context, job->compressorId);
    }

    bool Decompress(uint8_t* output, size_t outputSize, const uint8_t* in, size_t inSize, uint32_t numWorkers, ParallelForFn p_parallelFor)
    {
        if (nullptr == output || nullptr == in || 0 == outputSize || inSize < sizeof(TileStream))
            return false;

        numWorkers = std::min(kMaxWorkers, numWorkers);
        numWorkers = std::max(1u, numWorkers);

        auto header = reinterpret_cast<const TileStream*>(in);

        if (!ValidateStream(header))
            return false;

        const size_t tileTableSize = sizeof(TileStream) + static_cast<size_t>(header->numTiles) * sizeof(uint32_t);
        if (inSize < tileTableSize)
            return false;

        const size_t uncompressedSize = header->GetUncompressedSize();
        // The caller sizes its destination from the uncompressed size it declared, and
        // this stream is the only thing that says how many bytes it can produce. A
        // stream that declares fewer bytes than the destination is truncated or
        // hostile: decoding it would leave the tail of the destination untouched while
        // reporting success. The hardware path rejects the same mismatch before
        // submission and GDeflateDecompressor::validate_stream() rejects it before this
        // function is reached; the codec must not be the weakest of the three.
        if (uncompressedSize == 0 || outputSize != uncompressedSize) {
            return false;
        }

        DecompressionContext context{};

        context.inputPtr = in;
        context.inputSize = inSize;

        context.outputPtr = output;
        context.outputSize = uncompressedSize;

        context.globalIndex = 0;
        context.numItems = header->numTiles;

        context.failed = false;

        const uint32_t compressorId = header->id;

        // Tiles are claimed from an atomic index, so the worker count only decides how
        // much of the stream decodes in parallel; one runner always covers all tiles.
        const uint32_t workers = context.numItems > (2 * numWorkers) ? numWorkers : 1;

        DecompressJob job{ &context, compressorId };
        if (workers > 1 && p_parallelFor != nullptr)
        {
            p_parallelFor(&job, &TileDecompressionWorker, workers);
        }
        else
        {
            TileDecompressionJob(context, compressorId);
        }

        return (!context.failed);
    }
} // namespace GDeflate
