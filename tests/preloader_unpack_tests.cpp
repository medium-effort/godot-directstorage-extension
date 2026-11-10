#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "core/ds_request.h"
#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/memory.hpp>

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>

namespace godot {

struct PreloaderUnpackTest {
	static Ref<Texture2D> build_texture(const DSPreloader::Payload &p_meta, const std::vector<PackedByteArray> &p_buffers) {
		return DSPreloader::build_texture(p_meta, p_buffers);
	}

	// Header/payload validation seams. read_header_uncached() and validate_payload()
	// are private (ds_preloader.h:295, :316); this friend already exists for
	// build_texture() and entry injection, so the validation tests need no new seam.
	static bool validate(const DSPreloader::Payload &p_payload) {
		return DSPreloader::validate_payload(p_payload);
	}

	static bool read_header(const String &p_dstex_path, DSPreloader::PayloadHeader &r_out) {
		return DSPreloader::read_header_uncached(p_dstex_path, r_out);
	}

	static void payload_from_header(const String &p_engine_path, const String &p_dstex_path,
			const DSPreloader::PayloadHeader &p_header, DSPreloader::Payload &r_out) {
		DSPreloader::payload_from_header(p_engine_path, p_dstex_path, p_header, r_out);
	}

	static bool resolve(const String &p_engine_path, DSPreloader::Payload &r_out) {
		return DSPreloader::resolve_payload(p_engine_path, r_out);
	}

	// Memory-path request expansion. expand_requests() is private (ds_preloader.h);
	// the friend already exists, so the NONE-codec plumbing needs no new seam.
	static bool expand(const DSPreloader::Payload &p_payload, uint64_t p_tag, Vector<Ref<DSRequest>> &r_out) {
		return DSPreloader::expand_requests(p_payload, p_tag, r_out);
	}

	// Injects one already-terminal CPU prewarm entry straight into the pending
	// table. This is the only way to drive poll_completed() with a non-empty
	// table without a backend, disk I/O or a real stream: the friend access
	// exists for tests (ds_preloader.h:208).
	static int64_t inject_terminal_cpu_entry(uint64_t p_tag, const DSPreloader::Payload &p_meta, const PackedByteArray &p_payload) {
		Ref<DSRequest> req;
		req.instantiate();
		req->set_status(DSRequest::STATUS_COMPLETED);
		req->get_destination_buffer_ref() = p_payload;

		DSPreloader::Tracked tracked;
		tracked.tag = p_tag;
		tracked.meta = p_meta;
		tracked.is_gpu = false;
		tracked.requests.push_back(req);
		return publish(tracked);
	}

	// Injects a GPU entry that is already marked failed: poll_completed() drops
	// it through complete_gpu_entry() without storing anything, but still
	// charges estimate_payload_bytes(meta) to the slice byte budget. Used to
	// prove the byte budget is driven by consumed entries, not stored ones.
	static int64_t inject_gpu_failed_entry(uint64_t p_tag, const DSPreloader::Payload &p_meta) {
		DSPreloader::Tracked tracked;
		tracked.tag = p_tag;
		tracked.meta = p_meta;
		tracked.is_gpu = true;
		tracked.gpu_failed = true;
		return publish(tracked);
	}

private:
	static int64_t publish(DSPreloader::Tracked &p_tracked) {
		std::lock_guard<std::mutex> lock(DSPreloader::state().mutex);
		DSPreloader::State &s = DSPreloader::state();
		int64_t id = s.next_id++;
		if (s.next_id == 0) {
			s.next_id = 1;
		}
		s.pending[id] = p_tracked;
		return id;
	}
};

} // namespace godot

GDEX_TEST(preloader_unpack, oversized_payload_clamped_no_overflow) {
	// Setup v2 metadata for 256x4 RGBA8
	// 256 * 4 = 1024 bytes per row; row_pitch = 1024 (256-byte aligned).
	// source_size = 4096 bytes; footprint_size = 4096 bytes.
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_oversized.png";
	meta.width = 256;
	meta.height = 4;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;

	// Craft an oversized buffer: 8192 bytes (2x the capacity of natural_data)
	godot::PackedByteArray oversized_buf;
	oversized_buf.resize(8192);
	uint8_t *w = oversized_buf.ptrw();
	memset(w, 0xAA, 8192);

	std::vector<godot::PackedByteArray> buffers;
	buffers.push_back(oversized_buf);

	// Buffer must be clamped to layout.source_size (4096 bytes) and verified within
	// total_natural_size, preventing destination heap overflow.
	godot::Ref<godot::Texture2D> tex = godot::PreloaderUnpackTest::build_texture(meta, buffers);

	GDEX_EXPECT_TRUE(tex.is_valid());
	godot::Ref<godot::ImageTexture> img_tex = tex;
	GDEX_EXPECT_TRUE(img_tex.is_valid());
	godot::Ref<godot::Image> img = img_tex->get_image();
	GDEX_EXPECT_TRUE(img.is_valid());
	GDEX_EXPECT_EQ(img->get_width(), 256);
	GDEX_EXPECT_EQ(img->get_height(), 4);
}

GDEX_TEST(preloader_unpack, v2_padded_footprint_unpads_correctly) {
	// Setup v2 metadata for 100x4 RGBA8
	// source_row_bytes = 400.
	// row_pitch = 512 (256-byte aligned D3D12 footprint).
	// footprint_size = 512 * 4 = 2048 bytes.
	// source_size = 400 * 4 = 1600 bytes.
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_padded.png";
	meta.width = 100;
	meta.height = 4;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;

	// Populate padded footprint: write known pattern in source bytes, filler in padding
	godot::PackedByteArray footprint_buf;
	footprint_buf.resize(2048);
	uint8_t *w = footprint_buf.ptrw();
	memset(w, 0, 2048);
	for (int r = 0; r < 4; ++r) {
		memset(w + r * 512, 0x7F, 400); // valid pixel data
		memset(w + r * 512 + 400, 0xFF, 112); // padding
	}

	std::vector<godot::PackedByteArray> buffers;
	buffers.push_back(footprint_buf);

	godot::Ref<godot::Texture2D> tex = godot::PreloaderUnpackTest::build_texture(meta, buffers);

	GDEX_EXPECT_TRUE(tex.is_valid());
	godot::Ref<godot::ImageTexture> img_tex = tex;
	GDEX_EXPECT_TRUE(img_tex.is_valid());
	godot::Ref<godot::Image> img = img_tex->get_image();
	GDEX_EXPECT_TRUE(img.is_valid());
	GDEX_EXPECT_EQ(img->get_width(), 100);
	GDEX_EXPECT_EQ(img->get_height(), 4);
	GDEX_EXPECT_EQ(img->get_data().size(), 1600);
}

GDEX_TEST(preloader_unpack, truncated_payload_safe_under_copy) {
	// Setup v2 metadata for 256x4 RGBA8 (source_size = 4096)
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_truncated.png";
	meta.width = 256;
	meta.height = 4;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;

	// Supply truncated payload (2000 bytes instead of 4096)
	godot::PackedByteArray truncated_buf;
	truncated_buf.resize(2000);
	memset(truncated_buf.ptrw(), 0x33, 2000);

	std::vector<godot::PackedByteArray> buffers;
	buffers.push_back(truncated_buf);

	// Must not crash or buffer-overflow; under-copy safely fills up to 2000 bytes
	godot::Ref<godot::Texture2D> tex = godot::PreloaderUnpackTest::build_texture(meta, buffers);
	GDEX_EXPECT_TRUE(tex.is_valid());
}

GDEX_TEST(preloader_gpu, eligibility_matrix) {
	// Base: v2 BC7 color, complete tables -> eligible.
	godot::DSPreloader::Payload eligible;
	eligible.engine_path = "res://test_gpu_eligible.png";
	eligible.width = 64;
	eligible.height = 64;
	eligible.mip_count = 2;
	eligible.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_BPTC_RGBA);
	eligible.is_v2 = true;
	eligible.mip_offsets.resize(2);
	eligible.mip_sizes.resize(2);
	eligible.mip_uncompressed_sizes.resize(2);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(eligible));

	// as_image_texture exclusion (projector/decal sources stay CPU).
	godot::DSPreloader::Payload image_tex = eligible;
	image_tex.as_image_texture = true;
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(image_tex));

	// v1 single-stream has no footprint metadata.
	godot::DSPreloader::Payload v1 = eligible;
	v1.is_v2 = false;
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(v1));

	// Float data format (maps to self, no sRGB counterpart path): memory path.
	godot::DSPreloader::Payload data = eligible;
	data.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RF);
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(data));

	// Uncompressed single-channel data (R8 maps to self, no sRGB view): memory
	// path, both because it is cheap to move and because it is plausible 2D/UI
	// data with unverified DRD sampling.
	godot::DSPreloader::Payload r8 = eligible;
	r8.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_R8);
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(r8));

	// Linear-only block formats carry 3D material data (normal / roughness /
	// AO / height) and declare no shareable list - legal per Q44. They are
	// eligible despite mapping to self, because the CPU route is the
	// expensive one for these payloads.
	godot::DSPreloader::Payload bc4 = eligible;
	bc4.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGTC_R);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(bc4));

	godot::DSPreloader::Payload bc5 = eligible;
	bc5.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGTC_RG);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(bc5));

	// Linear color block data qualifies through its sRGB counterpart path.
	godot::DSPreloader::Payload bc3 = eligible;
	bc3.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_DXT5);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(bc3));

	// Already-sRGB base maps to self -> eligible.
	godot::DSPreloader::Payload srgb = eligible;
	srgb.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8, true);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(srgb));

	// Incomplete mip tables.
	godot::DSPreloader::Payload short_tables = eligible;
	short_tables.mip_sizes.clear();
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(short_tables));
}

GDEX_TEST(preloader_gpu, eligibility_accepts_npot) {
	// Contract lock-in: a non-multiple-of-4 payload is deliberately GPU-eligible.
	// The wrap is a Texture2DRD at the padded storage size (GODOT_QUIRKS.md Q79
	// documents the missing size setter); compatibility/as_image_texture is the
	// documented escape when get_width() must report the authoring size. Keeping
	// the texture on the GPU path is the shipped behaviour and is not changed by
	// the documentation fix for this finding.
	godot::DSPreloader::Payload eligible;
	eligible.engine_path = "res://test_gpu_npot.png";
	eligible.width = 100;
	eligible.height = 52;
	eligible.mip_count = 2;
	eligible.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_BPTC_RGBA);
	eligible.is_v2 = true;
	eligible.mip_offsets.resize(2);
	eligible.mip_sizes.resize(2);
	eligible.mip_uncompressed_sizes.resize(2);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(eligible));

	// 100x50 logical over 100x52 storage stays eligible; the GPU wrap will
	// report 100x52, which the prewarm warning and the docs state explicitly.
	godot::DSPreloader::Payload npot = eligible;
	npot.logical_width = 100;
	npot.logical_height = 50;
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(npot));

	// A non-zero pair equal to storage is equivalent to the producers' 0
	// encoding and must not change eligibility either.
	godot::DSPreloader::Payload equal = eligible;
	equal.logical_width = 100;
	equal.logical_height = 52;
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(equal));
}

GDEX_TEST(preloader_gpu, npot_header_logical_semantics) {
	// Companion lock-in: v2 cannot express a logical size, so the loader's GPU
	// branch can only meet needs_size_override through a v3 payload. This pins
	// the header contract the documented CPU/GPU size difference relies on.
	const char *v2_path = "user://gdextest_npot_semantics_v2.dstex";
	const char *v3_path = "user://gdextest_npot_semantics_v3.dstex";
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	GDEX_ASSERT_TRUE(dstex.is_valid());

	godot::PackedByteArray raw;
	raw.resize(100 * 52 * 4);
	memset(raw.ptrw(), 0x5A, static_cast<size_t>(raw.size()));

	GDEX_ASSERT_EQ((int64_t)dstex->write_to_file(godot::String(v2_path), raw, 100, 52,
						   godot::Image::FORMAT_RGBA8, 1, 1),
			(int64_t)godot::OK);
	GDEX_ASSERT_EQ((int64_t)dstex->write_to_file(godot::String(v3_path), raw, 100, 52,
						   godot::Image::FORMAT_RGBA8, 1, 1, false, false, 100, 50),
			(int64_t)godot::OK);

	godot::Dictionary v2 = dstex->read_header(godot::String(v2_path));
	godot::Dictionary v3 = dstex->read_header(godot::String(v3_path));
	GDEX_ASSERT_FALSE(v2.is_empty());
	GDEX_ASSERT_FALSE(v3.is_empty());
	GDEX_EXPECT_EQ((int64_t)v2["version"], (int64_t)2);
	GDEX_EXPECT_EQ((int64_t)v2["logical_width"], (int64_t)100);
	GDEX_EXPECT_EQ((int64_t)v2["logical_height"], (int64_t)52);
	GDEX_EXPECT_EQ((int64_t)v3["version"], (int64_t)3);
	GDEX_EXPECT_EQ((int64_t)v3["logical_width"], (int64_t)100);
	GDEX_EXPECT_EQ((int64_t)v3["logical_height"], (int64_t)50);

	godot::DirAccess::remove_absolute(godot::String(v2_path));
	godot::DirAccess::remove_absolute(godot::String(v3_path));
}

GDEX_TEST(preloader_gpu, pump_idle_is_noop) {
	// No chunks queued anywhere: must not crash, post nothing, free nothing.
	godot::DSPreloader::pump_gpu_prewarm();
	GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(0), 0);
}

GDEX_TEST(preloader_unpack, partial_mip_chain_falls_back_to_base_no_mipmaps) {
	// #0039: 64x64 needs a full chain of 7 for use_mipmaps=true.
	// Supply only 2 mips -> base slice without mipmaps + warning, not failure.
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_partial_chain.png";
	meta.width = 64;
	meta.height = 64;
	meta.mip_count = 2;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;

	// mip0 64x64 RGBA8: row 256, pitch 256, footprint 16384.
	godot::PackedByteArray mip0;
	mip0.resize(16384);
	memset(mip0.ptrw(), 0x5A, 16384);
	// mip1 32x32 RGBA8: row 128, pitch 256, footprint 8192.
	godot::PackedByteArray mip1;
	mip1.resize(8192);
	uint8_t *w1 = mip1.ptrw();
	memset(w1, 0, 8192);
	for (int r = 0; r < 32; ++r) {
		memset(w1 + r * 256, 0xA5, 128);
	}

	std::vector<godot::PackedByteArray> buffers;
	buffers.push_back(mip0);
	buffers.push_back(mip1);

	godot::Ref<godot::Texture2D> tex = godot::PreloaderUnpackTest::build_texture(meta, buffers);
	GDEX_EXPECT_TRUE(tex.is_valid());
	godot::Ref<godot::ImageTexture> img_tex = tex;
	GDEX_EXPECT_TRUE(img_tex.is_valid());
	godot::Ref<godot::Image> img = img_tex->get_image();
	GDEX_EXPECT_TRUE(img.is_valid());
	GDEX_EXPECT_EQ(img->get_width(), 64);
	GDEX_EXPECT_EQ(img->get_height(), 64);
	GDEX_EXPECT_FALSE(img->has_mipmaps());
	GDEX_EXPECT_EQ(img->get_data().size(), 16384);
}

// --- poll_completed budgeting and refusal accounting (#0137) ----------------
//
// What these tests can and cannot discriminate:
// - refused_stores_consume_budget_but_do_not_count: cache DISABLED, so every
//   store() is refused. This is the case where `stored` and `processed` differ;
//   it fails if the CPU branch counts a refused store as stored (pre-#0136
//   behaviour returned 1 here). The item/byte budget itself is pinned by the
//   max_items=0 test below: with max_items=1 the terminal-id scan cap already
//   limits the collection, so a budget-only regression cannot show up here.
// - unstored_entry_still_charges_the_byte_budget: max_items=0, so only the
//   64 MiB slice byte budget can bound the call. A GPU-failed entry that stores
//   nothing but estimates 64 MiB must still stop the slice; budgeting the gate
//   on `stored` (pre-#0137) processes the next entry instead.
// - poll_slice_honors_max_items is the happy path (cache enabled): it pins the
//   max_items behaviour and the residency contract, but on its own it CANNOT
//   tell `stored` from `processed` because both advance together.
// All three need no backend, disk I/O or real stream, so they run in headless CI.

namespace {

godot::DSPreloader::Payload make_budget_payload(const char *p_path) {
	godot::DSPreloader::Payload meta;
	meta.engine_path = godot::String(p_path);
	meta.payload_key = meta.engine_path;
	meta.width = 256;
	meta.height = 4;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;
	return meta;
}

godot::PackedByteArray make_budget_data(uint8_t p_seed) {
	// 256x4 RGBA8: row bytes 1024 (already 256-aligned) -> footprint 4096, so
	// build_texture()'s unpad loop is a 1:1 copy.
	godot::PackedByteArray data;
	data.resize(4096);
	uint8_t *w = data.ptrw();
	for (int i = 0; i < 4096; ++i) {
		w[i] = static_cast<uint8_t>((i * 7 + p_seed) & 0xFF);
	}
	return data;
}

godot::DSTextureCache *make_budget_cache(::gdextest::TestContext &ctx, bool p_enabled) {
	// Constructed per test on purpose: the ctor installs the static singleton and
	// the dtor nulls it, and both prewarm entry points consult the singleton.
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	cache->set_enabled(p_enabled);
	cache->clear();
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});
	return cache;
}

} // namespace

GDEX_TEST_T(preloader_budget, poll_slice_honors_max_items, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx, true);
	const uint64_t tag = 0x7FFF0137ULL;

	godot::PreloaderUnpackTest::inject_terminal_cpu_entry(tag, make_budget_payload("user://gdextest_budget_a.png"), make_budget_data(11));
	godot::PreloaderUnpackTest::inject_terminal_cpu_entry(tag, make_budget_payload("user://gdextest_budget_b.png"), make_budget_data(29));
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 2);

	// max_items=1: exactly one entry per call, second call finishes the table.
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 1, 0), 1);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 1);
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 1, 0), 1);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);

	// Cache enabled: both entries were really built and stored.
	GDEX_EXPECT_EQ(cache->get_cached_count(), 2);
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_budget_a.png").is_valid());
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_budget_b.png").is_valid());
}

// The discriminating case for the store count: with the cache DISABLED every
// store is refused, so a refusal must consume an entry without being counted.
GDEX_TEST_T(preloader_budget, refused_stores_consume_budget_but_do_not_count, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx, false);
	const uint64_t tag = 0x7FFF0138ULL;

	godot::PreloaderUnpackTest::inject_terminal_cpu_entry(tag, make_budget_payload("user://gdextest_budget_c.png"), make_budget_data(3));
	godot::PreloaderUnpackTest::inject_terminal_cpu_entry(tag, make_budget_payload("user://gdextest_budget_d.png"), make_budget_data(5));
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 2);

	// Consumed one, stored none. Pre-#0136 code returned 1 here (it counted the
	// refused store), which is exactly the regression this pins.
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 1, 0), 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 1);
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 1, 0), 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
	GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
}

// max_items=0 leaves only the byte budget to bound the slice. The GPU-failed
// entry stores nothing but still charges its estimated size, so a `stored`-gated
// budget would consume the following entry in the same call.
GDEX_TEST_T(preloader_budget, unstored_entry_still_charges_the_byte_budget, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx, false);
	const uint64_t tag = 0x7FFF0139ULL;

	godot::DSPreloader::Payload expensive = make_budget_payload("user://gdextest_budget_e.png");
	// estimate_payload_bytes() sums mip_uncompressed_sizes for v2 payloads; one
	// entry of exactly the 64 MiB slice cap is enough and allocates nothing.
	expensive.mip_uncompressed_sizes.push_back(64 * 1024 * 1024);
	godot::PreloaderUnpackTest::inject_gpu_failed_entry(tag, expensive);
	godot::PreloaderUnpackTest::inject_terminal_cpu_entry(tag, make_budget_payload("user://gdextest_budget_f.png"), make_budget_data(7));
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 2);

	// Nothing is stored (cache disabled), so only `processed` can stop the slice.
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 0, 0), 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 1);
	GDEX_EXPECT_EQ(cache->poll_completed(tag, 0, 0), 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
}

GDEX_TEST_T(preloader_budget, disabled_cache_refuses_both_entry_points, TAG_UNIT) {
	// Skip before asserting: an EXPECT here would record a failure on the very
	// path that means "environment not set up", turning a skip into a red suite.
	if (godot::DSContext::get_singleton() == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable (module init did not run)");
		return;
	}

	godot::DSTextureCache *cache = make_budget_cache(ctx, false);
	const int before_unbuilt = cache->get_unbuilt_prewarm_count(0);
	const int before_pending = cache->get_pending_prewarm_count(0);

	godot::PackedStringArray paths;
	// Deliberately nonexistent: the refusal must fire before payload resolution,
	// so attempting to resolve/read this path would be a defect.
	paths.append(godot::String("user://gdextest_prewarm_refused.dstex"));

	GDEX_EXPECT_EQ(cache->prewarm_async(paths), static_cast<uint64_t>(0));
	GDEX_EXPECT_EQ(cache->prewarm_gpu_async(paths, 16, true), static_cast<uint64_t>(0));

	// Nothing queued, nothing pending, nothing resident: no work was started.
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(0), before_unbuilt);
	GDEX_EXPECT_EQ(cache->get_pending_prewarm_count(0), before_pending);
	GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
}

// --- Header/payload validation parity --------------------------------------
//
// Gate A parity: prewarm must not admit a .dstex that the format layer, the
// archive reader or the Win32 enqueue path already refuse. DSTex::read_header()
// is the oracle used below: it is the exact predicate ResourceLoader.load()
// runs for a direct .dstex file, and it is cheap to call from a test.
//
// Fixtures are WRITER-PRODUCED then patched. DSTex::write_to_file() emits a
// valid v2 file, and each case rewrites exactly one field in a byte copy, so
// magic/format/mip-table/alignment stay valid and a rejection is attributable
// to the patched field instead of to a malformed fixture. Every rejection case
// also asserts its unpatched base is still accepted, so an implementation that
// simply rejects everything cannot pass.

namespace {

const char *const kValidBase1 = "user://gdex_validation_valid_1mip.dstex";
const char *const kValidBase7 = "user://gdex_validation_valid_7mip.dstex";
const char *const kPatchDim = "user://gdex_validation_patch_dim.dstex";
const char *const kPatchFootprint = "user://gdex_validation_patch_footprint.dstex";
const char *const kPatchMipSum = "user://gdex_validation_patch_mipsum.dstex";
const char *const kPatchChunkCap = "user://gdex_validation_patch_chunkcap.dstex";
const char *const kPatchV3Footprint = "user://gdex_validation_patch_v3footprint.dstex";
const char *const kPatchV3Offset = "user://gdex_validation_patch_v3offset.dstex";

// 64x64 RGBA8: base row is 256 B (already pitch-aligned), base mip 16 KiB.
constexpr int kBaseWidth = 64;
constexpr int kBaseHeight = 64;
constexpr int kBaseMips = 7;
// Full-chain footprint total: 16384+8192+4096+2048+1024+512+256.
constexpr uint64_t kBase7Footprint = 32512ULL;
// The footprint clause is width*height*32 (131072 here) plus the row-padding
// allowance the writer needs (512 * height = 32768), so 163840 is the limit;
// 400000 exceeds it while staying far below both the 512 MiB total-bytes cap
// and UINT32_MAX.
constexpr uint64_t kOverFootprint = 400000ULL;
// 7 x 132 MiB = 924 MiB: above the 512 MiB aggregate cap, and small enough that
// a pre-fix build allocates it without endangering the runner.
constexpr uint32_t kMipUncompressed = 132u * 1024u * 1024u;

godot::PackedByteArray make_chain_data(int p_mips) {
	godot::PackedByteArray data;
	for (int i = 0; i < p_mips; ++i) {
		const int dim = std::max(1, kBaseWidth >> i);
		godot::Ref<godot::Image> mip = godot::Image::create(dim, dim, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.2f, 0.4f, 0.6f, 1.0f));
		data.append_array(mip->get_data());
	}
	return data;
}

bool write_valid_base(const char *p_path, int p_mips) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), make_chain_data(p_mips), kBaseWidth,
			kBaseHeight, godot::Image::FORMAT_RGBA8, p_mips, 9) == godot::OK;
}

godot::PackedByteArray read_bytes(const char *p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return godot::PackedByteArray();
	}
	godot::PackedByteArray bytes = file->get_buffer(static_cast<int64_t>(file->get_length()));
	file->close();
	return bytes;
}

bool write_bytes(const char *p_path, const godot::PackedByteArray &p_bytes) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->store_buffer(p_bytes);
	// store_buffer() returns void in godot-cpp; the error is only observable here.
	const bool ok = file->get_error() == godot::OK;
	file->close();
	return ok;
}

void put_u32(godot::PackedByteArray &r_bytes, int64_t p_offset, uint32_t p_value) {
	memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

void put_u64(godot::PackedByteArray &r_bytes, int64_t p_offset, uint64_t p_value) {
	memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
}

uint32_t get_u32(const godot::PackedByteArray &p_bytes, int64_t p_offset) {
	uint32_t value = 0;
	memcpy(&value, p_bytes.ptr() + p_offset, sizeof(value));
	return value;
}

// Registers deletion for every fixture path. Register this BEFORE constructing
// the cache in integration cases: teardowns run LIFO, and the texture cache must
// be torn down before its request paths are removed from disk.
void track_cleanup(::gdextest::TestContext &ctx, std::initializer_list<const char *> p_paths) {
	for (const char *p : p_paths) {
		const godot::String path(p);
		ctx.add_teardown([path]() {
			godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
			if (ps != nullptr) {
				godot::DirAccess::remove_absolute(ps->globalize_path(path));
			}
		});
	}
}

godot::Dictionary loader_header(const char *p_path) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->read_header(godot::String(p_path));
}

} // namespace

GDEX_TEST_T(preloader_validation, header_dimension_bound_rejected, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	track_cleanup(ctx, { kValidBase1, kPatchDim });

	godot::PackedByteArray bytes = read_bytes(kValidBase1);
	GDEX_ASSERT_FALSE(bytes.is_empty());
	put_u32(bytes, 8, 65535u); // width; leaves height/original_size untouched
	GDEX_ASSERT_TRUE(write_bytes(kPatchDim, bytes));

	// Fixture isolation: the unpatched base parses.
	godot::DSPreloader::PayloadHeader base_header;
	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(kValidBase1), base_header));
	GDEX_EXPECT_EQ(base_header.width, static_cast<uint32_t>(kBaseWidth));

	// The loader's own predicate rejects the same bytes.
	GDEX_EXPECT_TRUE(loader_header(kPatchDim).is_empty());

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchDim), patched_header));
}

GDEX_TEST_T(preloader_validation, header_footprint_bound_rejected, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	track_cleanup(ctx, { kValidBase1, kPatchFootprint });

	godot::PackedByteArray bytes = read_bytes(kValidBase1);
	GDEX_ASSERT_FALSE(bytes.is_empty());
	put_u64(bytes, 24, kOverFootprint); // original_size
	GDEX_ASSERT_TRUE(write_bytes(kPatchFootprint, bytes));

	godot::DSPreloader::PayloadHeader base_header;
	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(kValidBase1), base_header));
	GDEX_EXPECT_EQ(base_header.original_size, 16384ULL);

	GDEX_EXPECT_TRUE(loader_header(kPatchFootprint).is_empty());

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchFootprint), patched_header));
}

GDEX_TEST_T(preloader_validation, v2_mip_sums_must_match_header, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase7, kBaseMips));
	track_cleanup(ctx, { kValidBase7, kPatchMipSum });

	godot::PackedByteArray bytes = read_bytes(kValidBase7);
	GDEX_ASSERT_FALSE(bytes.is_empty());
	const uint32_t mip_count = get_u32(bytes, 20);
	GDEX_ASSERT_EQ(mip_count, static_cast<uint32_t>(kBaseMips));
	for (uint32_t i = 0; i < mip_count; ++i) {
		put_u32(bytes, static_cast<int64_t>(48 + i * 24 + 12), kMipUncompressed);
	}
	GDEX_ASSERT_TRUE(write_bytes(kPatchMipSum, bytes));

	// Header totals are untouched and sane; only the mip table contradicts them.
	godot::DSPreloader::PayloadHeader base_header;
	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(kValidBase7), base_header));
	GDEX_EXPECT_EQ(base_header.original_size, kBase7Footprint);

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchMipSum), patched_header));

	// The same construction through the payload gate: a header that is sane on
	// its own but whose mip footprints sum to 7 x 132 MiB = 924 MiB.
	godot::DSPreloader::Payload payload;
	payload.engine_path = godot::String("user://gdex_validation_synthetic.png");
	payload.payload_key = payload.engine_path;
	payload.width = static_cast<uint32_t>(kBaseWidth);
	payload.height = static_cast<uint32_t>(kBaseHeight);
	payload.mip_count = static_cast<uint32_t>(kBaseMips);
	payload.pixel_format = static_cast<uint32_t>(godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8));
	payload.original_size = kBase7Footprint;
	payload.compressed_size = base_header.compressed_size;
	payload.is_v2 = true;
	for (int i = 0; i < kBaseMips; ++i) {
		payload.mip_offsets.push_back(static_cast<int64_t>(i) * 4);
		payload.mip_sizes.push_back(4);
		payload.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kMipUncompressed));
		payload.mip_row_pitches.push_back(256);
	}
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::validate(payload));
}

static godot::PackedByteArray make_v3_from_base(const godot::PackedByteArray &p_v2_bytes, uint64_t p_capabilities, uint32_t p_chunk_count = 1) {
	godot::PackedByteArray v3_bytes;
	v3_bytes.resize(p_v2_bytes.size() + 16);
	std::memcpy(v3_bytes.ptrw(), p_v2_bytes.ptr(), 48);
	std::memset(v3_bytes.ptrw() + 48, 0, 16);
	std::memcpy(v3_bytes.ptrw() + 64, p_v2_bytes.ptr() + 48, p_v2_bytes.size() - 48);
	put_u32(v3_bytes, 4, 3);
	put_u64(v3_bytes, 40, p_capabilities);
	put_u32(v3_bytes, 64 + 20, p_chunk_count);
	return v3_bytes;
}

GDEX_TEST_T(preloader_validation, v3_chunked_capability_without_chunked_mip_rejected, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	track_cleanup(ctx, { kValidBase1, kPatchChunkCap });

	godot::PackedByteArray bytes = make_v3_from_base(read_bytes(kValidBase1), static_cast<uint64_t>(godot::DSCompressionFormat::GDEFLATE) | godot::DSTEX_CAP_CHUNKED, 1);
	GDEX_ASSERT_TRUE(write_bytes(kPatchChunkCap, bytes));

	godot::DSPreloader::PayloadHeader base_header;
	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(kValidBase1), base_header));

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchChunkCap), patched_header));
}

GDEX_TEST_T(preloader_validation, v3_mip_footprint_mismatch_rejected, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	track_cleanup(ctx, { kValidBase1, kPatchV3Footprint });

	godot::PackedByteArray bytes = make_v3_from_base(read_bytes(kValidBase1), static_cast<uint64_t>(godot::DSCompressionFormat::GDEFLATE), 1);
	put_u64(bytes, 24, 512);    // header original_size
	put_u32(bytes, 64 + 12, 512); // mip uncompressed_size; sums still match
	GDEX_ASSERT_TRUE(write_bytes(kPatchV3Footprint, bytes));

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchV3Footprint), patched_header));
}

GDEX_TEST_T(preloader_validation, v3_mip_offset_inside_tables_rejected, TAG_UNIT) {
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	track_cleanup(ctx, { kValidBase1, kPatchV3Offset });

	godot::PackedByteArray bytes = make_v3_from_base(read_bytes(kValidBase1), static_cast<uint64_t>(godot::DSCompressionFormat::GDEFLATE), 1);
	put_u64(bytes, 64, 0); // mip offset aliases the header
	GDEX_ASSERT_TRUE(write_bytes(kPatchV3Offset, bytes));

	godot::DSPreloader::PayloadHeader patched_header;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::read_header(godot::String(kPatchV3Offset), patched_header));
}

GDEX_TEST_T(preloader_validation, validate_rejects_oversized_payload, TAG_UNIT) {
	// 64x4 RGBA8: width*height*32 = 8192, and 16384 exceeds it while staying
	// under both the 512 MiB total cap and UINT32_MAX, so only the footprint
	// clause can reject this - in the v1 branch and in the v2 branch alike.
	godot::DSPreloader::Payload v1;
	v1.engine_path = godot::String("user://gdex_validation_v1.png");
	v1.payload_key = v1.engine_path;
	v1.width = 64;
	v1.height = 4;
	v1.mip_count = 1;
	v1.pixel_format = static_cast<uint32_t>(godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8));
	v1.original_size = 16384;
	v1.compressed_size = 1024;
	v1.is_v2 = false;
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::validate(v1));

	godot::DSPreloader::Payload v2 = v1;
	v2.is_v2 = true;
	v2.mip_offsets.push_back(0);
	v2.mip_sizes.push_back(1024);
	v2.mip_uncompressed_sizes.push_back(16384);
	v2.mip_row_pitches.push_back(256);
	GDEX_EXPECT_FALSE(godot::PreloaderUnpackTest::validate(v2));
}

GDEX_TEST_T(preloader_validation, writer_output_still_accepted, TAG_UNIT) {
	// Anti-"reject everything" control: everything the writer emits must keep
	// passing all three predicates, or the change is a regression, not a fix.
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	GDEX_ASSERT_TRUE(write_valid_base(kValidBase7, kBaseMips));
	track_cleanup(ctx, { kValidBase1, kValidBase7 });

	const char *const paths[2] = { kValidBase1, kValidBase7 };
	const int expected_mips[2] = { 1, kBaseMips };
	for (int i = 0; i < 2; ++i) {
		godot::DSPreloader::PayloadHeader header;
		GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(paths[i]), header));
		GDEX_EXPECT_EQ(static_cast<int>(header.mip_count), expected_mips[i]);
		GDEX_EXPECT_TRUE(loader_header(paths[i]).size() > 0);

		godot::DSPreloader::Payload payload;
		GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::resolve(godot::String(paths[i]), payload));
		GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::validate(payload));
	}
}

GDEX_TEST_T(preloader_validation, payload_offset_matches_mip_table, TAG_UNIT) {
	// data_offset is the request offset the standalone prewarm uses, so it must be
	// the mip table's first payload byte. Enumerated over every mip count the
	// writer can produce from a 64x64 base, in both file layouts, so the claim
	// cannot hold for one shape and fail for another.
	const char *const kV2 = "user://gdex_offset_probe_v2.dstex";
	const char *const kV3 = "user://gdex_offset_probe_v3.dstex";
	track_cleanup(ctx, { kV2, kV3 });

	const char *const paths[2] = { kV2, kV3 };
	const int64_t expected_version[2] = { 2, 3 };
	int failing_shapes = 0;

	for (int mips = 1; mips <= 7; ++mips) {
		godot::Ref<godot::DSTex> writer;
		writer.instantiate();
		GDEX_ASSERT_EQ((int64_t)writer->write_to_file(godot::String(kV2), make_chain_data(mips),
							   kBaseWidth, kBaseHeight, godot::Image::FORMAT_RGBA8, mips, 1),
				(int64_t)godot::OK);
		GDEX_ASSERT_EQ((int64_t)writer->write_to_file(godot::String(kV3), make_chain_data(mips),
							   kBaseWidth, kBaseHeight, godot::Image::FORMAT_RGBA8, mips, 1,
							   false, false, kBaseWidth - 4, kBaseHeight - 8),
				(int64_t)godot::OK);

		for (int v = 0; v < 2; ++v) {
			godot::Dictionary dict = loader_header(paths[v]);
			GDEX_EXPECT_EQ((int64_t)dict["version"], expected_version[v]);
			const godot::PackedInt64Array table_offsets = dict["mip_offsets"];
			GDEX_ASSERT_EQ(table_offsets.size(), (int64_t)mips);

			godot::DSPreloader::PayloadHeader header;
			GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::read_header(godot::String(paths[v]), header));

			godot::DSPreloader::Payload payload;
			godot::PreloaderUnpackTest::payload_from_header(godot::String(paths[v]), godot::String(paths[v]), header, payload);

			const int64_t aligned = (int64_t)payload.data_offset;
			GDEX_EXPECT_EQ(aligned, table_offsets[0]);
			GDEX_EXPECT_EQ(aligned % 512, (int64_t)0);
			if (aligned != table_offsets[0]) {
				failing_shapes++;
			}
			godot::UtilityFunctions::print(godot::String("p2: mips=") + godot::String::num_int64(mips) +
					" version=" + godot::String::num_int64(expected_version[v]) +
					" data_offset=" + godot::String::num_int64(aligned) +
					" first_mip=" + godot::String::num_int64(table_offsets[0]));
		}
	}

	godot::UtilityFunctions::print(godot::String("p2: shapes_mismatched=") + godot::String::num_int64(failing_shapes));
	GDEX_EXPECT_EQ(failing_shapes, 0);
}

GDEX_TEST_T(preloader_validation, oversized_dstex_not_tracked, TAG_INTEGRATION) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable (module init did not run)");
		return;
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("initialize_ds() failed on this host");
		return;
	}

	GDEX_ASSERT_TRUE(write_valid_base(kValidBase1, 1));
	// Registered before the cache: teardowns run LIFO, so the cache (and its
	// request paths) is torn down before these files are removed from disk.
	track_cleanup(ctx, { kValidBase1, kPatchDim });

	godot::PackedByteArray bytes = read_bytes(kValidBase1);
	GDEX_ASSERT_FALSE(bytes.is_empty());
	put_u32(bytes, 8, 65535u);
	GDEX_ASSERT_TRUE(write_bytes(kPatchDim, bytes));

	// No ResourceLoader.load() on this path: loading populates DSTextureCache and
	// makes the following prewarm a cache hit, which would report (0, 0) for a
	// reason unrelated to validation.
	godot::DSTextureCache *cache = make_budget_cache(ctx, true);

	godot::PackedStringArray paths;
	paths.append(godot::String(kPatchDim));
	const uint64_t tag = cache->prewarm_async(paths);
	GDEX_EXPECT_GT(tag, static_cast<uint64_t>(0));

	// 0 tracked requests: the file was refused before any destination buffer or
	// stream request was created.
	const godot::Vector2i progress = ds->get_prewarm_progress(tag);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.y), static_cast<int64_t>(0));
	GDEX_EXPECT_TRUE(ds->is_prewarm_complete(tag));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->get_pending_prewarm_count(tag)), static_cast<int64_t>(0));

	ds->cancel_prewarm(tag);
}

namespace {
constexpr const char *kParityBptc = "user://importer_parity_bptc.dstex";
constexpr const char *kParityGuard = "user://importer_parity_guard.dstex";
} // namespace

GDEX_TEST(preloader_validation, bptc_round_trip) {
	godot::Ref<godot::Image> image = godot::Image::create(64, 64, false, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(image.is_valid());
	for (int y = 0; y < 64; y++) {
		for (int x = 0; x < 64; x++) {
			image->set_pixel(x, y, godot::Color(float(x) / 63.0f, float(y) / 63.0f, 0.25f, 1.0f));
		}
	}
	GDEX_ASSERT_EQ(static_cast<int64_t>(image->generate_mipmaps()), static_cast<int64_t>(godot::OK));

	godot::Error compress_error = image->compress(godot::Image::COMPRESS_BPTC, godot::Image::COMPRESS_SOURCE_SRGB);
	if (compress_error == godot::ERR_UNAVAILABLE) {
		GDEX_SKIP("BPTC encoder unavailable (editor build without cvtt)");
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(compress_error), static_cast<int64_t>(godot::OK));
	GDEX_EXPECT_TRUE(image->is_compressed());
	GDEX_EXPECT_EQ(static_cast<int64_t>(image->get_format()), static_cast<int64_t>(godot::Image::FORMAT_BPTC_RGBA));

	track_cleanup(ctx, { kParityBptc });

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const int mip_count = image->get_mipmap_count() + 1;
	GDEX_EXPECT_EQ(static_cast<int64_t>(dstex->write_to_file(godot::String(kParityBptc), image->get_data(),
			image->get_width(), image->get_height(), image->get_format(), mip_count, 1, true, false)),
			static_cast<int64_t>(godot::OK));

	const godot::Dictionary header = dstex->read_header(godot::String(kParityBptc));
	GDEX_EXPECT_FALSE(header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<int>(header["pixel_format"])),
			static_cast<int64_t>(godot::RenderingDevice::DATA_FORMAT_BC7_SRGB_BLOCK));
	GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<int>(header["mip_count"])), static_cast<int64_t>(mip_count));

	uint64_t expected_footprint = 0;
	uint32_t width = static_cast<uint32_t>(image->get_width());
	uint32_t height = static_cast<uint32_t>(image->get_height());
	for (int i = 0; i < mip_count; i++) {
		const godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(width, height, godot::Image::FORMAT_BPTC_RGBA);
		expected_footprint += layout.footprint_size;
		width = width > 1 ? width / 2 : 1;
		height = height > 1 ? height / 2 : 1;
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["original_size"]), static_cast<int64_t>(expected_footprint));
}

GDEX_TEST(preloader_validation, writer_guard) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	track_cleanup(ctx, { kParityGuard });

	godot::PackedByteArray tiny;
	tiny.push_back(0);

	GDEX_EXPECT_EQ(static_cast<int64_t>(dstex->write_to_file(godot::String(kParityGuard), tiny, 16385, 1,
			godot::Image::FORMAT_R8, 1, 1)),
			static_cast<int64_t>(godot::ERR_INVALID_DATA));

	// 16384x16384 RGBA8 is a 1 GiB footprint, over DSTEX_MAX_TOTAL_BYTES. The
	// guard runs after the layout pass and before the raw-size comparison, so a
	// 1-byte payload is enough to prove the rejection without allocating 1 GiB.
	GDEX_EXPECT_EQ(static_cast<int64_t>(dstex->write_to_file(godot::String(kParityGuard), tiny, 16384, 16384,
			godot::Image::FORMAT_RGBA8, 1, 1)),
			static_cast<int64_t>(godot::ERR_INVALID_DATA));

	godot::PackedByteArray small;
	small.resize(8 * 8);
	uint8_t *small_ptr = small.ptrw();
	memset(small_ptr, 0x5A, 8 * 8);
	GDEX_EXPECT_EQ(static_cast<int64_t>(dstex->write_to_file(godot::String(kParityGuard), small, 8, 8,
			godot::Image::FORMAT_R8, 1, 1)),
			static_cast<int64_t>(godot::OK));
}

// The memory prewarm path must carry the container's codec. A NONE payload
// (written when GDeflate loses to the raw footprint) labelled as GDeflate is
// exactly what makes the uncompressed fallback unloadable.
GDEX_TEST(preloader_unpack, none_v2_payload_expands_to_none_requests) {
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_none_v2.png";
	meta.file_path = "user://test_none_v2.dstex";
	meta.width = 64;
	meta.height = 64;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;
	meta.original_size = 64 * 64 * 4;
	meta.compressed_size = 64 * 64 * 4;
	meta.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	meta.data_offset = 512;
	meta.mip_offsets.push_back(512);
	meta.mip_sizes.push_back(64 * 64 * 4);
	meta.mip_uncompressed_sizes.push_back(64 * 64 * 4);
	meta.mip_row_pitches.push_back(64 * 4);

	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::validate(meta));
	godot::Vector<godot::Ref<godot::DSRequest>> requests;
	GDEX_ASSERT_TRUE(godot::PreloaderUnpackTest::expand(meta, 7, requests));
	GDEX_ASSERT_EQ(requests.size(), 1);
	GDEX_EXPECT_EQ(static_cast<int>(requests[0]->get_compression_format()),
			static_cast<int>(godot::DSRequest::COMPRESSION_NONE));
	GDEX_EXPECT_EQ(static_cast<int64_t>(requests[0]->get_size()), static_cast<int64_t>(64 * 64 * 4));
	GDEX_EXPECT_EQ(static_cast<int64_t>(requests[0]->get_uncompressed_size()), static_cast<int64_t>(64 * 64 * 4));
	GDEX_EXPECT_EQ(static_cast<int64_t>(requests[0]->get_offset()), static_cast<int64_t>(512));

	// Positive control: the same payload declared as GDeflate stays GDeflate.
	meta.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::GDEFLATE);
	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::validate(meta));
	requests.clear();
	GDEX_ASSERT_TRUE(godot::PreloaderUnpackTest::expand(meta, 7, requests));
	GDEX_ASSERT_EQ(requests.size(), 1);
	GDEX_EXPECT_EQ(static_cast<int>(requests[0]->get_compression_format()),
			static_cast<int>(godot::DSRequest::COMPRESSION_GDEFLATE));
}

GDEX_TEST(preloader_unpack, none_v1_payload_expands_to_none_request) {
	godot::DSPreloader::Payload meta;
	meta.engine_path = "res://test_none_v1.png";
	meta.file_path = "user://test_none_v1.dstex";
	meta.width = 64;
	meta.height = 64;
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = false;
	meta.data_offset = 28;
	meta.original_size = 64 * 64 * 4;
	meta.compressed_size = 64 * 64 * 4;
	meta.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);

	GDEX_EXPECT_TRUE(godot::PreloaderUnpackTest::validate(meta));
	godot::Vector<godot::Ref<godot::DSRequest>> requests;
	GDEX_ASSERT_TRUE(godot::PreloaderUnpackTest::expand(meta, 8, requests));
	GDEX_ASSERT_EQ(requests.size(), 1);
	GDEX_EXPECT_EQ(static_cast<int>(requests[0]->get_compression_format()),
			static_cast<int>(godot::DSRequest::COMPRESSION_NONE));
	GDEX_EXPECT_EQ(static_cast<int64_t>(requests[0]->get_offset()), static_cast<int64_t>(28));
}
