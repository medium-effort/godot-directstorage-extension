@tool
extends EditorPlugin

## DirectStorage Accelerator Editor Plugin
##
## Author: MediumEffort
## Description: Registers the DSBatchManager autoload, DSTex importer, and
## DSPkg export plugin in the Godot Editor. Enable this plugin to get the
## automatic texture import and archive bundling workflow.

const AUTOLOAD_NAME: String = "DSBatchManager"
const AUTOLOAD_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_batch_manager.gd"

var _importer: EditorImportPlugin = null
var _export_plugin: EditorExportPlugin = null

## Registers the autoload, import plugin, export plugin, and DirectStorage
## runtime when the addon is enabled.
func _enter_tree() -> void:
	if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)
		else:
			ProjectSettings.set_setting("autoload/" + AUTOLOAD_NAME, "*" + AUTOLOAD_PATH)

	print("[DirectStorage] Plugin enabled by MediumEffort. Autoload, Importer, and Exporter registered.")

	if _importer == null:
		var importer_script = load("res://addons/directstorage_accelerator/scripts/ds_importer.gd")
		if importer_script:
			_importer = importer_script.new()
			if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
				add_import_plugin(_importer)

	if _export_plugin == null:
		var exporter_script = load("res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd")
		if exporter_script:
			_export_plugin = exporter_script.new()
			if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
				add_export_plugin(_export_plugin)

	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()

## Unregisters the autoload, import plugin, and export plugin when disabled.
func _exit_tree() -> void:
	if _importer != null:
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			remove_import_plugin(_importer)
		_importer = null

	if _export_plugin != null:
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			remove_export_plugin(_export_plugin)
		_export_plugin = null

	if ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
			remove_autoload_singleton(AUTOLOAD_NAME)
		else:
			ProjectSettings.set_setting("autoload/" + AUTOLOAD_NAME, null)

	print("[DirectStorage] Plugin disabled. Autoload, Importer, and Exporter unregistered.")
