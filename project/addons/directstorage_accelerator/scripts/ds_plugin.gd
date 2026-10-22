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

var _importer: EditorImportPlugin = null
var _export_plugin: EditorExportPlugin = null
var _checked_scenes: Dictionary = {}


## Registers the autoload, import plugin, export plugin, and DirectStorage
## runtime when the addon is enabled.
func _enter_tree() -> void:
	# Purge legacy autoload if present from older addon versions
	if ProjectSettings.has_setting("autoload/" + LEGACY_AUTOLOAD_NAME):
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			remove_autoload_singleton(LEGACY_AUTOLOAD_NAME)
		else:
			ProjectSettings.set_setting("autoload/" + LEGACY_AUTOLOAD_NAME, null)

	if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)
		else:
			ProjectSettings.set_setting("autoload/" + AUTOLOAD_NAME, "*" + AUTOLOAD_PATH)

	print(
		"[DirectStorage] Plugin enabled by MediumEffort. Autoload, Importer, and Exporter registered."
	)

	for internal_setting in [
		"directstorage/export/bundle_dspkg",
		"directstorage/expected_archive_fingerprint",
		"directstorage/project_guid",
		"directstorage/archive_name",
	]:
		if ProjectSettings.has_setting(internal_setting):
			ProjectSettings.set_as_internal(internal_setting, true)

	if _importer == null:
		var importer_script = load("res://addons/directstorage_accelerator/scripts/ds_importer.gd")
		if importer_script:
			_importer = importer_script.new()
			add_import_plugin(_importer)

	if _export_plugin == null:
		var exporter_script = load(
			"res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd"
		)
		if exporter_script:
			_export_plugin = exporter_script.new()
			add_export_plugin(_export_plugin)

	if not scene_changed.is_connected(_on_scene_changed):
		scene_changed.connect(_on_scene_changed)
	if not scene_closed.is_connected(_on_scene_closed):
		scene_closed.connect(_on_scene_closed)

	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()


## Unregisters the autoload, import plugin, and export plugin when disabled.
func _exit_tree() -> void:
	if _importer != null:
		remove_import_plugin(_importer)
		_importer = null

	if _export_plugin != null:
		remove_export_plugin(_export_plugin)
		_export_plugin = null

	if scene_changed.is_connected(_on_scene_changed):
		scene_changed.disconnect(_on_scene_changed)
	if scene_closed.is_connected(_on_scene_closed):
		scene_closed.disconnect(_on_scene_closed)
	_checked_scenes.clear()

	if ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			remove_autoload_singleton(AUTOLOAD_NAME)
		else:
			ProjectSettings.set_setting("autoload/" + AUTOLOAD_NAME, null)

	print("[DirectStorage] Plugin disabled. Autoload, Importer, and Exporter unregistered.")


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
