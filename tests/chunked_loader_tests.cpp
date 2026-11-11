// End-to-end chunked loading: a chunked .dstex must come back byte-identical
// through BOTH consumer paths the runtime has.
//
//  - CPU/ImageTexture path (headless-safe): each mip is reassembled from its
//    independent chunk streams and unpadded.
//  - GPU path (windowed D3D12): one TEXTURE_REGION request per chunk, read back
//    from the device texture. A loader that still sent one whole-mip request
//    would feed chunk bytes to a single GDeflate stream and fail the byte
//    comparison, so the readback is what proves per-chunk routing.
//
// The chunk unit and the parser contract are covered by dstex_chunks; this file
// covers the loader/backend consumption.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "formats/dstex_format.h"
#include "integration/ds_resource_format_loader.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;

godot::PackedByteArray make_chain(int p_mips) {
	godot::PackedByteArray data;
	int w = kWidth;
	int h = kHeight;
	for (int i = 0; i < p_mips; ++i) {
		godot::Ref<godot::Image> mip = godot::Image::create(w, h, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.1f * static_cast<float>(i + 1), 0.3f, 0.6f, 1.0f));
		godot::PackedByteArray bytes = mip->get_data();
		for (int64_t b = 0; b < bytes.size(); ++b) {
			bytes[b] = static_cast<uint8_t>((bytes[b] + b * 17 + i * 5) & 0xFF);
		}
		data.append_array(bytes);
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

bool write_chunked(const char *p_path, const godot::PackedByteArray &p_data, int p_mips, bool p_as_image_texture) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), p_data, kWidth, kHeight,
			godot::Image::FORMAT_RGBA8, p_mips, 1, false, p_as_image_texture, 0, 0, 0, 1) == godot::OK;
}

// Same chain shape at an explicit size and chunk size, so a fixture large enough
// to split a non-zero mip into several chunks can be built without disturbing
// the cases above.
godot::PackedByteArray make_chain_sized(int p_width, int p_height, int p_mips) {
	godot::PackedByteArray data;
	int w = p_width;
	int h = p_height;
	for (int i = 0; i < p_mips; ++i) {
		godot::Ref<godot::Image> mip = godot::Image::create(w, h, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.1f * static_cast<float>(i + 1), 0.3f, 0.6f, 1.0f));
		godot::PackedByteArray bytes = mip->get_data();
		for (int64_t b = 0; b < bytes.size(); ++b) {
			bytes[b] = static_cast<uint8_t>((bytes[b] + b * 17 + i * 5) & 0xFF);
		}
		data.append_array(bytes);
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

bool write_chunked_sized(const char *p_path, const godot::PackedByteArray &p_data, int p_width,
		int p_height, int p_mips, int p_chunk_kb, bool p_as_image_texture) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), p_data, p_width, p_height,
			godot::Image::FORMAT_RGBA8, p_mips, 1, false, p_as_image_texture, 0, 0, 0, p_chunk_kb) == godot::OK;
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

GDEX_TEST(chunked_loader, cpu_image_texture_full_chain_round_trip) {
	const char *path = "user://gdextest_chunk_loader_cpu.dstex";
	track_cleanup(ctx, path);
	// 7 mips = the full chain for 64x64, so the loader builds a mipmapped image
	// and every mip's chunk assembly participates.
	const godot::PackedByteArray source = make_chain(7);
	GDEX_ASSERT_TRUE(write_chunked(path, source, 7, true));

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());
	godot::Variant result = loader->_load(godot::String(path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> texture(result);
	GDEX_ASSERT_TRUE(texture.is_valid());

	// as_image_texture forces the CPU path even on a D3D12 render thread.
	godot::Ref<godot::ImageTexture> image_texture = texture;
	GDEX_ASSERT_TRUE(image_texture.is_valid());
	godot::Ref<godot::Image> image = image_texture->get_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	GDEX_EXPECT_EQ(static_cast<int64_t>(image->get_width()), static_cast<int64_t>(kWidth));
	GDEX_EXPECT_EQ(static_cast<int64_t>(image->get_height()), static_cast<int64_t>(kHeight));
	GDEX_EXPECT_TRUE(image->has_mipmaps());
	const godot::PackedByteArray decoded = image->get_data();
	GDEX_ASSERT_EQ(decoded.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(decoded.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}

GDEX_TEST_T(chunked_loader, gpu_texture_single_mip_round_trip, TAG_INTEGRATION | TAG_SLOW) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}
	if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver).");
	}
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("No RenderingDevice for the GPU chunked load.");
	}

	const char *path = "user://gdextest_chunk_loader_gpu.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain(1);
	GDEX_ASSERT_TRUE(write_chunked(path, source, 1, false));

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());
	godot::Variant result = loader->_load(godot::String(path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> texture(result);
	GDEX_ASSERT_TRUE(texture.is_valid());

	// If chunking were unsupported the loader would have fallen back to an
	// ImageTexture; reaching the D3D12 device texture is part of the assertion.
	godot::Ref<godot::Texture2DRD> rd_texture = texture;
	GDEX_ASSERT_TRUE(rd_texture.is_valid());
	const godot::RID device_rid = rd_texture->get_texture_rd_rid();
	GDEX_ASSERT_TRUE(device_rid.is_valid());

	const godot::PackedByteArray readback = rd->texture_get_data(device_rid, 0);
	GDEX_ASSERT_EQ(readback.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(readback.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}

namespace {

// BC7 fixtures for the row-range unit check. 256 wide gives a 1024-byte block
// row (64 blocks * 16 bytes, already 256-aligned), so a 16 KiB chunk target is
// exactly 16 block rows. Each block row is filled with a distinct constant, so
// the payload is run-compressible (the writer keeps the GDeflate chunk path
// instead of falling back to NONE) and any row displacement is visible in the
// readback.
constexpr int kBcWidth = 256;
constexpr int kBcChunkKb = 16;

godot::PackedByteArray make_bc_row_gradient(int p_width, int p_height) {
	const godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(
			static_cast<uint32_t>(p_width), static_cast<uint32_t>(p_height),
			godot::Image::FORMAT_BPTC_RGBA);
	godot::PackedByteArray data;
	if (layout.footprint_size == 0 || layout.row_pitch == 0) {
		return data;
	}
	data.resize(static_cast<int64_t>(layout.footprint_size));
	uint8_t *writer = data.ptrw();
	for (uint64_t row = 0; row < layout.row_count; ++row) {
		memset(writer + row * layout.row_pitch,
				static_cast<int>((row * 37u + 11u) & 0xFFu),
				static_cast<size_t>(layout.row_pitch));
	}
	return data;
}

bool write_chunked_bc7(const char *p_path, int p_height) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::PackedByteArray data = make_bc_row_gradient(kBcWidth, p_height);
	return dstex->write_to_file(godot::String(p_path), data, kBcWidth, p_height,
				   godot::Image::FORMAT_BPTC_RGBA, 1, 1, false, false, 0, 0, 0, kBcChunkKb) == godot::OK;
}

// BC7 readback comes back as block bytes; accept either the 256B-padded
// footprint or the tight natural shape, exactly like the prewarm helper does.
// On a mismatch, report the first differing byte and its storage row so the
// observation says which bands were actually written.
bool readback_matches_bc7(const godot::PackedByteArray &p_readback,
		const godot::PackedByteArray &p_expected, int p_height, std::string &r_detail) {
	const godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(
			kBcWidth, static_cast<uint32_t>(p_height), godot::Image::FORMAT_BPTC_RGBA);
	const int64_t padded = static_cast<int64_t>(layout.footprint_size);
	const int64_t tight = static_cast<int64_t>(layout.source_size);
	const int64_t compare_size = p_readback.size() == padded ? padded
			: (p_readback.size() == tight ? tight : -1);
	if (compare_size < 0) {
		r_detail = "unexpected readback size " + std::to_string(p_readback.size()) +
				" (tight=" + std::to_string(tight) + ", padded=" + std::to_string(padded) + ")";
		return false;
	}
	const char *shape = compare_size == padded ? "padded" : "tight";
	const uint64_t row_bytes = compare_size == padded ? layout.row_pitch : layout.source_row_bytes;
	size_t first = 0;
	const size_t limit = static_cast<size_t>(compare_size);
	while (first < limit && p_readback.ptr()[first] == p_expected.ptr()[first]) {
		++first;
	}
	if (first == limit) {
		r_detail = shape;
		return true;
	}
	r_detail = std::string(shape) + ", first mismatch at byte " + std::to_string(first) +
			" (storage row " + std::to_string(row_bytes == 0 ? 0 : first / row_bytes) + ")";
	return false;
}

} // namespace

// Every chunk of a block-compressed mip must target a texel-row band of the
// subresource. The chunk table is written in 4x4 block rows, so the band a
// request carries is (block_row * 4 .. block_rows * 4). Two failure shapes are
// covered: a 256-high mip has four 16-block-row chunks whose four bottoms are
// all multiples of 4, so HEAD's band validation passes and the copy lands in 1/4
// of the rows (silent); a 200-high mip ends with a 2-block-row tail (8 texel
// rows) whose block-row bottom (50) is neither 4-aligned nor the mip edge, so it
// is rejected and the loader falls back to CPU (loud). Heights that are not a
// multiple of 4 are deliberately not a fixture: the queue rejects the whole
// texture with E_INVALIDARG (measured), so they cannot exercise this path.
GDEX_TEST_T(chunked_loader, gpu_bc7_row_range_round_trip, TAG_INTEGRATION | TAG_SLOW) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}
	if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver).");
	}
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("No RenderingDevice for the BC7 chunked load.");
	}

	struct Fixture {
		const char *path;
		int height;
		int tail_bytes; // uncompressed size of the final chunk (whole block rows)
	};
	const Fixture fixtures[] = {
		{ "user://gdextest_chunk_bc7_256.dstex", 256, 16384 },
		{ "user://gdextest_chunk_bc7_200.dstex", 200, 2048 },
	};

	for (const Fixture &fixture : fixtures) {
		track_cleanup(ctx, fixture.path);
		GDEX_ASSERT_TRUE(write_chunked_bc7(fixture.path, fixture.height));

		// Fixture guard: if the writer stopped chunking (or fell back to NONE),
		// no row-range request exists and the rest of the test would be vacuous.
		godot::Ref<godot::DSTex> probe;
		probe.instantiate();
		const godot::Dictionary header = probe->read_header(godot::String(fixture.path));
		GDEX_ASSERT_TRUE(!header.is_empty());
		GDEX_EXPECT_TRUE(static_cast<bool>(header.get("chunked", false)));
		GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<int>(header.get("compression_format", 0))),
				static_cast<int64_t>(1));
		GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<int>(header.get("width", 0))),
				static_cast<int64_t>(kBcWidth));
		GDEX_EXPECT_EQ(static_cast<int64_t>(static_cast<int>(header.get("height", 0))),
				static_cast<int64_t>(fixture.height));
		const godot::PackedInt32Array counts = (godot::PackedInt32Array)header["mip_chunk_counts"];
		const godot::PackedInt32Array pitches = (godot::PackedInt32Array)header["mip_row_pitches"];
		const godot::PackedInt32Array uncompressed = (godot::PackedInt32Array)header["chunk_uncompressed_sizes"];
		GDEX_ASSERT_EQ(static_cast<int64_t>(counts.size()), static_cast<int64_t>(1));
		GDEX_EXPECT_EQ(static_cast<int64_t>(counts[0]), static_cast<int64_t>(4));
		GDEX_EXPECT_EQ(static_cast<int64_t>(pitches[0]), static_cast<int64_t>(1024));
		GDEX_ASSERT_EQ(static_cast<int64_t>(uncompressed.size()), static_cast<int64_t>(4));
		GDEX_EXPECT_EQ(static_cast<int64_t>(uncompressed[0]), static_cast<int64_t>(16384));
		GDEX_EXPECT_EQ(static_cast<int64_t>(uncompressed[3]),
				static_cast<int64_t>(fixture.tail_bytes));

		godot::Ref<godot::DSResourceFormatLoader> loader;
		loader.instantiate();
		GDEX_ASSERT_TRUE(loader.is_valid());
		godot::Variant result = loader->_load(godot::String(fixture.path), godot::String(""), false,
				godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
		godot::Ref<godot::Texture2D> texture(result);
		GDEX_ASSERT_TRUE(texture.is_valid());

		// A CPU fallback is the failure shape for the tail-chunk fixture: the
		// block-row band overruns the mip edge and the request is rejected.
		// Record it per fixture instead of aborting, so one run reports both.
		godot::Ref<godot::Texture2DRD> gpu_texture = texture;
		if (gpu_texture.is_null()) {
			GDEX_FAIL(std::string(fixture.path) + ": GPU path was not used (loader fell back to CPU).");
			continue;
		}
		const godot::RID device_rid = gpu_texture->get_texture_rd_rid();
		GDEX_ASSERT_TRUE(device_rid.is_valid());

		const godot::Ref<godot::RDTextureFormat> format = rd->texture_get_format(device_rid);
		GDEX_ASSERT_TRUE(format.is_valid());
		GDEX_EXPECT_EQ(static_cast<int64_t>(format->get_height()),
				static_cast<int64_t>(fixture.height));

		const godot::PackedByteArray readback = rd->texture_get_data(device_rid, 0);
		const godot::PackedByteArray expected = make_bc_row_gradient(kBcWidth, fixture.height);
		std::string detail;
		if (!readback_matches_bc7(readback, expected, fixture.height, detail)) {
			GDEX_FAIL(std::string(fixture.path) + " BC7 row-range readback mismatch (" + detail + ")");
		}
	}
}

// Control for the multi-mip GPU case: the same 512x512 / full-chain / 64 KiB
// fixture, taken through the CPU path. It pins the per-mip chunk split and
// proves the payload reassembles byte-for-byte before any D3D12 box is built, so
// a red GPU case can only be the box. The chain must be FULL: the loader
// deliberately slices a partial chain to mip 0 without mipmaps
// (ds_resource_format_loader.cpp:934-947), which would hide the mip this exists
// to prove.
GDEX_TEST(chunked_loader, cpu_sized_multi_mip_chunked_chain_round_trip) {
	const char *path = "user://gdextest_chunk_loader_cpu_sized.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain_sized(512, 512, 10);
	GDEX_ASSERT_TRUE(write_chunked_sized(path, source, 512, 512, 10, 64, true));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_TRUE(static_cast<bool>(header["chunked"]));
	godot::UtilityFunctions::print("[sized] compression_format=",
			static_cast<int64_t>(header.get("compression_format", -1)));
	// 32 rows per chunk at a 2048-byte pitch; every tail mip fits one chunk.
	const godot::PackedInt32Array counts = header["mip_chunk_counts"];
	GDEX_ASSERT_EQ(counts.size(), static_cast<int64_t>(10));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[0]), static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[1]), static_cast<int64_t>(4));
	for (int64_t mip = 2; mip < counts.size(); ++mip) {
		GDEX_EXPECT_EQ(static_cast<int64_t>(counts[mip]), static_cast<int64_t>(1));
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());
	godot::Variant result = loader->_load(godot::String(path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> texture(result);
	GDEX_ASSERT_TRUE(texture.is_valid());
	godot::Ref<godot::ImageTexture> image_texture = texture;
	GDEX_ASSERT_TRUE(image_texture.is_valid());
	godot::Ref<godot::Image> image = image_texture->get_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	GDEX_EXPECT_EQ(static_cast<int64_t>(image->get_width()), static_cast<int64_t>(512));
	GDEX_EXPECT_EQ(static_cast<int64_t>(image->get_height()), static_cast<int64_t>(512));
	GDEX_EXPECT_TRUE(image->has_mipmaps());
	const godot::PackedByteArray decoded = image->get_data();
	GDEX_ASSERT_EQ(decoded.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(decoded.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}

// Row-range chunked upload across a mip boundary: a chunked payload emits one
// request per chunk, and a mip split into several chunks arrives as a sub-box of
// ONE subresource. The box therefore belongs to that subresource - its rows AND
// its horizontal span are the target mip's, never mip 0's.
//
// The chain is deliberately partial (two mips) and both mips are multi-chunk, so
// every request for this payload is a row-range request: a full chain's tail
// mips fit a single chunk and take a different destination shape, which would
// make this case fail for an unrelated reason.
GDEX_TEST_T(chunked_loader, gpu_texture_two_mip_chain_round_trip, TAG_INTEGRATION | TAG_SLOW) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}
	if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver).");
	}
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("No RenderingDevice for the GPU chunked load.");
	}

	const char *path = "user://gdextest_chunk_loader_gpu_two_mip.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain_sized(512, 512, 2);
	GDEX_ASSERT_TRUE(write_chunked_sized(path, source, 512, 512, 2, 64, false));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_TRUE(static_cast<bool>(header["chunked"]));
	godot::UtilityFunctions::print("[two_mip] compression_format=",
			static_cast<int64_t>(header.get("compression_format", -1)));
	// 32 rows per chunk at a 2048-byte pitch, 64 rows per chunk at 1024.
	const godot::PackedInt32Array counts = header["mip_chunk_counts"];
	GDEX_ASSERT_EQ(counts.size(), static_cast<int64_t>(2));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[0]), static_cast<int64_t>(16));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[1]), static_cast<int64_t>(4));

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());
	godot::Variant result = loader->_load(godot::String(path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> texture(result);
	GDEX_ASSERT_TRUE(texture.is_valid());
	// A CPU fallback here means the batch failed; the GPU path is the coverage.
	godot::Ref<godot::Texture2DRD> rd_texture = texture;
	GDEX_ASSERT_TRUE(rd_texture.is_valid());
	const godot::RID device_rid = rd_texture->get_texture_rd_rid();
	GDEX_ASSERT_TRUE(device_rid.is_valid());

	const godot::PackedByteArray readback = rd->texture_get_data(device_rid, 0);
	GDEX_ASSERT_EQ(readback.size(), source.size());
	if (readback.size() == source.size()) {
		const bool identical = memcmp(readback.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0;
		GDEX_EXPECT_TRUE(identical);
		if (!identical) {
			const int64_t mip1_start = 512 * 512 * 4;
			int64_t first = -1;
			for (int64_t i = 0; i < readback.size(); ++i) {
				if (readback[i] != source[i]) {
					first = i;
					break;
				}
			}
			GDEX_FAIL(std::string("readback differs from the source at byte ") + std::to_string(first) +
					" (mip 0 ends at " + std::to_string(mip1_start) + ")");
		}
	}
}

