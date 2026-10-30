#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "integration/ds_resource_format_loader.h"
#include "integration/ds_texture_cache.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
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

// Releases the DirectStorage context so the Win32 backend drops any IDStorageFile
// handle a batch still owns. Handles now close when the owning batch retires, so
// this only matters for a request that is still in flight; keeping it here means
// staging never races a live handle. A file opened by DirectStorage cannot be
// removed or replaced: remove fails with a sharing violation and Godot's
// safe-save copy commit is rolled back while still reporting success. Any test
// that stages over res://textures.dspkg after a GPU load releases first.
void shutdown_ds_singleton() {
	godot::Engine *engine = godot::Engine::get_singleton();
	if (engine && engine->has_singleton(godot::StringName("DSContext"))) {
		godot::Object *ds = engine->get_singleton(godot::StringName("DSContext"));
		if (ds != nullptr) {
			ds->call(godot::StringName("shutdown_ds"));
		}
	}
}

// Builds a single-entry .dspkg at p_dspkg_path indexing p_virtual_path and
// copies it to the well-known res://textures.dspkg logical path (mirrors
// test_dspkg_alignment.gd). Registers a teardown so the archive is removed
// even when the body aborts. Returns false when any step fails.
//
// Staging is release + remove + copy + reopen-verify with one retry, never the
// copy return code alone: a stale or locked res://textures.dspkg poisons every
// later resolve, and the copy has reported success over live scan windows.
// On failure the exact step is logged so a locked destination is
// distinguishable from a corrupt one.
bool stage_textures_dspkg(::gdextest::TestContext &ctx, const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path, bool p_release_context = true, const char *p_second_virtual_path = nullptr) {
	if (p_release_context) {
		shutdown_ds_singleton();
	}
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
	if (p_second_virtual_path != nullptr &&
			builder->add_texture(godot::String(p_second_virtual_path), godot::String(p_dstex_path)) != godot::OK) {
		return false;
	}
	if (builder->finalize() != godot::OK) {
		return false;
	}

	int64_t built_len = -1;
	{
		godot::Ref<godot::FileAccess> built = godot::FileAccess::open(godot::String(p_dspkg_path), godot::FileAccess::READ);
		if (built.is_valid()) {
			built_len = built->get_length();
			built->close();
		}
	}

	for (int attempt = 0; attempt < 2; ++attempt) {
		godot::Error remove_err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
		godot::Error copy_err = godot::DirAccess::copy_absolute(godot::String(p_dspkg_path), godot::String("res://textures.dspkg"));
		bool exists = godot::FileAccess::file_exists(godot::String("res://textures.dspkg"));

		int64_t staged_len = -1;
		godot::Ref<godot::FileAccess> staged = godot::FileAccess::open(godot::String("res://textures.dspkg"), godot::FileAccess::READ);
		if (staged.is_valid()) {
			staged_len = staged->get_length();
			staged->close();
		}

		godot::Error open_err = godot::ERR_CANT_OPEN;
		int64_t entries = -1;
		bool has_texture = false;
		godot::Ref<godot::DSPkg> verify;
		verify.instantiate();
		if (verify.is_valid()) {
			open_err = verify->open(godot::String("res://textures.dspkg"));
			entries = verify->get_entry_count();
			has_texture = verify->has_texture(godot::String(p_virtual_path));
		}

		if (exists && open_err == godot::OK && has_texture) {
			ctx.add_teardown([]() {
				shutdown_ds_singleton();
				godot::Error err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
				(void)err;
			});
			return true;
		}

		godot::UtilityFunctions::push_warning(
				"stage_textures_dspkg attempt ", attempt,
				": remove=", static_cast<int64_t>(remove_err),
				" copy=", static_cast<int64_t>(copy_err),
				" exists=", exists,
				" built_len=", built_len,
				" staged_len=", staged_len,
				" open=", static_cast<int64_t>(open_err),
				" entries=", entries,
				" has_texture=", has_texture);

		if (attempt == 0) {
			godot::OS *os = godot::OS::get_singleton();
			if (os != nullptr) {
				os->delay_msec(500);
			}
		}
	}
	return false;
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
		GDEX_SKIP("fixture asset missing: project/test_assets/test.png (assets glob project/test_assets/**/* did not stage it)");
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

// --- Load: partial mip chain keeps the base without inventing mipmaps (#0039) --

GDEX_TEST_T(loader, load_partial_mip_chain_returns_base_without_mipmaps, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_partial_chain.dstex";
	// 64x64 RGBA8 needs a full chain of 7 for use_mipmaps=true.
	// Write only 2 mips (base 16384 + mip1 4096 = 20480 natural bytes).
	godot::PackedByteArray raw;
	raw.resize(20480);
	uint8_t *w = raw.ptrw();
	for (int64_t i = 0; i < 20480; ++i) {
		w[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
	}
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	GDEX_ASSERT_TRUE(dstex.is_valid());
	if (dstex->write_to_file(godot::String(dstex_path), raw, 64, 64, godot::Image::FORMAT_RGBA8, 2, 1) != godot::OK) {
		GDEX_ABORT_TEST("fixture partial-chain DSTex write failed");
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	godot::Variant result = loader->_load(godot::String(dstex_path), godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	godot::Ref<godot::Texture2D> tex(result);
	GDEX_ASSERT_TRUE(tex.is_valid());
	GDEX_EXPECT_EQ(tex->get_width(), 64);
	GDEX_EXPECT_EQ(tex->get_height(), 64);

	// Two outcomes are legitimate for a partial (2-of-7) chain, and which one
	// applies depends on the load environment, not on the payload:
	//  - GPU path (v2 payload, D3D12 render thread, sub-threads off): a
	//    Texture2DRD holding exactly the two authored mips.
	//  - CPU path (headless, non-D3D12, or a worker-thread load): an
	//    ImageTexture with the base mip only - never a partial mip chain.
	godot::Ref<godot::ImageTexture> img_tex = tex;
	if (img_tex.is_valid()) {
		godot::Ref<godot::Image> img = img_tex->get_image();
		GDEX_ASSERT_TRUE(img.is_valid());
		GDEX_EXPECT_FALSE(img->has_mipmaps());
		GDEX_EXPECT_EQ(img->get_data().size(), 16384);
	} else {
		godot::Ref<godot::Texture2DRD> rd_tex = tex;
		GDEX_ASSERT_TRUE(rd_tex.is_valid());
		GDEX_EXPECT_TRUE(rd_tex->get_texture_rd_rid().is_valid());
	}

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

	// In gdextest (--editor), override editor bypass to verify archive resolution path.
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps) {
		ps->set_setting("directstorage/force_archive_resolution", true);
	}
	ctx.add_teardown([ps]() {
		if (ps) {
			ps->set_setting("directstorage/force_archive_resolution", false);
		}
	});

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	const godot::StringName tex2d(godot::String("Texture2D"));
	const godot::String res_path(virtual_path);

	// The packaged entry has no physical file, yet resolves via the archive when resolution is active.
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

GDEX_TEST_T(loader, editor_mode_unconditionally_bypasses_dspkg, TAG_INTEGRATION) {
	const char *dstex_path = "user://gdextest_loader_ed_bypass.dstex";
	const char *dspkg_path = "user://gdextest_loader_ed_bypass.dspkg";
	const char *virtual_path = "res://gdextest_loader_ed_bypass.png";
	if (!stage_textures_dspkg(ctx, dstex_path, dspkg_path, virtual_path)) {
		remove_user_file(dstex_path);
		remove_user_file(dspkg_path);
		GDEX_ABORT_TEST("textures.dspkg staging failed");
	}

	// Ensure force_archive_resolution is explicitly false (default behavior).
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps) {
		ps->set_setting("directstorage/force_archive_resolution", false);
	}

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	// In editor mode (running under gdextest --editor), archive resolution is bypassed:
	// Virtual paths inside textures.dspkg must NOT be recognized or resolve.
	const godot::String res_path(virtual_path);
	GDEX_EXPECT_FALSE(loader->_exists(res_path));

	godot::Variant load_result = loader->_load(res_path, godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	GDEX_EXPECT_TRUE(load_result.get_type() == godot::Variant::NIL);

	godot::Error err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
	(void)err;
	remove_user_file(dstex_path);
	remove_user_file(dspkg_path);
}

GDEX_TEST_T(loader, embedded_pck_detection_warning, TAG_INTEGRATION) {
	// True-embedded (VFS res:// true + physical globalized false) only occurs
	// inside an exported PCK, which unit tests cannot fabricate: in the
	// editor FileAccess::file_exists("res://x") and the globalized probe hit
	// the same disk file. This test exercises both automatable halves of the
	// probe so the warning branch runs without false positives:
	// (1) physical archive present -> resolves, no configuration error;
	// (2) archive absent -> empty resolution (missing path, not embedded).
	// Full embedded verification is manual: export with the .dspkg inside the
	// PCK and confirm the CONFIGURATION ERROR + CPU-decompression fallback.
	const char *dstex_path = "user://gdextest_loader_embedded.dstex";
	const char *dspkg_path = "user://gdextest_loader_embedded.dspkg";
	const char *virtual_path = "res://gdextest_loader_embedded.png";
	if (!stage_textures_dspkg(ctx, dstex_path, dspkg_path, virtual_path)) {
		remove_user_file(dstex_path);
		remove_user_file(dspkg_path);
		GDEX_ABORT_TEST("textures.dspkg staging failed");
	}

	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps) {
		ps->set_setting("directstorage/force_archive_resolution", true);
	}
	ctx.add_teardown([ps]() {
		if (ps) {
			ps->set_setting("directstorage/force_archive_resolution", false);
		}
	});

	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());

	// (1) Staged res://textures.dspkg exists physically: the packaged entry
	// resolves through the archive (direct-VFS branch, embedded probe passes
	// silently since the globalized file exists too).
	const godot::String res_path(virtual_path);
	const bool staged_resolves = loader->_exists(res_path);
	if (!staged_resolves) {
		// Distinguish "the archive on disk is not the one just staged" from
		// "the loader refused to resolve it": probe the same file directly.
		godot::Ref<godot::DSPkg> probe;
		probe.instantiate();
		godot::Error probe_err = probe->open(godot::String("res://textures.dspkg"));
		godot::String global_path = ps ? ps->globalize_path(godot::String("res://textures.dspkg")) : godot::String("res://textures.dspkg");
		godot::Engine *engine = godot::Engine::get_singleton();
		godot::UtilityFunctions::push_warning(
				"embedded_pck_detection_warning: loader->_exists=false; res_exists=",
				godot::FileAccess::file_exists(godot::String("res://textures.dspkg")),
				" global_exists=", godot::FileAccess::file_exists(global_path),
				" global_path=", global_path,
				" probe_open=", static_cast<int64_t>(probe_err),
				" entries=", probe->get_entry_count(),
				" has_texture=", probe->has_texture(res_path),
				" force=", ps ? static_cast<bool>(ps->get_setting("directstorage/force_archive_resolution")) : false,
				" editor=", engine ? engine->is_editor_hint() : false);
	}
	GDEX_EXPECT_TRUE(staged_resolves);

	// (2) Remove the archive: the packaged entry no longer resolves (missing,
	// and the loader's candidate-path diagnostic fires when bundle_dspkg set).
	godot::Error err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
	(void)err;
	remove_user_file(dstex_path);
	remove_user_file(dspkg_path);
}

// --- Live archive replacement is observed without a restart (#0182) ------------

GDEX_TEST_T(loader, archive_replacement_after_load_is_observed, TAG_INTEGRATION) {
	// The GPU path caches an IDStorageFile for the archive. It used to live for
	// the whole context, so a replacement could not even be written (remove
	// failed, safe-save rolled back) and a new load served the old bytes.
	// Two caches are covered here: the backend handle (released at batch
	// completion) and the loader's parsed TOC (revalidated by size + mtime).
	const char *a_dstex = "user://gdextest_loader_replace_a.dstex";
	const char *a_dspkg = "user://gdextest_loader_replace_a.dspkg";
	const char *a_virtual = "res://gdextest_loader_replace_a.png";
	const char *b_dstex = "user://gdextest_loader_replace_b.dstex";
	const char *b_dspkg = "user://gdextest_loader_replace_b.dspkg";
	const char *b_virtual = "res://gdextest_loader_replace_b.png";
	// Second entry so the replacement differs in size from the archive it
	// replaced: Godot's mtime has one-second granularity, and the size is what
	// makes a fast same-second replacement detectable.
	const char *b_control = "res://gdextest_loader_replace_b_control.png";

	if (!stage_textures_dspkg(ctx, a_dstex, a_dspkg, a_virtual)) {
		remove_user_file(a_dstex);
		remove_user_file(a_dspkg);
		GDEX_ABORT_TEST("archive A staging failed");
	}

	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps) {
		ps->set_setting("directstorage/force_archive_resolution", true);
	}
	ctx.add_teardown([ps]() {
		if (ps) {
			ps->set_setting("directstorage/force_archive_resolution", false);
		}
	});

	// One loader instance for the whole test: reusing it is the point, because
	// it caches the parsed TOC as well as going through the backend handle.
	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	GDEX_ASSERT_TRUE(loader.is_valid());
	godot::Ref<godot::Texture2D> first(loader->_load(godot::String(a_virtual), godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE));
	GDEX_ASSERT_TRUE(first.is_valid());
	GDEX_EXPECT_TRUE(loader->_exists(godot::String(a_virtual)));

	// Replace the archive over the same path WITHOUT releasing the context:
	// the completed batch must already have dropped its handle.
	if (!stage_textures_dspkg(ctx, b_dstex, b_dspkg, b_virtual, false, b_control)) {
		remove_user_file(a_dstex);
		remove_user_file(a_dspkg);
		remove_user_file(b_dstex);
		remove_user_file(b_dspkg);
		GDEX_ABORT_TEST("archive replacement failed: the previous handle is still locking res://textures.dspkg");
	}

	// Same instance: the stale TOC must be dropped and the new archive read.
	GDEX_EXPECT_FALSE(loader->_exists(godot::String(a_virtual)));
	GDEX_EXPECT_TRUE(loader->_exists(godot::String(b_virtual)));
	GDEX_EXPECT_TRUE(loader->_exists(godot::String(b_control)));
	godot::Ref<godot::Texture2D> second(loader->_load(godot::String(b_virtual), godot::String(""), false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE));
	GDEX_EXPECT_TRUE(second.is_valid());

	remove_user_file(a_dstex);
	remove_user_file(a_dspkg);
	remove_user_file(b_dstex);
	remove_user_file(b_dspkg);
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

// --- v2 mip-table contract -------------------------------------------------
//
// read_header()/read_from_file() now require a v2 mip table to (a) declare a
// plausible footprint per level, (b) add up to the header totals, and (c) match
// the padded footprint of its own mip level. Before this, the loader expanded
// one destination buffer per mip straight from the table with no aggregate
// bound, so a ~1 KB file could request gigabytes from ResourceLoader.load();
// and the old header-level width*height*32 heuristic refused the writer's own
// output for small or narrow textures, whose padded rows exceed that bound.

namespace {

constexpr int64_t DSTEX_OFF_ORIGINAL = 24;
constexpr int64_t DSTEX_OFF_ENTRY0 = 48;
constexpr int64_t DSTEX_V2_ENTRY_SIZE = 24;
constexpr int64_t DSTEX_ENTRY_OFF_UNCOMPRESSED = 12;

godot::PackedByteArray loader_read_all_bytes(const char *p_path) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	godot::PackedByteArray bytes;
	if (file.is_valid()) {
		bytes = file->get_buffer(static_cast<int64_t>(file->get_length()));
		file->close();
	}
	return bytes;
}

bool loader_write_all_bytes(const char *p_path, const godot::PackedByteArray &p_bytes) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::WRITE);
	if (file.is_null()) {
		return false;
	}
	file->store_buffer(p_bytes);
	const bool ok = file->get_error() == godot::OK;
	file->close();
	return ok;
}

void loader_put_u32(godot::PackedByteArray &p_bytes, int64_t p_offset, uint32_t p_value) {
	for (int i = 0; i < 4; ++i) {
		p_bytes[p_offset + i] = static_cast<uint8_t>((p_value >> (8 * i)) & 0xFFu);
	}
}

void loader_put_u64(godot::PackedByteArray &p_bytes, int64_t p_offset, uint64_t p_value) {
	for (int i = 0; i < 8; ++i) {
		p_bytes[p_offset + i] = static_cast<uint8_t>((p_value >> (8 * i)) & 0xFFu);
	}
}

// Copies p_src to p_dst and patches one mip entry's uncompressed_size (p_mip >= 0)
// and/or the header original_size, so a test can break exactly one clause.
bool patch_v2_mip_table(const char *p_src, const char *p_dst, int p_mip, uint32_t p_uncompressed,
		bool p_patch_original, uint64_t p_original) {
	godot::PackedByteArray bytes = loader_read_all_bytes(p_src);
	if (bytes.is_empty()) {
		return false;
	}
	if (p_mip >= 0) {
		loader_put_u32(bytes, DSTEX_OFF_ENTRY0 + DSTEX_V2_ENTRY_SIZE * p_mip + DSTEX_ENTRY_OFF_UNCOMPRESSED, p_uncompressed);
	}
	if (p_patch_original) {
		loader_put_u64(bytes, DSTEX_OFF_ORIGINAL, p_original);
	}
	return loader_write_all_bytes(p_dst, bytes);
}

// Writes a v2 .dstex carrying the natural (unpadded) data of a full RGBA8 mip
// chain for p_width x p_height. The writer pads each level itself.
bool write_rgba8_chain(const char *p_path, int p_width, int p_height) {
	godot::PackedByteArray payload;
	int mip_count = 0;
	int w = p_width;
	int h = p_height;
	while (mip_count < 16) {
		const int64_t size = static_cast<int64_t>(w) * static_cast<int64_t>(h) * 4;
		const int64_t base = payload.size();
		payload.resize(base + size);
		for (int64_t i = 0; i < size; ++i) {
			payload[base + i] = static_cast<uint8_t>((i * 31 + mip_count * 7) & 0xFF);
		}
		++mip_count;
		if (w == 1 && h == 1) {
			break;
		}
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), payload, p_width, p_height,
				   godot::Image::FORMAT_RGBA8, mip_count, 1) == godot::OK;
}

godot::Ref<godot::Texture2D> loader_load(const char *p_path) {
	godot::Ref<godot::DSResourceFormatLoader> loader;
	loader.instantiate();
	const godot::Variant result = loader->_load(godot::String(p_path), godot::String(""), false,
			godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
	return godot::Ref<godot::Texture2D>(result);
}

} // namespace

GDEX_TEST_T(loader, v2_mip_sum_mismatch_rejected, TAG_INTEGRATION) {
	const char *base = "user://gdextest_loader_miptable_base.dstex";
	const char *patched = "user://gdextest_loader_miptable_sum.dstex";
	if (!write_loose_dstex(base)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSTex> reader;
	reader.instantiate();
	// Control: the unpatched writer output parses, so a rejection below is
	// attributable to the patched field and not to fixture assembly.
	GDEX_ASSERT_TRUE(!reader->read_header(godot::String(base)).is_empty());

	// One entry claims 4 KiB while the header total stays 1 KiB.
	GDEX_ASSERT_TRUE(patch_v2_mip_table(base, patched, 0, 4096u, false, 0));

	GDEX_EXPECT_TRUE(reader->read_header(godot::String(patched)).is_empty());
	GDEX_EXPECT_TRUE(reader->read_from_file(godot::String(patched)).is_empty());
	GDEX_EXPECT_TRUE(loader_load(patched).is_null());

	remove_user_file(base);
	remove_user_file(patched);
}

GDEX_TEST_T(loader, v2_mip_uncompressed_underflow_rejected, TAG_INTEGRATION) {
	const char *base = "user://gdextest_loader_miptable_base2.dstex";
	const char *patched = "user://gdextest_loader_miptable_underflow.dstex";
	if (!write_loose_dstex(base)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSTex> reader;
	reader.instantiate();

	// 0xFFFFFFFF arrives as int32 -1 and would become a 4 GiB uint32 resize.
	GDEX_ASSERT_TRUE(patch_v2_mip_table(base, patched, 0, 0xFFFFFFFFu, false, 0));

	GDEX_EXPECT_TRUE(reader->read_header(godot::String(patched)).is_empty());
	GDEX_EXPECT_TRUE(loader_load(patched).is_null());

	remove_user_file(base);
	remove_user_file(patched);
}

GDEX_TEST_T(loader, v2_mip_footprint_must_match_layout, TAG_INTEGRATION) {
	const char *base = "user://gdextest_loader_miptable_base3.dstex";
	const char *patched = "user://gdextest_loader_miptable_layout.dstex";
	if (!write_loose_dstex(base)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSTex> reader;
	reader.instantiate();

	// Sums stay consistent (mip 2048 == original 2048) so only the per-level
	// footprint comparison can reject: 64x4 RGBA8 pads to 1024, not 2048.
	GDEX_ASSERT_TRUE(patch_v2_mip_table(base, patched, 0, 2048u, true, 2048u));

	GDEX_EXPECT_TRUE(reader->read_header(godot::String(patched)).is_empty());
	GDEX_EXPECT_TRUE(loader_load(patched).is_null());

	remove_user_file(base);
	remove_user_file(patched);
}

GDEX_TEST_T(loader, small_mip_chain_loads, TAG_INTEGRATION) {
	// Regression for the too-strict width*height*32 clause: an 8x8 RGBA8 full
	// chain costs 3840 padded bytes against a 2048 heuristic, so the writer's own
	// output used to be refused by every reader.
	const char *path = "user://gdextest_loader_small_chain.dstex";
	if (!write_rgba8_chain(path, 8, 8)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSTex> reader;
	reader.instantiate();
	const godot::Dictionary header = reader->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());

	godot::Ref<godot::Texture2D> tex = loader_load(path);
	GDEX_ASSERT_TRUE(tex.is_valid());
	GDEX_EXPECT_EQ(tex->get_width(), 8);
	GDEX_EXPECT_EQ(tex->get_height(), 8);

	remove_user_file(path);
}

GDEX_TEST_T(loader, narrow_mip_chain_loads, TAG_INTEGRATION) {
	// A 4-px-wide texture pads every mip row to 256 bytes; its footprint sum
	// (524032) is 4x the width*height*32 heuristic (131072) and used to be
	// rejected outright.
	const char *path = "user://gdextest_loader_narrow_chain.dstex";
	if (!write_rgba8_chain(path, 4, 1024)) {
		GDEX_ABORT_TEST("fixture DSTex write failed");
	}
	godot::Ref<godot::DSTex> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(!reader->read_header(godot::String(path)).is_empty());

	godot::Ref<godot::Texture2D> tex = loader_load(path);
	GDEX_ASSERT_TRUE(tex.is_valid());
	GDEX_EXPECT_EQ(tex->get_width(), 4);
	GDEX_EXPECT_EQ(tex->get_height(), 1024);

	remove_user_file(path);
}
