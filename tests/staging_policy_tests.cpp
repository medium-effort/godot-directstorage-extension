// Low-VRAM staging auto-tune policy (DSContext::low_vram_staging_cap_mb).
//
// Scope: this helper is applied only to DISCRETE adapters when
// dsgd_extension/auto_tune_for_low_vram_gpu is true; integrated/UMA adapters
// stay under auto_tune_for_integrated_gpu (64 MiB staging + 256 queue).
//
// Invariants:
//  - 0 means "no cap" (unknown/zero VRAM, or a card that can spare the default);
//  - 2-8 GiB cards cap at 64 MiB, which still fits a 4096x4096 RGBA8 mip, so the
//    cap never invents a new load failure on a tier that can render one;
//  - the cap is reduction-only: a lower user setting passes through untouched.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"

#include <algorithm>
#include <cstdint>

namespace {

int64_t cap(int64_t p_dedicated_mb) {
	return static_cast<int64_t>(godot::DSContext::low_vram_staging_cap_mb(p_dedicated_mb));
}

} // namespace

GDEX_TEST(staging_policy, unknown_vram_is_never_capped) {
	GDEX_EXPECT_EQ(cap(0), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(cap(-1), static_cast<int64_t>(0));
}

GDEX_TEST(staging_policy, discrete_tiers) {
	GDEX_EXPECT_EQ(cap(1024), static_cast<int64_t>(32));
	GDEX_EXPECT_EQ(cap(2047), static_cast<int64_t>(32));
	GDEX_EXPECT_EQ(cap(2048), static_cast<int64_t>(64));
	GDEX_EXPECT_EQ(cap(4096), static_cast<int64_t>(64));
	GDEX_EXPECT_EQ(cap(6144), static_cast<int64_t>(64));
	GDEX_EXPECT_EQ(cap(7167), static_cast<int64_t>(64));
	// DXGI-safe 8 GiB boundary: real 8 GiB cards report ~7900-8060 MiB.
	GDEX_EXPECT_EQ(cap(7900), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(cap(8191), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(cap(8192), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(cap(24576), static_cast<int64_t>(0));
}

GDEX_TEST(staging_policy, cap_is_reduction_only) {
	// The init path stores min(configured, cap) and never raises.
	GDEX_EXPECT_EQ(static_cast<int64_t>(std::min<uint32_t>(16, godot::DSContext::low_vram_staging_cap_mb(4096))),
			static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(std::min<uint32_t>(512, godot::DSContext::low_vram_staging_cap_mb(4096))),
			static_cast<int64_t>(64));
	// A tier with no cap (0) leaves the configured value alone; the init path
	// applies std::min only when the cap is non-zero.
	const uint32_t uncapped = godot::DSContext::low_vram_staging_cap_mb(24576);
	GDEX_EXPECT_EQ(static_cast<int64_t>(uncapped == 0 ? 128 : std::min<uint32_t>(128, uncapped)),
			static_cast<int64_t>(128));
}
