#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dstex_format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// Import-time pixel processing must not cost one engine API call per pixel.
// Two suites:
//   dstex_remap_parity - byte-for-byte equality between DSTex' channel remap /
//       normal normalization and a Color(get_pixel/set_pixel) oracle, over all
//       six 8-bit formats. Passes before and after the byte-path rewrite; it is
//       the guard that keeps the optimization from changing output.
//   dstex_remap_perf - wall-clock budget for a 4096x4096 RGBA8 remap. Fails
//       while the implementation issues two method-bind calls per pixel and
//       passes once it reads/writes the image data in bulk. Remap is the case
//       with a wide margin: normalize's engine calls are a small fraction of its
//       arithmetic, so its timing has no robust separator on an unoptimized
//       build and is covered by the parity case instead.
//
// No files, no engine singletons beyond Image/Time, no GPU: both suites run
// headless and untagged like the rest of tests/format_dstex_tests.cpp.

namespace {

struct RemapCase {
	godot::Image::Format source;
	godot::Image::Format expected_after;
	int red, green, blue, alpha;
};

// expected_after is the engine's channel-expansion result; asserting it turns a
// conversion regression into an explicit format mismatch instead of data noise.
// Ids are the documented 0-10 scale: R,G,B,A, inverted R,G,B,A, Unused, Zero, One.
const RemapCase REMAP_CASES[] = {
	{ godot::Image::FORMAT_RGBA8, godot::Image::FORMAT_RGBA8, 4, 5, 6, 7 },   // four inversions
	{ godot::Image::FORMAT_RGBA8, godot::Image::FORMAT_RGBA8, 2, 1, 0, 3 },   // red/blue swap
	{ godot::Image::FORMAT_RGB8, godot::Image::FORMAT_RGB8, 2, 1, 0, 3 },     // stays RGB8
	{ godot::Image::FORMAT_RGB8, godot::Image::FORMAT_RGBA8, 2, 1, 0, 1 },    // alpha from green
	{ godot::Image::FORMAT_RG8, godot::Image::FORMAT_RG8, 0, 4, 9, 10 },      // inverted green
	{ godot::Image::FORMAT_R8, godot::Image::FORMAT_RG8, 0, 0, 9, 10 },       // expands to RG8
	{ godot::Image::FORMAT_L8, godot::Image::FORMAT_L8, 9, 9, 9, 3 },         // luminance stays L8
	{ godot::Image::FORMAT_L8, godot::Image::FORMAT_RGB8, 1, 2, 3, 3 },       // non-uniform expand
	{ godot::Image::FORMAT_LA8, godot::Image::FORMAT_LA8, 0, 0, 0, 7 },       // inverted alpha
};

// Odd dimensions: a wrong row stride shows up immediately.
constexpr int PARITY_WIDTH = 65;
constexpr int PARITY_HEIGHT = 37;

constexpr int PERF_SIZE = 4096;
constexpr int PERF_RUNS = 3;
constexpr int64_t PERF_BUDGET_MS = 400;

int parity_bpp(godot::Image::Format p_format) {
	switch (p_format) {
		case godot::Image::FORMAT_L8:
		case godot::Image::FORMAT_R8:
			return 1;
		case godot::Image::FORMAT_LA8:
		case godot::Image::FORMAT_RG8:
			return 2;
		case godot::Image::FORMAT_RGB8:
			return 3;
		case godot::Image::FORMAT_RGBA8:
			return 4;
		default:
			return 0;
	}
}

godot::Ref<godot::Image> make_seeded_image(int p_w, int p_h, godot::Image::Format p_format) {
	const int bpp = parity_bpp(p_format);
	godot::PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(p_w) * p_h * bpp);
	uint8_t *w = bytes.ptrw();
	for (int64_t i = 0; i < bytes.size(); ++i) {
		w[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFF);
	}
	return godot::Image::create_from_data(p_w, p_h, false, p_format, bytes);
}

// godot-cpp exposes no Image::duplicate(); a fresh Image over the same bytes is
// the equivalent, and it shares nothing that a later write could mutate.
godot::Ref<godot::Image> clone_image(const godot::Ref<godot::Image> &p_source) {
	return godot::Image::create_from_data(p_source->get_width(), p_source->get_height(),
			p_source->has_mipmaps(), p_source->get_format(), p_source->get_data());
}

// The pre-optimization algorithm, kept as an oracle: the same channel switch
// applied through get_pixel/set_pixel. The production function's expansion step
// is applied to the expected image explicitly before this loop runs.
void reference_remap(const godot::Ref<godot::Image> &p_image, const int p_remaps[4]) {
	const int width = p_image->get_width();
	const int height = p_image->get_height();
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const godot::Color src = p_image->get_pixel(x, y);
			godot::Color dst;
			for (int i = 0; i < 4; ++i) {
				switch (p_remaps[i]) {
					case 0:
						dst[i] = src.r;
						break;
					case 1:
						dst[i] = src.g;
						break;
					case 2:
						dst[i] = src.b;
						break;
					case 3:
						dst[i] = src.a;
						break;
					case 4:
						dst[i] = 1.0f - src.r;
						break;
					case 5:
						dst[i] = 1.0f - src.g;
						break;
					case 6:
						dst[i] = 1.0f - src.b;
						break;
					case 7:
						dst[i] = 1.0f - src.a;
						break;
					case 8:
						break; // Unused, resolved by DSTex::remap_channels before its loop
					case 9:
						dst[i] = 0.0f;
						break;
					case 10:
						dst[i] = 1.0f;
						break;
					default:
						break;
				}
			}
			p_image->set_pixel(x, y, dst);
		}
	}
}

void reference_normalize(const godot::Ref<godot::Image> &p_image) {
	const int width = p_image->get_width();
	const int height = p_image->get_height();
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			godot::Color color = p_image->get_pixel(x, y);
			float nx = color.r * 2.0f - 1.0f;
			float ny = color.g * 2.0f - 1.0f;
			float nz = color.b * 2.0f - 1.0f;
			const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
			if (length > 0.0f) {
				nx /= length;
				ny /= length;
				nz /= length;
			} else {
				nx = 0.0f;
				ny = 0.0f;
				nz = 0.0f;
			}
			color.r = nx * 0.5f + 0.5f;
			color.g = ny * 0.5f + 0.5f;
			color.b = nz * 0.5f + 0.5f;
			p_image->set_pixel(x, y, color);
		}
	}
}

bool same_bytes(const godot::Ref<godot::Image> &p_a, const godot::Ref<godot::Image> &p_b) {
	const godot::PackedByteArray a = p_a->get_data();
	const godot::PackedByteArray b = p_b->get_data();
	if (a.size() != b.size()) {
		return false;
	}
	return std::memcmp(a.ptr(), b.ptr(), static_cast<size_t>(a.size())) == 0;
}

godot::Ref<godot::Image> make_perf_image() {
	godot::Ref<godot::Image> image = godot::Image::create(PERF_SIZE, PERF_SIZE, false, godot::Image::FORMAT_RGBA8);
	image->fill(godot::Color(0.35, 0.35, 0.35, 1.0));
	return image;
}

} // namespace

GDEX_TEST(dstex_remap_parity, remap_matches_pixel_reference) {
	for (const RemapCase &c : REMAP_CASES) {
		const godot::Ref<godot::Image> source = make_seeded_image(PARITY_WIDTH, PARITY_HEIGHT, c.source);
		GDEX_ASSERT_TRUE(source.is_valid());
		// The layout assumption the byte path relies on: no row padding.
		GDEX_ASSERT_EQ(source->get_data().size(),
				static_cast<int64_t>(PARITY_WIDTH) * PARITY_HEIGHT * parity_bpp(c.source));

		const godot::Ref<godot::Image> actual = clone_image(source);
		GDEX_ASSERT_TRUE(actual.is_valid());
		godot::DSTex::remap_channels(actual, c.red, c.green, c.blue, c.alpha);

		const godot::Ref<godot::Image> expected = clone_image(source);
		GDEX_ASSERT_TRUE(expected.is_valid());
		if (expected->get_format() != c.expected_after) {
			expected->convert(c.expected_after);
		}
		const int remaps[4] = { c.red, c.green, c.blue, c.alpha };
		reference_remap(expected, remaps);

		GDEX_EXPECT_EQ(static_cast<int64_t>(actual->get_format()), static_cast<int64_t>(c.expected_after));
		GDEX_EXPECT_EQ(static_cast<int64_t>(actual->get_format()), static_cast<int64_t>(expected->get_format()));
		GDEX_EXPECT_TRUE(same_bytes(actual, expected));
	}
}

GDEX_TEST(dstex_remap_parity, normalize_matches_pixel_reference) {
	const godot::Image::Format formats[] = {
		godot::Image::FORMAT_L8, godot::Image::FORMAT_LA8, godot::Image::FORMAT_R8,
		godot::Image::FORMAT_RG8, godot::Image::FORMAT_RGB8, godot::Image::FORMAT_RGBA8,
	};
	for (godot::Image::Format format : formats) {
		const godot::Ref<godot::Image> source = make_seeded_image(PARITY_WIDTH, PARITY_HEIGHT, format);
		GDEX_ASSERT_TRUE(source.is_valid());

		const godot::Ref<godot::Image> actual = clone_image(source);
		GDEX_ASSERT_TRUE(actual.is_valid());
		godot::DSTex::normalize_image(actual);

		const godot::Ref<godot::Image> expected = clone_image(source);
		GDEX_ASSERT_TRUE(expected.is_valid());
		reference_normalize(expected);

		GDEX_EXPECT_EQ(static_cast<int64_t>(actual->get_format()), static_cast<int64_t>(format));
		GDEX_EXPECT_TRUE(same_bytes(actual, expected));
	}
}

GDEX_TEST(dstex_remap_perf, inverted_remap_4096_rgba8_within_budget) {
	const godot::Ref<godot::Image> image = make_perf_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	// Warm-up: the LUT build and first-touch page faults stay outside the window.
	godot::DSTex::remap_channels(image, 4, 5, 6, 7);

	int64_t best_ms = std::numeric_limits<int64_t>::max();
	for (int run = 0; run < PERF_RUNS; ++run) {
		const int64_t started = static_cast<int64_t>(godot::Time::get_singleton()->get_ticks_msec());
		godot::DSTex::remap_channels(image, 4, 5, 6, 7);
		const int64_t elapsed = static_cast<int64_t>(godot::Time::get_singleton()->get_ticks_msec()) - started;
		best_ms = std::min(best_ms, elapsed);
	}
	godot::UtilityFunctions::print("dstex_remap_perf remap_4096_rgba8 best_ms=", best_ms);
	GDEX_EXPECT_LT(best_ms, PERF_BUDGET_MS);
}
