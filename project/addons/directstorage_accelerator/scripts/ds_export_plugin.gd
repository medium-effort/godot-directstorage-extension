@tool
extends EditorExportPlugin

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: Bundles imported .dstex texture files into one .dspkg archive
## during project export. The resulting archive is written as
## res://textures.dspkg for the runtime resource loader.

var _dspkg: RefCounted = null
var _temp_dspkg_path: String = "user://temp_textures.dspkg"
var _has_textures: bool = false

## Returns the stable export plugin name shown to Godot.
func _get_name() -> String:
	return "DirectStorageExporter"

## Starts a temporary DSPkg archive for the current export.
func _export_begin(features: PackedStringArray, is_debug: bool, path: String, flags: int) -> void:
	print("[DirectStorage Exporter] Export started for target: ", path)
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
		else:
			print("[DirectStorage Exporter] Initialized temporary DSPkg archive at: ", _temp_dspkg_path)

## Adds an imported .dstex resource to the archive and prevents the individual
## texture from being copied into the exported PCK.
func _export_file(path: String, type: String, features: PackedStringArray) -> void:
	if _dspkg == null:
		return

	var dstex_path: String = ""
	var resource_path: String = path

	if path.ends_with(".dstex"):
		dstex_path = path
	else:
		var import_path := path if path.ends_with(".import") else path + ".import"
		if FileAccess.file_exists(import_path):
			var config := ConfigFile.new()
			if config.load(import_path) == OK:
				var importer: String = config.get_value("remap", "importer", "")
				var remap_path: String = config.get_value("remap", "path", "")
				if importer == "directstorage_texture" or remap_path.ends_with(".dstex"):
					dstex_path = remap_path
					if resource_path.ends_with(".import"):
						resource_path = resource_path.trim_suffix(".import")

	if not dstex_path.is_empty() and FileAccess.file_exists(dstex_path):
		var add_err: Error = _dspkg.add_texture(resource_path, dstex_path)
		if add_err == OK:
			_has_textures = true
			print("[DirectStorage Exporter] Added texture to archive: ", resource_path, " (from ", dstex_path, ")")
			skip() # Skip adding individual .dstex / texture file to PCK since it will be bundled into .dspkg
		else:
			push_error("[DirectStorage Exporter] Failed to add texture to archive: " + dstex_path)

## Finalizes the archive, adds it to the export, and removes the temporary file.
func _export_end() -> void:
	if _dspkg != null:
		if _has_textures:
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
		else:
			print("[DirectStorage Exporter] Export finished. No DirectStorage (.dstex) textures were found to bundle.")
	_dspkg = null
	if FileAccess.file_exists(_temp_dspkg_path):
		DirAccess.remove_absolute(_temp_dspkg_path)
