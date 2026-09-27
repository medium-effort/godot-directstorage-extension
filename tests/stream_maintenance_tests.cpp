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
}
