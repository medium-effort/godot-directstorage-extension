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
#include "core/ds_error_handler.h"
#include "core/ds_profiler.h"

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#include "core/ds_request.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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

// Win32 reports paths with backslashes; Godot composes them with forward slashes.
// Any comparison of the two has to normalise, or it fails on a path that is equal.
std::string normalized_path(const godot::String &p_path) {
    std::string path = ::gdextest::to_std_string(p_path);
    for (char &ch : path) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return path;
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
    // Concurrent callers of ds_win32_get_d3d12_device() / ds_win32_get_d3d12_command_queue()
    // while another thread invalidates the cached pointers. Returns must be safe ComPtr
    // instances with no pointer tearing or dangling references, and a blocked main thread must
    // not cost a full acquisition budget per miss (the wait is bounded by main-loop liveness).
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    const bool d3d12_live = rd != nullptr && rs->get_current_rendering_driver_name().to_lower() == "d3d12";

    // Warm the cache first so the workers' first queries are served from it. Without this the
    // "a worker successfully acquired a device" assertion would race the invalidation storm.
    Microsoft::WRL::ComPtr<ID3D12Device> warm_device = godot::ds_win32_get_d3d12_device();
    if (d3d12_live) {
        GDEX_EXPECT_TRUE(warm_device.Get() != nullptr);
    }

    std::atomic<bool> stop_flag{false};
    std::atomic<int> query_success_count{0};
    constexpr int kWorkerThreads = 4;
    std::vector<std::thread> workers;
    workers.reserve(kWorkerThreads);

    const auto body_start = std::chrono::steady_clock::now();

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

    // Bounded wait for the workers to have hit the warm cache before invalidating under them.
    if (d3d12_live) {
        const auto warm_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
        while (query_success_count.load(std::memory_order_relaxed) == 0 &&
                std::chrono::steady_clock::now() < warm_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
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

    const int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - body_start).count();

    // 4 concurrent waiters against a blocked main thread used to cost two full 5 s budgets
    // (~11 s). The liveness-bounded wait costs ~400 ms per round; 4 s leaves generous slack.
    GDEX_EXPECT_TRUE(elapsed_ms < 4000);

    if (d3d12_live) {
        // The warm-cache window above makes this deterministic, not a start-up race.
        GDEX_EXPECT_TRUE(query_success_count.load(std::memory_order_relaxed) > 0);
        // The cache recovers after the invalidate storm, and the recovered device is the same
        // Godot D3D12 device the workers saw.
        Microsoft::WRL::ComPtr<ID3D12Device> recovered = godot::ds_win32_get_d3d12_device();
        GDEX_EXPECT_TRUE(recovered.Get() != nullptr);
        GDEX_EXPECT_TRUE(recovered.Get() == warm_device.Get());
    }
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

    // Execute the true rollback logic invoked on mid-loop enqueue_one failure.
    // The containers it edits belong to a backend instance now (they used to be
    // process-global), so the call goes through one. No device and no
    // initialize() are required: real rollback is pure bookkeeping.
    godot::Win32Backend *rollback_backend = memnew(godot::Win32Backend);
    ctx.add_teardown([rollback_backend]() { memdelete(rollback_backend); });
    rollback_backend->rollback_staged_requests(
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

    godot::Win32Backend *stage_backend = memnew(godot::Win32Backend);
    ctx.add_teardown([stage_backend]() { memdelete(stage_backend); });
    godot::Ref<godot::DSRequest> child;
    bool staged = stage_backend->stage_single_mip_request(parent, child);
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
    // get_completion_fence() reports the MAX of three
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
    // Anti-over-wait half: once every queue has actually
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

GDEX_TEST_T(win32_remediation, readers_see_no_backend_until_initialize_returns, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // A backend may only become visible to readers after its initialize() has
    // returned: until then its queues, fences and events do not exist, so any
    // reader that takes the pointer calls into a half-built object. The window is
    // a scheduler race, so the DEBUG/DEV seam parks the initializing thread at the
    // position publication used to occupy and the test thread asks the public
    // getters what they see from there.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton absent: real extension not loaded in this fixture.");
    }
    const bool was_initialized = ds->is_initialized();
    // LIFO: clear the hook before anything else can observe the context, then put
    // the process back in the state the previous suite left it in.
    ctx.add_teardown([ds, was_initialized]() {
        ds->_debug_set_initialization_window_hook_for_testing({});
        if (was_initialized && !ds->is_initialized()) {
            ds->initialize_ds();
        } else if (!was_initialized && ds->is_initialized()) {
            ds->shutdown_ds();
        }
    });

    ds->shutdown_ds();
    GDEX_EXPECT_TRUE(!ds->is_initialized());

    godot::String observed_name = godot::String("<unobserved>");
    bool observed_complete = false;
    bool hook_fired = false;
    // initialize_ds() runs on this (main) thread, exactly as production does: on a
    // D3D12 host the pre-backend UMA check reaches the render thread, which the
    // engine only services from the main loop, so driving init from a worker would
    // park against a render-thread request instead of the window under test. The
    // reader below is the concurrent observer.
    ds->_debug_set_initialization_window_hook_for_testing([&]() {
        hook_fired = true;
        std::thread reader([&]() {
            observed_name = ds->get_active_backend_name();
            observed_complete = ds->is_batch_complete();
        });
        reader.join();
    });

    const bool initialize_result = ds->initialize_ds();
    ds->_debug_set_initialization_window_hook_for_testing({});

    GDEX_EXPECT_TRUE(hook_fired);
    // Discriminator: with the backend published too early these read its name and
    // ask it about queues that have not been created yet.
    GDEX_EXPECT_TRUE(observed_name == godot::String("None"));
    GDEX_EXPECT_TRUE(observed_complete);
    // Anti-regression: the reorder must not strand the pointer.
    GDEX_EXPECT_TRUE(initialize_result);
    GDEX_EXPECT_TRUE(ds->is_initialized());
    GDEX_EXPECT_TRUE(ds->get_active_backend_name() != godot::String("None"));
#else
    (void)ctx;
    GDEX_SKIP("The initialization-window seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, config_fields_are_atomic, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Structural half of the same invariant: initialize_ds_locked() reads these
    // four fields on whichever thread first needs the context, while the public
    // setters are callable from any thread. No runtime oracle for the absence of a
    // data race exists in this toolchain (MSVC plus prebuilt DirectStorage and
    // godot-cpp binaries, so ThreadSanitizer cannot instrument both sides), so the
    // case asserts the declared type instead of pretending to observe a torn read.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton absent: real extension not loaded in this fixture.");
    }
    const int64_t previous_staging = static_cast<int64_t>(ds->get_staging_buffer_size_mb());
    const int64_t previous_capacity = static_cast<int64_t>(ds->get_queue_capacity());
    ctx.add_teardown([ds, previous_staging, previous_capacity]() {
        ds->set_staging_buffer_size_mb(static_cast<uint32_t>(previous_staging));
        ds->set_queue_capacity(static_cast<uint16_t>(previous_capacity));
    });

    GDEX_EXPECT_TRUE(ds->_debug_config_fields_are_atomic_for_testing());

    // The conversion must keep the getter/setter contracts intact.
    ds->set_staging_buffer_size_mb(192);
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_staging_buffer_size_mb()), static_cast<int64_t>(192));
    ds->set_queue_capacity(256);
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_queue_capacity()), static_cast<int64_t>(256));
#else
    (void)ctx;
    GDEX_SKIP("The configuration-field type probe is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, runtime_configuration_applies_per_loaded_instance, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // The DirectStorage settings live inside dstorage.dll, so a freshly loaded runtime
    // must be configured again. A process-wide latch configured only the first
    // instance, and every later load (after a shutdown released the modules) ran on
    // SDK defaults with the caller's preference ignored. Two probe cycles bracket a
    // full unload, so the second cycle necessarily sees a new module instance.
    if (!godot::Win32Backend::probe_system_support()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    const int after_first_probe = godot::Win32Backend::_debug_dstorage_config_apply_count();
    GDEX_EXPECT_TRUE(after_first_probe >= 1);

    // Same instance: applying again must be a no-op, the settings are already in it.
    godot::Win32Backend::apply_dstorage_configuration();
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::Win32Backend::_debug_dstorage_config_apply_count()),
            static_cast<int64_t>(after_first_probe));

    // initialize() claims the modules the probe stashed; shutdown() releases them,
    // and that release is what discards the configuration.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    if (!backend->initialize()) {
        memdelete(backend);
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }
    backend->shutdown();
    memdelete(backend);

    // New instance: the configuration has to be applied again.
    GDEX_EXPECT_TRUE(godot::Win32Backend::probe_system_support());
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::Win32Backend::_debug_dstorage_config_apply_count()),
            static_cast<int64_t>(after_first_probe + 1));
#else
    (void)ctx;
    GDEX_SKIP("The runtime-configuration counter is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, runtime_module_load_uses_hardened_search_flags, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // One loader for the DirectStorage runtime, with one search policy. The suite
    // pins the policy the helper applies, not the path a dependency resolves to: a
    // dependency that exists in System32 is bound from System32 under both the
    // hardened flags and a bare LoadLibraryW, so only the flag mask distinguishes
    // them. The mask is what decides whether a dependency absent from System32 fails
    // the load or is silently satisfied from the working directory or PATH.
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps == nullptr) {
        GDEX_SKIP("ProjectSettings unavailable in this fixture.");
    }

    // Locate the shipped pair the same way production does.
    godot::String source_dir;
    const godot::PackedStringArray search_dirs = godot::Win32Backend::get_runtime_search_dirs();
    for (int i = 0; i < search_dirs.size(); i++) {
        const godot::String dir = search_dirs[i];
        const godot::Ref<godot::FileAccess> core = godot::FileAccess::open(dir.path_join("dstoragecore.dll"), godot::FileAccess::READ);
        const godot::Ref<godot::FileAccess> runtime = godot::FileAccess::open(dir.path_join("dstorage.dll"), godot::FileAccess::READ);
        if (core.is_valid() && runtime.is_valid()) {
            source_dir = dir;
            break;
        }
    }
    if (source_dir.is_empty()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }

    // A directory that has never held these DLLs, so the module path asserted below
    // cannot be a leftover mapping of the shipped copy.
    const godot::String fixture_abs = ps->globalize_path("user://gdextest_runtime_loader");
    const godot::String core_abs = fixture_abs.path_join("dstoragecore.dll");
    const godot::String runtime_abs = fixture_abs.path_join("dstorage.dll");
    // Registered first, so it runs last (teardowns are LIFO): the module handles are
    // released before the files disappear.
    ctx.add_teardown([core_abs, runtime_abs, fixture_abs]() {
        godot::DirAccess::remove_absolute(runtime_abs);
        godot::DirAccess::remove_absolute(core_abs);
        godot::DirAccess::remove_absolute(fixture_abs);
    });

    godot::DirAccess::remove_absolute(runtime_abs);
    godot::DirAccess::remove_absolute(core_abs);
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::make_dir_recursive_absolute(fixture_abs)),
            static_cast<int64_t>(godot::OK));
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::copy_absolute(source_dir.path_join("dstoragecore.dll"), core_abs)),
            static_cast<int64_t>(godot::OK));
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::copy_absolute(source_dir.path_join("dstorage.dll"), runtime_abs)),
            static_cast<int64_t>(godot::OK));
    {
        const godot::Ref<godot::FileAccess> copied = godot::FileAccess::open(runtime_abs, godot::FileAccess::READ);
        GDEX_EXPECT_TRUE(copied.is_valid());
        if (copied.is_valid()) {
            GDEX_EXPECT_TRUE(copied->get_length() > 0);
            copied->close();
        }
    }

    const HMODULE runtime_module = godot::Win32Backend::load_runtime_module(fixture_abs, "dstorage.dll");
    if (runtime_module != nullptr) {
        ctx.add_teardown([runtime_module]() { FreeLibrary(runtime_module); });
    }
    GDEX_EXPECT_TRUE(runtime_module != nullptr);

    // The policy itself, stated independently of the production constant.
    godot::String recorded_path;
    uint32_t recorded_flags = 0;
    godot::Win32Backend::_debug_last_runtime_module_load_for_testing(recorded_path, recorded_flags);
    const uint32_t expected_flags = static_cast<uint32_t>(LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    GDEX_EXPECT_EQ(static_cast<int64_t>(recorded_flags), static_cast<int64_t>(expected_flags));
    // The two flags are mutually exclusive, so an implementation that reaches for the
    // altered-search-path flag instead must fail here rather than pass by accident.
    GDEX_EXPECT_EQ(static_cast<int64_t>(recorded_flags & LOAD_WITH_ALTERED_SEARCH_PATH), static_cast<int64_t>(0));
    GDEX_EXPECT_EQ(normalized_path(recorded_path), normalized_path(runtime_abs));

    if (runtime_module != nullptr) {
        wchar_t buffer[4096] = {};
        const DWORD length = GetModuleFileNameW(runtime_module, buffer, static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0])));
        GDEX_EXPECT_TRUE(length > 0);
        GDEX_EXPECT_EQ(normalized_path(godot::String::utf16(reinterpret_cast<const char16_t *>(buffer))),
                normalized_path(runtime_abs));
    }

    // The core load is the one whose own dependency resolution the policy governs.
    const HMODULE core_module = godot::Win32Backend::load_runtime_module(fixture_abs, "dstoragecore.dll");
    if (core_module != nullptr) {
        ctx.add_teardown([core_module]() { FreeLibrary(core_module); });
    }
    GDEX_EXPECT_TRUE(core_module != nullptr);
    godot::Win32Backend::_debug_last_runtime_module_load_for_testing(recorded_path, recorded_flags);
    GDEX_EXPECT_EQ(static_cast<int64_t>(recorded_flags), static_cast<int64_t>(expected_flags));
    GDEX_EXPECT_EQ(normalized_path(recorded_path), normalized_path(core_abs));
#else
    (void)ctx;
    GDEX_SKIP("The runtime-loader seam is compiled out of release builds.");
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

GDEX_TEST_T(win32_remediation, submit_preserves_unread_error, TAG_INTEGRATION) {
#ifdef _WIN32
    // A DirectStorage failure lands in the DSErrorHandler singleton and is read back
    // through the documented has_error() / get_last_error() pair, which a script polls
    // once per frame. Both submit paths used to reset that state unconditionally, so a
    // failure reported by the completion thread between frames was already gone when
    // the next frame's submit returned.
    //
    // No seam is needed for this contract: report_error() is public, so the state a
    // failure leaves behind can be installed directly, and its survival across a
    // *successful* submit is the whole measurement. The point of doing it with a
    // sentinel rather than a real queue failure is that it is deterministic: a clean
    // batch reports no error of its own, so anything missing from the state was wiped
    // by the submit.
    godot::DSErrorHandler *eh = godot::DSErrorHandler::get_singleton();
    if (eh == nullptr) {
        GDEX_SKIP("DSErrorHandler singleton absent: real extension not loaded in this fixture.");
    }

    const char *fixture_path = "user://submit_preserves_error_fixture.dstex";
    remove_temp_file(fixture_path);
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });
    // LIFO: this runs after the backend is destroyed, so no completion thread can
    // repopulate the flag once it has been cleared.
    ctx.add_teardown([eh]() { eh->clear_error(); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    constexpr uint64_t kTag = 9205;
    godot::Ref<godot::DSRequest> request = make_dummy_request(fixture_path, 1024);
    request->set_uncompressed_size(1024);
    request->set_cancellation_tag(kTag);

    const godot::String sentinel("diagnostic sentinel: unread queue error");
    eh->clear_error();
    eh->report_error(sentinel);
    GDEX_ASSERT_TRUE(eh->has_error());

    GDEX_ASSERT_TRUE(backend->enqueue_request(request));
    backend->submit_async();

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete() && request->get_status() == godot::DSRequest::STATUS_COMPLETED) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // A failing batch would overwrite the sentinel with its own message, so this has
    // to hold before the survival assertions mean anything.
    GDEX_EXPECT_TRUE(settled);

    // The invariant: a reported error stays readable until a caller clears it.
    GDEX_EXPECT_TRUE(eh->has_error());
    GDEX_EXPECT_TRUE(eh->get_last_error().contains(sentinel));

    // The other half: the clean batch that followed must not inherit the error either.
    // The sample is matched by the byte count this request contributes, so a previous
    // suite's sample cannot satisfy the read.
    godot::DSProfiler *profiler = godot::DSProfiler::get_singleton();
    if (profiler != nullptr) {
        bool sample_found = false;
        int64_t sample_error_state = -1;
        const auto sample_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < sample_deadline) {
            godot::Ref<godot::DSProfilerResult> sample = profiler->get_last_result();
            if (sample.is_valid() && sample->get_compressed_bytes_read() == 1024 &&
                    sample->get_request_count() == 1) {
                sample_error_state = static_cast<int64_t>(sample->get_error_state());
                sample_found = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        GDEX_EXPECT_TRUE(sample_found);
        if (sample_found) {
            GDEX_EXPECT_EQ(sample_error_state, static_cast<int64_t>(0));
        }
    }
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, queue_error_retrieval_is_serialized, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // IDStorageQueue::RetrieveErrorRecord() has two callers: the completion thread, which reaches
    // it through retrieve_queue_error() while holding batch_mutex, and process_queue_errors(),
    // which used to reach it holding only error_mutex - and that lock is released before the loop,
    // so the drain was effectively unserialised. The SDK ties the call to the error event and
    // counts failures "since the last RetrieveErrorRecord call", so two racing drains split one
    // failure record between them.
    //
    // The seam parks the draining thread after it takes the queue-access lock and before the first
    // RetrieveErrorRecord() call; the test then asks the backend whether its batch_mutex is free.
    // Post-fix it is held, so the probe answers false.
    godot::DSErrorHandler *eh = godot::DSErrorHandler::get_singleton();
    if (eh == nullptr) {
        GDEX_SKIP("DSErrorHandler singleton absent: real extension not loaded in this fixture.");
    }

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });
    // LIFO: the hook captures this frame, so it is cleared before the backend is deleted and
    // before unregister_queue() drops the borrowed mutex pointer.
    ctx.add_teardown([eh]() { eh->_debug_set_queue_error_reader_hook_for_testing({}); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    std::atomic<bool> window_open{ false };
    std::atomic<bool> release_window{ false };
    eh->_debug_set_queue_error_reader_hook_for_testing([&window_open, &release_window]() {
        window_open.store(true);
        // Self-releasing: a broken build must not hang the suite here.
        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_window.load() && std::chrono::steady_clock::now() < hook_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread drainer([eh]() { eh->process_queue_errors(); });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window_open.load() && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = window_open.load();
    // No batch was submitted, so nothing else in the backend can be holding this lock: a free
    // lock here means the drain is not serialised with the completion thread's own drain.
    const bool queue_access_held = entered && !backend->_debug_batch_mutex_is_free_for_testing();

    release_window.store(true);
    drainer.join();
    eh->_debug_set_queue_error_reader_hook_for_testing({});

    GDEX_EXPECT_TRUE(entered);
    GDEX_EXPECT_TRUE(queue_access_held);
#else
    (void)ctx;
    GDEX_SKIP("The queue-error reader seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, queue_error_drain_requires_error_event, TAG_INTEGRATION) {
#ifdef _WIN32
    // The SDK ties RetrieveErrorRecord() to the queue's error event: it may only be
    // called once that event has signalled, and not again until the next signal.
    // The completion path used to read the record on every batch completion - twice
    // per batch, because the main-thread poll drains too - so a failure record could
    // be consumed by a slot whose batch did not own the failing request.
    //
    // Measurement: a batch that reports no failure must not reach the API at all.
    // The counter is process-wide, so a delta is used and sibling cases that cancel
    // requests (which legitimately raise it) cannot distort the expectation.
    const char *fixture_path = "user://queue_error_event_fixture.dstex";
    remove_temp_file(fixture_path);
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    godot::Ref<godot::DSRequest> request = make_dummy_request(fixture_path, 1024);
    request->set_uncompressed_size(1024);
    request->set_cancellation_tag(9301);

    const int reads_before = backend->_debug_error_record_read_count();
    GDEX_ASSERT_TRUE(backend->enqueue_request(request));
    backend->submit_async();

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete() && request->get_status() == godot::DSRequest::STATUS_COMPLETED) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    GDEX_ASSERT_TRUE(settled);

    // The invariant under test: a clean batch never reads an error record.
    GDEX_EXPECT_EQ(static_cast<int64_t>(backend->_debug_error_record_read_count()),
            static_cast<int64_t>(reads_before));
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, queue_error_report_runs_outside_batch_mutex, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Two contracts in one measurement. (1) A failure that no retiring batch can
    // carry must still be reported - the old per-completion drain only ever looked
    // at the record while a batch was finishing, so a record with no batch in
    // flight was never surfaced. (2) push_error()/report_error() must run after
    // batch_mutex is released, or the completion thread parks inside the engine
    // while holding the lock every enqueue and eviction caller needs.
    //
    // The seam installs the record the SDK would have returned after an error event
    // (the only real source of an out-of-band record is a device removal) and runs
    // the same stash-or-report decision the error-event handler uses. Only the SDK
    // read is bypassed, so no runtime or initialized backend is required.
    godot::DSErrorHandler *eh = godot::DSErrorHandler::get_singleton();
    if (eh == nullptr) {
        GDEX_SKIP("DSErrorHandler singleton absent: real extension not loaded in this fixture.");
    }

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() {
        backend->_debug_set_queue_error_report_hook_for_testing({});
        memdelete(backend);
    });
    ctx.add_teardown([eh]() { eh->clear_error(); });

    const godot::String sentinel("diagnostic sentinel: out-of-band queue error");
    eh->clear_error();

    std::atomic<bool> window_open{ false };
    std::atomic<bool> release_window{ false };
    backend->_debug_set_queue_error_report_hook_for_testing([&window_open, &release_window]() {
        window_open.store(true);
        // Self-releasing: a broken build must not hang the suite here.
        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_window.load() && std::chrono::steady_clock::now() < hook_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread injector([backend, sentinel]() { backend->_debug_inject_queue_error_for_testing(0, sentinel); });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window_open.load() && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = window_open.load();
    // Nothing else can hold the lock here: no batch exists. A held lock at this
    // instant means the report is still inside the batch_mutex block.
    const bool mutex_free = entered && backend->_debug_batch_mutex_is_free_for_testing();

    release_window.store(true);
    injector.join();
    backend->_debug_set_queue_error_report_hook_for_testing({});

    GDEX_EXPECT_TRUE(entered);
    GDEX_EXPECT_TRUE(mutex_free);
    GDEX_EXPECT_TRUE(eh->has_error());
    GDEX_EXPECT_TRUE(eh->get_last_error().contains(sentinel));
#else
    (void)ctx;
    GDEX_SKIP("The queue-error report seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, queue_error_second_record_survives_to_its_batch, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // One queue, one batch, two records captured before that batch retires: a
    // cancellation and a fatal failure naming the same staged request. Both must
    // reach the batch and the fatal one must decide it. While QueueState held a
    // single pending_error, the second record was routed to the out-of-band report
    // and cleared, so poll_batch_status() then saw "no record" for the batch that
    // owned it - and "no record" means "batch succeeded" - so the failing request
    // completed as STATUS_COMPLETED.
    //
    // Both records are injected from the submit's enqueue window: the slot is
    // frozen (closing, so the queue already counts as unfinished) while the
    // requests have not been handed to DirectStorage yet. That makes the window
    // deterministic instead of a race against DMA completion.
    const char *fixture_path = "user://queue_error_two_records_fixture.dstex";
    remove_temp_file(fixture_path);
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }
    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    godot::Ref<godot::DSRequest> request = make_dummy_request(fixture_path, 1024);
    request->set_uncompressed_size(1024);
    request->set_cancellation_tag(9402);
    // Low priority maps to queue index 0 (priority_to_queue_index), so the
    // records below are injected on the queue that actually carries this batch.
    request->set_priority(godot::DSRequest::PRIORITY_LOW);

    const godot::String cancel_message("diagnostic sentinel: cancelled request record");
    const godot::String fatal_message("diagnostic sentinel: fatal request record");
    std::atomic<int> injected_records{ 0 };
    backend->_debug_set_enqueue_window_hook_for_testing([&]() {
        // The fresh backend stages this first batch on slot 0; the second slot is
        // only consulted so a rotated active slot cannot turn this into a vacuous
        // pass.
        godot::PackedStringArray names = backend->_debug_staged_request_names_for_testing(0, 0);
        if (names.is_empty()) {
            names = backend->_debug_staged_request_names_for_testing(0, 1);
        }
        if (names.is_empty()) {
            return;
        }
        const godot::String staged_name = names[0];
        backend->_debug_inject_queue_error_for_testing(0, cancel_message, staged_name, false);
        backend->_debug_inject_queue_error_for_testing(0, fatal_message, staged_name, true);
        injected_records.store(2);
    });
    ctx.add_teardown([backend]() { backend->_debug_set_enqueue_window_hook_for_testing({}); });

    GDEX_ASSERT_TRUE(backend->enqueue_request(request));
    backend->submit_async();

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete() && request->get_status() == godot::DSRequest::STATUS_FAILED) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    backend->_debug_set_enqueue_window_hook_for_testing({});

    GDEX_EXPECT_EQ(injected_records.load(), 2);
    GDEX_EXPECT_TRUE(settled);
    GDEX_EXPECT_EQ(static_cast<int64_t>(request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(request->get_error_message().contains(fatal_message));
#else
    (void)ctx;
    GDEX_SKIP("The queue-error injection seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, queue_error_two_fatal_records_fail_whole_batch, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Two fatal records naming two different staged requests, both captured before
    // the batch that staged them retires. Only one record can decide the batch: the
    // claim loop examines the slot's own entry first and folds every later record
    // into that failure, clearing it. This case pins what that fold guarantees, so a
    // later change to it has to be deliberate - both requests must end
    // STATUS_FAILED (a fatally failed batch never leaves a request looking
    // completed), the first record keeps the message root, and the folded record's
    // request is attributed to that root instead of carrying its own message.
    //
    // Both records are injected from the submit's enqueue window: the slot is
    // frozen (closing, so the queue already counts as unfinished) while the
    // requests have not been handed to DirectStorage yet. That makes the window
    // deterministic instead of a race against DMA completion.
    const char *fixture_path = "user://queue_error_two_fatal_fixture.dstex";
    remove_temp_file(fixture_path);
    ctx.add_teardown([fixture_path]() { remove_temp_file(fixture_path); });

    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });

    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }
    GDEX_ASSERT_TRUE(write_temp_dstex(fixture_path, 64, 64, 1, 16384));

    godot::Ref<godot::DSRequest> root_request = make_dummy_request(fixture_path, 1024);
    root_request->set_uncompressed_size(1024);
    root_request->set_cancellation_tag(9412);
    // Low priority maps to queue index 0 (priority_to_queue_index), so the
    // records below are injected on the queue that actually carries this batch.
    root_request->set_priority(godot::DSRequest::PRIORITY_LOW);

    godot::Ref<godot::DSRequest> folded_request = make_dummy_request(fixture_path, 1024);
    folded_request->set_uncompressed_size(1024);
    folded_request->set_cancellation_tag(9413);
    folded_request->set_priority(godot::DSRequest::PRIORITY_LOW);

    const godot::String root_message("diagnostic sentinel: first fatal record");
    const godot::String folded_message("diagnostic sentinel: second fatal record");
    // Written by the hook and read after submit_async() returns, so both happen on
    // this thread with a sequencing edge between them.
    godot::String root_name;
    std::atomic<int> injected_records{ 0 };
    backend->_debug_set_enqueue_window_hook_for_testing([&]() {
        // The fresh backend stages this first batch on slot 0; the second slot is
        // only consulted so a rotated active slot cannot turn this into a vacuous
        // pass.
        godot::PackedStringArray names = backend->_debug_staged_request_names_for_testing(0, 0);
        if (names.size() < 2) {
            names = backend->_debug_staged_request_names_for_testing(0, 1);
        }
        if (names.size() < 2) {
            return;
        }
        // Staging order is enqueue order, so names[0] belongs to root_request and
        // names[1] to folded_request. The first injection lands on the entry owned
        // by the slot that holds these names, which is also the entry the completing
        // batch examines first - that is what makes the winner deterministic.
        root_name = names[0];
        backend->_debug_inject_queue_error_for_testing(0, root_message, names[0], true);
        backend->_debug_inject_queue_error_for_testing(0, folded_message, names[1], true);
        injected_records.store(2);
    });
    ctx.add_teardown([backend]() { backend->_debug_set_enqueue_window_hook_for_testing({}); });

    GDEX_ASSERT_TRUE(backend->enqueue_request(root_request));
    GDEX_ASSERT_TRUE(backend->enqueue_request(folded_request));
    backend->submit_async();

    const uint64_t fence = backend->get_completion_fence();
    GDEX_EXPECT_TRUE(backend->wait_for_completion(fence, 5000));
    bool settled = false;
    const auto settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < settle_deadline) {
        backend->poll_batch_status();
        if (backend->is_batch_complete() &&
                root_request->get_status() == godot::DSRequest::STATUS_FAILED &&
                folded_request->get_status() == godot::DSRequest::STATUS_FAILED) {
            settled = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    backend->_debug_set_enqueue_window_hook_for_testing({});

    GDEX_EXPECT_EQ(injected_records.load(), 2);
    GDEX_EXPECT_TRUE(settled);
    GDEX_EXPECT_EQ(static_cast<int64_t>(root_request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(static_cast<int64_t>(folded_request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    // The first record decides the batch and keeps its message root.
    GDEX_EXPECT_TRUE(!root_name.is_empty());
    GDEX_EXPECT_TRUE(root_request->get_error_message().contains(root_message));
    // The folded record is cleared, so its request is attributed to the root rather
    // than completed or given a second root message. Changing this (for example by
    // surfacing folded records separately) must update this case on purpose.
    const godot::String folded_prefix =
            godot::String("DirectStorage batch aborted due to failure in request [") + root_name + "]: ";
    GDEX_EXPECT_TRUE(folded_request->get_error_message().begins_with(folded_prefix));
    GDEX_EXPECT_TRUE(!folded_request->get_error_message().contains(folded_message));
#else
    (void)ctx;
    GDEX_SKIP("The queue-error injection seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, second_instance_shutdown_preserves_first_instance_state, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Instance isolation: a second backend's initialize()/shutdown()/destruction
    // must leave the first instance's bookkeeping untouched. While these
    // containers were file-level statics, `second->shutdown()` cleared `first`'s
    // chunked-request state and released its resource guards, so a staged GPU
    // request lost both its AddRef and the parent link that propagates a terminal
    // status - the caller's handle stayed SUBMITTED forever. The seam seeds the
    // containers, so no device, no .dstex and no initialize() are needed.
    godot::Win32Backend *first = memnew(godot::Win32Backend);
    godot::Win32Backend *second = memnew(godot::Win32Backend);
    auto second_alive = std::make_shared<bool>(true);
    ctx.add_teardown([first, second, second_alive]() {
        if (*second_alive) {
            memdelete(second);
        }
        memdelete(first);
    });

    first->_debug_seed_instance_state_for_testing();
    {
        const godot::Dictionary before = first->_debug_instance_state_for_testing();
        GDEX_EXPECT_EQ(static_cast<int64_t>(before["chunked_requests"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(before["chunk_parents"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(before["resource_guards"]), static_cast<int64_t>(1));
    }

    // The destructive operation the defect names.
    second->shutdown();
    {
        const godot::Dictionary after_shutdown = first->_debug_instance_state_for_testing();
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_shutdown["chunked_requests"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_shutdown["chunk_parents"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_shutdown["resource_guards"]), static_cast<int64_t>(1));
    }

    // Destruction must be instance-scoped too, not only shutdown().
    memdelete(second);
    *second_alive = false;
    second = nullptr;
    {
        const godot::Dictionary after_delete = first->_debug_instance_state_for_testing();
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_delete["chunked_requests"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_delete["chunk_parents"]), static_cast<int64_t>(1));
        GDEX_EXPECT_EQ(static_cast<int64_t>(after_delete["resource_guards"]), static_cast<int64_t>(1));
    }
    // `first` is released by the teardown, exercising its own cleanup path.
#else
    (void)ctx;
    GDEX_SKIP("Instance-isolation seams are compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, fence_wait_does_not_hold_lifecycle_lock, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // DSContext::wait_for_completion() takes backend_lifecycle_mutex (shared) and
    // then blocks in the backend fence wait for the caller's whole budget.
    // shutdown_ds() needs that lock exclusively while holding init_mutex, so one
    // stalled transfer parks a quit for the caller's timeout - and on this lock's
    // writer-queueing rules it can park the eviction drain's reader behind the
    // pending writer as well. This case stalls the fence through the counter seam
    // and measures both halves.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton absent: real extension not loaded in this fixture.");
    }
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    const bool was_initialized = ds->is_initialized();
    if (!was_initialized && !ds->initialize_ds()) {
        GDEX_SKIP("DSContext could not initialize on this machine.");
    }
    if (!ds->is_d3d12_backend()) {
        GDEX_SKIP("D3D12 DirectStorage backend unavailable (headless or non-D3D12 driver).");
    }

    // A valid RID is all the reader probe needs: is_resource_in_flight() takes the
    // lifecycle reader lock before consulting the backend, so the probe measures
    // lock acquisition, not this RID's fate.
    const godot::RID probe_rid = create_destination_texture(rd, 1);
    GDEX_EXPECT_TRUE(probe_rid.is_valid());

    // Above any counter a real transfer in this process can reach, so the backend's
    // own fence never retires it; 4 s is a caller budget the tests never use.
    constexpr uint64_t kStallFence = 1000000;
    constexpr int64_t kStallBudgetMsec = 4000;

    ctx.add_teardown([ds, was_initialized, rd, probe_rid]() {
        // Unpin first: a pinned counter above what the real fence will reach makes
        // drain_all_queues() spend its whole budget in any later shutdown. A no-op
        // once the context is down.
        for (int q = 0; q < 3; ++q) {
            ds->_debug_set_backend_fence_counters_for_testing(q, 0, 0);
        }
        if (was_initialized) {
            if (!ds->is_initialized()) {
                ds->initialize_ds();
            }
        } else if (ds->is_initialized()) {
            ds->shutdown_ds();
        }
        if (probe_rid.is_valid() && rd->texture_is_valid(probe_rid)) {
            rd->free_rid(probe_rid);
        }
    });

    bool pinned = false;
    for (int attempt = 0; attempt < 3 && !pinned; ++attempt) {
        for (int q = 0; q < 3; ++q) {
            ds->_debug_set_backend_fence_counters_for_testing(q, kStallFence, 0);
        }
        pinned = ds->get_completion_fence() == kStallFence;
        if (!pinned) {
            godot::OS::get_singleton()->delay_msec(20);
        }
    }
    GDEX_EXPECT_TRUE(pinned);
    if (!pinned) {
        return;
    }

    std::atomic<bool> wait_done{false};
    bool wait_result = true;
    int64_t wait_elapsed_msec = 0;
    std::thread waiter([&]() {
        const int64_t t0 = godot::Time::get_singleton()->get_ticks_msec();
        wait_result = ds->wait_for_completion(static_cast<int>(kStallBudgetMsec));
        wait_elapsed_msec = godot::Time::get_singleton()->get_ticks_msec() - t0;
        wait_done.store(true, std::memory_order_release);
    });

    // The stall precondition: if the waiter already returned, the fixture did not
    // stall and the measurements below cannot discriminate (FAIL, not SKIP).
    for (int waited = 0; waited < 200 && !wait_done.load(std::memory_order_acquire); waited += 5) {
        godot::OS::get_singleton()->delay_msec(5);
    }
    const bool stalled = !wait_done.load(std::memory_order_acquire);

    // Unpin before shutdown so drain_all_queues() has nothing pinned to drain. The
    // waiter is already inside its wait, so this does not release it.
    for (int q = 0; q < 3; ++q) {
        ds->_debug_set_backend_fence_counters_for_testing(q, 0, 0);
    }

    int64_t shutdown_elapsed_msec = 0;
    std::atomic<bool> shutdown_done{false};
    std::thread shutdown_thread([&]() {
        const int64_t t0 = godot::Time::get_singleton()->get_ticks_msec();
        ds->shutdown_ds();
        shutdown_elapsed_msec = godot::Time::get_singleton()->get_ticks_msec() - t0;
        shutdown_done.store(true, std::memory_order_release);
    });
    // Let shutdown set shut_down and queue on the exclusive lock before probing.
    godot::OS::get_singleton()->delay_msec(50);

    const int64_t reader_t0 = godot::Time::get_singleton()->get_ticks_msec();
    (void)ds->is_resource_in_flight(probe_rid);
    const int64_t reader_elapsed_msec = godot::Time::get_singleton()->get_ticks_msec() - reader_t0;

    shutdown_thread.join();
    waiter.join();

    GDEX_EXPECT_TRUE(stalled);
    // The discriminator: one stalled transfer must not delay shutdown beyond one
    // wait slice. Without the slice this lands near the waiter's 4 s budget.
    GDEX_FAIL_UNLESS(shutdown_elapsed_msec < 1500,
            std::string("shutdown blocked for ") + std::to_string(shutdown_elapsed_msec) +
            " ms behind a stalled fence wait (reader probe " + std::to_string(reader_elapsed_msec) +
            " ms, waiter " + std::to_string(wait_elapsed_msec) + " ms)");
    // The reader half: a queued writer must not park the eviction drain's reader.
    GDEX_FAIL_UNLESS(reader_elapsed_msec < 1500,
            std::string("is_resource_in_flight() blocked for ") + std::to_string(reader_elapsed_msec) +
            " ms behind a queued shutdown writer");
    GDEX_EXPECT_FALSE(wait_result);
    GDEX_EXPECT_TRUE(wait_elapsed_msec >= 200);
    GDEX_EXPECT_TRUE(ds->was_last_shutdown_clean());
#else
    (void)ctx;
    GDEX_SKIP("The fence-counter seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, fence_wait_spends_the_whole_budget, TAG_INTEGRATION) {
#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // Control half: bounding the lock hold must not shorten the caller's budget.
    // A "fix" that returns after the first slice would pass the case above and
    // fail here.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton absent: real extension not loaded in this fixture.");
    }
    const bool was_initialized = ds->is_initialized();
    if (!was_initialized && !ds->initialize_ds()) {
        GDEX_SKIP("DSContext could not initialize on this machine.");
    }
    if (!ds->is_d3d12_backend()) {
        GDEX_SKIP("D3D12 DirectStorage backend unavailable (headless or non-D3D12 driver).");
    }

    constexpr uint64_t kStallFence = 1000000;
    constexpr int64_t kBudgetMsec = 800;

    ctx.add_teardown([ds, was_initialized]() {
        for (int q = 0; q < 3; ++q) {
            ds->_debug_set_backend_fence_counters_for_testing(q, 0, 0);
        }
        if (was_initialized) {
            if (!ds->is_initialized()) {
                ds->initialize_ds();
            }
        } else if (ds->is_initialized()) {
            ds->shutdown_ds();
        }
    });

    bool pinned = false;
    for (int attempt = 0; attempt < 3 && !pinned; ++attempt) {
        for (int q = 0; q < 3; ++q) {
            ds->_debug_set_backend_fence_counters_for_testing(q, kStallFence, 0);
        }
        pinned = ds->get_completion_fence() == kStallFence;
        if (!pinned) {
            godot::OS::get_singleton()->delay_msec(20);
        }
    }
    GDEX_EXPECT_TRUE(pinned);
    if (!pinned) {
        return;
    }

    const int64_t t0 = godot::Time::get_singleton()->get_ticks_msec();
    const bool completed = ds->wait_for_completion(static_cast<int>(kBudgetMsec));
    const int64_t elapsed_msec = godot::Time::get_singleton()->get_ticks_msec() - t0;

    for (int q = 0; q < 3; ++q) {
        ds->_debug_set_backend_fence_counters_for_testing(q, 0, 0);
    }

    GDEX_EXPECT_FALSE(completed);
    GDEX_FAIL_UNLESS(elapsed_msec >= kBudgetMsec - 100,
            std::string("a stalled fence wait returned after ") + std::to_string(elapsed_msec) +
            " ms for an " + std::to_string(kBudgetMsec) + " ms budget");
    GDEX_EXPECT_TRUE(elapsed_msec < 2000);
    // The reset must reach the backend: whatever the fixture's residual fence is,
    // it is no longer the pinned one.
    GDEX_EXPECT_TRUE(ds->get_completion_fence() != kStallFence);
#else
    (void)ctx;
    GDEX_SKIP("The fence-counter seam is compiled out of release builds.");
#endif
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, runtime_info_reports_installed_runtime, TAG_UNIT) {
#ifdef _WIN32
    (void)ctx;
    // The capability report must distinguish the SDK header this binary was
    // built against from the dstoragecore.dll actually present. Reading it is
    // read-only: it must not load a runtime that is not already loaded.
    const godot::Dictionary info = godot::Win32Backend::get_runtime_info();
    GDEX_EXPECT_TRUE(info.has("version"));
    GDEX_EXPECT_TRUE(info.has("version_major"));
    GDEX_EXPECT_TRUE(info.has("version_minor"));
    GDEX_EXPECT_TRUE(info.has("loaded"));
    GDEX_EXPECT_TRUE(info.has("capabilities"));

    const godot::Dictionary caps = info["capabilities"];
    GDEX_EXPECT_TRUE(caps.has("zstd"));
    GDEX_EXPECT_TRUE(caps.has("zstd_probed"));
    GDEX_EXPECT_TRUE(caps.has("gacl"));
    GDEX_EXPECT_TRUE(caps.has("enqueue_requests"));
    GDEX_EXPECT_TRUE(caps.has("configuration2"));
    GDEX_EXPECT_TRUE(caps.has("creator_id"));

    const bool loaded_before = info["loaded"];
    const bool zstd = caps["zstd"];
    const bool zstd_probed = caps["zstd_probed"];
    const bool gacl = caps["gacl"];
    const bool enqueue_requests = caps["enqueue_requests"];
    const int64_t major = info["version_major"];
    const int64_t minor = info["version_minor"];
    const godot::String version = info["version"];

    if (version == "Unavailable") {
        GDEX_SKIP("No DirectStorage runtime with a version resource is staged in this fixture.");
    }
    GDEX_EXPECT_TRUE(major >= 1);
    GDEX_EXPECT_TRUE(minor >= 0);

    // Capability flags must agree with the version they were derived from. The
    // Zstd probe is authoritative when it ran; otherwise the version decides.
    const bool at_least_1_3 = major > 1 || (major == 1 && minor >= 3);
    const bool at_least_1_4 = major > 1 || (major == 1 && minor >= 4);
    GDEX_EXPECT_EQ(static_cast<int64_t>(enqueue_requests), static_cast<int64_t>(at_least_1_3));
    if (!zstd_probed) {
        GDEX_EXPECT_EQ(static_cast<int64_t>(zstd), static_cast<int64_t>(at_least_1_4));
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(gacl), static_cast<int64_t>(zstd && at_least_1_4));

    // Reading the report must not have loaded the runtime modules.
    const godot::Dictionary after = godot::Win32Backend::get_runtime_info();
    GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<bool>(after["loaded"])), static_cast<int64_t>(loaded_before));

    // The context report carries the same values, so GDScript has one source.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    GDEX_ASSERT_TRUE(ds != nullptr);
    const godot::Dictionary report = ds->get_capability_report();
    GDEX_EXPECT_TRUE(report.has("runtime_version"));
    GDEX_EXPECT_TRUE(report.has("runtime_loaded"));
    GDEX_EXPECT_TRUE(report.has("runtime_capabilities"));
    GDEX_EXPECT_TRUE(godot::String(report["runtime_version"]) == version);
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_remediation, runtime_info_probes_loaded_runtime, TAG_INTEGRATION) {
#ifdef _WIN32
    (void)ctx;
    // The Zstd probe only runs once the DirectStorage shim is live, so this case
    // pins the loaded path: version, loaded flag, and probe/version agreement.
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); });
    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed on this machine.");
    }

    const godot::Dictionary info = godot::Win32Backend::get_runtime_info();
    GDEX_EXPECT_TRUE(static_cast<bool>(info["loaded"]));
    const godot::Dictionary caps = info["capabilities"];
    const int64_t major = info["version_major"];
    const int64_t minor = info["version_minor"];
    const bool at_least_1_4 = major > 1 || (major == 1 && minor >= 4);
    // 1.3 exposes no 1.4-only export, so the probe must not run there; on 1.4 it
    // must run and agree with the version. Either way Zstd follows the probe.
    GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<bool>(caps["zstd_probed"])), static_cast<int64_t>(at_least_1_4));
    GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<bool>(caps["zstd"])), static_cast<int64_t>(at_least_1_4));
    GDEX_EXPECT_TRUE(static_cast<bool>(caps["enqueue_requests"]));
#else
    (void)ctx;
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}


