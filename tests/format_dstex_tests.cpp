#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/color.hpp>

#include <cmath>

// Spike suite: pure DSTex helpers only — no engine singletons, no files.
// Real API from src/formats/dstex_format.h (verified):
//   struct DSTexHeader, DSTex::get_mip_layout, DSTex::is_format_supported.
// Dictionary read_header(const String &) needs a file + engine FileAccess,
// so it is deliberately NOT called here (covered by headless GDScript tests).

GDEX_TEST(dstex, mip_layout_row_pitch_256_aligned) {
    auto layout = godot::DSTex::get_mip_layout(1024, 1024, godot::Image::FORMAT_DXT5);
    GDEX_EXPECT_EQ(layout.row_pitch % 256, 0);
}
GDEX_TEST(dstex, unsupported_format_rejected) {
    GDEX_EXPECT_FALSE(godot::DSTex::is_format_supported(godot::Image::FORMAT_MAX));
}

GDEX_TEST(dstex, supported_block_formats_accepted) {
    GDEX_EXPECT_TRUE(godot::DSTex::is_format_supported(godot::Image::FORMAT_DXT1));
    GDEX_EXPECT_TRUE(godot::DSTex::is_format_supported(godot::Image::FORMAT_DXT5));
    GDEX_EXPECT_TRUE(godot::DSTex::is_format_supported(godot::Image::FORMAT_RGBA8));
    GDEX_EXPECT_TRUE(godot::DSTex::is_format_supported(godot::Image::FORMAT_BPTC_RGBA));
}

GDEX_TEST(dstex, header_packed_layout_matches_spec) {
    // #pragma pack(1): 4 + 2 + 2 + 4 + 4 + 4 + 4 + 8 + 8 + 8 = 48 bytes.
    GDEX_EXPECT_EQ(sizeof(godot::DSTexHeader), 48u);
    // "DTEX" little-endian tag.
    godot::DSTexHeader header{};
    header.magic = 0x58455444u;
    header.version = 1;
    GDEX_EXPECT_EQ(header.magic, 0x58455444u);
    GDEX_EXPECT_EQ(header.version, 1);
}

GDEX_TEST(dstex, mip_layout_uncompressed_rgba8_math) {
    auto layout = godot::DSTex::get_mip_layout(256, 4, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_EQ(layout.source_row_bytes, 1024ULL);
    GDEX_EXPECT_EQ(layout.row_count, 4ULL);
    GDEX_EXPECT_EQ(layout.row_pitch, 1024ULL);
    GDEX_EXPECT_EQ(layout.row_pitch % 256ULL, 0ULL);
    GDEX_EXPECT_EQ(layout.source_size, 4096ULL);
    GDEX_EXPECT_EQ(layout.footprint_size, 4096ULL);
}

GDEX_TEST(dstex, mip_layout_zero_extent_is_empty) {
    auto layout = godot::DSTex::get_mip_layout(0, 0, godot::Image::FORMAT_DXT5);
    GDEX_EXPECT_EQ(layout.row_pitch, 0ULL);
    GDEX_EXPECT_EQ(layout.footprint_size, 0ULL);
    GDEX_EXPECT_EQ(layout.source_size, 0ULL);
}

// FMT-01: hostile header caps are pure integer checks — no texture file is
// allocated. A 64x64 RGBA8 mip chain is ~16 KiB; the "2 GiB claim" below is
// just a uint64 passed to the validator, costing zero storage.
GDEX_TEST(dstex, sane_sizes_accept_small_texture) {
    GDEX_EXPECT_TRUE(godot::dstex_sizes_are_sane(64, 64, 16384, 1000));
}

GDEX_TEST(dstex, sane_sizes_reject_2gib_claim) {
    const uint64_t two_gib = 1ULL << 31;
    GDEX_EXPECT_FALSE(godot::dstex_sizes_are_sane(64, 64, two_gib, 1000));
    GDEX_EXPECT_FALSE(godot::dstex_sizes_are_sane(64, 64, 16384, two_gib));
}

GDEX_TEST(dstex, sane_sizes_reject_oversize_dim) {
    GDEX_EXPECT_FALSE(godot::dstex_sizes_are_sane(65535, 65535, 16384, 1000));
    GDEX_EXPECT_FALSE(godot::dstex_sizes_are_sane(0, 64, 16384, 1000));
}

// Phase 1 loader share policy: counterpart-only sRGB share via
// DSTex::get_srgb_rd_format(). Linear color formats map to their sRGB
// counterpart; already-sRGB returns itself (caller skips self-share as
// no-op); non-color data formats (floats, BC4/BC5/BC6, R16 family) return
// themselves and are skipped by the caller.
GDEX_TEST(dstex, srgb_counterpart_linear_maps_to_srgb) {
    using RD = godot::RenderingDevice;
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_R8G8B8A8_UNORM), (int)RD::DATA_FORMAT_R8G8B8A8_SRGB);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC1_RGB_UNORM_BLOCK), (int)RD::DATA_FORMAT_BC1_RGB_SRGB_BLOCK);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC3_UNORM_BLOCK), (int)RD::DATA_FORMAT_BC3_SRGB_BLOCK);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC7_UNORM_BLOCK), (int)RD::DATA_FORMAT_BC7_SRGB_BLOCK);
}

GDEX_TEST(dstex, srgb_counterpart_already_srgb_returns_self) {
    using RD = godot::RenderingDevice;
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_R8G8B8A8_SRGB), (int)RD::DATA_FORMAT_R8G8B8A8_SRGB);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC7_SRGB_BLOCK), (int)RD::DATA_FORMAT_BC7_SRGB_BLOCK);
    GDEX_EXPECT_TRUE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_R8G8B8A8_SRGB));
    GDEX_EXPECT_TRUE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_BC7_SRGB_BLOCK));
    GDEX_EXPECT_FALSE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_R8G8B8A8_UNORM));
    GDEX_EXPECT_FALSE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_BC7_UNORM_BLOCK));
}

GDEX_TEST(dstex, srgb_counterpart_noncolor_returns_self_for_skip) {
    using RD = godot::RenderingDevice;
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_R32_SFLOAT), (int)RD::DATA_FORMAT_R32_SFLOAT);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC4_UNORM_BLOCK), (int)RD::DATA_FORMAT_BC4_UNORM_BLOCK);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_BC6H_SFLOAT_BLOCK), (int)RD::DATA_FORMAT_BC6H_SFLOAT_BLOCK);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_R8_UNORM), (int)RD::DATA_FORMAT_R8_UNORM);
    GDEX_EXPECT_EQ(godot::DSTex::get_srgb_rd_format(RD::DATA_FORMAT_R8G8_UNORM), (int)RD::DATA_FORMAT_R8G8_UNORM);
    GDEX_EXPECT_FALSE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_R32_SFLOAT));
    GDEX_EXPECT_FALSE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_R8_UNORM));
    GDEX_EXPECT_FALSE(godot::DSTex::is_rd_format_srgb(RD::DATA_FORMAT_R8G8_UNORM));
}

GDEX_TEST(dstex, srgb_counterpart_exhaustive_image_formats_matrix) {
    using RD = godot::RenderingDevice;
    using Image = godot::Image;
    for (int fmt = 0; fmt < Image::FORMAT_MAX; ++fmt) {
        if (!godot::DSTex::is_format_supported(fmt)) {
            continue;
        }
        int rd_linear = godot::DSTex::godot_format_to_rd_format(fmt, false);
        int rd_srgb = godot::DSTex::godot_format_to_rd_format(fmt, true);

        // Neither linear nor sRGB mapping must ever produce invalid D3D12 sRGB formats (Q44)
        GDEX_EXPECT_NE(rd_linear, (int)RD::DATA_FORMAT_R8_SRGB);
        GDEX_EXPECT_NE(rd_linear, (int)RD::DATA_FORMAT_R8G8_SRGB);
        GDEX_EXPECT_NE(rd_linear, (int)RD::DATA_FORMAT_R8G8B8_SRGB);
        GDEX_EXPECT_NE(rd_linear, (int)RD::DATA_FORMAT_B8G8R8_SRGB);

        GDEX_EXPECT_NE(rd_srgb, (int)RD::DATA_FORMAT_R8_SRGB);
        GDEX_EXPECT_NE(rd_srgb, (int)RD::DATA_FORMAT_R8G8_SRGB);
        GDEX_EXPECT_NE(rd_srgb, (int)RD::DATA_FORMAT_R8G8B8_SRGB);
        GDEX_EXPECT_NE(rd_srgb, (int)RD::DATA_FORMAT_B8G8R8_SRGB);

        if (rd_linear != (int)RD::DATA_FORMAT_MAX) {
            int counterpart = godot::DSTex::get_srgb_rd_format(rd_linear);
            GDEX_EXPECT_NE(counterpart, (int)RD::DATA_FORMAT_R8_SRGB);
            GDEX_EXPECT_NE(counterpart, (int)RD::DATA_FORMAT_R8G8_SRGB);
            GDEX_EXPECT_NE(counterpart, (int)RD::DATA_FORMAT_R8G8B8_SRGB);
            GDEX_EXPECT_NE(counterpart, (int)RD::DATA_FORMAT_B8G8R8_SRGB);

            // If counterpart differs from rd_linear, it must be an allowed D3D12 sRGB format
            if (counterpart != rd_linear && counterpart != (int)RD::DATA_FORMAT_MAX) {
                bool is_allowed_d3d12_srgb = (
                    counterpart == (int)RD::DATA_FORMAT_R8G8B8A8_SRGB ||
                    counterpart == (int)RD::DATA_FORMAT_B8G8R8A8_SRGB ||
                    counterpart == (int)RD::DATA_FORMAT_A8B8G8R8_SRGB_PACK32 ||
                    counterpart == (int)RD::DATA_FORMAT_BC1_RGB_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_BC1_RGBA_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_BC2_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_BC3_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_BC7_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK ||
                    counterpart == (int)RD::DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK
                );
                GDEX_EXPECT_TRUE(is_allowed_d3d12_srgb);
            }
        }
    }
}

GDEX_TEST(dstex, normalize_image) {
    godot::Ref<godot::Image> image = godot::Image::create(4, 4, false, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_TRUE(image.is_valid());

    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            image->set_pixel(x, y, godot::Color(0.9f, 0.2f, 0.3f, 0.5f));
        }
    }

    godot::DSTex::normalize_image(image);

    const godot::Color c = image->get_pixel(2, 2);
    const float nx = c.r * 2.0f - 1.0f;
    const float ny = c.g * 2.0f - 1.0f;
    const float nz = c.b * 2.0f - 1.0f;
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    // 8-bit channel quantization bounds the achievable precision.
    GDEX_EXPECT_NEAR(length, 1.0f, 5e-3f);
    // Alpha is not a vector component and must survive normalization.
    GDEX_EXPECT_NEAR(c.a, 0.5f, 2e-3f);
}
