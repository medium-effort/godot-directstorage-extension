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
// - EVICTION GUARD: entries with reference_count > 1 (cache's own Ref plus
//   an outside holder: scene material, handed-out Ref, engine resident) are
//   skipped by _evict_oldest()/store()/set_max_entries(); all-held capacity
//   pressure keeps temporary over-capacity with a push_warning instead of
//   freeing a live Texture2DRD RID (GODOT_QUIRKS.md section 35). Aliases add
//   no Refs, so the 1:1 alias case still evicts at count == 1 and the
//   twin-guard drop_live_refs filter is unchanged.
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

    // No crash, no lock corruption. Note: `textures` still holds a Ref to
    // every stored texture, so all entries are externally held and eviction
    // correctly kept temporary over-capacity instead of freeing live RIDs.
    // Drop the externals and re-apply the cap before asserting the bound.
    textures.clear();
    cache->set_max_entries(64);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 64);
    cache->clear();
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
}

GDEX_TEST_T(cache, eviction_skips_externally_held_entries, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);
    cache->set_max_entries(3);

    // Resident held outside the cache (simulates a menu texture the visible
    // scene still draws while a prewarm stores over capacity).
    godot::Ref<godot::Texture2D> held = make_test_texture(0.1f);
    cache->store("res://gdextest_cache_guard_held", held);
    cache->store(cache_path("res://gdextest_cache_guard_", 1), make_test_texture(0.2f));
    cache->store(cache_path("res://gdextest_cache_guard_", 2), make_test_texture(0.3f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);

    // Over capacity with the oldest externally held: the held entry survives
    // (temporary over-capacity is NOT taken here since an unheld victim
    // exists); the oldest unheld entry evicts first and the held RID is kept.
    cache->store(cache_path("res://gdextest_cache_guard_", 3), make_test_texture(0.4f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_guard_held").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_guard_", 1)).is_null());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_guard_", 3)).is_valid());
    GDEX_EXPECT_TRUE(held.is_valid());

    // Every entry externally held: the store keeps temporary over-capacity
    // (with a push_warning) instead of freeing a live RID.
    godot::Ref<godot::Texture2D> hold2 = cache->get_cached(cache_path("res://gdextest_cache_guard_", 2));
    godot::Ref<godot::Texture2D> hold3 = cache->get_cached(cache_path("res://gdextest_cache_guard_", 3));
    GDEX_ASSERT_TRUE(hold2.is_valid());
    GDEX_ASSERT_TRUE(hold3.is_valid());
    cache->store(cache_path("res://gdextest_cache_guard_", 4), make_test_texture(0.5f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 4);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_guard_held").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_guard_", 4)).is_valid());

    // Release the newer holders: the next over-capacity store drains the
    // unheld entries oldest-first while the still-held oldest survives.
    hold2.unref();
    hold3.unref();
    cache->store(cache_path("res://gdextest_cache_guard_", 5), make_test_texture(0.6f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cache_guard_held").is_valid());
    GDEX_EXPECT_TRUE(held.is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached(cache_path("res://gdextest_cache_guard_", 5)).is_valid());
}

GDEX_TEST_T(cache, alias_storage_shares_single_lru_slot, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    godot::Ref<godot::Texture2D> tex = make_test_texture(0.7f);
    cache->store("res://gdextest_canonical.png", tex, godot::DSTextureCache::CACHE_MODE_REUSE, "res://.godot/imported/canonical.dstex");

    // Only 1 slot counted in cache_map/lru_list
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
    GDEX_EXPECT_TRUE(cache->has_alias("res://.godot/imported/canonical.dstex"));
    GDEX_EXPECT_TRUE(cache->get_canonical_path("res://.godot/imported/canonical.dstex") == "res://gdextest_canonical.png");

    // Both canonical and alias paths return the exact same resident texture pointer
    godot::Ref<godot::Texture2D> from_canonical = cache->get_cached("res://gdextest_canonical.png");
    godot::Ref<godot::Texture2D> from_alias = cache->get_cached("res://.godot/imported/canonical.dstex");
    GDEX_EXPECT_TRUE(from_canonical.is_valid());
    GDEX_EXPECT_TRUE(from_alias.is_valid());
    GDEX_EXPECT_TRUE(from_canonical.ptr() == from_alias.ptr());

    // Explicit add_alias / remove_alias
    cache->add_alias("res://gdextest_canonical.png", "res://second_alias.png");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);
    GDEX_EXPECT_TRUE(cache->has_alias("res://second_alias.png"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://second_alias.png").ptr() == tex.ptr());

    cache->remove_alias("res://second_alias.png");
    GDEX_EXPECT_FALSE(cache->has_alias("res://second_alias.png"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://second_alias.png").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_canonical.png").is_valid());
}

GDEX_TEST_T(cache, evict_via_alias_removes_canonical_and_all_aliases, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    godot::Ref<godot::Texture2D> tex = make_test_texture(0.4f);
    cache->store("res://gdextest_evict_canon.png", tex, godot::DSTextureCache::CACHE_MODE_REUSE, "res://.godot/imported/evict.dstex");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);

    // Evicting via the alias cleans up both canonical and alias
    cache->evict("res://.godot/imported/evict.dstex");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
    GDEX_EXPECT_FALSE(cache->has_alias("res://.godot/imported/evict.dstex"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_evict_canon.png").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://.godot/imported/evict.dstex").is_null());
}

GDEX_TEST_T(cache, lru_eviction_purges_canonical_and_aliases, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);
    cache->set_max_entries(2);

    cache->store("res://t0.png", make_test_texture(0.1f), godot::DSTextureCache::CACHE_MODE_REUSE, "res://t0.dstex");
    cache->store("res://t1.png", make_test_texture(0.2f), godot::DSTextureCache::CACHE_MODE_REUSE, "res://t1.dstex");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);

    // Store a 3rd texture -> must evict t0 (canonical and its alias)
    cache->store("res://t2.png", make_test_texture(0.3f), godot::DSTextureCache::CACHE_MODE_REUSE, "res://t2.dstex");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);
    GDEX_EXPECT_TRUE(cache->get_cached("res://t0.png").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://t0.dstex").is_null());
    GDEX_EXPECT_FALSE(cache->has_alias("res://t0.dstex"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://t1.png").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://t1.dstex").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://t2.png").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://t2.dstex").is_valid());
}

GDEX_TEST_T(cache, clear_unused_evicts_only_unreferenced_entries, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    cache->store("res://gdextest_cu_0", make_test_texture(0.1f));
    cache->store("res://gdextest_cu_1", make_test_texture(0.2f));
    cache->store("res://gdextest_cu_2", make_test_texture(0.3f));
    GDEX_EXPECT_EQ(cache->get_cached_count(), 3);

    // Hold external ref to texture #1 only
    godot::Ref<godot::Texture2D> ref1 = cache->get_cached("res://gdextest_cu_1");
    GDEX_ASSERT_TRUE(ref1.is_valid());

    int evicted = cache->clear_unused();
    GDEX_EXPECT_EQ(evicted, 2);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);

    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cu_0").is_null());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cu_1").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cu_1").ptr() == ref1.ptr());
    GDEX_EXPECT_TRUE(cache->get_cached("res://gdextest_cu_2").is_null());
}

GDEX_TEST_T(cache, clear_unused_with_aliases_and_subsequent_clear, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    godot::Ref<godot::Texture2D> tex_a = make_test_texture(0.4f);
    godot::Ref<godot::Texture2D> tex_b = make_test_texture(0.5f);
    cache->store("res://tex_a.png", tex_a, godot::DSTextureCache::CACHE_MODE_REUSE, "res://tex_a.dstex");
    cache->store("res://tex_b.png", tex_b, godot::DSTextureCache::CACHE_MODE_REUSE, "res://tex_b.dstex");
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);

    // Hold external ref to tex_a; drop our local ref to tex_b
    tex_b.unref();

    int evicted = cache->clear_unused();
    GDEX_EXPECT_EQ(evicted, 1);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 1);

    // tex_b and alias should be gone
    GDEX_EXPECT_TRUE(cache->get_cached("res://tex_b.png").is_null());
    GDEX_EXPECT_FALSE(cache->has_alias("res://tex_b.dstex"));

    // tex_a and alias should still be present
    GDEX_EXPECT_TRUE(cache->has_alias("res://tex_a.dstex"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://tex_a.png").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://tex_a.dstex").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://tex_a.dstex").ptr() == tex_a.ptr());

    // Drop external ref to tex_a, clear_unused again
    tex_a.unref();
    int evicted_second = cache->clear_unused();
    GDEX_EXPECT_EQ(evicted_second, 1);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
    GDEX_EXPECT_FALSE(cache->has_alias("res://tex_a.dstex"));
    GDEX_EXPECT_TRUE(cache->get_cached("res://tex_a.png").is_null());
}

GDEX_TEST_T(cache, clear_unused_all_held_or_empty, TAG_INTEGRATION) {
    godot::DSTextureCache *cache = new_enabled_cache(ctx);

    // Empty cache returns 0
    GDEX_EXPECT_EQ(cache->clear_unused(), 0);

    godot::Ref<godot::Texture2D> t0 = make_test_texture(0.6f);
    godot::Ref<godot::Texture2D> t1 = make_test_texture(0.7f);
    cache->store("res://held0.png", t0);
    cache->store("res://held1.png", t1);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);

    // Both held externally
    GDEX_EXPECT_EQ(cache->clear_unused(), 0);
    GDEX_EXPECT_EQ(cache->get_cached_count(), 2);
    GDEX_EXPECT_TRUE(cache->get_cached("res://held0.png").is_valid());
    GDEX_EXPECT_TRUE(cache->get_cached("res://held1.png").is_valid());
}
