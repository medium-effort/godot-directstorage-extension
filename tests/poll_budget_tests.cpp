// Slice-budget closure for DSPreloader::poll_completed().
//
// The defect: the pre-estimate defer is gated on `processed > 0`, so the first
// terminal entry is built on the calling thread whatever it costs. On the main
// thread a 4096x4096 RGBA8 build costs ~24 ms warm on a GTX 1050 Ti (measured:
// 14 ms 64 MiB resize + ~8 ms ImageTexture::create_from_image), against a 2-4 ms
// slice. The fix hands a CPU entry that does not fit the remaining slice to
// WorkerThreadPool instead of building it inline.
//
// These cases need no backend, no disk I/O and no real stream: the friend hook
// injects an already-terminal CPU entry straight into DSPreloader::state().pending
// (the same mechanism tests/preloader_unpack_tests.cpp uses for #0137).

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_request.h"
#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <mutex>
#include <vector>

namespace godot {

struct PrewarmBudgetTest {
	static int64_t inject_terminal_cpu_entry(uint64_t p_tag, const DSPreloader::Payload &p_meta,
			const PackedByteArray &p_payload) {
		Ref<DSRequest> req;
		req.instantiate();
		req->set_status(DSRequest::STATUS_COMPLETED);
		req->get_destination_buffer_ref() = p_payload;

		DSPreloader::Tracked tracked;
		tracked.tag = p_tag;
		tracked.meta = p_meta;
		tracked.is_gpu = false;
		tracked.requests.push_back(req);
		return publish(tracked);
	}

	// GPU entry already marked failed: poll_completed() drops it through
	// complete_gpu_entry() without storing, but still charges its estimate to
	// the slice byte budget. Used to pin that the first-item exemption survives
	// only for constant-cost GPU entries.
	static int64_t inject_gpu_failed_entry(uint64_t p_tag, const DSPreloader::Payload &p_meta) {
		DSPreloader::Tracked tracked;
		tracked.tag = p_tag;
		tracked.meta = p_meta;
		tracked.is_gpu = true;
		tracked.gpu_failed = true;
		return publish(tracked);
	}

private:
	static int64_t publish(DSPreloader::Tracked &p_tracked) {
		std::lock_guard<std::mutex> lock(DSPreloader::state().mutex);
		DSPreloader::State &s = DSPreloader::state();
		int64_t id = s.next_id++;
		if (s.next_id == 0) {
			s.next_id = 1;
		}
		s.pending[id] = p_tracked;
		return id;
	}
};

} // namespace godot

namespace {

constexpr int kBigDim = 4096;
constexpr uint64_t kBigBytes = 64ULL * 1024ULL * 1024ULL; // 4096*4096*4

// Payload for the pool warm-up below.
void noop_native_task(void *) {}

// The first native task in a process can pay worker-thread wake-up, which the
// full suite charged to the measured slice (observed 9.9 ms under memory
// pressure) even though posting is O(1). Warm the pool so the timed call only
// measures the fit test + post.
void warm_worker_pool() {
	godot::WorkerThreadPool *pool = godot::WorkerThreadPool::get_singleton();
	if (pool == nullptr) {
		return;
	}
	const int64_t warm = pool->add_native_task(&noop_native_task, nullptr, false, "poll_budget warmup");
	if (warm != static_cast<int64_t>(godot::WorkerThreadPool::INVALID_TASK_ID)) {
		pool->wait_for_task_completion(warm);
	}
}

godot::DSTextureCache *make_budget_cache(::gdextest::TestContext &ctx) {
	// The ctor installs the static singleton and the dtor nulls it, so each test
	// owns its cache for its whole body (see preloader_unpack_tests.cpp).
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	cache->set_enabled(true);
	cache->clear();
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});
	return cache;
}

godot::DSPreloader::Payload make_payload(const char *p_path, int p_dim) {
	godot::DSPreloader::Payload meta;
	meta.engine_path = godot::String(p_path);
	meta.payload_key = meta.engine_path;
	meta.width = static_cast<uint32_t>(p_dim);
	meta.height = static_cast<uint32_t>(p_dim);
	meta.mip_count = 1;
	meta.pixel_format = godot::DSTex::godot_format_to_rd_format(godot::Image::FORMAT_RGBA8);
	meta.is_v2 = true;
	return meta;
}

// Waits (bounded) for every entry of the tag to leave the pending table. With
// the offload in place the worker erases the entry when its build finishes, so
// this is the "the work still happened" guard; at HEAD the entry is already gone.
bool wait_until_unbuilt(godot::DSTextureCache *p_cache, uint64_t p_tag, int p_timeout_ms) {
	godot::OS *os = godot::OS::get_singleton();
	for (int elapsed = 0; elapsed <= p_timeout_ms; elapsed += 2) {
		if (p_cache->get_unbuilt_prewarm_count(p_tag) == 0) {
			return true;
		}
		if (os != nullptr) {
			os->delay_msec(2);
		}
	}
	return p_cache->get_unbuilt_prewarm_count(p_tag) == 0;
}

} // namespace

// The closure case. Fails at HEAD on three asserts: HEAD stores the entry
// inline (stored == 1), takes far longer than the budget, and leaves nothing
// pending.
GDEX_TEST_T(poll_budget, first_cpu_item_over_budget_is_offloaded, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013AULL;

	godot::DSPreloader::Payload meta = make_payload("user://gdextest_poll_budget_big.png", kBigDim);
	meta.original_size = kBigBytes;
	meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kBigBytes));

	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(kBigBytes));
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	GDEX_ASSERT_EQ(cache->get_unbuilt_prewarm_count(tag), 1);
	warm_worker_pool();

	const uint64_t t0 = godot::Time::get_singleton()->get_ticks_usec();
	const int stored = cache->poll_completed(tag, 0, 500);
	const uint64_t elapsed = godot::Time::get_singleton()->get_ticks_usec() - t0;

	GDEX_EXPECT_EQ(stored, 0); // the slice stored nothing; the build left the thread
	GDEX_EXPECT_LT(elapsed, static_cast<uint64_t>(3500)); // 500 us budget + 3 ms tolerance
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 1); // still held, in flight

	GDEX_EXPECT_TRUE(wait_until_unbuilt(cache, tag, 5000));
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_poll_budget_big.png").is_valid());
}

// Control: a small entry still builds inline in the same call. Guards against a
// "defer everything" fix that would pass the closure case by never building.
GDEX_TEST_T(poll_budget, small_first_cpu_item_builds_inline, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013BULL;

	godot::DSPreloader::Payload meta = make_payload("user://gdextest_poll_budget_small.png", 256);
	meta.height = 4;
	meta.original_size = 4096;
	meta.mip_uncompressed_sizes.push_back(4096);

	godot::PackedByteArray data;
	data.resize(4096);
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);

	GDEX_EXPECT_EQ(cache->poll_completed(tag, 0, 2000), 1);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_poll_budget_small.png").is_valid());
}

// Pins the default-argument half: with no arguments a direct API caller must
// still get a bounded slice, not an unbounded inline build. Fails at HEAD
// because p_max_usec defaults to 0.
GDEX_TEST_T(poll_budget, default_budget_is_bounded, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013CULL;

	godot::DSPreloader::Payload meta = make_payload("user://gdextest_poll_budget_default.png", kBigDim);
	meta.original_size = kBigBytes;
	meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kBigBytes));

	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(kBigBytes));
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	warm_worker_pool();

	const uint64_t t0 = godot::Time::get_singleton()->get_ticks_usec();
	const int stored = cache->poll_completed(tag);
	const uint64_t elapsed = godot::Time::get_singleton()->get_ticks_usec() - t0;

	GDEX_EXPECT_EQ(stored, 0);
	GDEX_EXPECT_LT(elapsed, static_cast<uint64_t>(8000)); // 4 ms default + 4 ms tolerance
	GDEX_EXPECT_TRUE(wait_until_unbuilt(cache, tag, 5000));
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_poll_budget_default.png").is_valid());
}

// Guard: the first-item exemption survives for GPU entries, whose completion is
// constant-cost and cannot leave the render thread. Passes at HEAD by design.
GDEX_TEST_T(poll_budget, gpu_first_item_keeps_its_exemption, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013DULL;

	godot::DSPreloader::Payload meta = make_payload("user://gdextest_poll_budget_gpu.png", kBigDim);
	meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kBigBytes));
	godot::PrewarmBudgetTest::inject_gpu_failed_entry(tag, meta);

	const int stored = cache->poll_completed(tag, 0, 500);

	GDEX_EXPECT_EQ(stored, 0);
	// The discriminating assert: a GPU entry is consumed in this slice even
	// though its estimate exceeds the budget. If the fix ever offloads it (or
	// defers it), it stays pending and this fails. No elapsed bound here: on a
	// real D3D12 host the main thread can block on render-thread work, which
	// makes a tight timing assert flaky without adding discrimination.
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
}
