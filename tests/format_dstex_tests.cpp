#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/color.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>

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

// Phase 3 item 3: DSTex::remap_channels mirrors
// ResourceImporterTexture::_remap_channels (REMAP_R..REMAP_1 = 0..10).
GDEX_TEST(dstex, remap_channels_swap_red_blue) {
    godot::Ref<godot::Image> image = godot::Image::create(2, 2, false, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_TRUE(image.is_valid());
    image->set_pixel(0, 0, godot::Color(0.1f, 0.2f, 0.3f, 0.4f));
    image->set_pixel(1, 1, godot::Color(0.5f, 0.6f, 0.7f, 0.8f));

    // Blue, Green, Red, Alpha: swap R and B, keep G and A.
    godot::DSTex::remap_channels(image, 2, 1, 0, 3);

    const godot::Color a = image->get_pixel(0, 0);
    GDEX_EXPECT_NEAR(a.r, 0.3f, 2e-3f);
    GDEX_EXPECT_NEAR(a.g, 0.2f, 2e-3f);
    GDEX_EXPECT_NEAR(a.b, 0.1f, 2e-3f);
    GDEX_EXPECT_NEAR(a.a, 0.4f, 2e-3f);
    const godot::Color b = image->get_pixel(1, 1);
    GDEX_EXPECT_NEAR(b.r, 0.7f, 2e-3f);
    GDEX_EXPECT_NEAR(b.b, 0.5f, 2e-3f);
}

GDEX_TEST(dstex, remap_channels_invert_zero_one) {
    godot::Ref<godot::Image> image = godot::Image::create(1, 1, false, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_TRUE(image.is_valid());
    image->set_pixel(0, 0, godot::Color(0.25f, 0.5f, 0.75f, 1.0f));

    // Inverted Red, Zero, One, Alpha. Epsilon is wide because the value
    // quantizes twice (store 0.25, then store the inversion).
    godot::DSTex::remap_channels(image, 4, 9, 10, 3);

    const godot::Color c = image->get_pixel(0, 0);
    GDEX_EXPECT_NEAR(c.r, 0.75f, 5e-3f);
    GDEX_EXPECT_NEAR(c.g, 0.0f, 5e-3f);
    GDEX_EXPECT_NEAR(c.b, 1.0f, 5e-3f);
    GDEX_EXPECT_NEAR(c.a, 1.0f, 5e-3f);
}

GDEX_TEST(dstex, remap_channels_identity_is_noop) {
    godot::Ref<godot::Image> image = godot::Image::create(2, 2, false, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_TRUE(image.is_valid());
    image->set_pixel(0, 0, godot::Color(0.1f, 0.2f, 0.3f, 0.4f));

    godot::DSTex::remap_channels(image, 0, 1, 2, 3);

    GDEX_EXPECT_EQ((int64_t)image->get_format(), (int64_t)godot::Image::FORMAT_RGBA8);
    const godot::Color c = image->get_pixel(0, 0);
    GDEX_EXPECT_NEAR(c.r, 0.1f, 2e-3f);
    GDEX_EXPECT_NEAR(c.g, 0.2f, 2e-3f);
    GDEX_EXPECT_NEAR(c.b, 0.3f, 2e-3f);
    GDEX_EXPECT_NEAR(c.a, 0.4f, 2e-3f);
}

GDEX_TEST(dstex, remap_channels_r8_identity_keeps_format) {
    // The early-out must not expand single-channel formats on identity:
    // R8 assets stay R8 (the importer relies on this for mode-1 payloads).
    godot::Ref<godot::Image> image = godot::Image::create(4, 4, false, godot::Image::FORMAT_R8);
    GDEX_EXPECT_TRUE(image.is_valid());

    godot::DSTex::remap_channels(image, 0, 1, 2, 3);

    GDEX_EXPECT_EQ((int64_t)image->get_format(), (int64_t)godot::Image::FORMAT_R8);
}

GDEX_TEST(dstex, remap_channels_r8_to_rg_on_demand) {
    // Requesting green output from an R8 image expands channels like the engine
    // (R8 -> RG8, not RGBA8); the pixel loop then reads the expanded image.
    godot::Ref<godot::Image> image = godot::Image::create(2, 2, false, godot::Image::FORMAT_R8);
    GDEX_EXPECT_TRUE(image.is_valid());
    image->set_pixel(0, 0, godot::Color(0.5f, 0.0f, 0.0f, 1.0f));

    // Red, Red, Zero, One: green reads red.
    godot::DSTex::remap_channels(image, 0, 0, 9, 10);

    GDEX_EXPECT_EQ((int64_t)image->get_format(), (int64_t)godot::Image::FORMAT_RG8);
    const godot::Color c = image->get_pixel(0, 0);
    GDEX_EXPECT_NEAR(c.r, 0.5f, 2e-3f);
    GDEX_EXPECT_NEAR(c.g, 0.5f, 2e-3f);
    GDEX_EXPECT_NEAR(c.b, 0.0f, 2e-3f);
    GDEX_EXPECT_NEAR(c.a, 1.0f, 2e-3f);
}

// The writer must never leave a truncated .dstex behind: a short file still
// carries a readable header and mip table, so a reader cannot tell it from a
// complete one. The injection drops bytes while the writer still reports
// success, which is why the final size verification is what gets exercised.
GDEX_TEST_T(dstex, write_failure_aborts, TAG_INTEGRATION) {
    const char *truncated_path = "user://gdextest_dstex_write_fail.dstex";
    const char *complete_path = "user://gdextest_dstex_write_ok.dstex";

    godot::PackedByteArray payload;
    payload.resize(64 * 64 * 4);
    uint32_t state = 0x9E3779B9u;
    for (int64_t i = 0; i < payload.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = static_cast<uint8_t>(state & 0xFF);
    }

    godot::Ref<godot::DSTex> failing;
    failing.instantiate();
    GDEX_ASSERT_TRUE(failing.is_valid());
    failing->_set_write_truncate_for_testing(8);
    GDEX_EXPECT_EQ(failing->write_to_file(godot::String(truncated_path), payload, 64, 64,
                           godot::Image::FORMAT_RGBA8, 1, 1),
            godot::ERR_FILE_CANT_WRITE);
    GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(truncated_path)));

    godot::Ref<godot::DSTex> clean;
    clean.instantiate();
    GDEX_ASSERT_TRUE(clean.is_valid());
    GDEX_EXPECT_EQ(clean->write_to_file(godot::String(complete_path), payload, 64, 64,
                           godot::Image::FORMAT_RGBA8, 1, 1),
            godot::OK);
    GDEX_ASSERT_TRUE(godot::FileAccess::file_exists(godot::String(complete_path)));

    godot::Dictionary header = clean->read_header(godot::String(complete_path));
    GDEX_EXPECT_TRUE(header.size() > 0);

    godot::Ref<godot::FileAccess> written = godot::FileAccess::open(godot::String(complete_path), godot::FileAccess::READ);
    GDEX_ASSERT_TRUE(written.is_valid());
    GDEX_EXPECT_TRUE(written->get_length() > static_cast<int64_t>(sizeof(godot::DSTexHeader)));
    written->close();

    godot::DirAccess::remove_absolute(godot::String(truncated_path));
    godot::DirAccess::remove_absolute(godot::String(complete_path));
}

// Compression is not always a benefit: when GDeflate expands the payload the
// file must hold the raw 256-byte-pitch footprints instead of a larger stream
// that still needs a decompression at load. NONE has no v2 representation
// (v2 carries no codec field), so the writer must upgrade the file to v3.
GDEX_TEST_T(dstex, uncompressible_payload_falls_back_to_none, TAG_INTEGRATION) {
    const char *path = "user://gdextest_dstex_none.dstex";

    godot::PackedByteArray payload;
    payload.resize(64 * 64 * 4);
    uint32_t state = 0x9E3779B9u;
    for (int64_t i = 0; i < payload.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = static_cast<uint8_t>(state & 0xFF);
    }

    godot::Ref<godot::DSTex> writer;
    writer.instantiate();
    GDEX_ASSERT_TRUE(writer.is_valid());
    GDEX_EXPECT_EQ(writer->write_to_file(godot::String(path), payload, 64, 64,
                           godot::Image::FORMAT_RGBA8, 1, 1),
            godot::OK);

    godot::Dictionary header = writer->read_header(godot::String(path));
    GDEX_ASSERT_TRUE(header.size() > 0);
    GDEX_EXPECT_EQ((int)header["version"], 3);
    GDEX_EXPECT_EQ((int)header["compression_format"], static_cast<int>(godot::DSCompressionFormat::NONE));
    GDEX_EXPECT_EQ((int64_t)header["original_size"], static_cast<int64_t>(payload.size()));
    GDEX_EXPECT_EQ((int64_t)header["compressed_size"], static_cast<int64_t>(payload.size()));

    godot::Dictionary decoded = writer->read_from_file(godot::String(path));
    GDEX_ASSERT_TRUE(decoded.size() > 0);
    godot::Array mip_data = decoded["mip_data"];
    GDEX_ASSERT_EQ(mip_data.size(), 1);
    godot::PackedByteArray mip0 = mip_data[0];
    GDEX_EXPECT_TRUE(mip0 == payload);

    godot::DirAccess::remove_absolute(godot::String(path));
}

// Positive control for the test above: data the codec CAN shrink must still be
// stored as GDeflate, and must stay on the v2 layout (no needless v3 upgrade).
GDEX_TEST_T(dstex, compressible_payload_keeps_gdeflate, TAG_INTEGRATION) {
    const char *path = "user://gdextest_dstex_gdeflate.dstex";

    godot::PackedByteArray payload;
    payload.resize(64 * 64 * 4); // resize() zero-fills: maximally compressible

    godot::Ref<godot::DSTex> writer;
    writer.instantiate();
    GDEX_ASSERT_TRUE(writer.is_valid());
    GDEX_EXPECT_EQ(writer->write_to_file(godot::String(path), payload, 64, 64,
                           godot::Image::FORMAT_RGBA8, 1, 1),
            godot::OK);

    godot::Dictionary header = writer->read_header(godot::String(path));
    GDEX_ASSERT_TRUE(header.size() > 0);
    GDEX_EXPECT_EQ((int)header["version"], 2);
    GDEX_EXPECT_EQ((int)header["compression_format"], static_cast<int>(godot::DSCompressionFormat::GDEFLATE));
    GDEX_EXPECT_LT((int64_t)header["compressed_size"], (int64_t)header["original_size"]);

    godot::DirAccess::remove_absolute(godot::String(path));
}

// Every NONE mip entry must declare compressed_size == uncompressed_size: the
// header reader, the v3 chunk parser and the loader all reject a mismatch.
GDEX_TEST_T(dstex, none_mip_entries_are_size_symmetric, TAG_INTEGRATION) {
    const char *path = "user://gdextest_dstex_none_mips.dstex";

    // Two mips of pseudorandom data: 256x256 then 128x128. Both pitches equal
    // their row bytes (1024 / 512), so no zero padding can rescue the ratio and
    // the total codec decision has to land on NONE.
    const int64_t base_bytes = 256 * 256 * 4;
    const int64_t next_bytes = 128 * 128 * 4;
    godot::PackedByteArray payload;
    payload.resize(base_bytes + next_bytes);
    uint32_t state = 0x1234567u;
    for (int64_t i = 0; i < payload.size(); ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        payload[i] = static_cast<uint8_t>(state & 0xFF);
    }

    godot::Ref<godot::DSTex> writer;
    writer.instantiate();
    GDEX_ASSERT_TRUE(writer.is_valid());
    GDEX_EXPECT_EQ(writer->write_to_file(godot::String(path), payload, 256, 256,
                           godot::Image::FORMAT_RGBA8, 2, 1),
            godot::OK);

    godot::Dictionary header = writer->read_header(godot::String(path));
    GDEX_ASSERT_TRUE(header.size() > 0);
    GDEX_EXPECT_EQ((int)header["compression_format"], static_cast<int>(godot::DSCompressionFormat::NONE));

    godot::PackedInt32Array mip_sizes = header["mip_sizes"];
    godot::PackedInt32Array mip_uncompressed = header["mip_uncompressed_sizes"];
    GDEX_ASSERT_EQ(mip_sizes.size(), 2);
    GDEX_ASSERT_EQ(mip_uncompressed.size(), 2);
    for (int i = 0; i < mip_sizes.size(); ++i) {
        GDEX_EXPECT_EQ(mip_sizes[i], mip_uncompressed[i]);
    }

    godot::Dictionary decoded = writer->read_from_file(godot::String(path));
    GDEX_ASSERT_TRUE(decoded.size() > 0);
    godot::Array mip_data = decoded["mip_data"];
    GDEX_ASSERT_EQ(mip_data.size(), 2);
    // 256x256 RGBA8 rows are already 256-byte multiples, so mip 0's footprint
    // equals its row-packed source bytes.
    godot::PackedByteArray expected0;
    expected0.resize(base_bytes);
    memcpy(expected0.ptrw(), payload.ptr(), static_cast<size_t>(base_bytes));
    godot::PackedByteArray mip0 = mip_data[0];
    GDEX_EXPECT_TRUE(mip0 == expected0);

    godot::DirAccess::remove_absolute(godot::String(path));
}

GDEX_TEST_T(dstex, repack_uncompressed_to_buffer_roundtrip, TAG_INTEGRATION) {
    const char *src_path = "user://gdextest_dstex_repack_src.dstex";
    const char *out_path = "user://gdextest_dstex_repack_out.dstex";
    godot::DirAccess::remove_absolute(godot::String(src_path));
    godot::DirAccess::remove_absolute(godot::String(out_path));

    // Compressible 64x64 RGBA8 (2 mips) with 1 KiB chunking -> chunked GDeflate v3.
    godot::PackedByteArray payload;
    int w = 64;
    int h = 64;
    for (int m = 0; m < 2; ++m) {
        godot::Ref<godot::Image> img = godot::Image::create(w, h, false, godot::Image::FORMAT_RGBA8);
        img->fill(godot::Color(0.2f * static_cast<float>(m + 1), 0.5f, 0.8f, 1.0f));
        payload.append_array(img->get_data());
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }

    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    GDEX_ASSERT_EQ(static_cast<int64_t>(dstex->write_to_file(
                           godot::String(src_path), payload, 64, 64, godot::Image::FORMAT_RGBA8, 2, 1, false, false, 0, 0, 0, 1)),
            static_cast<int64_t>(godot::OK));

    godot::Dictionary src_hdr = dstex->read_header(godot::String(src_path));
    GDEX_ASSERT_EQ((int)src_hdr["compression_format"], static_cast<int>(godot::DSCompressionFormat::GDEFLATE));
    GDEX_ASSERT_TRUE(static_cast<bool>(src_hdr["chunked"]));

    godot::PackedByteArray repacked = dstex->repack_uncompressed_to_buffer(godot::String(src_path));
    GDEX_ASSERT_TRUE(repacked.size() > 0);

    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(godot::String(out_path), godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid() && f->is_open());
    f->store_buffer(repacked);
    f->close();

    godot::Dictionary out_hdr = dstex->read_header(godot::String(out_path));
    GDEX_ASSERT_TRUE(!out_hdr.is_empty());
    GDEX_EXPECT_EQ((int)out_hdr["version"], 3);
    GDEX_EXPECT_EQ((int)out_hdr["compression_format"], static_cast<int>(godot::DSCompressionFormat::NONE));
    GDEX_EXPECT_TRUE(!static_cast<bool>(out_hdr["chunked"]));

    godot::Dictionary decoded = dstex->read_from_file(godot::String(out_path));
    godot::Array mip_data = decoded["mip_data"];
    GDEX_ASSERT_EQ(mip_data.size(), 2);
    godot::PackedByteArray mip0 = mip_data[0];
    GDEX_ASSERT_EQ(mip0.size(), 64 * 64 * 4);
    GDEX_EXPECT_EQ(memcmp(mip0.ptr(), payload.ptr(), 64 * 64 * 4), 0);

    godot::DirAccess::remove_absolute(godot::String(src_path));
    godot::DirAccess::remove_absolute(godot::String(out_path));
}

GDEX_TEST_T(dstex, repack_uncompressed_chunked_none_roundtrip, TAG_INTEGRATION) {
    // The writer stores an incompressible payload as NONE and still splits it on
    // the row grid, so a valid .dstex can be chunked NONE. Repacking it must
    // coalesce the raw row slices into one contiguous footprint per mip instead
    // of rejecting the codec or feeding raw bytes to the GDeflate decoder.
    const char *src_path = "user://gdextest_dstex_repack_none_src.dstex";
    const char *out_path = "user://gdextest_dstex_repack_none_out.dstex";
    godot::DirAccess::remove_absolute(godot::String(src_path));
    godot::DirAccess::remove_absolute(godot::String(out_path));

    godot::PackedByteArray payload;
    payload.resize(64 * 64 * 4);
    uint32_t state = 0x12345678u;
    for (int i = 0; i < payload.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        payload.ptrw()[i] = static_cast<uint8_t>(state >> 24);
    }

    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    GDEX_ASSERT_EQ(static_cast<int64_t>(dstex->write_to_file(
                           godot::String(src_path), payload, 64, 64, godot::Image::FORMAT_RGBA8, 1, 1, false, false, 0, 0, 0, 1)),
            static_cast<int64_t>(godot::OK));

    godot::Dictionary src_hdr = dstex->read_header(godot::String(src_path));
    GDEX_ASSERT_TRUE(!src_hdr.is_empty());
    GDEX_ASSERT_EQ((int)src_hdr["version"], 3);
    GDEX_ASSERT_EQ((int)src_hdr["compression_format"], static_cast<int>(godot::DSCompressionFormat::NONE));
    GDEX_ASSERT_TRUE(static_cast<bool>(src_hdr["chunked"]));

    godot::PackedByteArray repacked = dstex->repack_uncompressed_to_buffer(godot::String(src_path));
    GDEX_ASSERT_TRUE(repacked.size() > 0);

    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(godot::String(out_path), godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid() && f->is_open());
    f->store_buffer(repacked);
    f->close();

    godot::Dictionary out_hdr = dstex->read_header(godot::String(out_path));
    GDEX_ASSERT_TRUE(!out_hdr.is_empty());
    GDEX_EXPECT_EQ((int)out_hdr["version"], 3);
    GDEX_EXPECT_EQ((int)out_hdr["compression_format"], static_cast<int>(godot::DSCompressionFormat::NONE));
    GDEX_EXPECT_TRUE(!static_cast<bool>(out_hdr["chunked"]));

    godot::Dictionary decoded = dstex->read_from_file(godot::String(out_path));
    godot::Array mip_data = decoded["mip_data"];
    GDEX_ASSERT_EQ(mip_data.size(), 1);
    godot::PackedByteArray mip0 = mip_data[0];
    GDEX_ASSERT_EQ(mip0.size(), 64 * 64 * 4);
    GDEX_EXPECT_EQ(memcmp(mip0.ptr(), payload.ptr(), 64 * 64 * 4), 0);

    godot::DirAccess::remove_absolute(godot::String(src_path));
    godot::DirAccess::remove_absolute(godot::String(out_path));
}
