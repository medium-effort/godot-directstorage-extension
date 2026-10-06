@tool
extends EditorExportPlugin

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: Bundles imported .dstex texture files into one .dspkg archive
## during project export and automatically deploys native GDExtension binaries
## to the required addons/directstorage_accelerator/bin subdirectory.

const OPTION_BUNDLE_DSPKG: StringName = &"directstorage/bundle_textures_dspkg"

## During export a combined header index (textures.headers.idx) is written
## next to textures.dspkg so the runtime can look up bundled texture headers
## without opening every file.
const COMBINED_INDEX_MAGIC: int = 0x58444948
const COMBINED_INDEX_VERSION: int = 1
const COMBINED_INDEX_MAX_ENTRIES: int = 8192
const COMBINED_INDEX_MAX_KEY_BYTES: int = 1024
const COMBINED_INDEX_MIP_SLOTS: int = 16

var _dspkg: RefCounted = null
## Staging/final archive paths. Both live in the target export directory so no
## temporary data is written to C: (user://). finalize() writes the staging
## file once; it is then renamed to the final name (metadata-only, no extra
## disk space needed for a copy).
var _staging_dspkg_path: String = ""
var _target_dspkg_path: String = ""
## Legacy temp location from before direct-to-export writes. Only used to
## clean up stale artifacts on C:.
const LEGACY_TEMP_DSPKG_PATH: String = "user://temp_textures.dspkg"
var _has_textures: bool = false
## Texture paths bundled by this export run.
var _bundled_entries: Array[Dictionary] = []
var _archive_failed: bool = false
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
	_bundled_entries.clear()
	_archive_failed = false
	_dspkg = null
	_staging_dspkg_path = ""
	_target_dspkg_path = ""

	if not _bundle_dspkg_enabled:
		print("[DirectStorage Exporter] Standalone DSPkg bundling is disabled. Textures (.dstex) will be packaged directly into the PCK.")
		return

	print("[DirectStorage Exporter] Standalone DSPkg bundling is ENABLED. Textures will be bundled into textures.dspkg.")

	if not ClassDB.class_exists("DSPkg"):
		push_warning("[DirectStorage Exporter] DSPkg class not found. Skipping archive bundling.")
		return

	if _target_export_dir.is_empty():
		push_error("[DirectStorage Exporter] FATAL: no export directory resolved; textures.dspkg cannot be written. The export is incomplete and binaries will NOT be deployed.")
		_archive_failed = true
		return

	_target_dspkg_path = _target_export_dir.path_join("textures.dspkg")
	_staging_dspkg_path = _target_export_dir.path_join("textures.dspkg.tmp")
	_warn_if_low_disk_space(_target_export_dir)
	_cleanup_staging()
	_cleanup_legacy_user_temp()
	_dspkg = ClassDB.instantiate("DSPkg")
	if _dspkg != null:
		var err: Error = _dspkg.create(_staging_dspkg_path)
		if err != OK:
			push_error("[DirectStorage Exporter] Failed to create DSPkg staging archive at: " + _staging_dspkg_path + ": " + error_string(err))
			_dspkg = null
			_archive_failed = true
		else:
			print("[DirectStorage Exporter] Initialized DSPkg staging archive at: ", _staging_dspkg_path)

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
		if add_err == OK and dstex_path != resource_path:
			var remapped_add_err: Error = _dspkg.add_texture(dstex_path, dstex_path, mip_metadata)
			if remapped_add_err != OK:
				_archive_failed = true
				_dspkg = null
				push_error("[DirectStorage Exporter] Failed to add remapped texture entry '" + dstex_path + "': " + error_string(remapped_add_err) + ". DSPkg bundling was aborted and the texture will remain in the PCK.")
				return

		if add_err != OK:
			_archive_failed = true
			_dspkg = null
			push_error("[DirectStorage Exporter] Failed to add texture to archive ('" + resource_path + "' and remap target '" + dstex_path + "'): " + error_string(add_err) + ". DSPkg bundling was aborted and the texture will remain in the PCK.")
			return

		_has_textures = true
		_track_bundled_entry(dstex_path)
		print("[DirectStorage Exporter] Added texture to archive: ", resource_path, " (from ", dstex_path, ")")
		# Skip texture payload from PCK so it is only stored in textures.dspkg.
		skip()
		print("[DirectStorage Exporter] Skipped texture payload from PCK: ", resource_path)

## Finalizes the archive, renames textures.dspkg.tmp to textures.dspkg
## (if enabled), and deploys binaries. A DSPkg failure is fatal for this
## plugin's outputs: partial archives are removed and binary deployment is
## skipped so a broken build is never silently shipped. NOTE: Godot does not
## let an EditorExportPlugin abort the core PCK/exe steps from _export_end(),
## so push_error() here marks the export as failed in the log but the
## remaining core steps still run outside this plugin's control.
func _export_end() -> void:
	var dspkg_ok := true
	if _archive_failed:
		dspkg_ok = false
		push_error("[DirectStorage Exporter] FATAL: export finished without a textures.dspkg archive because bundling was aborted after an earlier failure. Partial archives were removed and binaries were NOT deployed. See earlier errors for the cause (a likely candidate is out of disk space).")
	elif _dspkg != null and _bundle_dspkg_enabled:
		if _has_textures:
			var err: Error = _dspkg.finalize()
			if err != OK:
				dspkg_ok = false
				_report_storage_failure("finalize DSPkg staging archive", err, _staging_dspkg_path)
				_cleanup_staging()
				_remove_file_silent(_target_dspkg_path)
			elif _staging_dspkg_path.is_empty() or _target_dspkg_path.is_empty():
				dspkg_ok = false
				push_error("[DirectStorage Exporter] FATAL: export directory was lost before finalize; textures.dspkg was not deployed and binaries were NOT deployed.")
			elif not FileAccess.file_exists(_staging_dspkg_path):
				dspkg_ok = false
				push_error("[DirectStorage Exporter] FATAL: finalized staging archive is missing at '" + _staging_dspkg_path + "'; textures.dspkg was not deployed and binaries were NOT deployed.")
			else:
				_remove_file_silent(_target_dspkg_path)
				var rename_err := DirAccess.rename_absolute(_staging_dspkg_path, _target_dspkg_path)
				if rename_err == OK:
					print("[DirectStorage Exporter] Successfully deployed standalone textures archive to: ", _target_dspkg_path)
					_write_combined_header_index(_target_dspkg_path.get_basename() + ".headers.idx")
				else:
					dspkg_ok = false
					_report_storage_failure("rename textures.dspkg.tmp to textures.dspkg", rename_err, _target_dspkg_path)
					_cleanup_staging()
					_remove_file_silent(_target_dspkg_path)
		else:
			print("[DirectStorage Exporter] Export finished. No DirectStorage (.dstex) textures were found to bundle.")

	_dspkg = null
	_cleanup_staging()
	_cleanup_legacy_user_temp()

	if not dspkg_ok:
		# Do not ship DLLs next to a missing/broken textures.dspkg.
		return

	# Deploy native shared libraries to the exact subfolder expected by GDExtension runtime
	if not _target_export_dir.is_empty() and _export_features.has("windows"):
		_deploy_windows_binaries(_target_export_dir, _is_debug_export, _export_features)

## Removes the per-export staging file (textures.dspkg.tmp) in the target
## export directory. A rename needs no extra disk space, so normally nothing
## is left behind; this covers finalize/rename failures and interruptions.
func _cleanup_staging() -> void:
	_remove_file_silent(_staging_dspkg_path)

## Removes the pre-direct-write temp artifact on C:, if a previous version
## left one behind. Best-effort migration cleanup only.
func _cleanup_legacy_user_temp() -> void:
	if not FileAccess.file_exists(LEGACY_TEMP_DSPKG_PATH):
		return
	var global_path := ProjectSettings.globalize_path(LEGACY_TEMP_DSPKG_PATH)
	var remove_err := DirAccess.remove_absolute(global_path)
	if remove_err != OK and FileAccess.file_exists(LEGACY_TEMP_DSPKG_PATH):
		push_warning("[DirectStorage Exporter] Could not remove legacy temporary archive '" + LEGACY_TEMP_DSPKG_PATH + "': " + error_string(remove_err) + ". Remove it before the next export.")

func _remove_file_silent(abs_path: String) -> void:
	if abs_path.is_empty() or not FileAccess.file_exists(abs_path):
		return
	DirAccess.remove_absolute(abs_path)

## Best-effort free-space query for an absolute directory. Returns bytes free,
## or -1 when the engine build does not expose DirAccess.get_space_left().
func _get_free_space_bytes(abs_dir: String) -> int:
	if abs_dir.is_empty():
		return -1
	if not DirAccess.dir_exists_absolute(abs_dir):
		return -1
	var probe: DirAccess = DirAccess.open(abs_dir)
	if probe == null:
		return -1
	if not probe.has_method("get_space_left"):
		return -1
	var free_value: Variant = probe.call("get_space_left")
	if typeof(free_value) == TYPE_INT:
		return int(free_value)
	return -1

func _format_bytes(bytes_value: int) -> String:
	if bytes_value < 0:
		return "unknown"
	var value := float(bytes_value)
	if value < 1024.0:
		return str(bytes_value) + " B"
	value /= 1024.0
	if value < 1024.0:
		return "%.1f KiB" % value
	value /= 1024.0
	if value < 1024.0:
		return "%.1f MiB" % value
	value /= 1024.0
	return "%.2f GiB" % value

## Early warning when the target drive is nearly full. The final archive size
## is only known at finalize() time, so this cannot abort the export; it just
## names the drive and the free bytes while there is still time to free space.
func _warn_if_low_disk_space(abs_dir: String) -> void:
	const WARN_THRESHOLD_BYTES: int = 1024 * 1024 * 1024
	var free_bytes := _get_free_space_bytes(abs_dir)
	if free_bytes < 0:
		return
	print("[DirectStorage Exporter] Free space on export drive: ", _format_bytes(free_bytes), " (", abs_dir, ")")
	if free_bytes < WARN_THRESHOLD_BYTES:
		push_warning("[DirectStorage Exporter] Low disk space: only " + _format_bytes(free_bytes) + " free at '" + abs_dir + "'. The textures.dspkg + PCK + exe typically need several hundred MiB. Free up space before the export fails with file-write errors.")

func _is_disk_space_error(err: Error) -> bool:
	return err == ERR_FILE_CANT_WRITE or err == ERR_FILE_CANT_OPEN or err == ERR_CANT_CREATE or err == FAILED

## Explicit failure report that names disk space as the likely cause (with
## free bytes and staging size when known) instead of a generic write error.
## Partial outputs are the caller's responsibility; binaries are NOT deployed.
func _report_storage_failure(stage: String, err: Error, abs_path: String) -> void:
	var free_bytes := _get_free_space_bytes(_target_export_dir)
	var staging_bytes := -1
	if not _staging_dspkg_path.is_empty() and FileAccess.file_exists(_staging_dspkg_path):
		var probe := FileAccess.open(_staging_dspkg_path, FileAccess.READ)
		if probe != null and probe.is_open():
			staging_bytes = int(probe.get_length())
			probe.close()
	var detail := "[DirectStorage Exporter] FATAL: failed to " + stage + " ('" + abs_path + "'): " + error_string(err)
	if _is_disk_space_error(err):
		detail += ". This is most likely OUT OF DISK SPACE on the export drive"
		if not _target_export_dir.is_empty():
			detail += " ('" + _target_export_dir + "'"
			if free_bytes >= 0:
				detail += ", free: " + _format_bytes(free_bytes)
			detail += ")"
		if staging_bytes >= 0:
			detail += "; staging archive size: " + _format_bytes(staging_bytes)
		detail += ". Free up space (the PCK + exe + textures.dspkg together need several hundred MiB), delete any partial textures.dspkg / textures.dspkg.tmp, and re-export."
	else:
		detail += ". Verify the export directory is writable and not locked by another process"
		if free_bytes >= 0:
			detail += " (free space: " + _format_bytes(free_bytes) + " at '" + _target_export_dir + "')"
		detail += "."
	detail += " Partial archives were removed and binaries were NOT deployed."
	push_error(detail)

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

## Records a bundled dstex res path for the combined header index (deduped).
func _track_bundled_entry(dstex_path: String) -> void:
	for existing in _bundled_entries:
		if String(existing.get("dstex_path", "")) == dstex_path:
			return
	_bundled_entries.append({"dstex_path": dstex_path})

## Writes the combined header index (e.g. textures.headers.idx) next
## to the deployed textures.dspkg. Best-effort: any failure only
## warns and never affects the deployed archive.
func _write_combined_header_index(target_index_path: String) -> void:
	if _bundled_entries.is_empty():
		return
	if _bundled_entries.size() > COMBINED_INDEX_MAX_ENTRIES:
		push_warning("[DirectStorage Exporter] Too many bundled textures for the combined header index (" + str(_bundled_entries.size()) + " > " + str(COMBINED_INDEX_MAX_ENTRIES) + "); skipping textures.headers.idx.")
		return
	if not ClassDB.class_exists("DSTex"):
		push_warning("[DirectStorage Exporter] DSTex class not found; skipping combined header index.")
		return
	var payload := PackedByteArray()
	var entry_count := 0
	for entry in _bundled_entries:
		var entry_bytes := _build_combined_index_entry(String(entry.get("dstex_path", "")))
		if entry_bytes.is_empty():
			continue
		payload.append_array(entry_bytes)
		entry_count += 1
	if entry_count == 0:
		push_warning("[DirectStorage Exporter] No valid texture headers for the combined header index; skipping textures.headers.idx.")
		return
	var blob := PackedByteArray()
	blob.append_array(_idx_u32(COMBINED_INDEX_MAGIC))
	blob.append_array(_idx_u16(COMBINED_INDEX_VERSION))
	blob.append_array(_idx_u16(0))
	blob.append_array(_idx_u32(entry_count))
	blob.append_array(_idx_u32(_idx_crc32_ieee(payload)))
	blob.append_array(payload)
	var file := FileAccess.open(target_index_path, FileAccess.WRITE)
	if file == null or not file.is_open():
		push_warning("[DirectStorage Exporter] Could not write combined header index to '" + target_index_path + "'. The textures.dspkg archive is unaffected.")
		return
	file.store_buffer(blob)
	file.close()
	print("[DirectStorage Exporter] Wrote combined header index (", entry_count, " entries) to: ", target_index_path)

## Serializes one combined-index entry for a bundled dstex path. Returns an
## empty array when the header is missing/invalid (caller skips it).
func _build_combined_index_entry(dstex_path: String) -> PackedByteArray:
	var key_bytes := dstex_path.to_utf8_buffer()
	if key_bytes.size() < 1 or key_bytes.size() > COMBINED_INDEX_MAX_KEY_BYTES:
		push_warning("[DirectStorage Exporter] Skipping index entry with invalid key length: " + dstex_path)
		return PackedByteArray()
	var dstex: RefCounted = ClassDB.instantiate("DSTex")
	if dstex == null:
		return PackedByteArray()
	var header: Dictionary = dstex.read_header(dstex_path)
	var width := int(header.get("width", 0))
	var height := int(header.get("height", 0))
	var dstex_version := int(header.get("version", 0))
	var mip_count := int(header.get("mip_count", 0))
	if width <= 0 or height <= 0 or mip_count < 1 or mip_count > COMBINED_INDEX_MIP_SLOTS or (dstex_version != 1 and dstex_version != 2):
		push_warning("[DirectStorage Exporter] Skipping index entry with invalid header: " + dstex_path)
		return PackedByteArray()
	var pixel_format := int(header.get("pixel_format", 0))
	var flags := int(header.get("flags", 0))
	var original_size := int(header.get("original_size", 0))
	var compressed_size := int(header.get("compressed_size", 0))
	var mtime := int(FileAccess.get_modified_time(dstex_path))
	var file_len := 0
	var probe := FileAccess.open(dstex_path, FileAccess.READ)
	if probe != null and probe.is_open():
		file_len = int(probe.get_length())
		probe.close()
	var mip_offsets: PackedInt64Array = header.get("mip_offsets", PackedInt64Array())
	var mip_sizes: PackedInt32Array = header.get("mip_sizes", PackedInt32Array())
	var mip_uncompressed: PackedInt32Array = header.get("mip_uncompressed_sizes", PackedInt32Array())
	var row_pitches: PackedInt32Array = header.get("mip_row_pitches", PackedInt32Array())
	if mip_offsets.size() < mip_count or mip_sizes.size() < mip_count or mip_uncompressed.size() < mip_count or row_pitches.size() < mip_count:
		push_warning("[DirectStorage Exporter] Skipping index entry with incomplete mip tables: " + dstex_path)
		return PackedByteArray()
	var out := PackedByteArray()
	out.append_array(_idx_u32(key_bytes.size()))
	out.append_array(key_bytes)
	out.append_array(_idx_u32(width))
	out.append_array(_idx_u32(height))
	out.append_array(_idx_u32(pixel_format))
	out.append_array(_idx_u32(flags))
	out.append_array(_idx_u16(dstex_version))
	out.append_array(_idx_u16(mip_count))
	out.append_array(_idx_u64(original_size))
	out.append_array(_idx_u64(compressed_size))
	out.append_array(_idx_u64(mtime))
	out.append_array(_idx_u64(file_len))
	for i in range(COMBINED_INDEX_MIP_SLOTS):
		var off := 0
		if i < mip_count:
			off = int(mip_offsets[i])
		out.append_array(_idx_i64(off))
	for i in range(COMBINED_INDEX_MIP_SLOTS):
		var sz := 0
		if i < mip_count:
			sz = int(mip_sizes[i])
		out.append_array(_idx_i32(sz))
	for i in range(COMBINED_INDEX_MIP_SLOTS):
		var uncomp := 0
		if i < mip_count:
			uncomp = int(mip_uncompressed[i])
		out.append_array(_idx_i32(uncomp))
	for i in range(COMBINED_INDEX_MIP_SLOTS):
		var pitch := 0
		if i < mip_count:
			pitch = int(row_pitches[i])
		out.append_array(_idx_i32(pitch))
	return out

## IEEE CRC32 (poly 0xEDB88320, init/xorout 0xFFFFFFFF), bitwise to mirror
## DSPreloader::crc32_ieee exactly.
static func _idx_crc32_ieee(data: PackedByteArray) -> int:
	var crc: int = 0xFFFFFFFF
	for b in data:
		crc ^= b
		for i in range(8):
			if (crc & 1) != 0:
				crc = (0xEDB88320 ^ (crc >> 1)) & 0xFFFFFFFF
			else:
				crc = (crc >> 1) & 0xFFFFFFFF
	return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF

static func _idx_u16(v: int) -> PackedByteArray:
	return PackedByteArray([v & 0xFF, (v >> 8) & 0xFF])

static func _idx_u32(v: int) -> PackedByteArray:
	return PackedByteArray([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])

static func _idx_u64(v: int) -> PackedByteArray:
	return PackedByteArray([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF, (v >> 32) & 0xFF, (v >> 40) & 0xFF, (v >> 48) & 0xFF, (v >> 56) & 0xFF])

static func _idx_i32(v: int) -> PackedByteArray:
	return _idx_u32(v & 0xFFFFFFFF)

static func _idx_i64(v: int) -> PackedByteArray:
	return _idx_u64(v)
