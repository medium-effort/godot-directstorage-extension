// Finding 1.3 Phase A: mode-aware request budgets. Pure arithmetic tests of
// the shared helper used by DSContext, DSPreloader and Win32Backend.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_request_budget.h"

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
