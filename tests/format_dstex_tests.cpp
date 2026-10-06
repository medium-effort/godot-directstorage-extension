#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/image.hpp>

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
