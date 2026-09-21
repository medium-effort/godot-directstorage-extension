#include "ds_texture_cache.h"
#include "ds_preloader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
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
// owner-loss (evict / clear / capacity replace / disable / shutdown) must
// explicitly free the RID or it leaks until device teardown.
//
// Release discipline:
// - The evicted Ref is held locally until its RID is freed inline or queued
//   for the render thread (Ref held until terminal). The RID value itself
//   survives Texture2DRD death because its destructor never frees it.
// - RenderingDevice::free_rid() asserts the render thread
//   (ERR_RENDER_THREAD_GUARD), so off-render-thread callers queue the RID and
//   post a drain worker via RenderingServer::call_on_render_thread().
//   Missing/dead singletons (headless Dummy, module teardown) skip the free:
//   no live device means no live GPU resource.
// - Idempotent: the map entry is removed under the cache lock before any
//   free, so a repeat evict/clear finds nothing and frees nothing; only
//   valid RIDs are ever freed, exactly once per evicted texture. A liveness
//   check skips stale handles from re-stored evicted textures, so re-entry
//   can never double-free ("Attempted to free invalid ID").
// - Everything below runs OUTSIDE the cache lock (free_rid takes
//   RenderingDevice locks; the deferred path crosses threads). No
//   drain-logic change: context/backend queues are untouched.
static std::mutex s_evict_free_mutex;
static std::vector<RID> s_evict_free_queue;

static void ds_texture_cache_drain_evict_frees_on_render_thread() {
	std::vector<RID> pending;
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		pending.swap(s_evict_free_queue);
	}
	if (pending.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		return; // Device gone (shutdown/teardown); RIDs die with it.
	}
	for (size_t i = 0; i < pending.size(); ++i) {
		// Liveness check: a re-stored evicted texture still carries its stale
		// handle, and the engine never frees it itself, so a second free of
		// the same value would spam "Attempted to free invalid ID".
		if (pending[i].is_valid() && rd->texture_is_valid(pending[i])) {
			rd->free_rid(pending[i]);
		}
	}
}

static void ds_texture_cache_free_texture_rids(const std::vector<RID> &p_rids) {
	if (p_rids.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (rd == nullptr) {
			return;
		}
		for (size_t i = 0; i < p_rids.size(); ++i) {
			if (p_rids[i].is_valid() && rd->texture_is_valid(p_rids[i])) {
				rd->free_rid(p_rids[i]);
			}
		}
		return;
	}
	{
		std::lock_guard<std::mutex> lock(s_evict_free_mutex);
		for (size_t i = 0; i < p_rids.size(); ++i) {
			if (p_rids[i].is_valid()) {
				s_evict_free_queue.push_back(p_rids[i]);
			}
		}
	}
	rs->call_on_render_thread(callable_mp_static(ds_texture_cache_drain_evict_frees_on_render_thread));
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

// Releases evicted textures: frees owned Texture2DRD RIDs on the render
// thread, then drops the Refs. MUST be called WITHOUT cache_mutex held.
static void ds_texture_cache_release_evicted(std::vector<Ref<Texture2D>> &p_evicted) {
	if (p_evicted.empty()) {
		return;
	}
	std::vector<RID> rids;
	rids.reserve(p_evicted.size());
	for (size_t i = 0; i < p_evicted.size(); ++i) {
		RID rid = ds_texture_cache_extract_rd_rid(p_evicted[i]);
		if (rid.is_valid()) {
			rids.push_back(rid);
		}
	}
	// Twin entries erased together reference one RID: free it exactly once.
	std::sort(rids.begin(), rids.end(), [](const RID &a, const RID &b) { return a.get_id() < b.get_id(); });
	rids.erase(std::unique(rids.begin(), rids.end(), [](const RID &a, const RID &b) { return a.get_id() == b.get_id(); }), rids.end());
	ds_texture_cache_free_texture_rids(rids);
	p_evicted.clear();
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
	ClassDB::bind_method(D_METHOD("store", "path", "texture", "cache_mode"), &DSTextureCache::store, DEFVAL(static_cast<int>(CACHE_MODE_REUSE)));
	ClassDB::bind_method(D_METHOD("evict", "path"), &DSTextureCache::evict);
	ClassDB::bind_method(D_METHOD("clear"), &DSTextureCache::clear);

	ClassDB::bind_method(D_METHOD("set_enabled", "enabled"), &DSTextureCache::set_enabled);
	ClassDB::bind_method(D_METHOD("is_enabled"), &DSTextureCache::is_enabled);
	ClassDB::bind_method(D_METHOD("is_cache_mode_ignored", "cache_mode"), &DSTextureCache::is_cache_mode_ignored);

	ClassDB::bind_method(D_METHOD("set_max_entries", "max"), &DSTextureCache::set_max_entries);
	ClassDB::bind_method(D_METHOD("get_max_entries"), &DSTextureCache::get_max_entries);
	ClassDB::bind_method(D_METHOD("get_cached_count"), &DSTextureCache::get_cached_count);

	ClassDB::bind_method(D_METHOD("prewarm_async", "paths"), &DSTextureCache::prewarm_async);
	ClassDB::bind_method(D_METHOD("poll_completed", "tag"), &DSTextureCache::poll_completed, DEFVAL((uint64_t)0));
	ClassDB::bind_method(D_METHOD("get_pending_prewarm_count", "tag"), &DSTextureCache::get_pending_prewarm_count, DEFVAL((uint64_t)0));

	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "enabled"), "set_enabled", "is_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_entries"), "set_max_entries", "get_max_entries");
}

DSTextureCache::DSTextureCache() {
	{
		std::lock_guard<std::mutex> lock(singleton_mutex);
		singleton = this;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && ps->has_setting("directstorage/texture_cache_enabled")) {
		enabled = ps->get_setting("directstorage/texture_cache_enabled");
	}
	if (ps && ps->has_setting("directstorage/texture_cache_max_entries")) {
		max_entries = ps->get_setting("directstorage/texture_cache_max_entries");
		if (max_entries < 1) {
			max_entries = 256;
		}
	}
}

DSTextureCache::~DSTextureCache() {
	clear();
	// Release preloader-held request Refs + archive handle while Godot is
	// still alive so no Ref outlives module teardown.
	DSPreloader::shutdown();
	{
		std::lock_guard<std::mutex> lock(singleton_mutex);
		if (singleton == this) {
			singleton = nullptr;
		}
	}
}

bool DSTextureCache::_evict_oldest() {
	if (lru_list.is_empty()) {
		return false;
	}

	List<String>::Element *oldest = lru_list.back();
	const String oldest_path = oldest->get();
	lru_list.erase(oldest);
	cache_map.erase(oldest_path);
	return true;
}

void DSTextureCache::_remove_entry(const String &p_path) {
	if (!cache_map.has(p_path)) {
		return;
	}

	CacheEntry entry = cache_map[p_path];
	if (entry.lru_element) {
		lru_list.erase(entry.lru_element);
	}
	cache_map.erase(p_path);
}

bool DSTextureCache::_is_cache_mode_ignored(int p_cache_mode) {
	return p_cache_mode == CACHE_MODE_IGNORE || p_cache_mode == CACHE_MODE_IGNORE_DEEP;
}

bool DSTextureCache::_is_cache_mode_replace(int p_cache_mode) {
	return p_cache_mode == CACHE_MODE_REPLACE;
}

Ref<Texture2D> DSTextureCache::get_cached(const String &p_path, int p_cache_mode) {
	std::vector<Ref<Texture2D>> evicted;
	Ref<Texture2D> result;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (!enabled || _is_cache_mode_ignored(p_cache_mode) || _is_cache_mode_replace(p_cache_mode) || !cache_map.has(p_path)) {
			return Ref<Texture2D>();
		}

		CacheEntry &entry = cache_map[p_path];
		if (entry.texture.is_null() || !entry.texture.is_valid()) {
			evicted.push_back(entry.texture);
			_remove_entry(p_path);
		} else {
			if (entry.lru_element && entry.lru_element != lru_list.front()) {
				lru_list.move_to_front(entry.lru_element);
			}

			result = entry.texture;
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
	return result;
}

void DSTextureCache::store(const String &p_path, const Ref<Texture2D> &p_texture, int p_cache_mode) {
	Ref<Texture2D> resident;
	bool make_resident = false;
	// Refs of replaced/evicted textures: held until their owned RIDs are
	// released below (Ref held until terminal), outside cache_mutex.
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (!enabled || _is_cache_mode_ignored(p_cache_mode) || p_texture.is_null() || !p_texture.is_valid()) {
			return;
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
			if (entry.lru_element && entry.lru_element != lru_list.front()) {
				lru_list.move_to_front(entry.lru_element);
			}
		} else {
			while (static_cast<int>(cache_map.size()) >= max_entries) {
				if (lru_list.is_empty()) {
					for (const KeyValue<String, CacheEntry> &kv : cache_map) {
						evicted.push_back(kv.value.texture);
					}
					cache_map.clear();
					break;
				}
				const String oldest_path = lru_list.back()->get();
				if (cache_map.has(oldest_path)) {
					evicted.push_back(cache_map[oldest_path].texture);
				}
				if (!_evict_oldest()) {
					for (const KeyValue<String, CacheEntry> &kv : cache_map) {
						evicted.push_back(kv.value.texture);
					}
					cache_map.clear();
					break;
				}
			}

			lru_list.push_front(p_path);
			CacheEntry new_entry;
			new_entry.texture = p_texture;
			new_entry.lru_element = lru_list.front();
			cache_map[p_path] = new_entry;
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
	if (make_resident && resident.is_valid()) {
		resident->take_over_path(p_path);
	}
	ds_texture_cache_release_evicted(evicted);
}

uint64_t DSTextureCache::prewarm_async(const PackedStringArray &p_paths) {
	return DSPreloader::prewarm_async(p_paths);
}

int DSTextureCache::poll_completed(uint64_t p_tag) {
	return DSPreloader::poll_completed(p_tag);
}

int DSTextureCache::get_pending_prewarm_count(uint64_t p_tag) const {
	return DSPreloader::get_pending_prewarm_count(p_tag);
}

void DSTextureCache::evict(const String &p_path) {
	// Idempotent: repeat evict finds no entry and frees nothing.
	std::vector<Ref<Texture2D>> evicted;
	{
		std::lock_guard<std::mutex> lock(cache_mutex);

		if (!cache_map.has(p_path)) {
			return;
		}

		evicted.push_back(cache_map[p_path].texture);
		_remove_entry(p_path);

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

		for (const KeyValue<String, CacheEntry> &kv : cache_map) {
			evicted.push_back(kv.value.texture);
		}
		cache_map.clear();
		lru_list.clear();
		// Map is empty: no twin can remain; filter is a no-op kept for
		// symmetry with the other owner-loss paths.
		std::vector<const Texture2D *> live_refs;
		ds_texture_cache_drop_live_refs(evicted, live_refs);
	}
	ds_texture_cache_release_evicted(evicted);
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
			for (const KeyValue<String, CacheEntry> &kv : cache_map) {
				evicted.push_back(kv.value.texture);
			}
			cache_map.clear();
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

		max_entries = p_max < 1 ? 1 : p_max;
		while (static_cast<int>(cache_map.size()) > max_entries) {
			if (lru_list.is_empty()) {
				for (const KeyValue<String, CacheEntry> &kv : cache_map) {
					evicted.push_back(kv.value.texture);
				}
				cache_map.clear();
				break;
			}
			const String oldest_path = lru_list.back()->get();
			if (cache_map.has(oldest_path)) {
				evicted.push_back(cache_map[oldest_path].texture);
			}
			if (!_evict_oldest()) {
				for (const KeyValue<String, CacheEntry> &kv : cache_map) {
					evicted.push_back(kv.value.texture);
				}
				cache_map.clear();
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

} // namespace godot
