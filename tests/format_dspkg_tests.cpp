#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "monocypher.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
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
// DSPkgHeader is 64 B packed in v4 (DSPKG_HEADER_SIZE_V4); legacy v1-v3 headers are 28 B (DSPKG_HEADER_SIZE_LEGACY); DSPkgMipEntry is 24 B packed (static_assert in header).
//
// Pure-C++ cases (untagged GDEX_TEST) touch structs/math only: no engine
// singletons, no files. Everything that instantiates DSPkg or touches
// FileAccess (create/finalize/open/read) is GDEX_TEST_T(..., TAG_INTEGRATION).
// Multi-mip GPU-footprint alignment cases additionally carry TAG_SLOW: they run
// headless (offset math only, no D3D12 device) but the true GPU upload path
// is deferred to D3D12 backend tests.

namespace {

struct GlobalKeyResetGuard {
	godot::String restore_key;
	GlobalKeyResetGuard(const godot::String &p_restore = "") : restore_key(p_restore) {}
	~GlobalKeyResetGuard() {
		godot::DSPkg::set_global_encryption_key(restore_key);
	}
};

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

godot::PackedByteArray make_incompressible_payload(int64_t p_size, uint32_t p_seed = 0x12345678) {
	godot::PackedByteArray payload;
	payload.resize(p_size);
	uint32_t state = p_seed;
	for (int64_t i = 0; i < p_size; ++i) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		payload[i] = static_cast<uint8_t>(state & 0xFF);
	}
	return payload;
}

bool write_temp_dstex_incompressible(const char *p_path, int p_width, int p_height, int p_mip_count, int64_t p_payload_size, uint32_t p_seed = 0x12345678, bool p_is_srgb = false, bool p_as_image_texture = false) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	if (dstex.is_null()) {
		return false;
	}
	godot::Error err = dstex->write_to_file(
			godot::String(p_path), make_incompressible_payload(p_payload_size, p_seed),
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

GDEX_TEST(dspkg, header_v4_layout) {
	GDEX_ASSERT_EQ(godot::DSPKG_HEADER_SIZE_LEGACY, 28u);
	GDEX_ASSERT_EQ(sizeof(godot::DSPkgHeader), 64u);
	GDEX_ASSERT_EQ(offsetof(godot::DSPkgHeader, compression_codec), 28u);
	GDEX_ASSERT_EQ(offsetof(godot::DSPkgHeader, archive_salt), 32u);
	GDEX_ASSERT_EQ(offsetof(godot::DSPkgHeader, min_reader_version), 48u);
	GDEX_ASSERT_EQ(offsetof(godot::DSPkgHeader, key_id), 52u);
	GDEX_ASSERT_EQ(offsetof(godot::DSPkgHeader, reserved), 60u);
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

GDEX_TEST_T(dspkg, steampipe_1mb_bin_packing, TAG_INTEGRATION) {
	const char *dstex_a = "user://gdextest_binpack_a.dstex";
	const char *dstex_b = "user://gdextest_binpack_b.dstex";
	const char *dstex_c = "user://gdextest_binpack_c.dstex";
	const char *dspkg_path = "user://gdextest_binpack.dspkg";
	const char *res_a = "res://textures/binpack_a.dstex";
	const char *res_b = "res://textures/binpack_b.dstex";
	const char *res_c = "res://textures/binpack_c.dstex";

	const int64_t TEX_SIZE = 400 * 1024; // 400 KB
	if (!write_temp_dstex_incompressible(dstex_a, 512, 200, 1, TEX_SIZE, 0x11111111) ||
			!write_temp_dstex_incompressible(dstex_b, 512, 200, 1, TEX_SIZE, 0x22222222) ||
			!write_temp_dstex_incompressible(dstex_c, 512, 200, 1, TEX_SIZE, 0x33333333)) {
		GDEX_ABORT_TEST("fixture DSTex writes failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	builder->set_payload_alignment(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT);
	builder->set_bin_packing_enabled(true);
	GDEX_EXPECT_TRUE(builder->is_bin_packing_enabled());

	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_a), godot::String(dstex_a)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_b), godot::String(dstex_b)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_c), godot::String(dstex_c)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_path)) == godot::OK);

	godot::Dictionary info_a = reader->get_entry_info(godot::String(res_a));
	godot::Dictionary info_b = reader->get_entry_info(godot::String(res_b));
	godot::Dictionary info_c = reader->get_entry_info(godot::String(res_c));

	int64_t offset_a = info_a.get("data_offset", -1);
	int64_t offset_b = info_b.get("data_offset", -1);
	int64_t offset_c = info_c.get("data_offset", -1);

	const int64_t base_offset = static_cast<int64_t>(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT);
	const int64_t chunk_size = static_cast<int64_t>(godot::DSPkg::STEAMPIPE_1MB_ALIGNMENT);

	// A and B both start in Chunk 0 (< base_offset + 1 MiB) and are 512-byte aligned
	GDEX_EXPECT_EQ(offset_a, base_offset);
	GDEX_EXPECT_EQ((offset_b - base_offset) % 512, int64_t(0));
	GDEX_EXPECT_GT(offset_b, offset_a);
	GDEX_EXPECT_LT(offset_b, base_offset + chunk_size);

	// C crossed the 1 MiB boundary, so it jumped to Chunk 1 (base_offset + 1 MiB)
	GDEX_EXPECT_EQ(offset_c, base_offset + chunk_size);

	// Read textures and verify byte payloads match
	godot::Dictionary tex_a = reader->read_texture(godot::String(res_a));
	godot::Dictionary tex_b = reader->read_texture(godot::String(res_b));
	godot::Dictionary tex_c = reader->read_texture(godot::String(res_c));

	godot::PackedByteArray read_a = tex_a.get("compressed_data", godot::PackedByteArray());
	godot::PackedByteArray read_b = tex_b.get("compressed_data", godot::PackedByteArray());
	godot::PackedByteArray read_c = tex_c.get("compressed_data", godot::PackedByteArray());

	godot::Ref<godot::DSTex> dstex_reader;
	dstex_reader.instantiate();
	godot::PackedByteArray expected_a = dstex_reader->read_from_file(godot::String(dstex_a)).get("compressed_data", godot::PackedByteArray());
	godot::PackedByteArray expected_b = dstex_reader->read_from_file(godot::String(dstex_b)).get("compressed_data", godot::PackedByteArray());
	godot::PackedByteArray expected_c = dstex_reader->read_from_file(godot::String(dstex_c)).get("compressed_data", godot::PackedByteArray());

	GDEX_ASSERT_EQ(read_a.size(), expected_a.size());
	GDEX_ASSERT_EQ(read_b.size(), expected_b.size());
	GDEX_ASSERT_EQ(read_c.size(), expected_c.size());

	GDEX_EXPECT_EQ(memcmp(read_a.ptr(), expected_a.ptr(), expected_a.size()), 0);
	GDEX_EXPECT_EQ(memcmp(read_b.ptr(), expected_b.ptr(), expected_b.size()), 0);
	GDEX_EXPECT_EQ(memcmp(read_c.ptr(), expected_c.ptr(), expected_c.size()), 0);

	remove_temp_file(dstex_a);
	remove_temp_file(dstex_b);
	remove_temp_file(dstex_c);
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

GDEX_TEST_T(dspkg, archive_fingerprint_zero_content_half_guid_only, TAG_INTEGRATION) {
	// A subset export publishes expected = guid << 32 (content half zero)
	// because its whole-project manifest cannot describe the shipped archive.
	// Zero in the content half must mean "pair on the project GUID only".
	const char *dstex_path = "user://gdextest_dspkg_zero_half.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_zero_half.dspkg";
	const char *virtual_path = "res://textures/test_zero_half.dstex";
	const uint64_t guid = 0x12345678ULL;
	const uint64_t archive_fingerprint = (guid << 32) | 0xAABBCCDDULL;
	const uint64_t guid_only = guid << 32;
	const uint64_t wrong_content_half = (guid << 32) | 0x99887766ULL;
	const uint64_t wrong_guid = 0x87654321ULL << 32;

	if (!write_temp_dstex(dstex_path, 64, 4, 1, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), archive_fingerprint) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	// 1. Content half zero: succeeds even though the archive carries a real hash.
	godot::Ref<godot::DSPkg> reader1;
	reader1.instantiate();
	GDEX_ASSERT_TRUE(reader1.is_valid());
	GDEX_EXPECT_EQ(reader1->open(godot::String(dspkg_path), guid_only), godot::OK);
	GDEX_EXPECT_EQ(reader1->get_entry_count(), 1);

	// 2. A non-zero but wrong content half still fails.
	godot::Ref<godot::DSPkg> reader2;
	reader2.instantiate();
	GDEX_ASSERT_TRUE(reader2.is_valid());
	GDEX_EXPECT_EQ(reader2->open(godot::String(dspkg_path), wrong_content_half), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader2->get_entry_count(), 0);

	// 3. The GUID half is enforced even when the content half is zero.
	godot::Ref<godot::DSPkg> reader3;
	reader3.instantiate();
	GDEX_ASSERT_TRUE(reader3.is_valid());
	GDEX_EXPECT_EQ(reader3->open(godot::String(dspkg_path), wrong_guid), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader3->get_entry_count(), 0);

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
	// Header is 64 bytes (v4). Path length is 2 bytes (store_16) + "test.png" (8 bytes).
	// Fixed fields: running_data_offset(8), comp_size(8), orig_size(8), w(4), h(4), fmt(4), mip_count(4)
	fa->seek(64 + 8 + 2 + 8 + 8 + 8 + 8 + 4 + 4 + 4);
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

GDEX_TEST_T(dspkg, encryption_tamper_and_auth, TAG_INTEGRATION) {
	const char *dstex_path = "user://temp_enc_test.dstex";
	const char *dspkg_path = "user://temp_enc_test.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const godot::String wrong_key_hex = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0x1234567890ABCDEFULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder->set_encryption_key(key_hex) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture("test.png", godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	// 1. Valid open with matching global key
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
		GDEX_EXPECT_TRUE(godot::DSPkg::get_global_encryption_key_id() == godot::DSPkg::compute_key_id(key_hex));
	}

	// 2. Open with wrong key -> ERR_UNAUTHORIZED
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(wrong_key_hex);
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);
	}

	// 3. MAC Tamper: alter 1 byte at offset 64
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);
		godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		GDEX_ASSERT_TRUE(fa.is_valid());
		fa->seek(64);
		uint8_t byte = fa->get_8() ^ 0xFF;
		fa->seek(64);
		fa->store_8(byte);
		fa->close();

		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);

		// Restore MAC byte
		fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		fa->seek(64);
		fa->store_8(byte ^ 0xFF);
		fa->close();
	}

	// 4. Key ID mismatch: alter key_id at offset 52
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);
		godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		GDEX_ASSERT_TRUE(fa.is_valid());
		fa->seek(52);
		uint64_t kid = fa->get_64() ^ 0x01ULL;
		fa->seek(52);
		fa->store_64(kid);
		fa->close();

		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);

		// Restore key_id
		fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		fa->seek(52);
		fa->store_64(kid ^ 0x01ULL);
		fa->close();
	}

	// 5. Header AD Tamper: alter archive_fingerprint at offset 20
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);
		godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		GDEX_ASSERT_TRUE(fa.is_valid());
		fa->seek(20);
		uint64_t fp = fa->get_64() ^ 0x01ULL;
		fa->seek(20);
		fa->store_64(fp);
		fa->close();

		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);

		// Restore fingerprint
		fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		fa->seek(20);
		fa->store_64(fp ^ 0x01ULL);
		fa->close();
	}

	// 6. Ciphertext Tamper: alter TOC ciphertext at offset 80
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);
		godot::Ref<godot::FileAccess> fa = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ_WRITE);
		GDEX_ASSERT_TRUE(fa.is_valid());
		fa->seek(80);
		uint8_t byte = fa->get_8() ^ 0xFF;
		fa->seek(80);
		fa->store_8(byte);
		fa->close();

		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);
	}

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, encrypted_archive_fingerprint_matching, TAG_INTEGRATION) {
	const char *dstex_path = "user://temp_enc_fp.dstex";
	const char *dspkg_path = "user://temp_enc_fp.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	const godot::String key_hex = "1111111122222222333333334444444455555555666666667777777788888888";
	const uint64_t expected_fp = 0xAABBCCDDEEFF0011ULL;

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), expected_fp) == godot::OK);
	GDEX_ASSERT_TRUE(builder->set_encryption_key(key_hex) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture("test.png", godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	GlobalKeyResetGuard key_guard("");
	godot::DSPkg::set_global_encryption_key(key_hex);

	// Open with matching fingerprint -> OK
	{
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), expected_fp), godot::OK);
	}

	// Open with mismatching fingerprint -> ERR_FILE_UNRECOGNIZED
	{
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), expected_fp ^ 0x01ULL), godot::ERR_FILE_UNRECOGNIZED);
	}

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, bin_packing_overflow_and_alignment_cap, TAG_INTEGRATION) {
	const char *dstex_path = "user://temp_overflow.dstex";
	const char *dspkg_path = "user://temp_overflow.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	// 16 MiB alignment cap: setting above 16 MiB should be rejected/ignored
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0ULL) == godot::OK);
		builder->set_payload_alignment(32 * 1024 * 1024);
		GDEX_EXPECT_EQ(builder->get_payload_alignment(), 512ULL);
		remove_temp_file(dspkg_path);
	}

	// Bin-packing Chunk #0 overflow: >= 7 entries (TOC envelope > 512 B)
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0ULL) == godot::OK);
		builder->set_bin_packing_enabled(true);
		builder->set_payload_alignment(512);

		for (int i = 0; i < 7; ++i) {
			godot::String name = "texture_" + godot::String::num_int64(i) + ".png";
			GDEX_ASSERT_TRUE(builder->add_texture(name, godot::String(dstex_path)) == godot::OK);
		}
		// Finalize must fail ERR_INVALID_PARAMETER because TOC envelope exceeds payload_alignment
		GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_INVALID_PARAMETER);
		// Output file must not exist
		godot::Ref<godot::DirAccess> da = godot::DirAccess::open("user://");
		if (da.is_valid()) {
			GDEX_EXPECT_FALSE(da->file_exists(godot::String(dspkg_path).get_file()));
		}
	}

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
GDEX_TEST_T(dspkg, steampipe_1mib_chunk_isolation, TAG_INTEGRATION) {
	const char *dstex_path1 = "user://temp_chunk_1.dstex";
	const char *dstex_path2 = "user://temp_chunk_2.dstex";
	const char *dstex_path3_a = "user://temp_chunk_3a.dstex";
	const char *dstex_path3_b = "user://temp_chunk_3b.dstex";
	const char *dstex_path4 = "user://temp_chunk_4.dstex";
	const char *dstex_path5 = "user://temp_chunk_5.dstex";
	const char *dspkg_v1 = "user://temp_chunk_v1.dspkg";
	const char *dspkg_v2 = "user://temp_chunk_v2.dspkg";

	remove_temp_file(dstex_path1);
	remove_temp_file(dstex_path2);
	remove_temp_file(dstex_path3_a);
	remove_temp_file(dstex_path3_b);
	remove_temp_file(dstex_path4);
	remove_temp_file(dstex_path5);
	remove_temp_file(dspkg_v1);
	remove_temp_file(dspkg_v2);

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path1, 64, 64, 1, 64 * 64 * 4));
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path2, 64, 64, 1, 64 * 64 * 4));
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path3_a, 64, 64, 1, 64 * 64 * 4));
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path3_b, 64, 64, 1, 64 * 64 * 4));
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path4, 64, 64, 1, 64 * 64 * 4));
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path5, 64, 64, 1, 64 * 64 * 4));

	const godot::String key_hex = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
	godot::PackedByteArray fixed_salt;
	fixed_salt.resize(16);
	for (int i = 0; i < 16; ++i) {
		fixed_salt[i] = static_cast<uint8_t>(i + 1);
	}

	// Build V1
	{
		godot::Ref<godot::DSPkg> b1;
		b1.instantiate();
		GDEX_ASSERT_TRUE(b1.is_valid());
		GDEX_ASSERT_TRUE(b1->create(godot::String(dspkg_v1), 0ULL) == godot::OK);
		b1->set_bin_packing_enabled(false);
		b1->set_payload_alignment(1048576);
		GDEX_ASSERT_TRUE(b1->set_encryption_key(key_hex) == godot::OK);
		b1->_set_archive_salt_for_testing(fixed_salt);
		GDEX_ASSERT_TRUE(b1->add_texture("tex1.png", godot::String(dstex_path1)) == godot::OK);
		GDEX_ASSERT_TRUE(b1->add_texture("tex2.png", godot::String(dstex_path2)) == godot::OK);
		GDEX_ASSERT_TRUE(b1->add_texture("tex3.png", godot::String(dstex_path3_a)) == godot::OK);
		GDEX_ASSERT_TRUE(b1->add_texture("tex4.png", godot::String(dstex_path4)) == godot::OK);
		GDEX_ASSERT_TRUE(b1->add_texture("tex5.png", godot::String(dstex_path5)) == godot::OK);
		GDEX_ASSERT_TRUE(b1->finalize() == godot::OK);
	}

	// Build V2 (tex3 modified)
	{
		godot::Ref<godot::DSPkg> b2;
		b2.instantiate();
		GDEX_ASSERT_TRUE(b2.is_valid());
		GDEX_ASSERT_TRUE(b2->create(godot::String(dspkg_v2), 0ULL) == godot::OK);
		b2->set_bin_packing_enabled(false);
		b2->set_payload_alignment(1048576);
		GDEX_ASSERT_TRUE(b2->set_encryption_key(key_hex) == godot::OK);
		b2->_set_archive_salt_for_testing(fixed_salt);
		GDEX_ASSERT_TRUE(b2->add_texture("tex1.png", godot::String(dstex_path1)) == godot::OK);
		GDEX_ASSERT_TRUE(b2->add_texture("tex2.png", godot::String(dstex_path2)) == godot::OK);
		GDEX_ASSERT_TRUE(b2->add_texture("tex3.png", godot::String(dstex_path3_b)) == godot::OK);
		GDEX_ASSERT_TRUE(b2->add_texture("tex4.png", godot::String(dstex_path4)) == godot::OK);
		GDEX_ASSERT_TRUE(b2->add_texture("tex5.png", godot::String(dstex_path5)) == godot::OK);
		GDEX_ASSERT_TRUE(b2->finalize() == godot::OK);
	}

	godot::PackedByteArray v1_bytes = godot::FileAccess::get_file_as_bytes(godot::String(dspkg_v1));
	godot::PackedByteArray v2_bytes = godot::FileAccess::get_file_as_bytes(godot::String(dspkg_v2));
	GDEX_ASSERT_TRUE(v1_bytes.size() >= 5 * 1048576);
	GDEX_ASSERT_TRUE(v2_bytes.size() >= 5 * 1048576);

	// Check that unmodified chunk slices match exactly
	for (int64_t offset : {1048576LL, 2097152LL, 4194304LL}) {
		if (offset + 1024 <= v1_bytes.size() && offset + 1024 <= v2_bytes.size()) {
			GDEX_EXPECT_TRUE(v1_bytes.slice(offset, offset + 1024) == v2_bytes.slice(offset, offset + 1024));
		}
	}

	// Suite 4 steps 4-6: Runtime test under GlobalKeyResetGuard + wrong-key negative control
	{
		GlobalKeyResetGuard key_guard("");
		godot::DSPkg::set_global_encryption_key(key_hex);

		godot::Ref<godot::DSPkg> r1;
		r1.instantiate();
		GDEX_ASSERT_TRUE(r1.is_valid());
		GDEX_EXPECT_EQ(r1->open(godot::String(dspkg_v1), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(r1->get_entry_count(), 5);

		godot::Ref<godot::DSPkg> r2;
		r2.instantiate();
		GDEX_ASSERT_TRUE(r2.is_valid());
		GDEX_EXPECT_EQ(r2->open(godot::String(dspkg_v2), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(r2->get_entry_count(), 5);

		// Negative control: wrong key fails with ERR_UNAUTHORIZED
		const godot::String wrong_key_hex = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";
		godot::DSPkg::set_global_encryption_key(wrong_key_hex);
		godot::Ref<godot::DSPkg> r_bad;
		r_bad.instantiate();
		GDEX_ASSERT_TRUE(r_bad.is_valid());
		GDEX_EXPECT_EQ(r_bad->open(godot::String(dspkg_v1), 0ULL), godot::ERR_UNAUTHORIZED);
	}

	remove_temp_file(dstex_path1);
	remove_temp_file(dstex_path2);
	remove_temp_file(dstex_path3_a);
	remove_temp_file(dstex_path3_b);
	remove_temp_file(dstex_path4);
	remove_temp_file(dstex_path5);
	remove_temp_file(dspkg_v1);
	remove_temp_file(dspkg_v2);
}
#endif


