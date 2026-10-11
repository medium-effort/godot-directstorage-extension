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
    // once terminal, same lifecycle as the sync path above.
    const godot::Dictionary stream_report = ds->get_request_execution_report(stream_req);
    const godot::String stream_backend = ds->get_actual_backend(stream_req);
    GDEX_EXPECT_FALSE(stream_backend == godot::String("None"));
    GDEX_EXPECT_TRUE(stream_backend == sync_backend);
    GDEX_EXPECT_TRUE(static_cast<godot::String>(stream_report.get("actual_backend", "None")) == sync_backend);
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_actual_destination_type(stream_req)), sync_dest);

    if (!pump_stream_to_terminal(ds, stream_req)) {
        const int64_t st = static_cast<int64_t>(stream_req->get_status());
        const godot::String err = stream_req->get_error_message();
        GDEX_FAIL(std::string("stream request did not reach STATUS_COMPLETED (status=") +
                std::to_string(st) + ", error='" + ::gdextest::to_std_string(err) + "').");
    }
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
