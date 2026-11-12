#ifndef DS_FRAME_WINDOW_H
#define DS_FRAME_WINDOW_H

#include <cstdint>

namespace godot {

// Frame-time impact of one streaming window: the mean frame time of the idle
// frames before a load starts versus the mean while it runs, plus how many
// frames it spanned. Kept free of Godot types so the aggregation is
// unit-testable off-engine; DSProfiler owns one instance and feeds it once per
// frame from DSRuntime.
struct DSFrameWindowStats {
	static constexpr int kBaselineCapacity = 60;
	// Hard ceiling on one load window. A caller that opens a window and never
	// closes it (the bound begin_load_window without end_load_window, a sampler
	// muted mid-window, a prewarm tag that never completes) must not freeze the
	// idle baseline for the rest of the session: at the cap the window closes
	// itself and reports what it measured.
	static constexpr int64_t kMaxWindowFrames = 36000;

	void record_frame(double p_delta_usec) {
		if (p_delta_usec < 0.0) {
			return;
		}
		if (window_active) {
			during_sum_usec += p_delta_usec;
			during_frames++;
			if (during_frames >= kMaxWindowFrames) {
				// end_window() reads the sums before resetting them, so the
				// capping frame is counted once, in the window it closes.
				end_window();
			}
			return;
		}
		// Rolling baseline over the most recent idle frames. While a window is
		// open this branch is never reached, so the baseline stays frozen at
		// the values that preceded the load.
		if (baseline_samples < kBaselineCapacity) {
			baseline_usec[baseline_head] = p_delta_usec;
			baseline_sum_usec += p_delta_usec;
			baseline_samples++;
		} else {
			baseline_sum_usec -= baseline_usec[baseline_head];
			baseline_usec[baseline_head] = p_delta_usec;
			baseline_sum_usec += p_delta_usec;
		}
		baseline_head = (baseline_head + 1) % kBaselineCapacity;
	}

	void begin_window() {
		if (window_active) {
			// Two starts before the first end describe the same load; keeping the
			// first avoids dropping the frames already counted.
			return;
		}
		window_active = true;
		during_sum_usec = 0.0;
		during_frames = 0;
	}

	void end_window() {
		if (!window_active) {
			return;
		}
		window_active = false;
		last_mean_before_usec = baseline_samples > 0 ? baseline_sum_usec / static_cast<double>(baseline_samples) : 0.0;
		last_mean_during_usec = during_frames > 0 ? during_sum_usec / static_cast<double>(during_frames) : 0.0;
		last_frame_count = during_frames;
		completed_window_count++;
		during_sum_usec = 0.0;
		during_frames = 0;
	}

	bool is_active() const { return window_active; }

	// Idle baseline mean (0 until at least one idle frame has been sampled).
	double mean_before_usec() const {
		return baseline_samples > 0 ? baseline_sum_usec / static_cast<double>(baseline_samples) : 0.0;
	}

	// Running mean while a window is open, the last completed window's mean after it closes.
	double mean_during_usec() const {
		if (window_active) {
			return during_frames > 0 ? during_sum_usec / static_cast<double>(during_frames) : 0.0;
		}
		return last_mean_during_usec;
	}

	int64_t frame_count() const { return window_active ? during_frames : last_frame_count; }
	int64_t completed_windows() const { return completed_window_count; }

private:
	bool window_active = false;
	double baseline_usec[kBaselineCapacity] = {};
	double baseline_sum_usec = 0.0;
	int baseline_samples = 0;
	int baseline_head = 0;
	double during_sum_usec = 0.0;
	int64_t during_frames = 0;
	double last_mean_before_usec = 0.0;
	double last_mean_during_usec = 0.0;
	int64_t last_frame_count = 0;
	int64_t completed_window_count = 0;
};

} // namespace godot

#endif // DS_FRAME_WINDOW_H
