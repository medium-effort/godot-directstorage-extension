/**************************************************************************/
/*  ds_settings.h                                                         */
/**************************************************************************/
#pragma once

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {
namespace dsgd_settings {

// Every extension setting sits one level under a single namespace:
// "dsgd_extension/<name>".
//
// A namespace whose keys all stop at two segments renders as ONE selectable
// node whose page lists the settings (editor/inspector/
// editor_sectioned_inspector.cpp: _section_selected() sets
// allow_sub = node has no children, and the filter strips the section prefix).
// That is the normal third-party-addon layout. Mixing in a deeper key
// ("dsgd_extension/group/name") would add a child node and split the page, so
// everything here stays two segments, including the internal keys.
inline String key(const char *p_suffix) {
	return String("dsgd_extension/") + String(p_suffix);
}

inline String legacy_key(const char *p_legacy_suffix) {
	return String("directstorage/") + String(p_legacy_suffix);
}

struct SettingMapping {
	const char *key;
	const char *legacy;
};

// The legacy suffix differs from the new one wherever the old layout nested a
// group ("import/default_profile", "export/bundle_dspkg").
inline constexpr SettingMapping MIGRATED_SETTINGS[] = {
	{ "staging_buffer_size_mb", "staging_buffer_size_mb" },
	{ "disable_gpu_decompression", "disable_gpu_decompression" },
	{ "queue_capacity", "queue_capacity" },
	{ "auto_tune_for_integrated_gpu", "auto_tune_for_integrated_gpu" },
	{ "texture_cache_enabled", "texture_cache_enabled" },
	{ "texture_cache_max_entries", "texture_cache_max_entries" },
	{ "shutdown_drain_timeout_ms", "shutdown_drain_timeout_ms" },
	{ "sync_flush_timeout_ms", "sync_flush_timeout_ms" },
	{ "default_import_profile", "import/default_profile" },
	{ "force_archive_resolution", "force_archive_resolution" },
	{ "archive_name", "archive_name" },
	{ "bundle_dspkg", "export/bundle_dspkg" },
	{ "archive_id", "export/archive_id" },
	{ "protected", "export/protected" },
};

inline bool has_setting(const char *p_suffix, const char *p_legacy_suffix) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return false;
	}
	return ps->has_setting(key(p_suffix)) || ps->has_setting(legacy_key(p_legacy_suffix));
}

inline Variant get_setting(const char *p_suffix, const char *p_legacy_suffix, const Variant &p_default = Variant()) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return p_default;
	}
	const String new_name = key(p_suffix);
	if (ps->has_setting(new_name)) {
		return ps->get_setting(new_name, p_default);
	}
	const String old_name = legacy_key(p_legacy_suffix);
	if (ps->has_setting(old_name)) {
		return ps->get_setting(old_name, p_default);
	}
	return p_default;
}

// Copies a legacy value onto its new key, then removes the legacy key.
//
// clear() is deliberate. A leftover two-segment "directstorage/..." entry is
// re-added to ProjectSettings::props on every load (project_settings.cpp:342),
// which would add a second selectable category next to "Dsgd Extension", and
// it would be written back to project.godot forever. This must run BEFORE the
// defaults are registered, otherwise the default value already occupies the
// new key and masks the user's migrated value.
inline void migrate_setting(const SettingMapping &p_mapping) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return;
	}
	const String new_name = key(p_mapping.key);
	const String old_name = legacy_key(p_mapping.legacy);
	if (!ps->has_setting(new_name) && ps->has_setting(old_name)) {
		ps->set_setting(new_name, ps->get_setting(old_name));
	}
	if (ps->has_setting(old_name)) {
		ps->clear(old_name);
	}
}

inline void migrate_all() {
	for (const SettingMapping &mapping : MIGRATED_SETTINGS) {
		migrate_setting(mapping);
	}
}

} // namespace dsgd_settings
} // namespace godot
