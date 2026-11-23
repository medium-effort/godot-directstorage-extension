extends RefCounted

## Shared cleanup and ProjectSettings helpers for GUT unit suites.


static func rm_dir_recursive(res_or_user_path: String) -> void:
	var abs_path := ProjectSettings.globalize_path(res_or_user_path)
	if not DirAccess.dir_exists_absolute(abs_path):
		return
	var da := DirAccess.open(abs_path)
	if da == null:
		return
	da.list_dir_begin()
	var entry := da.get_next()
	while not entry.is_empty():
		if entry != "." and entry != "..":
			var child := abs_path.path_join(entry)
			if da.current_is_dir():
				rm_dir_recursive(child)
			else:
				DirAccess.remove_absolute(child)
		entry = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs_path)


static func ensure_clean_dir(res_or_user_path: String) -> void:
	rm_dir_recursive(res_or_user_path)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(res_or_user_path))


static func restore_setting(key: String, had_before: bool, prev_value: Variant) -> void:
	if had_before:
		ProjectSettings.set_setting(key, prev_value)
	elif ProjectSettings.has_setting(key):
		ProjectSettings.clear(key)
