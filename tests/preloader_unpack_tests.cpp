#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstring>
#include <vector>

namespace godot {

struct PreloaderUnpackTest {
	static Ref<Texture2D> build_texture(const DSPreloader::Payload &p_meta, const std::vector<PackedByteArray> &p_buffers) {
		return DSPreloader::build_texture(p_meta, p_buffers);
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

	// Float data format (maps to self, no sRGB counterpart path).
	godot::DSPreloader::Payload data = eligible;
	data.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RF);
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(data));

	// Single-channel block data (BC4 maps to self).
	godot::DSPreloader::Payload bc4 = eligible;
	bc4.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGTC_R);
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(bc4));

	// Already-sRGB base maps to self -> eligible.
	godot::DSPreloader::Payload srgb = eligible;
	srgb.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8, true);
	GDEX_EXPECT_TRUE(godot::DSPreloader::is_gpu_prewarm_eligible(srgb));

	// Incomplete mip tables.
	godot::DSPreloader::Payload short_tables = eligible;
	short_tables.mip_sizes.clear();
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_gpu_prewarm_eligible(short_tables));
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
