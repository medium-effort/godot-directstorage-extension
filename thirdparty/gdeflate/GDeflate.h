/*
 * SPDX-FileCopyrightText: Copyright (c) 2020, 2021, 2022 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-FileCopyrightText: Copyright (c) Microsoft Corportaion. All rights reserved.
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

#pragma once

#include "config.h"

namespace GDeflate
{
    // See README.MD in libdeflate_1_8 for details on Compression Levels
    static const uint32_t MinimumCompressionLevel = 1;
    static const uint32_t MaximumCompressionLevel = 12;

    enum Flags
    {
        COMPRESS_SINGLE_THREAD = 0x200, /*!< Force compression using a single thread. */
    };

    // Runs p_job(p_user, p_index) for every p_index in [0, p_workers) and returns only
    // after all invocations finished. The codecs never create threads of their own: with
    // p_parallelFor == nullptr the work runs inline on the calling thread, otherwise the
    // caller's executor decides where it runs and how many workers it really gets. Both
    // codecs claim work from an atomic index, so an executor with fewer real workers is
    // slow but always correct.
    using ParallelForFn = void (*)(void* p_user, void (*p_job)(void*, uint32_t), uint32_t p_workers);

    size_t CompressBound(size_t size);

    bool Compress(
        uint8_t* output,
        size_t* outputSize,
        const uint8_t* in,
        size_t inSize,
        uint32_t level,
        uint32_t flags,
        ParallelForFn p_parallelFor = nullptr);

    bool Decompress(
        uint8_t* output,
        size_t outputSize,
        const uint8_t* in,
        size_t inSize,
        uint32_t numWorkers,
        ParallelForFn p_parallelFor = nullptr);

} // namespace GDeflate
