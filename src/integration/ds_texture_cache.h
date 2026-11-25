#ifndef DS_TEXTURE_CACHE_H
#define DS_TEXTURE_CACHE_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mutex>

#include <cstdint>
#include <vector>

namespace godot {

class DSTextureCache : public Object {
	GDCLASS(DSTextureCache, Object);

private:
	static DSTextureCache *singleton;
	static std::mutex singleton_mutex;
	mutable std::mutex cache_mutex;
	bool enabled = false;
	int max_entries = 512;
	bool max_entries_user_overridden = false;
	// Evicted prewarm-origin entries (guarded by cache_mutex). One-shot warning
	// makes the otherwise silent "prewarm self-eviction" failure diagnosable.
	int prewarm_evictions = 0;
	bool warned_prewarm_eviction = false;
	// One-shot gate for the capacity warning emitted by store(). Kept separate
	// from warned_prewarm_eviction: consuming it there would silence the real
	// "prewarm entry evicted before its batch finished" diagnostic later.
	bool warned_prewarm_over_capacity = false;

	struct CacheEntry {
		Ref<Texture2D> texture;
		List<String>::Element *lru_element = nullptr;
		Vector<String> aliases;
		bool prewarm_origin = false;
	};

	HashMap<String, CacheEntry> cache_map;
	HashMap<String, String> alias_map;
	List<String> lru_list;
	HashSet<String> in_flight_takeovers;

	bool _is_entry_evictable_locked(const String &p_path, const Ref<Texture2D> &p_texture) const;
	bool _evict_oldest(std::vector<Ref<Texture2D>> &r_evicted, bool p_skip_prewarm = false);
	void _remove_entry(const String &p_canonical_path);
	void _bind_alias_locked(const String &p_canonical_path, const String &p_alias_path);
	void _unbind_alias_locked(const String &p_alias_path);
	String _resolve_canonical_locked(const String &p_path) const;
	static bool _is_cache_mode_ignored(int p_cache_mode);
	static bool _is_cache_mode_replace(int p_cache_mode);
	// Protected prewarm entries may overshoot max_entries by a bounded amount
	// so a batch cannot evict its own waiting textures, but the overshoot must
	// still cap resident textures. Shared by store() and the test seam.
	static int prewarm_hard_cap(int p_max_entries);
	bool _can_own_locked(int p_cache_mode) const;

protected:
	static void _bind_methods();

public:
	enum CacheMode {
		CACHE_MODE_IGNORE = 0,
		CACHE_MODE_REUSE = 1,
		CACHE_MODE_REPLACE = 2,
		CACHE_MODE_IGNORE_DEEP = 3,
	};

	static DSTextureCache *get_singleton();

	DSTextureCache();
	~DSTextureCache();

	Ref<Texture2D> get_cached(const String &p_path, int p_cache_mode = CACHE_MODE_REUSE);
	// Returns true when the cache took ownership of p_texture. False means the
	// call was a no-op (disabled cache, ignored mode, null/invalid texture or
	// empty path). On false, a caller that created a Texture2DRD still owns the
	// RenderingDevice texture RID and must free it; an ImageTexture owns its
	// RenderingServer RID and frees it when the Ref drops, so it needs no
	// cleanup. Returning the decision instead of only logging it is what makes
	// GPU ownership transfer race-free: the caller never assumes a store
	// succeeded (#0126).
	// p_prewarm marks entries stored by a prewarm batch; evicting one before
	// its batch finishes is counted/warned once (C++-only, not a new binding).
	bool store(const String &p_path, const Ref<Texture2D> &p_texture, int p_cache_mode = CACHE_MODE_REUSE, const String &p_alias_path = String(), bool p_prewarm = false);
	void add_alias(const String &p_canonical_path, const String &p_alias_path);
	void remove_alias(const String &p_alias_path);
	bool has_alias(const String &p_alias_path) const;
	String get_canonical_path(const String &p_path) const;
	void evict(const String &p_path);
	void clear();
	int clear_unused();

	// Async prewarm entry points. DSPreloader does the work; these thin
	// delegates are the GDScript-visible surface (this class is already
	// registered, DSPreloader is not).
	// prewarm_async() accepts engine texture paths (.png/.jpg/.webp),
	// resolves .import/package remaps internally, skips paths already held
	// in the caches, enqueues the DSContext stream queue and returns the
	// stream tag for DSContext::is_prewarm_complete/get_prewarm_progress/
	// cancel_prewarm. prewarm_gpu_async() is the Phase-2 GPU variant (same
	// contract; GPU-eligible payloads allocate on the render thread and
	// complete as Texture2DRD, the rest take the memory path under the
	// same tag). pump_gpu_prewarm() posts at most one pending GPU chunk
	// per call (batch manager ticks it every frame). poll_completed()
	// builds textures for finished requests and stores them; returns
	// textures stored by this call.
	uint64_t prewarm_async(const PackedStringArray &p_paths);
	uint64_t prewarm_cpu_paced_async(const PackedStringArray &p_paths);
	void pump_cpu_prewarm(int p_max_items, int64_t p_max_bytes);
	int get_queued_cpu_prewarm_count(uint64_t p_tag = 0) const;
	int get_in_flight_cpu_prewarm_count(uint64_t p_tag = 0) const;
	int64_t get_in_flight_cpu_prewarm_bytes(uint64_t p_tag = 0) const;
	uint64_t prewarm_gpu_async(const PackedStringArray &p_paths, int p_chunk_size = 16, bool p_prefer_gpu = true, int64_t p_max_chunk_bytes = 0);
	void pump_gpu_prewarm();
	bool is_gpu_prewarm_available();
	int poll_completed(uint64_t p_tag = 0, int p_max_items = 0, int p_max_usec = 4000, bool p_gpu_only = false);
	int get_pending_prewarm_count(uint64_t p_tag = 0) const;
	int get_unbuilt_prewarm_count(uint64_t p_tag = 0) const;
	int get_unbuilt_cpu_prewarm_count(uint64_t p_tag = 0) const;
	int get_unbuilt_gpu_prewarm_count(uint64_t p_tag = 0) const;
	bool has_gpu_requests_in_flight(uint64_t p_tag = 0) const;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// C++-only test seams; deliberately not bound (no new GDScript surface).
	int64_t _get_prewarm_eviction_count_for_testing() const;
	int _get_prewarm_hard_cap_for_testing() const;
	void _reset_prewarm_eviction_stats_for_testing();
#endif
	void set_background_pacing(bool p_enabled);
	bool is_background_pacing() const;

	void set_enabled(bool p_enabled);
	bool is_enabled() const;
	bool is_cache_mode_ignored(int p_cache_mode) const;

	// True when store() would take ownership of a valid texture under this
	// cache mode (cache enabled and the mode is not an ignored one). The
	// preloader uses it to decide whether a GPU prewarm result has a home at
	// all. The loader no longer gates on it: a texture the cache refuses gets a
	// DSTextureOwner instead, so uncached loads keep the GPU path (#0126).
	bool can_own(int p_cache_mode) const;

	// Releases a device texture RID this extension owns, from any thread:
	// inline on the render thread, otherwise queued + drained there, and
	// deferred while a DirectStorage request still targets it. Static so
	// DSTextureOwner destructors work after the cache singleton is gone.
	static void release_owned_rid(const RID &p_rid);

	// Frees every deferred RID WITHOUT the in-flight test. Teardown only.
	// p_ds_drain_clean must be DSContext::was_last_shutdown_clean(): when the
	// backend could not drain in-flight DMA the queue is left untouched on
	// purpose, because freeing a RID under live DMA is a use-after-free; those
	// RIDs die with the device.
	static void flush_deferred_rid_releases(bool p_ds_drain_clean);

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test seam: when armed, every texture that reaches the eviction release is
	// copied into p_out first - a holder created after the evictability sample
	// but before the RID decision, which is exactly the window eviction must
	// survive. C++ only (not bound). The caller owns the pointee and MUST clear
	// the seam (pass nullptr) before that storage dies.
	static void _set_evict_post_sample_probe_for_testing(std::vector<Ref<Texture2D>> *p_out);
#endif

	void set_max_entries(int p_max);
	int get_max_entries() const;
	// Reduction-only VRAM guard: called after RenderingServer is up (e.g. from
	// DSContext::initialize_ds()) because DSTextureCache is constructed at
	// MODULE_INITIALIZATION_LEVEL_SCENE before RenderingServer exists.
	void apply_vram_budget_guard(const Dictionary &p_adapter = Dictionary());
	int get_cached_count() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DSTextureCache::CacheMode);

#endif // DS_TEXTURE_CACHE_H
