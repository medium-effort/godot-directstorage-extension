@tool
extends EditorPlugin

## DirectStorage Accelerator Editor Plugin
##
## Author: MediumEffort
## Description: Handles registration of the DSBatchManager autoload singleton in Godot Editor.

const AUTOLOAD_NAME: String = "DSBatchManager"
const AUTOLOAD_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_batch_manager.gd"

## Called when the plugin enters the scene tree in Godot Editor.
func _enter_tree() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)
	print("[DirectStorage] Plugin enabled by MediumEffort. Autoload singleton registered.")

## Called when the plugin exits the scene tree or is disabled in Godot Editor.
func _exit_tree() -> void:
	remove_autoload_singleton(AUTOLOAD_NAME)
	print("[DirectStorage] Plugin disabled. Autoload singleton unregistered.")
