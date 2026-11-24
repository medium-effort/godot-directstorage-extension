#include "ds_texture_cache.h"
#include "ds_preloader.h"
#include "ds_texture_owner.h"
#include "ds_texture_pipeline_common.h"
#include "../core/ds_context.h"
#include "core/ds_settings.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <mutex>
#include <vector>

namespace godot {

// Evict-path GPU resource ownership.
//
// store() makes cached textures engine-resident via take_over_path(), and the
// GPU-backed entries own a RenderingDevice texture RID created by
// texture_create(). The texture object itself is a non-owning view: dropping
// the last Ref does NOT free the device texture, so every cache-side
// owner-loss (evict / clear / capacity replace / disable / shutdown) must hand
// the RID to someone before the cache's Ref goes away, or it leaks until device
// teardown.
//
// Release discipline:
// - Eviction never frees a RID. It transfers the RID to a DSTextureOwner
//   carried by the texture, so the free runs inside the destruction of the
//   last Ref - the only point at which no holder can still obtain one. The
//   evictability guard's reference-count read is a sample of the holders that
//   existed when the victim was selected, not proof about the release: a
//   holder that appears after the sample (the engine Resident returned by
//   ResourceLoader.load, a loader worker handing a Ref to the main thread)
//   keeps the RID alive with it.
// - Explicit drops (evict / clear / disable) transfer ownership even when an
//   outside holder still draws the texture: the entry leaves the map either
//   way, so keeping the RID back would orphan it (#0258). Capacity eviction
//   (`_evict_oldest` / `set_max_entries`) is the different case: it can pick a
//   different victim, so it keeps a held entry cached instead.
// - RenderingDevice::free_rid() asserts the render thread
//   (ERR_RENDER_THREAD_GUARD), so the owner's release path frees inline on the
//   render thread and otherwise queues the RID and posts a drain worker via
//   RenderingServer::call_on_render_thread(). Missing/dead singletons (headless
//   Dummy, module teardown) skip the free: no live device means no live GPU
//   resource.
// - Idempotent: the map entry is removed under the cache lock before ownership
//   moves, so a repeat evict/clear finds nothing and transfers nothing; only
//   valid RIDs are ever armed, and one texture is armed once per RID. A
//   liveness check skips stale handles from re-stored evicted textures, so
//   re-entry can never double-free ("Attempted to free invalid ID").
// - Everything below runs OUTSIDE the cache lock (attach() takes the owner
//   table lock, free_rid takes RenderingDevice locks, the deferred path crosses
//   threads). No drain-logic change: context/backend queues are untouched.
//
// Second owner (#0126): the loader's uncached GPU path attaches a
// DSTextureOwner to the texture instead, and that owner calls the static
// release_owned_rid() below - same discipline, same queue, so both owners
// share one free path.
static std::mutex s_evict_free_mutex;
static std::vector<RID> s_evict_free_queue;
// One drain hop at a time. Cleared at the start of every drain.
static bool s_evict_drain_scheduled = false;
// Consecutive drains that found only in-flight RIDs (reset on progress), so a
// permanently in-flight slot cannot spin a retry forever.
static int s_evict_drain_retries = 0;
static const int kEvictDrainMaxRetries = 240; // ~4 s at 60 fps; stays queued after that

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Armed only by the closure test for the eviction-ownership invariant. Not
// thread-local on purpose: the seam is a test control, not production state.
static std::vector<Ref<Texture2D>> *s_evict_post_sample_probe = nullptr;
void DSTextureCache::_set_evict_post_sample_probe_for_testing(std::vector<Ref<Texture2D>> *p_out) {
	s_evict_post_sample_probe = p_out;
}
#endif

static void ds_texture_cache_drain_evict_frees_on_render_thread();

// Schedules exactly one drain. Any thread: on the render thread it runs inline,
// otherwise it is posted to the render thread. Public entry points call this,
// so deferred-only releases get a retry too (#0126 drain hardening).
static void ds_texture_cache_schedule_drain() {
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		if (s_evict_drain_scheduled) {
			return;
		}
		s_evict_drain_scheduled = true;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		// Engine/device gone: the queue dies with it. Clear the flag so a late
		// release cannot wedge future scheduling.
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_drain_scheduled = false;
		return;
	}
	if (rs->is_on_render_thread()) {
		ds_texture_cache_drain_evict_frees_on_render_thread();
		return;
	}
	rs->call_on_render_thread(callable_mp_static(ds_texture_cache_drain_evict_frees_on_render_thread));
}

// Next-frame retry for RIDs that were still in flight. MUST run on the render
// thread: request_frame_drawn_callback() appends to an unlocked list and cannot
// be called from workers. Deliberately does not set s_evict_drain_scheduled, so
// a normal release can still schedule a drain if frames are not drawn.
static void ds_texture_cache_retry_drain_next_frame() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	rs->request_frame_drawn_callback(callable_mp_static(ds_texture_cache_drain_evict_frees_on_render_thread));
}

static void ds_texture_cache_drain_evict_frees_on_render_thread() {
	std::vector<RID> pending;
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_drain_scheduled = false;
		pending.swap(s_evict_free_queue);
	}
	if (pending.empty()) {
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_drain_retries = 0;
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		// Cannot free here (no device, or teardown already tore it down).
		// Re-queue instead of dropping the batch so a later release - or the
		// teardown flush - still sees these RIDs. Deliberately no retry is
		// armed: a null device means there is nothing to free, and Godot never
		// brings a RenderingDevice up later in the process.
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_free_queue.insert(s_evict_free_queue.end(), pending.begin(), pending.end());
		return;
	}
	DSContext *ctx = DSContext::get_singleton();
	std::vector<RID> still_in_flight;
	bool freed_any = false;
	for (size_t i = 0; i < pending.size(); ++i) {
		if (pending[i].is_valid()) {
			if (ctx && ctx->is_resource_in_flight(pending[i])) {
				still_in_flight.push_back(pending[i]);
				continue;
			}
			if (rd->texture_is_valid(pending[i])) {
				rd->free_rid(pending[i]);
				freed_any = true;
			}
		}
	}
	if (still_in_flight.empty()) {
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_drain_retries = 0;
		return;
	}
	int retries = 0;
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		s_evict_free_queue.insert(s_evict_free_queue.end(), still_in_flight.begin(), still_in_flight.end());
		if (freed_any) {
			s_evict_drain_retries = 0;
		}
		retries = ++s_evict_drain_retries;
	}
	if (retries > kEvictDrainMaxRetries) {
		if (retries == kEvictDrainMaxRetries + 1) {
			UtilityFunctions::push_warning("DSTextureCache: ", (int64_t)still_in_flight.size(), " texture RID(s) still in flight after ", (int64_t)kEvictDrainMaxRetries, " retries; leaving them queued for the next release or teardown.");
		}
		return;
	}
	// Retry on the next drawn frame: the request that owns the RID is polled
	// from the main loop, so this is the earliest useful moment.
	ds_texture_cache_retry_drain_next_frame();
}

static void ds_texture_cache_free_texture_rids(const std::vector<RID> &p_rids) {
	if (p_rids.empty()) {
		return;
	}
	DSContext *ctx = DSContext::get_singleton();
	std::vector<RID> immediate_rids;
	std::vector<RID> deferred_rids;
	for (size_t i = 0; i < p_rids.size(); ++i) {
		if (!p_rids[i].is_valid()) {
			continue;
		}
		if (ctx && ctx->is_resource_in_flight(p_rids[i])) {
			deferred_rids.push_back(p_rids[i]);
		} else {
			immediate_rids.push_back(p_rids[i]);
		}
	}

	if (!deferred_rids.empty()) {
		{
			std::lock_guard<std::mutex> lock(s_evict_free_mutex);
			for (size_t i = 0; i < deferred_rids.size(); ++i) {
				s_evict_free_queue.push_back(deferred_rids[i]);
			}
			// A new wave of in-flight releases deserves fresh retries: without
			// this, a capped counter from an older stuck RID would also suppress
			// frame retries for these.
			s_evict_drain_retries = 0;
		}
		// Deferred releases always get a retry, including when there are no
		// immediate RIDs and when the device/thread guards below bail out.
		ds_texture_cache_schedule_drain();
	}

	if (immediate_rids.empty()) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		// No server means no device and no live GPU resource: dropping the
		// immediate set is safe. Any deferred RIDs were already queued above
		// and stay visible to the teardown flush.
		return;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (rd == nullptr) {
			return;
		}
		for (size_t i = 0; i < immediate_rids.size(); ++i) {
			if (rd->texture_is_valid(immediate_rids[i])) {
				rd->free_rid(immediate_rids[i]);
			}
		}
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		for (size_t i = 0; i < immediate_rids.size(); ++i) {
			s_evict_free_queue.push_back(immediate_rids[i]);
		}
	}
	ds_texture_cache_schedule_drain();
}

static RID ds_texture_cache_extract_rd_rid(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return RID();
	}
	Ref<Texture2DRD> tex_rd(p_texture);
	if (tex_rd.is_null()) {
		return RID(); // ImageTexture: RS RID auto-frees on Ref drop.
	}
	return tex_rd->get_texture_rd_rid();
}

// Drops evicted textures that are still referenced by a remaining cache
// entry (twin-key guard: prewarm stores one texture under engine_path +
// payload_key; freeing while the twin entry is live would kill a cached
// texture, and erasing both twins at once would otherwise free one RID
// twice). p_live holds every texture pointer still in the map; callers build
// it under cache_mutex AFTER their erasures.
static void ds_texture_cache_drop_live_refs(std::vector<Ref<Texture2D>> &p_evicted, const std::vector<const Texture2D *> &p_live) {
	for (size_t i = 0; i < p_evicted.size();) {
		const Texture2D *ptr = p_evicted[i].is_valid() ? p_evicted[i].ptr() : nullptr;
		bool still_cached = false;
		if (ptr != nullptr) {
			for (size_t j = 0; j < p_live.size(); ++j) {
				if (p_live[j] == ptr) {
					still_cached = true;
					break;
				}
			}
		}
		if (still_cached) {
			p_evicted.erase(p_evicted.begin() + i);
		} else {
			++i;
		}
	}
}

// External-holder guard (GODOT_QUIRKS.md section 35): the cache holds exactly
// one Ref per canonical entry (aliases add no Refs), so a reference count
// above 1 means an outside owner still draws the texture (scene material,
// TextureRect, handed-out get_cached() Ref, engine ResourceCache resident via
// take_over_path). Evicting such an entry and freeing its Texture2DRD RID
// would kill a texture the visible scene still binds (per-frame
// uniform_set_create/draw_list spam). Null/invalid entries hold no live
// object and stay evictable, preserving today's behavior.
static bool ds_texture_cache_is_externally_held(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null() || !p_texture.is_valid()) {
		return false;
	}
	return p_texture->get_reference_count() > 1;
}

// Releases evicted textures WITHOUT freeing anything here: the cache has just
// dropped its only Ref, so this is the last instant at which it can still name
// the device RID. Ownership moves to a DSTextureOwner carried by the texture,
// which releases it inside the destruction of the last Ref - the only point at
// which no other holder can still obtain one. The guard that selected the
// victim is a sample of the holders that existed then, not a proof about the
// release; a holder that appears after the sample keeps the RID alive with it.
// MUST be called WITHOUT cache_mutex held.
static void ds_texture_cache_release_evicted(std::vector<Ref<Texture2D>> &p_evicted) {
	if (p_evicted.empty()) {
		return;
	}
	for (size_t i = 0; i < p_evicted.size(); ++i) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (s_evict_post_sample_probe != nullptr) {
			// Test seam: a holder that appears after the evictability sample.
			s_evict_post_sample_probe->push_back(p_evicted[i]);
		}
#endif
		RID rid = ds_texture_cache_extract_rd_rid(p_evicted[i]);
		if (rid.is_valid()) {
			// Idempotent: twin entries reference one texture, and attach()
			// returns early when the same RID is already armed.
			DSTextureOwner::attach(p_evicted[i], rid);
		}
	}
	if (dsgd_texture_pipeline::is_dummy_renderer()) {
		std::lock_guard<std::mutex> lock(dsgd_texture_pipeline::dummy_texture_owner_mutex());
		p_evicted.clear();
	} else {
		p_evicted.clear();
	}
}

DSTextureCache *DSTextureCache::singleton = nullptr;
std::mutex DSTextureCache::singleton_mutex;

DSTextureCache *DSTextureCache::get_singleton() {
	std::lock_guard<std::mutex> lock(singleton_mutex);
	return singleton;
}

void DSTextureCache::_bind_methods() {
	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REUSE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REPLACE);
	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE_DEEP);

	ClassDB::bind_method(D_METHOD("get_cached", "path", "cache_mode"), &DSTextureCache::get_cached, DEFVAL(static_cast<int>(CACHE_MODE_REUSE)));
	ClassDB::bind_method(D_METHOD("store", "path", "texture", "cache_mode", "alias_path"), &DSTextureCache::store, DEFVAL(static_cast<int>(CACHE_MODE_REUSE)), DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("add_alias", "canonical_path", "alias_path"), &DSTextureCache::add_alias);
	ClassDB::bind_method(D_METHOD("remove_alias", "alias_path"), &DSTextureCache::remove_alias);
	ClassDB::bind_method(D_METHOD("has_alias", "alias_path"), &DSTextureCache::has_alias);
	ClassDB::bind_method(D_METHOD("get_canonical_path", "path"), &DSTextureCache::get_canonical_path);
	ClassDB::bind_method(D_METHOD("evict", "path"), &DSTextureCache::evict);
	ClassDB::bind_method(D_METHOD("clear"), &DSTextureCache::clear);
	ClassDB::bind_method(D_METHOD("clear_unused"), &DSTextureCache::clear_unused);

	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &DSTextureCache::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &DSTextureCache::is_enabled);
	ClassDB::bind_method(D_METHOD("is_cache_mode_ignored", "cache_mode"), &DSTextureCache::is_cache_mode_ignored);

	ClassDB::bind_method(D_METHOD("set_max_entries", "max"), &DSTextureCache::set_max_entries);
	ClassDB::bind_method(D_METHOD("get_max_entries"), &DSTextureCache::get_max_entries);
	ClassDB::bind_method(D_METHOD("get_cached_count"), &DSTextureCache::get_cached_count);

	ClassDB::bind_method(D_METHOD("prewarm_async", "paths"), &DSTextureCache::prewarm_async);
	ClassDB::bind_method(D_METHOD("prewarm_cpu_paced_async", "paths"), &DSTextureCache::prewarm_cpu_paced_async);
	ClassDB::bind_method(D_METHOD("pump_cpu_prewarm", "max_items", "max_bytes"), &DSTextureCache::pump_cpu_prewarm);
	ClassDB::bind_method(D_METHOD("get_queued_cpu_prewarm_count", "tag"), &DSTextureCache::get_queued_cpu_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_in_flight_cpu_prewarm_count", "tag"), &DSTextureCache::get_in_flight_cpu_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_in_flight_cpu_prewarm_bytes", "tag"), &DSTextureCache::get_in_flight_cpu_prewarm_bytes, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("prewarm_gpu_async", "paths", "chunk_size", "prefer_gpu", "max_chunk_bytes"), &DSTextureCache::prewarm_gpu_async, DEFVAL(16), DEFVAL(true), DEFVAL((int64_t)0));
	ClassDB::bind_method(D_METHOD("pump_gpu_prewarm"), &DSTextureCache::pump_gpu_prewarm);
	ClassDB::bind_method(D_METHOD("is_gpu_prewarm_available"), &DSTextureCache::is_gpu_prewarm_available);
	ClassDB::bind_method(D_METHOD("poll_completed", "tag", "max_items", "max_usec", "gpu_only"), &DSTextureCache::poll_completed, DEFVAL((uint64_t)0), DEFVAL(0), DEFVAL(4000), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_pending_prewarm_count", "tag"), &DSTextureCache::get_pending_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_unbuilt_prewarm_count", "tag"), &DSTextureCache::get_unbuilt_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_unbuilt_cpu_prewarm_count", "tag"), &DSTextureCache::get_unbuilt_cpu_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_unbuilt_gpu_prewarm_count", "tag"), &DSTextureCache::get_unbuilt_gpu_prewarm_count, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("has_gpu_requests_in_flight", "tag"), &DSTextureCache::has_gpu_requests_in_flight, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("set_background_pacing", "enabled"), &DSTextureCache::set_background_pacing);
	ClassDB::bind_method(D_METHOD("is_background_pacing"), &DSTextureCache::is_background_pacing);

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_entries"), "set_max_entries", "get_max_entries");
}

DSTextureCache::DSTextureCache() {
	{
		std::lock_guard<std::mutex> lock(singleton_mutex);
		singleton = this;
	}

	if (dsgd_settings::has_setting("texture_cache_enabled", "texture_cache_enabled")) {
		enabled = dsgd_settings::get_setting("texture_cache_enabled", "texture_cache_enabled");
	}
	if (dsgd_settings::has_setting("texture_cache_max_entries", "texture_cache_max_entries")) {
		max_entries = dsgd_settings::get_setting("texture_cache_max_entries", "texture_cache_max_entries");
		if (max_entries < 1) {
			max_entries = 512;
		}
	}
	// DSTextureCache is constructed at MODULE_INITIALIZATION_LEVEL_SCENE before
	// RenderingServer is registered; querying get_gpu_adapter_info() before
	// RenderingServer exists logs a singleton error in debug and crashes in
	// template_release. Defer the VRAM guard until RenderingServer is up
	// (DSContext::initialize_ds() calls apply_vram_budget_guard() as well).
	Engine *engine = Engine::get_singleton();
	if (engine && engine->has_singleton("RenderingServer")) {
		apply_vram_budget_guard();
	}
}

void DSTextureCache::apply_vram_budget_guard(const Dictionary &p_adapter) {
	Dictionary adapter = p_adapter;
	if (adapter.is_empty()) {
		DSContext *ctx = DSContext::get_singleton();
		if (!ctx) {
			return;
		}
		adapter = ctx->get_gpu_adapter_info();
	}

	const bool auto_integrated = static_cast<bool>(dsgd_settings::get_setting("auto_tune_for_integrated_gpu", "auto_tune_for_integrated_gpu", Variant(true)));
	const bool auto_low_vram = static_cast<bool>(dsgd_settings::get_setting("auto_tune_for_low_vram_gpu", "auto_tune_for_low_vram_gpu", Variant(true)));
	const bool available = static_cast<bool>(adapter.get("available", false));
	const bool integrated = static_cast<bool>(adapter.get("is_uma", false)) ||
			static_cast<bool>(adapter.get("integrated_gpu_guess", false));
	const int64_t dedicated_mb = static_cast<int64_t>(adapter.get("dedicated_vram_mb", static_cast<int64_t>(0)));
	int cap = 0;
	if (auto_integrated && integrated) {
		cap = 128;
	} else if (auto_low_vram && available && !integrated && dedicated_mb > 0) {
		if (dedicated_mb < 4096) {
			cap = 128;
		} else if (dedicated_mb < 7168) {
			// DXGI-safe 8 GiB boundary: real 8 GiB cards report ~7900-8060 MiB,
			// so 7168 keeps them at the full 512 default.
			cap = 256;
		}
	}
	if (cap > 0) {
		std::lock_guard<std::mutex> lock(cache_mutex);
		if (!max_entries_user_overridden) {
			max_entries = std::min(max_entries, cap);
		}
	}
}

DSTextureCache::~DSTextureCache() {
	{
		std::lock_guard<std::mutex> lock(singleton_mutex);
		if (singleton == this) {
			singleton = nullptr;
		}
	}
	clear();
	// Release preloader-held request Refs + archive handle while Godot is
	// still alive so no Ref outlives module teardown.
	DSPreloader::shutdown();
}

bool DSTextureCache::_is_entry_evictable_locked(const String &p_path, const Ref<Texture2D> &p_texture) const {
	if (in_flight_takeovers.has(p_path)) {
		return false;
	}
	if (ds_texture_cache_is_externally_held(p_texture)) {
		return false;
	}
	return true;
}

// Evicts the oldest entry not held outside the cache, retaining its Ref in
// r_evicted for the caller's render-thread RID release. The external-holder
// scan MUST run on pristine counts: callers must not push candidate Refs
// anywhere before this call (each push_back adds one to the reference count
// and would make the true oldest look externally held, silently shifting
// eviction to newer entries). Returns false when every entry is externally
// held (or the list is empty), in which case r_evicted is untouched and the
// caller keeps temporary over-capacity instead of force-freeing a live RID.
bool DSTextureCache::_evict_oldest(std::vector<Ref<Texture2D>> &r_evicted) {
	if (lru_list.is_empty()) {
		return false;
	}

	// Eviction guard: scan oldest-first and skip entries still held outside
	// the cache (their Texture2DRD RID must never be freed while drawn).
	// Stale LRU nodes with no map entry are pruned inline so the scan can
	// continue toward newer candidates.
	for (List<String>::Element *e = lru_list.back(); e != nullptr;) {
		List<String>::Element *older = e->prev();
		const String path = e->get();
		if (!cache_map.has(path)) {
			lru_list.erase(e);
		} else if (_is_entry_evictable_locked(path, cache_map[path].texture)) {
			if (cache_map[path].prewarm_origin) {
				prewarm_evictions++;
				if (!warned_prewarm_eviction) {
					warned_prewarm_eviction = true;
					UtilityFunctions::push_warning("DSTextureCache: a prewarmed texture was evicted before its batch finished; raise dsgd_extension/texture_cache_max_entries or prewarm in smaller batches.");
				}
			}
			r_evicted.push_back(cache_map[path].texture);
			_remove_entry(path);
			return true;
		}
		e = older;
	}
	return false;
}

void DSTextureCache::_remove_entry(const String &p_canonical_path) {
	if (!cache_map.has(p_canonical_path)) {
		return;
	}

	CacheEntry entry = cache_map[p_canonical_path];
	for (int i = 0; i < entry.aliases.size(); ++i) {
		alias_map.erase(entry.aliases[i]);
	}
	if (entry.lru_element) {
		lru_list.erase(entry.lru_element);
	}
	cache_map.erase(p_canonical_path);
}

String DSTextureCache::_resolve_canonical_locked(const String &p_path) const {
	if (cache_map.has(p_path)) {
		return p_path;
	}
	if (alias_map.has(p_path)) {
		return alias_map[p_path];
	}
	return String();
}

void DSTextureCache::_unbind_alias_locked(const String &p_alias_path) {
	if (!alias_map.has(p_alias_path)) {
		return;
	}
	String canonical = alias_map[p_alias_path];
	alias_map.erase(p_alias_path);
	if (cache_map.has(canonical)) {
		CacheEntry &entry = cache_map[canonical];
		for (int i = 0; i < entry.aliases.size(); ++i) {
			if (entry.aliases[i] == p_alias_path) {
				entry.aliases.remove_at(i);
				break;
			}
		}
	}
}

void DSTextureCache::_bind_alias_locked(const String &p_canonical_path, const String &p_alias_path) {
	if (p_alias_path.is_empty() || p_alias_path == p_canonical_path) {
		return;
	}
	if (!cache_map.has(p_canonical_path)) {
		return;
	}
	if (alias_map.has(p_alias_path)) {
		if (alias_map[p_alias_path] == p_canonical_path) {
			return;
		}
		_unbind_alias_locked(p_alias_path);
	}
	if (cache_map.has(p_alias_path)) {
		return;
	}
	alias_map[p_alias_path] = p_canonical_path;
	CacheEntry &entry = cache_map[p_canonical_path];
	if (!entry.aliases.has(p_alias_path)) {
		entry.aliases.push_back(p_alias_path);
	}
}

bool DSTextureCache::_is_cache_mode_ignored(int p_cache_mode) {
	return p_cache_mode == CACHE_MODE_IGNORE || p_cache_mode == CACHE_MODE_IGNORE_DEEP;
}

bool DSTextureCache::_is_cache_mode_replace(int p_cache_mode) {
	return p_cache_mode == CACHE_MODE_REPLACE;
}

bool DSTextureCache::_can_own_locked(int p_cache_mode) const {
	return enabled && !_is_cache_mode_ignored(p_cache_mode);
}

bool DSTextureCache::can_own(int p_cache_mode) const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return _can_own_locked(p_cache_mode);
}

void DSTextureCache::release_owned_rid(const RID &p_rid) {
	if (!p_rid.is_valid()) {
		return;
	}
	// Reuses the evict-path release discipline: free inline on the render
	// thread, otherwise queue + drain on the render thread, and defer while a
	// DirectStorage request still targets the RID. Static on purpose: late
	// destructors must work without a live cache singleton.
	std::vector<RID> rids;
	rids.push_back(p_rid);
	ds_texture_cache_free_texture_rids(rids);
}

void DSTextureCache::flush_deferred_rid_releases(bool p_ds_drain_clean) {
	if (!p_ds_drain_clean) {
		// DirectStorage could not drain in-flight DMA. Do not touch the queue:
		// freeing a RID that hardware may still be writing to is exactly the
		// use-after-free the deferred queue exists to avoid. Device teardown
		// reclaims them, which is what the backend's own timeout path assumes.
		UtilityFunctions::print_verbose("DSTextureCache: DS shutdown did not drain; leaving deferred RIDs for device teardown.");
		return;
	}
	// Check the device/thread BEFORE swapping: a swap-then-bail would silently
	// lose the queue (neither freed nor left for a later drain).
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr || !rs->is_on_render_thread()) {
		UtilityFunctions::print_verbose("DSTextureCache: no live device on the render thread at teardown; leaving deferred RIDs for device teardown.");
		return;
	}
	std::vector<RID> pending;
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		pending.swap(s_evict_free_queue);
		s_evict_drain_scheduled = false;
		s_evict_drain_retries = 0;
	}
	if (pending.empty()) {
		return;
	}
	int64_t released = 0;
	for (size_t i = 0; i < pending.size(); ++i) {
		if (pending[i].is_valid() && rd->texture_is_valid(pending[i])) {
			rd->free_rid(pending[i]);
			++released;
		}
	}
	if (released > 0) {
		UtilityFunctions::print_verbose("DSTextureCache: released ", released, " deferred texture RID(s) at teardown.");
	}
}

Ref<Texture2D> DSTextureCache::get_cached(const String &p_path, int p_cache_mode) {
	std::vector<Ref<Texture2D>> evicted;
	Ref<Texture2D> result;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (!enabled || _is_cache_mode_ignored(p_cache_mode) || _is_cache_mode_replace(p_cache_mode)) {
			return Ref<Texture2D>();
		}

		String canonical = _resolve_canonical_locked(p_path);
		if (canonical.is_empty() || !cache_map.has(canonical)) {
			return Ref<Texture2D>();
		}

		CacheEntry &entry = cache_map[canonical];
		if (entry.texture.is_null() || !entry.texture.is_valid()) {
			evicted.push_back(entry.texture);
			_remove_entry(canonical);
		} else {
			if (entry.lru_element && entry.lru_element != lru_list.front()) {
				lru_list.move_to_front(entry.lru_element);
			}

			// The prewarm batch has been consumed by the scene; a later LRU
			// eviction is ordinary turnover, not "evicted before batch finished".
			entry.prewarm_origin = false;
			result = entry.texture;
		}

		// The live-pointer set is only used to filter evicted textures. A pure
		// hit has none, so scanning the whole cache under cache_mutex here is
		// guaranteed work with no effect (measured ~23.7 ms per 4,140 hits
		// with 274 cached textures on the Bistro-like cache-hit path).
		if (!evicted.empty()) {
			std::vector<const Texture2D *> live_refs;
			live_refs.reserve(cache_map.size());
			for (const KeyValue<String, CacheEntry> &kv : cache_map) {
				if (kv.value.texture.is_valid()) {
					live_refs.push_back(kv.value.texture.ptr());
				}
			}
			ds_texture_cache_drop_live_refs(evicted, live_refs);
		}
	}
	if (!evicted.empty()) {
		ds_texture_cache_release_evicted(evicted);
	}
	return result;
}

bool DSTextureCache::store(const String &p_path, const Ref<Texture2D> &p_texture, int p_cache_mode, const String &p_alias_path, bool p_prewarm) {
	Ref<Texture2D> resident;
	bool make_resident = false;
	// Refs of replaced/evicted textures: held until their owned RIDs are
	// released below (Ref held until terminal), outside cache_mutex.
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (!_can_own_locked(p_cache_mode) || p_texture.is_null() || !p_texture.is_valid() || p_path.is_empty()) {
			return false;
		}

		// If p_path was previously an alias, unbind it so it becomes its own canonical entry.
		if (alias_map.has(p_path)) {
			_unbind_alias_locked(p_path);
		}

		if (cache_map.has(p_path)) {
			CacheEntry &entry = cache_map[p_path];
			// Capacity-replace of an owned texture: the previous Ref loses
			// its cache owner here, so its Texture2DRD RID must be released
			// (same ownership as evict/clear).
			if (entry.texture.is_valid() && entry.texture.ptr() != p_texture.ptr()) {
				evicted.push_back(entry.texture);
			}
			entry.texture = p_texture;
			entry.prewarm_origin = p_prewarm;
			if (entry.lru_element && entry.lru_element != lru_list.front()) {
				lru_list.move_to_front(entry.lru_element);
			}
		} else {
			while (static_cast<int>(cache_map.size()) >= max_entries) {
				// _evict_oldest() scans pristine counts, retains the victim's
				// Ref, and removes the entry; still-cached Refs can no longer
				// land in `evicted`, so drop_live_refs below only filters the
				// capacity-replace twin case as before.
				if (!_evict_oldest(evicted)) {
					if (lru_list.is_empty()) {
						// Inconsistent state (entries without LRU nodes):
						// never mass-free live RIDs; keep over-capacity.
						UtilityFunctions::push_warning("DSTextureCache: capacity (", (int64_t)max_entries, ") reached with no LRU order; keeping '", p_path, "' over capacity instead of freeing live RIDs.");
					} else {
						// Every entry is externally held: keep them (temporary
						// over-capacity) instead of force-freeing the oldest
						// live RID (see GODOT_QUIRKS.md section 35).
						UtilityFunctions::push_warning("DSTextureCache: capacity (", (int64_t)max_entries, ") reached with all entries externally held; keeping '", p_path, "' over capacity instead of freeing a live RID.");
					}
					break;
				}
			}

			lru_list.push_front(p_path);
			CacheEntry new_entry;
			new_entry.texture = p_texture;
			new_entry.lru_element = lru_list.front();
			new_entry.prewarm_origin = p_prewarm;
			cache_map[p_path] = new_entry;
		}

		if (!p_alias_path.is_empty() && p_alias_path != p_path) {
			_bind_alias_locked(p_path, p_alias_path);
		}

		// Engine ResourceCache residency (#14): make the stored texture
		// visible to ResourceLoader.load(p_path, ..., CACHE_MODE_REUSE) so
		// later scene binds are pointer-identical with zero disk re-read.
		// take_over_path() registers the resource under p_path in the
		// engine cache (replacing any stale entry). REUSE only fills a hole
		// (never evicts a foreign resident); REPLACE always takes over.
		// Done OUTSIDE cache_mutex below: take_over_path takes the engine
		// ResourceCache lock, and the engine itself calls into loaders while
		// holding load mutexes, so nesting engine locks inside ours risks
		// inversion. A local Ref keeps the texture alive across unlock.
		if (!p_path.is_empty() && (p_cache_mode == CACHE_MODE_REPLACE || p_cache_mode == CACHE_MODE_REUSE)) {
			resident = p_texture;
			if (p_cache_mode == CACHE_MODE_REPLACE) {
				make_resident = true;
			} else {
				ResourceLoader *rl = ResourceLoader::get_singleton();
				make_resident = rl == nullptr || !rl->has_cached(p_path);
			}
			if (make_resident) {
				in_flight_takeovers.insert(p_path);
			}
		}

		std::vector<const Texture2D *> live_refs;
		live_refs.reserve(cache_map.size());
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			if (kv.value.texture.is_valid()) {
				live_refs.push_back(kv.value.texture.ptr());
			}
		}
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}

	// Ownership transfer: a texture that arrived with its own RID owner (the
	// uncached GPU path, #0126) now belongs to the cache. Disarm it so the RID
	// is released exactly once - by evict/replace/clear, never by both.
	DSTextureOwner::disarm(p_texture);

	struct TakeoverGuard {
		std::mutex &mutex;
		HashSet<String> &takeovers;
		String path;
		bool active;
		TakeoverGuard(std::mutex &m, HashSet<String> &t, const String &p, bool act)
				: mutex(m), takeovers(t), path(p), active(act) {}
		~TakeoverGuard() {
			if (active && !path.is_empty()) {
				std::lock_guard<std::mutex> lock(mutex);
				takeovers.erase(path);
			}
		}
	} takeover_guard(cache_mutex, in_flight_takeovers, p_path, make_resident);

	if (make_resident && resident.is_valid()) {
		resident->take_over_path(p_path);
	}
	ds_texture_cache_release_evicted(evicted);
	return true;
}

void DSTextureCache::add_alias(const String &p_canonical_path, const String &p_alias_path) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	if (!enabled || p_canonical_path.is_empty() || p_alias_path.is_empty() || p_canonical_path == p_alias_path) {
		return;
	}
	String canonical = _resolve_canonical_locked(p_canonical_path);
	if (!canonical.is_empty() && cache_map.has(canonical)) {
		_bind_alias_locked(canonical, p_alias_path);
	}
}

void DSTextureCache::remove_alias(const String &p_alias_path) {
	std::lock_guard<std::mutex> lock(cache_mutex);
	_unbind_alias_locked(p_alias_path);
}

bool DSTextureCache::has_alias(const String &p_alias_path) const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return alias_map.has(p_alias_path);
}

String DSTextureCache::get_canonical_path(const String &p_path) const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return _resolve_canonical_locked(p_path);
}

uint64_t DSTextureCache::prewarm_async(const PackedStringArray &p_paths) {
	return DSPreloader::prewarm_async(p_paths);
}

uint64_t DSTextureCache::prewarm_cpu_paced_async(const PackedStringArray &p_paths) {
	return DSPreloader::prewarm_cpu_paced_async(p_paths);
}

void DSTextureCache::pump_cpu_prewarm(int p_max_items, int64_t p_max_bytes) {
	DSPreloader::pump_cpu_prewarm(p_max_items, p_max_bytes);
}

int DSTextureCache::get_queued_cpu_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_queued_cpu_prewarm_count(p_tag);
}

int DSTextureCache::get_in_flight_cpu_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_in_flight_cpu_prewarm_count(p_tag);
}

int64_t DSTextureCache::get_in_flight_cpu_prewarm_bytes(uint64_t p_tag) const {
	return DSPreloader::get_in_flight_cpu_prewarm_bytes(p_tag);
}

uint64_t DSTextureCache::prewarm_gpu_async(const PackedStringArray &p_paths, int p_chunk_size, bool p_prefer_gpu, int64_t p_max_chunk_bytes) {
	return DSPreloader::prewarm_gpu_async(p_paths, p_chunk_size, p_prefer_gpu, p_max_chunk_bytes);
}

void DSTextureCache::pump_gpu_prewarm() {
	DSPreloader::pump_gpu_prewarm();
}

bool DSTextureCache::is_gpu_prewarm_available() {
	return DSPreloader::is_gpu_prewarm_available();
}

int DSTextureCache::poll_completed(uint64_t p_tag, int p_max_items, int p_max_usec, bool p_gpu_only) {
	uint32_t max_usec = 0;
	if (p_max_usec < 0) {
		UtilityFunctions::push_warning("DSTextureCache::poll_completed: negative max_usec (", p_max_usec, ") clamped to 1 us; pass 0 for unbounded.");
		max_usec = 1;
	} else {
		max_usec = static_cast<uint32_t>(p_max_usec);
	}
	return DSPreloader::poll_completed(p_tag, p_max_items, max_usec, String(), p_gpu_only);
}

int DSTextureCache::get_pending_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_pending_prewarm_count(p_tag);
}

int DSTextureCache::get_unbuilt_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_unbuilt_prewarm_count(p_tag);
}

int DSTextureCache::get_unbuilt_cpu_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_unbuilt_cpu_prewarm_count(p_tag);
}

int DSTextureCache::get_unbuilt_gpu_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_unbuilt_gpu_prewarm_count(p_tag);
}

bool DSTextureCache::has_gpu_requests_in_flight(uint64_t p_tag) const {
	return DSPreloader::has_gpu_requests_in_flight(p_tag);
}

void DSTextureCache::set_background_pacing(bool p_enabled) {
	DSPreloader::set_background_pacing(p_enabled);
}

bool DSTextureCache::is_background_pacing() const {
	return DSPreloader::is_background_pacing();
}

void DSTextureCache::evict(const String &p_path) {
	// Idempotent: repeat evict finds no entry and frees nothing.
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		String canonical = _resolve_canonical_locked(p_path);
		if (canonical.is_empty() || !cache_map.has(canonical)) {
			return;
		}

		Ref<Texture2D> victim = cache_map[canonical].texture;
		_remove_entry(canonical);
		// The entry is removed whether or not an outside holder still draws it,
		// so its RID must move to an owner either way. Skipping held textures
		// here left them with no cache owner and no armed owner, orphaning the
		// RID on Texture2DRD death (#0258). release_evicted() arms a
		// DSTextureOwner: an unheld victim is freed when this Ref drops, a held
		// one when its last outside holder goes.
		evicted.push_back(victim);

		std::vector<const Texture2D *> live_refs;
		live_refs.reserve(cache_map.size());
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			if (kv.value.texture.is_valid()) {
				live_refs.push_back(kv.value.texture.ptr());
			}
		}
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}
	ds_texture_cache_release_evicted(evicted);
}

void DSTextureCache::clear() {
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		// clear() drops every entry, including textures an outside holder still
		// draws, so every RID must transfer to a DSTextureOwner (#0258). The
		// owner keeps the device texture alive for those holders and frees it
		// with the last one; skipping held entries here orphaned their RIDs.
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			evicted.push_back(kv.value.texture);
		}
		cache_map.clear();
		alias_map.clear();
		lru_list.clear();
		// Map is empty: no twin can remain; filter is a no-op kept for
		// symmetry with the other owner-loss paths.
		std::vector<const Texture2D *> live_refs;
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}
	ds_texture_cache_release_evicted(evicted);
}

int DSTextureCache::clear_unused() {
	std::vector<Ref<Texture2D>> evicted;
	int evicted_count = 0;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		// 1. Identify unreferenced canonical paths using pristine refcounts
		std::vector<String> to_remove;
		to_remove.reserve(cache_map.size());
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			if (_is_entry_evictable_locked(kv.key, kv.value.texture)) {
				to_remove.push_back(kv.key);
			}
		}

		// 2. Remove unreferenced entries and retain victim refs
		for (const String &path : to_remove) {
			if (cache_map.has(path)) {
				evicted.push_back(cache_map[path].texture);
				_remove_entry(path);
				evicted_count++;
			}
		}

		// 3. Drop live refs to handle twin-key aliasing safely
		std::vector<const Texture2D *> live_refs;
		live_refs.reserve(cache_map.size());
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			if (kv.value.texture.is_valid()) {
				live_refs.push_back(kv.value.texture.ptr());
			}
		}
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}

	// 4. Release GPU RIDs on render thread outside cache_mutex
	ds_texture_cache_release_evicted(evicted);
	return evicted_count;
}

void DSTextureCache::set_enabled(bool p_enabled) {
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (enabled == p_enabled) {
			return;
		}

		enabled = p_enabled;
		if (!enabled) {
			// Disabling drains the cache completely, so ownership of every
			// entry (held or not) transfers to a DSTextureOwner (#0258).
			for (const KeyValue<String, CacheEntry> &kv : cache_map) {
				evicted.push_back(kv.value.texture);
			}
			cache_map.clear();
			alias_map.clear();
			lru_list.clear();
			std::vector<const Texture2D *> live_refs;
			ds_texture_cache_drop_live_refs(evicted, live_refs);
		}
	}
	ds_texture_cache_release_evicted(evicted);
}

bool DSTextureCache::is_enabled() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return enabled;
}

bool DSTextureCache::is_cache_mode_ignored(int p_cache_mode) const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return _is_cache_mode_ignored(p_cache_mode);
}

void DSTextureCache::set_max_entries(int p_max) {
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		max_entries_user_overridden = true;
		max_entries = p_max < 1 ? 1 : p_max;
		while (static_cast<int>(cache_map.size()) > max_entries) {
			// Same single-call pattern as store(): pristine-count scan,
			// victim Ref retained, entry removed.
			if (!_evict_oldest(evicted)) {
				if (lru_list.is_empty()) {
					// Inconsistent state (entries without LRU nodes): never
					// mass-free live RIDs; keep over-capacity and warn.
					UtilityFunctions::push_warning("DSTextureCache: shrink to max_entries (", (int64_t)max_entries, ") deferred with no LRU order; keeping temporary over-capacity instead of freeing live RIDs.");
				} else {
					// Every entry is externally held: keep temporary
					// over-capacity instead of force-freeing a live RID.
					UtilityFunctions::push_warning("DSTextureCache: shrink to max_entries (", (int64_t)max_entries, ") deferred with all entries externally held; keeping temporary over-capacity instead of freeing live RIDs.");
				}
				break;
			}
		}

		std::vector<const Texture2D *> live_refs;
		live_refs.reserve(cache_map.size());
		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			if (kv.value.texture.is_valid()) {
				live_refs.push_back(kv.value.texture.ptr());
			}
		}
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}
	ds_texture_cache_release_evicted(evicted);
}

int DSTextureCache::get_max_entries() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return max_entries;
}

int DSTextureCache::get_cached_count() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return static_cast<int>(cache_map.size());
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
int64_t DSTextureCache::_get_prewarm_eviction_count_for_testing() const {
	std::lock_guard<std::mutex> lock(cache_mutex);
	return prewarm_evictions;
}

void DSTextureCache::_reset_prewarm_eviction_stats_for_testing() {
	std::lock_guard<std::mutex> lock(cache_mutex);
	prewarm_evictions = 0;
	warned_prewarm_eviction = false;
}
#endif

} // namespace godot
