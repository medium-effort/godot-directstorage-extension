#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_pack_scanner.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <cstring>

namespace {

godot::PackedByteArray make_bytes(const uint8_t *p_data, int64_t p_len) {
	godot::PackedByteArray arr;
	arr.resize(p_len);
	if (p_len > 0) {
		std::memcpy(arr.ptrw(), p_data, static_cast<size_t>(p_len));
	}
	return arr;
}

} // namespace

GDEX_TEST_T(pack_scanner, buffer_contains_bytes_handles_edges_and_matches, TAG_UNIT) {
	(void)ctx;

	const uint8_t needle_raw[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	const godot::PackedByteArray needle = make_bytes(needle_raw, 4);
	const godot::PackedByteArray empty;

	GDEX_EXPECT_FALSE(godot::DSPackScanner::buffer_contains_bytes(empty, needle));
	GDEX_EXPECT_FALSE(godot::DSPackScanner::buffer_contains_bytes(needle, empty));

	const uint8_t short_raw[3] = { 0xDE, 0xAD, 0xBE };
	GDEX_EXPECT_FALSE(godot::DSPackScanner::buffer_contains_bytes(make_bytes(short_raw, 3), needle));

	// Exact match
	GDEX_EXPECT_TRUE(godot::DSPackScanner::buffer_contains_bytes(needle, needle));

	// Match after partial prefix false positive
	const uint8_t partial_then_hit[10] = { 0xDE, 0xAD, 0x00, 0xDE, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22 };
	GDEX_EXPECT_TRUE(godot::DSPackScanner::buffer_contains_bytes(make_bytes(partial_then_hit, 10), needle));

	// Absent
	const uint8_t miss_raw[8] = { 0xDE, 0xAD, 0xBE, 0x00, 0x01, 0x02, 0x03, 0x04 };
	GDEX_EXPECT_FALSE(godot::DSPackScanner::buffer_contains_bytes(make_bytes(miss_raw, 8), needle));
}

GDEX_TEST_T(pack_scanner, file_contains_bytes_handles_missing_clean_and_chunk_boundary, TAG_UNIT) {
	const uint8_t needle_raw[16] = {
		0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
		0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90,
	};
	const godot::PackedByteArray needle = make_bytes(needle_raw, 16);

	// Missing file -> ok == false
	const godot::Dictionary missing = godot::DSPackScanner::file_contains_bytes("user://gdextest_nonexistent_pack_scanner.bin", needle);
	GDEX_EXPECT_FALSE(static_cast<bool>(missing.get("ok", true)));
	GDEX_EXPECT_FALSE(static_cast<bool>(missing.get("found", true)));

	const godot::String clean_path = "user://gdextest_pack_scanner_clean.bin";
	const godot::String boundary_path = "user://gdextest_pack_scanner_boundary.bin";
	ctx.add_teardown([clean_path, boundary_path]() {
		godot::DirAccess::remove_absolute(clean_path);
		godot::DirAccess::remove_absolute(boundary_path);
	});

	// Clean 4 MiB + 64 B file -> ok == true, found == false
	constexpr int64_t kChunk = 4 * 1024 * 1024;
	godot::PackedByteArray buf;
	buf.resize(kChunk + 64);
	std::memset(buf.ptrw(), 0x55, static_cast<size_t>(buf.size()));

	{
		godot::Ref<godot::FileAccess> f = godot::FileAccess::open(clean_path, godot::FileAccess::WRITE);
		GDEX_ASSERT_TRUE(f.is_valid());
		f->store_buffer(buf);
		GDEX_ASSERT_EQ(static_cast<int64_t>(f->get_error()), static_cast<int64_t>(godot::OK));
	}

	const godot::Dictionary clean_res = godot::DSPackScanner::file_contains_bytes(clean_path, needle);
	GDEX_EXPECT_TRUE(static_cast<bool>(clean_res.get("ok", false)));
	GDEX_EXPECT_FALSE(static_cast<bool>(clean_res.get("found", true)));
	GDEX_EXPECT_EQ(static_cast<int64_t>(clean_res.get("size", int64_t(-1))), buf.size());

	// Needle straddling the 4 MiB chunk boundary (offset = kChunk - 8)
	std::memcpy(buf.ptrw() + (kChunk - 8), needle_raw, 16);
	{
		godot::Ref<godot::FileAccess> f = godot::FileAccess::open(boundary_path, godot::FileAccess::WRITE);
		GDEX_ASSERT_TRUE(f.is_valid());
		f->store_buffer(buf);
		GDEX_ASSERT_EQ(static_cast<int64_t>(f->get_error()), static_cast<int64_t>(godot::OK));
	}

	const godot::Dictionary boundary_res = godot::DSPackScanner::file_contains_bytes(boundary_path, needle);
	GDEX_EXPECT_TRUE(static_cast<bool>(boundary_res.get("ok", false)));
	GDEX_EXPECT_TRUE(static_cast<bool>(boundary_res.get("found", false)));
	GDEX_EXPECT_EQ(static_cast<int64_t>(boundary_res.get("size", int64_t(-1))), buf.size());
}
