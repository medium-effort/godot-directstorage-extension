@tool
extends EditorExportPlugin

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: EditorExportPlugin for bundling .dstex texture files into a single .dspkg archive upon export.

var _dspkg: RefCounted = null
var _temp_dspkg_path: String = "user://temp_textures.dspkg"
var _has_textures: bool = false

func _get_name() -> String:
	return "DirectStorageExporter"

func _export_begin(features: PackedStringArray, is_debug: bool, path: String, flags: int) -> void:
	if not ClassDB.class_exists("DSPkg"):
		push_warning("[DirectStorage Exporter] DSPkg class not found. Skipping archive bundling.")
		_dspkg = null
		return

	_dspkg = ClassDB.instantiate("DSPkg")
	_has_textures = false
	if _dspkg != null:
		var err: Error = _dspkg.create(_temp_dspkg_path)
		if err != OK:
			push_error("[DirectStorage Exporter] Failed to create temporary DSPkg archive at: " + _temp_dspkg_path)

func _export_file(path: String, type: String, features: PackedStringArray) -> void:
	if _dspkg == null:
		return

	if path.ends_with(".dstex"):
		var add_err: Error = _dspkg.add_texture(path, path)
		if add_err == OK:
			_has_textures = true
			skip() # Skip adding individual .dstex file to PCK since it will be bundled into .dspkg
		else:
			push_error("[DirectStorage Exporter] Failed to add texture to archive: " + path)

func _export_end() -> void:
	if _dspkg != null and _has_textures:
		var err: Error = _dspkg.finalize()
		if err == OK:
			var bytes := FileAccess.get_file_as_bytes(_temp_dspkg_path)
			if bytes.size() > 0:
				add_file("res://textures.dspkg", bytes, false)
				print("[DirectStorage Exporter] Successfully bundled textures into res://textures.dspkg (", bytes.size(), " bytes)")
			else:
				push_error("[DirectStorage Exporter] Generated .dspkg archive is empty.")
		else:
			push_error("[DirectStorage Exporter] Failed to finalize DSPkg archive.")
	_dspkg = null
