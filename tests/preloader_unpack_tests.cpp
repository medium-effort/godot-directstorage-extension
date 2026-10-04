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
