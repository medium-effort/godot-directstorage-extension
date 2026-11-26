@tool
extends EditorScript

## Run inside the Godot Editor via File > Run (Ctrl+Shift+X) to permanently
## delete res://demo_generated/ and rescan the filesystem.

func _run() -> void:
	var target_res := DemoAssetGenerator.GENERATED_ROOT.simplify_path()
	if not target_res.begins_with("res://") or target_res == "res://" or target_res == "res://.":
		push_error("[demo_tool] Refusing delete: unsafe target path '%s'." % target_res)
		return

	var abs_project := ProjectSettings.globalize_path("res://").simplify_path().replace("\\", "/").trim_suffix("/")
	var abs_root := ProjectSettings.globalize_path(target_res).simplify_path().replace("\\", "/").trim_suffix("/")
	if abs_root == abs_project or not abs_root.begins_with(abs_project + "/") or abs_root.get_file() != "demo_generated":
		push_error("[demo_tool] Refusing delete: '%s' is outside project root '%s' or unexpected." % [abs_root, abs_project])
		return

	if not DirAccess.dir_exists_absolute(abs_root):
		print("[demo_tool] Nothing to clear: %s does not exist." % DemoAssetGenerator.GENERATED_ROOT)
		return

	var err := _remove_dir_recursive(abs_root)
	if err != OK:
		push_error("[demo_tool] Failed deleting %s: %s" % [abs_root, error_string(err)])
		return

	var fs := EditorInterface.get_resource_filesystem()
	fs.scan()
	print("[demo_tool] Permanently deleted %s and triggered filesystem rescan." % DemoAssetGenerator.GENERATED_ROOT)


static func _remove_dir_recursive(abs_dir: String) -> Error:
	var da := DirAccess.open(abs_dir)
	if da == null:
		return DirAccess.get_open_error()
	da.list_dir_begin()
	var entry := da.get_next()
	while not entry.is_empty():
		if entry != "." and entry != "..":
			var child := abs_dir.path_join(entry)
			if da.current_is_dir():
				var sub_err := _remove_dir_recursive(child)
				if sub_err != OK:
					da.list_dir_end()
					return sub_err
			else:
				var file_err := DirAccess.remove_absolute(child)
				if file_err != OK:
					da.list_dir_end()
					return file_err
		entry = da.get_next()
	da.list_dir_end()
	return DirAccess.remove_absolute(abs_dir)
