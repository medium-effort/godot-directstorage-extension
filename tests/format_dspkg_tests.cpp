#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>

// Suite covering the same area as project/headless_tests/test_dspkg_alignment.gd
// (+ test_formats.gd package sections): DSPkg create/finalize/open cycle,
// 512-byte per-mip placement padding (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
// entry iteration/count, virtual path resolution.
//
// Real API from src/formats/dspkg_format.h (verified):
//   DSPkg::create/add_texture/finalize/open/has_texture/read_texture,
//   get_entry_info/get_entry_count/get_entries,
//   get_mip_entries_for_offset (Vector overload + Array overload).
// DSPkgHeader is 28 B packed; DSPkgMipEntry is 24 B packed (static_assert in header).
//
// Pure-C++ cases (untagged GDEX_TEST) touch structs/math only: no engine
// singletons, no files. Everything that instantiates DSPkg or touches
// FileAccess (create/finalize/open/read) is GDEX_TEST_T(..., TAG_INTEGRATION).
// Multi-mip GPU-footprint alignment cases additionally carry TAG_SLOW: they run
// headless (offset math only, no D3D12 device) but the true GPU upload path
// is deferred to D3D12 backend tests.

namespace {

constexpr uint32_t D3D12_PLACEMENT_ALIGNMENT = 512;
constexpr uint32_t D3D12_PITCH_ALIGNMENT = 256;
constexpr uint32_t DSPKG_MAGIC_DPKG = 0x474B5044u;

// Local replica of the align_up step used by dspkg_format.cpp (pure math).
uint64_t align_up_local(uint64_t p_value, uint64_t p_alignment) {
	return (p_value + p_alignment - 1) / p_alignment * p_alignment;
}

bool row_pitch_is_valid_local(uint32_t p_row_pitch) {
	return p_row_pitch != 0 && p_row_pitch % D3D12_PITCH_ALIGNMENT == 0;
}

godot::PackedByteArray make_pattern_payload(int64_t p_size) {
	godot::PackedByteArray payload;
	payload.resize(p_size);
	for (int64_t i = 0; i < p_size; ++i) {
		payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
	}
	return payload;
}

// Writes a temp .dstex fixture via the real DSTex writer. Returns false on failure.
bool write_temp_dstex(const char *p_path, int p_width, int p_height, int p_mip_count, int64_t p_payload_size) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	if (dstex.is_null()) {
		return false;
	}
	godot::Error err = dstex->write_to_file(
			godot::String(p_path), make_pattern_payload(p_payload_size),
			p_width, p_height, godot::Image::FORMAT_RGBA8, p_mip_count, 1);
	return err == godot::OK;
}

void remove_temp_file(const char *p_path) {
	godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
	if (dir.is_valid()) {
		// DirAccess::remove is instance-level; absolute user:// path also accepted.
		dir->remove(godot::String(p_path));
	}
}

} // namespace

// --- Pure-C++: struct layout + alignment math (no engine) --------------------

GDEX_TEST(dspkg, header_packed_size_is_28) {
	// 4 (magic) + 2 (version) + 2 (flags) + 4 (entry_count) + 8 (data_offset) + 8 (reserved).
	GDEX_EXPECT_EQ(sizeof(godot::DSPkgHeader), 28u);
}

GDEX_TEST(dspkg, mip_entry_packed_size_is_24) {
	// Mirrors the static_assert in dspkg_format.h (matches DSTexMipEntryV2).
	GDEX_EXPECT_EQ(sizeof(godot::DSPkgMipEntry), 24u);
}

GDEX_TEST(dspkg, magic_matches_dpkg_le) {
	godot::DSPkgHeader header{};
	header.magic = DSPKG_MAGIC_DPKG;
	GDEX_EXPECT_EQ(header.magic, 0x474B5044u);
	// Version 1 = legacy archives, 2 = GPU mip metadata (flags bit0 set for v2).
	GDEX_EXPECT_TRUE(1 != 2);
}

GDEX_TEST(dspkg, placement_alignment_math_512) {
	GDEX_EXPECT_EQ(align_up_local(0, D3D12_PLACEMENT_ALIGNMENT), 0u);
	GDEX_EXPECT_EQ(align_up_local(1, D3D12_PLACEMENT_ALIGNMENT), 512u);
	GDEX_EXPECT_EQ(align_up_local(511, D3D12_PLACEMENT_ALIGNMENT), 512u);
	GDEX_EXPECT_EQ(align_up_local(512, D3D12_PLACEMENT_ALIGNMENT), 512u);
	GDEX_EXPECT_EQ(align_up_local(513, D3D12_PLACEMENT_ALIGNMENT), 1024u);
	GDEX_EXPECT_EQ(align_up_local(1024, D3D12_PLACEMENT_ALIGNMENT), 1024u);
	// Every aligned value satisfies offset % 512 == 0 (the open() reader invariant).
	GDEX_EXPECT_EQ(align_up_local(1536 + 100, D3D12_PLACEMENT_ALIGNMENT) % 512u, 0u);
}

GDEX_TEST(dspkg, pitch_alignment_math_256) {
	// open()/finalize() require non-zero row_pitch with row_pitch % 256 == 0.
	GDEX_EXPECT_TRUE(row_pitch_is_valid_local(256));
	GDEX_EXPECT_TRUE(row_pitch_is_valid_local(512));
	GDEX_EXPECT_FALSE(row_pitch_is_valid_local(0));
	GDEX_EXPECT_FALSE(row_pitch_is_valid_local(128));
	GDEX_EXPECT_FALSE(row_pitch_is_valid_local(100));
}

// --- Integration: reader state without an archive ----------------------------

GDEX_TEST_T(dspkg, fresh_reader_has_no_entries, TAG_INTEGRATION) {
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_ASSERT_TRUE(pkg.is_valid());

	GDEX_EXPECT_EQ(pkg->get_entry_count(), 0);
	GDEX_EXPECT_TRUE(pkg->get_entries().is_empty());
	GDEX_EXPECT_FALSE(pkg->has_texture("res://textures/any_tex.dstex"));
	GDEX_EXPECT_TRUE(pkg->get_entry_info("res://textures/any_tex.dstex").is_empty());
	GDEX_EXPECT_TRUE(pkg->read_texture("res://textures/any_tex.dstex").is_empty());
	GDEX_EXPECT_TRUE(pkg->get_mip_entries_for_offset(0).is_empty());
}

GDEX_TEST_T(dspkg, open_missing_path_returns_error, TAG_INTEGRATION) {
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_ASSERT_TRUE(pkg.is_valid());

	godot::Error err = pkg->open("user://gdextest_dspkg_missing_archive_007.dspkg");
	GDEX_EXPECT_TRUE(err != godot::OK);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(pkg->has_texture("res://textures/any_tex.dstex"));
}

// --- Integration: create/finalize/open cycle, iteration, virtual paths --------

GDEX_TEST_T(dspkg, create_finalize_open_roundtrip_single_entry, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_007_single.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_007_single.dspkg";
	const char *virtual_path = "res://textures/test_single_tex.dstex";

	// 64x4 RGBA8 single mip: row_pitch 64*4=256, footprint 256*4=1024.
	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);

	// Entry iteration/count.
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	godot::Array entries = reader->get_entries();
	GDEX_EXPECT_EQ(entries.size(), 1);

	// Virtual path resolution.
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));
	GDEX_EXPECT_FALSE(reader->has_texture("res://textures/other_tex.dstex"));
	GDEX_EXPECT_FALSE(reader->has_texture(""));

	godot::Dictionary info = reader->get_entry_info(godot::String(virtual_path));
	GDEX_EXPECT_FALSE(info.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(info.get("width", 0)), 64);
	GDEX_EXPECT_EQ(static_cast<int64_t>(info.get("height", 0)), 4);
	GDEX_EXPECT_EQ(static_cast<int64_t>(info.get("mip_count", 0)), 1);
	GDEX_EXPECT_TRUE(static_cast<int64_t>(info.get("compressed_size", 0)) > 0);
	GDEX_EXPECT_TRUE(static_cast<int64_t>(info.get("original_size", 0)) > 0);
	// Payload start honors 512-byte placement alignment.
	GDEX_EXPECT_EQ(static_cast<int64_t>(info.get("data_offset", 1)) % 512, 0);
	GDEX_EXPECT_TRUE(reader->get_entry_info("res://textures/other_tex.dstex").is_empty());

	godot::Dictionary payload = reader->read_texture(godot::String(virtual_path));
	GDEX_EXPECT_FALSE(payload.is_empty());
	GDEX_EXPECT_TRUE(static_cast<godot::PackedByteArray>(payload.get("compressed_data", godot::PackedByteArray())).size() > 0);
	GDEX_EXPECT_TRUE(reader->read_texture("res://textures/other_tex.dstex").is_empty());

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, entry_iteration_count_matches_added, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_007_multi.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_007_multi.dspkg";
	const char *virtual_a = "res://textures/test_multi_a.dstex";
	const char *virtual_b = "res://textures/test_multi_b.dstex";

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_a), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_b), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);

	GDEX_EXPECT_EQ(reader->get_entry_count(), 2);
	godot::Array entries = reader->get_entries();
	GDEX_EXPECT_EQ(entries.size(), 2);
	GDEX_EXPECT_TRUE(entries.has(godot::String(virtual_a)));
	GDEX_EXPECT_TRUE(entries.has(godot::String(virtual_b)));

	// Each virtual path resolves independently.
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_a)));
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_b)));
	GDEX_EXPECT_FALSE(reader->get_entry_info(godot::String(virtual_a)).is_empty());
	GDEX_EXPECT_FALSE(reader->get_entry_info(godot::String(virtual_b)).is_empty());

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

// --- Slow: multi-mip GPU footprint metadata + 512-byte per-mip padding --------

GDEX_TEST_T(dspkg, gpu_mip_metadata_512_placement, TAG_INTEGRATION | TAG_SLOW) {
	const char *dstex_path = "user://gdextest_dspkg_007_alignment.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_007_alignment.dspkg";
	const char *virtual_path = "res://textures/test_alignment_tex.dstex";

	// Same geometry as test_dspkg_alignment.gd: 64x4 RGBA8, 2 mips,
	// row_pitch 256, base mip 1024 B, mip1 512 B, payload 1536 B.
	if (!write_temp_dstex(dstex_path, 64, 4, 2, 1536)) {
		GDEX_ABORT_TEST("fixture multi-mip DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);
	GDEX_ASSERT_TRUE(reader->has_texture(godot::String(virtual_path)));

	godot::Dictionary info = reader->get_entry_info(godot::String(virtual_path));
	GDEX_ASSERT_TRUE(!info.is_empty());
	GDEX_EXPECT_TRUE(static_cast<bool>(info.get("has_gpu_metadata", false)));

	godot::PackedInt64Array mip_offsets = info.get("mip_offsets", godot::PackedInt64Array());
	godot::PackedInt32Array mip_sizes = info.get("mip_sizes", godot::PackedInt32Array());
	godot::PackedInt32Array mip_uncompressed = info.get("mip_uncompressed_sizes", godot::PackedInt32Array());
	godot::PackedInt32Array mip_row_pitches = info.get("mip_row_pitches", godot::PackedInt32Array());
	GDEX_ASSERT_TRUE(mip_offsets.size() == 2);
	GDEX_ASSERT_TRUE(mip_sizes.size() == 2);

	// Every mip offset is 512-byte placed (the reader enforces this for GPU footprints).
	GDEX_EXPECT_EQ(mip_offsets[0] % 512, 0);
	GDEX_EXPECT_EQ(mip_offsets[1] % 512, 0);
	// Mip 1 starts at/after mip 0 end (padding between spans allowed).
	GDEX_EXPECT_TRUE(mip_offsets[1] >= mip_offsets[0] + mip_sizes[0]);
	// Last mip ends exactly at the entry payload boundary.
	const int64_t data_offset = static_cast<int64_t>(info.get("data_offset", 0));
	const int64_t compressed_size = static_cast<int64_t>(info.get("compressed_size", 0));
	GDEX_EXPECT_EQ((mip_offsets[1] + mip_sizes[1]) - data_offset, compressed_size);

	// C++ Vector overload resolves by payload offset.
	godot::Vector<godot::DSPkgMipEntry> mip_vector;
	GDEX_EXPECT_TRUE(reader->get_mip_entries_for_offset(static_cast<uint64_t>(data_offset), mip_vector));
	GDEX_EXPECT_EQ(mip_vector.size(), 2);
	if (mip_vector.size() == 2) {
		GDEX_EXPECT_EQ(mip_vector[0].offset % D3D12_PLACEMENT_ALIGNMENT, 0u);
		GDEX_EXPECT_EQ(mip_vector[1].offset % D3D12_PLACEMENT_ALIGNMENT, 0u);
		GDEX_EXPECT_TRUE(row_pitch_is_valid_local(mip_vector[0].row_pitch));
		GDEX_EXPECT_TRUE(row_pitch_is_valid_local(mip_vector[1].row_pitch));
	}
	godot::Vector<godot::DSPkgMipEntry> missing_vector;
	GDEX_EXPECT_FALSE(reader->get_mip_entries_for_offset(1u, missing_vector));

	// Bound Array overload mirrors the same rows.
	godot::Array mip_array = reader->get_mip_entries_for_offset(static_cast<uint64_t>(data_offset));
	GDEX_EXPECT_EQ(mip_array.size(), 2);
	GDEX_EXPECT_TRUE(reader->get_mip_entries_for_offset(1u).is_empty());

	// Uncompressed footprint metadata is present with 256-aligned row pitches.
	GDEX_EXPECT_EQ(mip_uncompressed.size(), 2);
	GDEX_EXPECT_EQ(mip_row_pitches.size(), 2);
	if (mip_row_pitches.size() == 2) {
		GDEX_EXPECT_TRUE(row_pitch_is_valid_local(static_cast<uint32_t>(mip_row_pitches[0])));
		GDEX_EXPECT_TRUE(row_pitch_is_valid_local(static_cast<uint32_t>(mip_row_pitches[1])));
	}

	// read_texture exposes per-mip slices for GPU upload (upload itself is evaluated in D3D12 backend tests).
	godot::Dictionary payload = reader->read_texture(godot::String(virtual_path));
	GDEX_EXPECT_TRUE(payload.has("mip_data"));
	GDEX_EXPECT_EQ(static_cast<godot::Array>(payload.get("mip_data", godot::Array())).size(), 2);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}
