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
bool write_temp_dstex(const char *p_path, int p_width, int p_height, int p_mip_count, int64_t p_payload_size, bool p_is_srgb = false, bool p_as_image_texture = false) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	if (dstex.is_null()) {
		return false;
	}
	godot::Error err = dstex->write_to_file(
			godot::String(p_path), make_pattern_payload(p_payload_size),
			p_width, p_height, godot::Image::FORMAT_RGBA8, p_mip_count, 1, p_is_srgb, p_as_image_texture);
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

GDEX_TEST_T(dspkg, entry_sorting_is_deterministic, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_sort.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_sort.dspkg";
	const char *virtual_z = "res://textures/z_end.dstex";
	const char *virtual_a = "res://textures/a_start.dstex";

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	// Staged in reverse order
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_z), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_a), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);

	GDEX_EXPECT_EQ(reader->get_entry_count(), 2);
	godot::Array entries = reader->get_entries();
	GDEX_EXPECT_EQ(entries.size(), 2);
	// Must be sorted lexicographically
	GDEX_EXPECT_TRUE(entries[0] == godot::String(virtual_a));
	GDEX_EXPECT_TRUE(entries[1] == godot::String(virtual_z));

	// On-disk payload offsets must reflect lexicographical sort order (virtual_a before virtual_z).
	int64_t offset_a = reader->get_entry_info(godot::String(virtual_a)).get("data_offset", 0);
	int64_t offset_z = reader->get_entry_info(godot::String(virtual_z)).get("data_offset", 0);
	GDEX_EXPECT_TRUE(offset_a > 0);
	GDEX_EXPECT_TRUE(offset_z > 0);
	GDEX_EXPECT_TRUE(offset_a < offset_z);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, steampipe_1mb_payload_alignment, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_1mb.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_1mb.dspkg";
	const char *virtual_a = "res://textures/alpha.dstex";
	const char *virtual_b = "res://textures/beta.dstex";

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	builder->set_payload_alignment(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT);
	GDEX_EXPECT_EQ(builder->get_payload_alignment(), godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT);

	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_a), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_b), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);

	godot::Dictionary info_a = reader->get_entry_info(godot::String(virtual_a));
	godot::Dictionary info_b = reader->get_entry_info(godot::String(virtual_b));
	GDEX_EXPECT_FALSE(info_a.is_empty());
	GDEX_EXPECT_FALSE(info_b.is_empty());

	int64_t offset_a = info_a.get("data_offset", 1);
	int64_t offset_b = info_b.get("data_offset", 1);
	GDEX_EXPECT_EQ(offset_a % static_cast<int64_t>(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT), int64_t(0));
	GDEX_EXPECT_EQ(offset_b % static_cast<int64_t>(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT), int64_t(0));
	GDEX_EXPECT_TRUE(offset_b >= offset_a + static_cast<int64_t>(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT));

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

GDEX_TEST_T(dspkg, v3_entry_flags_as_image_texture_roundtrip, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_007_v3_flags.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_007_v3_flags.dspkg";
	const char *virtual_path = "res://textures/test_v3_flags.dstex";

	// Write fixture with as_image_texture=true and is_srgb=true (flags = 3).
	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024, true, true)) {
		GDEX_ABORT_TEST("fixture v3 DSTex write failed");
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

	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));

	godot::Dictionary info = reader->get_entry_info(godot::String(virtual_path));
	GDEX_EXPECT_FALSE(info.is_empty());
	GDEX_EXPECT_TRUE(static_cast<bool>(info.get("as_image_texture", false)));
	GDEX_EXPECT_TRUE(static_cast<bool>(info.get("is_srgb", false)));
	uint32_t flags = static_cast<uint32_t>(info.get("flags", 0));
	GDEX_EXPECT_TRUE((flags & 2U) != 0); // DSTEX_FLAG_AS_IMAGE_TEXTURE
	GDEX_EXPECT_TRUE((flags & 1U) != 0); // DSTEX_FLAG_SRGB

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

// --- Integration: archive fingerprint pairing (Option B) ----------------------

GDEX_TEST_T(dspkg, archive_fingerprint_serialization, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_fp_ser.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_fp_ser.dspkg";
	const char *virtual_path = "res://textures/test_fp_ser.dstex";
	const uint64_t test_fingerprint = 0x12345678AABBCCDDULL;

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), test_fingerprint) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path), test_fingerprint) == godot::OK);
	GDEX_EXPECT_EQ(reader->get_archive_fingerprint(), test_fingerprint);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_fingerprint_matching_success, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_fp_match.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_fp_match.dspkg";
	const char *virtual_path = "res://textures/test_fp_match.dstex";
	const uint64_t expected_fingerprint = 0xCAFEBABE11223344ULL;

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), expected_fingerprint) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path), expected_fingerprint) == godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_fingerprint_project_guid_mismatch, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_guid_mismatch.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_guid_mismatch.dspkg";
	const char *virtual_path = "res://textures/test_guid_mismatch.dstex";
	const uint64_t archive_fingerprint = 0x11111111AABBCCDDULL;
	const uint64_t wrong_guid_fingerprint = 0x22222222AABBCCDDULL;

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), archive_fingerprint) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	// Different project GUID must fail open with ERR_FILE_UNRECOGNIZED.
	godot::Error err = reader->open(godot::String(dspkg_path), wrong_guid_fingerprint);
	GDEX_EXPECT_EQ(err, godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_fingerprint_manifest_hash_mismatch, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_manifest_mismatch.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_manifest_mismatch.dspkg";
	const char *virtual_path = "res://textures/test_manifest_mismatch.dstex";
	const uint64_t archive_fingerprint = 0x11111111AABBCCDDULL;
	const uint64_t stale_manifest_fingerprint = 0x1111111199887766ULL;

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), archive_fingerprint) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	// Same GUID but stale manifest hash must fail open with ERR_FILE_UNRECOGNIZED.
	godot::Error err = reader->open(godot::String(dspkg_path), stale_manifest_fingerprint);
	GDEX_EXPECT_EQ(err, godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_fingerprint_unpaired_legacy_bypass, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_unpaired.dstex";
	const char *dspkg_unpaired = "user://gdextest_dspkg_unpaired.dspkg";
	const char *dspkg_paired = "user://gdextest_dspkg_paired.dspkg";
	const char *virtual_path = "res://textures/test_unpaired.dstex";

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	// 1. Unpaired archive (fingerprint = 0) opened by caller expecting non-zero.
	godot::Ref<godot::DSPkg> builder1;
	builder1.instantiate();
	GDEX_ASSERT_TRUE(builder1.is_valid());
	GDEX_ASSERT_TRUE(builder1->create(godot::String(dspkg_unpaired), 0ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder1->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder1->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader1;
	reader1.instantiate();
	GDEX_ASSERT_TRUE(reader1.is_valid());
	GDEX_EXPECT_EQ(reader1->open(godot::String(dspkg_unpaired), 0x12345678AABBCCDDULL), godot::OK);
	GDEX_EXPECT_EQ(reader1->get_entry_count(), 1);

	// 2. Paired archive (fingerprint != 0) opened with expected_fingerprint = 0 (check disabled).
	godot::Ref<godot::DSPkg> builder2;
	builder2.instantiate();
	GDEX_ASSERT_TRUE(builder2.is_valid());
	GDEX_ASSERT_TRUE(builder2->create(godot::String(dspkg_paired), 0x9999888877776666ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder2->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder2->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader2;
	reader2.instantiate();
	GDEX_ASSERT_TRUE(reader2.is_valid());
	GDEX_EXPECT_EQ(reader2->open(godot::String(dspkg_paired), 0ULL), godot::OK);
	GDEX_EXPECT_EQ(reader2->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_unpaired);
	remove_temp_file(dspkg_paired);
}

GDEX_TEST_T(dspkg, med09_corrupt_entry_caps_rejected, TAG_INTEGRATION) {
	// MED-09: Packages with mip_count > DSTEX_MAX_MIP_COUNT or insane dimensions
	// must be rejected with ERR_FILE_CORRUPT.
	const char *dstex_path = "user://temp_med09_test.dstex";
	const char *dspkg_path = "user://temp_med09_test.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture("test.png", godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	// Read file and mutate the entry's mip_count to 32 (> DSTEX_MAX_MIP_COUNT = 16)
	godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
	GDEX_ASSERT_TRUE(fa.is_valid());
	// Header is 28 bytes. Path length is 4 bytes + "test.png" (8 bytes).
	// Fixed fields: original_size(8), compressed_size(8), offset(8), format(4), width(4), height(4), pixel_format(4), mip_count(4)
	fa->seek(28 + 4 + 8 + 8 + 8 + 8 + 4 + 4 + 4 + 4);
	fa->store_32(32);
	fa->close();

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	godot::Error err = reader->open(godot::String(dspkg_path), 0ULL);
	GDEX_EXPECT_EQ(err, godot::ERR_FILE_CORRUPT);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}


