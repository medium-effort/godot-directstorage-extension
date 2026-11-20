// Finding 1.1: Win32Backend's two batch slots must be able to overlap.
// RED state at HEAD (before implementation):
//   - two_batches_can_be_in_flight: second submit_async() returns false (behavioral)
//   - per_slot_admission_is_half_capacity: one slot stages capacity-2, not capacity/2-2
//   - sibling_slot_harvest_while_other_in_flight: DSContext has no harvest predicate
//   - capacity_rejection_still_classified_transient: third enqueue is staged, not refused
// All four are behavioral (no compile-red) so they can be run against HEAD.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#ifdef _WIN32
#include "core/ds_context.h"
#include "core/ds_request.h"
#include "platform/win32/ds_win32_backend.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <chrono>
#include <thread>

namespace {

constexpr int64_t kPayloadBytes = 4096;

void remove_temp_file(const godot::String &p_path) {
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps == nullptr || !godot::FileAccess::file_exists(p_path)) {
		return;
	}
	godot::DirAccess::remove_absolute(ps->globalize_path(p_path));
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
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path(p_path);
	req->set_offset(0);
	req->set_size(static_cast<uint32_t>(kPayloadBytes));
	req->set_uncompressed_size(static_cast<uint32_t>(kPayloadBytes));
	req->set_compression_format(godot::DSRequest::COMPRESSION_NONE);
	req->set_destination_type(godot::DSRequest::DESTINATION_MEMORY);
	return req;
}

godot::Win32Backend *make_initialized_backend(::gdextest::TestContext &ctx, const godot::String &fixture) {
	remove_temp_file(fixture);
	// Added first so LIFO deletes the backend (and its open IDStorageFile) before the file.
	ctx.add_teardown([fixture]() { remove_temp_file(fixture); });
	godot::Win32Backend *backend = memnew(godot::Win32Backend);
	ctx.add_teardown([backend]() { memdelete(backend); });
	if (!backend->is_available()) {
		GDEX_SKIP("DirectStorage runtime absent on this machine.");
	}
	if (!backend->initialize()) {
		GDEX_SKIP("Win32Backend initialize() failed (windowed D3D12 required).");
	}
	if (!write_fixture(fixture)) {
		GDEX_FAIL("could not write batch-pipelining fixture");
	}
	return backend;
}

bool wait_for_pending_results(godot::Win32Backend *p_backend, int p_timeout_ms) {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(p_timeout_ms);
	while (std::chrono::steady_clock::now() < deadline) {
		if (p_backend->has_pending_results()) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	return p_backend->has_pending_results();
}

} // namespace

GDEX_TEST_T(batch_pipelining, two_batches_can_be_in_flight, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_a.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);

	godot::Ref<godot::DSRequest> a = make_memory_request(fixture);
	GDEX_ASSERT_TRUE(backend->enqueue_request(a));
	GDEX_ASSERT_TRUE(backend->submit_async());

	godot::Ref<godot::DSRequest> b = make_memory_request(fixture);
	GDEX_ASSERT_TRUE(backend->enqueue_request(b));
	// RED: at HEAD the all-slot claim gate in submit_async() returns false here.
	GDEX_EXPECT_TRUE(backend->submit_async());
	GDEX_EXPECT_FALSE(backend->is_batch_complete());
}

GDEX_TEST_T(batch_pipelining, per_slot_admission_is_half_capacity, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_b.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);

	godot::DSContext *ds = godot::DSContext::get_singleton();
	GDEX_ASSERT_TRUE(ds != nullptr);
	const uint32_t capacity = ds->get_queue_capacity();
	int accepted = 0;
	for (int i = 0; i < 2048; ++i) {
		godot::PlatformBackend::EnqueueOutcome outcome = godot::PlatformBackend::EnqueueOutcome::STAGED;
		if (!backend->enqueue_request(make_memory_request(fixture), &outcome)) {
			GDEX_EXPECT_EQ(static_cast<int64_t>(outcome), static_cast<int64_t>(godot::PlatformBackend::EnqueueOutcome::TRANSIENT));
			break;
		}
		++accepted;
	}
	// RED: at HEAD one slot can stage capacity-2 requests; with two-slot pipelining
	// the per-slot budget is capacity/2 - 2 (falls back to capacity for < 8).
	const int expected = capacity >= 8 ? static_cast<int>(capacity / 2) - 2 : static_cast<int>(capacity) - 2;
	GDEX_EXPECT_EQ(static_cast<int64_t>(accepted), static_cast<int64_t>(expected));
}

GDEX_TEST_T(batch_pipelining, sibling_slot_harvest_while_other_in_flight, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_c.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);
	godot::DSContext *ds = godot::DSContext::get_singleton();
	GDEX_ASSERT_TRUE(ds != nullptr);

	// RED: the runtime-facing predicate does not exist at HEAD.
	GDEX_ASSERT_TRUE(ds->has_method("has_harvestable_results"));

	godot::Ref<godot::DSRequest> a = make_memory_request(fixture);
	godot::Ref<godot::DSRequest> b = make_memory_request(fixture);
	GDEX_ASSERT_TRUE(backend->enqueue_request(a));
	GDEX_ASSERT_TRUE(backend->submit_async());
	GDEX_ASSERT_TRUE(backend->enqueue_request(b));
	GDEX_ASSERT_TRUE(backend->submit_async());

	GDEX_ASSERT_TRUE(wait_for_pending_results(backend, 2000));
	// A finished sibling must be collectable even though the batch as a whole is
	// not complete (the DSRuntime gate now uses the new predicate above).
	backend->collect_completed_results();
	GDEX_EXPECT_FALSE(backend->has_pending_results());
}

GDEX_TEST_T(batch_pipelining, capacity_rejection_still_classified_transient, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_d.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);

	for (int i = 0; i < 2; ++i) {
		GDEX_ASSERT_TRUE(backend->enqueue_request(make_memory_request(fixture)));
		GDEX_ASSERT_TRUE(backend->submit_async());
	}
	godot::PlatformBackend::EnqueueOutcome outcome = godot::PlatformBackend::EnqueueOutcome::STAGED;
	const bool accepted = backend->enqueue_request(make_memory_request(fixture), &outcome);
	// RED: at HEAD the third request lands in the still-staged alternate slot (STAGED).
	GDEX_EXPECT_FALSE(accepted);
	GDEX_EXPECT_EQ(static_cast<int64_t>(outcome), static_cast<int64_t>(godot::PlatformBackend::EnqueueOutcome::TRANSIENT));
}

// Regression for the inverted quantifier in has_free_batch_slot(): it used to
// return true when ANY slot on ANY queue was free, while submit_async() claims
// a slot index across ALL queues. With both slots of queue 0 busy, the old gate
// still admitted flushes because queues 1 and 2 were idle.
GDEX_TEST_T(batch_pipelining, gate_false_when_both_slots_busy, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_gate.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);
	backend->_debug_mark_queue_slots_submitted_for_testing(0);
	GDEX_EXPECT_FALSE(backend->has_free_batch_slot());
}

// Regression for the reclaim leak: collect_completed_batch() cleared requests
// but left request_names / request_name_map / dstorage_requests, so later
// batches on the same slot inherited stale DSTORAGE_REQUEST Name pointers.
GDEX_TEST_T(batch_pipelining, collect_clears_request_names_and_map, TAG_INTEGRATION) {
	const godot::String fixture = "user://batch_pipeline_names.bin";
	godot::Win32Backend *backend = make_initialized_backend(ctx, fixture);
	GDEX_ASSERT_TRUE(backend->enqueue_request(make_memory_request(fixture)));
	GDEX_ASSERT_TRUE(backend->submit_async());
	GDEX_ASSERT_TRUE(wait_for_pending_results(backend, 2000));
	backend->collect_completed_results();
	GDEX_EXPECT_TRUE(backend->_debug_staged_request_names_for_testing(0, 0).is_empty());
}

#else // !_WIN32

GDEX_TEST(batch_pipelining, win32_only) {
	GDEX_SKIP("Win32Backend batch-slot tests require _WIN32.");
}

#endif // _WIN32
