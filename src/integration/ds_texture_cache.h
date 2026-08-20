#ifndef DS_TEXTURE_CACHE_H
#define DS_TEXTURE_CACHE_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/list.hpp>
#include <godot_cpp/variant/string.hpp>

#include <mutex>

namespace godot {

class DSTextureCache : public Object {
	GDCLASS(DSTextureCache, Object);

private:
	static DSTextureCache *singleton;
	static std::mutex singleton_mutex;
	mutable std::mutex cache_mutex;
	bool enabled = false;
	int max_entries = 256;

	struct CacheEntry {
		Ref<Texture2D> texture;
		List<String>::Element *lru_element = nullptr;
	};

	HashMap<String, CacheEntry> cache_map;
	List<String> lru_list;

	bool _evict_oldest();
	void _remove_entry(const String &p_path);
	static bool _is_cache_mode_ignored(int p_cache_mode);
	static bool _is_cache_mode_replace(int p_cache_mode);

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
	void store(const String &p_path, const Ref<Texture2D> &p_texture, int p_cache_mode = CACHE_MODE_REUSE);
	void evict(const String &p_path);
	void clear();

	void set_enabled(bool p_enabled);
	bool is_enabled() const;
	bool is_cache_mode_ignored(int p_cache_mode) const;

	void set_max_entries(int p_max);
	int get_max_entries() const;
	int get_cached_count() const;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::DSTextureCache::CacheMode);

#endif // DS_TEXTURE_CACHE_H
