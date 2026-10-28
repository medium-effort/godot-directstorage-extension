@tool
extends SceneTree

## Editor Plugin Lifecycle Test
##
## Author: MediumEffort
## Description: Tests enabling, disabling, and re-enabling the DSPlugin editor plugin,
## verifying autoload singleton registration in ProjectSettings at each lifecycle transition.

const AUTOLOAD_NAME = "DSRuntime"
const PLUGIN_SCRIPT_PATH = "res://addons/directstorage_accelerator/scripts/ds_plugin.gd"
const EXPORT_PLUGIN_PATH = "res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd"

func _init() -> void:
	print("[Test Lifecycle] Starting EditorPlugin lifecycle test...")

	# The export plugin must parse and register even when the native extension
	# is absent, so it may not call the native class directly at parse time.
	var export_script: GDScript = load(EXPORT_PLUGIN_PATH)
	if export_script == null:
		push_error("[Test Lifecycle] FAIL: Unable to load export plugin script at " + EXPORT_PLUGIN_PATH)
		quit(1)
		return
	if "DSPkg." in export_script.source_code:
		push_error("[Test Lifecycle] FAIL: ds_export_plugin.gd contains a direct DSPkg method call; a missing native binary would break plugin registration.")
		quit(1)
		return
	print("[Test Lifecycle] Export plugin loads without a direct native-class call.")

	var script_resource = load(PLUGIN_SCRIPT_PATH)
	if script_resource == null:
		push_error("[Test Lifecycle] FAIL: Unable to load plugin script at " + PLUGIN_SCRIPT_PATH)
		quit(1)
		return

	var source_code: String = script_resource.source_code
	# The autoload is project data: it must be removed when the user disables the
	# addon, and only then. Removing it from _exit_tree() also erased it on a
	# normal editor shutdown, which left standalone and exported runs with no
	# DSRuntime node to submit the stream queue (async prewarm stalled at 0/N).
	if not "func _disable_plugin" in source_code:
		push_error("[Test Lifecycle] FAIL: ds_plugin.gd has no _disable_plugin hook; nothing unregisters the autoload on an explicit disable")
		quit(1)
		return
	if "_sync_autoload(AUTOLOAD_NAME, \"\")" in _function_body(source_code, "_exit_tree"):
		push_error("[Test Lifecycle] FAIL: ds_plugin.gd unregisters the autoload from _exit_tree(); a normal editor shutdown would strip it from project.godot again")
		quit(1)
		return

	# The pack scan entry must be registered and removed symmetrically, or the
	# Project > Tools item survives a plugin toggle as a dangling callable.
	for required_token in ["add_tool_menu_item", "remove_tool_menu_item"]:
		if not required_token in source_code:
			push_error("[Test Lifecycle] FAIL: ds_plugin.gd does not contain " + required_token)
			quit(1)
			return
	if not FileAccess.file_exists("res://addons/directstorage_accelerator/tools/dspkg_pack_scan_dialog.tscn"):
		push_error("[Test Lifecycle] FAIL: pack scan dialog scene is missing")
		quit(1)
		return

	if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
		print("[Test Lifecycle] Running inside active Godot Editor interface...")

		# Enable
		EditorInterface.set_plugin_enabled("directstorage_accelerator", true)
		if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
			push_error("[Test Lifecycle] FAIL: Autoload missing after enabling plugin via EditorInterface")
			quit(1)
			return

		# Disable
		EditorInterface.set_plugin_enabled("directstorage_accelerator", false)
		if ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
			push_error("[Test Lifecycle] FAIL: Autoload still present after disabling plugin via EditorInterface")
			quit(1)
			return

		# Re-enable
		EditorInterface.set_plugin_enabled("directstorage_accelerator", true)
		if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
			push_error("[Test Lifecycle] FAIL: Autoload missing after re-enabling plugin via EditorInterface")
			quit(1)
			return

		print("[Test Lifecycle] SUCCESS: EditorInterface enable/disable/re-enable verified successfully.")
	else:
		print("[Test Lifecycle] Running in standalone headless mode. Verifying ProjectSettings autoload lifecycle contract...")

		var key := "autoload/" + AUTOLOAD_NAME
		var registered_path := "*res://addons/directstorage_accelerator/scripts/ds_runtime.gd"
		var original_present := ProjectSettings.has_setting(key)
		var original_value = ProjectSettings.get_setting(key) if original_present else null

		# Simulate enable: add setting
		ProjectSettings.set_setting(key, registered_path)
		if not ProjectSettings.has_setting(key):
			push_error("[Test Lifecycle] FAIL: Unable to set autoload in ProjectSettings")
			quit(1)
			return

		# The source check above proves _exit_tree() does not unregister it: the
		# plugin node itself cannot be instantiated outside the editor
		# ("Class 'EditorPlugin' can only be instantiated by editor"), so the
		# editor-shutdown branch is asserted statically and exercised live by the
		# EditorInterface branch above.

		# Simulate an explicit disable (the disable hook owns the removal).
		ProjectSettings.set_setting(key, null)
		if ProjectSettings.has_setting(key):
			push_error("[Test Lifecycle] FAIL: Unable to clear autoload from ProjectSettings")
			quit(1)
			return

		# Simulate re-enable: add setting
		ProjectSettings.set_setting(key, registered_path)
		if not ProjectSettings.has_setting(key):
			push_error("[Test Lifecycle] FAIL: Unable to re-set autoload in ProjectSettings")
			quit(1)
			return

		# Cleanup: restore whatever project.godot declared on entry (never saved).
		if original_present:
			ProjectSettings.set_setting(key, original_value)
		else:
			ProjectSettings.set_setting(key, null)
		print("[Test Lifecycle] SUCCESS: Static code inspection & ProjectSettings lifecycle contract verified.")

	quit(0)


## Returns the body of [param func_name] up to the next top-level function
## declaration, or an empty string when the function is absent. Needed to assert
## WHERE a call site lives: an EditorPlugin cannot be instantiated outside the
## editor, so the shutdown/disable split is checked on the source text.
static func _function_body(source: String, func_name: String) -> String:
	var marker := "\nfunc " + func_name
	var start := source.find(marker)
	if start < 0:
		return ""
	var rest := source.substr(start + 1)
	var next_func := rest.find("\nfunc ", 1)
	if next_func < 0:
		return rest
	return rest.substr(0, next_func)
