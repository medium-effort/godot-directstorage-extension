@tool
extends EditorExportPlugin

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: Bundles imported .dstex texture files into one .dspkg archive
## during project export and automatically deploys native GDExtension binaries
## to the required addons/directstorage_accelerator/bin subdirectory.

const OPTION_BUNDLE_DSPKG: StringName = &"directstorage/bundle_textures_dspkg"

var _dspkg: RefCounted = null
var _temp_dspkg_path: String = "user://temp_textures.dspkg"
var _has_textures: bool = false
var _target_export_dir: String = ""
var _export_features: PackedStringArray = []
var _is_debug_export: bool = false
var _bundle_dspkg_enabled: bool = false

## Returns the stable export plugin name shown to Godot.
func _get_name() -> String:
	return "DirectStorageExporter"

## Declares whether this export plugin supports the target platform.
func _supports_platform(platform: EditorExportPlatform) -> bool:
	if platform is EditorExportPlatformWindows:
		return true
	if platform != null and platform.get_os_name() == "Windows":
		return true
	return false

## Exposes custom export options for Windows Desktop in the Export dialog.
func _get_export_options(platform: EditorExportPlatform) -> Array[Dictionary]:
	if not _supports_platform(platform):
		return []
	return [
		{
			"option": {
				"name": OPTION_BUNDLE_DSPKG,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": true,
		}
	]

## Starts a temporary DSPkg archive for the current export if bundling is enabled.
func _export_begin(features: PackedStringArray, is_debug: bool, path: String, flags: int) -> void:
	print("[DirectStorage Exporter] Export started for target: ", path)
	var resolved_export_path := path.strip_edges().simplify_path()
	if resolved_export_path.is_empty():
		_target_export_dir = ""
		push_error("[DirectStorage Exporter] Export path is empty; archive and binary deployment will be skipped.")
	else:
		if resolved_export_path.begins_with("res://") or resolved_export_path.begins_with("user://"):
			resolved_export_path = ProjectSettings.globalize_path(resolved_export_path)
		elif not resolved_export_path.is_absolute_path():
			resolved_export_path = ProjectSettings.globalize_path("res://" + resolved_export_path.trim_prefix("./"))
		_target_export_dir = resolved_export_path.get_base_dir().simplify_path()
		if not _target_export_dir.is_absolute_path():
			push_error("[DirectStorage Exporter] Could not resolve an absolute export directory from: " + path)
			_target_export_dir = ""
		else:
			var target_dir_err := DirAccess.make_dir_recursive_absolute(_target_export_dir)
			if target_dir_err != OK and not DirAccess.dir_exists_absolute(_target_export_dir):
				push_error("[DirectStorage Exporter] Failed to prepare export directory '" + _target_export_dir + "': " + error_string(target_dir_err) + ". Verify the export path is writable.")
			else:
				print("[DirectStorage Exporter] Resolved export directory: ", _target_export_dir)
	_export_features = features
	_is_debug_export = is_debug
	_bundle_dspkg_enabled = bool(get_option(OPTION_BUNDLE_DSPKG))
	_has_textures = false
	_dspkg = null

	if not _bundle_dspkg_enabled:
		print("[DirectStorage Exporter] Standalone DSPkg bundling is disabled. Textures (.dstex) will be packaged directly into the PCK.")
		return

	print("[DirectStorage Exporter] Standalone DSPkg bundling is ENABLED. Textures will be bundled into textures.dspkg.")

	if not ClassDB.class_exists("DSPkg"):
		push_warning("[DirectStorage Exporter] DSPkg class not found. Skipping archive bundling.")
		return

	_cleanup_temp_dspkg()
	_dspkg = ClassDB.instantiate("DSPkg")
	if _dspkg != null:
		var err: Error = _dspkg.create(_temp_dspkg_path)
		if err != OK:
			push_error("[DirectStorage Exporter] Failed to create temporary DSPkg archive at: " + _temp_dspkg_path + ": " + error_string(err))
			_dspkg = null
		else:
			print("[DirectStorage Exporter] Initialized temporary DSPkg archive at: ", _temp_dspkg_path)

## Reads v2 per-mip footprint data so DSPkg can preserve GPU upload layouts.
func _read_v2_footprint_metadata(dstex_path: String) -> Dictionary:
	if not ClassDB.class_exists("DSTex"):
		return {}
	var dstex: RefCounted = ClassDB.instantiate("DSTex")
	if dstex == null:
		return {}
	var metadata: Dictionary = dstex.read_header(dstex_path)
	if int(metadata.get("version", 0)) != 2:
		return {}
	var mip_count := int(metadata.get("mip_count", 0))
	var mip_sizes: PackedInt32Array = metadata.get("mip_sizes", PackedInt32Array())
	var mip_uncompressed_sizes: PackedInt32Array = metadata.get("mip_uncompressed_sizes", PackedInt32Array())
	var mip_row_pitches: PackedInt32Array = metadata.get("mip_row_pitches", PackedInt32Array())
	if mip_count <= 0 or mip_sizes.size() != mip_count or mip_uncompressed_sizes.size() != mip_count or mip_row_pitches.size() != mip_count:
		push_warning("[DirectStorage Exporter] Invalid v2 footprint metadata in: " + dstex_path)
		return {}
	return metadata

## Adds an imported .dstex resource to the archive when bundling is enabled.
func _export_file(path: String, type: String, features: PackedStringArray) -> void:
	if _dspkg == null or not _bundle_dspkg_enabled:
		# When DSPkg bundling is disabled, let Godot export .dstex directly into PCK
		return

	# Never skip .import files so Godot preserves remap tables in PCK
	if path.ends_with(".import"):
		print("[DirectStorage Exporter] Preserving remap metadata in PCK: ", path)
		return

	var dstex_path: String = ""
	var resource_path: String = path

	if path.ends_with(".dstex"):
		dstex_path = path
	else:
		var import_path := path + ".import"
		if FileAccess.file_exists(import_path):
			var config := ConfigFile.new()
			if config.load(import_path) == OK:
				var importer: String = config.get_value("remap", "importer", "")
				var remap_path: String = config.get_value("remap", "path", "")
				if importer == "directstorage_texture" or remap_path.ends_with(".dstex"):
					dstex_path = remap_path

	if not dstex_path.is_empty() and FileAccess.file_exists(dstex_path):
		var mip_metadata := _read_v2_footprint_metadata(dstex_path)
		var add_err: Error = _dspkg.add_texture(resource_path, dstex_path, mip_metadata)
		if dstex_path != resource_path:
			var remapped_add_err: Error = _dspkg.add_texture(dstex_path, dstex_path, mip_metadata)
			if add_err == OK:
				add_err = remapped_add_err

		if add_err == OK:
			_has_textures = true
			print("[DirectStorage Exporter] Added texture to archive: ", resource_path, " (from ", dstex_path, ")")
			# Skip texture payload from PCK so it is only stored in textures.dspkg.
			skip()
			print("[DirectStorage Exporter] Skipped texture payload from PCK: ", resource_path)
		else:
			push_error("[DirectStorage Exporter] Failed to add texture to archive ('" + resource_path + "' and remap target '" + dstex_path + "'): " + error_string(add_err) + ". The texture will remain in the PCK.")

## Finalizes the archive, writes textures.dspkg (if enabled), and deploys binaries.
func _export_end() -> void:
	if _dspkg != null and _bundle_dspkg_enabled:
		if _has_textures:
			var err: Error = _dspkg.finalize()
			if err == OK:
				if not _target_export_dir.is_empty():
					var target_dspkg_path := _target_export_dir.path_join("textures.dspkg")
					var temp_dspkg_path := ProjectSettings.globalize_path(_temp_dspkg_path)
					if not FileAccess.file_exists(temp_dspkg_path):
						push_error("[DirectStorage Exporter] Finalized archive is missing at '" + temp_dspkg_path + "'; textures.dspkg was not deployed.")
					else:
						var copy_err := DirAccess.copy_absolute(temp_dspkg_path, target_dspkg_path)
						if copy_err == OK:
							print("[DirectStorage Exporter] Successfully deployed standalone textures archive to: ", target_dspkg_path)
						else:
							push_error("[DirectStorage Exporter] Failed to copy textures.dspkg to '" + target_dspkg_path + "': " + error_string(copy_err) + ". Verify the export directory is writable and not locked by another process.")
				else:
					push_error("[DirectStorage Exporter] Target export directory is empty; could not deploy textures.dspkg. Verify the export path passed to _export_begin().")
			else:
				push_error("[DirectStorage Exporter] Failed to finalize DSPkg archive: " + error_string(err) + ". The temporary archive will be removed.")
		else:
			print("[DirectStorage Exporter] Export finished. No DirectStorage (.dstex) textures were found to bundle.")

	_dspkg = null
	_cleanup_temp_dspkg()

	# Deploy native shared libraries to the exact subfolder expected by GDExtension runtime
	if not _target_export_dir.is_empty() and _export_features.has("windows"):
		_deploy_windows_binaries(_target_export_dir, _is_debug_export, _export_features)

## Removes the per-export archive without leaving stale user:// artifacts behind.
func _cleanup_temp_dspkg() -> void:
	if not FileAccess.file_exists(_temp_dspkg_path):
		return
	var temp_dspkg_path := ProjectSettings.globalize_path(_temp_dspkg_path)
	var remove_err := DirAccess.remove_absolute(temp_dspkg_path)
	if remove_err != OK and FileAccess.file_exists(_temp_dspkg_path):
		push_warning("[DirectStorage Exporter] Could not remove temporary archive '" + _temp_dspkg_path + "': " + error_string(remove_err) + ". Remove it before the next export.")

## Deploys the GDExtension DLL and DirectStorage runtime dependencies to addons/directstorage_accelerator/bin.
func _deploy_windows_binaries(target_base_dir: String, is_debug: bool, features: PackedStringArray) -> void:
	var target_bin_dir := target_base_dir.path_join("addons/directstorage_accelerator/bin")
	var make_dir_err := DirAccess.make_dir_recursive_absolute(target_bin_dir)
	if make_dir_err != OK and not DirAccess.dir_exists_absolute(target_bin_dir):
		push_error("[DirectStorage Exporter] Failed to create GDExtension target binary directory: " + target_bin_dir)
		return

	var src_bin_dir := "res://addons/directstorage_accelerator/bin/"
	var dlls_to_copy: Array[String] = [
		"dstorage.dll",
		"dstoragecore.dll"
	]

	var main_dll := ""
	if is_debug:
		if features.has("dev") and FileAccess.file_exists(src_bin_dir.path_join("directstorage_accelerator.windows.template_debug.dev.x86_64.dll")):
			main_dll = "directstorage_accelerator.windows.template_debug.dev.x86_64.dll"
		else:
			main_dll = "directstorage_accelerator.windows.template_debug.x86_64.dll"
	else:
		main_dll = "directstorage_accelerator.windows.template_release.x86_64.dll"

	dlls_to_copy.append(main_dll)

	for dll_name in dlls_to_copy:
		var src_path := src_bin_dir.path_join(dll_name)
		var dst_path := target_bin_dir.path_join(dll_name)
		if FileAccess.file_exists(src_path):
			var copy_err := DirAccess.copy_absolute(src_path, dst_path)
			if copy_err == OK:
				print("[DirectStorage Exporter] Deployed native binary: ", dst_path)
			else:
				push_warning("[DirectStorage Exporter] Could not copy " + dll_name + " to export directory: " + error_string(copy_err))
		else:
			push_warning("[DirectStorage Exporter] Source binary missing at: " + src_path)

		# Clean up redundant duplicate DLL from root export folder left by Godot's default exporter
		var root_dll_path := target_base_dir.path_join(dll_name)
		if FileAccess.file_exists(root_dll_path):
			var remove_err := DirAccess.remove_absolute(root_dll_path)
			if remove_err == OK:
				print("[DirectStorage Exporter] Cleaned up redundant root binary: ", dll_name)
