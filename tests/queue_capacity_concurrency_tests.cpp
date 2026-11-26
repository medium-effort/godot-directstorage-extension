// The atomic claim gate (queue-capacity contract, G2) after finding 1.1.
//
// submit_async() claims the batch while holding batch_mutex, but the claim is
// only visible to other submitters as `closing` until the second lock sets
// `submitted`. is_batch_complete() reports "complete" during that window. The
// window is a few hundred instructions wide and is not reachable by a stress
// loop; the DEBUG/DEV enqueue hook parks the first submitter inside it instead.
//
// With two-slot pipelining, a second submitter MAY claim the diverted slot: each
// slot now stages at most capacity/2 - 2 requests, so two batches in flight
// consume exactly the queue's operation budget. The invariant to protect is no
// longer "only one submitter wins" but "both claims land in distinct slots and
// both batches complete without losing a request".

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#ifdef _WIN32
#include "core/ds_request.h"
#include "platform/win32/ds_win32_backend.h"

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <atomic>
#include <chrono>
#include <thread>

namespace {

constexpr int64_t kPayloadBytes = 1 << 20; // 1 MiB: big enough to be a real read
constexpr int64_t kLargePayloadBytes = 8 << 20; // 8 MiB: material pre-size retention

void remove_temp_file(const godot::String &p_path) {
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps == nullptr) {
		return;
	}
	const godot::String absolute = ps->globalize_path(p_path);
	if (godot::FileAccess::file_exists(absolute)) {
		godot::DirAccess::remove_absolute(absolute);
	}
}

bool write_fixture(const godot::String &p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(p_path, godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	godot::PackedByteArray payload;
	payload.resize(kPayloadBytes);
	uint8_t *write = payload.ptrw();
	for (int64_t i = 0; i < kPayloadBytes; ++i) {
		write[i] = static_cast<uint8_t>(i & 0xFF);
	}
	file->store_buffer(payload);
	file->close();
	return true;
}

godot::Ref<godot::DSRequest> make_memory_request(const godot::String &p_path) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	request->set_file_path(p_path);
	request->set_offset(0);
	request->set_size(static_cast<uint32_t>(kPayloadBytes));
	request->set_uncompressed_size(static_cast<uint32_t>(kPayloadBytes));
	request->set_compression_format(godot::DSRequest::COMPRESSION_NONE);
	// Memory destinations produce no GPU transitions, so the parked submitter
	// reaches the enqueue hook on its first instruction after the claim.
	request->set_destination_type(godot::DSRequest::DESTINATION_MEMORY);
	return request;
}

bool write_fixture_sized(const godot::String &p_path, int64_t p_bytes) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(p_path, godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	godot::PackedByteArray payload;
	payload.resize(p_bytes);
	uint8_t *write = payload.ptrw();
	for (int64_t i = 0; i < p_bytes; ++i) {
		write[i] = static_cast<uint8_t>(i & 0xFF);
	}
	file->store_buffer(payload);
	file->close();
	return true;
}

godot::Ref<godot::DSRequest> make_memory_request_sized(const godot::String &p_path, int64_t p_bytes) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	request->set_file_path(p_path);
	request->set_offset(0);
	request->set_size(static_cast<uint32_t>(p_bytes));
	request->set_uncompressed_size(static_cast<uint32_t>(p_bytes));
	request->set_compression_format(godot::DSRequest::COMPRESSION_NONE);
	request->set_destination_type(godot::DSRequest::DESTINATION_MEMORY);
	return request;
}

bool wait_for_flag(const std::atomic<bool> &p_flag, int p_timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_timeout_ms);
	while (!p_flag.load() && std::chrono::steady_clock::now() < deadline) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return p_flag.load();
}

} // namespace

GDEX_TEST_T(queue_capacity_concurrency, claim_gate_allows_two_budgeted_submitters, TAG_INTEGRATION) {
	const godot::String fixture = "user://queue_capacity_claim_fixture.bin";
	remove_temp_file(fixture);
	// Registered before the backend teardown below so LIFO deletes the backend
	// (and its open IDStorageFile handle) before removing the file.
	ctx.add_teardown([fixture]() { remove_temp_file(fixture); });

	godot::Win32Backend *backend = memnew(godot::Win32Backend);
	ctx.add_teardown([backend]() {
		// A hook that captured this frame must be gone before the delete.
		backend->_debug_set_enqueue_window_hook_for_testing({});
		memdelete(backend);
	});

	if (!backend->is_available()) {
		GDEX_SKIP("DirectStorage runtime absent on this machine.");
	}
	if (!backend->initialize()) {
		GDEX_SKIP("Win32Backend initialize() failed (windowed D3D12 required).");
	}
	if (!write_fixture(fixture)) {
		GDEX_FAIL("could not write the claim-gate fixture");
		return;
	}

	godot::Ref<godot::DSRequest> first = make_memory_request(fixture);
	GDEX_ASSERT_TRUE(backend->enqueue_request(first));

	std::atomic<bool> window_open{ false };
	std::atomic<bool> release_window{ false };
	backend->_debug_set_enqueue_window_hook_for_testing([&window_open, &release_window]() {
		window_open.store(true);
		// Self-releasing: a broken build must not hang the suite here.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!release_window.load() && std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	});

	std::atomic<bool> first_result{ false };
	std::thread submitter([&first_result, backend]() { first_result.store(backend->submit_async()); });

	const bool entered = wait_for_flag(window_open, 5000);
	GDEX_EXPECT_TRUE(entered);
	if (!entered) {
		release_window.store(true);
		submitter.join();
		return;
	}

	// The claim is parked as `closing`, and is_batch_complete() only looks at
	// `submitted`, so it still says "complete" during the window.
	GDEX_EXPECT_TRUE(backend->is_batch_complete());

	// Stage into the diverted slot (slot 0 is closing) and claim it. Unlike the
	// old single-batch contract, this must now succeed: the halved admission
	// budget keeps two in-flight batches within the queue's operation budget.
	godot::Ref<godot::DSRequest> second = make_memory_request(fixture);
	GDEX_EXPECT_TRUE(backend->enqueue_request(second));
	GDEX_EXPECT_TRUE(backend->submit_async());

	release_window.store(true);
	submitter.join();
	backend->_debug_set_enqueue_window_hook_for_testing({});
	GDEX_EXPECT_TRUE(first_result.load());

	// Both batches are in flight; both must complete without losing a request.
	const uint64_t first_fence = backend->get_completion_fence();
	GDEX_EXPECT_TRUE(backend->wait_for_completion(first_fence, 5000));
	for (int i = 0; i < 100 &&
			(first->get_status() != godot::DSRequest::STATUS_COMPLETED ||
					second->get_status() != godot::DSRequest::STATUS_COMPLETED); ++i) {
		backend->poll_batch_status();
		backend->collect_completed_results();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(first->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
	GDEX_EXPECT_EQ(static_cast<int64_t>(second->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
}

// Regression: a post-lock TRANSIENT rejection must not retain the buffer that
// enqueue_request() grew before taking batch_mutex.
GDEX_TEST_T(queue_capacity_concurrency, transient_rejection_releases_preallocated_destination_buffer, TAG_INTEGRATION) {
	const godot::String fixture = "user://queue_capacity_presize_fixture.bin";
	remove_temp_file(fixture);
	ctx.add_teardown([fixture]() { remove_temp_file(fixture); });

	godot::Win32Backend *backend = memnew(godot::Win32Backend);
	ctx.add_teardown([backend]() { memdelete(backend); });

	if (!backend->is_available()) {
		GDEX_SKIP("DirectStorage runtime absent on this machine.");
	}
	if (!backend->initialize()) {
		GDEX_SKIP("Win32Backend initialize() failed (windowed D3D12 required).");
	}
	if (!write_fixture_sized(fixture, kLargePayloadBytes)) {
		GDEX_FAIL("could not write the pre-size fixture");
		return;
	}

	// Control leg: a request admitted with a free slot legitimately keeps its
	// 8 MiB buffer, and completes. Same size, destination, codec and fixture.
	godot::Ref<godot::DSRequest> admitted = make_memory_request_sized(fixture, kLargePayloadBytes);
	PlatformBackend::EnqueueOutcome admitted_outcome = PlatformBackend::EnqueueOutcome::TERMINAL;
	GDEX_EXPECT_TRUE(backend->enqueue_request(admitted, &admitted_outcome));
	GDEX_EXPECT_EQ(static_cast<int64_t>(admitted_outcome),
			static_cast<int64_t>(PlatformBackend::EnqueueOutcome::STAGED));
	GDEX_EXPECT_EQ(static_cast<int64_t>(admitted->get_destination_buffer().size()),
			static_cast<int64_t>(kLargePayloadBytes));
	backend->submit_batch();
	GDEX_EXPECT_TRUE(backend->wait_for_completion(backend->get_completion_fence(), 5000));
	for (int i = 0; i < 100 && admitted->get_status() != godot::DSRequest::STATUS_COMPLETED; ++i) {
		backend->poll_batch_status();
		backend->collect_completed_results();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(admitted->get_status()),
			static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));

	// Force a post-lock TRANSIENT rejection on a request of the same shape.
	for (int queue_index = 0; queue_index < 3; ++queue_index) {
		backend->_debug_mark_queue_slots_submitted_for_testing(queue_index);
	}
	godot::Ref<godot::DSRequest> rejected = make_memory_request_sized(fixture, kLargePayloadBytes);
	PlatformBackend::EnqueueOutcome outcome = PlatformBackend::EnqueueOutcome::TERMINAL;
	GDEX_EXPECT_FALSE(backend->enqueue_request(rejected, &outcome));
	GDEX_EXPECT_EQ(static_cast<int64_t>(outcome),
			static_cast<int64_t>(PlatformBackend::EnqueueOutcome::TRANSIENT));
	// The pre-size ran; the reject path must have released it.
	GDEX_EXPECT_EQ(static_cast<int64_t>(rejected->get_destination_buffer().size()), 0);
}

#else

GDEX_TEST_T(queue_capacity_concurrency, claim_gate_allows_two_budgeted_submitters, TAG_INTEGRATION) {
	(void)ctx;
	GDEX_SKIP("The enqueue-window seam is compiled out of release builds.");
}

GDEX_TEST_T(queue_capacity_concurrency, transient_rejection_releases_preallocated_destination_buffer, TAG_INTEGRATION) {
	(void)ctx;
	GDEX_SKIP("The Win32 pre-size seam is compiled out of release builds.");
}

#endif
#else

GDEX_TEST_T(queue_capacity_concurrency, claim_gate_allows_two_budgeted_submitters, TAG_INTEGRATION) {
	(void)ctx;
	GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
}

GDEX_TEST_T(queue_capacity_concurrency, transient_rejection_releases_preallocated_destination_buffer, TAG_INTEGRATION) {
	(void)ctx;
	GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
}

#endif
