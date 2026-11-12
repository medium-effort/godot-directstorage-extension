// Bound row-range API: DSRequest::set_texture_region and its getters.
//
// The backend path (a TEXTURE_REGION request into a sub-box of one subresource)
// is covered by tests/m0_region_chunks_tests.cpp under the windowed D3D12 runner.
// These unit tests pin the script-facing marshalling instead, because the binding
// is what a benchmark or a user streaming path depends on: the default means
// "whole subresource", a set band round trips, and the immutable-request guard is
// the same one every other setter uses.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include <godot_cpp/classes/ref.hpp>

#include "core/ds_request.h"

GDEX_TEST(row_range_binding, defaults_to_whole_subresource) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	GDEX_EXPECT_EQ(request->get_texture_region_top(), static_cast<uint32_t>(0));
	GDEX_EXPECT_EQ(request->get_texture_region_bottom(), static_cast<uint32_t>(0));
}

GDEX_TEST(row_range_binding, set_round_trips) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	request->set_texture_region(256, 512);
	GDEX_EXPECT_EQ(request->get_texture_region_top(), static_cast<uint32_t>(256));
	GDEX_EXPECT_EQ(request->get_texture_region_bottom(), static_cast<uint32_t>(512));
	// A later call replaces the band; it is not append-only.
	request->set_texture_region(0, 128);
	GDEX_EXPECT_EQ(request->get_texture_region_top(), static_cast<uint32_t>(0));
	GDEX_EXPECT_EQ(request->get_texture_region_bottom(), static_cast<uint32_t>(128));
}

// Intentionally triggers one push_error from the immutable-request guard; the
// assertion is that the band did not change, which is what a script would rely on.
GDEX_TEST(row_range_binding, rejected_once_submitted) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	request->set_texture_region(64, 96);
	request->set_status(godot::DSRequest::STATUS_SUBMITTED);
	request->set_texture_region(1, 2);
	GDEX_EXPECT_EQ(request->get_texture_region_top(), static_cast<uint32_t>(64));
	GDEX_EXPECT_EQ(request->get_texture_region_bottom(), static_cast<uint32_t>(96));
	request->set_status(godot::DSRequest::STATUS_PENDING);
	request->set_texture_region(1, 2);
	GDEX_EXPECT_EQ(request->get_texture_region_top(), static_cast<uint32_t>(1));
	GDEX_EXPECT_EQ(request->get_texture_region_bottom(), static_cast<uint32_t>(2));
}

// A band that starts at row 0 is still a band; both bounds at zero is the whole
// subresource. A non-zero pair is classified as a row band whatever its values,
// so the backend can reject an empty or inverted one instead of silently
// downgrading it to a whole-subresource upload.
GDEX_TEST(row_range_binding, non_zero_pair_is_a_row_region) {
	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	GDEX_EXPECT_FALSE(request->has_texture_row_region());
	request->set_texture_region(0, 32);
	GDEX_EXPECT_TRUE(request->has_texture_row_region());
	request->set_texture_region(64, 96);
	GDEX_EXPECT_TRUE(request->has_texture_row_region());
	request->set_texture_region(64, 64);
	GDEX_EXPECT_TRUE(request->has_texture_row_region());
	request->set_texture_region(64, 32);
	GDEX_EXPECT_TRUE(request->has_texture_row_region());
}

