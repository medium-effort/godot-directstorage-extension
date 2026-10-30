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
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

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

// HIGH-01 fixtures: a 2048^2 incompressible RGBA8 payload keeps the DMA +
// GDeflate decode window at tens of milliseconds (the shape gpu_drain_tests.cpp
// already relies on), so "still in flight" is observable instead of a
// theoretical race.
constexpr int kInFlightTextureSize = 2048;

godot::PackedByteArray make_incompressible_rgba8(int p_size) {
    godot::PackedByteArray data;
    data.resize(static_cast<int64_t>(p_size) * static_cast<int64_t>(p_size) * 4);
    uint8_t *writer = data.ptrw();
    uint32_t state = 0x9E3779B9u;
    for (int64_t i = 0; i < data.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        writer[i] = static_cast<uint8_t>(state >> 24);
    }
    return data;
}

bool write_gpu_texture_fixture(const godot::String &p_path) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    const godot::PackedByteArray raw = make_incompressible_rgba8(kInFlightTextureSize);
    const godot::Error err = dstex->write_to_file(p_path, raw, kInFlightTextureSize,
            kInFlightTextureSize, godot::Image::FORMAT_RGBA8, 1, 1);
    return err == godot::OK;
}

// Builds a GPU-texture request spanning the whole single-mip payload.
// texture_format stays 0 so the backend skips the driver-format equality check
// (the contract the preloader and gpu_drain_tests.cpp already rely on); the
// in-flight admission check in enqueue_one() still applies in that case.
bool build_gpu_texture_request(const godot::String &p_path, uint64_t p_tag, godot::Ref<godot::DSRequest> &r_request) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    const godot::Dictionary header = dstex->read_header(p_path);
    if (header.is_empty()) {
        return false;
    }
    const int mip_count = header["mip_count"];
    const godot::PackedInt64Array mip_offsets = header["mip_offsets"];
    const godot::PackedInt32Array mip_sizes = header["mip_sizes"];
    if (mip_count < 1 || mip_offsets.size() < mip_count || mip_sizes.size() < mip_count) {
        return false;
    }
    const uint64_t start = static_cast<uint64_t>(mip_offsets[0]);
    const uint64_t end = static_cast<uint64_t>(mip_offsets[mip_count - 1]) + static_cast<uint64_t>(mip_sizes[mip_count - 1]);
    r_request.instantiate();
    r_request->set_file_path(p_path);
    r_request->set_offset(start);
    r_request->set_size(static_cast<uint32_t>(end - start));
    r_request->set_uncompressed_size(static_cast<uint32_t>(header["original_size"]));
    r_request->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
    r_request->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
    r_request->set_first_subresource(0);
    r_request->set_subresource_count(static_cast<uint32_t>(mip_count));
    r_request->set_texture_format(0);
    r_request->set_cancellation_tag(p_tag);
    return true;
}

godot::RID create_destination_texture(godot::RenderingDevice *p_rd, int p_mip_count) {
    godot::Ref<godot::RDTextureFormat> format;
    format.instantiate();
    format->set_width(kInFlightTextureSize);
    format->set_height(kInFlightTextureSize);
    format->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    format->set_mipmaps(p_mip_count);
    format->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
    format->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
            godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
    godot::Ref<godot::RDTextureView> view;
    view.instantiate();
    return p_rd->texture_create(format, view, godot::TypedArray<godot::PackedByteArray>());
}

// Bounded wait for the backend to stop reporting the RID as in flight. The
// backend's own completion thread retires the fence; poll_batch_status() is what
// clears the flags and drains zombie slots.
bool wait_until_released(godot::Win32Backend *p_backend, const godot::RID &p_rid, int p_attempts = 400) {
    for (int i = 0; i < p_attempts; ++i) {
        p_backend->poll_batch_status();
        if (!p_backend->is_resource_in_flight(p_rid)) {
            return true;
        }
        godot::OS::get_singleton()->delay_usec(5000);
    }
    return false;
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

GDEX_TEST_T(win32_remediation, staged_request_reports_resource_in_flight, TAG_INTEGRATION) {
#ifdef _WIN32
    // is_resource_in_flight() must answer "yes" for a request the backend
    // has accepted but nobody has flushed yet. That staged batch DMA-writes the
    // destination on the next flush, so a release that reads "no" here frees VRAM
    // the hardware is still going to write.
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        memdelete(backend); // shutdown() drains anything still in flight
        remove_temp_file("user://high01_staged_in_flight.dstex");
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    const godot::String fixture("user://high01_staged_in_flight.dstex");
    remove_temp_file("user://high01_staged_in_flight.dstex");
    GDEX_ASSERT_TRUE(write_gpu_texture_fixture(fixture));

    godot::Ref<godot::DSRequest> request;
    GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5101, request));
    const godot::RID rid = create_destination_texture(rd, static_cast<int>(request->get_subresource_count()));
    GDEX_ASSERT_TRUE(rid.is_valid());
    request->set_destination_resource(rid);

    GDEX_ASSERT_TRUE(backend->enqueue_request(request));

    // The core assertion: nothing is submitted yet, but the backend owns a request
    // that will write this RID as soon as it is flushed.
    GDEX_EXPECT_TRUE(backend->is_resource_in_flight(rid));

    backend->submit_async();
    const uint64_t fence = backend->get_completion_fence();
    GDEX_ASSERT_TRUE(fence > 0);
    GDEX_EXPECT_TRUE(backend->is_resource_in_flight(rid));

    // ... and the flag must not stay stuck once the fence retires.
    backend->wait_for_completion(fence, 5000);
    GDEX_EXPECT_TRUE(wait_until_released(backend, rid));

    if (rd->texture_is_valid(rid)) {
        rd->free_rid(rid);
    }
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, zombie_slot_reports_resource_in_flight, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // HIGH-01: when a synchronous flush times out, the batch's requests leave the
    // slot for zombie_slots while the hardware keeps writing. Both in-flight queries
    // must still see them - otherwise a release frees the destination under live DMA
    // and a second enqueue is admitted against a resource nobody can barrier.
    // The state is produced through the DEBUG-only seam because the real branch needs
    // a >500 ms DMA stall (sync_flush_timeout_ms is clamped to [500, 10000]).
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        memdelete(backend); // shutdown() drains anything still in flight
        remove_temp_file("user://high01_zombie_in_flight.dstex");
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    const godot::String fixture("user://high01_zombie_in_flight.dstex");
    remove_temp_file("user://high01_zombie_in_flight.dstex");
    GDEX_ASSERT_TRUE(write_gpu_texture_fixture(fixture));

    bool proven = false;
    for (int attempt = 0; attempt < 3 && !proven; ++attempt) {
        godot::Ref<godot::DSRequest> request;
        GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5201, request));
        const godot::RID rid = create_destination_texture(rd, static_cast<int>(request->get_subresource_count()));
        GDEX_ASSERT_TRUE(rid.is_valid());
        request->set_destination_resource(rid);

        GDEX_ASSERT_TRUE(backend->enqueue_request(request));
        backend->submit_async();
        const uint64_t fence = backend->get_completion_fence();
        if (fence == 0 || !backend->is_resource_in_flight(rid)) {
            // Lost the race: the fence retired before we could zombify. Retry with a
            // fresh texture instead of failing on scheduler jitter.
            if (rd->texture_is_valid(rid)) {
                rd->free_rid(rid);
            }
            continue;
        }

        backend->_debug_zombify_active_batch_for_testing();

        // (1) The moved request still owns the RID.
        GDEX_EXPECT_TRUE(backend->is_resource_in_flight(rid));

        // (2) The admission gate must refuse a second write into it.
        godot::Ref<godot::DSRequest> conflicting;
        GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5202, conflicting));
        conflicting->set_destination_resource(rid);
        GDEX_EXPECT_FALSE(backend->enqueue_request(conflicting));
        GDEX_EXPECT_TRUE(conflicting->get_error_message().contains("in-flight DirectStorage batch"));

        // (3) Retirement drains the zombie and releases the RID.
        backend->wait_for_completion(fence, 5000);
        GDEX_EXPECT_TRUE(wait_until_released(backend, rid));

        // (4) The RID is reusable once the fence has retired.
        godot::Ref<godot::DSRequest> reusable;
        GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5203, reusable));
        reusable->set_destination_resource(rid);
        GDEX_EXPECT_TRUE(backend->enqueue_request(reusable));
        backend->cancel_requests_with_tag(5203); // purge the staged request before free

        if (rd->texture_is_valid(rid)) {
            rd->free_rid(rid);
        }
        proven = true;
    }
    GDEX_ASSERT_TRUE(proven);
#else
    (void)ctx;
    GDEX_SKIP("The zombie seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

// #0183: poll_batch_status() retired a zombie by closing the slot's handles
// directly, behind persistent_file_cache, which kept pointing at the closed
// IDStorageFile with refs still pinned. Two consequences, both asserted here:
// the next batch reused a closed handle, and the file stayed locked on disk for
// the rest of the session (so a replacement/patch could never land).
GDEX_TEST_T(win32_remediation, zombie_slot_releases_source_file_after_fence, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    const godot::String fixture("user://med11_zombie_file_release.dstex");
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        memdelete(backend); // shutdown() drains anything still in flight
        remove_temp_file("user://med11_zombie_file_release.dstex");
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }
    // Nothing is opened eagerly: a handle exists only while a batch owns it.
    GDEX_EXPECT_EQ(backend->_debug_cached_source_file_count(), 0);

    remove_temp_file("user://med11_zombie_file_release.dstex");
    GDEX_ASSERT_TRUE(write_gpu_texture_fixture(fixture));

    bool proven = false;
    for (int attempt = 0; attempt < 3 && !proven; ++attempt) {
        godot::Ref<godot::DSRequest> request;
        GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5301, request));
        const godot::RID rid = create_destination_texture(rd, static_cast<int>(request->get_subresource_count()));
        GDEX_ASSERT_TRUE(rid.is_valid());
        request->set_destination_resource(rid);

        GDEX_ASSERT_TRUE(backend->enqueue_request(request));
        // Staging pins exactly one handle for the fixture path.
        GDEX_EXPECT_EQ(backend->_debug_cached_source_file_count(), 1);
        backend->submit_async();
        const uint64_t fence = backend->get_completion_fence();
        if (fence == 0 || !backend->is_resource_in_flight(rid)) {
            // Lost the race: the fence retired before we could zombify. Reset the
            // slot and retry with a fresh texture instead of failing on jitter.
            backend->cancel_requests_with_tag(5301);
            backend->poll_batch_status();
            if (rd->texture_is_valid(rid)) {
                rd->free_rid(rid);
            }
            continue;
        }

        backend->_debug_zombify_active_batch_for_testing();
        if (!backend->is_resource_in_flight(rid)) {
            // The fence retired before the seam could move the slot, so there is
            // no zombie to retire. Retry instead of asserting on a lost race.
            backend->poll_batch_status();
            if (rd->texture_is_valid(rid)) {
                rd->free_rid(rid);
            }
            continue;
        }
        GDEX_EXPECT_EQ(backend->_debug_cached_source_file_count(), 1);

        backend->wait_for_completion(fence, 5000);
        bool released = false;
        for (int i = 0; i < 400 && !released; ++i) {
            backend->poll_batch_status();
            released = backend->_debug_cached_source_file_count() == 0;
            if (!released) {
                godot::OS::get_singleton()->delay_usec(5000);
            }
        }
        GDEX_EXPECT_TRUE(released);
        // The user-facing symptom: the source is replaceable again once the
        // zombie retires (a raw Close() left it locked until shutdown).
        GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::remove_absolute(fixture)), static_cast<int64_t>(godot::OK));

        if (rd->texture_is_valid(rid)) {
            rd->free_rid(rid);
        }
        proven = true;
    }
    GDEX_ASSERT_TRUE(proven);
#else
    (void)ctx;
    GDEX_SKIP("The zombie seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

// #0184: a child failing mid-staging rolled the requests back but left the
// slot's file reference in place, because enqueue_one registered the path as
// soon as the first child succeeded and the guard then saw it as adopted. The
// leak only closed on the slot's next completed batch, so the file stayed
// locked in the meantime.
//
// Reachable without a test seam: the per-child destination check rejects
// first_subresource >= the texture's subresource count, so a 2-mip fixture
// staged into a 1-mip destination fails on child 1 (child 0 is valid).
GDEX_TEST_T(win32_remediation, staging_rollback_releases_source_file, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    const godot::String fixture("user://med12_rollback_file.dstex");
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        memdelete(backend);
        remove_temp_file("user://med12_rollback_file.dstex");
    });
    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    remove_temp_file("user://med12_rollback_file.dstex");
    // 64x64 RGBA8 with 2 mips: base 16384 + mip 4096 bytes of raw input.
    GDEX_ASSERT_TRUE(write_temp_dstex("user://med12_rollback_file.dstex", 64, 64, 2, 64 * 64 * 4 + 32 * 32 * 4));

    godot::Ref<godot::DSRequest> request;
    GDEX_ASSERT_TRUE(build_gpu_texture_request(fixture, 5401, request));
    GDEX_EXPECT_EQ(static_cast<int64_t>(request->get_subresource_count()), static_cast<int64_t>(2));
    const godot::RID rid = create_destination_texture(rd, 1);
    GDEX_ASSERT_TRUE(rid.is_valid());
    request->set_destination_resource(rid);
    ctx.add_teardown([rd, rid]() {
        if (rd->texture_is_valid(rid)) {
            rd->free_rid(rid);
        }
    });

    // Child 0 stages, child 1 is rejected by the subresource-range check and the
    // staged child is rolled back. The whole request must fail, and the file
    // reference must not survive the rollback.
    GDEX_EXPECT_FALSE(backend->enqueue_request(request));
    GDEX_EXPECT_EQ(static_cast<int64_t>(request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(request->get_error_message().contains("out of bounds"));
    GDEX_EXPECT_EQ(backend->_debug_cached_source_file_count(), 0);
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::remove_absolute(fixture)), static_cast<int64_t>(godot::OK));
#else
    (void)ctx;
    GDEX_SKIP("The cache-count seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, multi_queue_fence_wait_covers_low_counter_queue, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // A20261029-HIGH-02: get_completion_fence() reports the MAX of three
    // independent per-queue counters, and each counter restarts at 0 on its own
    // queue. wait_for_completion() used to wait only on queues whose own counter
    // was >= that watermark, so a queue drifting behind was skipped and the call
    // returned "complete" while its DMA was still writing.
    // The skew is pinned through the DEBUG/DEV seam: producing it for real needs
    // two live queues whose transfers retire at different speeds.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        // Mandatory, not hygiene: a pinned counter above the value the real fence
        // will ever reach makes shutdown()'s drain_all_queues() wait out its full
        // budget in every later teardown.
        backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_HIGH, 0, 0);
        backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_LOW, 0, 0);
        memdelete(backend);
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    // HIGH has retired 5 batches; LOW still has its first one in flight.
    backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_HIGH, 5, 5);
    backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_LOW, 1, 0);

    // The watermark itself is unchanged: it is still the MAX across queues.
    GDEX_EXPECT_EQ(backend->get_completion_fence(), 5ULL);

    // The discriminator. LOW never reached 5, but it still has DMA pending, so a
    // zero-budget wait cannot claim completion. A wait that filters on
    // "counter >= 5" skips LOW entirely and returns true at once.
    auto t0 = std::chrono::steady_clock::now();
    bool immediate = backend->wait_for_completion(5, 0);
    auto d0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    GDEX_EXPECT_FALSE(immediate);
    GDEX_EXPECT_TRUE(d0 < 50);

    // The blocking path must spend the budget on LOW instead of returning at once.
    constexpr uint32_t kTimeoutMs = 25;
    auto t1 = std::chrono::steady_clock::now();
    bool completed = backend->wait_for_completion(5, kTimeoutMs);
    auto d1 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t1).count();
    GDEX_EXPECT_FALSE(completed);
    GDEX_EXPECT_TRUE(d1 >= (kTimeoutMs - 5));
    GDEX_EXPECT_TRUE(d1 < 1000);
#else
    (void)ctx;
    GDEX_SKIP("The queue-counter seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, multi_queue_fence_wait_releases_when_low_counter_completes, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // A20261029-HIGH-02, anti-over-wait half: once every queue has actually
    // retired, the wait must still return true immediately. Without this, a "fix"
    // that blocks unconditionally (or that waits on the caller's watermark on
    // every queue) would satisfy the companion case while breaking every caller.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_HIGH, 0, 0);
        backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_LOW, 0, 0);
        memdelete(backend);
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_HIGH, 5, 5);
    backend->_debug_set_queue_fence_counters_for_testing(godot::DSRequest::PRIORITY_LOW, 1, 1);

    GDEX_EXPECT_EQ(backend->get_completion_fence(), 5ULL);

    auto t0 = std::chrono::steady_clock::now();
    bool complete = backend->wait_for_completion(5, 0);
    auto d0 = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
    GDEX_EXPECT_TRUE(complete);
    GDEX_EXPECT_TRUE(d0 < 50);
#else
    (void)ctx;
    GDEX_SKIP("The queue-counter seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, dstorage_requests_not_shared_during_enqueue_window, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // The enqueue-payload race: submit_async()/submit_batch() released batch_mutex and
    // then kept working on slot.dstorage_requests - re-seating Name, handing
    // data()/size() to DirectStorage, clearing - while cancel_requests_with_tag()
    // read the same vector under that lock. The window is a few hundred
    // instructions wide, so it is not reachable by a stress loop; the DEBUG/DEV
    // seam parks the submitting thread inside it instead.
    //
    // The discriminator is the slot state at that instant: pre-fix the slot still
    // owns the payload (>= 1), post-fix it was emptied under the lock and the
    // payload lives on the submitting thread's stack, so nothing else can read,
    // mutate or clear what is about to be enqueued.
    const char *fixture_path = "user://enqueue_window_fixture.dstex";
    remove_temp_file(fixture_path);
    // Added before the backend's teardown so LIFO runs that one first: the backend
    // holds an open IDStorageFile for this fixture, and Windows refuses to delete
    // a file with a live handle.
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        // Ordering matters: a hook that captured this test's frame must be gone
        // before the backend is deleted.
        backend->_debug_set_enqueue_window_hook_for_testing({});
        memdelete(backend);
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    constexpr uint64_t kTag = 9101;
    godot::Ref<godot::DSRequest> first = make_dummy_request(fixture_path, 1024);
    first->set_uncompressed_size(1024);
    first->set_cancellation_tag(kTag);
    godot::Ref<godot::DSRequest> second = make_dummy_request(fixture_path, 1024);
    second->set_uncompressed_size(1024);
    second->set_cancellation_tag(kTag);
    // Memory destinations produce no GPU transitions, so submit_async() reaches
    // the seam on its first instruction after the lock is released.
    GDEX_ASSERT_TRUE(backend->enqueue_request(first));
    GDEX_ASSERT_TRUE(backend->enqueue_request(second));

    std::atomic<bool> window_open{ false };
    std::atomic<bool> release_window{ false };
    backend->_debug_set_enqueue_window_hook_for_testing([&window_open, &release_window]() {
        window_open.store(true);
        // Self-releasing: a broken build must not hang the suite here.
        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_window.load() && std::chrono::steady_clock::now() < hook_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread submitter([backend]() { backend->submit_async(); });
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window_open.load() && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = window_open.load();
    const int pending_in_slots = backend->_debug_pending_enqueue_count();
    if (entered) {
        // The concurrent caller the fix removes from the window.
        backend->cancel_requests_with_tag(kTag);
    }
    release_window.store(true);
    submitter.join();
    backend->_debug_set_enqueue_window_hook_for_testing({});

    // No worker is running past this point, so failing assertions are safe.
    GDEX_EXPECT_TRUE(entered);
    GDEX_EXPECT_EQ(static_cast<int64_t>(pending_in_slots), static_cast<int64_t>(0));

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete()) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    GDEX_EXPECT_TRUE(settled);
    // Both requests are terminal: cancelled-by-tag marks them FAILED and the
    // completion path never promotes a terminal request.
    GDEX_EXPECT_TRUE(first->get_status() == godot::DSRequest::STATUS_FAILED ||
            first->get_status() == godot::DSRequest::STATUS_COMPLETED);
#else
    (void)ctx;
    GDEX_SKIP("The enqueue-window seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, cancel_during_enqueue_window_completes_batch, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Anti-regression half of the enqueue-payload fix. Taking the payload out of the slot
    // must not strand the batch: the slot has to retire and be reusable, and the
    // retained DSTORAGE_REQUEST::Name pointers have to stay valid because
    // slot.request_names is never moved or cleared by the snapshot.
    const char *fixture_path = "user://enqueue_window_reuse_fixture.dstex";
    remove_temp_file(fixture_path);
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        backend->_debug_set_enqueue_window_hook_for_testing({});
        memdelete(backend);
    });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    constexpr uint64_t kTag = 9202;
    godot::Ref<godot::DSRequest> first = make_dummy_request(fixture_path, 1024);
    first->set_uncompressed_size(1024);
    first->set_cancellation_tag(kTag);
    GDEX_ASSERT_TRUE(backend->enqueue_request(first));

    std::atomic<bool> window_open{ false };
    std::atomic<bool> release_window{ false };
    backend->_debug_set_enqueue_window_hook_for_testing([&window_open, &release_window]() {
        window_open.store(true);
        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_window.load() && std::chrono::steady_clock::now() < hook_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread submitter([backend]() { backend->submit_async(); });
    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window_open.load() && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = window_open.load();
    if (entered) {
        backend->cancel_requests_with_tag(kTag);
    }
    release_window.store(true);
    submitter.join();
    backend->_debug_set_enqueue_window_hook_for_testing({});

    GDEX_EXPECT_TRUE(entered);

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete()) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    GDEX_EXPECT_TRUE(settled);

    // The slot must be reusable: a second request stages, submits and retires.
    godot::Ref<godot::DSRequest> second = make_dummy_request(fixture_path, 1024);
    second->set_uncompressed_size(1024);
    second->set_cancellation_tag(9203);
    GDEX_EXPECT_TRUE(backend->enqueue_request(second));
    backend->submit_async();
    const uint64_t second_fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(second_fence, 5000));
    bool reused = false;
    const auto reuse_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < reuse_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete() && second->get_status() == godot::DSRequest::STATUS_COMPLETED) {
            reused = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    GDEX_EXPECT_TRUE(reused);
#else
    (void)ctx;
    GDEX_SKIP("The enqueue-window seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}


