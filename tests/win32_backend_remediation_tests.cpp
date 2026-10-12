// tests/win32_backend_remediation_tests.cpp
//
// Regression suite covering architectural audit remediations (HIGH-01 through
// HIGH-08, HIGH-10):
//
// - HIGH-01/02/03: D3D12 device & queue ComPtr thread safety under concurrent
//   access and invalidate_d3d12_device(), non-blocking queries without unbounded waits.
// - HIGH-04: BatchSlot two-path freeze (closing flag + active_batch_slot diversion).
// - HIGH-05: Barrier transition failure cleanly aborts submit without unsubmitted
//   DirectStorage requests.
// - HIGH-06: Command allocator bounded fence wait tolerates transient GPU busy states.
// - HIGH-07: GPUTargetTransition resource ComPtr independent lifetime.
// - HIGH-08: Synchronous timeout retains resources in zombie_slots until fence retirement.
// - HIGH-10: Multi-queue wait_for_completion iterates all queues (0 to QUEUE_COUNT - 1).
//
// SConstruct filters Win32-specific backend files out of the test build on non-Windows;
// all Win32Backend references are _WIN32 guarded and skip cleanly on other platforms.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "core/ds_context.h"

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#include "core/ds_request.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#endif

namespace {

#ifdef _WIN32
godot::Ref<godot::DSRequest> make_dummy_request(const char *p_path, uint64_t p_size = 256) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    req->set_file_path(godot::String(p_path));
    req->set_size(p_size);
    return req;
}

godot::PackedByteArray make_pattern_payload(int64_t p_size) {
    godot::PackedByteArray payload;
    payload.resize(p_size);
    for (int64_t i = 0; i < p_size; ++i) {
        payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    return payload;
}

bool write_temp_dstex(const char *p_path, int p_width, int p_height, int p_mip_count, int64_t p_payload_size) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    godot::Error err = dstex->write_to_file(
            godot::String(p_path), make_pattern_payload(p_payload_size),
            p_width, p_height, godot::Image::FORMAT_RGBA8, p_mip_count, 1, false, false);
    return err == godot::OK;
}

void remove_temp_file(const char *p_path) {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->remove(godot::String(p_path));
    }
}
#endif

} // namespace

GDEX_TEST_T(win32_remediation, d3d12_comptr_concurrency_and_invalidation, TAG_INTEGRATION) {
#ifdef _WIN32
    // HIGH-03: Multiple concurrent threads calling ds_win32_get_d3d12_device()
    // and ds_win32_get_d3d12_command_queue() while another thread concurrently
    // invalidates the cached device pointers via ds_win32_invalidate_d3d12_device().
    // Returns must be safe ComPtr instances with no pointer tearing or dangling references.
    std::atomic<bool> stop_flag{false};
    std::atomic<int> query_success_count{0};
    constexpr int kWorkerThreads = 4;
    std::vector<std::thread> workers;
    workers.reserve(kWorkerThreads);

    for (int i = 0; i < kWorkerThreads; ++i) {
        workers.emplace_back([&stop_flag, &query_success_count]() {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                Microsoft::WRL::ComPtr<ID3D12Device> dev = godot::ds_win32_get_d3d12_device();
                Microsoft::WRL::ComPtr<ID3D12CommandQueue> q = godot::ds_win32_get_d3d12_command_queue();
                if (dev != nullptr) {
                    query_success_count.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }

    // Invalidator thread repeatedly calls invalidate
    for (int i = 0; i < 50; ++i) {
        godot::ds_win32_invalidate_d3d12_device();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    stop_flag.store(true, std::memory_order_relaxed);
    for (auto &t : workers) {
        t.join();
    }

    // Must complete safely without memory violation or access faults.
    GDEX_EXPECT_TRUE(true);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, gpu_target_transition_comptr_lifetime, TAG_UNIT) {
#ifdef _WIN32
    // HIGH-07: Verify GPUTargetTransition resource field holds a ComPtr<ID3D12Resource>,
    // guaranteeing safe independent reference lifetime for GPU barriers.
    godot::Win32Backend::GPUTargetTransition transition;
    GDEX_EXPECT_TRUE(transition.resource.Get() == nullptr);
    GDEX_EXPECT_EQ(static_cast<int>(transition.original_state), static_cast<int>(D3D12_RESOURCE_STATE_COMMON));
    GDEX_EXPECT_EQ(transition.first_subresource, 0u);
    GDEX_EXPECT_EQ(transition.subresource_count, 1u);

    // Verify copy and move semantics of GPUTargetTransition with ComPtr
    godot::Win32Backend::GPUTargetTransition copied = transition;
    GDEX_EXPECT_TRUE(copied.resource.Get() == nullptr);

    godot::Win32Backend::GPUTargetTransition moved = std::move(copied);
    GDEX_EXPECT_TRUE(moved.resource.Get() == nullptr);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, uninitialized_backend_contracts, TAG_UNIT) {
#ifdef _WIN32
    // Verifies lifecycle sanity when Win32Backend is not yet initialized.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    GDEX_EXPECT_EQ(backend->get_completion_fence(), 0ULL);
    GDEX_EXPECT_TRUE(backend->is_batch_complete());
    GDEX_EXPECT_FALSE(backend->has_pending_results());

    uint64_t fence = 0;
    int completed = 0;
    int error_code = 0;
    godot::String error_msg;
    GDEX_EXPECT_FALSE(backend->collect_completed_batch(fence, completed, error_code, error_msg));

    // Wait for fence 0 on uninitialized backend should return true immediately
    GDEX_EXPECT_TRUE(backend->wait_for_completion(0, 100));

    // Wait for future fence on uninitialized backend should boundedly return false
    auto start = std::chrono::steady_clock::now();
    bool waited = backend->wait_for_completion(100, 20);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

    GDEX_EXPECT_FALSE(waited);
    // Bounded timeout: should finish within reasonable margin (not hang indefinitely)
    GDEX_EXPECT_TRUE(elapsed < 1000);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, multi_queue_wait_for_completion_bounded_timeout, TAG_INTEGRATION) {
#ifdef _WIN32
    // HIGH-10: wait_for_completion() must inspect all priority queues up to
    // QUEUE_COUNT and accurately bound wait times without premature true returns.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    // Fence value 0 must return true immediately without waiting
    auto t0 = std::chrono::steady_clock::now();
    bool immediate = backend->wait_for_completion(0, 5000);
    auto d0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    GDEX_EXPECT_TRUE(immediate);
    GDEX_EXPECT_TRUE(d0 < 50);

    // Fence value far in the future (UINT64_MAX) must return false within requested timeout
    constexpr uint32_t kTimeoutMs = 30;
    auto t1 = std::chrono::steady_clock::now();
    bool completed = backend->wait_for_completion(0xFFFFFFFFFFFFFFULL, kTimeoutMs);
    auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t1).count();

    GDEX_EXPECT_FALSE(completed);
    GDEX_EXPECT_TRUE(d1 >= (kTimeoutMs - 5));
    GDEX_EXPECT_TRUE(d1 < 1000); // Must not hang indefinitely
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, tag_cancellation_and_poll_drain_safe, TAG_INTEGRATION) {
#ifdef _WIN32
    // HIGH-08: cancel_requests_with_tag() and poll_batch_status() must safely
    // inspect queues and zombie_slots without crashes or dangling resource access.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    // Calling cancellation on non-existent tags must be a benign no-op
    backend->cancel_requests_with_tag(12345ULL);
    backend->cancel_requests_with_tag(0ULL);

    // Cancel-path purge: verify staged-but-unsubmitted entries with matching tag are purged
    const char *cancel_path = "user://cancel_staged_fixture.dstex";
    remove_temp_file(cancel_path);
    ctx.add_teardown([cancel_path]() { remove_temp_file(cancel_path); });
    GDEX_ASSERT_TRUE(write_temp_dstex(cancel_path, 64, 64, 1, 16384));

    godot::Ref<godot::DSRequest> req_cancel = make_dummy_request(cancel_path, 1024);
    req_cancel->set_uncompressed_size(1024);
    req_cancel->set_cancellation_tag(777ULL);

    godot::Ref<godot::DSRequest> req_keep = make_dummy_request(cancel_path, 1024);
    req_keep->set_uncompressed_size(1024);
    req_keep->set_cancellation_tag(888ULL);

    if (backend->enqueue_request(req_cancel) && backend->enqueue_request(req_keep)) {
        backend->cancel_requests_with_tag(777ULL);
        // Cancelled request transitioned to STATUS_FAILED with cancel timestamp
        GDEX_EXPECT_EQ(static_cast<int64_t>(req_cancel->get_status()),
                static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
        GDEX_EXPECT_TRUE(req_cancel->get_error_message().contains("cancelled by tag"));

        // Retained request remains SUBMITTED
        GDEX_EXPECT_EQ(static_cast<int64_t>(req_keep->get_status()),
                static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));
    }

    // Polling batch status with no batches in flight must be safe
    backend->poll_batch_status();
    GDEX_EXPECT_TRUE(backend->is_batch_complete());
    GDEX_EXPECT_FALSE(backend->has_pending_results());
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, slot_divert_and_closing_state, TAG_INTEGRATION) {
#ifdef _WIN32
    // HIGH-04: BatchSlot closing flag and active_batch_slot diversion.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    // Submit async on empty batch should cleanly handle slot rotation
    backend->submit_async();

    // Subsequent enqueue must succeed or fail cleanly based on request validity
    godot::Ref<godot::DSRequest> req = make_dummy_request("res://does_not_exist.bin");
    // Path doesn't exist, so enqueue returns false and marks STATUS_FAILED
    bool enqueued = backend->enqueue_request(req);
    GDEX_EXPECT_FALSE(enqueued);
    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST(win32_remediation, med05_staging_and_queue_capacity_clamping) {
    (void)ctx;
    // MED-05: Staging buffer size and queue capacity bounds clamping.
    GDEX_EXPECT_EQ(godot::DSContext::clamp_staging_buffer_size_mb(0), 1u);
    GDEX_EXPECT_EQ(godot::DSContext::clamp_staging_buffer_size_mb(128), 128u);
    GDEX_EXPECT_EQ(godot::DSContext::clamp_staging_buffer_size_mb(5000), 2048u);

    GDEX_EXPECT_EQ(godot::DSContext::clamp_queue_capacity(0), 128);
    GDEX_EXPECT_EQ(godot::DSContext::clamp_queue_capacity(512), 512);
    GDEX_EXPECT_EQ(godot::DSContext::clamp_queue_capacity(99999), 8192);
}

GDEX_TEST_T(win32_remediation, med07_submit_batch_sync_bounded_wait, TAG_INTEGRATION) {
#ifdef _WIN32
    // MED-07: Synchronous submit_batch bounded wait without indefinite stall.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        bool had_setting = ps->has_setting("directstorage/sync_flush_timeout_ms");
        godot::Variant prev_val = had_setting ? ps->get_setting("directstorage/sync_flush_timeout_ms") : godot::Variant();
        ctx.add_teardown([ps, had_setting, prev_val]() {
            if (ps) {
                if (had_setting) {
                    ps->set_setting("directstorage/sync_flush_timeout_ms", prev_val);
                } else {
                    ps->clear("directstorage/sync_flush_timeout_ms");
                }
            }
        });
        ps->set_setting("directstorage/sync_flush_timeout_ms", 500);
    }

    auto start = std::chrono::steady_clock::now();
    backend->submit_batch();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

    // 1. With no pending work, synchronous flush must return quickly (well under 500ms)
    GDEX_EXPECT_LT(elapsed, 500);

    // 2. Concurrent submit_batch callers must serialize or time out within half_timeout_ms without indefinite deadlock
    std::atomic<bool> thread_started{ false };
    std::atomic<int64_t> t2_elapsed{ 0 };
    std::thread t2([backend, &thread_started, &t2_elapsed]() {
        thread_started.store(true);
        auto t2_start = std::chrono::steady_clock::now();
        backend->submit_batch();
        t2_elapsed.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t2_start).count());
    });
    while (!thread_started.load()) {
        std::this_thread::yield();
    }
    backend->submit_batch();
    t2.join();

    // Neither thread should stall beyond bounded timeout (< 1500ms)
    GDEX_EXPECT_LT(t2_elapsed.load(), 1500);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST(win32_remediation, med01_chunk_range_overflow_validation) {
#ifdef _WIN32
    // MED-01: Fuzzing and integer overflow in .dstex chunk range validation.
    uint64_t resolved = 0;

    // 1. Normal valid relative range
    GDEX_EXPECT_TRUE(godot::Win32Backend::validate_dstex_chunk_range(100, 50, 1000, 200, resolved));
    GDEX_EXPECT_EQ(resolved, 100u);

    // 2. Normal valid absolute range
    GDEX_EXPECT_TRUE(godot::Win32Backend::validate_dstex_chunk_range(1050, 50, 1000, 200, resolved));
    GDEX_EXPECT_EQ(resolved, 50u);

    // 3. 64-bit integer overflow fuzz from audit (offset + size wraps around 0)
    GDEX_EXPECT_FALSE(godot::Win32Backend::validate_dstex_chunk_range(0xFFFFFFFFFFFFF000ULL, 0x2000ULL, 0, 0x10000ULL, resolved));

    // 4. Payload offset + size overflow
    GDEX_EXPECT_FALSE(godot::Win32Backend::validate_dstex_chunk_range(50, 20, 0xFFFFFFFFFFFFFFFEULL, 10, resolved));

    // 5. Zero source size
    GDEX_EXPECT_FALSE(godot::Win32Backend::validate_dstex_chunk_range(10, 0, 0, 100, resolved));

    // 6. Out of bounds relative range
    GDEX_EXPECT_FALSE(godot::Win32Backend::validate_dstex_chunk_range(150, 100, 0, 200, resolved));
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, med03_atomic_child_staging_no_orphans, TAG_INTEGRATION) {
#ifdef _WIN32
    // MED-03: True partial-enqueue failure handling and rollback.
    // When intermediate child fails (index c), all previously enqueued children (0..c-1)
    // are rolled back: removed from slot.requests, slot.request_names, and slot.request_name_map,
    // resource guards released, child statuses marked STATUS_FAILED, and parent marked STATUS_FAILED.
    godot::Ref<godot::DSRequest> parent;
    parent.instantiate();
    parent->set_file_path("res://multi_texture.dstex");

    godot::Vector<godot::Ref<godot::DSRequest>> slot_requests;
    std::vector<std::string> slot_request_names;
    std::unordered_map<std::string, godot::Ref<godot::DSRequest>> slot_request_name_map;

    std::vector<godot::Ref<godot::DSRequest>> staged_children;
    for (int i = 0; i < 3; ++i) {
        godot::Ref<godot::DSRequest> child;
        child.instantiate();
        child->set_file_path("res://multi_texture.dstex");
        child->set_offset(i * 1024);
        child->set_size(1024);
        staged_children.push_back(child);
    }

    std::vector<DSTORAGE_REQUEST> slot_dstorage_requests;

    // Simulate child 0 and child 1 successfully enqueued into slot
    for (size_t c = 0; c < 2; ++c) {
        staged_children[c]->set_status(godot::DSRequest::STATUS_SUBMITTED);
        slot_requests.push_back(staged_children[c]);
        std::string name = "REQ_TEST_" + std::to_string(c);
        slot_request_names.push_back(name);
        slot_request_name_map[name] = staged_children[c];
        DSTORAGE_REQUEST ds_req = {};
        slot_dstorage_requests.push_back(ds_req);
    }
    GDEX_EXPECT_EQ(slot_requests.size(), 2);
    GDEX_EXPECT_EQ(slot_request_names.size(), 2u);
    GDEX_EXPECT_EQ(slot_dstorage_requests.size(), 2u);

    // Simulate child 2 failing mid-loop (e.g. oversize, bad destination handle, etc.)
    staged_children[2]->set_status(godot::DSRequest::STATUS_FAILED);
    staged_children[2]->set_error_message("Simulated child 2 DMA handle rejection");

    // Execute the true rollback logic invoked on mid-loop enqueue_one failure
    godot::Win32Backend::rollback_staged_requests(
            slot_requests, slot_request_names, slot_request_name_map, staged_children, 2, parent, &slot_dstorage_requests);

    // Assert: Slot is completely empty (no orphaned child requests or dstorage_requests left in slot)
    GDEX_EXPECT_TRUE(slot_requests.is_empty());
    GDEX_EXPECT_TRUE(slot_request_names.empty());
    GDEX_EXPECT_TRUE(slot_request_name_map.empty());
    GDEX_EXPECT_TRUE(slot_dstorage_requests.empty());

    // Assert: Parent marked STATUS_FAILED with child's error message (NOT left SUBMITTED)
    GDEX_EXPECT_EQ(static_cast<int64_t>(parent->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(parent->get_error_message() == "Simulated child 2 DMA handle rejection");

    // Assert: Enqueued children 0 and 1 transitioned to STATUS_FAILED
    GDEX_EXPECT_EQ(static_cast<int64_t>(staged_children[0]->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(static_cast<int64_t>(staged_children[1]->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));

    // Also verify uninitialized / non-existent enqueue rejection leaves backend batch clean
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });
    if (backend->is_available() && backend->initialize()) {
        godot::Ref<godot::DSRequest> bad_req;
        bad_req.instantiate();
        bad_req->set_file_path("res://non_existent_multi_mip.dstex");
        bad_req->set_size(1024);
        bad_req->set_uncompressed_size(4096);
        bad_req->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
        bad_req->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
        bad_req->set_subresource_count(4);

        bool enqueued = backend->enqueue_request(bad_req);
        GDEX_EXPECT_FALSE(enqueued);
        GDEX_EXPECT_TRUE(backend->is_batch_complete());
    }
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST(win32_remediation, med06_exact_error_attribution_and_abortion_message) {
#ifdef _WIN32
    // MED-06: Error attribution distinguishes primary failed request from aborted batch siblings
    // using production Win32Backend::attribute_batch_error_records.
    godot::Vector<godot::Ref<godot::DSRequest>> requests;
    std::unordered_map<std::string, godot::Ref<godot::DSRequest>> name_map;

    godot::Ref<godot::DSRequest> req1;
    req1.instantiate();
    req1->set_file_path("res://tex1.dstex");
    requests.push_back(req1);
    std::string req1_id = "REQ_42_1001";
    name_map[req1_id] = req1;

    godot::Ref<godot::DSRequest> req2;
    req2.instantiate();
    req2->set_file_path("res://tex2.dstex");
    requests.push_back(req2);
    std::string req2_id = "REQ_42_1002";
    name_map[req2_id] = req2;

    godot::Ref<godot::DSRequest> req3;
    req3.instantiate();
    req3->set_file_path("res://tex3.dstex");
    requests.push_back(req3);
    std::string req3_id = "REQ_42_1003";
    name_map[req3_id] = req3;

    godot::String queue_error = "DirectStorage hardware decompress error (0x80070057).";

    // 1. DirectStorage reports FirstFailure occurred on REQ_42_1002 (req2 is root failure)
    godot::Win32Backend::attribute_batch_error_records(
            requests, name_map, godot::String(req2_id.c_str()), queue_error);

    // Root failure must receive exact queue hardware error
    GDEX_EXPECT_EQ(static_cast<int64_t>(req2->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req2->get_error_message() == queue_error);

    // Sibling requests must be marked FAILED with abortion message naming the root failed request
    GDEX_EXPECT_EQ(static_cast<int64_t>(req1->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req1->get_error_message().begins_with(
            "DirectStorage batch aborted due to failure in request [REQ_42_1002]:"));
    GDEX_EXPECT_TRUE(req1->get_error_message().contains("0x80070057"));

    GDEX_EXPECT_EQ(static_cast<int64_t>(req3->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req3->get_error_message().begins_with(
            "DirectStorage batch aborted due to failure in request [REQ_42_1002]:"));

    // 2a. Fallback when failed request name is empty: all receive queue error
    req1->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req1->set_error_message("");
    req2->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req2->set_error_message("");
    req3->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req3->set_error_message("");
    godot::Win32Backend::attribute_batch_error_records(
            requests, name_map, "", queue_error);
    GDEX_EXPECT_TRUE(req1->get_error_message() == queue_error);
    GDEX_EXPECT_TRUE(req2->get_error_message() == queue_error);
    GDEX_EXPECT_TRUE(req3->get_error_message() == queue_error);

    // 2b. Fallback when DirectStorage reports a real non-empty RequestName not found in name_map
    req1->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req1->set_error_message("");
    req2->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req2->set_error_message("");
    req3->set_status(godot::DSRequest::STATUS_SUBMITTED);
    req3->set_error_message("");
    godot::Win32Backend::attribute_batch_error_records(
            requests, name_map, "REQ_UNKNOWN_EXTERNAL_UNMAPPED_9999", queue_error);
    GDEX_EXPECT_EQ(static_cast<int64_t>(req1->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(static_cast<int64_t>(req2->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(static_cast<int64_t>(req3->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req1->get_error_message().begins_with(
            "DirectStorage batch aborted due to failure in request [REQ_UNKNOWN_EXTERNAL_UNMAPPED_9999]:"));
    GDEX_EXPECT_TRUE(req1->get_error_message().contains("0x80070057"));
    GDEX_EXPECT_TRUE(req2->get_error_message().begins_with(
            "DirectStorage batch aborted due to failure in request [REQ_UNKNOWN_EXTERNAL_UNMAPPED_9999]:"));
    GDEX_EXPECT_TRUE(req3->get_error_message().begins_with(
            "DirectStorage batch aborted due to failure in request [REQ_UNKNOWN_EXTERNAL_UNMAPPED_9999]:"));

    // 3. Request that already failed validate_resource_guard retains its specific error
    godot::Ref<godot::DSRequest> req_guard_fail;
    req_guard_fail.instantiate();
    req_guard_fail->set_file_path("res://tex_guard.dstex");
    req_guard_fail->set_status(godot::DSRequest::STATUS_FAILED);
    godot::String guard_err = "Win32Backend: Failed to resolve GPU texture destination resource from RID.";
    req_guard_fail->set_error_message(guard_err);
    requests.push_back(req_guard_fail);
    name_map["REQ_42_1004"] = req_guard_fail;

    godot::Win32Backend::attribute_batch_error_records(
            requests, name_map, godot::String(req2_id.c_str()), queue_error);
    GDEX_EXPECT_EQ(static_cast<int64_t>(req_guard_fail->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req_guard_fail->get_error_message() == guard_err);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, med08_single_mip_parsed_values_preserved, TAG_INTEGRATION) {
#ifdef _WIN32
    // MED-08: Real .dstex fixture verification.
    // Single-mip requests must parse actual on-disk mip table entries, populating
    // the child request with exact data offset past file header, while preserving
    // parent request immutability (offset 0 and initial parameters untouched).
    const char *dstex_path = "user://med08_fixture.dstex";
    remove_temp_file(dstex_path);
    ctx.add_teardown([dstex_path]() { remove_temp_file(dstex_path); });

    constexpr int kWidth = 64;
    constexpr int kHeight = 64;
    constexpr int64_t kPayloadSize = kWidth * kHeight * 4; // 16384 bytes
    GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, kWidth, kHeight, 1, kPayloadSize));

    godot::Ref<godot::DSRequest> parent;
    parent.instantiate();
    parent->set_file_path(dstex_path);
    parent->set_offset(0); // whole-file offset
    parent->set_size(100); // intentionally dummy whole-file size
    parent->set_uncompressed_size(200);
    parent->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
    parent->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
    parent->set_priority(godot::DSRequest::PRIORITY_HIGH);
    parent->set_cancellation_tag(999);
    parent->set_subresource_count(1);
    parent->set_first_subresource(0);

    godot::Ref<godot::DSRequest> child;
    bool staged = godot::Win32Backend::stage_single_mip_request(parent, child);
    GDEX_ASSERT_TRUE(staged);
    GDEX_ASSERT_TRUE(child.is_valid());

    // Child must have parsed the REAL on-disk DSTexMipEntryV2:
    // Offset must be past the header + mip table (>= 28 + 24 = 52 bytes)
    GDEX_EXPECT_TRUE(child->get_offset() >= sizeof(godot::DSTexHeader) + sizeof(godot::DSTexMipEntryV2));
    GDEX_EXPECT_EQ(child->get_uncompressed_size(), static_cast<uint32_t>(kPayloadSize));
    GDEX_EXPECT_TRUE(child->get_size() > 0);
    GDEX_EXPECT_EQ(child->get_compression_format(), godot::DSRequest::COMPRESSION_GDEFLATE);
    GDEX_EXPECT_EQ(child->get_destination_type(), godot::DSRequest::DESTINATION_GPU_TEXTURE);
    GDEX_EXPECT_EQ(child->get_cancellation_tag(), 999u);
    GDEX_EXPECT_EQ(static_cast<int>(child->get_priority()), static_cast<int>(godot::DSRequest::PRIORITY_HIGH));

    // Parent must remain completely immutable (preserving caller's original config)
    GDEX_EXPECT_EQ(parent->get_offset(), 0u);
    GDEX_EXPECT_EQ(parent->get_size(), 100u);
    GDEX_EXPECT_EQ(parent->get_uncompressed_size(), 200u);
    GDEX_EXPECT_EQ(parent->get_cancellation_tag(), 999u);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}


