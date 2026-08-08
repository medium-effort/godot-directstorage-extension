#include "ds_texture_cache.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

DSTextureCache *DSTextureCache::singleton = nullptr;

DSTextureCache *DSTextureCache::get_singleton() {
	return singleton;
}

void DSTextureCache::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_cached", "path"), &DSTextureCache::get_cached);
	ClassDB::bind_method(D_METHOD("store", "path", "texture"), &DSTextureCache::store);
	ClassDB::bind_method(D_METHOD("evict", "path"), &DSTextureCache::evict);
	ClassDB::bind_method(D_METHOD("clear"), &DSTextureCache::clear);

	ClassDB::bind_method(D_METHOD("set_max_entries", "max"), &DSTextureCache::set_max_entries);
	ClassDB::bind_method(D_METHOD("get_max_entries"), &DSTextureCache::get_max_entries);
	ClassDB::bind_method(D_METHOD("get_cached_count"), &DSTextureCache::get_cached_count);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_entries"), "set_max_entries", "get_max_entries");
}

DSTextureCache::DSTextureCache() {
	singleton = this;

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && ps->has_setting("directstorage/texture_cache_max_entries")) {
		max_entries = ps->get_setting("directstorage/texture_cache_max_entries");
		if (max_entries < 1) {
			max_entries = 256;
		}
	}
}

DSTextureCache::~DSTextureCache() {
	clear();
	if (singleton == this) {
		singleton = nullptr;
	}
}

void DSTextureCache::_evict_oldest() {
	if (lru_list.is_empty()) {
		return;
	}

	String oldest_path = lru_list.back()->get();
	lru_list.pop_back();
	cache_map.erase(oldest_path);
}

Ref<Texture2D> DSTextureCache::get_cached(const String &p_path) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (!cache_map.has(p_path)) {
		return Ref<Texture2D>();
	}

	CacheEntry &entry = cache_map[p_path];
	if (entry.lru_element && entry.lru_element != lru_list.front()) {
		lru_list.move_to_front(entry.lru_element);
	}

	return entry.texture;
}

void DSTextureCache::store(const String &p_path, const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return;
	}

	std::lock_guard<std::mutex> lock(cache_mutex);

	if (cache_map.has(p_path)) {
		CacheEntry &entry = cache_map[p_path];
		entry.texture = p_texture;
		if (entry.lru_element && entry.lru_element != lru_list.front()) {
			lru_list.move_to_front(entry.lru_element);
		}
		return;
	}

	while (static_cast<int>(cache_map.size()) >= max_entries) {
		_evict_oldest();
	}

	lru_list.push_front(p_path);
	CacheEntry new_entry;
	new_entry.texture = p_texture;
	new_entry.lru_element = lru_list.front();
	cache_map[p_path] = new_entry;
}

void DSTextureCache::evict(const String &p_path) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (!cache_map.has(p_path)) {
		return;
	}

	CacheEntry entry = cache_map[p_path];
	if (entry.lru_element) {
		lru_list.erase(entry.lru_element);
	}
	cache_map.erase(p_path);
}

void DSTextureCache::clear() {
	std::lock_guard<std::mutex> lock(cache_mutex);

	cache_map.clear();
	lru_list.clear();
}

void DSTextureCache::set_max_entries(int p_max) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	max_entries = p_max < 1 ? 1 : p_max;
	while (static_cast<int>(cache_map.size()) > max_entries) {
		_evict_oldest();
	}
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
