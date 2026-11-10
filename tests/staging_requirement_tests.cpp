// Import-time staging requirement: read_header() must report the smallest
// staging buffer that keeps every request the file can produce inside
// DirectStorage's limit (E_DSTORAGE_REQUEST_TOO_LARGE otherwise).
//
// The importer uses the value to warn with an exact MiB number instead of the
// old whole-chain estimate, which over-warned on every multi-mip texture the
// backend splits per mip anyway.
//
// Pure format + FileAccess; no RenderingDevice, so this runs in the headless
// suite. The GDeflate chain is produced by the real writer, so the assertion
// cannot drift from the payload layout.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstdint>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

godot::PackedByteArray make_chain(int p_mips) {
	godot::PackedByteArray data;
	int dim = kWidth;
	for (int i = 0; i < p_mips; ++i) {
		godot::Ref<godot::Image> mip = godot::Image::create(dim, dim, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.2f, 0.4f, 0.6f, 1.0f));
		data.append_array(mip->get_data());
		dim = std::max(1, dim / 2);
	}
	return data;
}

bool write_chain(const char *p_path, int p_mips) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), make_chain(p_mips), kWidth, kHeight,
			godot::Image::FORMAT_RGBA8, p_mips, 1) == godot::OK;
}

void track_cleanup(::gdextest::TestContext &ctx, const char *p_path) {
	const godot::String path(p_path);
	ctx.add_teardown([path]() {
		godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
		if (ps != nullptr) {
			godot::DirAccess::remove_absolute(ps->globalize_path(path));
		}
	});
}

} // namespace

GDEX_TEST(staging_requirement, multi_mip_is_largest_single_mip_not_the_chain) {
	const char *path = "user://gdextest_min_staging_multi.dstex";
	track_cleanup(ctx, path);
	GDEX_ASSERT_TRUE(write_chain(path, 3));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["mip_count"]), static_cast<int64_t>(3));

	const godot::PackedInt32Array mip_sizes = header["mip_sizes"];
	const godot::PackedInt32Array mip_uncompressed = header["mip_uncompressed_sizes"];
	GDEX_ASSERT_EQ(mip_sizes.size(), static_cast<int64_t>(3));
	GDEX_ASSERT_EQ(mip_uncompressed.size(), static_cast<int64_t>(3));

	int64_t expected = 0;
	for (int64_t i = 0; i < mip_sizes.size(); ++i) {
		expected = std::max<int64_t>(expected,
				std::max<int64_t>(static_cast<int64_t>(mip_sizes[i]),
						static_cast<int64_t>(mip_uncompressed[i])));
	}
	const int64_t reported = static_cast<int64_t>(header["min_staging_bytes"]);
	GDEX_EXPECT_EQ(reported, expected);

	// Anti-vacuity: per-mip is strictly below the whole chain, so this cannot be
	// satisfied by a sum-of-all-mips implementation.
	const int64_t original_size = static_cast<int64_t>(header["original_size"]);
	GDEX_EXPECT_TRUE(reported < original_size);
	// 64x64 RGBA8 footprint = 64 rows * 256-byte pitch.
	GDEX_EXPECT_EQ(reported, static_cast<int64_t>(64 * 256));
}

GDEX_TEST(staging_requirement, single_mip_equals_whole_payload) {
	const char *path = "user://gdextest_min_staging_single.dstex";
	track_cleanup(ctx, path);
	GDEX_ASSERT_TRUE(write_chain(path, 1));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["min_staging_bytes"]),
			static_cast<int64_t>(header["original_size"]));
}
