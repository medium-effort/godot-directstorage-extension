@tool
extends EditorPlugin

## DirectStorage Accelerator Editor Plugin
##
## Author: MediumEffort
## Description: Handles registration of the DSBatchManager autoload singleton,
## custom DSTex importer, and DSPkg export plugin in Godot Editor.

const AUTOLOAD_NAME: String = "DSBatchManager"
const AUTOLOAD_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_batch_manager.gd"

var _importer: EditorImportPlugin = null
var _export_plugin: EditorExportPlugin = null

## Called when the plugin enters the scene tree in Godot Editor.
func _enter_tree() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)
	
	var importer_script = load("res://addons/directstorage_accelerator/scripts/ds_importer.gd")
	if importer_script:
		_importer = importer_script.new()
		add_import_plugin(_importer)
	
	var exporter_script = load("res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd")
	if exporter_script:
		_export_plugin = exporter_script.new()
		add_export_plugin(_export_plugin)
		
	print("[DirectStorage] Plugin enabled by MediumEffort. Autoload, Importer, and Exporter registered.")

## Called when the plugin exits the scene tree or is disabled in Godot Editor.
func _exit_tree() -> void:
	remove_autoload_singleton(AUTOLOAD_NAME)
	
	if _importer:
		remove_import_plugin(_importer)
		_importer = null
		
	if _export_plugin:
		remove_export_plugin(_export_plugin)
		_export_plugin = null
		
	print("[DirectStorage] Plugin disabled. Autoload, Importer, and Exporter unregistered.")

