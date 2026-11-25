// tests/stream_maintenance_tests.cpp
//
// Covers the e1f09ee..HEAD stream-maintenance changes (backend-agnostic,
// SoftwareFallback-friendly):
//
// 1. Stream enqueue parity (src/core/ds_context.cpp): public
//    enqueue_request(req, true) queues for flush_stream_async(), which drains
//    through enqueue_stream_request_internal() ->
//    enqueue_request_impl(req, true). Report bookkeeping (including
//    report_order) must be identical to the sync path even though per-request
//    collect/finalize is hoisted to once per flush.
// 2. poll_completed() budgets (src/integration/ds_preloader.cpp): single-scan
//    terminal-ID collect with per-item extract; p_max_items / p_max_usec must
//    bound the drain without dropping the remainder, and empty-pending polls
//    with any budget must return 0 immediately.
//
// Linkage: src/core + src/integration compile into the test library
// (.gdextest.toml [gdextest.tests] sources), so direct includes are used.
// DSContext instances are owned per-test (teardown shutdown + memdelete);
// the static singleton is restored by the destructor.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "core/ds_context.h"
#include "core/ds_request.h"
#include "integration/ds_preloader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int64_t kProbeBytes = 256;

godot::DSContext *new_test_context(::gdextest::TestContext &ctx) {
    godot::DSContext *ds = memnew(godot::DSContext);
    ctx.add_teardown([ds]() {
        ds->shutdown_ds();
        memdelete(ds);
    });
    return ds;
}

godot::DSContext *require_initialized_context(::gdextest::TestContext &ctx) {
    godot::DSContext *ds = new_test_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
    }
    return ds;
}

bool write_probe_file(const char *p_path) {
    godot::PackedByteArray payload;
    payload.resize(kProbeBytes);
    for (int64_t i = 0; i < kProbeBytes; ++i) {
        payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    godot::Ref<godot::FileAccess> out =
        godot::FileAccess::open(godot::String(p_path), godot::FileAccess::WRITE);
    if (out.is_null()) {
        return false;
    }
    out->store_buffer(payload);
    out->close();
    return true;
}

void remove_probe_file(const char *p_path) {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_valid()) {
        (void)dir->remove(godot::String(p_path));
    }
}

godot::Ref<godot::DSRequest> make_memory_request(const char *p_path, uint64_t p_tag = 0) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    req->set_file_path(godot::String(p_path));
    req->set_size(static_cast<uint32_t>(kProbeBytes));
    req->set_cancellation_tag(p_tag);
    return req;
}

// Bounded convergence pump for the stream path: each pass lets the hoisted
// once-per-flush collect harvest finished batches. delay_msec yields the
// CPU so the fallback completion worker (same batch_mutex) gets a window;
// a pure spin starves it and the request stays SUBMITTED (observed: whole
// test finished in 9 ms with the worker never scheduled).
bool pump_stream_to_terminal(godot::DSContext *ds, const godot::Ref<godot::DSRequest> &req) {
    godot::OS *os = godot::OS::get_singleton();
    for (int i = 0; i < 40; ++i) {
        ds->flush_stream_async();
        ds->wait_for_completion(500);
        if (os != nullptr) {
            os->delay_msec(50);
        }
        ds->flush_stream_async(); // harvest what the wait just finished
        const int64_t st = static_cast<int64_t>(req->get_status());
        if (st == static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED) ||
                st == static_cast<int64_t>(godot::DSRequest::STATUS_FAILED)) {
            return st == static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED);
        }
    }
    return false;
}

} // namespace

GDEX_TEST_T(stream_maintenance, flush_stream_async_empty_returns_ok, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    // Empty queue on an initialized context: early return must precede any
    // backend slot use (ERR_UNCONFIGURED is only for uninitialized contexts).
    GDEX_EXPECT_FALSE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->flush_stream_async()), static_cast<int64_t>(godot::OK));
}

GDEX_TEST_T(stream_maintenance, sync_and_stream_reports_agree, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);

    constexpr const char *kPath = "user://gdextest_stream_parity.bin";
    if (!write_probe_file(kPath)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([kPath]() { remove_probe_file(kPath); });

    // Sync path: immediate backend enqueue with per-request maintenance.
    // Capture the report BEFORE flush_batch: finalize removes execution
    // reports once a request turns terminal, and the fallback default dict
    // (actual_backend "None") is never empty, so post-hoc reads can't tell
    // "removed" from "never written".
    godot::Ref<godot::DSRequest> sync_req = make_memory_request(kPath);
    GDEX_EXPECT_TRUE(ds->enqueue_request(sync_req, false));
    const godot::String sync_backend = ds->get_actual_backend(sync_req);
    const int64_t sync_dest = static_cast<int64_t>(ds->get_actual_destination_type(sync_req));
    GDEX_EXPECT_TRUE(sync_backend == ds->get_active_backend_name());
    ds->flush_batch();
    ds->wait_for_completion(5000);
    GDEX_EXPECT_EQ(static_cast<int64_t>(sync_req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));

    // Stream path: queue-only enqueue, drained by the hoisted-maintenance flush.
    // Nonzero tag keeps stream_tags distinct from the tag-0 wildcard used by
    // poll-style APIs. Reports are compared BEFORE convergence: finalize
    // removes execution reports once a request turns terminal by design.
    godot::Ref<godot::DSRequest> stream_req = make_memory_request(kPath, 77031);
    GDEX_EXPECT_TRUE(ds->enqueue_request(stream_req, true));
    GDEX_EXPECT_TRUE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 1);
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->flush_stream_async()), static_cast<int64_t>(godot::OK));
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);

    // Parity: identical bookkeeping despite skipped per-request maintenance.
    // Read BEFORE pumping to completion: finalize removes the stream report
    // once terminal, same lifecycle as the sync path above. Unlike the sync
    // path (report read before submit_batch), the stream report can only be
    // observed after flush_stream_async, which ends with the hoisted
    // finalize: the fallback worker may already have completed the 256-byte
    // probe inside submit_async, in which case the report is legitimately
    // gone. get_actual_backend() returns "None" once finalized, so
    // actual_backend != "None" indicates the report is still live.
    const godot::String stream_backend = ds->get_actual_backend(stream_req);
    const int64_t stream_status = static_cast<int64_t>(stream_req->get_status());
    const bool stream_terminal =
            stream_status == static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED) ||
            stream_status == static_cast<int64_t>(godot::DSRequest::STATUS_FAILED);
    const bool report_present = (stream_backend != godot::String("None"));
    if (report_present) {
        GDEX_EXPECT_TRUE(stream_backend == sync_backend);
        GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_actual_destination_type(stream_req)), sync_dest);
    } else {
        // Only finalize drops the report, and only for terminal requests.
        GDEX_EXPECT_TRUE(stream_terminal);
    }

    if (!pump_stream_to_terminal(ds, stream_req)) {
        const int64_t st = static_cast<int64_t>(stream_req->get_status());
        const godot::String err = stream_req->get_error_message();
        GDEX_FAIL(std::string("stream request did not reach STATUS_COMPLETED (status=") +
                std::to_string(st) + ", error='" + ::gdextest::to_std_string(err) + "').");
    }
}

// Regression (#0132): the CPU fallback delivered nothing for uncompressed
// memory requests. process_request() copied the payload with the guarded
// set_destination_buffer() AFTER enqueue_request had already set the request to
// STATUS_SUBMITTED, so the immutability guard rejected the write and the
// request still reported COMPLETED - with an empty buffer. Nothing here
// pre-allocates a destination buffer, which is exactly the shape that used to
// fail (Linux/macOS/headless and any export that lands on SoftwareFallback).
GDEX_TEST_T(stream_maintenance, sync_memory_request_delivers_payload, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);

    constexpr const char *kPath = "user://gdextest_stream_payload.bin";
    if (!write_probe_file(kPath)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([kPath]() { remove_probe_file(kPath); });

    godot::Ref<godot::DSRequest> req = make_memory_request(kPath);
    GDEX_EXPECT_TRUE(ds->enqueue_request(req, false));
    ds->flush_batch();
    ds->wait_for_completion(5000);
    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));

    const godot::PackedByteArray delivered = req->get_destination_buffer();
    GDEX_EXPECT_EQ(delivered.size(), static_cast<int64_t>(kProbeBytes));
    bool matches = delivered.size() == static_cast<int64_t>(kProbeBytes);
    for (int64_t i = 0; matches && i < kProbeBytes; ++i) {
        matches = delivered[i] == static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    GDEX_EXPECT_TRUE(matches);
}

GDEX_TEST_T(preloader_budget, empty_poll_budgets_return_zero, TAG_INTEGRATION) {
    // No pending entries anywhere: every budget combination must be an
    // immediate no-op (exercises the single-scan + per-item-extract path
    // with nothing to extract, and the Time-singleton usec branch).
    GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 0, 0), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 1, 0), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 0, 1000), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 64, 4000), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(12345, 64, 4000), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(12345), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_cpu_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_cpu_prewarm_count(12345), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_gpu_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_gpu_prewarm_count(12345), 0);
    ctx.add_teardown([]() { (void)godot::DSPreloader::poll_completed(); });
}

GDEX_TEST_T(preloader_budget, cache_delegates_budgets, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
    cache->set_enabled(true);
    ctx.add_teardown([cache]() {
        cache->clear();
        memdelete(cache);
    });
    // Delegation (ds_texture_cache.cpp -> DSPreloader) must forward budgets.
    GDEX_EXPECT_EQ(cache->poll_completed(0, 1, 1000), 0);
    GDEX_EXPECT_EQ(cache->poll_completed(0, 64, 4000), 0);
    GDEX_EXPECT_EQ(cache->poll_completed(12345, 64, 4000), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(12345), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_cpu_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_cpu_prewarm_count(12345), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_gpu_prewarm_count(0), 0);
    GDEX_EXPECT_EQ(cache->get_unbuilt_gpu_prewarm_count(12345), 0);
}

GDEX_TEST_T(preloader_cancellation_retirement, request_in_flight_lifecycle, TAG_INTEGRATION) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    GDEX_EXPECT_FALSE(req->is_gpu_in_flight());
    GDEX_EXPECT_EQ(req->get_cancel_timestamp_usec(), static_cast<uint64_t>(0));

    req->set_gpu_in_flight(true);
    GDEX_EXPECT_TRUE(req->is_gpu_in_flight());

    req->set_cancel_timestamp_usec(1234567ULL);
    GDEX_EXPECT_EQ(req->get_cancel_timestamp_usec(), 1234567ULL);

    req->set_gpu_in_flight(false);
    GDEX_EXPECT_FALSE(req->is_gpu_in_flight());
}

GDEX_TEST_T(preloader_cancellation_retirement, preloader_is_terminal_gate_and_fallback, TAG_INTEGRATION) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();

    // 1. Pending request is not terminal
    req->set_status(godot::DSRequest::STATUS_PENDING);
    GDEX_EXPECT_FALSE(godot::DSPreloader::is_terminal(req));

    // 2. Completed request is always terminal
    req->set_status(godot::DSRequest::STATUS_COMPLETED);
    req->set_gpu_in_flight(true);
    GDEX_EXPECT_TRUE(godot::DSPreloader::is_terminal(req));

    // 3. Failed request with gpu_in_flight == true must NOT be terminal (VRAM protection gate)
    req->set_status(godot::DSRequest::STATUS_FAILED);
    req->set_gpu_in_flight(true);
    req->set_cancel_timestamp_usec(0);
    GDEX_EXPECT_FALSE(godot::DSPreloader::is_terminal(req));

    // 4. Failed request with gpu_in_flight == false IS terminal (clean hardware retirement)
    req->set_gpu_in_flight(false);
    GDEX_EXPECT_TRUE(godot::DSPreloader::is_terminal(req));

    // 5. Bounded fallback timeout: simulate a cancelled request that has hung past the timeout
    req->set_gpu_in_flight(true);
    godot::Time *time = godot::Time::get_singleton();
    if (time != nullptr) {
        uint64_t now_us = time->get_ticks_usec();
        uint64_t past_timestamp = (now_us > 10000ULL) ? (now_us - 10000ULL) : 1ULL;
        req->set_cancel_timestamp_usec(past_timestamp);

        // Before timeout (with 5.0s timeout budget), must NOT be terminal (VRAM protected)
        GDEX_EXPECT_FALSE(godot::DSPreloader::is_terminal(req, 5000000ULL));
        GDEX_EXPECT_TRUE(req->is_gpu_in_flight());

        // Past timeout (with 5000us timeout budget when >=10000us has elapsed), trips fallback:
        // force clears gpu_in_flight, returns true
        GDEX_EXPECT_TRUE(godot::DSPreloader::is_terminal(req, 5000ULL));
        GDEX_EXPECT_FALSE(req->is_gpu_in_flight());
    }
}

GDEX_TEST_T(preloader_cancellation_retirement, context_cancelled_stream_progress_gate, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    godot::PackedStringArray paths;
    paths.push_back("res://dummy_tex_1.dstex");
    paths.push_back("res://dummy_tex_2.dstex");
    uint64_t tag = ds->prewarm_async(paths);
    ds->set_prewarm_total(tag, 2);

    GDEX_EXPECT_FALSE(ds->is_prewarm_complete(tag));

    // Cancel tag
    ds->cancel_prewarm(tag);

    // Cancelled tag with completed < total must NOT report complete prematurely
    GDEX_EXPECT_FALSE(ds->is_prewarm_complete(tag));

    // Once both terminal failures are counted (retired), it completes
    ds->count_stream_terminal_failure(tag);
    GDEX_EXPECT_FALSE(ds->is_prewarm_complete(tag));
    ds->count_stream_terminal_failure(tag);
    GDEX_EXPECT_TRUE(ds->is_prewarm_complete(tag));
}

// A tag whose remaining work was destroyed with the backend can never reach
// completed == total, so the predicate must retire it: once nothing can complete
// a tag, "still loading" is a lie that makes await_prewarm_finished() wait
// forever and holds any consumer's load bracket open.
GDEX_TEST_T(preloader_cancellation_retirement, prewarm_tag_retires_after_shutdown, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    const uint64_t tag = 700001;
    ds->set_prewarm_total(tag, 5);
    GDEX_EXPECT_FALSE(ds->is_prewarm_complete(tag));

    ctx.add_teardown([ds]() {
        if (ds != nullptr && ds->is_initialized()) {
            ds->shutdown_ds();
        }
    });

    ds->shutdown_ds();
    ds->initialize_ds();

    GDEX_EXPECT_TRUE(ds->is_prewarm_complete(tag));
    const godot::Vector2i progress = ds->get_prewarm_progress(tag);
    GDEX_EXPECT_EQ(static_cast<int64_t>(progress.x), static_cast<int64_t>(0));
    GDEX_EXPECT_EQ(static_cast<int64_t>(progress.y), static_cast<int64_t>(5));
}

GDEX_TEST_T(request_payload_immutability, setters_rejected_when_submitted_or_gpu_in_flight, TAG_UNIT) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();

    // 1. Initial state: STATUS_PENDING and !gpu_in_flight -> mutable
    GDEX_EXPECT_TRUE(req->is_mutable());
    req->set_file_path("res://initial_path.bin");
    req->set_offset(1024);
    req->set_size(4096);
    req->set_uncompressed_size(4096);

    godot::PackedByteArray initial_buf;
    initial_buf.resize(4096);
    initial_buf[0] = 0xAA;
    req->set_destination_buffer(initial_buf);

    GDEX_EXPECT_TRUE(req->get_file_path() == "res://initial_path.bin");
    GDEX_EXPECT_EQ(req->get_offset(), static_cast<uint64_t>(1024));
    GDEX_EXPECT_EQ(req->get_size(), static_cast<uint32_t>(4096));
    GDEX_EXPECT_EQ(req->get_destination_buffer().size(), static_cast<int64_t>(4096));

    // 2. Submit the request -> is_mutable() must become false
    req->set_status(godot::DSRequest::STATUS_SUBMITTED);
    GDEX_EXPECT_FALSE(req->is_mutable());

    // 3. Attempt mutations while submitted -> must be rejected without mutating fields
    req->set_file_path("res://tampered_path.bin");
    req->set_offset(9999);
    req->set_size(8888);

    godot::PackedByteArray tampered_buf;
    tampered_buf.resize(16);
    tampered_buf[0] = 0xFF;
    req->set_destination_buffer(tampered_buf);

    GDEX_EXPECT_TRUE(req->get_file_path() == "res://initial_path.bin");
    GDEX_EXPECT_EQ(req->get_offset(), static_cast<uint64_t>(1024));
    GDEX_EXPECT_EQ(req->get_size(), static_cast<uint32_t>(4096));
    GDEX_EXPECT_EQ(req->get_destination_buffer().size(), static_cast<int64_t>(4096));
    GDEX_EXPECT_EQ(static_cast<int>(req->get_destination_buffer()[0]), 0xAA);

    // 4. Test gpu_in_flight gate independently
    req->set_status(godot::DSRequest::STATUS_PENDING);
    req->set_gpu_in_flight(true);
    GDEX_EXPECT_FALSE(req->is_mutable());

    req->set_file_path("res://gpu_tampered.bin");
    GDEX_EXPECT_TRUE(req->get_file_path() == "res://initial_path.bin");
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Regression for #0277: when submit_async() returns false (ERR_BUSY) after
// draining the last items from stream_queue, stream_pending_submit_requests > 0
// must keep has_stream_pending() true and get_stream_pending_count() > 0 so
// DSRuntime._process() continues calling flush_stream_async().
GDEX_TEST_T(stream_maintenance, has_stream_pending_counts_staged_submit_retries, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    GDEX_EXPECT_FALSE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);

    ds->_debug_set_stream_pending_submit_requests_for_testing(4);
    ctx.add_teardown([ds]() {
        ds->_debug_set_stream_pending_submit_requests_for_testing(0);
    });

    GDEX_EXPECT_TRUE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 4);

    ds->_debug_set_stream_pending_submit_requests_for_testing(0);
    GDEX_EXPECT_FALSE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);
}
#endif

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Step 2 of the prewarm stutter plan: only a memory-only stream may be
// offloaded. has_stream_gpu_pending() must see both queued GPU requests and
// GPU requests staged for a retried submit, and go false once they are gone.
GDEX_TEST_T(stream_maintenance, has_stream_gpu_pending_tracks_queued_and_staged, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());

    const char *path = "user://gdextest_stream_gpu_pending.bin";
    if (!write_probe_file(path)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([path]() { remove_probe_file(path); });

    // Memory-only queue: safe to offload even while a stream flush is pending.
    const uint64_t mem_tag = 101;
    godot::Ref<godot::DSRequest> mem_req = make_memory_request(path, mem_tag);
    GDEX_ASSERT_TRUE(ds->enqueue_request(mem_req, true));
    GDEX_EXPECT_TRUE(ds->has_stream_pending());
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());

    // A GPU-destination request in the same queue flips the verdict.
    const uint64_t gpu_tag = 102;
    godot::Ref<godot::DSRequest> gpu_req;
    gpu_req.instantiate();
    gpu_req->set_file_path(godot::String(path));
    gpu_req->set_size(static_cast<uint32_t>(kProbeBytes));
    gpu_req->set_cancellation_tag(gpu_tag);
    gpu_req->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
    GDEX_ASSERT_TRUE(ds->enqueue_request(gpu_req, true));
    GDEX_EXPECT_TRUE(ds->has_stream_gpu_pending());

    // Cancellation drops the GPU request from the queue again; the memory
    // request is still pending, so has_stream_pending() stays true.
    ds->cancel_prewarm(gpu_tag);
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());
    GDEX_EXPECT_TRUE(ds->has_stream_pending());

    // Staged-for-retry GPU state is tracked independently of the queue.
    ds->_debug_set_stream_pending_submit_requests_for_testing(3);
    ds->_debug_set_stream_pending_submit_has_gpu_for_testing(true);
    ctx.add_teardown([ds]() {
        ds->_debug_set_stream_pending_submit_requests_for_testing(0);
        ds->_debug_set_stream_pending_submit_has_gpu_for_testing(false);
    });
    GDEX_EXPECT_TRUE(ds->has_stream_gpu_pending());
    ds->_debug_set_stream_pending_submit_has_gpu_for_testing(false);
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());

    // The GPU flag without a non-zero staged count must not block offload.
    ds->_debug_set_stream_pending_submit_has_gpu_for_testing(true);
    ds->_debug_set_stream_pending_submit_requests_for_testing(0);
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());
}
#endif

// A flush running on a worker must not consume a GPU-destination request that
// was staged after the DSRuntime gate snapshot. The gate is an optimisation;
// the flush re-derives affinity from the executing thread.
GDEX_TEST_T(stream_maintenance, worker_flush_leaves_gpu_requests_queued, TAG_INTEGRATION) {
    const char *path = "user://gdextest_worker_flush_gpu.bin";
    remove_probe_file(path); // clean any stale fixture from an interrupted run
    if (!write_probe_file(path)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([path]() { remove_probe_file(path); }); // runs after shutdown
    godot::DSContext *ds = require_initialized_context(ctx);

    // Gate verdict: memory only, so DSRuntime would dispatch a worker flush.
    const uint64_t mem_tag = 201;
    godot::Ref<godot::DSRequest> mem_req = make_memory_request(path, mem_tag);
    GDEX_ASSERT_TRUE(ds->enqueue_request(mem_req, true));
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());

    // Same-frame GPU stage invalidates that snapshot before the worker runs.
    const uint64_t gpu_tag = 202;
    godot::Ref<godot::DSRequest> gpu_req;
    gpu_req.instantiate();
    gpu_req->set_file_path(godot::String(path));
    gpu_req->set_size(static_cast<uint32_t>(kProbeBytes));
    gpu_req->set_cancellation_tag(gpu_tag);
    gpu_req->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
    GDEX_ASSERT_TRUE(ds->enqueue_request(gpu_req, true));
    GDEX_EXPECT_TRUE(ds->has_stream_gpu_pending());

    // The worker's flush drains memory work but must leave GPU work queued.
    std::thread worker([ds]() { ds->flush_stream_async(); });
    worker.join();

    GDEX_EXPECT_TRUE(ds->has_stream_gpu_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 1);
    GDEX_EXPECT_EQ(static_cast<int64_t>(gpu_req->get_status()),
                   static_cast<int64_t>(godot::DSRequest::Status(0)));
    GDEX_EXPECT_TRUE(ds->has_stream_pending());

    // Control: the same request is processed normally on the main thread.
    GDEX_ASSERT_EQ(static_cast<int64_t>(ds->flush_stream_async()), static_cast<int64_t>(godot::OK));
    GDEX_EXPECT_FALSE(ds->has_stream_gpu_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);
}

// Regression for #0277: concurrent worker + main-thread flush_stream_async()
// calls over >128 requests (spanning multiple flushes) must never strand
// staged requests in an unsubmitted slot with has_stream_pending() == false.
GDEX_TEST_T(stream_maintenance, concurrent_flush_stream_async_drains_all_requests, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);

    constexpr const char *kPath = "user://gdextest_concurrent_stream_flush.bin";
    if (!write_probe_file(kPath)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([kPath]() { remove_probe_file(kPath); });

    constexpr int kRequestCount = 160; // > max_logical_requests (128) in flush_stream_async()
    std::vector<godot::Ref<godot::DSRequest>> reqs;
    reqs.reserve(kRequestCount);
    for (int i = 0; i < kRequestCount; ++i) {
        godot::Ref<godot::DSRequest> req = make_memory_request(kPath);
        GDEX_EXPECT_TRUE(ds->enqueue_request(req, true));
        reqs.push_back(req);
    }
    GDEX_EXPECT_TRUE(ds->has_stream_pending());

    std::thread worker([ds]() {
        for (int i = 0; i < 10; ++i) {
            ds->flush_stream_async();
        }
    });
    for (int i = 0; i < 10; ++i) {
        ds->flush_stream_async();
    }
    worker.join();

    godot::OS *os = godot::OS::get_singleton();
    for (int pass = 0; pass < 80; ++pass) {
        if (ds->has_stream_pending()) {
            ds->flush_stream_async();
        }
        ds->wait_for_completion(200);
        if (os != nullptr) {
            os->delay_msec(20);
        }
        ds->collect_completed_results();
        if (!ds->has_stream_pending()) {
            bool all_done = true;
            for (int i = 0; i < kRequestCount; ++i) {
                const auto st = reqs[i]->get_status();
                if (st != godot::DSRequest::STATUS_COMPLETED && st != godot::DSRequest::STATUS_FAILED) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) {
                break;
            }
        }
    }

    GDEX_EXPECT_FALSE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(ds->get_stream_pending_count(), 0);
    for (int i = 0; i < kRequestCount; ++i) {
        GDEX_EXPECT_EQ(static_cast<int64_t>(reqs[i]->get_status()),
                static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    }
}

// Finding 1.3 Phase A RED: a loading-mode flush at queue_capacity=1024 must
// drain 256 requests (cap/4), splitting a 407-request scene across the two
// 1.1 slots. At HEAD it drains the old hard-clamped 128.
GDEX_TEST_T(stream_maintenance, stream_flush_loading_scales_while_preserving_pipelining, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    const uint16_t previous_capacity = ds->get_queue_capacity();
    ds->set_queue_capacity(1024);
    godot::DSPreloader::set_background_pacing(false);
    // The backend reads queue_capacity from the global singleton while this
    // test drives a local DSContext; align both so admission accepts 256.
    godot::DSContext *singleton = godot::DSContext::get_singleton();
    GDEX_ASSERT_TRUE(singleton != nullptr);
    const uint16_t previous_singleton_capacity = singleton->get_queue_capacity();
    if (singleton != ds) {
        singleton->set_queue_capacity(1024);
    }
    ctx.add_teardown([ds, singleton, previous_capacity, previous_singleton_capacity]() {
        ds->set_queue_capacity(previous_capacity);
        if (singleton != ds) {
            singleton->set_queue_capacity(previous_singleton_capacity);
        }
        godot::DSPreloader::set_background_pacing(false);
    });

    constexpr const char *kPath = "user://gdextest_loading_flush_scale.bin";
    GDEX_ASSERT_TRUE(write_probe_file(kPath));
    ctx.add_teardown([]() { remove_probe_file(kPath); });

    std::vector<godot::Ref<godot::DSRequest>> reqs;
    reqs.reserve(300);
    for (int i = 0; i < 300; ++i) {
        godot::Ref<godot::DSRequest> req = make_memory_request(kPath);
        GDEX_ASSERT_TRUE(ds->enqueue_request(req, true));
        reqs.push_back(req);
    }
    const int before = ds->get_stream_pending_count();
    ds->flush_stream_async();
    const int staged_by_first_flush = before - ds->get_stream_pending_count();
    // 256 keeps 44 requests for the second slot; 128 (HEAD) is too small and
    // 510 (v1) would collapse the scene into one monolithic batch.
    GDEX_EXPECT_EQ(static_cast<int64_t>(staged_by_first_flush), static_cast<int64_t>(256));
}

// Background pacing must keep the strict 128-request cap even at high queue
// capacity, so large scenes cannot produce 256+ request bursts during 60 FPS
// gameplay. Proves DSPreloader -> DSContext mode propagation as well.
GDEX_TEST_T(stream_maintenance, stream_flush_background_pacing_caps_at_128, TAG_INTEGRATION) {
    godot::DSContext *ds = require_initialized_context(ctx);
    const uint16_t previous_capacity = ds->get_queue_capacity();
    ds->set_queue_capacity(1024);
    godot::DSPreloader::set_background_pacing(true);
    // DSPreloader forwards to the module singleton; this test drives a local
    // DSContext, so assert the forwarding separately and set the local flag.
    godot::DSContext *singleton = godot::DSContext::get_singleton();
    GDEX_ASSERT_TRUE(singleton != nullptr);
    GDEX_EXPECT_TRUE(singleton->is_stream_background_pacing());
    ds->set_stream_background_pacing(true);
    const uint16_t previous_singleton_capacity = singleton->get_queue_capacity();
    if (singleton != ds) {
        singleton->set_queue_capacity(1024);
    }
    ctx.add_teardown([ds, singleton, previous_capacity, previous_singleton_capacity]() {
        ds->set_queue_capacity(previous_capacity);
        if (singleton != ds) {
            singleton->set_queue_capacity(previous_singleton_capacity);
        }
        ds->set_stream_background_pacing(false);
        godot::DSPreloader::set_background_pacing(false);
    });

    constexpr const char *kPath = "user://gdextest_background_flush_cap.bin";
    GDEX_ASSERT_TRUE(write_probe_file(kPath));
    ctx.add_teardown([]() { remove_probe_file(kPath); });

    std::vector<godot::Ref<godot::DSRequest>> reqs;
    reqs.reserve(300);
    for (int i = 0; i < 300; ++i) {
        godot::Ref<godot::DSRequest> req = make_memory_request(kPath);
        GDEX_ASSERT_TRUE(ds->enqueue_request(req, true));
        reqs.push_back(req);
    }
    const int before = ds->get_stream_pending_count();
    ds->flush_stream_async();
    const int staged_by_first_flush = before - ds->get_stream_pending_count();
    GDEX_EXPECT_EQ(static_cast<int64_t>(staged_by_first_flush), static_cast<int64_t>(128));
}
