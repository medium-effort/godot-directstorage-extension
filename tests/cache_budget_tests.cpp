// Finding 2.2: the texture cache default must be large enough to hold a whole
// scene prewarm, and evicting a prewarmed entry before its batch finishes must
// be observable instead of silently causing a synchronous reload later.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "integration/ds_texture_cache.h"
#include "core/ds_context.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/color.hpp>

namespace {

godot::Ref<godot::Texture2D> make_test_texture(float p_red) {
	godot::Ref<godot::Image> img = godot::Image::create_empty(4, 4, false, godot::Image::FORMAT_RGBA8);
	img->fill(godot::Color(p_red, 0.2f, 0.4f, 1.0f));
	return godot::ImageTexture::create_from_image(img);
}

} // namespace

GDEX_TEST(cache_budget, registered_default_is_512) {
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps == nullptr || !ps->has_setting("dsgd_extension/texture_cache_max_entries")) {
		GDEX_SKIP("texture_cache_max_entries setting not registered.");
	}
	const godot::Variant value = ps->get_setting("dsgd_extension/texture_cache_max_entries");
	GDEX_EXPECT_EQ(static_cast<int64_t>(value), static_cast<int64_t>(512));
}

GDEX_TEST(cache_budget, prewarm_origin_eviction_is_counted_and_warned_once) {
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->set_max_entries(2);
	cache->_reset_prewarm_eviction_stats_for_testing();
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	// A prewarm-origin entry evicted by ordinary pressure must be counted.
	// Prewarm stores now protect their own batch, so the evicting store is a
	// normal one (see prewarm_batch_does_not_evict_its_own_entries below).
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_a", make_test_texture(0.1f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_b", make_test_texture(0.2f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_c", make_test_texture(0.3f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(1));

	// Ordinary (non-prewarm) eviction must not inflate the diagnostic counter.
	cache->clear();
	cache->_reset_prewarm_eviction_stats_for_testing();
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_d", make_test_texture(0.4f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_e", make_test_texture(0.5f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_f", make_test_texture(0.6f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(0));
}

// Audit gap 5: a prewarmed entry consumed via get_cached() is no longer "waiting
// for its batch"; a later LRU eviction is ordinary turnover and must not warn.
GDEX_TEST(cache_budget, consumed_prewarm_entry_evicts_without_warning) {
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->set_max_entries(2);
	cache->_reset_prewarm_eviction_stats_for_testing();
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	GDEX_ASSERT_TRUE(cache->store("res://cache_consumed_a", make_test_texture(0.1f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	// Consuming the entry clears its prewarm origin.
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_consumed_a").is_valid());
	// Two more stores push the consumed entry out of the 2-entry cache.
	GDEX_ASSERT_TRUE(cache->store("res://cache_consumed_b", make_test_texture(0.2f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_ASSERT_TRUE(cache->store("res://cache_consumed_c", make_test_texture(0.3f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(0));
}

GDEX_TEST(cache_budget, vram_guard_clamps_default_by_adapter_tier) {
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	GDEX_ASSERT_TRUE(ps != nullptr);

	auto save_setting = [&](const char *key) {
		bool had = ps->has_setting(key);
		godot::Variant prev = had ? ps->get_setting(key) : godot::Variant();
		ctx.add_teardown([ps, key, had, prev]() {
			if (had) {
				ps->set_setting(key, prev);
			} else {
				ps->clear(key);
			}
		});
	};
	save_setting("dsgd_extension/texture_cache_max_entries");
	save_setting("dsgd_extension/auto_tune_for_integrated_gpu");
	save_setting("dsgd_extension/auto_tune_for_low_vram_gpu");

	ps->set_setting("dsgd_extension/texture_cache_max_entries", 512);
	ps->set_setting("dsgd_extension/auto_tune_for_integrated_gpu", true);
	ps->set_setting("dsgd_extension/auto_tune_for_low_vram_gpu", true);

	auto make_cache = [&](bool p_auto_low_vram = true) {
		// Suppress the constructor's live-adapter VRAM guard so the synthetic
		// adapter tests below start from the un-clamped 512 default on any GPU.
		ps->set_setting("dsgd_extension/auto_tune_for_integrated_gpu", false);
		ps->set_setting("dsgd_extension/auto_tune_for_low_vram_gpu", false);
		godot::DSTextureCache *c = memnew(godot::DSTextureCache);
		ps->set_setting("dsgd_extension/auto_tune_for_integrated_gpu", true);
		ps->set_setting("dsgd_extension/auto_tune_for_low_vram_gpu", p_auto_low_vram);
		ctx.add_teardown([c]() {
			c->clear();
			memdelete(c);
		});
		return c;
	};

	// Discrete <3.5 GiB -> clamped to 128.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(3583);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(128));
	}

	// Real 4 GiB DXGI report (~4004 MiB) -> medium tier, not the <4 GiB cap.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(4004);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(256));
	}

	// Discrete 4-8 GiB -> clamped to 256.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(6144);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(256));
	}

	// Discrete 4-8 GiB DXGI report (real 8 GiB card ~7900 MiB) -> keeps 512.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(7900);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(512));
	}

	// Discrete >=8 GiB -> keeps 512.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(8192);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(512));
	}

	// Integrated/UMA -> clamped to 128.
	{
		godot::DSTextureCache *c = make_cache();
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = true;
		adapter["integrated_gpu_guess"] = true;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(0);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(128));
	}

	// Explicit user override via set_max_entries() is preserved.
	{
		godot::DSTextureCache *c = make_cache();
		c->set_max_entries(400);
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(2048);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(400));
	}

	// Disabling auto_tune_for_low_vram_gpu keeps 512.
	{
		godot::DSTextureCache *c = make_cache(false);
		godot::Dictionary adapter;
		adapter["available"] = true;
		adapter["is_uma"] = false;
		adapter["integrated_gpu_guess"] = false;
		adapter["dedicated_vram_mb"] = static_cast<int64_t>(2048);
		c->apply_vram_budget_guard(adapter);
		GDEX_EXPECT_EQ(static_cast<int64_t>(c->get_max_entries()), static_cast<int64_t>(512));
	}
}

GDEX_TEST(cache_budget, initialize_ds_applies_vram_guard_on_live_d3d12) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (!ds || !ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable (headless or non-D3D12 driver)");
	}

	godot::DSTextureCache *prev_cache = godot::DSTextureCache::get_singleton();
	godot::DSTextureCache *fresh_cache = memnew(godot::DSTextureCache);
	ctx.add_teardown([ds, fresh_cache]() {
		ds->shutdown_ds();
		fresh_cache->clear();
		memdelete(fresh_cache);
	});
	// Ensure singleton is restored if prev_cache was replaced; DSTextureCache
	// destructor nulls singleton when fresh_cache is deleted, which matches
	// gdextest's existing per-test DSTextureCache lifecycle.
	(void)prev_cache;

	GDEX_ASSERT_TRUE(ds->initialize_ds());
	const godot::Dictionary adapter = ds->get_gpu_adapter_info();
	GDEX_ASSERT_TRUE(static_cast<bool>(adapter.get("available", false)));

	const bool integrated = static_cast<bool>(adapter.get("is_uma", false)) ||
			static_cast<bool>(adapter.get("integrated_gpu_guess", false));
	const int64_t dedicated_mb = static_cast<int64_t>(adapter.get("dedicated_vram_mb", static_cast<int64_t>(0)));
	int64_t expected_cap = 512;
	if (integrated || (dedicated_mb > 0 && dedicated_mb < 3584)) {
		expected_cap = 128;
	} else if (dedicated_mb >= 3584 && dedicated_mb < 7168) {
		expected_cap = 256;
	}
	GDEX_EXPECT_LE(static_cast<int64_t>(fresh_cache->get_max_entries()), expected_cap);
}

// A prewarm batch must not evict its own unconsumed entries. With a 2-entry
// cap, three prewarm stores stay resident; only a later ordinary store turns
// the now-consumed entries back into normal LRU victims.
GDEX_TEST(cache_budget, prewarm_batch_does_not_evict_its_own_entries) {
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	cache->set_max_entries(2);
	cache->_reset_prewarm_eviction_stats_for_testing();
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	GDEX_ASSERT_TRUE(cache->store("res://cache_guard_a", make_test_texture(0.1f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_guard_b", make_test_texture(0.2f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_guard_c", make_test_texture(0.3f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));

	// All three survive the batch despite the 2-entry cap. These lookups also
	// consume them, clearing prewarm_origin for the turnover leg below.
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(0));
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_guard_a").is_valid());
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_guard_b").is_valid());
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_guard_c").is_valid());

	// Ordinary pressure can now evict the consumed entries normally.
	cache->_reset_prewarm_eviction_stats_for_testing();
	GDEX_ASSERT_TRUE(cache->store("res://cache_guard_d", make_test_texture(0.4f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(0));
	GDEX_ASSERT_FALSE(cache->get_cached("res://cache_guard_a").is_valid());
	GDEX_ASSERT_FALSE(cache->get_cached("res://cache_guard_b").is_valid());
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_guard_c").is_valid());
	GDEX_ASSERT_TRUE(cache->get_cached("res://cache_guard_d").is_valid());
}
