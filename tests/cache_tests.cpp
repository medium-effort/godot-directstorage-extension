#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/color.hpp>

#include <string>
#include <thread>
#include <vector>

// Live-engine DSTextureCache suite (TAG_INTEGRATION).
//
// Drafted against the REAL src/integration/ds_texture_cache.h / .cpp —
// per the fallback rule, comments below cite header/source behavior and
// never mirror a GDScript expectation where it differs:
//
// - CAP IS ENTRY COUNT, not bytes: `max_entries` (header line 27, default
//   256). store() evicts while `cache_map.size() >= max_entries`
//   (ds_texture_cache.cpp, store()). The task wording "memory cap" is
//   therefore encoded as the entry-count cap.
// - LRU: new paths push_front, get_cached()/re-store move_to_front, eviction
//   takes lru_list.back() (the oldest). get_cached() with CACHE_MODE_REPLACE
//   always misses; store() with IGNORE/IGNORE_DEEP is a no-op; evict() is
//   idempotent; set_enabled(false) drains the cache; set_max_entries()
//   clamps <1 to 1 and evicts oldest-first while over cap.
// - LINKAGE NOTE: compile-in decided, consumer_extension NOT
//   configured, so the Engine "DSTextureCache" singleton is absent at
//   runtime. Every test memnews its OWN instance and never touches
//   DSTextureCache::get_singleton().
// - ImageTexture (never Texture2DRD) fixtures: the evict path extracts an RD
//   RID only from Texture2DRD and returns invalid for ImageTexture, whose RS
//   RID auto-frees on Ref drop — so evict/clear need no render device.
// - Per-test ctx.add_teardown() clears + frees the instance so leak
//   detection is never polluted across tests (see tests/ds_adapter.cpp).

namespace {

godot::Ref<godot::Texture2D> make_test_texture(float p_red) {
    godot::Ref<godot::Image> img =
        godot::Image::create_empty(4, 4, false, godot::Image::FORMAT_RGBA8);
    img->fill(godot::Color(p_red, 0.2f, 0.4f, 1.0f));
    godot::Ref<godot::ImageTexture> tex = godot::ImageTexture::create_from_image(img);
    return tex;
}

godot::String cache_path(const char *p_stem, int p_index) {
    return godot::String((std::string(p_stem) + std::to_string(p_index)).c_str());
}

godot::DSTextureCache *new_enabled_cache(::gdextest::TestContext &ctx) {
    godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
    cache->set_enabled(true);
    cache->set_max_entries(256);
    ctx.add_teardown([cache]() {
        cache->clear();
        memdelete(cache);
    });
    return cache;
}

} // namespace

GDEX_TEST_T(cache, disabled_cache_stores_nothing, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
    ctx.add_teardown([cache]() {
        cache->clear();
        memdelete(cache);
    });
    // Drive the state explicitly: the ctor reads ProjectSettings
    // (directstorage/texture_cache_enabled), which the test fixture does not
    // set, so never assert the default — pin it instead.
    cache->set_enabled(false);
    cache->store("res://gdextest_cache_disabled_0", make_test_texture(1.0f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_disabled_0").is_null());
}

GDEX_TEST_T(cache, lru_eviction_honors_entry_cap, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);
    cache->set_max_entries(3);

    for (int i = 0; i < 3; ++i) {
        cache->store(cache_path("res://gdextest_cache_cap_", i), make_test_texture(0.1f * i));
    }
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);

    // One past capacity: size stays capped and the OLDEST (index 0) is gone.
    cache->store(cache_path("res://gdextest_cache_cap_", 3), make_test_texture(0.9f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_cap_", 0)).is_null());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_cap_", 3)).is_valid());
}

GDEX_TEST_T(cache, get_marks_entry_most_recently_used, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);
    cache->set_max_entries(3);

    for (int i = 0; i < 3; ++i) {
        cache->store(cache_path("res://gdextest_cache_mru_", i), make_test_texture(0.2f));
    }
    // Touch index 0 -> it becomes MRU (move_to_front in get_cached); the
    // next insert must evict index 1 (oldest untouched), not index 0.
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_mru_", 0)).is_valid());
    cache->store(cache_path("res://gdextest_cache_mru_", 3), make_test_texture(0.8f));

    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_mru_", 1)).is_null());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_mru_", 0)).is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_mru_", 2)).is_valid());
}

GDEX_TEST_T(cache, evict_single_entry_is_idempotent, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    cache->store("res://gdextest_cache_evict_a", make_test_texture(1.0f));
    cache->store("res://gdextest_cache_evict_b", make_test_texture(0.0f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);

    cache->evict("res://gdextest_cache_evict_a");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_evict_a").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_evict_b").is_valid());

    // Repeat + unknown-path evicts free nothing and change nothing.
    cache->evict("res://gdextest_cache_evict_a");
    cache->evict("res://gdextest_cache_no_such_entry");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
}

GDEX_TEST_T(cache, clear_empties_cache_keeps_live_refs, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    cache->store("res://gdextest_cache_clear_a", make_test_texture(1.0f));
    cache->store("res://gdextest_cache_clear_b", make_test_texture(0.5f));
    godot::Ref<godot::Texture2D> held = cache->get_cached("res://gdextest_cache_clear_a");
    GDEX_EXPECT_TRUE(held.is_valid());

    cache->clear();
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_clear_a").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_clear_b").is_null());
    // Dropping the cache entry must not invalidate textures already handed
    // out (mirrors project/headless_tests/test_integration.gd evict/clear
    // checks): the external Ref keeps the object alive.
    GDEX_EXPECT_TRUE(held.is_valid());

    // Disabling drains too (set_enabled(false) clears under the lock).
    cache->store("res://gdextest_cache_clear_c", make_test_texture(0.3f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
    cache->set_enabled(false);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
    cache->set_enabled(true);
}

GDEX_TEST_T(cache, cache_mode_gating_and_replace, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    // IGNORE / IGNORE_DEEP stores are no-ops (real _is_cache_mode_ignored).
    cache->store("res://gdextest_cache_mode_ign", make_test_texture(1.0f),
        godot::DSTextureCache::CACHE_MODE_IGNORE);
    cache->store("res://gdextest_cache_mode_deep", make_test_texture(1.0f),
        godot::DSTextureCache::CACHE_MODE_IGNORE_DEEP);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);

    GDEX_EXPECT_TRUE(cache->is_cache_mode_ignored(godot::DSTextureCache::CACHE_MODE_IGNORE));
    GDEX_EXPECT_TRUE(cache->is_cache_mode_ignored(godot::DSTextureCache::CACHE_MODE_IGNORE_DEEP));
    GDEX_EXPECT_FALSE(cache->is_cache_mode_ignored(godot::DSTextureCache::CACHE_MODE_REUSE));
    GDEX_EXPECT_FALSE(cache->is_cache_mode_ignored(godot::DSTextureCache::CACHE_MODE_REPLACE));

    // REPLACE overwrites the stored texture for an existing path ...
    godot::Ref<godot::Texture2D> first = make_test_texture(0.1f);
    godot::Ref<godot::Texture2D> second = make_test_texture(0.9f);
    cache->store("res://gdextest_cache_mode_rep", first);
    cache->store("res://gdextest_cache_mode_rep", second,
        godot::DSTextureCache::CACHE_MODE_REPLACE);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_mode_rep").ptr() == second.ptr());

    // ... but get_cached() in REPLACE mode always misses (real get_cached
    // early-out), as do IGNORE modes even when an entry exists.
    GDEX_EXPECT_TRUE(cache
                         ->get_cached("res://gdextest_cache_mode_rep",
                             godot::DSTextureCache::CACHE_MODE_REPLACE)
                         .is_null());
    GDEX_EXPECT_TRUE(cache
                         ->get_cached("res://gdextest_cache_mode_rep",
                             godot::DSTextureCache::CACHE_MODE_IGNORE)
                         .is_null());
}

GDEX_TEST_T(cache, shrink_max_entries_evicts_oldest_and_clamps, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    for (int i = 0; i < 4; ++i) {
        cache->store(cache_path("res://gdextest_cache_shrink_", i), make_test_texture(0.25f * i));
    }
    GDEX_EXPECT_EQ(cache->get_cached_count(), 4);

    // Shrinking evicts oldest-first until size fits (real set_max_entries).
    cache->set_max_entries(2);
    GDEX_EXPECT_EQ(cache->get_max_entries(), 2);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_shrink_", 0)).is_null());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_shrink_", 1)).is_null());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_shrink_", 2)).is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_shrink_", 3)).is_valid());

    // Out-of-range clamps to 1 (never 0/negative).
    cache->set_max_entries(0);
    GDEX_EXPECT_EQ(cache->get_max_entries(), 1);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
}

GDEX_TEST_T(cache, thread_safety_smoke_concurrent_access, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);
    cache->set_max_entries(64);

    // Textures are built on the test thread: workers only move Refs through
    // the mutex-guarded store/get/evict paths (take_over_path runs outside
    // the cache lock by design, so concurrent stores are the exercise).
    constexpr int kThreads = 4;
    constexpr int kPerThread = 20;
    std::vector<godot::String> paths;
    std::vector<godot::Ref<godot::Texture2D>> textures;
    paths.reserve(kThreads * kPerThread);
    textures.reserve(kThreads * kPerThread);
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            paths.push_back(cache_path("res://gdextest_cache_mt_", t * kPerThread + i));
            textures.push_back(make_test_texture(0.05f * t));
        }
    }

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([cache, &paths, &textures, t]() {
            for (int i = 0; i < kPerThread; ++i) {
                const int idx = t * kPerThread + i;
                cache->store(paths[idx], textures[idx]);
                (void)cache->get_cached(paths[idx]);
                (void)cache->get_cached_count();
            }
            // Overlapping evict/read of a neighbor's range stresses the lock.
            const int victim = ((t + 1) % kThreads) * kPerThread;
            cache->evict(paths[victim]);
            (void)cache->get_cached(paths[victim]);
        });
    }
    for (auto &worker : workers) {
        worker.join();
    }

    // No crash, no lock corruption: count is coherent and within the cap.
    GDEX_EXPECT_GE(cache->get_cached_count(), 0);
    GDEX_EXPECT_LE(cache->get_cached_count(), cache->get_max_entries());
    cache->clear();
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
}
