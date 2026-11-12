#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "integration/ds_texture_cache.h"
#include "integration/ds_texture_owner.h"

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>
#include <memory>
#include <vector>

// Eviction ownership: a device texture RID is released only when no holder can
// still obtain a Ref to the wrapper that owns it.
//
// The cache keeps exactly one Ref per canonical entry, so the evictability
// guard's `get_reference_count() > 1` is a sample of the holders that exist
// when the victim is selected - not a proof about the release that follows the
// unlock. A holder can appear in between (the engine Resident returned by
// ResourceLoader.load, a loader worker handing a Ref to the main thread), and
// freeing the RID then hands a live wrapper a dead device texture. The fix
// transfers the RID to the texture's DSTextureOwner before the last cache Ref
// drops, so the release happens where no new Ref can exist.
//
// This suite needs a live RenderingDevice (windowed D3D12): it creates real
// device textures directly, so it needs no DirectStorage request and no
// DSContext initialization. Suite name starts with gpu_ so the default
// -Filter 'gpu_*' of run_d3d12_windowed.ps1 selects it.
//
// The post-sample holder is injected through a DEBUG-only, unbound seam because
// the window is a handful of instructions on the render thread: no script or
// stress loop can place a holder inside it deterministically.

GDEX_TEST_T(gpu_cache_evict, external_holder_after_evict_sample_keeps_rid, TAG_INTEGRATION) {
#if !defined(DEBUG_ENABLED) && !defined(DEV_ENABLED)
	// The seam is compiled out of release builds; the invariant it proves is
	// still covered in debug/dev builds, which is where the suite runs.
	(void)ctx;
	GDEX_SKIP("eviction-probe seam is compiled out of this build (release)");
#else
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
	}
	if (!rs->is_on_render_thread()) {
		// The pre-fix leg relies on the release running inline on the render
		// thread; a body on another thread would make the discriminator pass
		// vacuously. Skip rather than claim a green that proves nothing.
		GDEX_SKIP("test body is not on the render thread; the deferred drain cannot be forced here");
	}

	struct State {
		godot::RID control_rid;
		godot::RID held_rid;
		uint64_t held_id = 0;
		std::vector<godot::Ref<godot::Texture2D>> probe;
	};
	auto st = std::make_shared<State>();

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	cache->set_enabled(true);
	cache->set_max_entries(256);

	ctx.add_teardown([rd, cache, st]() {
		// Disarm BEFORE anything can run again: the probe storage dies with
		// `st`, and a seam left armed would inject Refs into another suite's
		// eviction and write into freed memory.
		godot::DSTextureCache::_set_evict_post_sample_probe_for_testing(nullptr);
		st->probe.clear();
		cache->clear();
		memdelete(cache);
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		if (st->held_rid.is_valid() && rd->texture_is_valid(st->held_rid)) {
			rd->free_rid(st->held_rid);
		}
		if (st->control_rid.is_valid() && rd->texture_is_valid(st->control_rid)) {
			rd->free_rid(st->control_rid);
		}
	});

	godot::Ref<godot::RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_width(4);
	fmt->set_height(4);
	fmt->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fmt->set_mipmaps(1);
	fmt->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
	fmt->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	godot::Ref<godot::RDTextureView> view;
	view.instantiate();

	const godot::String control_path("res://gdextest_gpu_cache_evict_control");
	const godot::String held_path("res://gdextest_gpu_cache_evict_held");

	// Control: same path kind, same texture shape, no holder. Eviction must
	// still release the RID, so "the cache simply stopped freeing" cannot pass
	// the discriminator below.
	{
		godot::RID rid = rd->texture_create(fmt, view, godot::TypedArray<godot::PackedByteArray>());
		GDEX_ASSERT_TRUE(rid.is_valid());
		st->control_rid = rid;
		godot::Ref<godot::Texture2DRD> tex;
		tex.instantiate();
		tex->set_texture_rd_rid(rid);
		GDEX_EXPECT_TRUE(cache->store(control_path, tex));
		// Pristine count (GODOT_QUIRKS.md section 39): drop the caller's Ref so
		// the cache is the only owner and the entry is evictable.
		tex.unref();
		cache->evict(control_path);
		GDEX_EXPECT_FALSE(rd->texture_is_valid(st->control_rid));
	}

	// Defect leg: store, drop the caller's Ref, then inject the holder after
	// the evictability sample and evict.
	{
		godot::RID rid = rd->texture_create(fmt, view, godot::TypedArray<godot::PackedByteArray>());
		GDEX_ASSERT_TRUE(rid.is_valid());
		st->held_rid = rid;
		godot::Ref<godot::Texture2DRD> tex;
		tex.instantiate();
		tex->set_texture_rd_rid(rid);
		st->held_id = tex->get_instance_id();
		GDEX_ASSERT_TRUE(cache->store(held_path, tex));
		tex.unref();
		GDEX_EXPECT_EQ(cache->get_cached_count(), 1);

		godot::DSTextureCache::_set_evict_post_sample_probe_for_testing(&st->probe);
		cache->evict(held_path);
		godot::DSTextureCache::_set_evict_post_sample_probe_for_testing(nullptr);
	}

	// The seam fired exactly once, on the evicted texture, and the entry is
	// gone. Without this a seam that never ran would let the discriminator pass
	// for the wrong reason (nothing evicted, RID still valid).
	GDEX_EXPECT_EQ(static_cast<int64_t>(st->probe.size()), static_cast<int64_t>(1));
	if (st->probe.size() != 1) {
		GDEX_FAIL("the post-sample probe did not fire; the eviction never reached the release");
		return;
	}
	GDEX_EXPECT_TRUE(st->probe[0].is_valid());
	GDEX_EXPECT_EQ(static_cast<int64_t>(st->probe[0]->get_instance_id()), static_cast<int64_t>(st->held_id));
	GDEX_EXPECT_EQ(cache->get_cached_count(), 0);

	// Discriminator: with a live holder, the device texture must still exist.
	// The flush forces the deferred queue for the case where the body is not the
	// render thread; it is a no-op for an owner-held RID.
	godot::DSTextureCache::flush_deferred_rid_releases(true);
	GDEX_EXPECT_TRUE(rd->texture_is_valid(st->held_rid));
	// ... and the pass must come from the ownership transfer, not from a
	// decision to stop freeing evicted RIDs.
	GDEX_EXPECT_TRUE(godot::DSTextureOwner::has_owner(st->probe[0]));

	// Not a leak: dropping the last holder releases the RID.
	st->probe.clear();
	godot::DSTextureCache::flush_deferred_rid_releases(true);
	GDEX_EXPECT_FALSE(rd->texture_is_valid(st->held_rid));
#endif
}

namespace {

// 4x4 device texture wrapped exactly like the GPU loader path wraps one:
// Texture2DRD is a non-owning view, so the RID outlives the wrapper unless an
// owner releases it. That is the premise of every assertion below.
struct DeviceTexture {
	godot::RID rid;
	godot::Ref<godot::Texture2DRD> texture;
};

DeviceTexture make_device_texture(godot::RenderingDevice *p_rd,
		const godot::Ref<godot::RDTextureFormat> &p_format,
		const godot::Ref<godot::RDTextureView> &p_view) {
	DeviceTexture out;
	out.rid = p_rd->texture_create(p_format, p_view, godot::TypedArray<godot::PackedByteArray>());
	if (out.rid.is_valid()) {
		out.texture.instantiate();
		out.texture->set_texture_rd_rid(out.rid);
	}
	return out;
}

} // namespace

// #0258: evict() / clear() / set_enabled(false) drop their entry even when an
// outside holder still draws the texture. Such an entry must hand its device
// RID to a DSTextureOwner: the holder keeps the texture alive, and the owner
// releases the RID when the last holder goes. Skipping the transfer orphans
// the RID on Texture2DRD death (the wrapper never frees it).
//
// The control leg is the discriminator: an unheld victim must still be freed,
// so a build that simply stopped freeing evicted RIDs cannot pass this test.
GDEX_TEST_T(gpu_cache_evict, explicit_drop_transfers_ownership_for_held_textures, TAG_INTEGRATION) {
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
	}
	if (!rs->is_on_render_thread()) {
		// The owner's destructor frees inline on the render thread; elsewhere
		// the release is queued for a later frame, which this synchronous body
		// cannot observe.
		GDEX_SKIP("test body is not on the render thread; the release cannot be observed synchronously");
	}

	struct State {
		godot::RID rids[4];
	};
	auto st = std::make_shared<State>();

	godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
	cache->set_enabled(true);
	cache->set_max_entries(256);
	ctx.add_teardown([rd, cache, st]() {
		cache->clear();
		memdelete(cache);
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		// Backstop: a failed assertion above must not become a leak the
		// renderer reports at teardown.
		for (int i = 0; i < 4; ++i) {
			if (st->rids[i].is_valid() && rd->texture_is_valid(st->rids[i])) {
				rd->free_rid(st->rids[i]);
			}
		}
	});

	godot::Ref<godot::RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_width(4);
	fmt->set_height(4);
	fmt->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fmt->set_mipmaps(1);
	fmt->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
	fmt->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	godot::Ref<godot::RDTextureView> view;
	view.instantiate();

	// Control: no outside holder. Eviction must still release the RID.
	{
		DeviceTexture t = make_device_texture(rd, fmt, view);
		GDEX_ASSERT_TRUE(t.rid.is_valid());
		st->rids[0] = t.rid;
		const godot::String path("res://gdextest_drop_control");
		GDEX_EXPECT_TRUE(cache->store(path, t.texture, godot::DSTextureCache::CACHE_MODE_REUSE));
		t.texture.unref(); // the cache is the only owner
		cache->evict(path);
		GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		GDEX_EXPECT_FALSE(rd->texture_is_valid(st->rids[0]));
	}

	// evict() on a held texture: the RID lives with the holder, then dies.
	{
		DeviceTexture t = make_device_texture(rd, fmt, view);
		GDEX_ASSERT_TRUE(t.rid.is_valid());
		st->rids[1] = t.rid;
		const godot::String path("res://gdextest_drop_evict_held");
		GDEX_EXPECT_TRUE(cache->store(path, t.texture, godot::DSTextureCache::CACHE_MODE_REUSE));
		cache->evict(path);
		GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
		GDEX_EXPECT_TRUE(rd->texture_is_valid(st->rids[1]));
		GDEX_EXPECT_TRUE(godot::DSTextureOwner::has_owner(t.texture));
		t.texture.unref();
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		GDEX_EXPECT_FALSE(rd->texture_is_valid(st->rids[1]));
	}

	// clear() on a held texture.
	{
		DeviceTexture t = make_device_texture(rd, fmt, view);
		GDEX_ASSERT_TRUE(t.rid.is_valid());
		st->rids[2] = t.rid;
		const godot::String path("res://gdextest_drop_clear_held");
		GDEX_EXPECT_TRUE(cache->store(path, t.texture, godot::DSTextureCache::CACHE_MODE_REUSE));
		cache->clear();
		GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
		GDEX_EXPECT_TRUE(rd->texture_is_valid(st->rids[2]));
		GDEX_EXPECT_TRUE(godot::DSTextureOwner::has_owner(t.texture));
		t.texture.unref();
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		GDEX_EXPECT_FALSE(rd->texture_is_valid(st->rids[2]));
	}

	// set_enabled(false) on a held texture.
	{
		DeviceTexture t = make_device_texture(rd, fmt, view);
		GDEX_ASSERT_TRUE(t.rid.is_valid());
		st->rids[3] = t.rid;
		const godot::String path("res://gdextest_drop_disable_held");
		GDEX_EXPECT_TRUE(cache->store(path, t.texture, godot::DSTextureCache::CACHE_MODE_REUSE));
		cache->set_enabled(false);
		GDEX_EXPECT_EQ(cache->get_cached_count(), 0);
		GDEX_EXPECT_TRUE(rd->texture_is_valid(st->rids[3]));
		GDEX_EXPECT_TRUE(godot::DSTextureOwner::has_owner(t.texture));
		t.texture.unref();
		godot::DSTextureCache::flush_deferred_rid_releases(true);
		GDEX_EXPECT_FALSE(rd->texture_is_valid(st->rids[3]));
	}
}
