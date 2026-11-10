// Chunked prewarm (CPU path): the preloader must expand one request per chunk
// and reassemble the chunk buffers into a footprint per mip before building the
// texture.
//
// Chunked GPU prewarm is intentionally CPU-routed for now (see
// is_gpu_prewarm_eligible), so this is the prewarm path every chunked asset takes
// by default. Headless-safe: it uses the memory/CPU route.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kSize = 1024;
constexpr int kMipCount = 1;

godot::PackedByteArray make_noise() {
	const int64_t bytes = static_cast<int64_t>(kSize) * kSize * 4;
	godot::PackedByteArray data;
	data.resize(bytes);
	uint8_t *dst = data.ptrw();
	uint32_t state = 0x9E3779B9u;
	for (int64_t i = 0; i < bytes; ++i) {
		state = state * 1664525u + 1013904223u;
		dst[i] = static_cast<uint8_t>(state >> 24);
	}
	return data;
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

GDEX_TEST_T(chunked_prewarm, cpu_prewarm_expands_chunks_and_reassembles, TAG_INTEGRATION) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->clear();
	// The singleton can be absent after a suite that created and deleted its own
	// cache, so this test owns one (same pattern as the GPU prewarm suite).
	godot::DSPreloader::shutdown();

	const char *path = "user://gdextest_chunked_prewarm.dstex";
	track_cleanup(ctx, path);

	// 1024x1024 RGBA8 with 256 KiB chunks: 64 rows per chunk, so 16 chunks for
	// the single mip. One request per chunk is what the test must observe.
	const godot::PackedByteArray source = make_noise();
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	GDEX_ASSERT_EQ(dstex->write_to_file(godot::String(path), source, kSize, kSize,
						   godot::Image::FORMAT_RGBA8, kMipCount, 1, false, false, 0, 0, 0, 256),
			godot::OK);
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_ASSERT_TRUE(static_cast<bool>(header["chunked"]));
	const godot::PackedInt32Array counts = header["mip_chunk_counts"];
	int64_t expected_requests = 0;
	for (int64_t mip = 0; mip < counts.size(); ++mip) {
		expected_requests += counts[mip] > 1 ? counts[mip] : 1;
	}
	GDEX_ASSERT_TRUE(expected_requests > static_cast<int64_t>(kMipCount));

	godot::OS *os = godot::OS::get_singleton();
	godot::Time *time = godot::Time::get_singleton();
	if (os == nullptr || time == nullptr) {
		GDEX_SKIP("OS/Time singletons unavailable.");
	}

	godot::PackedStringArray paths;
	paths.push_back(godot::String(path));
	const uint64_t tag = godot::DSPreloader::prewarm_async(paths);
	GDEX_ASSERT_TRUE(tag != 0);
	ctx.add_teardown([ds, cache, tag]() {
		ds->cancel_prewarm(tag);
		godot::DSPreloader::shutdown();
		cache->clear();
		memdelete(cache);
	});

	// Same pump the batch manager performs: flush stream work, harvest completed
	// results, drain prewarmed textures into the cache.
	const uint64_t deadline = time->get_ticks_msec() + 20000;
	bool settled = false;
	while (time->get_ticks_msec() < deadline) {
		if (ds->has_stream_pending()) {
			ds->flush_stream_async();
		}
		ds->collect_completed_results();
		cache->poll_completed(tag, 0, 0);
		if (ds->is_prewarm_complete(tag)) {
			settled = true;
			break;
		}
		os->delay_msec(10);
	}
	if (!settled) {
		const godot::Vector2i progress = ds->get_prewarm_progress(tag);
		GDEX_FAIL(std::string("chunked prewarm did not settle (done=") + std::to_string(progress.x) +
				", total=" + std::to_string(progress.y) + ").");
		return;
	}
	// Final harvest: is_prewarm_complete() can flip between poll_completed() and
	// the check above, and a CPU build that exceeds the poll budget is handed to
	// the WorkerThreadPool. Keep ticking like the batch manager until the
	// texture is resident or the grace window expires.
	godot::Ref<godot::Texture2D> texture;
	const uint64_t cache_deadline = time->get_ticks_msec() + 10000;
	while (time->get_ticks_msec() < cache_deadline) {
		ds->collect_completed_results();
		cache->poll_completed(tag, 0, 4000);
		texture = cache->get_cached(godot::String(path), godot::DSTextureCache::CACHE_MODE_REUSE);
		if (texture.is_valid()) {
			break;
		}
		os->delay_msec(10);
	}

	// One request per chunk: the tag's denominator must equal the chunk count,
	// not the mip count.
	const godot::Vector2i progress = ds->get_prewarm_progress(tag);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.y), expected_requests);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.x), static_cast<int64_t>(progress.y));

	if (!texture.is_valid()) {
		GDEX_FAIL(std::string("prewarm texture not cached: enabled=") + (cache->is_enabled() ? "1" : "0") +
				" cached_entries=" + std::to_string(cache->get_cached_count()) +
				" unbuilt_cpu=" + std::to_string(godot::DSPreloader::get_unbuilt_cpu_prewarm_count(tag)) +
				" unbuilt_total=" + std::to_string(godot::DSPreloader::get_unbuilt_prewarm_count(tag)) +
				" progress=(" + std::to_string(progress.x) + "," + std::to_string(progress.y) + ")");
		return;
	}
	GDEX_ASSERT_TRUE(texture.is_valid());
	godot::Ref<godot::ImageTexture> image_texture = texture;
	GDEX_ASSERT_TRUE(image_texture.is_valid());
	const godot::Ref<godot::Image> image = image_texture->get_image();
	GDEX_ASSERT_TRUE(image.is_valid());
	const godot::PackedByteArray decoded = image->get_data();
	GDEX_ASSERT_EQ(decoded.size(), source.size());
	GDEX_EXPECT_TRUE(memcmp(decoded.ptr(), source.ptr(), static_cast<size_t>(source.size())) == 0);
}
