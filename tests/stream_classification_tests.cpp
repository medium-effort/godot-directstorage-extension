// tests/stream_classification_tests.cpp
//
// The stream drain decides "retry" from the backend's classification, never from
// the backend's message text. Two directions are asserted here:
//
//   1. A refusal the backend classifies TERMINAL is counted terminal even though
//      its sentence is one a text classifier would have to reject anyway.
//   2. A refusal it classifies TRANSIENT is requeued even when the sentence has
//      been rewritten to match nothing the old classifier looked for.
//
// Windows-only on purpose: it needs the Win32Backend admission budget and the
// DEBUG text seam, and including that header defines the NTSTATUS macros
// (STATUS_PENDING) that the backend-agnostic suites avoid by never including it.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "core/ds_context.h"
#include "core/ds_request.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#endif

#include <string>
#include <vector>

namespace {

constexpr int kProbeBytes = 256;
constexpr uint16_t kCapacity = 128; // documented minimum
constexpr int kStreamRequests = 4;
constexpr uint64_t kTerminalTag = 90001;
constexpr uint64_t kTransientTag = 90002;

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
    req->set_offset(0);
    req->set_size(static_cast<uint32_t>(kProbeBytes));
    req->set_uncompressed_size(static_cast<uint32_t>(kProbeBytes));
    req->set_compression_format(godot::DSRequest::COMPRESSION_NONE);
    req->set_destination_type(godot::DSRequest::DESTINATION_MEMORY);
    req->set_cancellation_tag(p_tag);
    return req;
}

// The admission budget is read from the singleton (Win32Backend asks
// DSContext::get_singleton() for its capacity), so the capacity refusal can only
// be driven through the singleton, not through a private instance.
godot::DSContext *require_singleton_context(::gdextest::TestContext &ctx) {
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton unavailable (module init did not run).");
    }
    if (!ds->is_initialized() && (!ds->initialize_ds() || !ds->is_initialized())) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
    }
    ctx.add_teardown([ds]() {
        if (ds != nullptr && ds->is_initialized()) {
            ds->shutdown_ds();
        }
    });
    return ds;
}

} // namespace

GDEX_TEST_T(stream_classification, refusal_outcome_does_not_read_message_text, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->is_d3d12_backend()) {
        GDEX_SKIP("fallback backend has no admission budget; the capacity refusal needs Win32DirectStorage");
    }

    const char *kPath = "user://gdextest_stream_classification.bin";
    if (!write_probe_file(kPath)) {
        GDEX_FAIL("failed to stage probe file.");
    }
    const uint16_t previous_capacity = ds->get_queue_capacity();
    ctx.add_teardown([ds, previous_capacity, kPath]() {
        ds->set_queue_capacity(previous_capacity);
        godot::Win32Backend::_set_capacity_reject_text_for_testing(godot::String());
        remove_probe_file(kPath);
    });
    ds->set_queue_capacity(kCapacity);

    // (1) TERMINAL half, on an empty slot: a zero-size request can never
    // succeed, so the drain must count it terminal and drop it from the queue.
    {
        godot::Ref<godot::DSRequest> bad = make_memory_request(kPath, kTerminalTag);
        bad->set_size(0);
        GDEX_EXPECT_TRUE(ds->enqueue_request(bad, true));
        GDEX_EXPECT_EQ(static_cast<int64_t>(ds->flush_stream_async()),
                static_cast<int64_t>(godot::OK));
        GDEX_EXPECT_EQ(static_cast<int64_t>(bad->get_status()),
                static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
        GDEX_EXPECT_FALSE(ds->has_stream_pending());
    }

    // (2) Rewrite the capacity sentence so it matches nothing the old text
    // classifier looked for. The refusal it produces is still retryable, which is
    // the whole point of the classification travelling beside the message.
    godot::Win32Backend::_set_capacity_reject_text_for_testing(
            godot::String("DirectStorage: staging budget busy, retry after the batch retires."));

    // Fill one slot to its admission limit (capacity - 2 operations) with
    // pre-staged synchronous requests, so every stream request is refused.
    const int admission_limit = static_cast<int>(kCapacity) - 2;
    std::vector<godot::Ref<godot::DSRequest>> staged;
    staged.reserve(static_cast<size_t>(admission_limit));
    for (int i = 0; i < admission_limit; ++i) {
        godot::Ref<godot::DSRequest> req = make_memory_request(kPath);
        staged.push_back(req);
        if (!ds->enqueue_request(req, false)) {
            GDEX_FAIL(std::string("pre-staging refused at slot index ") + std::to_string(i) + ": " +
                    ::gdextest::to_std_string(req->get_error_message()));
        }
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_pending_request_count()),
            static_cast<int64_t>(admission_limit));

    const godot::Dictionary stats_before =
            godot::Win32Backend::_debug_get_queue_admission_stats_for_testing();

    std::vector<godot::Ref<godot::DSRequest>> streaming;
    streaming.reserve(kStreamRequests);
    for (int i = 0; i < kStreamRequests; ++i) {
        godot::Ref<godot::DSRequest> req = make_memory_request(kPath, kTransientTag);
        streaming.push_back(req);
        GDEX_EXPECT_TRUE(ds->enqueue_request(req, true));
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_stream_pending_count()),
            static_cast<int64_t>(kStreamRequests));

    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->flush_stream_async()),
            static_cast<int64_t>(godot::ERR_BUSY));

    const godot::Dictionary stats_after =
            godot::Win32Backend::_debug_get_queue_admission_stats_for_testing();
    const int64_t rejected = static_cast<int64_t>(stats_after.get("rejected_admissions", 0)) -
            static_cast<int64_t>(stats_before.get("rejected_admissions", 0));
    // One admission attempt per flush: the drain breaks on the first transient
    // refusal and requeues the remainder without offering it to the backend.
    GDEX_EXPECT_EQ(rejected, static_cast<int64_t>(1));
    GDEX_EXPECT_TRUE(ds->has_stream_pending());
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_stream_pending_count()),
            static_cast<int64_t>(kStreamRequests));
    for (int i = 0; i < kStreamRequests; ++i) {
        GDEX_EXPECT_TRUE(streaming[static_cast<size_t>(i)]->get_status() !=
                godot::DSRequest::STATUS_FAILED);
    }

    // Same state on the next frame: still refused, still queued, never dropped.
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->flush_stream_async()),
            static_cast<int64_t>(godot::ERR_BUSY));
    GDEX_EXPECT_TRUE(ds->has_stream_pending());

    // Drop the staged requests now; the teardown only restores settings.
    ds->shutdown_ds();
#else
    (void)ctx;
    GDEX_SKIP("needs the Windows debug build (Win32Backend admission budget and text seam)");
#endif
}
