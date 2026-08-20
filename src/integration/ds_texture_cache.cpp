#include "ds_texture_cache.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

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
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (!enabled || _is_cache_mode_ignored(p_cache_mode) || _is_cache_mode_replace(p_cache_mode) || !cache_map.has(p_path)) {
		return Ref<Texture2D>();
	}

	CacheEntry &entry = cache_map[p_path];
	if (entry.texture.is_null() || !entry.texture.is_valid()) {
		_remove_entry(p_path);
		return Ref<Texture2D>();
	}

	if (entry.lru_element && entry.lru_element != lru_list.front()) {
		lru_list.move_to_front(entry.lru_element);
	}

	return entry.texture;
}

void DSTextureCache::store(const String &p_path, const Ref<Texture2D> &p_texture, int p_cache_mode) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (!enabled || _is_cache_mode_ignored(p_cache_mode) || p_texture.is_null() || !p_texture.is_valid()) {
		return;
	}

	if (cache_map.has(p_path)) {
		CacheEntry &entry = cache_map[p_path];
		entry.texture = p_texture;
		if (entry.lru_element && entry.lru_element != lru_list.front()) {
			lru_list.move_to_front(entry.lru_element);
		}
		return;
	}

	while (static_cast<int>(cache_map.size()) >= max_entries) {
		if (!_evict_oldest()) {
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

void DSTextureCache::evict(const String &p_path) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (!cache_map.has(p_path)) {
		return;
	}

	_remove_entry(p_path);
}

void DSTextureCache::clear() {
	std::lock_guard<std::mutex> lock(cache_mutex);

	cache_map.clear();
	lru_list.clear();
}

void DSTextureCache::set_enabled(bool p_enabled) {
	std::lock_guard<std::mutex> lock(cache_mutex);

	if (enabled == p_enabled) {
		return;
	}

	enabled = p_enabled;
	if (!enabled) {
		cache_map.clear();
		lru_list.clear();
	}
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
	std::lock_guard<std::mutex> lock(cache_mutex);

	max_entries = p_max < 1 ? 1 : p_max;
	while (static_cast<int>(cache_map.size()) > max_entries) {
		if (!_evict_oldest()) {
			cache_map.clear();
			break;
		}
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
