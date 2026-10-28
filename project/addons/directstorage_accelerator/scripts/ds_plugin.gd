@tool
extends EditorPlugin

## DirectStorage Accelerator Editor Plugin
##
## Author: MediumEffort
## Description: Registers the DSBatchManager autoload, DSTex importer, and
## DSPkg export plugin in the Godot Editor. Enable this plugin to get the
## automatic texture import and archive bundling workflow.

const AUTOLOAD_NAME: String = "DSRuntime"
const AUTOLOAD_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_runtime.gd"
const LEGACY_AUTOLOAD_NAME: String = "DSBatchManager"
const VERIFY_MENU_ITEM: String = "Verify exported pack (DSPkg)…"
const VERIFY_DIALOG_SCENE: String = "uid://bdepjlb375cg5"
const VERIFY_DIALOG_SCENE_PATH: String = "res://addons/directstorage_accelerator/tools/dspkg_pack_scan_dialog.tscn"

var _importer: EditorImportPlugin = null
var _export_plugin: EditorExportPlugin = null
var _checked_scenes: Dictionary = {}
var _verify_dialog: Window = null


## Registers the autoload, import plugin, export plugin, and DirectStorage
## runtime when the addon is enabled.
##
## The autoload is installed here rather than in `_enable_plugin()`:
## `EditorNode::init_plugins()` enables addons with `p_config_changed = false`,
## so the engine only calls `_enable_plugin()` on a runtime toggle, never when
## the editor starts with the addon already enabled. `_enter_tree()` runs on
## both paths. Unregistration lives in `_disable_plugin()`, the one callback
## the engine fires for a user-initiated disable alone (see `_exit_tree()`).
func _enter_tree() -> void:
	# Purge legacy autoload if present from older addon versions
	_sync_autoload(LEGACY_AUTOLOAD_NAME, "")
	_sync_autoload(AUTOLOAD_NAME, AUTOLOAD_PATH)

	if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		push_error(
			"[DirectStorage] Failed to register the %s autoload in project.godot. Async texture prewarm needs that node to drive the stream queue and will stay unavailable until 'autoload/%s' exists in Project Settings." % [AUTOLOAD_NAME, AUTOLOAD_NAME]
		)

	print(
		"[DirectStorage] Plugin enabled by MediumEffort. Autoload, Importer, and Exporter registered."
	)

	for internal_setting in [
		"directstorage/export/bundle_dspkg",
		"directstorage/export/archive_id",
		"directstorage/export/protected",
		"directstorage/archive_name",
	]:
		if ProjectSettings.has_setting(internal_setting):
			ProjectSettings.set_as_internal(internal_setting, true)

	if _importer == null:
		var importer_script = load("uid://kqg57i3wmiw")
		if importer_script:
			_importer = importer_script.new()
			add_import_plugin(_importer)

	if _export_plugin == null:
		var exporter_script = load("uid://37jisblsb2jo")
		if exporter_script:
			_export_plugin = exporter_script.new()
			add_export_plugin(_export_plugin)

	if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
		add_tool_menu_item(VERIFY_MENU_ITEM, _on_verify_exported_pack)

	if not scene_changed.is_connected(_on_scene_changed):
		scene_changed.connect(_on_scene_changed)
	if not scene_closed.is_connected(_on_scene_closed):
		scene_closed.connect(_on_scene_closed)

	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()


## Unregisters the import plugin, export plugin, and tool menu item when the
## editor stops this plugin: a normal editor shutdown and a user-initiated
## disable both land here.
##
## The autoload is deliberately NOT touched. It is project data, not editor
## session state: erasing it on shutdown also erased it from project.godot, so
## the next standalone or exported run had no `DSRuntime` node - nothing
## submitted or harvested the async stream queue, and prewarm stalled at
## 0/<total> with no diagnostic because the backend still initialised lazily.
## Only `_disable_plugin()`, which the engine calls for a user-initiated
## disable alone, unregisters the autoload.
func _exit_tree() -> void:
	if _importer != null:
		remove_import_plugin(_importer)
		_importer = null

	if _export_plugin != null:
		remove_export_plugin(_export_plugin)
		_export_plugin = null

	if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
		remove_tool_menu_item(VERIFY_MENU_ITEM)

	if is_instance_valid(_verify_dialog):
		_verify_dialog.queue_free()
		_verify_dialog = null

	if scene_changed.is_connected(_on_scene_changed):
		scene_changed.disconnect(_on_scene_changed)
	if scene_closed.is_connected(_on_scene_closed):
		scene_closed.disconnect(_on_scene_closed)
	_checked_scenes.clear()

	print(
		"[DirectStorage] Plugin stopped (addon still enabled); %s autoload kept in project.godot."
		% AUTOLOAD_NAME
	)


## Called by the engine only when the user disables the addon in the Plugin tab
## of the project settings (the editor never calls it while quitting). This is
## the single place that unregisters the autoload, so a plain editor shutdown
## cannot strip it from project.godot again.
func _disable_plugin() -> void:
	_sync_autoload(AUTOLOAD_NAME, "")

	print("[DirectStorage] Plugin disabled. Autoload, Importer, and Exporter unregistered.")


## Writes (or erases) an `autoload/` entry directly, then persists project.godot.
##
## `add_autoload_singleton()`/`remove_autoload_singleton()` are deliberately NOT
## used: they go through EditorAutoloadSettings, which wraps the change in a
## GLOBAL_HISTORY undo action ("Add Autoload"/"Remove Autoload"). Committing that
## sets the editor's global unsaved flag, which is what puts "(*)" in the window
## title and makes quitting ask to save modified resources - and unlike a scene
## edit it cannot be cleared by undo. Writing the setting has no undo entry, and
## saving immediately means the entry on disk matches the plugin state instead of
## waiting for the editor to save project.godot.
##
## An empty `p_path` removes the entry.
func _sync_autoload(p_name: String, p_path: String) -> void:
	var key := "autoload/" + p_name

	if p_path.is_empty():
		if not ProjectSettings.has_setting(key):
			return
		ProjectSettings.clear(key)
	elif ProjectSettings.has_setting(key):
		# Already registered (possibly as *uid://...) - leave the form alone.
		return
	else:
		ProjectSettings.set_setting(key, "*" + p_path)
		ProjectSettings.set_as_basic(key, true)

	var err := ProjectSettings.save()
	if err != OK:
		push_warning(
			"[DirectStorage] Could not persist %s to project.godot (error %d)." % [key, err]
		)


func _on_scene_closed(filepath: String) -> void:
	_checked_scenes.erase(filepath)


func _on_scene_changed(root: Node) -> void:
	if root == null:
		return
	var scene_path := root.scene_file_path
	if scene_path.is_empty() or _checked_scenes.has(scene_path):
		return

	_checked_scenes[scene_path] = true
	_check_scene_compatibility.call_deferred(root, scene_path)


func _check_scene_compatibility(root: Node, scene_path: String) -> void:
	if not is_instance_valid(root):
		return

	var lights: Array[Node] = []
	if root is Light3D:
		lights.append(root)
	lights.append_array(root.find_children("*", "Light3D", true, false))

	var decals: Array[Node] = []
	if root is Decal:
		decals.append(root)
	decals.append_array(root.find_children("*", "Decal", true, false))

	if lights.is_empty() and decals.is_empty():
		return

	var invalid_assets: Dictionary = {}
	var light_count := 0
	var decal_count := 0

	for node in lights:
		var light := node as Light3D
		if light and light.light_projector and light.light_projector is Texture2DRD:
			var path := light.light_projector.resource_path
			invalid_assets[path if not path.is_empty() else "(unnamed Texture2DRD)"] = true
			light_count += 1

	for node in decals:
		var decal := node as Decal
		if decal:
			for i in 4:
				var tex := decal.get_texture(i)
				if tex and tex is Texture2DRD:
					var path := tex.resource_path
					invalid_assets[path if not path.is_empty() else "(unnamed Texture2DRD)"] = true
					decal_count += 1

	if invalid_assets.is_empty():
		return

	var msg := "[color=yellow][DirectStorage][/color] Scene [b]%s[/b] has " % scene_path.get_file()
	var parts: Array[String] = []
	if light_count > 0:
		parts.append("%d Light3D projector(s)" % light_count)
	if decal_count > 0:
		parts.append("%d Decal texture(s)" % decal_count)
	msg += " and ".join(parts) + " using [b]Texture2DRD[/b].\n"
	msg += "To eliminate atlas engine warnings, re-import the following texture(s) with [b]compatibility/as_image_texture[/b] enabled in the Import dock:\n"
	for asset_path in invalid_assets:
		msg += "  • %s\n" % asset_path

	print_rich(msg)


## Opens the pack scan dialog. A fresh window is instantiated per invocation and
## the dialog frees itself on close, so no half-finished scan survives between
## runs or between plugin reloads.
##
## The window is parented to the editor's base Control rather than shown through
## EditorInterface.popup_dialog_centered(). That helper parents it to EditorNode,
## which is a plain Node, and theme inheritance chains are broken by nodes that
## are neither Control nor Window - the dialog then renders with the default
## theme no matter what is assigned to its own `theme` property. gui_base is
## themed, and is exactly where the editor attaches its own dialogs.
func _on_verify_exported_pack() -> void:
	var dialog_scene := load(VERIFY_DIALOG_SCENE) as PackedScene
	if dialog_scene == null:
		push_error("[DirectStorage] Pack scan dialog missing at " + VERIFY_DIALOG_SCENE_PATH)
		return
	var dialog := dialog_scene.instantiate() as Window
	_verify_dialog = dialog
	var base := EditorInterface.get_base_control()
	if base == null:
		# Extremely unlikely; keep the API path working rather than dropping the
		# request on the floor.
		EditorInterface.popup_dialog_centered(dialog)
		return
	base.add_child(dialog)
	dialog.popup_centered()
