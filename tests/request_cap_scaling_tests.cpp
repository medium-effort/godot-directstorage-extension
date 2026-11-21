// Finding 1.3 Phase A: mode-aware request budgets. Pure arithmetic tests of
// the shared helper used by DSContext, DSPreloader and Win32Backend.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "core/ds_request_budget.h"

#include <cstdint>

GDEX_TEST(request_cap_scaling, per_flush_loading_scales_at_quarter_capacity) {
	const uint16_t capacities[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
	const int64_t expected[] = { 62, 126, 128, 256, 512, 1024, 1024 };
	for (int i = 0; i < 7; ++i) {
		GDEX_EXPECT_EQ(static_cast<int64_t>(dsgd_budget::per_flush_request_budget(capacities[i], false)), expected[i]);
	}
}

GDEX_TEST(request_cap_scaling, per_flush_background_clamps_to_128) {
	const uint16_t capacities[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
	const int64_t expected[] = { 62, 126, 128, 128, 128, 128, 128 };
	for (int i = 0; i < 7; ++i) {
		GDEX_EXPECT_EQ(static_cast<int64_t>(dsgd_budget::per_flush_request_budget(capacities[i], true)), expected[i]);
	}
}

GDEX_TEST(request_cap_scaling, per_flush_and_subresource_never_exceed_slot_ceiling) {
	const uint16_t capacities[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
	for (uint16_t cap : capacities) {
		const int64_t ceiling = static_cast<int64_t>(dsgd_budget::per_slot_request_ceiling(cap));
		GDEX_EXPECT_LE(static_cast<int64_t>(dsgd_budget::per_flush_subresource_budget(cap)), ceiling);
		GDEX_EXPECT_LE(static_cast<int64_t>(dsgd_budget::per_flush_request_budget(cap, false)), ceiling);
		GDEX_EXPECT_LE(static_cast<int64_t>(dsgd_budget::per_flush_request_budget(cap, true)), ceiling);
	}
}

GDEX_TEST(request_cap_scaling, gpu_callback_request_budget_mode_split) {
	const uint16_t capacities[] = { 128, 256, 512, 1024, 2048, 4096, 8192 };
	const int64_t bg_expected[] = { 62, 64, 64, 64, 64, 64, 64 };
	const int64_t loading_expected[] = { 62, 64, 64, 128, 256, 512, 1024 };
	for (int i = 0; i < 7; ++i) {
		GDEX_EXPECT_EQ(static_cast<int64_t>(dsgd_budget::gpu_callback_request_budget(capacities[i], true)), bg_expected[i]);
		GDEX_EXPECT_EQ(static_cast<int64_t>(dsgd_budget::gpu_callback_request_budget(capacities[i], false)), loading_expected[i]);
		GDEX_EXPECT_LE(static_cast<int64_t>(dsgd_budget::gpu_callback_request_budget(capacities[i], false)),
				static_cast<int64_t>(dsgd_budget::per_slot_request_ceiling(capacities[i])));
	}
}

// Phase B RED: the byte cap must stay at 16 MiB for background pacing,
// integrated GPUs, and discrete GPUs below the DXGI-safe 8 GiB threshold.
GDEX_TEST(request_cap_scaling, gpu_byte_budget_conservative_tiers) {
	constexpr uint64_t mib = 1024ULL * 1024ULL;
	// Background pacing: always 2 MiB, whatever the hardware.
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 4004, false, true), 2ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(128, 16384, false, true), 2ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 4004, true, true), 2ULL * mib);
	// Integrated or low-VRAM discrete: 16 MiB, unchanged from Phase A.
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 4004, false, false), 16ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(128, 4004, false, false), 16ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 6144, false, false), 16ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(128, 16384, true, false), 16ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 0, false, false), 16ULL * mib);
}

GDEX_TEST(request_cap_scaling, gpu_byte_budget_high_vram_scales_with_staging) {
	constexpr uint64_t mib = 1024ULL * 1024ULL;
	// >= 7168 MiB discrete DXGI-safe tier: staging/2 clamped to [16, 64] MiB.
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(32, 7900, false, false), 16ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(64, 7900, false, false), 32ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(128, 16384, false, false), 64ULL * mib);
	GDEX_EXPECT_EQ(dsgd_budget::gpu_callback_byte_budget(256, 16384, false, false), 64ULL * mib);
}

// No-regression proof on this host: feed the REAL adapter/staging values into
// the pure function and assert the shipped binary still resolves to 16 MiB.
GDEX_TEST_T(request_cap_scaling, gpu_byte_budget_is_unchanged_on_this_host, TAG_INTEGRATION) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	GDEX_ASSERT_TRUE(ds != nullptr);
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (byte budget is hardware-independent).");
	}
	const uint64_t resolved = dsgd_budget::gpu_callback_byte_budget(
			ds->get_staging_buffer_size_mb(), ds->get_dedicated_vram_mb(),
			ds->is_integrated_gpu(), false);
	GDEX_EXPECT_EQ(resolved, 16ULL * 1024ULL * 1024ULL);
}
