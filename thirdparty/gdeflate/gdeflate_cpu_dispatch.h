/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 MediumEffort. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime CPU feature probes shared by the GDeflate compressor and decompressor.
 * Both MSVC /arch:AVX2 translation units are selected only after one of these
 * predicates passes; every other platform compiles the baseline libdeflate path.
 */

#pragma once

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>

namespace GDeflate {

inline bool has_avx2_support() {
	int cpu_info[4] = { 0, 0, 0, 0 };
	__cpuid(cpu_info, 0);
	if (cpu_info[0] < 7) {
		return false;
	}
	__cpuid(cpu_info, 1);
	if ((cpu_info[2] & (1 << 27)) == 0 || (cpu_info[2] & (1 << 28)) == 0) {
		return false; // OSXSAVE and AVX are both required for XGETBV.
	}
	const unsigned long long xcr0 = _xgetbv(0);
	if ((xcr0 & 0x6ULL) != 0x6ULL) {
		return false; // XMM and YMM state must be enabled by the OS.
	}
	__cpuidex(cpu_info, 7, 0);
	return (cpu_info[1] & (1 << 5)) != 0; // AVX2.
}

inline bool has_avx2_bmi2_support() {
	if (!has_avx2_support()) {
		return false;
	}
	int cpu_info[4] = { 0, 0, 0, 0 };
	__cpuidex(cpu_info, 7, 0);
	return (cpu_info[1] & (1 << 3)) != 0 && (cpu_info[1] & (1 << 8)) != 0; // BMI1 + BMI2.
}

} // namespace GDeflate

#else

namespace GDeflate {

inline bool has_avx2_support() {
	return false;
}

inline bool has_avx2_bmi2_support() {
	return false;
}

} // namespace GDeflate

#endif
