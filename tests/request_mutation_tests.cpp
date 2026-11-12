// Request payload ownership.
//
// A mutating setter checks is_mutable() and then writes a non-atomic field; the
// submitter reads those fields after staging. One per-request mutex joins them:
// the setter holds it across (guard, write), and claim_submission() holds it
// across PENDING -> SUBMITTED before the submitter's first field read.
//
// setter_window_excludes_claim is the discriminator: a DEBUG hook parks a setter
// inside its guarded window while a second thread tries the claim with a
// non-blocking try-lock. With the lock the try-claim must fail; without it the
// try-claim wins and the write lands after the request was sealed. The handshake
// loops in-process because the gdextest runner has no repeat flag.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include <godot_cpp/classes/ref.hpp>

#include "core/ds_request.h"

#include <condition_variable>
#include <mutex>
#include <thread>

GDEX_TEST_T(request_mutation, claim_then_setter_is_refused, TAG_UNIT) {
	(void)ctx;
	godot::Ref<godot::DSRequest> req;
	req.instantiate();

	req->set_file_path("first");
	GDEX_EXPECT_TRUE(req->get_file_path() == godot::String("first"));
	GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_PENDING));

	GDEX_EXPECT_TRUE(req->claim_submission());
	GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));

	// Refused by the guard: one expected push_error, payload unchanged.
	req->set_file_path("second");
	GDEX_EXPECT_TRUE(req->get_file_path() == godot::String("first"));
	GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));

	GDEX_EXPECT_FALSE(req->claim_submission());
	GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));
}

GDEX_TEST_T(request_mutation, claim_refused_when_gpu_in_flight, TAG_UNIT) {
	(void)ctx;
	godot::Ref<godot::DSRequest> req;
	req.instantiate();

	req->set_gpu_in_flight(true);
	GDEX_EXPECT_FALSE(req->claim_submission());
	GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_PENDING));

	req->set_gpu_in_flight(false);
	GDEX_EXPECT_TRUE(req->claim_submission());
}

GDEX_TEST_T(request_mutation, setter_window_excludes_claim, TAG_UNIT) {
	(void)ctx;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	constexpr int kIterations = 200;
	for (int iteration = 0; iteration < kIterations; iteration++) {
		godot::Ref<godot::DSRequest> req;
		req.instantiate();
		req->set_file_path("first");

		std::mutex handshake;
		std::condition_variable in_window_cv;
		std::condition_variable release_cv;
		bool in_window = false;
		bool release = false;
		bool claimed_mid_write = true;

		req->_debug_set_mutation_window_hook_for_testing([&]() {
			std::unique_lock<std::mutex> lock(handshake);
			in_window = true;
			in_window_cv.notify_one();
			release_cv.wait(lock, [&]() { return release; });
		});

		std::thread prober([&]() {
			{
				std::unique_lock<std::mutex> lock(handshake);
				in_window_cv.wait(lock, [&]() { return in_window; });
			}
			// A submitter landing in the window must not be able to claim.
			claimed_mid_write = req->_debug_try_claim_submission_for_testing();
			{
				std::lock_guard<std::mutex> lock(handshake);
				release = true;
			}
			release_cv.notify_one();
		});

		req->set_file_path("second"); // parks inside the guarded window
		prober.join();
		req->_debug_set_mutation_window_hook_for_testing({});

		if (claimed_mid_write) {
			GDEX_FAIL(std::string("submitter claimed inside the setter window at iteration ") +
					std::to_string(iteration) + " (the setter's guard and write are not joined to "
					"claim_submission).");
			return;
		}
		GDEX_EXPECT_TRUE(req->get_file_path() == godot::String("second"));
		GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
				static_cast<int64_t>(godot::DSRequest::STATUS_PENDING));
		GDEX_EXPECT_TRUE(req->claim_submission());
		GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
				static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));
	}
#else
	GDEX_SKIP("Request mutation-window seam unavailable (release build).");
#endif
}
