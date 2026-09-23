#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "integration/ds_resource_format_loader.h"
#include "integration/ds_texture_cache.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

// Live-engine suite for DSResourceFormatLoader (covers the same area as
// project/headless_tests/test_integration.gd loader sections — which stay
// authoritative and are NOT touched — plus the res://textures.dspkg half of
// test_dspkg_alignment.gd).
//
// Real API from src/integration/ds_resource_format_loader.h (verified):
//   _get_recognized_extensions / _recognize_path / _handles_type /
//   _get_resource_type / _exists /
//   _load(p_path, p_original_path, p_use_sub_threads, p_cache_mode).
// Linkage note: compile-in decided, consumer_extension NOT
// configured, so the real extension is absent at runtime — DSContext and
// DSTextureCache singletons are null unless a test provisions them below.
// Consequences, handled explicitly:
// - The loader is instantiated directly (memnew needs no ClassDB
//   registration) and exercised through its real C++ virtuals.
// - DSContext stays null: _load takes the CPU fallback
//   (FileAccess + GDeflate + ImageTexture), which is deterministic headless
//   and exercises the CPU fallback path. No D3D12 device is required.
// - DSTextureCache is self-provisioned per test (instantiate sets the
//   singleton; set_enabled(true) arms store/get which are enabled-gated;
//   ctx.add_teardown() unrefs it so no Ref outlives the test and later
//   tests see a clean singleton).
// - Loader instances are stack Refs: each destructor invalidates the
//   loader-local res://textures.dspkg resolution cache, so staging/removing
//   the archive in one test cannot poison another regardless of run order.
// Fixtures: .dstex/.dspkg files are self-staged at runtime (DSTex writer +
// DSPkg builder + copy to res://textures.dspkg, removed afterwards), so this
// suite needs NO new staged assets. The only static fixture referenced is
// res://project/test_assets/test.png (negative interception case); if it is
// absent that test skips with the missing-asset message.

// Engine <-> cache contract: the loader forwards ResourceFormatLoader cache
// modes straight into DSTextureCache::store/get_cached, so the enumerators
// must agree. Header-only check — no link dependency.
static_assert(godot::ResourceFormatLoader::CACHE_MODE_IGNORE == godot::DSTextureCache::CACHE_MODE_IGNORE, "cache mode IGNORE parity");
static_assert(godot::ResourceFormatLoader::CACHE_MODE_REUSE == godot::DSTextureCache::CACHE_MODE_REUSE, "cache mode REUSE parity");
static_assert(godot::ResourceFormatLoader::CACHE_MODE_REPLACE == godot::DSTextureCache::CACHE_MODE_REPLACE, "cache mode REPLACE parity");

namespace {

constexpr int LOADER_TEX_WIDTH = 64;
constexpr int LOADER_TEX_HEIGHT = 4;
// 64x4 RGBA8 single mip, natural (unpadded) layout: 64*4 bytes/row x 4 rows.
constexpr int64_t LOADER_PAYLOAD_SIZE = 1024;

godot::PackedByteArray make_pattern_payload(int64_t p_size) {
	godot::PackedByteArray payload;
	payload.resize(p_size);
	for (int64_t i = 0; i < p_size; ++i) {
		payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
	}
	return payload;
}

// Writes a loose v1 .dstex via the real DSTex writer. Returns false on failure.
bool write_loose_dstex(const char *p_path) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	if (dstex.is_null()) {
		return false;
	}
	godot::Error err = dstex->write_to_file(
			godot::String(p_path), make_pattern_payload(LOADER_PAYLOAD_SIZE),
			LOADER_TEX_WIDTH, LOADER_TEX_HEIGHT, godot::Image::FORMAT_RGBA8, 1, 1);
	return err == godot::OK;
}

void remove_user_file(const char *p_path) {
	godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
	if (dir.is_valid()) {
		godot::Error err = dir->remove(godot::String(p_path));
		(void)err;
	}
}

// Builds a single-entry .dspkg at p_dspkg_path indexing p_virtual_path and
// copies it to the well-known res://textures.dspkg logical path (mirrors
// test_dspkg_alignment.gd). Registers a teardown so the archive is removed
// even when the body aborts. Returns false when any step fails.
bool stage_textures_dspkg(::gdextest::TestContext &ctx, const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path) {
	if (!write_loose_dstex(p_dstex_path)) {
		return false;
	}
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	if (builder.is_null()) {
		return false;
	}
	if (builder->create(godot::String(p_dspkg_path)) != godot::OK) {
		return false;
	}
	if (builder->add_texture(godot::String(p_virtual_path), godot::String(p_dstex_path)) != godot::OK) {
		return false;
	}
	if (builder->finalize() != godot::OK) {
		return false;
	}
	if (godot::DirAccess::copy_absolute(godot::String(p_dspkg_path), godot::String("res://textures.dspkg")) != godot::OK) {
		return false;
	}
	ctx.add_teardown([]() {
		godot::Error err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
		(void)err;
	});
	return true;
}

} // namespace

// --- Interception: recognized extensions -------------------------------------

GDEX_TEST_T(loader, recognized_extensions_cover_dstex_and_dspkg, TAG_INTEGRATION) {
	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	godot::PackedStringArray exts = loader->_get_recognized_extensions();
	GDEX_EXPECT_TRUE(exts.has("dstex"));
	GDEX_EXPECT_TRUE(exts.has("dspkg"));
}

// --- Interception: handled types ----------------------------------------------

GDEX_TEST_T(loader, handles_type_accepts_2d_family_rejects_layered, TAG_INTEGRATION) {
	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::StringName empty_type(godot::String(""));
	const godot::StringName tex2d(godot::String("Texture2D"));
	const godot::StringName compressed(godot::String("CompressedTexture2D"));
	const godot::StringName resource(godot::String("Resource"));
	const godot::StringName texture(godot::String("Texture"));
	const godot::StringName texture3d(godot::String("Texture3D"));

	GDEX_EXPECT_TRUE(loader->_handles_type(empty_type));
	GDEX_EXPECT_TRUE(loader->_handles_type(tex2d));
	GDEX_EXPECT_TRUE(loader->_handles_type(compressed));
	GDEX_EXPECT_TRUE(loader->_handles_type(resource));
	GDEX_EXPECT_TRUE(loader->_handles_type(texture));
	// NOTE: the rejection path pushes one engine error line by design
	// (unsupported non-2D texture type); the log entry is expected, not a
	// test failure.
	GDEX_EXPECT_FALSE(loader->_handles_type(texture3d));
}

// --- Interception: .dstex paths, claim-vs-existence split ---------------------

GDEX_TEST_T(loader, recognize_path_intercepts_dstex, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_loose.dstex";
	if (!write_loose_dstex(dstex_path)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::StringName tex2d(godot::String("Texture2D"));
	const godot::String res_path(dstex_path);

	GDEX_EXPECT_TRUE(loader->_recognize_path(res_path, tex2d));
	GDEX_EXPECT_TRUE(loader->_get_resource_type(res_path) == godot::String("Texture2D"));
	GDEX_EXPECT_TRUE(loader->_exists(res_path));

	// Physical source images belong to the importer, not this loader, even
	// when they exist on disk (static fixture asset, no staging needed).
	const godot::String png_fixture("res://project/test_assets/test.png");
	if (!godot::FileAccess::file_exists(png_fixture)) {
		GDEX_SKIP("fixture asset missing: project/test_assets/test.png (assets glob project/test_assets/** did not stage it)");
	}
	GDEX_EXPECT_FALSE(loader->_recognize_path(png_fixture, tex2d));

	// Extension claim is path-based: a missing .dstex is still intercepted
	// (recognition true) while existence/load correctly report absent.
	const godot::String missing_dstex("user://gdextest_loader_012_nonexistent.dstex");
	GDEX_EXPECT_TRUE(loader->_recognize_path(missing_dstex, tex2d));
	GDEX_EXPECT_FALSE(loader->_exists(missing_dstex));
	godot::Variant missing_result = loader->_load(missing_dstex, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	GDEX_EXPECT_TRUE(missing_result.get_type() == godot::Variant::NIL);

	remove_user_file(dstex_path);
}

// --- Load: loose .dstex with CACHE_MODE_IGNORE ---------------------------------

GDEX_TEST_T(loader, load_loose_dstex_ignore_returns_valid_texture, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_ignore.dstex";
	if (!write_loose_dstex(dstex_path)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	godot::Variant result = loader->_load(godot::String(dstex_path), godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> tex(result);
	GDEX_ASSERT_TRUE(tex.is_valid());
	GDEX_EXPECT_EQ(tex->get_width(), LOADER_TEX_WIDTH);
	GDEX_EXPECT_EQ(tex->get_height(), LOADER_TEX_HEIGHT);

	// IGNORE must not populate the texture cache: provision + enable one and
	// prove the load left no entry behind.
	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	godot::Variant again = loader->_load(godot::String(dstex_path), godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> tex_again(again);
	GDEX_ASSERT_TRUE(tex_again.is_valid());
	GDEX_EXPECT_TRUE(cache->get_cached(godot::String(dstex_path), godot::DSTextureCache::CACHE_MODE_REUSE).is_null());

	remove_user_file(dstex_path);
}

// --- Cache modes: REUSE returns the same Ref ------------------------------------

GDEX_TEST_T(loader, cache_mode_reuse_returns_same_ref, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_reuse.dstex";
	if (!write_loose_dstex(dstex_path)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::String res_path(dstex_path);
	godot::Ref<godot::Texture2D> first(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_REUSE));
	GDEX_ASSERT_TRUE(first.is_valid());
	godot::Ref<godot::Texture2D> second(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_REUSE));
	GDEX_ASSERT_TRUE(second.is_valid());

	// Same Ref: no re-decode, pointer-identical.
	GDEX_EXPECT_TRUE(first.ptr() == second.ptr());

	godot::Ref<godot::Texture2D> cached = cache->get_cached(res_path, godot::DSTextureCache::CACHE_MODE_REUSE);
	GDEX_ASSERT_TRUE(cached.is_valid());
	GDEX_EXPECT_TRUE(cached.ptr() == first.ptr());

	remove_user_file(dstex_path);
}

// --- Cache modes: REPLACE refreshes the entry -------------------------------------

GDEX_TEST_T(loader, cache_mode_replace_refreshes_entry, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_replace.dstex";
	if (!write_loose_dstex(dstex_path)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	GDEX_ASSERT_TRUE(cache != nullptr);
	cache->set_enabled(true);
	ctx.add_teardown([cache]() {
		cache->clear();
		memdelete(cache);
	});

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::String res_path(dstex_path);
	godot::Ref<godot::Texture2D> seeded(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_REUSE));
	GDEX_ASSERT_TRUE(seeded.is_valid());

	godot::Ref<godot::Texture2D> replaced(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_REPLACE));
	GDEX_ASSERT_TRUE(replaced.is_valid());
	GDEX_EXPECT_EQ(replaced->get_width(), LOADER_TEX_WIDTH);
	GDEX_EXPECT_EQ(replaced->get_height(), LOADER_TEX_HEIGHT);

	// The REPLACE result is what the cache now holds.
	godot::Ref<godot::Texture2D> cached = cache->get_cached(res_path, godot::DSTextureCache::CACHE_MODE_REUSE);
	GDEX_ASSERT_TRUE(cached.is_valid());
	GDEX_EXPECT_TRUE(cached.ptr() == replaced.ptr());

	remove_user_file(dstex_path);
}

// --- Virtual paths: res://textures.dspkg entries -----------------------------------

GDEX_TEST_T(loader, virtual_dspkg_path_resolves_through_archive, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_virtual.dstex";
	const char *dspkg_path = "user://gdextest_loader_012_virtual.dspkg";
	const char *virtual_path = "res://gdextest_loader_packaged_012.png";
	if (!stage_textures_dspkg(ctx, dstex_path, dspkg_path, virtual_path)) {
		remove_user_file(dstex_path);
		remove_user_file(dspkg_path);
		GDEX_ABORT_TEST("textures.dspkg staging failed");
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::StringName tex2d(godot::String("Texture2D"));
	const godot::String res_path(virtual_path);

	// The packaged entry has no physical file, yet resolves via the archive.
	GDEX_EXPECT_FALSE(godot::FileAccess::file_exists(res_path));
	GDEX_EXPECT_TRUE(loader->_exists(res_path));
	GDEX_EXPECT_TRUE(loader->_get_resource_type(res_path) == godot::String("Texture2D"));
	GDEX_EXPECT_TRUE(loader->_recognize_path(res_path, tex2d));

	godot::Ref<godot::Texture2D> tex(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE));
	GDEX_ASSERT_TRUE(tex.is_valid());
	GDEX_EXPECT_EQ(tex->get_width(), LOADER_TEX_WIDTH);
	GDEX_EXPECT_EQ(tex->get_height(), LOADER_TEX_HEIGHT);

	// Entries not in the archive stay absent on every axis.
	const godot::String missing_entry("res://gdextest_loader_missing_012.png");
	GDEX_EXPECT_FALSE(loader->_exists(missing_entry));
	godot::Variant missing_result = loader->_load(missing_entry, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	GDEX_EXPECT_TRUE(missing_result.get_type() == godot::Variant::NIL);

	godot::Error err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
	(void)err;
	remove_user_file(dstex_path);
	remove_user_file(dspkg_path);
}

// --- .dspkg branch: _load returns the archive resource ---------------------------------

GDEX_TEST_T(loader, load_dspkg_returns_archive_resource, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_012_pkg.dstex";
	const char *dspkg_path = "user://gdextest_loader_012_pkg.dspkg";
	const char *virtual_path = "res://gdextest_loader_pkg_entry_012.png";
	if (!write_loose_dstex(dstex_path)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_TRUE(builder.is_valid());
	GDEX_ASSERT_TRUE(builder->create(godot::String(dspkg_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->add_texture(godot::String(virtual_path), godot::String(dstex_path)) == godot::OK);
	GDEX_ASSERT_TRUE(builder->finalize() == godot::OK);
	builder.unref();

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::String res_path(dspkg_path);
	GDEX_EXPECT_TRUE(loader->_get_resource_type(res_path) == godot::String("Resource"));
	GDEX_EXPECT_TRUE(loader->_exists(res_path));

	godot::Ref<godot::DSPkg> pkg(loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE));
	GDEX_ASSERT_TRUE(pkg.is_valid());
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(pkg->has_texture(godot::String(virtual_path)));

	remove_user_file(dstex_path);
	remove_user_file(dspkg_path);
}
