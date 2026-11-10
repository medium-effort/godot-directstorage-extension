#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_frame_window.h"

#include <cmath>

// Frame-time impact is the only metric that answers "did streaming cause a
// hitch"; byte-throughput numbers cannot. The aggregation is engine-free, so
// these cases run identically on every platform.

namespace {

bool near(double p_a, double p_b) {
	return std::fabs(p_a - p_b) < 1e-6;
}

} // namespace

GDEX_TEST(frame_window, means_cover_before_and_during) {
	godot::DSFrameWindowStats stats;
	for (int i = 0; i < godot::DSFrameWindowStats::kBaselineCapacity; i++) {
		stats.record_frame(10000.0);
	}
	GDEX_EXPECT_TRUE(near(stats.mean_before_usec(), 10000.0));
	GDEX_EXPECT_FALSE(stats.is_active());

	stats.begin_window();
	GDEX_EXPECT_TRUE(stats.is_active());
	for (int i = 0; i < 10; i++) {
		stats.record_frame(20000.0);
	}
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 20000.0));
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(10));
	// The idle baseline is frozen while the window is open.
	GDEX_EXPECT_TRUE(near(stats.mean_before_usec(), 10000.0));

	stats.end_window();
	GDEX_EXPECT_FALSE(stats.is_active());
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 20000.0));
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(10));
	GDEX_EXPECT_EQ(stats.completed_windows(), static_cast<int64_t>(1));
}

GDEX_TEST(frame_window, baseline_keeps_only_recent_idle_frames) {
	godot::DSFrameWindowStats stats;
	// 120 idle frames: only the most recent 60 may define the baseline.
	for (int i = 0; i < 120; i++) {
		stats.record_frame(static_cast<double>(i + 1) * 1000.0);
	}
	// Mean of 61 ms .. 120 ms is 90.5 ms.
	GDEX_EXPECT_TRUE(near(stats.mean_before_usec(), 90500.0));

	// A window with no frames still completes and records zeros.
	stats.begin_window();
	stats.end_window();
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(0));
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 0.0));
	GDEX_EXPECT_EQ(stats.completed_windows(), static_cast<int64_t>(1));
}

GDEX_TEST(frame_window, repeated_begin_keeps_the_first_window) {
	godot::DSFrameWindowStats stats;
	stats.begin_window();
	stats.record_frame(5000.0);
	stats.record_frame(5000.0);
	stats.begin_window(); // ignored: does not drop the two frames already counted
	stats.record_frame(5000.0);
	stats.end_window();
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(3));
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 5000.0));
	GDEX_EXPECT_EQ(stats.completed_windows(), static_cast<int64_t>(1));
}

GDEX_TEST(frame_window, end_without_begin_is_ignored) {
	godot::DSFrameWindowStats stats;
	stats.record_frame(4000.0);
	stats.end_window();
	GDEX_EXPECT_EQ(stats.completed_windows(), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(0));
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 0.0));
	GDEX_EXPECT_FALSE(stats.is_active());
}

GDEX_TEST(frame_window, negative_delta_is_ignored) {
	godot::DSFrameWindowStats stats;
	stats.record_frame(-1.0);
	GDEX_EXPECT_TRUE(near(stats.mean_before_usec(), 0.0));
	stats.begin_window();
	stats.record_frame(-5.0);
	stats.end_window();
	GDEX_EXPECT_EQ(stats.frame_count(), static_cast<int64_t>(0));
	GDEX_EXPECT_TRUE(near(stats.mean_during_usec(), 0.0));
}
