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

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
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
// 4096x1536 RGBA8 = 25165824 bytes (24 MiB): two of these fit the 64 MiB slice, three do not.
constexpr uint64_t kUnboundedEntryBytes = 4096ULL * 1536ULL * 4ULL;
// 4096x4608 RGBA8 = 75497472 bytes (72 MiB): a single entry larger than the whole slice.
constexpr uint64_t kEssentialEntryBytes = 4096ULL * 4608ULL * 4ULL;

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

// --- Unbounded branch (p_max_usec == 0): bounded in bytes, not in time -------
//
// The unbounded branch used to OR the fit test away, so the only bound on the slice was the loop-top
// 64 MiB check, which runs after the entry that crossed it. Three 24 MiB entries make that
// observable: 48 MiB fits, adding the third (72 MiB) does not, so the branch must take two and hand
// the third to WorkerThreadPool. `stored` counts this call's inline stores only, so an offloaded
// build cannot satisfy it, and nothing here races the worker.
GDEX_TEST_T(poll_budget, unbounded_pump_defers_over_cap_cpu_entry, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013EULL;

	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(kUnboundedEntryBytes));
	uint8_t *w = data.ptrw();
	for (int64_t i = 0; i < data.size(); i += 4093) {
		w[i] = static_cast<uint8_t>(i & 0xFF);
	}

	const char *paths[3] = {
		"user://gdextest_poll_unbounded_a.png",
		"user://gdextest_poll_unbounded_b.png",
		"user://gdextest_poll_unbounded_c.png",
	};
	for (int i = 0; i < 3; ++i) {
		godot::DSPreloader::Payload meta = make_payload(paths[i], 4096);
		meta.height = 1536;
		meta.original_size = kUnboundedEntryBytes;
		meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kUnboundedEntryBytes));
		godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	}
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 3);
	warm_worker_pool();

	const int stored = cache->poll_completed(tag, 0, 0);

	// The invariant: only what the pre-estimate admits is built on this thread.
	GDEX_EXPECT_EQ(stored, 2);
	GDEX_EXPECT_TRUE(wait_until_unbuilt(cache, tag, 5000));
	for (int i = 0; i < 3; ++i) {
		GDEX_EXPECT_TRUE(cache->get_cached(godot::String(paths[i])).is_valid());
	}
}

// Control: one 4 KiB entry still builds inline under the unbounded pump, so a fix that defers every
// entry would fail here instead of passing the case above.
GDEX_TEST_T(poll_budget, unbounded_pump_small_entry_still_builds_inline, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF013FULL;

	godot::DSPreloader::Payload meta = make_payload("user://gdextest_poll_unbounded_small.png", 256);
	meta.height = 4;
	meta.original_size = 4096;
	meta.mip_uncompressed_sizes.push_back(4096);

	godot::PackedByteArray data;
	data.resize(4096);
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);

	GDEX_EXPECT_EQ(cache->poll_completed(tag, 0, 0), 1);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
	GDEX_EXPECT_TRUE(cache->get_cached("user://gdextest_poll_unbounded_small.png").is_valid());
}

// The caller-named entry is the one exemption: a render-thread await names the texture it is about
// to look up, and deferring that build would turn the pump into a miss plus a duplicate synchronous
// load. An entry the caller names is taken inline even past the cap; the same entry with nobody
// naming it is offloaded and built on the pool.
GDEX_TEST_T(poll_budget, unbounded_pump_essential_entry_exceeds_cap, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF0140ULL;
	const char *essential = "user://gdextest_poll_essential.png";
	const char *backlog = "user://gdextest_poll_essential_backlog.png";

	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(kEssentialEntryBytes));
	uint8_t *w = data.ptrw();
	for (int64_t i = 0; i < data.size(); i += 4093) {
		w[i] = static_cast<uint8_t>(i & 0xFF);
	}

	godot::DSPreloader::Payload meta = make_payload(essential, 4096);
	meta.height = 4608;
	meta.original_size = kEssentialEntryBytes;
	meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kEssentialEntryBytes));
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	warm_worker_pool();

	// Named: built on this thread although the entry alone exceeds the slice cap.
	GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 0, 0, godot::String(essential)), 1);
	GDEX_EXPECT_TRUE(cache->get_cached(godot::String(essential)).is_valid());
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);

	// Same shape, nobody naming it: offloaded, then built on the pool.
	meta.engine_path = godot::String(backlog);
	meta.payload_key = meta.engine_path;
	godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	GDEX_EXPECT_EQ(godot::DSPreloader::poll_completed(0, 0, 0, godot::String(essential)), 0);
	GDEX_EXPECT_TRUE(wait_until_unbuilt(cache, tag, 5000));
	GDEX_EXPECT_TRUE(cache->get_cached(godot::String(backlog)).is_valid());
}

// DSRuntime's main-thread drain passes gpu_only=true so it only wraps
// Texture2DRD entries and never builds CPU ImageTextures inline.
GDEX_TEST_T(poll_budget, gpu_only_poll_leaves_cpu_entries_for_worker, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF0141ULL;

	godot::PackedByteArray small_data;
	small_data.resize(4096);
	for (int i = 0; i < 2; ++i) {
		godot::String p = godot::String("user://gdextest_poll_gpu_only_cpu_") + godot::String::num_int64(i) + ".png";
		godot::DSPreloader::Payload meta = make_payload(p.utf8().get_data(), 256);
		meta.height = 4;
		meta.original_size = 4096;
		meta.mip_uncompressed_sizes.push_back(4096);
		godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, small_data);
	}
	godot::DSPreloader::Payload gpu_meta = make_payload("user://gdextest_poll_gpu_only_gpu.png", 256);
	gpu_meta.mip_uncompressed_sizes.push_back(4096);
	godot::PrewarmBudgetTest::inject_gpu_failed_entry(tag, gpu_meta);

	const int gpu_stored = cache->poll_completed(tag, 32, 2000, true);
	GDEX_EXPECT_EQ(gpu_stored, 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_gpu_prewarm_count(tag), 0);
	GDEX_EXPECT_EQ(cache->get_unbuilt_cpu_prewarm_count(tag), 2);

	const int cpu_stored = cache->poll_completed(tag, 32, 2000, false);
	GDEX_EXPECT_EQ(cpu_stored, 2);
	GDEX_EXPECT_EQ(cache->get_unbuilt_prewarm_count(tag), 0);
}

// Regression test for the completion race: a worker thread calling poll_completed()
// must keep each entry in s.pending (build_in_flight=true) until complete_cpu_entry()
// has stored the ImageTexture into DSTextureCache, so a concurrent main-thread
// get_unbuilt_prewarm_count(tag) == 0 check never fires prewarm_finished() early.
GDEX_TEST_T(poll_budget, concurrent_worker_poll_keeps_unbuilt_until_cached, TAG_UNIT) {
	godot::DSTextureCache *cache = make_budget_cache(ctx);
	const uint64_t tag = 0x7FFF0142ULL;
	constexpr int kCount = 8;

	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(kUnboundedEntryBytes));
	uint8_t *w = data.ptrw();
	for (int64_t i = 0; i < data.size(); i += 4093) {
		w[i] = static_cast<uint8_t>(i & 0xFF);
	}

	std::vector<godot::String> paths;
	paths.reserve(kCount);
	for (int i = 0; i < kCount; ++i) {
		godot::String p = godot::String("user://gdextest_poll_race_") + godot::String::num_int64(i) + ".png";
		paths.push_back(p);
		godot::DSPreloader::Payload meta = make_payload(p.utf8().get_data(), 4096);
		meta.height = 1536;
		meta.original_size = kUnboundedEntryBytes;
		meta.mip_uncompressed_sizes.push_back(static_cast<int32_t>(kUnboundedEntryBytes));
		godot::PrewarmBudgetTest::inject_terminal_cpu_entry(tag, meta, data);
	}
	warm_worker_pool();

	std::atomic<bool> stop_worker{ false };
	std::thread worker([&]() {
		while (!stop_worker.load()) {
			cache->poll_completed(tag, 2, 4000, false);
			std::this_thread::yield();
		}
	});

	bool saw_zero = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		if (cache->get_unbuilt_prewarm_count(tag) == 0) {
			saw_zero = true;
			break;
		}
		std::this_thread::yield();
	}

	// Capture the invariant immediately at the moment unbuilt hits 0, before
	// joining the worker thread.
	const int cached_at_zero = cache->get_cached_count();
	std::vector<bool> all_cached_at_zero;
	all_cached_at_zero.reserve(kCount);
	for (int i = 0; i < kCount; ++i) {
		all_cached_at_zero.push_back(cache->get_cached(paths[i]).is_valid());
	}

	stop_worker.store(true);
	worker.join();

	GDEX_EXPECT_TRUE(saw_zero);
	GDEX_EXPECT_EQ(cached_at_zero, kCount);
	for (int i = 0; i < kCount; ++i) {
		GDEX_EXPECT_TRUE(all_cached_at_zero[i]);
	}
}
