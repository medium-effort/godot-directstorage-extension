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
	mutable std::mutex cache_mutex;
	int max_entries = 256;

	struct CacheEntry {
		Ref<Texture2D> texture;
		List<String>::Element *lru_element = nullptr;
	};

	HashMap<String, CacheEntry> cache_map;
	List<String> lru_list;

	void _evict_oldest();

protected:
	static void _bind_methods();

public:
	static DSTextureCache *get_singleton();

	DSTextureCache();
	~DSTextureCache();

	Ref<Texture2D> get_cached(const String &p_path);
	void store(const String &p_path, const Ref<Texture2D> &p_texture);
	void evict(const String &p_path);
	void clear();

	void set_max_entries(int p_max);
	int get_max_entries() const;
	int get_cached_count() const;
};

} // namespace godot

#endif // DS_TEXTURE_CACHE_H
