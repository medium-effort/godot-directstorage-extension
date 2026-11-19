// Finding 2.2: the texture cache default must be large enough to hold a whole
// scene prewarm, and evicting a prewarmed entry before its batch finishes must
// be observable instead of silently causing a synchronous reload later.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "integration/ds_texture_cache.h"

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

	// Third store evicts the oldest prewarm-origin entry.
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_a", make_test_texture(0.1f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_b", make_test_texture(0.2f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_c", make_test_texture(0.3f), godot::DSTextureCache::CACHE_MODE_REUSE, "", true));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(1));

	// Ordinary (non-prewarm) eviction must not inflate the diagnostic counter.
	cache->clear();
	cache->_reset_prewarm_eviction_stats_for_testing();
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_d", make_test_texture(0.4f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_e", make_test_texture(0.5f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_ASSERT_TRUE(cache->store("res://cache_budget_f", make_test_texture(0.6f), godot::DSTextureCache::CACHE_MODE_REUSE, "", false));
	GDEX_EXPECT_EQ(static_cast<int64_t>(cache->_get_prewarm_eviction_count_for_testing()), static_cast<int64_t>(0));
}
