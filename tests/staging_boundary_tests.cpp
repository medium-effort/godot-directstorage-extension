// Staging boundary matrix: the lowered default must load the assets that now
// matter (chunked, 8 MiB chunks) and must still fail loudly when a request is
// genuinely larger than the staging buffer.
//
// The GPU rows run through the real loader: a loader that ignored chunks would
// decode chunk bytes as one stream and fail the byte compare, so readback is the
// assertion. The failure rows drive DSRequest directly because the loader
// intentionally falls back to CPU decompression (which has no staging limit) and
// would mask the backend rejection.
//
// SetStagingBufferSize is locked after init, so each row rebuilds the context
// with its own value (shutdown -> ProjectSettings -> initialize) and restores the
// prior state. Headless rows pin the format-level requirement instead.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "core/ds_request.h"
#include "formats/dstex_format.h"
#include "integration/ds_resource_format_loader.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
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

constexpr int kFourK = 4096;
constexpr int kTwoK = 2048;
constexpr const char *kStagingKey = "dsgd_extension/staging_buffer_size_mb";
constexpr const char *kUmaKey = "dsgd_extension/auto_tune_for_integrated_gpu";
constexpr const char *kLowVramKey = "dsgd_extension/auto_tune_for_low_vram_gpu";

struct StagingScope {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	bool had_staging = false;
	bool had_uma = false;
	bool had_vram = false;
	godot::Variant prev_staging;
	godot::Variant prev_uma;
	godot::Variant prev_vram;

	StagingScope() {
		if (ps != nullptr) {
			had_staging = ps->has_setting(kStagingKey);
			had_uma = ps->has_setting(kUmaKey);
			had_vram = ps->has_setting(kLowVramKey);
			prev_staging = had_staging ? ps->get_setting(kStagingKey) : godot::Variant();
			prev_uma = had_uma ? ps->get_setting(kUmaKey) : godot::Variant();
			prev_vram = had_vram ? ps->get_setting(kLowVramKey) : godot::Variant();
		}
	}

	void apply(uint32_t p_mb) {
		if (ds == nullptr || ps == nullptr) {
			return;
		}
		if (ds->is_initialized()) {
			ds->shutdown_ds();
		}
		ps->set_setting(kStagingKey, static_cast<int64_t>(p_mb));
		// Deterministic effective value: the tests state the staging size, and
		// the auto-tune caps are covered by staging_default.
		ps->set_setting(kUmaKey, false);
		ps->set_setting(kLowVramKey, false);
		ds->initialize_ds();
	}

	~StagingScope() {
		if (ds == nullptr || ps == nullptr) {
			return;
		}
		if (ds->is_initialized()) {
			ds->shutdown_ds();
		}
		auto restore = [this](const char *p_key, bool p_had, const godot::Variant &p_prev) {
			if (p_had) {
				ps->set_setting(p_key, p_prev);
			} else {
				ps->clear(p_key);
			}
		};
		restore(kStagingKey, had_staging, prev_staging);
		restore(kUmaKey, had_uma, prev_uma);
		restore(kLowVramKey, had_vram, prev_vram);
		ds->initialize_ds();
	}
};

godot::PackedByteArray make_noise(int p_width, int p_height, int p_mips, int p_format) {
	godot::PackedByteArray data;
	int w = p_width;
	int h = p_height;
	for (int mip = 0; mip < p_mips; ++mip) {
		int64_t bytes = 0;
		if (p_format == godot::Image::FORMAT_RGBA8) {
			bytes = static_cast<int64_t>(w) * static_cast<int64_t>(h) * 4;
		} else {
			// DXT5: 16 bytes per 4x4 block.
			bytes = static_cast<int64_t>(std::max(1, (w + 3) / 4)) *
					static_cast<int64_t>(std::max(1, (h + 3) / 4)) * 16;
		}
		const int64_t start = data.size();
		data.resize(start + bytes);
		uint8_t *dst = data.ptrw() + start;
		uint32_t state = 0x9E3779B9u ^ static_cast<uint32_t>(mip * 7919);
		for (int64_t i = 0; i < bytes; ++i) {
			state = state * 1664525u + 1013904223u;
			dst[i] = static_cast<uint8_t>(state >> 24);
		}
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

bool write_texture(const char *p_path, const godot::PackedByteArray &p_data, int p_width, int p_height,
		int p_format, int p_mips, int p_chunk_kb, bool p_as_image_texture = false) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), p_data, p_width, p_height, p_format, p_mips, 1,
			false, p_as_image_texture, 0, 0, 0, p_chunk_kb) == godot::OK;
}

godot::Ref<godot::Texture2D> load_texture(const char *p_path) {
	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	if (!loader.is_valid()) {
		return godot::Ref<godot::Texture2D>();
	}
	godot::Variant result = loader->_load(godot::String(p_path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	return godot::Ref<godot::Texture2D>(result);
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

// Reads the device texture behind a Texture2DRD and byte-compares it.
bool gpu_readback_matches(const godot::Ref<godot::Texture2D> &p_texture, const godot::PackedByteArray &p_source) {
	godot::Ref<godot::Texture2DRD> rd_texture = p_texture;
	if (!rd_texture.is_valid()) {
		return false;
	}
	const godot::RID rid = rd_texture->get_texture_rd_rid();
	if (!rid.is_valid()) {
		return false;
	}
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		return false;
	}
	const godot::PackedByteArray readback = rd->texture_get_data(rid, 0);
	return readback.size() == p_source.size() &&
			memcmp(readback.ptr(), p_source.ptr(), static_cast<size_t>(p_source.size())) == 0;
}

bool gpu_ready(::gdextest::TestContext &ctx) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable.");
	}
	if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver).");
	}
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	if (rs == nullptr || rs->get_rendering_device() == nullptr) {
		GDEX_SKIP("No RenderingDevice for the GPU staging load.");
	}
	return true;
}

} // namespace

GDEX_TEST_T(staging_boundary, gpu_four_k_chunked_loads_at_default_32, TAG_INTEGRATION | TAG_SLOW) {
	if (!gpu_ready(ctx)) {
		return;
	}
	StagingScope scope;
	scope.apply(32);
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (!ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable after staging re-init.");
	}
	const char *path = "user://gdextest_staging_4k.dstex";
	track_cleanup(ctx, path);
	// 64 MiB mip: the size the old 128 MiB default existed for, split into eight
	// 8 MiB chunks by the import default.
	const godot::PackedByteArray source = make_noise(kFourK, kFourK, 1, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(write_texture(path, source, kFourK, kFourK, godot::Image::FORMAT_RGBA8, 1, 8192));
	const godot::Ref<godot::Texture2D> texture = load_texture(path);
	GDEX_ASSERT_TRUE(texture.is_valid());
	GDEX_ASSERT_TRUE(gpu_readback_matches(texture, source));
}

GDEX_TEST_T(staging_boundary, gpu_two_k_chunked_loads_at_16, TAG_INTEGRATION | TAG_SLOW) {
	if (!gpu_ready(ctx)) {
		return;
	}
	StagingScope scope;
	scope.apply(16);
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (!ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable after staging re-init.");
	}
	const char *path = "user://gdextest_staging_2k.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_noise(kTwoK, kTwoK, 1, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(write_texture(path, source, kTwoK, kTwoK, godot::Image::FORMAT_RGBA8, 1, 8192));
	const godot::Ref<godot::Texture2D> texture = load_texture(path);
	GDEX_ASSERT_TRUE(texture.is_valid());
	GDEX_ASSERT_TRUE(gpu_readback_matches(texture, source));
}

GDEX_TEST_T(staging_boundary, bc3_chunked_cpu_round_trip, TAG_INTEGRATION | TAG_SLOW) {
	if (!gpu_ready(ctx)) {
		return;
	}
	StagingScope scope;
	scope.apply(16);
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (!ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable after staging re-init.");
	}
	const char *path = "user://gdextest_staging_bc3.dstex";
	track_cleanup(ctx, path);
	// 16 MiB of DXT5 in 1 MiB chunks (16 block-rows each): every chunk boundary
	// must stay on the 4-row block boundary, and the CPU consumer reassembles the
	// per-chunk streams into the mip footprint. RD readback is format-sensitive
	// for compressed textures, so this row asserts on the image data instead.
	const godot::PackedByteArray source = make_noise(kFourK, kFourK, 1, godot::Image::FORMAT_DXT5);
	GDEX_ASSERT_TRUE(write_texture(path, source, kFourK, kFourK, godot::Image::FORMAT_DXT5, 1, 1024, true));
	const godot::Ref<godot::Texture2D> texture = load_texture(path);
	GDEX_ASSERT_TRUE(texture.is_valid());
	godot::Ref<godot::ImageTexture> image_texture = texture;
	GDEX_ASSERT_TRUE(image_texture.is_valid());
	const godot::Ref<godot::Image> image = image_texture->get_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	const godot::PackedByteArray decoded = image->get_data();
	GDEX_ASSERT_EQ(decoded.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(decoded.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}

GDEX_TEST_T(staging_boundary, chunk_larger_than_staging_fails, TAG_INTEGRATION | TAG_SLOW) {
	if (!gpu_ready(ctx)) {
		return;
	}
	StagingScope scope;
	scope.apply(6);
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (!ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable after staging re-init.");
	}
	const char *path = "user://gdextest_staging_fail.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_noise(kTwoK, kTwoK, 1, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(write_texture(path, source, kTwoK, kTwoK, godot::Image::FORMAT_RGBA8, 1, 8192));
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_ASSERT_TRUE(static_cast<bool>(header["chunked"]));
	const godot::PackedInt64Array offsets = header["chunk_offsets"];
	const godot::PackedInt32Array compressed = header["chunk_compressed_sizes"];
	const godot::PackedInt32Array uncompressed = header["chunk_uncompressed_sizes"];
	GDEX_ASSERT_TRUE(offsets.size() > 0);

	godot::Ref<godot::DSRequest> request;
	request.instantiate();
	request->set_file_path(godot::String(path));
	request->set_offset(static_cast<uint64_t>(offsets[0]));
	request->set_size(static_cast<uint32_t>(compressed[0]));
	request->set_uncompressed_size(static_cast<uint32_t>(uncompressed[0]));
	request->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
	// The backend may reject an oversized chunk at enqueue (chunked files cannot
	// be re-split by the legacy staging splitter) or the runtime rejects it on
	// completion. Both must end FAILED with an actionable message.
	if (ds->enqueue_request(request, false)) {
		ds->flush_batch();
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
	const godot::String error = request->get_error_message();
	GDEX_EXPECT_TRUE(!error.is_empty());
	GDEX_EXPECT_TRUE(error.to_lower().contains("staging") || error.to_lower().contains("too_large"));
}

GDEX_TEST_T(staging_boundary, legacy_requirement_exceeds_default, TAG_INTEGRATION) {
	// An un-chunked 4096x4096 RGBA8 texture still needs a 64 MiB request; the
	// importer must be able to see that it no longer fits the 32 MiB default.
	const char *path = "user://gdextest_staging_legacy.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_noise(kFourK, kFourK, 1, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(write_texture(path, source, kFourK, kFourK, godot::Image::FORMAT_RGBA8, 1, 0));
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_FALSE(static_cast<bool>(header["chunked"]));
	const int64_t required = static_cast<int64_t>(header["min_staging_bytes"]);
	GDEX_EXPECT_TRUE(required > static_cast<int64_t>(godot::DSContext::DEFAULT_STAGING_BUFFER_SIZE_MB) * 1024 * 1024);
}

GDEX_TEST_T(staging_boundary, cpu_two_k_chunked_loads_at_default, TAG_INTEGRATION) {
	// The CPU path has no staging limit, so the lowered default must not change
	// anything there (as_image_texture forces the CPU consumer).
	const char *path = "user://gdextest_staging_cpu.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_noise(kTwoK, kTwoK, 1, godot::Image::FORMAT_RGBA8);
	GDEX_ASSERT_TRUE(write_texture(path, source, kTwoK, kTwoK, godot::Image::FORMAT_RGBA8, 1, 8192, true));
	const godot::Ref<godot::Texture2D> texture = load_texture(path);
	GDEX_ASSERT_TRUE(texture.is_valid());
	godot::Ref<godot::ImageTexture> image_texture = texture;
	GDEX_ASSERT_TRUE(image_texture.is_valid());
	const godot::Ref<godot::Image> image = image_texture->get_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	const godot::PackedByteArray decoded = image->get_data();
	GDEX_ASSERT_EQ(decoded.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(decoded.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}
