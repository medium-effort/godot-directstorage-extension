// Finding 1.2: GDeflate header validation must stay fail-closed while moving
// off the frame thread. The shared header rule is exercised directly, and the
// per-request validation record must only be honoured for the exact triple
// (path, offset, declared size) that was validated off-thread.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_gdeflate_stream.h"
#include "core/ds_request.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstring>

namespace {

void make_valid_header(uint64_t p_uncompressed_size, uint8_t *r_bytes) {
	GDeflate::TileStream stream(static_cast<size_t>(p_uncompressed_size));
	std::memset(r_bytes, 0, sizeof(GDeflate::TileStream));
	std::memcpy(r_bytes, &stream, sizeof(GDeflate::TileStream));
}

} // namespace

GDEX_TEST(gdeflate_header, valid_header_accepted) {
	uint8_t bytes[8] = {};
	make_valid_header(64 * 1024, bytes);
	godot::String error;
	GDEX_EXPECT_TRUE(dsgd_gdeflate::validate_stream_header(
			bytes, sizeof(bytes), 4096, 64 * 1024, "Test: ", "res://x.dstex", error));
	GDEX_EXPECT_TRUE(error.is_empty());
}

GDEX_TEST(gdeflate_header, mismatched_declared_size_rejected) {
	uint8_t bytes[8] = {};
	make_valid_header(64 * 1024, bytes);
	godot::String error;
	GDEX_EXPECT_FALSE(dsgd_gdeflate::validate_stream_header(
			bytes, sizeof(bytes), 4096, 128 * 1024, "Test: ", "res://x.dstex", error));
	GDEX_EXPECT_TRUE(error.contains("stream declares"));
}

GDEX_TEST(gdeflate_header, corrupt_header_rejected) {
	uint8_t bytes[8] = {};
	make_valid_header(64 * 1024, bytes);
	bytes[0] ^= 0x01; // breaks id == (magic ^ 0xff)
	godot::String error;
	GDEX_EXPECT_FALSE(dsgd_gdeflate::validate_stream_header(
			bytes, sizeof(bytes), 4096, 64 * 1024, "Test: ", "res://x.dstex", error));
	GDEX_EXPECT_TRUE(error.contains("Invalid GDeflate tile-stream header"));
}

GDEX_TEST(gdeflate_header, short_header_rejected) {
	uint8_t bytes[8] = {};
	make_valid_header(64 * 1024, bytes);
	godot::String error;
	GDEX_EXPECT_FALSE(dsgd_gdeflate::validate_stream_header(
			bytes, 4, 4096, 64 * 1024, "Test: ", "res://x.dstex", error));
	GDEX_EXPECT_TRUE(error.contains("shorter than a tile-stream header"));
}

GDEX_TEST(gdeflate_header, request_record_honours_exact_triple) {
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path("res://textures.dspkg");
	req->set_offset(8192);
	req->set_uncompressed_size(64 * 1024);
	req->set_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 64 * 1024);

	GDEX_EXPECT_TRUE(req->is_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 64 * 1024));
	// A different path, offset or declared size must fall back to the backend's
	// own synchronous validation instead of trusting a stale record.
	GDEX_EXPECT_FALSE(req->is_gdeflate_stream_prevalidated("res://other.dspkg", 8192, 64 * 1024));
	GDEX_EXPECT_FALSE(req->is_gdeflate_stream_prevalidated("res://textures.dspkg", 4096, 64 * 1024));
	GDEX_EXPECT_FALSE(req->is_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 32 * 1024));
}

GDEX_TEST(gdeflate_header, unstamped_request_falls_back) {
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path("res://textures.dspkg");
	req->set_offset(8192);
	req->set_uncompressed_size(64 * 1024);
	GDEX_EXPECT_FALSE(req->is_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 64 * 1024));
}

// Whole-texture GPU prewarm requests cover all mips in one DSRequest, so a
// single (offset, size) record cannot match each mip the backend validates.
// The range record must match the exact (path, offset, first, count) identity.
GDEX_TEST(gdeflate_header, whole_texture_record_honours_exact_range) {
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path("res://textures.dspkg");
	req->set_offset(8192);
	req->set_first_subresource(0);
	req->set_subresource_count(11);
	req->set_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 0, 11);

	GDEX_EXPECT_TRUE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 0, 11));
	// Any mismatch must fall back to the backend's synchronous per-mip check.
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://other.dspkg", 8192, 0, 11));
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 4096, 0, 11));
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 1, 11));
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 0, 10));
}

// A single-stream stamp (used for chunked/single-mip requests) must NOT be
// mistaken for a whole-range stamp: a multi-mip request still has every mip
// validated by the backend unless the full range was prevalidated off-thread.
GDEX_TEST(gdeflate_header, single_stream_stamp_does_not_imply_whole_range) {
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path("res://textures.dspkg");
	req->set_offset(8192);
	req->set_uncompressed_size(64 * 1024);
	req->set_first_subresource(0);
	req->set_subresource_count(11);
	req->set_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 64 * 1024);

	GDEX_EXPECT_TRUE(req->is_gdeflate_stream_prevalidated("res://textures.dspkg", 8192, 64 * 1024));
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 0, 11));
}

GDEX_TEST(gdeflate_header, unstamped_whole_range_falls_back) {
	godot::Ref<godot::DSRequest> req;
	req.instantiate();
	req->set_file_path("res://textures.dspkg");
	req->set_offset(8192);
	req->set_first_subresource(0);
	req->set_subresource_count(11);
	GDEX_EXPECT_FALSE(req->is_gdeflate_streams_prevalidated("res://textures.dspkg", 8192, 0, 11));
}
