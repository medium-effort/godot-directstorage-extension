@tool
extends SceneTree

func _init():
	print("[TestManifest] Starting GDExtension manifest validation...")

	var candidates = [
		"res://addons/directstorage_accelerator/directstorage_accelerator.gdextension",
		"project/addons/directstorage_accelerator/directstorage_accelerator.gdextension"
	]
	var manifest_path = ""
	for c in candidates:
		if FileAccess.file_exists(c):
			manifest_path = c
			break
			
	if manifest_path.is_empty():
		printerr("[TestManifest] FAILED: Manifest file not found at candidates: ", candidates)
		quit(1)
		return

	var file = FileAccess.open(manifest_path, FileAccess.READ)
	if not file:
		printerr("[TestManifest] FAILED: Could not open manifest at: ", manifest_path)
		quit(1)
		return

	var content = file.get_as_text()
	file.close()

	var lines = content.split("\n")
	var current_section = ""
	var sections = {}
	
	for line in lines:
		var trimmed = line.strip_edges()
		if trimmed.is_empty() or trimmed.begins_with("#") or trimmed.begins_with(";"):
			continue
		if trimmed.begins_with("[") and trimmed.ends_with("]"):
			current_section = trimmed.substr(1, trimmed.length() - 2)
			sections[current_section] = {}
		elif current_section != "" and "=" in trimmed:
			var parts = trimmed.split("=", true, 1)
			var key = parts[0].strip_edges()
			var val = parts[1].strip_edges()
			sections[current_section][key] = val

	print("[TestManifest] Parsed sections: ", sections.keys())

	for sec in ["configuration", "libraries", "dependencies"]:
		if not sections.has(sec):
			printerr("[TestManifest] FAILED: Missing section [", sec, "]")
			quit(1)
			return

	# Validate library declarations for the platform and target currently running.
	var lib_keys = sections["libraries"].keys()
	print("[TestManifest] Declared library targets: ", lib_keys)

	if lib_keys.is_empty():
		printerr("[TestManifest] FAILED: No library entries declared in [libraries]")
		quit(1)
		return

	var platform_name = {
		"Windows": "windows",
		"Linux": "linux",
		"macOS": "macos"
	}.get(OS.get_name(), "")
	if platform_name.is_empty():
		printerr("[TestManifest] FAILED: Unsupported test platform: ", OS.get_name())
		quit(1)
		return

	var target_name = "release" if OS.has_feature("template_release") else "debug"
	var target_prefix = platform_name + "." + target_name + "."
	var target_key = platform_name + "." + target_name
	var active_library_count = 0

	# Verify only artifacts for the active build target. Other target/platform
	# artifacts are produced by their corresponding release build job.
	for k in lib_keys:
		var key_str = String(k)
		if key_str != target_key and not key_str.begins_with(target_prefix):
			continue
		active_library_count += 1
		var rel_path = sections["libraries"][k].strip_edges().trim_prefix("\"").trim_suffix("\"")
		var full_path = "res://addons/directstorage_accelerator/" + rel_path
		if not FileAccess.file_exists(full_path):
			printerr("[TestManifest] FAILED: Declared library binary file does not exist on disk: ", full_path)
			quit(1)
			return

	if active_library_count == 0:
		printerr("[TestManifest] FAILED: No library entry declared for active target: ", target_prefix)
		quit(1)
		return

	# Validate dependencies keys (must be Windows only)
	var dep_keys = sections["dependencies"].keys()
	print("[TestManifest] Declared dependency targets: ", dep_keys)

	for k in dep_keys:
		var key_str = String(k)
		if not key_str.begins_with("windows."):
			printerr("[TestManifest] FAILED: Dependency target '", key_str, "' is not Windows-only!")
			quit(1)
			return

	for k in lib_keys:
		var key_str = String(k)
		if (key_str.begins_with("linux.") or key_str.begins_with("macos.")) and sections["dependencies"].has(k):
			printerr("[TestManifest] FAILED: Non-Windows library target has a dependency entry: ", key_str)
			quit(1)
			return

	print("[TestManifest] SUCCESS: Manifest structural validation passed!")
	quit(0)
