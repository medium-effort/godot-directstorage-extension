@tool
extends EditorExportPlugin

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: Bundles imported .dstex texture files into one .dspkg archive
## during project export. Native GDExtension binaries (dsgd_64.dll /
## dsgd_64.debug.dll plus dstorage.dll / dstoragecore.dll) are placed flat
## on the export root automatically by Godot's built-in GDExtension exporter
## via the .gdextension [libraries] / [dependencies] mappings — no manual
## DLL relocation is performed by this plugin.

const OPTION_BUNDLE_DSPKG: StringName = &"directstorage/bundle_textures_dspkg"
const OPTION_ENFORCE_ARCHIVE_PAIRING: StringName = &"directstorage/enforce_archive_pairing"
const OPTION_STEAMPIPE_1MB_ALIGNMENT: StringName = &"directstorage/steampipe_1mb_alignment"
const OPTION_ENCRYPTION_ENABLED: StringName = &"directstorage/encryption_enabled"
const OPTION_ENCRYPTION_KEY: StringName = &"directstorage/encryption_key"

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
var _archive_failed: bool = false
var _target_export_dir: String = ""
var _export_features: PackedStringArray = []
var _is_debug_export: bool = false
var _bundle_dspkg_enabled: bool = false
var _enforce_pairing_enabled: bool = true
var _steampipe_1mb_alignment_enabled: bool = false
var _encryption_enabled: bool = false
var _current_fingerprint: int = 0
var _archive_name: String = "textures.dspkg"
## Manifest accumulator: dstex/archive entry path -> uncompressed size.
## Deduplicated by path; hashed sorted (FNV-1a 32-bit) in _export_end so the
## manifest hash is order-independent and code-only patches re-ship nothing.
var _manifest_entries: Dictionary = {}

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
		},
		{
			"option": {
				"name": OPTION_ENFORCE_ARCHIVE_PAIRING,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": true,
		},
		{
			"option": {
				"name": OPTION_STEAMPIPE_1MB_ALIGNMENT,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": false,
		},
		{
			"option": {
				"name": OPTION_ENCRYPTION_ENABLED,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": false,
		},
		{
			"option": {
				"name": OPTION_ENCRYPTION_KEY,
				"type": TYPE_STRING,
				"hint": PROPERTY_HINT_PASSWORD,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SECRET
			},
			"default_value": "",
		}
	]

## Starts a temporary DSPkg archive for the current export if bundling is enabled.
func _export_begin(features: PackedStringArray, is_debug: bool, path: String, flags: int) -> void:
	print("[DirectStorage Exporter] Export started for target: ", path)
	var resolved_export_path := path.strip_edges().simplify_path()
	if resolved_export_path.is_empty():
		_target_export_dir = ""
		push_error("[DirectStorage Exporter] Export path is empty; archive bundling will be skipped.")
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
	_enforce_pairing_enabled = bool(get_option(OPTION_ENFORCE_ARCHIVE_PAIRING))
	_steampipe_1mb_alignment_enabled = bool(get_option(OPTION_STEAMPIPE_1MB_ALIGNMENT))
	_encryption_enabled = bool(get_option(OPTION_ENCRYPTION_ENABLED))
	_has_textures = false
	_archive_failed = false
	_dspkg = null
	_staging_dspkg_path = ""
	_target_dspkg_path = ""

	_archive_name = String(ProjectSettings.get_setting("directstorage/archive_name", "textures.dspkg"))
	if _archive_name.is_empty():
		_archive_name = "textures.dspkg"

	# Ensure stable project GUID persisted to project.godot
	_ensure_project_guid()
	_manifest_entries.clear()
	_current_fingerprint = 0

	if _bundle_dspkg_enabled:
		_precompute_manifest()
		if _enforce_pairing_enabled and _has_textures:
			var guid := _ensure_project_guid()
			var manifest := _compute_manifest_hash()
			_current_fingerprint = (guid << 32) | manifest

	ProjectSettings.set_setting("directstorage/export/bundle_dspkg", _bundle_dspkg_enabled and _has_textures)
	ProjectSettings.set_as_internal("directstorage/export/bundle_dspkg", true)
	ProjectSettings.set_setting("directstorage/expected_archive_fingerprint", _current_fingerprint)
	ProjectSettings.set_as_internal("directstorage/expected_archive_fingerprint", true)
	ProjectSettings.set_setting("directstorage/archive_name", _archive_name)
	ProjectSettings.set_as_internal("directstorage/archive_name", true)

	if not _bundle_dspkg_enabled and _encryption_enabled:
		push_warning("[DirectStorage Exporter] Encryption is enabled but DirectStorage package bundling is disabled. Package will not be generated.")
	if not _bundle_dspkg_enabled:
		print("[DirectStorage Exporter] Standalone DSPkg bundling is disabled. Textures (.dstex) will be packaged directly into the PCK and loaded via CPU decompression at runtime.")
		return

	print("[DirectStorage Exporter] Standalone DSPkg bundling is ENABLED. Target archive: ", _archive_name, " (pairing fingerprint: ", String.num_uint64(_current_fingerprint, 16), ")")

	if not ClassDB.class_exists("DSPkg"):
		push_warning("[DirectStorage Exporter] DSPkg class not found. Skipping archive bundling.")
		return

	if _target_export_dir.is_empty():
		push_error("[DirectStorage Exporter] FATAL: no export directory resolved; " + _archive_name + " cannot be written. The export is incomplete.")
		_archive_failed = true
		return

	_target_dspkg_path = _target_export_dir.path_join(_archive_name)
	_staging_dspkg_path = _target_export_dir.path_join(_archive_name + ".tmp")
	_warn_if_low_disk_space(_target_export_dir)
	_cleanup_staging()
	_cleanup_legacy_user_temp()
	_dspkg = ClassDB.instantiate("DSPkg")
	if _dspkg != null:
		var err: Error = _dspkg.create(_staging_dspkg_path, _current_fingerprint)
		if err != OK:
			push_error("[DirectStorage Exporter] Failed to create DSPkg staging archive at: " + _staging_dspkg_path + ": " + error_string(err))
			_dspkg = null
			_archive_failed = true
		else:
			if _steampipe_1mb_alignment_enabled:
				if _dspkg.has_method("set_payload_alignment"):
					_dspkg.set_payload_alignment(1048576)
					if _dspkg.has_method("set_bin_packing_enabled"):
						_dspkg.set_bin_packing_enabled(true)
						print("[DirectStorage Exporter] SteamPipe 1MB payload bin-packing is ENABLED.")
					else:
						push_warning("[DirectStorage Exporter] Active DSPkg binary does not support 'set_bin_packing_enabled'; archive will fall back to strict 1MB alignment.")
				else:
					push_warning("[DirectStorage Exporter] Active DSPkg binary does not support 'set_payload_alignment'; archive will use default 512-byte alignment.")
			print("[DirectStorage Exporter] Initialized DSPkg staging archive at: ", _staging_dspkg_path)

			if _encryption_enabled:
				if not _dspkg.has_method("set_encryption_key") or not ClassDB.class_has_method("DSPkg", "compute_key_id") or not ClassDB.class_has_method("DSPkg", "get_global_encryption_key_id"):
					push_error("[DirectStorage Exporter] Loaded DirectStorage extension binary does not support DSPkg TOC encryption. Recompile extension.")
					_dspkg = null
					_archive_failed = true
					_cleanup_staging()
					return

				var key_hex: String = String(get_option(OPTION_ENCRYPTION_KEY)).strip_edges()
				if key_hex.is_empty():
					key_hex = OS.get_environment("DSPKG_EXPORT_ENCRYPTION_KEY").strip_edges()
				if key_hex.is_empty():
					key_hex = OS.get_environment("DSPKG_ENCRYPTION_KEY").strip_edges()

				if key_hex.length() != 64 or not key_hex.is_valid_hex_number(false):
					push_error("[DirectStorage Exporter] Encryption key must be a valid 64-character hexadecimal string.")
					_dspkg = null
					_archive_failed = true
					_cleanup_staging()
					return

				var entered_id: String = DSPkg.compute_key_id(key_hex)
				var active_id: String = DSPkg.get_global_encryption_key_id()
				if not active_id.is_empty() and entered_id != active_id:
					push_error("[DirectStorage Exporter] Encryption key mismatch! Entered export key ID (" + entered_id + ") differs from active runtime key ID (" + active_id + ") in editor build.")
					_dspkg = null
					_archive_failed = true
					_cleanup_staging()
					return
				elif active_id.is_empty():
					print("[DirectStorage Exporter] Notice: No active runtime key detected in editor build. Ensure runtime sets key via set_global_encryption_key() or baked template.")

				err = _dspkg.set_encryption_key(key_hex)
				if err != OK:
					push_error("[DirectStorage Exporter] Failed to configure encryption key: " + error_string(err))
					_dspkg = null
					_archive_failed = true
					_cleanup_staging()
					return

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
	# Standalone archives must never be embedded in the PCK (breaks Win32
	# DirectStorage DMA). Exclude any .dspkg in the project tree regardless
	# of the bundling option.
	if path.ends_with(".dspkg"):
		skip()
		print("[DirectStorage Exporter] Excluded standalone archive from PCK: ", path)
		return

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
		_track_manifest_entry(resource_path, dstex_path)
		if dstex_path != resource_path:
			_track_manifest_entry(dstex_path, dstex_path)
		print("[DirectStorage Exporter] Added texture to archive: ", resource_path, " (from ", dstex_path, ")")
		# Skip texture payload from PCK so it is only stored in textures.dspkg.
		skip()
		print("[DirectStorage Exporter] Skipped texture payload from PCK: ", resource_path)

## Finalizes the archive, renames textures.dspkg.tmp to textures.dspkg
## (if enabled). A DSPkg failure is fatal for this plugin's outputs:
## partial archives are removed so a broken build is never silently shipped.
## NOTE: Godot does not
## let an EditorExportPlugin abort the core PCK/exe steps from _export_end(),
## so push_error() here marks the export as failed in the log but the
## remaining core steps still run outside this plugin's control. Native DLLs
## (dsgd_64, dstorage) are exported flat to the export root by Godot itself.
func _export_end() -> void:
	var dspkg_ok := true
	if _archive_failed:
		dspkg_ok = false
		push_error("[DirectStorage Exporter] FATAL: export finished without a textures.dspkg archive because bundling was aborted after an earlier failure. Partial archives were removed. See earlier errors for the cause (a likely candidate is out of disk space).")
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
				push_error("[DirectStorage Exporter] FATAL: export directory was lost before finalize; " + _archive_name + " was not deployed.")
			elif not FileAccess.file_exists(_staging_dspkg_path):
				dspkg_ok = false
				push_error("[DirectStorage Exporter] FATAL: finalized staging archive is missing at '" + _staging_dspkg_path + "'; " + _archive_name + " was not deployed.")
			else:
				_remove_file_silent(_target_dspkg_path)
				var rename_err := DirAccess.rename_absolute(_staging_dspkg_path, _target_dspkg_path)
				if rename_err == OK:
					print("[DirectStorage Exporter] Successfully deployed standalone textures archive to: ", _target_dspkg_path)
				else:
					dspkg_ok = false
					_report_storage_failure("rename staging archive to " + _target_dspkg_path.get_file(), rename_err, _target_dspkg_path)
					_cleanup_staging()
					_remove_file_silent(_target_dspkg_path)
		else:
			print("[DirectStorage Exporter] Export finished. No DirectStorage (.dstex) textures were found to bundle.")

	_dspkg = null
	_cleanup_staging()
	_cleanup_legacy_user_temp()

	if not dspkg_ok:
		# Do not leave a missing/broken textures.dspkg unreported.
		return

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
	detail += " Partial archives were removed."
	push_error(detail)

func _compute_project_guid() -> int:
	var custom_guid = ProjectSettings.get_setting("directstorage/project_guid", 0)
	if typeof(custom_guid) == TYPE_INT and int(custom_guid) != 0:
		return int(custom_guid) & 0xFFFFFFFF
	# Back-compat fallback for projects that never got a persisted GUID:
	# hash the project name (stable for the same project, unlike random).
	var project_name: String = ProjectSettings.get_setting("application/config/name", "game")
	var h: int = 0x811c9dc5
	for b in project_name.to_utf8_buffer():
		h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
	if h == 0:
		h = 1
	return h

## Generates and persists directstorage/project_guid once per project so the
## upper 32 bits of the fingerprint are stable across exports (cross-game
## collision protection). Existing projects keep their fallback hash until
## this runs once; afterwards the stored value wins.
func _ensure_project_guid() -> int:
	var custom_guid = ProjectSettings.get_setting("directstorage/project_guid", 0)
	if typeof(custom_guid) == TYPE_INT and int(custom_guid) != 0:
		ProjectSettings.set_as_internal("directstorage/project_guid", true)
		return int(custom_guid) & 0xFFFFFFFF
	var guid := randi() & 0xFFFFFFFF
	if guid == 0:
		guid = 1
	ProjectSettings.set_setting("directstorage/project_guid", guid)
	ProjectSettings.set_as_internal("directstorage/project_guid", true)
	var save_err := ProjectSettings.save()
	if save_err != OK:
		push_warning("[DirectStorage Exporter] Could not persist directstorage/project_guid to project.godot: " + error_string(save_err))
	return guid

## Records one bundled entry for the manifest hash (deduplicated by path).
## Uses the uncompressed size per plan; falls back to reading the dstex
## header when the size is unknown (0 passed by alias entries).
func _track_manifest_entry(entry_path: String, dstex_path: String, original_size: int = 0) -> void:
	if entry_path.is_empty():
		return
	if _manifest_entries.has(entry_path):
		return
	var size_val := original_size
	if size_val <= 0 and ClassDB.class_exists("DSTex") and FileAccess.file_exists(dstex_path):
		var probe: RefCounted = ClassDB.instantiate("DSTex")
		if probe != null and probe.has_method("read_header"):
			var header: Dictionary = probe.call("read_header", dstex_path)
			size_val = int(header.get("original_size", 0))
	_manifest_entries[entry_path] = maxi(size_val, 0)

## FNV-1a 32-bit over sorted (path, uncompressed size) pairs. Sorted so the
## hash is independent of export-callback order; code-only patches produce
## the identical hash and reuse the shipped archive.
func _compute_manifest_hash() -> int:
	var keys := _manifest_entries.keys()
	keys.sort()
	var h: int = 0x811c9dc5
	for k in keys:
		var path_str := String(k)
		for b in path_str.to_utf8_buffer():
			h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
		h ^= 0xFF
		h = (h * 0x01000193) & 0xFFFFFFFF
		var size_val := int(_manifest_entries[k])
		for i in range(4):
			h = ((h ^ ((size_val >> (i * 8)) & 0xFF)) * 0x01000193) & 0xFFFFFFFF
	var manifest := h & 0xFFFFFFFF
	if manifest == 0:
		manifest = 1 # 0 means unpaired/legacy; paired builds must not alias it.
	return manifest

## Recursively crawls res:// for all textures that will be bundled into .dspkg
## (either raw .dstex or source images with a directstorage_texture remap).
## Computes the manifest entries ahead of time so the real fingerprint is known
## in _export_begin() and baked directly into the PCK project settings.
func _precompute_manifest() -> void:
	_manifest_entries.clear()
	_scan_dir_for_manifest("res://")
	_has_textures = not _manifest_entries.is_empty()

func _scan_dir_for_manifest(dir_path: String) -> void:
	var dir := DirAccess.open(dir_path)
	if dir == null:
		return
	dir.include_hidden = false
	dir.include_navigational = false
	if dir.list_dir_begin() != OK:
		return
	var file_name := dir.get_next()
	while not file_name.is_empty():
		if dir.current_is_dir():
			if not file_name.begins_with(".") and file_name != "addons":
				_scan_dir_for_manifest(dir_path.path_join(file_name))
			elif file_name == "addons":
				var sub_dir_path := dir_path.path_join(file_name)
				var sub_dir := DirAccess.open(sub_dir_path)
				if sub_dir != null and sub_dir.list_dir_begin() == OK:
					var sub_name := sub_dir.get_next()
					while not sub_name.is_empty():
						if sub_dir.current_is_dir() and sub_name != "directstorage_accelerator" and sub_name != "godot_ai" and not sub_name.begins_with("."):
							_scan_dir_for_manifest(sub_dir_path.path_join(sub_name))
						sub_name = sub_dir.get_next()
					sub_dir.list_dir_end()
		else:
			var full_path := dir_path.path_join(file_name)
			if file_name.ends_with(".dstex"):
				_track_manifest_entry(full_path, full_path)
			elif file_name.ends_with(".import"):
				var config := ConfigFile.new()
				if config.load(full_path) == OK:
					var importer: String = config.get_value("remap", "importer", "")
					var remap_path: String = config.get_value("remap", "path", "")
					if importer == "directstorage_texture" or remap_path.ends_with(".dstex"):
						var res_path := full_path.trim_suffix(".import")
						_track_manifest_entry(res_path, remap_path)
						if remap_path != res_path:
							_track_manifest_entry(remap_path, remap_path)
		file_name = dir.get_next()
	dir.list_dir_end()
