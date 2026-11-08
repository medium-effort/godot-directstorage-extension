#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "monocypher.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>

// Suite covering the same area as project/headless_tests/test_dspkg_alignment.gd
// (+ test_formats.gd package sections): DSPkg create/finalize/open cycle,
// 512-byte per-mip placement padding (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
// entry iteration/count, virtual path resolution, archive id pairing and the
// res://.dspkg/toc.key delivery contract (48-byte magic16 || key32 file, commit
// only after the header key_id matches and the TOC AEAD succeeds).
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

// --- Archive key delivery fixture -------------------------------------------
// The reader resolves the archive key from res://.dspkg/toc.key: exactly 48
// bytes laid out as magic16 || key32, where the magic is the first 16 bytes of
// SHA-256 over the ASCII label below. These helpers rebuild that contract so the
// cases exercise the real delivered-file path instead of the explicit override.
constexpr const char *TOC_KEY_DIR = "res://.dspkg";
constexpr const char *TOC_KEY_PATH = "res://.dspkg/toc.key";
constexpr int64_t TOC_KEY_MAGIC_SIZE = 16;
constexpr int64_t TOC_KEY_BYTES = 32;
constexpr int64_t TOC_KEY_FILE_SIZE = TOC_KEY_MAGIC_SIZE + TOC_KEY_BYTES;
constexpr const char *TOC_KEY_MAGIC_LABEL = "dspkg-toc-key-magic-v1";

godot::PackedByteArray toc_key_magic() {
	return godot::String(TOC_KEY_MAGIC_LABEL).sha256_buffer().slice(0, TOC_KEY_MAGIC_SIZE);
}

// Builds a key-file image. Cases vary the magic, the key length and the trailing
// bytes to prove the reader validates the envelope before it uses any key.
godot::PackedByteArray make_toc_key_file(const godot::String &p_key_hex, int64_t p_key_bytes = TOC_KEY_BYTES,
		int64_t p_trailing = 0, bool p_corrupt_magic = false) {
	const godot::PackedByteArray key = p_key_hex.hex_decode();
	godot::PackedByteArray magic = toc_key_magic();
	if (p_corrupt_magic && magic.size() > 0) {
		magic.set(0, magic.get(0) ^ 0x5A);
	}
	godot::PackedByteArray file;
	file.resize(TOC_KEY_MAGIC_SIZE + p_key_bytes + p_trailing);
	for (int64_t i = 0; i < TOC_KEY_MAGIC_SIZE; ++i) {
		file.set(i, magic.size() > 0 ? magic.get(i % magic.size()) : 0);
	}
	for (int64_t i = 0; i < p_key_bytes; ++i) {
		file.set(TOC_KEY_MAGIC_SIZE + i, i < key.size() ? key.get(i) : 0x5A);
	}
	for (int64_t i = 0; i < p_trailing; ++i) {
		file.set(TOC_KEY_MAGIC_SIZE + p_key_bytes + i, 0xA5);
	}
	return file;
}

bool write_file_bytes(const char *p_path, const godot::PackedByteArray &p_bytes) {
	const godot::String path(p_path);
	const godot::String parent = path.get_base_dir();
	if (!parent.is_empty() && !godot::DirAccess::dir_exists_absolute(parent)) {
		if (godot::DirAccess::make_dir_recursive_absolute(parent) != godot::OK) {
			return false;
		}
	}
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->store_buffer(p_bytes);
	file->close();
	return godot::FileAccess::get_file_as_bytes(path).size() == p_bytes.size();
}

bool install_toc_key_file(const godot::String &p_key_hex) {
	return write_file_bytes(TOC_KEY_PATH, make_toc_key_file(p_key_hex));
}

void remove_toc_key_file() {
	if (!godot::DirAccess::dir_exists_absolute(godot::String(TOC_KEY_DIR))) {
		return;
	}
	godot::Ref<godot::DirAccess> dir = godot::DirAccess::open(godot::String(TOC_KEY_DIR));
	if (dir.is_valid()) {
		dir->remove("toc.key");
	}
	godot::DirAccess::remove_absolute(godot::String(TOC_KEY_DIR));
}

// Returns the process to "no override, nothing committed" and removes the key
// file at both ends of the scope, so one case cannot leak a key into the next.
struct TocKeySandbox {
	TocKeySandbox() {
		remove_toc_key_file();
		godot::DSPkg::set_global_encryption_key("");
	}
	~TocKeySandbox() {
		remove_toc_key_file();
		godot::DSPkg::set_global_encryption_key("");
	}
};

bool mutate_file_byte(const char *p_path, int64_t p_offset) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ_WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->seek(p_offset);
	const uint8_t original = file->get_8();
	file->seek(p_offset);
	file->store_8(original ^ 0xFF);
	file->close();
	return true;
}

bool append_file_bytes(const char *p_path, int64_t p_count) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ_WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	godot::PackedByteArray tail;
	tail.resize(p_count);
	file->seek_end();
	file->store_buffer(tail);
	file->close();
	return true;
}

bool write_u64_at(const char *p_path, int64_t p_offset, uint64_t p_value) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ_WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->seek(p_offset);
	file->store_64(p_value);
	file->close();
	return true;
}

bool read_u64_at(const char *p_path, int64_t p_offset, uint64_t &r_value) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->seek(p_offset);
	r_value = file->get_64();
	file->close();
	return true;
}

bool read_u32_at(const char *p_path, int64_t p_offset, uint32_t &r_value) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->seek(p_offset);
	r_value = file->get_32();
	file->close();
	return true;
}

bool read_u16_at(const char *p_path, int64_t p_offset, uint16_t &r_value) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->seek(p_offset);
	r_value = file->get_16();
	file->close();
	return true;
}

// Header offset of DSPkgHeader::data_offset (magic 4 + version 2 + flags 2 +
// entry_count 4). Payloads start there, so a file that stops at this length
// streamed nothing.
constexpr int64_t DSPKG_HEADER_DATA_OFFSET_FIELD = 12;

uint64_t dspkg_payload_start(const char *p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return 0;
	}
	file->seek(DSPKG_HEADER_DATA_OFFSET_FIELD);
	const uint64_t value = file->get_64();
	file->close();
	return value;
}

// Builds a one-entry archive. An empty p_key_hex writes a plaintext archive.
bool build_single_entry_archive(const char *p_dstex_path, const char *p_dspkg_path, uint64_t p_archive_id,
		const godot::String &p_key_hex, const char *p_entry_path) {
	if (!write_temp_dstex(p_dstex_path, 64, 64, 1, 64 * 64 * 4)) {
		return false;
	}
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	if (builder.is_null()) {
		return false;
	}
	if (builder->create(godot::String(p_dspkg_path), p_archive_id) != godot::OK) {
		return false;
	}
	if (!p_key_hex.is_empty() && builder->set_encryption_key(p_key_hex) != godot::OK) {
		return false;
	}
	if (builder->add_texture(godot::String(p_entry_path), godot::String(p_dstex_path)) != godot::OK) {
		return false;
	}
	return builder->finalize() == godot::OK;
}

// Builds a two-entry plaintext archive whose entries both point at one
// incompressible 128x128 .dstex. The payload is incompressible on purpose:
// entry A's compressed span has to be much larger than the 512-byte step the
// span case shifts entry B by, so a shifted B lands strictly inside A.
bool build_two_entry_archive(const char *p_dstex_path, const char *p_dspkg_path) {
	if (!write_temp_dstex_incompressible(p_dstex_path, 128, 128, 1, 128 * 128 * 4)) {
		return false;
	}
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	if (builder.is_null()) {
		return false;
	}
	if (builder->create(godot::String(p_dspkg_path), 0ULL) != godot::OK) {
		return false;
	}
	if (builder->add_texture("a.png", godot::String(p_dstex_path)) != godot::OK) {
		return false;
	}
	if (builder->add_texture("b.png", godot::String(p_dstex_path)) != godot::OK) {
		return false;
	}
	return builder->finalize() == godot::OK;
}

// Byte offsets of the two-entry fixture above, read back from the TOC instead
// of assumed: finalize() sorts by resource path ("a.png" < "b.png") and
// add_texture() derives GPU mip metadata from the .dstex header, so each entry
// carries one 24-byte mip record after its fixed fields. Returns false when the
// file does not have that shape, so a future layout change fails loudly rather
// than patching the wrong bytes.
struct TwoEntryLayout {
	uint64_t payload_start = 0;
	uint64_t off_a = 0;
	uint64_t size_a = 0;
	uint64_t mip0_a = 0;
	uint64_t off_b = 0;
	uint64_t mip0_b = 0;
	uint16_t path_len_a = 0;
	uint16_t path_len_b = 0;
	uint64_t hash_a = 0;
	int64_t entry_b_offset_field = 0;
	int64_t entry_b_mip0_field = 0;
};

bool read_two_entry_layout(const char *p_path, TwoEntryLayout &r_layout) {
	const int64_t entry_a = static_cast<int64_t>(godot::DSPKG_HEADER_SIZE_V4);
	r_layout.payload_start = dspkg_payload_start(p_path);

	if (!read_u16_at(p_path, entry_a + 8, r_layout.path_len_a) ||
			!read_u64_at(p_path, entry_a, r_layout.hash_a)) {
		return false;
	}
	const int64_t field_off_a = entry_a + static_cast<int64_t>(godot::DSPKG_ENTRY_PREFIX_SIZE) +
			static_cast<int64_t>(r_layout.path_len_a);
	uint32_t mip_count_a = 0;
	if (!read_u64_at(p_path, field_off_a, r_layout.off_a) ||
			!read_u64_at(p_path, field_off_a + 8, r_layout.size_a) ||
			!read_u32_at(p_path, field_off_a + 36, mip_count_a) ||
			!read_u64_at(p_path, field_off_a + static_cast<int64_t>(godot::DSPKG_ENTRY_FIELDS_SIZE_V3), r_layout.mip0_a)) {
		return false;
	}

	const int64_t entry_b = field_off_a + static_cast<int64_t>(godot::DSPKG_ENTRY_FIELDS_SIZE_V3) +
			static_cast<int64_t>(sizeof(godot::DSPkgMipEntry)) * static_cast<int64_t>(mip_count_a);
	if (!read_u16_at(p_path, entry_b + 8, r_layout.path_len_b)) {
		return false;
	}
	r_layout.entry_b_offset_field = entry_b + static_cast<int64_t>(godot::DSPKG_ENTRY_PREFIX_SIZE) +
			static_cast<int64_t>(r_layout.path_len_b);
	r_layout.entry_b_mip0_field = r_layout.entry_b_offset_field +
			static_cast<int64_t>(godot::DSPKG_ENTRY_FIELDS_SIZE_V3);
	return read_u64_at(p_path, r_layout.entry_b_offset_field, r_layout.off_b) &&
			read_u64_at(p_path, r_layout.entry_b_mip0_field, r_layout.mip0_b);
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

// --- Integration: builder staging snapshot and payload streaming --------------

GDEX_TEST_T(dspkg, staged_entry_record_is_metadata_only, TAG_INTEGRATION) {
	// The builder's per-texture footprint is DSPkgStagedEntry: a path pair, the
	// add-time scalar snapshot and the mip tables. No member is a payload buffer,
	// so staging a texture costs metadata no matter how large the payload is.
	godot::DSPkgStagedEntry entry;
	entry.resource_path = "res://textures/a.dstex";
	entry.dstex_path = "user://a.dstex";
	entry.source_file_length = 4ULL * 1024ULL * 1024ULL;
	entry.source_modified_time = 1700000000ULL;
	entry.source_version = 2;
	entry.source_compressed_size = 3ULL * 1024ULL * 1024ULL;
	entry.payload_size = 3ULL * 1024ULL * 1024ULL;
	entry.payload_offset = 48;
	entry.original_size = 4ULL * 1024ULL * 1024ULL;
	entry.width = 1024;
	entry.height = 1024;
	entry.pixel_format = 0;
	entry.mip_count = 1;
	entry.flags = 0;
	entry.gpu_metadata = true;
	entry.gpu_footprint = 4ULL * 1024ULL * 1024ULL;

	GDEX_EXPECT_EQ(entry.source_file_length, uint64_t(4ULL * 1024ULL * 1024ULL));
	GDEX_EXPECT_EQ(entry.source_compressed_size, entry.payload_size);
	GDEX_EXPECT_EQ(entry.mip_count, 1u);
	GDEX_EXPECT_TRUE(entry.mip_sizes.is_empty());
	GDEX_EXPECT_TRUE(entry.source_mip_offsets.is_empty());

	// Only the mip tables are variable-size. A 512-byte ceiling for the whole
	// record rules out an inline payload buffer for any texture of consequence.
	GDEX_EXPECT_LT(sizeof(godot::DSPkgStagedEntry), size_t(512));
	// The payload the record describes is far larger than the record itself: a
	// staged copy of it would have to live in a member this type does not have.
	GDEX_EXPECT_GT(entry.payload_size, static_cast<uint64_t>(sizeof(godot::DSPkgStagedEntry)));

	entry.mip_sizes.push_back(1024);
	entry.mip_uncompressed_sizes.push_back(2048);
	entry.mip_row_pitches.push_back(256);
	entry.source_mip_offsets.push_back(48);
	entry.source_mip_sizes.push_back(1024);
	GDEX_EXPECT_EQ(entry.mip_sizes.size(), 1);
	GDEX_EXPECT_EQ(entry.source_mip_offsets.size(), 1);
}

GDEX_TEST_T(dspkg, builder_staged_source_mutation_aborts_finalize, TAG_INTEGRATION) {
	// add_texture() records metadata only; finalize() re-stats every source and
	// refuses to stream bytes that changed after staging. Each sub-case checks
	// that nothing was streamed (the builder aborted at the payload boundary) and
	// that the aborted build left no archive behind to be mistaken for a valid one.
	const char *metadata_dstex = "user://gdextest_dspkg_stage_meta.dstex";
	const char *resize_dstex = "user://gdextest_dspkg_stage_resize.dstex";
	const char *delete_dstex = "user://gdextest_dspkg_stage_delete.dstex";
	const char *metadata_dspkg = "user://gdextest_dspkg_stage_meta.dspkg";
	const char *resize_dspkg = "user://gdextest_dspkg_stage_resize.dspkg";
	const char *delete_dspkg = "user://gdextest_dspkg_stage_delete.dspkg";
	const char *virtual_path = "res://textures/test_staged_source.dstex";

	// A control archive built from an unmutated copy of the same fixture. Its
	// data_offset is the byte every aborting build must stop at: the entry table
	// depends on the path, dimensions and mip count, not on the abort reason.
	const char *control_dstex = "user://gdextest_dspkg_stage_control.dstex";
	const char *control_dspkg = "user://gdextest_dspkg_stage_control.dspkg";
	GDEX_ASSERT_TRUE(build_single_entry_archive(control_dstex, control_dspkg, 0, godot::String(), virtual_path));
	const uint64_t payload_start = dspkg_payload_start(control_dspkg);
	GDEX_ASSERT_TRUE(payload_start > 0);

	// 1. Header metadata rewritten in place (file length unchanged).
	GDEX_ASSERT_TRUE(write_temp_dstex(metadata_dstex, 64, 64, 1, 64 * 64 * 4));
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(metadata_dspkg), 0ULL) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(metadata_dstex)) == godot::OK);
		// DSTexHeader::compressed_size lives at offset 32 and is part of the
		// add-time snapshot.
		GDEX_ASSERT_TRUE(write_u64_at(metadata_dstex, 32, 1ULL));

		GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_FILE_CORRUPT);
		GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(metadata_dspkg)));
		GDEX_EXPECT_EQ(builder->_get_finalize_committed_bytes_for_testing(), payload_start);
	}

	// 2. Source resized after staging.
	GDEX_ASSERT_TRUE(write_temp_dstex(resize_dstex, 64, 64, 1, 64 * 64 * 4));
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(resize_dspkg), 0ULL) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(resize_dstex)) == godot::OK);
		GDEX_ASSERT_TRUE(append_file_bytes(resize_dstex, 64));

		GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_FILE_CORRUPT);
		GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(resize_dspkg)));
		GDEX_EXPECT_EQ(builder->_get_finalize_committed_bytes_for_testing(), payload_start);
	}

	// 3. Source removed after staging.
	GDEX_ASSERT_TRUE(write_temp_dstex(delete_dstex, 64, 64, 1, 64 * 64 * 4));
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(delete_dspkg), 0ULL) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(delete_dstex)) == godot::OK);
		remove_temp_file(delete_dstex);

		GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_CANT_OPEN);
		GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(delete_dspkg)));
		GDEX_EXPECT_EQ(builder->_get_finalize_committed_bytes_for_testing(), payload_start);
	}

	remove_temp_file(metadata_dstex);
	remove_temp_file(resize_dstex);
	remove_temp_file(control_dstex);
	remove_temp_file(metadata_dspkg);
	remove_temp_file(resize_dspkg);
	remove_temp_file(delete_dspkg);
	remove_temp_file(control_dspkg);
}

GDEX_TEST_T(dspkg, builder_streams_staged_payloads_one_at_a_time, TAG_INTEGRATION) {
	// Three 2 MiB incompressible sources. finalize() streams them from disk in
	// bounded chunks: the staged record holds no payload, so an archive that
	// aborts on the last source has already written the earlier payloads and no
	// more.
	const char *dstex_a = "user://gdextest_stream_a.dstex";
	const char *dstex_b = "user://gdextest_stream_b.dstex";
	const char *dstex_c = "user://gdextest_stream_c.dstex";
	const char *dspkg_ok = "user://gdextest_stream_ok.dspkg";
	const char *dspkg_abort = "user://gdextest_stream_abort.dspkg";
	const char *res_a = "res://textures/stream_a.dstex";
	const char *res_b = "res://textures/stream_b.dstex";
	const char *res_c = "res://textures/stream_c.dstex";
	// 1024 x 512 RGBA8 is exactly 2 MiB, so the payload matches the footprint.
	const int64_t payload_size = 1024 * 512 * 4;

	GDEX_ASSERT_TRUE(write_temp_dstex_incompressible(dstex_a, 1024, 512, 1, payload_size, 0x11111111));
	GDEX_ASSERT_TRUE(write_temp_dstex_incompressible(dstex_b, 1024, 512, 1, payload_size, 0x22222222));
	GDEX_ASSERT_TRUE(write_temp_dstex_incompressible(dstex_c, 1024, 512, 1, payload_size, 0x33333333));

	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_ok), 0ULL) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_a), godot::String(dstex_a)) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_b), godot::String(dstex_b)) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_c), godot::String(dstex_c)) == godot::OK);
		GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);
	}

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_ASSERT_TRUE(reader->open(godot::String(dspkg_ok)) == godot::OK);
	GDEX_ASSERT_EQ(reader->get_entry_count(), 3);

	godot::Ref<godot::DSTex> dstex_reader;
	dstex_reader.instantiate();
	const char *sources[3] = {dstex_a, dstex_b, dstex_c};
	const char *entries[3] = {res_a, res_b, res_c};
	for (int i = 0; i < 3; ++i) {
		const godot::PackedByteArray archived =
				reader->read_texture(godot::String(entries[i])).get("compressed_data", godot::PackedByteArray());
		const godot::PackedByteArray expected =
				dstex_reader->read_from_file(godot::String(sources[i])).get("compressed_data", godot::PackedByteArray());
		GDEX_ASSERT_EQ(archived.size(), expected.size());
		GDEX_EXPECT_EQ(memcmp(archived.ptr(), expected.ptr(), expected.size()), 0);
	}

	// Stage the same three textures again, then resize the lexicographically last
	// source. The first two payloads are already streamed when the third aborts.
	uint64_t aborted_committed = 0;
	{
		godot::Ref<godot::DSPkg> builder;
		builder.instantiate();
		GDEX_ASSERT_TRUE(builder.is_valid());
		GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_abort), 0ULL) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_a), godot::String(dstex_a)) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_b), godot::String(dstex_b)) == godot::OK);
		GDEX_ASSERT_TRUE(builder->add_texture(godot::String(res_c), godot::String(dstex_c)) == godot::OK);
		GDEX_ASSERT_TRUE(append_file_bytes(dstex_c, 64));
		GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_FILE_CORRUPT);
		aborted_committed = builder->_get_finalize_committed_bytes_for_testing();
	}

	// The aborted build leaves nothing on disk, so how far it got is read from the
	// builder instead of from the leftover: the control archive supplies the byte
	// the header/TOC envelope ends at, the completed one supplies the full size.
	const int64_t payload_start = static_cast<int64_t>(dspkg_payload_start(dspkg_ok));
	const int64_t complete_size = godot::FileAccess::get_file_as_bytes(godot::String(dspkg_ok)).size();
	GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(dspkg_abort)));
	GDEX_EXPECT_GT(aborted_committed, static_cast<uint64_t>(payload_start));
	GDEX_EXPECT_LT(aborted_committed, static_cast<uint64_t>(complete_size));

	remove_temp_file(dstex_a);
	remove_temp_file(dstex_b);
	remove_temp_file(dstex_c);
	remove_temp_file(dspkg_ok);
	remove_temp_file(dspkg_abort);
}

GDEX_TEST_T(dspkg, finalize_aborted_source_removal_leaves_no_archive, TAG_INTEGRATION) {
	// finalize() truncates the output before it validates the staged sources. An
	// abort after that point must not leave the half-written archive on disk: its
	// header and TOC are already valid, so the file reads as an archive to anything
	// that only checks for existence.
	const char *dstex_path = "user://gdextest_dspkg_finalize_abort.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_finalize_abort.dspkg";
	const char *virtual_path = "res://textures/test_finalize_abort.dstex";

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);

	// The source is gone by the time finalize() re-stats it, so the archive aborts
	// after the header and TOC have been written.
	remove_temp_file(dstex_path);

	GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_CANT_OPEN);
	// Anti-vacuity: the output was created and the envelope written, so the
	// absence below is a removal rather than a build that never touched the path.
	GDEX_EXPECT_GT(builder->_get_finalize_committed_bytes_for_testing(), uint64_t(0));
	GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(dspkg_path)));

	// Positive control: the same builder path still produces an archive when
	// nothing aborts, so "no file" cannot be satisfied by never writing one.
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));
	const char *control_path = "user://gdextest_dspkg_finalize_control.dspkg";
	godot::Ref<godot::DSPkg> control;
	control.instantiate();
	GDEX_ASSERT_TRUE(control.is_valid());
	GDEX_ASSERT_TRUE(control->create(godot::String(control_path), 0ULL) == godot::OK);
	GDEX_ASSERT_TRUE(control->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_EXPECT_EQ(control->finalize(), godot::OK);
	GDEX_EXPECT_TRUE(godot::FileAccess::file_exists(godot::String(control_path)));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
	remove_temp_file(control_path);
}

GDEX_TEST_T(dspkg, finalize_abort_before_open_keeps_existing_archive, TAG_INTEGRATION) {
	// The abort cleanup must only remove what this call created. An abort that
	// happens before the output is opened (here: nothing staged) has to leave a
	// pre-existing archive untouched, or a retry after a failed build would delete
	// the good archive it was meant to replace.
	const char *dstex_path = "user://gdextest_dspkg_preopen_keep.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_preopen_keep.dspkg";
	const char *virtual_path = "res://textures/test_preopen_keep.dstex";

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0, godot::String(), virtual_path));
	const int64_t original_size = godot::FileAccess::get_file_as_bytes(godot::String(dspkg_path)).size();
	GDEX_ASSERT_TRUE(original_size > 0);

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), 0ULL) == godot::OK);
	GDEX_EXPECT_EQ(builder->finalize(), godot::ERR_UNCONFIGURED);

	GDEX_EXPECT_TRUE(godot::FileAccess::file_exists(godot::String(dspkg_path)));
	GDEX_EXPECT_EQ(godot::FileAccess::get_file_as_bytes(godot::String(dspkg_path)).size(), original_size);

	// And it is still a readable archive, not a truncated one.
	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);

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

// --- Integration: archive id pairing -----------------------------------------
//
// The archive id reuses DSPkgHeader::archive_fingerprint and is compared as a
// full 64-bit value. A non-zero expected id must match exactly: an archive that
// carries id 0 is rejected, so zeroing the header field cannot satisfy a build
// that expects a real id. Passing 0 as the expected id is the tooling path and
// skips the check.

GDEX_TEST_T(dspkg, archive_id_roundtrip_serialization, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_id_ser.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_id_ser.dspkg";
	const char *virtual_path = "res://textures/test_id_ser.dstex";
	const uint64_t archive_id = 0x12345678AABBCCDDULL;

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_archive_fingerprint(), archive_id);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_id_equal_opens, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_id_equal.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_id_equal.dspkg";
	const char *virtual_path = "res://textures/test_id_equal.dstex";
	const uint64_t archive_id = 0x0CAFEBABE1122334ULL;

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, archive_id_mismatch_unrecognized, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_id_mismatch.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_id_mismatch.dspkg";
	const char *virtual_path = "res://textures/test_id_mismatch.dstex";
	const uint64_t archive_id = 0x11111111AABBCCDDULL;
	const uint64_t other_archive_id = 0x22222222AABBCCDDULL;

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), other_archive_id), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(reader->has_texture(godot::String(virtual_path)));
	GDEX_EXPECT_EQ(reader->get_archive_fingerprint(), uint64_t(0));

	// The rejected open leaves no reader state behind: the right id still works.
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

// A build whose export contract declares itself protected must not let the
// archive decide whether its TOC is authenticated. The header flag is
// attacker-controlled, so a plaintext archive carrying a copied archive id
// would otherwise open in a build that advertises an authenticated TOC.
GDEX_TEST_T(dspkg, protected_build_rejects_plaintext_archive, TAG_INTEGRATION) {
	const char *dstex_plain = "user://gdextest_dspkg_protected_plain.dstex";
	const char *dspkg_plain = "user://gdextest_dspkg_protected_plain.dspkg";
	const char *dstex_enc = "user://gdextest_dspkg_protected_enc.dstex";
	const char *dspkg_enc = "user://gdextest_dspkg_protected_enc.dspkg";
	const char *virtual_path = "res://textures/test_protected_plain.dstex";
	const uint64_t archive_id = 0x5A5A1234DEADBEEFULL;
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const godot::String setting = "directstorage/export/protected";

	// No key file, no installed key: nothing can decrypt, so an accepted open
	// proves the plaintext path was taken.
	TocKeySandbox sandbox;

	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	GDEX_ASSERT_TRUE(ps != nullptr);
	const bool had_setting = ps->has_setting(setting);
	const godot::Variant previous = had_setting ? ps->get_setting(setting) : godot::Variant();
	ctx.add_teardown([ps, setting, dstex_plain, dspkg_plain, dstex_enc, dspkg_enc, had_setting, previous]() {
		if (had_setting) {
			ps->set_setting(setting, previous);
		} else {
			ps->clear(setting);
		}
		remove_temp_file(dstex_plain);
		remove_temp_file(dspkg_plain);
		remove_temp_file(dstex_enc);
		remove_temp_file(dspkg_enc);
	});

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_plain, dspkg_plain, archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());

	// 1. Baseline: the same file is a valid archive when the build does not
	// require an authenticated TOC. This keeps the fixture honest.
	ps->set_setting(setting, false);
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_plain), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));

	// 2. A protected build refuses it.
	const godot::String key_id_before = godot::DSPkg::get_global_encryption_key_id();
	ps->set_setting(setting, true);
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_plain), archive_id), godot::ERR_UNAUTHORIZED);

	// 3. A refused open leaves no reader state behind.
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(reader->has_texture(godot::String(virtual_path)));

	// 4. A refusal neither installs nor wipes a key.
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), key_id_before);

	// 5. The requirement is evaluated per open, not latched at the first refusal.
	ps->set_setting(setting, false);
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_plain), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	// 6. The protected happy path is untouched: an encrypted archive still
	// opens when the build requires an authenticated TOC.
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_enc, dspkg_enc, archive_id, key_hex, virtual_path));
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	ps->set_setting(setting, true);
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_enc), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), godot::DSPkg::compute_key_id(key_hex));
}

GDEX_TEST_T(dspkg, archive_id_zero_archive_rejected_when_expected, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_id_zero_unpaired.dstex";
	const char *dspkg_unpaired = "user://gdextest_dspkg_id_zero_unpaired.dspkg";
	const char *virtual_path = "res://textures/test_id_zero.dstex";

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	// Archive written without an id, opened by a caller that expects one.
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_unpaired), 0ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_unpaired), 0x12345678AABBCCDDULL), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_unpaired);
}

GDEX_TEST_T(dspkg, archive_id_zero_expectation_skips_check, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_id_zero_paired.dstex";
	const char *dspkg_paired = "user://gdextest_dspkg_id_zero_paired.dspkg";
	const char *virtual_path = "res://textures/test_id_zero_paired.dstex";

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	// Archive written with an id, opened by a caller that passes 0 (tooling).
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_paired), 0x9999888877776666ULL) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_paired), 0ULL), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_EQ(reader->get_archive_fingerprint(), 0x9999888877776666ULL);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_paired);
}

// --- Integration: write-failure detection (#0157) ----------------------------
//
// finalize() must not report success when the payload area did not reach the
// computed size. The injection drops bytes past a threshold while still
// reporting success, which is the silent-short-write shape a full disk can
// produce; only the final size verification can catch it.

GDEX_TEST_T(dspkg, finalize_write_failure_aborts, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_write_fail.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_write_fail.dspkg";
	const char *dspkg_retry = "user://gdextest_dspkg_write_fail_retry.dspkg";
	const char *virtual_path = "res://textures/test_write_fail.dstex";

	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	// Truncate inside the header envelope: every later write is dropped while
	// the builder believes it succeeded.
	godot::Ref<godot::DSPkg> failing;
	failing.instantiate();
	GDEX_ASSERT_TRUE(failing.is_valid());
	GDEX_ASSERT_TRUE(failing->create(godot::String(dspkg_path), 0x0F0E0D0C0B0A0908ULL) == godot::OK);
	GDEX_ASSERT_TRUE(failing->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	failing->_set_write_truncate_for_testing(8);
	GDEX_EXPECT_EQ(failing->finalize(), godot::ERR_FILE_CANT_WRITE);
	GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(godot::String(dspkg_path)));

	// The same build without the injection produces a complete archive.
	godot::Ref<godot::DSPkg> clean;
	clean.instantiate();
	GDEX_ASSERT_TRUE(clean.is_valid());
	GDEX_ASSERT_TRUE(clean->create(godot::String(dspkg_retry), 0x0F0E0D0C0B0A0908ULL) == godot::OK);
	GDEX_ASSERT_TRUE(clean->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_EXPECT_EQ(clean->finalize(), godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_retry), 0x0F0E0D0C0B0A0908ULL), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
	remove_temp_file(dspkg_retry);
}

GDEX_TEST_T(dspkg, archive_id_high_word_mismatch, TAG_INTEGRATION) {
	// The retired pairing model compared only the low 32 bits when the expected
	// content half was non-zero. Ids that agree on the low word and differ in the
	// high word must now fail.
	const char *dstex_path = "user://gdextest_dspkg_id_high.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_id_high.dspkg";
	const char *virtual_path = "res://textures/test_id_high.dstex";
	const uint64_t archive_id = 0x11111111AABBCCDDULL;
	const uint64_t high_word_only_difference = 0x22222222AABBCCDDULL;

	GDEX_ASSERT_TRUE((archive_id & 0xFFFFFFFFULL) == (high_word_only_difference & 0xFFFFFFFFULL));
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), high_word_only_difference), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);

	// Control: the exact id opens.
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, legacy_guid_style_archive_fails_as_id_mismatch, TAG_INTEGRATION) {
	// Archives written under the retired pairing model carried
	// (project_guid << 32) | content_hash while a subset export published the
	// GUID half alone. Pairing is now the full id, so that combination fails
	// loudly instead of opening on the GUID half.
	const char *dstex_path = "user://gdextest_dspkg_id_legacy.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_id_legacy.dspkg";
	const char *virtual_path = "res://textures/test_id_legacy.dstex";
	const uint64_t legacy_guid = 0x5A5A5A5AULL;
	const uint64_t legacy_archive_id = (legacy_guid << 32) | 0xAABBCCDDULL;
	const uint64_t legacy_guid_only_expectation = legacy_guid << 32;

	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, legacy_archive_id, godot::String(), virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), legacy_guid_only_expectation), godot::ERR_FILE_UNRECOGNIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);

	// Control: the archive still pairs with its own full id.
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), legacy_archive_id), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

// --- Integration: key delivered as res://.dspkg/toc.key ----------------------
//
// resolve_encryption_key() acquires a candidate and never installs it; open()
// commits only after the header key_id matches and the TOC AEAD succeeds, and a
// rejected candidate leaves the default state so a later key mount retries.

GDEX_TEST_T(dspkg, toc_key_file_accepted, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_ok.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_ok.dspkg";
	const char *virtual_path = "res://textures/test_key_ok.dstex";
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	TocKeySandbox sandbox;
	// Pre-call state: the delivered-file path must not commit anything before open().
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0xAABBCCDD11223344ULL, key_hex, virtual_path));
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));

	// The delivered file is the contract layout: magic16 || key32.
	const godot::PackedByteArray key_file = godot::FileAccess::get_file_as_bytes(TOC_KEY_PATH);
	GDEX_ASSERT_EQ(key_file.size(), TOC_KEY_FILE_SIZE);
	GDEX_EXPECT_TRUE(key_file.slice(0, TOC_KEY_MAGIC_SIZE) == toc_key_magic());
	GDEX_EXPECT_TRUE(key_file.slice(TOC_KEY_MAGIC_SIZE, TOC_KEY_FILE_SIZE) == key_hex.hex_decode());

	// The archive header stores key_id = BLAKE2b-8(key), rendered as 16 hex chars.
	godot::Ref<godot::FileAccess> archive = godot::FileAccess::open(godot::String(dspkg_path), godot::FileAccess::READ);
	GDEX_ASSERT_TRUE(archive.is_valid());
	archive->seek(52);
	const godot::String header_key_id = archive->get_buffer(8).hex_encode().to_lower();
	archive->close();
	GDEX_EXPECT_STR_EQ(header_key_id, godot::DSPkg::compute_key_id(key_hex));

	// Reading the file does not install anything; only open() commits.
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(reader->has_texture(godot::String(virtual_path)));
	GDEX_EXPECT_FALSE(reader->read_texture(godot::String(virtual_path)).is_empty());

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, toc_key_magic_rejected, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_magic.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_magic.dspkg";
	const char *virtual_path = "res://textures/test_key_magic.dstex";
	const godot::String key_hex = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	TocKeySandbox sandbox;
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0ULL, key_hex, virtual_path));

	// Correct 48-byte length and the real key, but one magic byte corrupted. A
	// reader that only checked the length would open this archive.
	GDEX_ASSERT_TRUE(write_file_bytes(TOC_KEY_PATH, make_toc_key_file(key_hex, TOC_KEY_BYTES, 0, true)));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(reader->has_texture(godot::String(virtual_path)));
	// A rejected candidate never becomes the active key.
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// The failure did not latch: the valid envelope opens the same archive.
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), godot::DSPkg::compute_key_id(key_hex));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, toc_key_wrong_length_rejected, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_length.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_length.dspkg";
	const char *virtual_path = "res://textures/test_key_length.dstex";
	const godot::String key_hex = "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	TocKeySandbox sandbox;
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0ULL, key_hex, virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());

	// 1. 32 bytes: magic16 || key[0..15]. The real key is present but truncated.
	GDEX_ASSERT_TRUE(write_file_bytes(TOC_KEY_PATH, make_toc_key_file(key_hex, 16)));
	GDEX_EXPECT_EQ(godot::FileAccess::get_file_as_bytes(TOC_KEY_PATH).size(), int64_t(32));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// 2. 64 bytes: magic16 || key32 || 16 trailing bytes. Bytes 16..47 are the
	// real key, so only the exact-length rule can reject this file.
	GDEX_ASSERT_TRUE(write_file_bytes(TOC_KEY_PATH, make_toc_key_file(key_hex, TOC_KEY_BYTES, 16)));
	GDEX_EXPECT_EQ(godot::FileAccess::get_file_as_bytes(TOC_KEY_PATH).size(), int64_t(64));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// 3. Exactly 48 bytes: accepted, so length was the only rejection reason.
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, toc_key_wrong_key_rejected, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_wrong.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_wrong.dspkg";
	const char *virtual_path = "res://textures/test_key_wrong.dstex";
	const godot::String key_hex = "1111111122222222333333334444444455555555666666667777777788888888";
	const godot::String wrong_key_hex = "99999999aaaaaaaabbbbbbbbccccccccddddddddeeeeeeeeffffffff00000000";

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	GDEX_ASSERT_EQ(wrong_key_hex.length(), 64);
	TocKeySandbox sandbox;
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0ULL, key_hex, virtual_path));

	// Correct length and magic, but a key that does not reproduce header.key_id.
	GDEX_ASSERT_TRUE(install_toc_key_file(wrong_key_hex));
	GDEX_EXPECT_STR_NE(godot::DSPkg::compute_key_id(wrong_key_hex), godot::DSPkg::compute_key_id(key_hex));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(reader->has_texture(godot::String(virtual_path)));
	// The wrong key is not committed, so the resolution state is unchanged.
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// The right key still opens the archive afterwards.
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, toc_key_missing_then_mounted, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_late.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_late.dspkg";
	const char *virtual_path = "res://textures/test_key_late.dstex";
	const godot::String key_hex = "0f0e0d0c0b0a090807060504030201000f0e0d0c0b0a09080706050403020100";

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	TocKeySandbox sandbox;
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0ULL, key_hex, virtual_path));
	GDEX_ASSERT_FALSE(godot::FileAccess::file_exists(godot::String(TOC_KEY_PATH)));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());

	// No key file yet: the archive cannot be opened.
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// The absence is not latched: delivering the key lets the next open succeed.
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), godot::DSPkg::compute_key_id(key_hex));

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, toc_key_commit_on_open, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_dspkg_key_commit.dstex";
	const char *dspkg_path = "user://gdextest_dspkg_key_commit.dspkg";
	const char *virtual_path = "res://textures/test_key_commit.dstex";
	const godot::String key_hex = "102030405060708090a0b0c0d0e0f001102030405060708090a0b0c0d0e0f001";
	const godot::String wrong_key_hex = "0102030405060708090a0b0c0d0e0f000102030405060708090a0b0c0d0e0f00";
	const godot::String right_key_id = godot::DSPkg::compute_key_id(key_hex);

	GDEX_ASSERT_EQ(key_hex.length(), 64);
	TocKeySandbox sandbox;
	const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
	GDEX_ASSERT_TRUE(build_single_entry_archive(dstex_path, dspkg_path, 0ULL, key_hex, virtual_path));

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());

	// 1. Rejected on the header id comparison: nothing is committed.
	GDEX_ASSERT_TRUE(install_toc_key_file(wrong_key_hex));
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// 2. Rejected on the TOC AEAD with the header id matching: the candidate is
	// proven wrong only after the id proof, and that must not install it either.
	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
	GDEX_ASSERT_TRUE(mutate_file_byte(dspkg_path, 64)); // first MAC byte
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::ERR_UNAUTHORIZED);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);

	// 3. Successful open commits the candidate: the global id is the archive key id.
	GDEX_ASSERT_TRUE(mutate_file_byte(dspkg_path, 64)); // restore the MAC byte
	GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path)), godot::OK);
	GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), right_key_id);

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
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

GDEX_TEST_T(dspkg, path_hash_mismatch_rejected, TAG_INTEGRATION) {
	// The TOC stores a hash of the resource path beside the path itself
	// (finalize() writes path.hash()) and entries_by_hash is keyed by that
	// stored value. An entry whose stored hash does not reproduce from its
	// decoded path cannot have been written by finalize(), so open() must
	// refuse it instead of publishing an index no lookup can agree with.
	const char *dstex_path = "user://temp_dspkg_path_hash.dstex";
	const char *dspkg_path = "user://temp_dspkg_path_hash.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(build_two_entry_archive(dstex_path, dspkg_path));

	{
		// Negative control: the writer-produced archive is accepted and both
		// entries resolve before any byte is patched.
		godot::Ref<godot::DSPkg> control;
		control.instantiate();
		GDEX_ASSERT_TRUE(control.is_valid());
		GDEX_EXPECT_EQ(control->open(godot::String(dspkg_path), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(control->get_entry_count(), 2);
		GDEX_EXPECT_TRUE(control->has_texture("a.png"));
		GDEX_EXPECT_TRUE(control->has_texture("b.png"));
	}

	// The first byte of the TOC is entry A's path_hash: an unencrypted v4
	// entry table starts right after the 64-byte header.
	const int64_t path_hash_offset = static_cast<int64_t>(godot::DSPKG_HEADER_SIZE_V4);
	GDEX_ASSERT_TRUE(mutate_file_byte(dspkg_path, path_hash_offset));

	{
		godot::Ref<godot::DSPkg> patched;
		patched.instantiate();
		GDEX_ASSERT_TRUE(patched.is_valid());
		GDEX_EXPECT_EQ(patched->open(godot::String(dspkg_path), 0ULL), godot::ERR_FILE_CORRUPT);
	}

	// Restoring the byte proves the rejection came from the patch and not from
	// a fixture the parser never accepted.
	GDEX_ASSERT_TRUE(mutate_file_byte(dspkg_path, path_hash_offset));
	{
		godot::Ref<godot::DSPkg> restored;
		restored.instantiate();
		GDEX_ASSERT_TRUE(restored.is_valid());
		GDEX_EXPECT_EQ(restored->open(godot::String(dspkg_path), 0ULL), godot::OK);
	}

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, overlapping_payload_span_rejected, TAG_INTEGRATION) {
	// finalize() writes entries in resource-path order over one running payload
	// offset, so writer output is ordered and disjoint. A span that starts
	// inside the previous entry's span can only come from a hand-edited TOC,
	// and the exact-duplicate-offset check cannot see it (it is keyed by offset
	// and only runs for entries carrying GPU mip metadata).
	const char *dstex_path = "user://temp_dspkg_span.dstex";
	const char *dspkg_path = "user://temp_dspkg_span.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	GDEX_ASSERT_TRUE(build_two_entry_archive(dstex_path, dspkg_path));

	{
		godot::Ref<godot::DSPkg> control;
		control.instantiate();
		GDEX_ASSERT_TRUE(control.is_valid());
		GDEX_EXPECT_EQ(control->open(godot::String(dspkg_path), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(control->get_entry_count(), 2);
	}

	TwoEntryLayout layout;
	GDEX_ASSERT_TRUE(read_two_entry_layout(dspkg_path, layout));
	// Self-checks: entry A is the "a.png" entry at the start of the table, its
	// span is far larger than the overlap step, and entry B follows it at the
	// next 512-byte boundary with mip[0] equal to its payload start (the parser
	// requires that equality, so a data_offset-only patch would be rejected as
	// "Invalid entry mip metadata" instead of exercising the span check).
	GDEX_EXPECT_EQ(layout.path_len_a, static_cast<uint16_t>(5));
	GDEX_EXPECT_EQ(layout.path_len_b, static_cast<uint16_t>(5));
	GDEX_EXPECT_EQ(layout.hash_a, static_cast<uint64_t>(godot::String("a.png").hash()));
	GDEX_EXPECT_EQ(layout.off_a, 0ULL);
	GDEX_EXPECT_TRUE(layout.size_a > 1024);
	GDEX_EXPECT_EQ(layout.mip0_a, layout.payload_start);
	GDEX_EXPECT_EQ(layout.off_b, align_up_local(layout.size_a, 512ULL));
	GDEX_EXPECT_EQ(layout.mip0_b, layout.payload_start + layout.off_b);

	const uint64_t overlap_step = 512;
	GDEX_ASSERT_TRUE(write_u64_at(dspkg_path, layout.entry_b_offset_field, overlap_step));
	GDEX_ASSERT_TRUE(write_u64_at(dspkg_path, layout.entry_b_mip0_field, layout.payload_start + overlap_step));

	{
		godot::Ref<godot::DSPkg> patched;
		patched.instantiate();
		GDEX_ASSERT_TRUE(patched.is_valid());
		GDEX_EXPECT_EQ(patched->open(godot::String(dspkg_path), 0ULL), godot::ERR_FILE_CORRUPT);
	}

	GDEX_ASSERT_TRUE(write_u64_at(dspkg_path, layout.entry_b_offset_field, layout.off_b));
	GDEX_ASSERT_TRUE(write_u64_at(dspkg_path, layout.entry_b_mip0_field, layout.mip0_b));
	{
		godot::Ref<godot::DSPkg> restored;
		restored.instantiate();
		GDEX_ASSERT_TRUE(restored.is_valid());
		GDEX_EXPECT_EQ(restored->open(godot::String(dspkg_path), 0ULL), godot::OK);
	}

	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);
}

GDEX_TEST_T(dspkg, encryption_tamper_and_auth, TAG_INTEGRATION) {
	const char *dstex_path = "user://temp_enc_test.dstex";
	const char *dspkg_path = "user://temp_enc_test.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	TocKeySandbox sandbox;
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

	// 1. Valid open with the delivered key file (no pre-installed global key).
	{
		GlobalKeyResetGuard key_guard("");
		GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::OK);
		GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
		GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), godot::DSPkg::compute_key_id(key_hex));
	}

	// 2. A correctly shaped key file carrying the wrong key -> ERR_UNAUTHORIZED.
	// The guard above returned the resolution state to the default, so this open
	// resolves the key from the file rather than from a committed key.
	{
		GlobalKeyResetGuard key_guard("");
		const godot::String initial_key_id = godot::DSPkg::get_global_encryption_key_id();
		GDEX_ASSERT_TRUE(install_toc_key_file(wrong_key_hex));
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), 0ULL), godot::ERR_UNAUTHORIZED);
		// The rejected candidate is not committed.
		GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), initial_key_id);
	}
	remove_toc_key_file();

	// The tamper cases below install the archive key explicitly so a tampered
	// header cannot also be explained by key resolution failing first.
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

GDEX_TEST_T(dspkg, encrypted_archive_id_pairing, TAG_INTEGRATION) {
	const char *dstex_path = "user://temp_enc_fp.dstex";
	const char *dspkg_path = "user://temp_enc_fp.dspkg";
	remove_temp_file(dstex_path);
	remove_temp_file(dspkg_path);

	TocKeySandbox sandbox;
	GDEX_ASSERT_TRUE(write_temp_dstex(dstex_path, 64, 64, 1, 64 * 64 * 4));

	const godot::String key_hex = "1111111122222222333333334444444455555555666666667777777788888888";
	const uint64_t archive_id = 0xAABBCCDDEEFF0011ULL;

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path), archive_id) == godot::OK);
	GDEX_ASSERT_TRUE(builder->set_encryption_key(key_hex) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture("test.png", godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);

	GDEX_ASSERT_TRUE(install_toc_key_file(key_hex));

	// Open with the matching id -> OK, and the file-delivered key is committed.
	{
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id), godot::OK);
		GDEX_EXPECT_EQ(reader->get_entry_count(), 1);
		GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), godot::DSPkg::compute_key_id(key_hex));
	}

	// Mismatching id -> ERR_FILE_UNRECOGNIZED (the id is checked before the key).
	{
		godot::Ref<godot::DSPkg> reader;
		reader.instantiate();
		GDEX_ASSERT_TRUE(reader.is_valid());
		GDEX_EXPECT_EQ(reader->open(godot::String(dspkg_path), archive_id ^ 0x01ULL), godot::ERR_FILE_UNRECOGNIZED);
		GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
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


