// Chunked prewarm (CPU path): the preloader must expand one request per chunk
// and reassemble the chunk buffers into a footprint per mip before building the
// texture.
//
// This test drives prewarm_async(), which always takes the memory/CPU route.
// Chunked payloads are also GPU-eligible through prewarm_gpu_async() (per-chunk
// row-range requests; see gpu_prewarm.chunked_dma_fills_vram_texture).
// Headless-safe: it uses the memory/CPU route.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
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

godot::PackedByteArray read_bytes(const char *p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return godot::PackedByteArray();
	}
	godot::PackedByteArray bytes = file->get_buffer(static_cast<int64_t>(file->get_length()));
	file->close();
	return bytes;
}

bool write_bytes(const char *p_path, const godot::PackedByteArray &p_bytes) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->store_buffer(p_bytes);
	// store_buffer() returns void in godot-cpp; the error is only observable here.
	const bool ok = file->get_error() == godot::OK;
	file->close();
	return ok;
}

void put_u32(godot::PackedByteArray &r_bytes, int64_t p_offset, uint32_t p_value) {
	memcpy(r_bytes.ptrw() + p_offset, &p_value, sizeof(p_value));
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

// A chunked .dstex whose chunk table is corrupt must fail closed during
// resolution: no payload, no request, nothing tracked. Before that contract
// existed the preloader reclassified the file as unchunked, because a v3 mip
// table already stores the first chunk offset and the summed chunk sizes, so
// the downgraded payload looked valid and expanded to ONE whole-mip request
// over concatenated per-chunk streams (see the 4-vs-1 progress split below).
// Headless-safe and backend-independent: the rejection happens before a
// backend or a GPU path is selected, so the GPU entry point must agree.
GDEX_TEST_T(chunked_prewarm, corrupt_chunk_table_tracks_nothing, TAG_INTEGRATION) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}

	const char *valid_path = "user://gdextest_chunked_corrupt_src.dstex";
	const char *corrupt_path = "user://gdextest_chunked_corrupt_patched.dstex";
	// Registered before the cache: teardowns run LIFO, so the cache is torn
	// down before its request paths disappear.
	track_cleanup(ctx, valid_path);
	track_cleanup(ctx, corrupt_path);

	godot::Ref<godot::DSTex> writer;
	writer.instantiate();
	GDEX_ASSERT_EQ((int64_t)writer->write_to_file(godot::String(valid_path), make_noise(), kSize, kSize,
						   godot::Image::FORMAT_RGBA8, kMipCount, 1, false, false, 0, 0, 0, 256),
			(int64_t)godot::OK);

	const godot::Dictionary valid_header = writer->read_header(godot::String(valid_path));
	GDEX_ASSERT_TRUE(!valid_header.is_empty());
	GDEX_ASSERT_TRUE(static_cast<bool>(valid_header["chunked"]));
	const godot::PackedInt32Array counts = valid_header["mip_chunk_counts"];
	GDEX_ASSERT_EQ((int64_t)counts[0], (int64_t)16);

	// Chunk table follows the v3 header + one 24-byte mip entry; each
	// DSChunkEntry is offset(u64) / compressed_size(u32) / uncompressed_size(u32).
	const int64_t chunk_table = (int64_t)sizeof(godot::DSTexHeaderV3) + (int64_t)sizeof(godot::DSTexMipEntryV2);
	godot::PackedByteArray bytes = read_bytes(valid_path);
	GDEX_ASSERT_TRUE(bytes.size() > chunk_table + 16);
	put_u32(bytes, chunk_table + 12, 0); // entry[0].uncompressed_size
	GDEX_ASSERT_TRUE(write_bytes(corrupt_path, bytes));
	GDEX_ASSERT_TRUE(writer->read_header(godot::String(corrupt_path)).is_empty());

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->clear();
	godot::DSPreloader::shutdown();

	godot::PackedStringArray valid_paths;
	valid_paths.push_back(godot::String(valid_path));
	const uint64_t control_tag = godot::DSPreloader::prewarm_async(valid_paths);
	GDEX_ASSERT_TRUE(control_tag != 0);
	godot::PackedStringArray corrupt_paths;
	corrupt_paths.push_back(godot::String(corrupt_path));
	const uint64_t tag = godot::DSPreloader::prewarm_async(corrupt_paths);
	GDEX_ASSERT_TRUE(tag != 0);
	const uint64_t tag_gpu = godot::DSPreloader::prewarm_gpu_async(corrupt_paths, 16, true);
	GDEX_ASSERT_TRUE(tag_gpu != 0);
	ctx.add_teardown([ds, cache, control_tag, tag, tag_gpu]() {
		ds->cancel_prewarm(control_tag);
		ds->cancel_prewarm(tag);
		ds->cancel_prewarm(tag_gpu);
		godot::DSPreloader::shutdown();
		cache->clear();
		memdelete(cache);
	});

	// Positive control: the same writer output, unpatched, still expands one
	// request per chunk. Only the denominator is asserted: the completed count
	// may already be moving on FallbackBackend workers by the time this line
	// runs, while the total is fixed at resolution time and is exactly what
	// separates "chunked" from "downgraded" (16 requests vs 1). Without some
	// control a zero total below could just mean the fixture never resolved.
	const godot::Vector2i control = ds->get_prewarm_progress(control_tag);
	GDEX_EXPECT_EQ(static_cast<int64_t>(control.y), (int64_t)16);

	// The corrupt file is rejected during resolution: nothing tracked, nothing
	// pending, no stream work, no texture, and it is not marked in flight.
	GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(tag), 0);
	GDEX_EXPECT_EQ(godot::DSPreloader::get_pending_prewarm_count(tag), 0);
	const godot::Vector2i progress = ds->get_prewarm_progress(tag);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.x), (int64_t)0);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.y), (int64_t)0);
	GDEX_EXPECT_FALSE(cache->get_cached(godot::String(corrupt_path), godot::DSTextureCache::CACHE_MODE_REUSE).is_valid());
	GDEX_EXPECT_FALSE(godot::DSPreloader::is_texture_pending(godot::String(corrupt_path)));

	// The GPU entry point shares resolution, so it must agree.
	GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(tag_gpu), 0);
	const godot::Vector2i gpu_progress = ds->get_prewarm_progress(tag_gpu);
	GDEX_EXPECT_EQ(static_cast<int64_t>(gpu_progress.x), (int64_t)0);
	GDEX_EXPECT_EQ(static_cast<int64_t>(gpu_progress.y), (int64_t)0);
}

// The paced CPU path locks its tag total at resolution time through
// count_cpu_requests_for_payload(). That helper used to count mips only, while
// expand_requests() emits one request per chunk for a chunked v3 payload, so a
// 4K/8K texture reported complete while the remaining chunks were still
// streaming. No stream work is pumped here: the total is fixed synchronously
// by prewarm_cpu_paced_async(), which keeps the red/green deterministic and
// headless-safe.
GDEX_TEST_T(chunked_prewarm, paced_cpu_prewarm_counts_chunks, TAG_INTEGRATION) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	if (!ds->is_initialized() && !ds->initialize_ds()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}

	const char *path = "user://gdextest_chunked_paced_prewarm.dstex";
	track_cleanup(ctx, path);

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
	GDEX_ASSERT_EQ(expected_requests, (int64_t)16);

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->clear();
	godot::DSPreloader::shutdown();

	godot::PackedStringArray paths;
	paths.push_back(godot::String(path));
	const uint64_t tag = godot::DSPreloader::prewarm_cpu_paced_async(paths);
	GDEX_ASSERT_TRUE(tag != 0);
	ctx.add_teardown([ds, cache, tag]() {
		ds->cancel_prewarm(tag);
		godot::DSPreloader::shutdown();
		cache->clear();
		memdelete(cache);
	});

	// Nothing has been pumped yet, so no completion can race the total.
	// At HEAD this is mip_count (1); with the fix it is the chunk count (16).
	GDEX_EXPECT_EQ(godot::DSPreloader::get_queued_cpu_prewarm_count(tag), 1);
	const godot::Vector2i progress = ds->get_prewarm_progress(tag);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.x), (int64_t)0);
	GDEX_EXPECT_EQ(static_cast<int64_t>(progress.y), expected_requests);
}
