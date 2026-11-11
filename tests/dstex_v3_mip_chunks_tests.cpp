// Headless coverage for the standalone-.dstex chunk splitter's mip-table read.
// Fixtures are produced by DSTex::write_to_file() so a rejection is attributable
// to the splitter's own parse, not to a hand-built header: every file here is
// accepted by DSTex::read_header() first (assert 1), and only then handed to the
// splitter.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>

#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
#include "platform/win32/ds_win32_backend.h"
#endif

namespace {

constexpr int kBaseWidth = 64;
constexpr int kBaseHeight = 64;
constexpr int kLogicalWidth = 60;
constexpr int kLogicalHeight = 56;
constexpr int kMipCount = 3;
constexpr uint64_t kChunkCapacity = 64ull * 1024ull * 1024ull;

godot::PackedByteArray make_chain_data(int p_mips) {
	godot::PackedByteArray data;
	for (int i = 0; i < p_mips; ++i) {
		const int dim = std::max(1, kBaseWidth >> i);
		godot::Ref<godot::Image> mip = godot::Image::create(dim, dim, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.2f, 0.4f, 0.6f, 1.0f));
		data.append_array(mip->get_data());
	}
	return data;
}

// p_v3 selects the 64-byte header: a logical size that differs from storage is
// the writer's v3 trigger.
bool write_chain(const char *p_path, bool p_v3) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), make_chain_data(kMipCount), kBaseWidth,
			kBaseHeight, godot::Image::FORMAT_RGBA8, kMipCount, 1, false, false,
			p_v3 ? kLogicalWidth : 0, p_v3 ? kLogicalHeight : 0) == godot::OK;
}

uint64_t file_length(const char *p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null()) {
		return 0;
	}
	const uint64_t len = file->get_length();
	file->close();
	return len;
}

void track_cleanup(::gdextest::TestContext &ctx, std::initializer_list<const char *> p_paths) {
	for (const char *p : p_paths) {
		const godot::String path(p);
		ctx.add_teardown([path]() {
			godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
			if (ps != nullptr) {
				godot::DirAccess::remove_absolute(ps->globalize_path(path));
			}
		});
	}
}

#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
// Asserts that the splitter reproduces the validated table exactly.
void expect_split_matches_table(::gdextest::TestContext &ctx, const char *p_path, int p_expected_version) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	godot::Dictionary header = dstex->read_header(godot::String(p_path));

	// 1. Anti-vacuity: the fixture is the layout this case claims to cover.
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["version"]), static_cast<int64_t>(p_expected_version));
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["mip_count"]), static_cast<int64_t>(kMipCount));
	GDEX_EXPECT_FALSE(static_cast<bool>(header["chunked"]));

	const godot::PackedInt64Array table_offsets = header["mip_offsets"];
	const godot::PackedInt32Array table_sizes = header["mip_sizes"];
	const godot::PackedInt32Array table_uncompressed = header["mip_uncompressed_sizes"];

	// 2. The splitter must accept a file every other reader accepts.
	godot::Dictionary split = godot::Win32Backend::_debug_get_dstex_mip_chunks_for_testing(
			godot::String(p_path), 0, file_length(p_path), kChunkCapacity);
	if (split.is_empty()) {
		GDEX_FAIL("standalone .dstex chunk splitter rejected a writer-produced file that read_header() accepts");
		return;
	}

	const godot::PackedInt64Array source_offsets = split["source_offsets"];
	const godot::PackedInt64Array source_sizes = split["source_sizes"];
	const godot::PackedInt64Array destination_offsets = split["destination_offsets"];
	const godot::PackedInt64Array destination_sizes = split["destination_sizes"];

	// 3. One chunk per mip.
	GDEX_EXPECT_EQ(source_offsets.size(), static_cast<int64_t>(kMipCount));
	if (source_offsets.size() != static_cast<int64_t>(kMipCount)) {
		return;
	}

	// 4-5. Every chunk is the table's own entry, and the destination is the
	// cumulative footprint the child requests rely on.
	int64_t cumulative = 0;
	for (int64_t i = 0; i < kMipCount; ++i) {
		GDEX_EXPECT_EQ(source_offsets[i], table_offsets[i]);
		GDEX_EXPECT_EQ(source_sizes[i], static_cast<int64_t>(table_sizes[i]));
		GDEX_EXPECT_EQ(destination_sizes[i], static_cast<int64_t>(table_uncompressed[i]));
		GDEX_EXPECT_EQ(destination_offsets[i], cumulative);
		cumulative += destination_sizes[i];
	}

	// 6. The split covers the whole declared footprint.
	GDEX_EXPECT_EQ(cumulative, static_cast<int64_t>(header["original_size"]));
}
#endif

} // namespace

GDEX_TEST_T(dstex_v3_chunks, standalone_v3_whole_payload_splits_per_mip, TAG_UNIT) {
	const char *const kPath = "user://gdex_v3_chunks_multi.dstex";
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	track_cleanup(ctx, { kPath });
	GDEX_ASSERT_TRUE(write_chain(kPath, true));
	expect_split_matches_table(ctx, kPath, 3);
#else
	GDEX_SKIP("Win32 chunk splitter seam unavailable (non-Windows or release build)");
#endif
}

GDEX_TEST_T(dstex_v3_chunks, standalone_v2_whole_payload_splits_per_mip, TAG_UNIT) {
	const char *const kPath = "user://gdex_v3_chunks_multi_v2.dstex";
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	track_cleanup(ctx, { kPath });
	GDEX_ASSERT_TRUE(write_chain(kPath, false));
	expect_split_matches_table(ctx, kPath, 2);
#else
	GDEX_SKIP("Win32 chunk splitter seam unavailable (non-Windows or release build)");
#endif
}

GDEX_TEST_T(dstex_v3_chunks, non_dstex_path_returns_empty, TAG_UNIT) {
	const char *const kPath = "user://gdex_v3_chunks_multi.notdstex";
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	track_cleanup(ctx, { kPath });
	GDEX_ASSERT_TRUE(write_chain(kPath, true));
	godot::Dictionary split = godot::Win32Backend::_debug_get_dstex_mip_chunks_for_testing(
			godot::String(kPath), 0, file_length(kPath), kChunkCapacity);
	GDEX_EXPECT_TRUE(split.is_empty());
#else
	GDEX_SKIP("Win32 chunk splitter seam unavailable (non-Windows or release build)");
#endif
}

// A chunk's upload unit is counted in the container's storage rows (block rows
// for the 4x4 formats) but the request destination is a D3D12_BOX, which speaks
// texel rows. This pins the translation and the mip-edge clamp that the loader
// and the prewarm walker share.
GDEX_TEST_T(dstex_v3_chunks, chunk_texel_region_units, TAG_UNIT) {
	(void)ctx;
	uint32_t top = 0;
	uint32_t bottom = 0;

	// 256-wide BC7: 64 block rows for a 256-high mip; 16 block rows per chunk.
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(256, 0, 16,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(top), static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(64));
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(256, 48, 16,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(top), static_cast<int64_t>(192));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(256));

	// 256x200 BC7: 50 block rows, so the tail chunk is 2 block rows = 8 texels
	// and ends exactly on the mip edge.
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(200, 48, 2,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(top), static_cast<int64_t>(192));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(200));

	// Linear formats are one for one.
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(64, 8, 4,
			godot::Image::FORMAT_RGBA8, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(top), static_cast<int64_t>(8));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(12));

	// Height not a multiple of 4: ceil(22/4) = 6 storage rows, so a 2-row band
	// at row 4 spans 16..24 and must clamp to the mip edge (22).
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(22, 4, 2,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(top), static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(22));

	// Unknown formats classify as linear; every caller has rejected them before
	// this point and the backend's DXGI-side check is the cross-check.
	GDEX_EXPECT_TRUE(godot::DSTex::get_chunk_texel_region(256, 0, 16,
			godot::Image::FORMAT_MAX, top, bottom));
	GDEX_EXPECT_EQ(static_cast<int64_t>(bottom), static_cast<int64_t>(16));

	// Rejections: empty band, band starting past the mip, additive overflow,
	// zero mip height.
	GDEX_EXPECT_FALSE(godot::DSTex::get_chunk_texel_region(256, 0, 0,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_FALSE(godot::DSTex::get_chunk_texel_region(22, 6, 1,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_FALSE(godot::DSTex::get_chunk_texel_region(256, UINT32_MAX, 2,
			godot::Image::FORMAT_BPTC_RGBA, top, bottom));
	GDEX_EXPECT_FALSE(godot::DSTex::get_chunk_texel_region(0, 0, 1,
			godot::Image::FORMAT_RGBA8, top, bottom));
}
