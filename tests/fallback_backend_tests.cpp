#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "platform/fallback/ds_fallback_backend.h"
#include "compression/gdeflate_compressor.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/ref.hpp>

// Pre-allocation guards in FallbackBackend::process_request.
//
// The soft-decompress path sizes its destination from the request's declared
// uncompressed_size. Nothing bounded that value or checked the payload's
// TileStream header first, so a hostile size allocated before any validation
// and a truncated tile table was only rejected after the allocation. Each case
// asserts on the destination buffer's size because the return status alone
// cannot distinguish "rejected up front" from "allocated, then failed in the
// codec".
//
// Driven through the public entry points: enqueue_request() moves the request to
// SUBMITTED and submit_batch() processes it inline on this thread (batch_in_flight
// is false until submit_async/initialize), so no thread, device or DSContext
// singleton is involved and the suite runs on every platform.

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
