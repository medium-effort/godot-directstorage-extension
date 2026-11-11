// Per-queue operation budget.
//
// A submitted batch consumes one queue operation per request plus one
// EnqueueSignal and one EnqueueSetEvent, and a queue slot stays in use until
// its operation completes (Microsoft's DirectStorage guidance). A batch slot may
// therefore stage at most capacity - 2 requests; staging one more is what turns
// the final enqueue into a blocking call. Win32Backend::admits_requests() is the
// single place that arithmetic lives, so it is tested directly; the header is
// Windows-only, hence the split below.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"

using godot::Win32Backend;

GDEX_TEST(queue_capacity, full_batch_fits) {
	// 126 requests + signal + event = exactly 128 operations.
	GDEX_EXPECT_TRUE(Win32Backend::admits_requests(0, 126, 128));
	GDEX_EXPECT_TRUE(Win32Backend::admits_requests(100, 26, 128));
	GDEX_EXPECT_TRUE(Win32Backend::admits_requests(0, 510, 512));
}

GDEX_TEST(queue_capacity, signal_and_event_are_reserved) {
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(0, 127, 128));
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(126, 1, 128));
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(0, 511, 512));
}

GDEX_TEST(queue_capacity, tiny_budgets_are_never_admitted) {
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(0, 0, 0));
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(0, 0, 1));
	GDEX_EXPECT_FALSE(Win32Backend::admits_requests(0, 1, 2));
	// Two operations of budget are the signal and the event: no request fits.
	GDEX_EXPECT_TRUE(Win32Backend::admits_requests(0, 0, 2));
	GDEX_EXPECT_TRUE(Win32Backend::admits_requests(0, 1, 3));
}

#else

GDEX_TEST(queue_capacity, win32_only) {
	(void)ctx;
	GDEX_SKIP("Win32Backend admission budget is Windows-only");
}

#endif
