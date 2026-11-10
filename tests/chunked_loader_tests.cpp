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

#include <algorithm>
#include <cstring>

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
