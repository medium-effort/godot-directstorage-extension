#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "core/ds_profiler.h"
#include "platform/fallback/ds_fallback_backend.h"
#include "compression/gdeflate_compressor.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/time.hpp>

// FallbackBackend: pre-allocation guards in process_request(), plus the lifetime
// of the per-worker input handle cache.
//
// The soft-decompress path sizes its destination from the request's declared
// uncompressed_size. Nothing bounded that value or checked the payload's
// TileStream header first, so a hostile size allocated before any validation
// and a truncated tile table was only rejected after the allocation. Each guard
// case asserts on the destination buffer's size because the return status alone
// cannot distinguish "rejected up front" from "allocated, then failed in the
// codec". Those cases are driven through the public entry points: enqueue_request()
// moves the request to SUBMITTED and submit_batch() processes it inline on this
// thread, so no thread, device or DSContext singleton is involved.
//
// fallback_file_cache uses submit_async() and a real worker pool instead. A
// worker that keeps an input handle after its batch drains locks the archive
// against replacement and can serve a stale handle to a later batch; the case
// asserts both that no handle survives a drain and that the archive stays usable.

namespace {

constexpr int64_t kTileSize = 65536; // GDeflate::kDefaultTileSize
constexpr uint32_t kCapPlusOne = 512u * 1024u * 1024u + 1u; // DSTEX_MAX_TOTAL_BYTES + 1

godot::PackedByteArray make_stream_header(uint32_t p_tiles, uint32_t p_last_tile_size) {
    godot::PackedByteArray header;
    header.resize(8);
    header.encode_u8(0, 4); // GDeflate::kGDeflateId
    header.encode_u8(1, 4 ^ 0xff); // magic
    header.encode_u16(2, static_cast<uint16_t>(p_tiles));
    header.encode_u32(4, 1u | (p_last_tile_size << 2)); // tileSizeIdx = 1, lastTileSize
    return header;
}

bool write_bytes(const char *p_path, const godot::PackedByteArray &p_payload) {
    godot::Ref<godot::FileAccess> out =
            godot::FileAccess::open(godot::String(p_path), godot::FileAccess::WRITE);
    if (out.is_null() || !out->is_open()) {
        return false;
    }
    out->store_buffer(p_payload);
    const bool ok = out->get_error() == godot::OK;
    out->close();
    return ok;
}

void remove_file(const char *p_path) {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_valid()) {
        (void)dir->remove(godot::String(p_path));
    }
}

godot::Ref<godot::DSRequest> make_gdeflate_request(const char *p_path, uint32_t p_read_size,
        uint32_t p_uncompressed_size) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    req->set_file_path(godot::String(p_path));
    req->set_offset(0);
    req->set_size(static_cast<int64_t>(p_read_size));
    req->set_uncompressed_size(static_cast<int64_t>(p_uncompressed_size));
    req->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
    return req;
}

godot::Error remove_file_result(const char *p_path) {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_null()) {
        return godot::ERR_CANT_OPEN;
    }
    return dir->remove(godot::String(p_path));
}

godot::Ref<godot::DSRequest> make_raw_request(const char *p_path, int64_t p_offset, int64_t p_size) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    req->set_file_path(godot::String(p_path));
    req->set_offset(static_cast<uint64_t>(p_offset));
    req->set_size(p_size);
    req->set_compression_format(godot::DSRequest::COMPRESSION_NONE);
    return req;
}

bool wait_for_fallback_drain(godot::FallbackBackend &p_backend, int64_t p_timeout_ms) {
    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() +
            static_cast<uint64_t>(p_timeout_ms);
    while (!p_backend.is_batch_complete()) {
        if (godot::Time::get_singleton()->get_ticks_msec() >= deadline) {
            return false;
        }
        godot::OS::get_singleton()->delay_msec(2);
    }
    return true;
}

} // namespace

// Declared size above the 512 MiB container cap. The header itself claims the
// cap (8192 tiles of 65536 bytes), so only the cap predicate can reject it.
GDEX_TEST_T(fallback_gdeflate_guard, hostile_uncompressed_size_rejected_before_resize,
        TAG_UNIT) {
    const char *kPath = "user://gdextest_fallback_cap.bin";
    const godot::PackedByteArray payload = make_stream_header(8192, 0);
    GDEX_ASSERT_TRUE(write_bytes(kPath, payload));
    ctx.add_teardown([kPath]() { remove_file(kPath); });

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req =
            make_gdeflate_request(kPath, static_cast<uint32_t>(payload.size()), kCapPlusOne);
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    backend.submit_batch();

    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    // The closure assert: nothing may be sized from a value that was not bounded
    // first. At HEAD this is 536870913 (the 512 MiB allocation).
    GDEX_EXPECT_EQ(req->get_destination_buffer_ref().size(), static_cast<int64_t>(0));
    GDEX_EXPECT_TRUE(req->get_error_message().find("512 MiB") != -1);
}

// Under the cap, so only the stream predicate can reject it: a header that
// declares one 64 KiB tile but ships no tile table at all.
GDEX_TEST_T(fallback_gdeflate_guard, truncated_tile_table_rejected_before_resize,
        TAG_UNIT) {
    const char *kPath = "user://gdextest_fallback_truncated.bin";
    const godot::PackedByteArray payload = make_stream_header(1, 0);
    GDEX_ASSERT_TRUE(write_bytes(kPath, payload));
    ctx.add_teardown([kPath]() { remove_file(kPath); });

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req = make_gdeflate_request(
            kPath, static_cast<uint32_t>(payload.size()), static_cast<uint32_t>(kTileSize));
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    backend.submit_batch();

    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(req->get_destination_buffer_ref().size(), static_cast<int64_t>(0));
    GDEX_EXPECT_TRUE(req->get_error_message().find("Invalid GDeflate stream") != -1);
}

// Stream and request disagree about the output size (64 KiB header, 128 KiB
// declared). The codec rejects this too, but only after the destination exists.
GDEX_TEST_T(fallback_gdeflate_guard, stream_size_mismatch_rejected_before_resize,
        TAG_UNIT) {
    const char *kPath = "user://gdextest_fallback_mismatch.bin";
    const godot::PackedByteArray payload = make_stream_header(1, 0);
    GDEX_ASSERT_TRUE(write_bytes(kPath, payload));
    ctx.add_teardown([kPath]() { remove_file(kPath); });

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req = make_gdeflate_request(
            kPath, static_cast<uint32_t>(payload.size()), static_cast<uint32_t>(kTileSize * 2));
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    backend.submit_batch();

    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_EQ(req->get_destination_buffer_ref().size(), static_cast<int64_t>(0));
    GDEX_EXPECT_TRUE(req->get_error_message().find("Invalid GDeflate stream") != -1);
}

// Anti-over-tightening: a stream the shipped encoder produces must still
// decompress through the fallback. Guards against a predicate that rejects
// everything and makes the three cases above pass for the wrong reason.
GDEX_TEST_T(fallback_gdeflate_guard, legal_stream_still_decompresses, TAG_UNIT) {
    godot::PackedByteArray original;
    original.resize(kTileSize);
    for (int64_t i = 0; i < original.size(); i++) {
        original[i] = static_cast<uint8_t>((i * 17 + 5) % 256);
    }

    godot::Ref<godot::GDeflateCompressor> compressor;
    compressor.instantiate();
    const godot::PackedByteArray compressed = compressor->compress(original, 6);
    GDEX_ASSERT_TRUE(compressed.size() > 0);

    const char *kPath = "user://gdextest_fallback_legal.bin";
    GDEX_ASSERT_TRUE(write_bytes(kPath, compressed));
    ctx.add_teardown([kPath]() { remove_file(kPath); });

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req = make_gdeflate_request(
            kPath, static_cast<uint32_t>(compressed.size()), static_cast<uint32_t>(kTileSize));
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    backend.submit_batch();

    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    GDEX_EXPECT_EQ(req->get_destination_buffer_ref().size(), kTileSize);
    GDEX_EXPECT_TRUE(req->get_destination_buffer_ref() == original);
}

// A cached input handle must not outlive the batch that opened it. The control
// leg proves the delete probe succeeds with no cache outstanding; the batch leg
// populates the cache, then asserts the count settles to zero, the file is
// replaceable while the backend is alive, and a later batch reads the
// replacement bytes.
GDEX_TEST_T(fallback_file_cache, input_handle_released_when_batch_drains, TAG_INTEGRATION) {
    const char *kPath = "user://gdextest_fallback_cache.bin";
    godot::PackedByteArray first;
    first.resize(4096);
    for (int64_t i = 0; i < first.size(); i++) {
        first[i] = static_cast<uint8_t>((i * 31 + 7) % 251);
    }
    godot::PackedByteArray second = first;
    for (int64_t i = 0; i < second.size(); i++) {
        second[i] = static_cast<uint8_t>(255 - first[i]);
    }

    ctx.add_teardown([kPath]() { remove_file(kPath); });

    // Control: the probe works before any handle is cached.
    GDEX_ASSERT_TRUE(write_bytes(kPath, first));
    GDEX_EXPECT_EQ(static_cast<int64_t>(remove_file_result(kPath)),
            static_cast<int64_t>(godot::OK));
    GDEX_ASSERT_TRUE(write_bytes(kPath, first));

    godot::FallbackBackend backend;
    GDEX_ASSERT_TRUE(backend.initialize());
    GDEX_EXPECT_EQ(static_cast<int64_t>(backend._get_cached_file_handle_count_for_testing()),
            static_cast<int64_t>(0));

    // Exercise the cache: two requests for the same path in one async batch.
    godot::Ref<godot::DSRequest> r1 = make_raw_request(kPath, 0, 4096);
    godot::Ref<godot::DSRequest> r2 = make_raw_request(kPath, 0, 4096);
    GDEX_ASSERT_TRUE(backend.enqueue_request(r1));
    GDEX_ASSERT_TRUE(backend.enqueue_request(r2));
    GDEX_ASSERT_TRUE(backend.submit_async());
    GDEX_ASSERT_TRUE(wait_for_fallback_drain(backend, 5000));
    GDEX_EXPECT_EQ(static_cast<int64_t>(r1->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    GDEX_EXPECT_TRUE(r1->get_destination_buffer_ref() == first);
    GDEX_EXPECT_EQ(static_cast<int64_t>(r2->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    GDEX_EXPECT_TRUE(r2->get_destination_buffer_ref() == first);
    GDEX_EXPECT_TRUE(backend._get_cached_file_handle_inserts_for_testing() > 0);

    // The clear runs outside batch_mutex, so allow a scheduling window after
    // completion before asserting that an idle worker holds nothing.
    const uint64_t settle_deadline = godot::Time::get_singleton()->get_ticks_msec() + 2000;
    while (backend._get_cached_file_handle_count_for_testing() != 0 &&
            godot::Time::get_singleton()->get_ticks_msec() < settle_deadline) {
        godot::OS::get_singleton()->delay_msec(2);
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(backend._get_cached_file_handle_count_for_testing()),
            static_cast<int64_t>(0));

    // The archive must be replaceable while the backend stays alive.
    GDEX_EXPECT_EQ(static_cast<int64_t>(remove_file_result(kPath)),
            static_cast<int64_t>(godot::OK));
    GDEX_ASSERT_TRUE(write_bytes(kPath, second));

    // A later batch must open the path as it is on disk now, not reuse the
    // handle the previous batch held.
    godot::Ref<godot::DSRequest> r3 = make_raw_request(kPath, 0, 4096);
    GDEX_ASSERT_TRUE(backend.enqueue_request(r3));
    GDEX_ASSERT_TRUE(backend.submit_async());
    GDEX_ASSERT_TRUE(wait_for_fallback_drain(backend, 5000));
    GDEX_EXPECT_EQ(static_cast<int64_t>(r3->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    GDEX_EXPECT_TRUE(r3->get_destination_buffer_ref() == second);
    GDEX_EXPECT_TRUE(r3->get_destination_buffer_ref() != first);
}

// FallbackBackend must feed DSProfiler the same completed-batch fields the
// Win32Backend feeds it, so the benchmark's "DS Batch (MiB/s)" column is not
// blank for Mode 3. The batch is async because that is the path the paced
// gameplay streaming benchmark exercises.
GDEX_TEST_T(fallback_profiler, batch_metrics_reported_on_async_completion, TAG_INTEGRATION) {
    const char *kPath = "user://gdextest_fallback_profiler.bin";
    godot::PackedByteArray payload;
    payload.resize(4096);
    for (int64_t i = 0; i < payload.size(); i++) {
        payload[i] = static_cast<uint8_t>((i * 13 + 29) % 251);
    }
    ctx.add_teardown([kPath]() { remove_file(kPath); });
    GDEX_ASSERT_TRUE(write_bytes(kPath, payload));

    godot::DSProfiler *profiler = godot::DSProfiler::get_singleton();
    GDEX_ASSERT_TRUE(profiler != nullptr);
    // get_last_result() builds a fresh Ref on every call, so the history length
    // (not object identity) is the reliable "a new sample was published" signal.
    const int64_t history_before = profiler->get_result_history().size();

    godot::FallbackBackend backend;
    GDEX_ASSERT_TRUE(backend.initialize());
    godot::Ref<godot::DSRequest> r1 = make_raw_request(kPath, 0, 4096);
    godot::Ref<godot::DSRequest> r2 = make_raw_request(kPath, 0, 4096);
    GDEX_ASSERT_TRUE(backend.enqueue_request(r1));
    GDEX_ASSERT_TRUE(backend.enqueue_request(r2));
    GDEX_ASSERT_TRUE(backend.submit_async());
    GDEX_ASSERT_TRUE(wait_for_fallback_drain(backend, 5000));
    GDEX_EXPECT_EQ(static_cast<int64_t>(r1->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
    GDEX_EXPECT_EQ(static_cast<int64_t>(r2->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));

    // Allow the reporting worker to publish its sample after the drain flips.
    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 2000;
    while (profiler->get_result_history().size() == history_before &&
            godot::Time::get_singleton()->get_ticks_msec() < deadline) {
        godot::OS::get_singleton()->delay_msec(2);
    }
    GDEX_ASSERT_TRUE(profiler->get_result_history().size() > history_before);
    const godot::Ref<godot::DSProfilerResult> after = profiler->get_last_result();
    GDEX_ASSERT_TRUE(after.is_valid());
    GDEX_EXPECT_EQ(after->get_compressed_bytes_read(), static_cast<int64_t>(8192));
    GDEX_EXPECT_EQ(after->get_uncompressed_bytes_produced(), static_cast<int64_t>(8192));
    GDEX_EXPECT_EQ(after->get_request_count(), 2);
    GDEX_EXPECT_TRUE(after->get_physical_read_duration_usec() > 0);
    GDEX_EXPECT_TRUE(after->get_start_time_usec() > 0);
    GDEX_EXPECT_TRUE(after->get_end_time_usec() >= after->get_start_time_usec());
    GDEX_EXPECT_EQ(after->get_error_state(), 0);
}

// A staged request is claimed into SUBMITTED and parked in pending_requests.
// Workers only drain request_queue, so shutdown must fail it before clearing
// the staging container; otherwise a holder of the Ref never converges.
GDEX_TEST_T(fallback_shutdown, staged_request_failed_on_shutdown, TAG_UNIT) {
    const char *kPath = "user://gdextest_fallback_shutdown.bin";
    ctx.add_teardown([kPath]() { remove_file(kPath); });
    GDEX_ASSERT_TRUE(write_bytes(kPath, make_stream_header(0, 0)));

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req = make_raw_request(kPath, 0, 8);
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_SUBMITTED));

    backend.shutdown();

    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
            static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    GDEX_EXPECT_TRUE(req->get_error_message().find("shut down") != -1);
    GDEX_EXPECT_TRUE(backend.is_batch_complete());
}

// Control of the same shape: a request that was actually submitted reaches a
// terminal state, and shutdown must not overwrite that result. This catches an
// unguarded fail-everything loop as well as proving the healthy leg exists.
GDEX_TEST_T(fallback_shutdown, submitted_request_result_survives_shutdown, TAG_UNIT) {
    const char *kPath = "user://gdextest_fallback_shutdown_control.bin";
    ctx.add_teardown([kPath]() { remove_file(kPath); });
    GDEX_ASSERT_TRUE(write_bytes(kPath, make_stream_header(0, 0)));

    godot::FallbackBackend backend;
    godot::Ref<godot::DSRequest> req = make_raw_request(kPath, 0, 8);
    GDEX_ASSERT_TRUE(backend.enqueue_request(req));
    backend.submit_batch();

    const int64_t terminal = static_cast<int64_t>(req->get_status());
    GDEX_ASSERT_TRUE(terminal == static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED) ||
            terminal == static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    backend.shutdown();
    GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()), terminal);
}
