@tool
extends EditorExportPlugin

## Uid of the shipping-rules script (ds_export_policy.gd), shared with
## project/headless_tests/test_export_policy.gd.
##
## Resolved at instantiation instead of a `const Policy = preload(...)`: a cold first
## import compiles scripts in directory order, and this file is compiled before
## ds_export_policy.gd has registered its uid, so a compile-time uid preload fails on a
## freshly copied project (#0187). A runtime load happens after the scan, when the uid is
## known. Kept as a uid, not a res:// path, so renaming the addon directory cannot break it.
const POLICY_UID: String = "uid://bpbf4g8bgjeec"
const VERIFY_UID: String = "uid://bjyun30ppok5g"


static func _load_policy() -> GDScript:
	var script: GDScript = ResourceLoader.load(POLICY_UID, "GDScript") as GDScript
	if script == null:
		push_error("[DirectStorage] Export policy script not found (%s)." % POLICY_UID)
	return script


static func _load_verifier() -> GDScript:
	var script: GDScript = ResourceLoader.load(VERIFY_UID, "GDScript") as GDScript
	if script == null:
		push_error("[DirectStorage] Pack verifier script not found (%s)." % VERIFY_UID)
	return script


## Pure shipping rules, resolved once per plugin instance.
var _policy: GDScript = _load_policy()

## DirectStorage Export Plugin
##
## Author: MediumEffort
## Description: Bundles imported .dstex texture files into one .dspkg archive
## during project export. Native GDExtension binaries (dsgd_64.dll /
## dsgd_64.debug.dll plus dstorage.dll / dstoragecore.dll) are placed flat
## on the export root automatically by Godot's built-in GDExtension exporter
## via the .gdextension [libraries] / [dependencies] mappings — no manual
## DLL relocation is performed by this plugin.

const OPTION_BUNDLE_DSPKG: StringName = &"dsgd_extension/bundle_textures_dspkg"
const OPTION_VERIFY_PACK: StringName = &"dsgd_extension/verify_pack_on_export"
const OPTION_ENFORCE_ARCHIVE_PAIRING: StringName = &"dsgd_extension/enforce_archive_pairing"
const OPTION_STEAMPIPE_1MB_ALIGNMENT: StringName = &"dsgd_extension/steampipe_1mb_alignment"
const OPTION_STORE_UNCOMPRESSED_TEXTURES: StringName = &"dsgd_extension/store_uncompressed_textures"
const OPTION_INCLUDE_DSGD_LICENSE_FILE: StringName = &"dsgd_extension/include_dsgd_license_file"
const OPTION_INCLUDE_DIRECTSTORAGE_LICENSE_FILE: StringName = &"dsgd_extension/include_directstorage_license_file"
const OPTION_MERGE_LICENSE_FILES: StringName = &"dsgd_extension/merge_license_files"
## Retired key. Read only as a fallback when a preset was never migrated.
const OPTION_LEGACY_INCLUDE_THIRD_PARTY_NOTICES: StringName = &"dsgd_extension/include_third_party_notices"

## Binary distributions ship these files next to the executable. The DSGD file
## is the package-level bundle (project license + third-party components); the
## DirectStorage file carries Microsoft's runtime license and notices.
const ADDON_LICENSE_SOURCE: String = "res://addons/directstorage_accelerator/LICENSE"
const THIRD_PARTY_NOTICES_SOURCE: String = "res://addons/directstorage_accelerator/THIRD_PARTY_NOTICES.txt"
const DSGD_LICENSE_EXPORT_NAME: String = "dsgd_LICENSE.txt"
const DIRECTSTORAGE_LICENSE_EXPORT_NAME: String = "DIRECTSTORAGE_LICENSE.txt"
## Retired name from before the DSGD license file existed. Removed on the first
## export after an upgrade so the directory never carries two notice files.
const LEGACY_THIRD_PARTY_NOTICES_EXPORT_NAME: String = "dsgd_THIRD_PARTY_NOTICES.txt"

## Runtime texts live beside the installed runtime. The installer writes the
## exact package LICENSE.txt / NOTICES.txt accepted by the user.
const ADDON_RUNTIME_BIN: String = "res://addons/directstorage_accelerator/bin"
const RUNTIME_ACTIVE_PATH: String = ADDON_RUNTIME_BIN + "/.runtime.json"
const RUNTIME_VERSIONS_DIR: String = ADDON_RUNTIME_BIN + "/.versions"
const RUNTIME_LICENSE_FILE: String = "LICENSE.txt"
const RUNTIME_NOTICES_FILE: String = "NOTICES.txt"
const NOTICE_SEPARATOR: String = "=============================================================================="

## The archive key is delivered inside the encrypted PCK at this virtual path.
## The file never exists on disk: it is a 48-byte add_file() entry only.
const KEY_FILE_PATH: String = "res://.dspkg/toc.key"
const ARCHIVE_KEY_SIZE: int = 32
## The first 16 bytes of the key file are a recognition tag derived from this
## label. The scanner searches for the derived bytes, so neither the bytes nor
## their hex form may appear in this script, which itself ships inside the PCK.
const KEY_MAGIC_LABEL: String = "dspkg-toc-key-magic-v1"

## Runtime bridge to the native static bindings. It is loaded only after
## ClassDB reports the DSPkg class, so no direct native call is made at parse
## time and a missing binary cannot stop the plugin from registering.
const DSPKG_STATIC_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_dspkg_static.gd"

var _dspkg: RefCounted = null
var _dspkg_static: GDScript = null
var _dspkg_static_unavailable: bool = false
## Staging/final archive paths. Both live in the target export directory so no
## temporary data is written to C: (user://). finalize() writes the staging
## file once; it is then renamed to the final name (metadata-only, no extra
## disk space needed for a copy).
var _staging_dspkg_path: String = ""
var _target_dspkg_path: String = ""
## Legacy temp location from before direct-to-export writes. Only used to
## clean up stale artifacts on C:.
const LEGACY_TEMP_DSPKG_PATH: String = "user://temp_textures.dspkg"
## Archive entry paths successfully added in this export.
var _archive_entries: Dictionary = {}
## Payloads removed from the PCK. Any skip means the game can only load that
## texture from the archive, so this - not the add count - decides the contract.
var _skipped_payloads: int = 0
## Monotonic expectation flag: false at export start, true from the first
## payload that leaves the PCK, never demoted. A texture that exists only in
## the archive must keep the runtime looking for one even if a later step fails.
var _expect_archive: bool = false
## Set when this export cannot deliver a usable archive for any reason.
var _archive_failed: bool = false
## One of the four _policy.OUTCOME_* values, decided once per export from the
## active preset. Never cached across exports: "Export All" reuses one instance.
var _outcome: String = _policy.OUTCOME_DISABLED
## First failing condition when the outcome is OUTCOME_FAILED, for the error
## message only.
var _protection_failure_reason: String = ""
var _target_export_dir: String = ""
## Absolute path of the exported executable. Used only to size the pre-flight
## disk-space estimate from the previous export's artifact set.
var _target_export_path: String = ""
## platform.get_message_count() at _export_begin. _export_end considers only
## messages added after this index, so an accumulated list (CLI exports, or one
## plugin instance serving several presets) cannot inherit an earlier error.
var _platform_messages_at_begin: int = 0
var _export_features: PackedStringArray = []
var _is_debug_export: bool = false
var _is_windows_export: bool = true
var _bundle_dspkg_enabled: bool = false
var _verify_pack_enabled: bool = true
var _pack_failure_detail: String = ""
var _enforce_pairing_enabled: bool = true
var _steampipe_1mb_alignment_enabled: bool = false
var _store_uncompressed_textures: bool = false
var _include_dsgd_license_file: bool = true
var _include_directstorage_license_file: bool = true
var _merge_license_files: bool = true
var _archive_key: PackedByteArray = PackedByteArray()
var _archive_id: int = 0
var _archive_name: String = "textures.dspkg"

## Returns the stable export plugin name shown to Godot.
func _get_name() -> String:
	return "dsgd Extension"

## Returns whether the export platform targets Windows.
static func _is_windows_platform(platform: EditorExportPlatform) -> bool:
	if platform is EditorExportPlatformWindows:
		return true
	return platform != null and platform.get_os_name() == "Windows"

## Declares whether this export plugin supports the target platform.
func _supports_platform(platform: EditorExportPlatform) -> bool:
	if _is_windows_platform(platform):
		return true
	if platform != null and platform.get_os_name() in ["Linux", "macOS"]:
		return true
	return false

## Exposes custom export options for supported desktop platforms in the Export dialog.
func _get_export_options(platform: EditorExportPlatform) -> Array[Dictionary]:
	if not _supports_platform(platform):
		return []
	var options: Array[Dictionary] = [
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
				"name": OPTION_VERIFY_PACK,
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
				"name": OPTION_STORE_UNCOMPRESSED_TEXTURES,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": not _is_windows_platform(platform),
		},
		{
			"option": {
				"name": OPTION_INCLUDE_DSGD_LICENSE_FILE,
				"type": TYPE_BOOL,
				"hint": PROPERTY_HINT_NONE,
				"hint_string": "",
				"usage": PROPERTY_USAGE_DEFAULT
			},
			"default_value": true,
		},
	]
	if _is_windows_platform(platform):
		options.append_array([
			{
				"option": {
					"name": OPTION_INCLUDE_DIRECTSTORAGE_LICENSE_FILE,
					"type": TYPE_BOOL,
					"hint": PROPERTY_HINT_NONE,
					"hint_string": "",
					"usage": PROPERTY_USAGE_DEFAULT
				},
				"default_value": true,
			},
			{
				"option": {
					"name": OPTION_MERGE_LICENSE_FILES,
					"type": TYPE_BOOL,
					"hint": PROPERTY_HINT_NONE,
					"hint_string": "",
					"usage": PROPERTY_USAGE_DEFAULT
				},
				"default_value": true,
			}
		])
	return options

## Export options have no tooltip field, so the warning text is the only place
## the dialog can explain an interaction. Keep it to combinations that actually
## change the result.
func _get_export_option_warning(platform: EditorExportPlatform, option: String) -> String:
	if not _is_windows_platform(platform):
		return ""
	var include_dsgd := _option_bool(OPTION_INCLUDE_DSGD_LICENSE_FILE, true)
	var include_runtime := _option_bool(OPTION_INCLUDE_DIRECTSTORAGE_LICENSE_FILE, true)
	if option == String(OPTION_MERGE_LICENSE_FILES):
		if _option_bool(OPTION_MERGE_LICENSE_FILES, true) and not (include_dsgd and include_runtime):
			return "Merge has no effect unless both license files are included."
	elif option == String(OPTION_INCLUDE_DIRECTSTORAGE_LICENSE_FILE):
		if include_runtime and _resolve_directstorage_texts(false).is_empty():
			if _has_runtime_dlls():
				return "No DirectStorage runtime license texts were found, even though DirectStorage DLLs are present. Reinstall the runtime through Tools > DSGD Tools... > Manage DirectStorage Runtime."
			return "No DirectStorage runtime license texts were found. Install a runtime through Tools > DSGD Tools... > Manage DirectStorage Runtime."
	return ""

## Starts the archive for the current export and decides the protection outcome.
func _export_begin(features: PackedStringArray, is_debug: bool, path: String, flags: int) -> void:
	print("[DirectStorage Exporter] Export started for target: ", path)
	# Snapshot the platform message count now: the engine's own failure
	# messages (template copy, PCK write) are added to this list before
	# _export_end runs, and only those must count as this run's errors.
	var platform: EditorExportPlatform = get_export_platform()
	_platform_messages_at_begin = platform.get_message_count() if platform != null and platform.has_method("get_message_count") else 0
	var resolved_export_path := path.strip_edges().simplify_path()
	if resolved_export_path.is_empty():
		_target_export_dir = ""
		_target_export_path = ""
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
			_target_export_path = ""
		else:
			var target_dir_err := DirAccess.make_dir_recursive_absolute(_target_export_dir)
			if target_dir_err != OK and not DirAccess.dir_exists_absolute(_target_export_dir):
				push_error("[DirectStorage Exporter] Failed to prepare export directory '" + _target_export_dir + "': " + error_string(target_dir_err) + ". Verify the export path is writable.")
				_target_export_path = ""
			else:
				_target_export_path = resolved_export_path
				print("[DirectStorage Exporter] Resolved export directory: ", _target_export_dir)

	_export_features = features
	_is_debug_export = is_debug
	_is_windows_export = _is_windows_platform(platform)
	_bundle_dspkg_enabled = bool(get_option(OPTION_BUNDLE_DSPKG))
	_verify_pack_enabled = _option_bool(OPTION_VERIFY_PACK, true)
	_enforce_pairing_enabled = bool(get_option(OPTION_ENFORCE_ARCHIVE_PAIRING))
	_steampipe_1mb_alignment_enabled = bool(get_option(OPTION_STEAMPIPE_1MB_ALIGNMENT))
	_store_uncompressed_textures = _option_bool(
		OPTION_STORE_UNCOMPRESSED_TEXTURES,
		not _is_windows_export
	)
	# A preset created before an option existed has no key and returns nil;
	# treat that as the default instead of silently disabling an output.
	_include_dsgd_license_file = _option_bool(OPTION_INCLUDE_DSGD_LICENSE_FILE, true)
	_include_directstorage_license_file = _option_bool(OPTION_INCLUDE_DIRECTSTORAGE_LICENSE_FILE, true) if _is_windows_export else false
	_merge_license_files = _option_bool(OPTION_MERGE_LICENSE_FILES, true) if _is_windows_export else false

	_archive_entries.clear()
	_skipped_payloads = 0
	_expect_archive = false
	_archive_failed = false
	_dspkg = null
	_dspkg_static = null
	_dspkg_static_unavailable = false
	_protection_failure_reason = ""
	_pack_failure_detail = ""
	_staging_dspkg_path = ""
	_target_dspkg_path = ""
	_archive_key = PackedByteArray()
	_archive_id = 0

	_archive_name = String(ProjectSettings.get_setting("dsgd_extension/archive_name", "textures.dspkg"))
	if _archive_name.is_empty():
		_archive_name = "textures.dspkg"

	_outcome = _resolve_outcome()
	if _outcome == _policy.OUTCOME_PROTECTED:
		_archive_key = _new_archive_key()
		_archive_id = _new_archive_id()
	elif _outcome == _policy.OUTCOME_PLAINTEXT:
		_archive_id = _new_archive_id()

	_publish_contract()

	if _outcome == _policy.OUTCOME_DISABLED:
		print("[DirectStorage Exporter] Standalone DSPkg bundling is disabled. Textures (.dstex) will be packaged directly into the PCK and loaded via CPU decompression at runtime.")
		return

	print("[DirectStorage Exporter] Standalone DSPkg bundling is ENABLED. Target archive: ", _archive_name, " (archive id: ", String.num_uint64(_archive_id, 16), ", mode: ", _outcome, ")")

	if not _target_export_dir.is_empty():
		_target_dspkg_path = _target_export_dir.path_join(_archive_name)
		_staging_dspkg_path = _target_export_dir.path_join(_archive_name + ".tmp")

	if _outcome == _policy.OUTCOME_FAILED:
		# Fail closed: no key file, no archive, and every texture payload stays
		# out of the PCK so a protected build cannot ship plaintext textures.
		_archive_failed = true
		push_error("[DirectStorage Exporter] Protection was requested but cannot be delivered: " + _protection_failure_reason + ". No archive will be written and texture payloads are removed from the PCK.")
		if not _target_dspkg_path.is_empty():
			_cleanup_staging()
			_remove_file_silent(_target_dspkg_path)
		return

	if _target_export_dir.is_empty():
		push_error("[DirectStorage Exporter] FATAL: no export directory resolved; " + _archive_name + " cannot be written. The export is incomplete.")
		_archive_failed = true
		return

	if not ClassDB.class_exists("DSPkg"):
		push_error("[DirectStorage Exporter] The DSPkg class is not available; " + _archive_name + " cannot be written.")
		_archive_failed = true
		return

	_warn_if_low_disk_space(_target_export_dir)
	_cleanup_staging()
	# Remove the previous archive now rather than just before the rename: the
	# peak footprint then excludes it, and an export that produces no archive
	# (or fails) can never leave a stale one next to a freshly written PCK.
	_remove_file_silent(_target_dspkg_path)
	_cleanup_legacy_user_temp()
	_dspkg = ClassDB.instantiate("DSPkg")
	if _dspkg == null:
		push_error("[DirectStorage Exporter] Failed to instantiate DSPkg; " + _archive_name + " cannot be written.")
		_archive_failed = true
		return

	var err: Error = _dspkg.create(_staging_dspkg_path, _archive_id)
	if err != OK:
		push_error("[DirectStorage Exporter] Failed to create DSPkg staging archive at: " + _staging_dspkg_path + ": " + error_string(err))
		_dspkg = null
		_archive_failed = true
		_cleanup_staging()
		return

	_apply_steampipe_alignment()
	if _dspkg.has_method("set_repack_uncompressed"):
		_dspkg.set_repack_uncompressed(_store_uncompressed_textures)
		if _store_uncompressed_textures:
			print("[DirectStorage Exporter] Export-time uncompressed texture repack is ENABLED.")
	elif _store_uncompressed_textures:
		push_warning("[DirectStorage Exporter] Active DSPkg binary does not support 'set_repack_uncompressed'; archive will keep source compression.")
	if _dspkg.has_method("set_payload_dedup_enabled"):
		_dspkg.set_payload_dedup_enabled(true)

	if _outcome == _policy.OUTCOME_PROTECTED:
		if not _apply_archive_key():
			_cleanup_staging()
			return
		# The key file is a PCK entry only; res://.dspkg/ is never created on disk.
		add_file(KEY_FILE_PATH, _build_key_file(_archive_key), false)

	print("[DirectStorage Exporter] Initialized DSPkg staging archive at: ", _staging_dspkg_path)

## Decides the protection outcome once, from the active preset.
##
## The preset is the only authority: the export dialog's encryption toggle, its
## key and its include/exclude filters describe what the PCK will actually
## encrypt. The add-on keeps no cached copy because one plugin instance serves
## every preset of an "Export All" run.
func _resolve_outcome() -> String:
	if not _bundle_dspkg_enabled:
		return _policy.OUTCOME_DISABLED

	var preset: EditorExportPreset = get_export_preset()
	var requested: bool = preset != null and preset.get_encrypt_pck()
	if not requested:
		return _policy.OUTCOME_PLAINTEXT

	# Read only for an emptiness test; the archive key is never derived from it.
	var key_present: bool = not String(preset.get_encryption_key()).strip_edges().is_empty()
	var key_encrypted: bool = _path_is_encrypted(KEY_FILE_PATH)
	if key_present and key_encrypted:
		return _policy.OUTCOME_PROTECTED

	if not key_present:
		_protection_failure_reason = "the preset has no PCK encryption key (set one under Encryption > 'Encryption Key')"
	elif not _filter_matches(preset.get_encryption_in_filter(), KEY_FILE_PATH):
		_protection_failure_reason = KEY_FILE_PATH + " is not encrypted by the preset (add '.dspkg/*' or '*' to Encryption > 'Filters to include files/folders')"
	else:
		_protection_failure_reason = KEY_FILE_PATH + " is not encrypted by the preset because it matches an exclude filter (remove it from Encryption > 'Filters to exclude files/folders')"
	return _policy.OUTCOME_FAILED

## True when the engine would encrypt this path with the active preset.
func _path_is_encrypted(path: String) -> bool:
	var preset: EditorExportPreset = get_export_preset()
	if preset == null:
		return false
	if not _filter_matches(preset.get_encryption_in_filter(), path):
		return false
	return not _filter_matches(preset.get_encryption_ex_filter(), path)

## Replicates the engine matcher: a comma-separated glob list, each entry
## stripped, matched against the full `res://` path and the path without the
## prefix. Exclude wins over include.
func _filter_matches(filter: String, path: String) -> bool:
	for raw_entry in filter.split(",", false):
		var entry := raw_entry.strip_edges()
		if entry.is_empty():
			continue
		if path.matchn(entry) or path.trim_prefix("res://").matchn(entry):
			return true
	return false

## 16-byte recognition tag for a key file, derived rather than stored.
func _key_magic16() -> PackedByteArray:
	var hashing := HashingContext.new()
	hashing.start(HashingContext.HASH_SHA256)
	hashing.update(KEY_MAGIC_LABEL.to_utf8_buffer())
	return hashing.finish().slice(0, 16)

## Random archive key: one value per export, never derived from the PCK key.
func _new_archive_key() -> PackedByteArray:
	return Crypto.new().generate_random_bytes(ARCHIVE_KEY_SIZE)

## Random 64-bit archive id, or 0 when pairing is disabled for local tooling.
## The top bit is cleared so the value stays positive in GDScript and survives
## a ProjectSettings round-trip.
func _new_archive_id() -> int:
	if not _enforce_pairing_enabled:
		return 0
	var bytes := Crypto.new().generate_random_bytes(8)
	var id := bytes.decode_u64(0) & 0x7FFFFFFFFFFFFFFF
	if id == 0:
		id = 1
	return id

## The injected key file: 16-byte magic followed by the 32-byte key.
func _build_key_file(key: PackedByteArray) -> PackedByteArray:
	var key_file := _key_magic16()
	key_file.append_array(key)
	return key_file

## Installs the generated key on the writer so the archive is built encrypted.
func _apply_archive_key() -> bool:
	if not _dspkg.has_method("set_encryption_key"):
		push_error("[DirectStorage Exporter] The loaded DSPkg binary does not support TOC encryption. Rebuild the extension.")
		_dspkg = null
		_archive_failed = true
		return false
	var err: Error = _dspkg.set_encryption_key(_archive_key.hex_encode())
	if err != OK:
		push_error("[DirectStorage Exporter] Failed to configure the archive encryption key: " + error_string(err))
		_dspkg = null
		_archive_failed = true
		return false
	return true

## Applies the optional SteamPipe payload layout before any entry is added.
func _apply_steampipe_alignment() -> void:
	if not _steampipe_1mb_alignment_enabled:
		return
	if not _dspkg.has_method("set_payload_alignment"):
		push_warning("[DirectStorage Exporter] Active DSPkg binary does not support 'set_payload_alignment'; archive will use default 512-byte alignment.")
		return
	_dspkg.set_payload_alignment(1048576)
	if _dspkg.has_method("set_bin_packing_enabled"):
		_dspkg.set_bin_packing_enabled(true)
		print("[DirectStorage Exporter] SteamPipe 1MB payload bin-packing is ENABLED.")
	else:
		push_warning("[DirectStorage Exporter] Active DSPkg binary does not support 'set_bin_packing_enabled'; archive will fall back to strict 1MB alignment.")

## Reads v2/v3 per-mip footprint data so DSPkg can preserve GPU upload layouts.
## v3 GDeflate-inline files share the v2 footprint layout. Chunked v3 files are
## staged with their chunk table into a v5 archive; only Zstd/GACL still has no
## archive representation, so those fall back to the header-only path.
func _read_v2_footprint_metadata(dstex_path: String) -> Dictionary:
	if not ClassDB.class_exists("DSTex"):
		return {}
	var dstex: RefCounted = ClassDB.instantiate("DSTex")
	if dstex == null:
		return {}
	var metadata: Dictionary = dstex.read_header(dstex_path)
	var version := int(metadata.get("version", 0))
	if version != 2 and version != 3:
		return {}
	if version == 3:
		# NONE (0) is a payload the v5 archive can stage, so only Zstd/GACL
		# (and any future codec) lacks an archive representation here.
		if int(metadata.get("compression_format", 1)) not in [0, 1] or int(metadata.get("gacl_transform", 0)) != 0:
			push_warning("[DirectStorage Exporter] v3 payload in '" + dstex_path + "' needs Zstd/GACL support archives do not have; bundling without GPU mip metadata.")
			return {}
	var mip_count := int(metadata.get("mip_count", 0))
	var mip_sizes: PackedInt32Array = metadata.get("mip_sizes", PackedInt32Array())
	var mip_uncompressed_sizes: PackedInt32Array = metadata.get("mip_uncompressed_sizes", PackedInt32Array())
	var mip_row_pitches: PackedInt32Array = metadata.get("mip_row_pitches", PackedInt32Array())
	if mip_count <= 0 or mip_sizes.size() != mip_count or mip_uncompressed_sizes.size() != mip_count or mip_row_pitches.size() != mip_count:
		push_warning("[DirectStorage Exporter] Invalid v2 footprint metadata in: " + dstex_path)
		return {}
	return metadata

## Single writer for the four values the shipped game reads from
## `project.binary` and the runtime sentinel reports.
func _publish_contract() -> void:
	ProjectSettings.set_setting("dsgd_extension/bundle_dspkg", _expect_archive)
	ProjectSettings.set_as_internal("dsgd_extension/bundle_dspkg", true)
	ProjectSettings.set_setting("dsgd_extension/archive_id", _archive_id)
	ProjectSettings.set_as_internal("dsgd_extension/archive_id", true)
	ProjectSettings.set_setting("dsgd_extension/protected", _outcome == _policy.OUTCOME_PROTECTED or _outcome == _policy.OUTCOME_FAILED)
	ProjectSettings.set_as_internal("dsgd_extension/protected", true)
	ProjectSettings.set_setting("dsgd_extension/archive_name", _archive_name)
	ProjectSettings.set_as_internal("dsgd_extension/archive_name", true)

## Marks the archive as failed and keeps the published contract honest.
##
## The partial archive is kept only when at least one texture was already
## skipped from the PCK: those bytes then exist nowhere else, so deleting it
## would lose them. With no entries yet, the staging file is removed and the
## deployed path is cleaned up in `_export_end`.
func _abort_archive(reason: String) -> void:
	_archive_failed = true
	push_error("[DirectStorage Exporter] " + reason)
	if _archive_entries.is_empty():
		_dspkg = null
		_cleanup_staging()

## Removes one texture payload from the PCK and promotes the contract on the
## first skip. Republishing here is still in time, because `project.binary` is
## written after the file walk.
func _skip_payload(path: String) -> void:
	_skipped_payloads += 1
	if not _expect_archive and _policy.resolve_bundle_effective(_bundle_dspkg_enabled, _skipped_payloads):
		_expect_archive = true
		_publish_contract()
	skip()

## Adds an imported .dstex resource to the archive when bundling is enabled.
func _export_file(path: String, type: String, features: PackedStringArray) -> void:
	# Standalone archives must never be embedded in the PCK (breaks Win32
	# DirectStorage DMA). Exclude any .dspkg in the project tree regardless
	# of the bundling option.
	if path.ends_with(".dspkg"):
		skip()
		print("[DirectStorage Exporter] Excluded standalone archive from PCK: ", path)
		return

	# Never skip .import files: the remap table must reach the PCK so
	# load("res://texture.png") can resolve to the archive entry.
	if path.ends_with(".import"):
		print("[DirectStorage Exporter] Preserving remap metadata in PCK: ", path)
		return

	var dstex_path := _resolve_dstex_path(path)
	if dstex_path.is_empty():
		return

	var action: String = _policy.resolve_file_action(_outcome, _archive_failed, _dspkg != null)
	if action == _policy.ACTION_PACK:
		if _store_uncompressed_textures and ClassDB.class_exists("DSTex"):
			var tex_repacker := ClassDB.instantiate("DSTex")
			if tex_repacker != null and tex_repacker.has_method("repack_uncompressed_to_buffer"):
				var repacked: PackedByteArray = tex_repacker.call("repack_uncompressed_to_buffer", dstex_path)
				if not repacked.is_empty():
					# The engine writes `<path>.import` after the plugin
					# callbacks, so skip() drops it, and remap=false drops the
					# `<path>.remap` alternative: the PCK would then hold the
					# payload with nothing pointing at it. Keep the mapping.
					add_file(dstex_path, repacked, dstex_path != path)
					skip()
					print("[DirectStorage Exporter] Repacked uncompressed .dstex into PCK: ", dstex_path)
					return
				push_warning("[DirectStorage Exporter] Failed to repack '" + dstex_path + "' uncompressed; keeping original .dstex in PCK.")
		# Intended CPU fallback: let Godot export .dstex directly into the PCK.
		return
	if action == _policy.ACTION_SKIP:
		# The archive is unavailable, so a protected build must not leave a
		# plaintext copy of the payload in the PCK.
		_skip_payload(path)
		return

	var mip_metadata := _read_v2_footprint_metadata(dstex_path)
	var add_err: Error = _dspkg.add_texture(path, dstex_path, mip_metadata)
	if add_err != OK:
		_abort_archive("Failed to add texture to archive ('" + path + "' and remap target '" + dstex_path + "'): " + error_string(add_err) + ". DSPkg bundling was aborted.")
		if _outcome != _policy.OUTCOME_PLAINTEXT:
			_skip_payload(path)
		return

	_archive_entries[path] = true

	if dstex_path != path:
		var remapped_add_err: Error = _dspkg.add_texture(dstex_path, dstex_path, mip_metadata)
		if remapped_add_err == OK:
			_archive_entries[dstex_path] = true
		else:
			# The primary entry is already in the archive, so the texture is
			# complete for the loader; only the remapped alias is missing.
			_abort_archive("Failed to add remapped texture entry '" + dstex_path + "': " + error_string(remapped_add_err) + ". DSPkg bundling was aborted.")

	print("[DirectStorage Exporter] Added texture to archive: ", path, " (from ", dstex_path, ")")
	# Skip the payload as soon as the primary entry exists, so a later alias
	# failure can never leave the same payload in both containers.
	_skip_payload(path)
	print("[DirectStorage Exporter] Skipped texture payload from PCK: ", path)

## Resolves the .dstex file a resource path stands for, or an empty string when
## the path is not a DirectStorage texture payload.
func _resolve_dstex_path(path: String) -> String:
	if path.ends_with(".dstex"):
		return path if FileAccess.file_exists(path) else ""
	var import_path := path + ".import"
	if not FileAccess.file_exists(import_path):
		return ""
	var config := ConfigFile.new()
	if config.load(import_path) != OK:
		return ""
	var importer: String = config.get_value("remap", "importer", "")
	var remap_path: String = config.get_value("remap", "path", "")
	if importer != "directstorage_texture" and not remap_path.ends_with(".dstex"):
		return ""
	return remap_path if FileAccess.file_exists(remap_path) else ""

## Finalizes the archive, renames textures.dspkg.tmp to textures.dspkg, and
## proves a protected archive opens with the key this export generated.
## A failure never leaves a stale archive from an earlier build behind: the
## archive is either removed, so the game fails loudly, or deployed as a
## partial archive when textures were already skipped and exist nowhere else.
## NOTE: Godot does not let an EditorExportPlugin abort the core PCK/exe steps
## from _export_end(), so push_error() here marks the export as failed in the
## log but the remaining core steps still run outside this plugin's control.
## Native DLLs (dsgd_64, dstorage) are exported flat by Godot itself.
func _export_end() -> void:
	_sync_notice_files()
	var dspkg_ok := true
	if _archive_failed:
		dspkg_ok = false
		if _policy.resolve_failure_action(_outcome, _archive_entries.size()) == _policy.ACTION_DELETE:
			# Nothing can be salvaged: a protected build must fail loudly, and
			# an unprotected build with no entries still has every texture in
			# the PCK. Never leave a loadable archive behind either way.
			push_error("[DirectStorage Exporter] FATAL: export finished without a usable " + _archive_name + " archive because bundling was aborted. No archive is deployed. See earlier errors for the cause (a likely candidate is out of disk space).")
			_dspkg = null
			_cleanup_staging()
			_remove_file_silent(_target_dspkg_path)
		else:
			# Textures were already skipped from the PCK, so those bytes exist
			# only in the partial archive. Deploy it rather than deleting data.
			push_warning("[DirectStorage Exporter] WARNING: bundling was aborted after " + str(_archive_entries.size()) + " texture(s) were added. Deploying a PARTIAL archive; the remaining textures stay in the PCK. Re-export to produce a complete archive.")

	if _core_export_reports_error() and _dspkg != null and not _archive_entries.is_empty():
		# The engine already recorded an export error, almost certainly in the
		# template copy or the PCK write. Deploying now would pair a fresh
		# archive (new archive id) with whatever exe/PCK survived, so fail
		# closed instead. The plugin cannot abort the engine's own steps; the
		# error added at the end is what marks the export failed.
		dspkg_ok = false
		push_error("[DirectStorage Exporter] FATAL: the export failed before " + _archive_name + " could be deployed (see the engine export errors above); the archive was not written.")
		_dspkg = null
		_cleanup_staging()
		_remove_file_silent(_target_dspkg_path)
	elif _dspkg != null and _outcome != _policy.OUTCOME_DISABLED and _outcome != _policy.OUTCOME_FAILED and not _archive_entries.is_empty():
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
				if _outcome == _policy.OUTCOME_PROTECTED:
					dspkg_ok = _verify_written_archive()
			else:
				dspkg_ok = false
				_report_storage_failure("rename staging archive to " + _target_dspkg_path.get_file(), rename_err, _target_dspkg_path)
				_cleanup_staging()
				_remove_file_silent(_target_dspkg_path)
	elif not _archive_failed and _dspkg != null and _outcome != _policy.OUTCOME_DISABLED and _outcome != _policy.OUTCOME_FAILED:
		print("[DirectStorage Exporter] Export finished. No DirectStorage (.dstex) textures were found to bundle.")

	var pack_ok := true
	if _outcome == _policy.OUTCOME_PROTECTED:
		pack_ok = _verify_pack_protection()
	_dspkg = null
	_cleanup_staging()
	_cleanup_legacy_user_temp()
	if not dspkg_ok:
		var detail := ""
		if _outcome == _policy.OUTCOME_FAILED and not _protection_failure_reason.is_empty():
			detail = "Protection was requested but cannot be delivered: %s; %s was not deployed." % [_protection_failure_reason, _archive_name]
		_mark_export_failed(detail)
		return
	if not pack_ok:
		_mark_export_failed(_pack_failure_detail)
		return


func _has_runtime_dlls() -> bool:
	return FileAccess.file_exists(ADDON_RUNTIME_BIN.path_join("dstorage.dll"))


## The runtime is redistributed under Microsoft's license. Whenever runtime DLLs
## are present without a DirectStorage license file written to the export
## (whether the option is disabled or the runtime texts could not be resolved),
## print a reminder: this is an informational obligation on the game developer.
func _notice_runtime_license(runtime_license_written: bool = false) -> void:
	if not _is_windows_export or runtime_license_written or not _has_runtime_dlls():
		return
	print("[DirectStorage Exporter] NOTICE: dstorage.dll and dstoragecore.dll are Microsoft DirectStorage redistributables governed by Microsoft's license. Microsoft's own NuGet build targets copy only the DLLs; the license texts travel in the runtime package. You remain responsible for your distribution terms.")


## Reconciles the notice files in the export directory with this preset.
##
## Managed files are removed first, then the enabled set is written, so an
## option turned off (or a merge-mode switch) cannot leave a stale file behind.
## Missing sources and failed writes are warnings, not export failures: the
## binaries remain usable and a text file is not worth failing a build over.
func _sync_notice_files() -> void:
	if _target_export_dir.is_empty():
		if _include_dsgd_license_file or _include_directstorage_license_file:
			push_warning("[DirectStorage Exporter] License files are enabled but the export directory is unknown; no license file was deployed.")
		_notice_runtime_license(false)
		return

	_remove_managed_notice_files()

	var written: PackedStringArray = []
	var merged_runtime := false
	var runtime_license_written := false

	var dsgd_sections: Array[Dictionary] = []
	if _include_dsgd_license_file:
		dsgd_sections = _collect_dsgd_license_sections()

	var runtime_texts: Dictionary = {}
	if _include_directstorage_license_file:
		runtime_texts = _resolve_directstorage_texts()

	if _include_dsgd_license_file and not dsgd_sections.is_empty():
		var merge_runtime := _merge_license_files and not runtime_texts.is_empty()
		if merge_runtime:
			dsgd_sections.append_array(_collect_directstorage_sections(runtime_texts))
		var header := _notice_header("DSGD - license and notices", dsgd_sections)
		if _write_notice_file(_target_export_dir.path_join(DSGD_LICENSE_EXPORT_NAME), _compose_notice_file(header, dsgd_sections)):
			written.append(DSGD_LICENSE_EXPORT_NAME)
			merged_runtime = merge_runtime
			runtime_license_written = merged_runtime
	elif _include_dsgd_license_file:
		push_warning("[DirectStorage Exporter] The DSGD license file is enabled but neither '" + ADDON_LICENSE_SOURCE + "' nor '" + THIRD_PARTY_NOTICES_SOURCE + "' could be read. No file was written.")

	if _include_directstorage_license_file and not merged_runtime:
		if runtime_texts.is_empty():
			var action := "Reinstall the runtime" if _has_runtime_dlls() else "Install a runtime"
			push_warning("[DirectStorage Exporter] The DirectStorage license file is enabled but the runtime texts could not be resolved. " + action + " through Tools > DSGD Tools... > Manage DirectStorage Runtime, or disable dsgd_extension/include_directstorage_license_file.")
		else:
			var runtime_sections := _collect_directstorage_sections(runtime_texts)
			var header := _notice_header("DirectStorage - license and notices", runtime_sections)
			if _write_notice_file(_target_export_dir.path_join(DIRECTSTORAGE_LICENSE_EXPORT_NAME), _compose_notice_file(header, runtime_sections)):
				written.append(DIRECTSTORAGE_LICENSE_EXPORT_NAME)
				runtime_license_written = true

	if written.is_empty():
		print("[DirectStorage Exporter] Notices: none (managed license files removed).")
	elif merged_runtime:
		print("[DirectStorage Exporter] Notices: ", DSGD_LICENSE_EXPORT_NAME, " (DirectStorage runtime texts merged).")
	else:
		print("[DirectStorage Exporter] Notices: ", ", ".join(written), ".")

	_notice_runtime_license(runtime_license_written)


## Project license and the components inside dsgd_64.dll. Both describe the same
## artifact, so they are one file; a reader never needs one without the other.
func _collect_dsgd_license_sections() -> Array[Dictionary]:
	var sections: Array[Dictionary] = []
	var license_data := _read_notice_source(ADDON_LICENSE_SOURCE)
	if license_data.is_empty():
		push_warning("[DirectStorage Exporter] DSGD license text is missing at '" + ADDON_LICENSE_SOURCE + "'. Run a build to copy LICENSE from the repository root.")
	else:
		sections.append({"title": "DSGD license", "data": license_data})
	var notices_data := _read_notice_source(THIRD_PARTY_NOTICES_SOURCE)
	if notices_data.is_empty():
		push_warning("[DirectStorage Exporter] Third-party notices are missing at '" + THIRD_PARTY_NOTICES_SOURCE + "'. Run a build to copy it from the repository root.")
	else:
		sections.append({"title": "Third-party components of dsgd_64.dll", "data": notices_data})
	return sections


## Microsoft's LICENSE.txt and NOTICES.txt, in the package's own document order.
func _collect_directstorage_sections(runtime: Dictionary) -> Array[Dictionary]:
	var sections: Array[Dictionary] = []
	var version := String(runtime.get("version", "unknown"))
	var license_data := _read_notice_source(String(runtime.get("license_path", "")))
	if license_data.is_empty():
		push_warning("[DirectStorage Exporter] Could not read the DirectStorage runtime license text for version " + version + ".")
	else:
		sections.append({"title": "Microsoft DirectStorage runtime license (version %s)" % version, "data": license_data})
	var notices_data := _read_notice_source(String(runtime.get("notices_path", "")))
	if notices_data.is_empty():
		push_warning("[DirectStorage Exporter] Could not read the DirectStorage runtime notices for version " + version + ".")
	else:
		sections.append({"title": "Microsoft DirectStorage runtime third-party notices (version %s)" % version, "data": notices_data})
	return sections


## Resolves the texts for the runtime that is actually installed. The active
## version marker is authoritative; with no marker, a single version directory
## is unambiguous enough to use. Ambiguous installs are skipped rather than
## guessed, because shipping the wrong version's terms is worse than none.
func _resolve_directstorage_texts(warn_on_failure: bool = true) -> Dictionary:
	var active_version := _read_active_runtime_version()
	var candidates: PackedStringArray = []
	if not active_version.is_empty():
		candidates.append(RUNTIME_VERSIONS_DIR.path_join(active_version))

	var versions_dir := DirAccess.open(ProjectSettings.globalize_path(RUNTIME_VERSIONS_DIR))
	if versions_dir != null:
		for sub_dir in versions_dir.get_directories():
			var candidate := RUNTIME_VERSIONS_DIR.path_join(sub_dir)
			if not candidates.has(candidate):
				candidates.append(candidate)

	var found: Array[Dictionary] = []
	for candidate in candidates:
		var license_path := ProjectSettings.globalize_path(candidate.path_join(RUNTIME_LICENSE_FILE))
		var notices_path := ProjectSettings.globalize_path(candidate.path_join(RUNTIME_NOTICES_FILE))
		if FileAccess.file_exists(license_path) and FileAccess.file_exists(notices_path):
			found.append({
				"version": candidate.get_file(),
				"license_path": license_path,
				"notices_path": notices_path,
			})

	if found.is_empty():
		return {}
	if not active_version.is_empty():
		for entry in found:
			if String(entry["version"]) == active_version:
				return entry
		if warn_on_failure:
			push_warning("[DirectStorage Exporter] The active DirectStorage runtime '" + active_version + "' has no license texts at '" + RUNTIME_VERSIONS_DIR + "'. No runtime license file was written.")
		return {}
	if found.size() == 1:
		return found[0]
	if warn_on_failure:
		var names := PackedStringArray()
		for entry in found:
			names.append(String(entry["version"]))
		push_warning("[DirectStorage Exporter] Multiple DirectStorage runtime versions carry license texts (" + ", ".join(names) + ") and none is marked active. Set an active runtime through Manage DirectStorage Runtime; no runtime license file was written.")
	return {}


func _read_active_runtime_version() -> String:
	var path := ProjectSettings.globalize_path(RUNTIME_ACTIVE_PATH)
	if not FileAccess.file_exists(path):
		return ""
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		return ""
	var data: Dictionary = parsed
	return String(data.get("active", ""))


func _notice_header(title: String, sections: Array[Dictionary]) -> PackedStringArray:
	var lines := PackedStringArray()
	lines.append(title)
	lines.append(NOTICE_SEPARATOR)
	lines.append("Generated by the DSGD exporter. Do not edit.")
	lines.append("")
	for index in sections.size():
		lines.append("  %d. %s" % [index + 1, String(sections[index]["title"])])
	lines.append(NOTICE_SEPARATOR)
	return lines


## Concatenates the documents byte-for-byte. No timestamp: two exports of the
## same inputs must produce identical files.
func _compose_notice_file(header: PackedStringArray, sections: Array[Dictionary]) -> PackedByteArray:
	var output := PackedByteArray()
	output.append_array(("\n".join(header) + "\n").to_utf8_buffer())
	for section in sections:
		output.append_array(("\n" + NOTICE_SEPARATOR + "\n").to_utf8_buffer())
		output.append_array((String(section["title"]) + "\n").to_utf8_buffer())
		output.append_array((NOTICE_SEPARATOR + "\n\n").to_utf8_buffer())
		var data: PackedByteArray = section["data"]
		output.append_array(data)
		output.append_array("\n".to_utf8_buffer())
	return output


## Reads a notice source as bytes so its text stays verbatim. A UTF-8 BOM is
## stripped: it would otherwise land in the middle of a concatenated file.
func _read_notice_source(path: String) -> PackedByteArray:
	if path.is_empty():
		return PackedByteArray()
	var global_path := ProjectSettings.globalize_path(path) if path.begins_with("res://") else path
	if not FileAccess.file_exists(global_path):
		return PackedByteArray()
	var data := FileAccess.get_file_as_bytes(global_path)
	if data.size() >= 3 and data[0] == 0xEF and data[1] == 0xBB and data[2] == 0xBF:
		data = data.slice(3)
	return data


func _write_notice_file(target: String, data: PackedByteArray) -> bool:
	var file := FileAccess.open(target, FileAccess.WRITE)
	if file == null:
		push_warning("[DirectStorage Exporter] Could not open '" + target + "' for writing: " + error_string(FileAccess.get_open_error()) + ".")
		return false
	file.store_buffer(data)
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		push_warning("[DirectStorage Exporter] Failed to write '" + target + "': " + error_string(write_error) + ".")
		return false
	return true


## Removes every exporter-owned notice name, including the retired one, before
## the current settings write their set. Exact names only: a game's own
## LICENSE.txt / NOTICES.txt must never be touched.
func _remove_managed_notice_files() -> void:
	var managed: PackedStringArray = [
		DSGD_LICENSE_EXPORT_NAME,
		DIRECTSTORAGE_LICENSE_EXPORT_NAME,
		LEGACY_THIRD_PARTY_NOTICES_EXPORT_NAME,
	]
	for file_name in managed:
		var target := _target_export_dir.path_join(file_name)
		if not FileAccess.file_exists(target):
			continue
		var remove_error := DirAccess.remove_absolute(target)
		if remove_error != OK and FileAccess.file_exists(target):
			push_warning("[DirectStorage Exporter] Could not remove stale license file '" + target + "': " + error_string(remove_error) + ".")


func _option_bool(option: StringName, default_value: bool) -> bool:
	var value: Variant = get_option(option)
	if value == null and option == OPTION_INCLUDE_DSGD_LICENSE_FILE:
		value = get_option(OPTION_LEGACY_INCLUDE_THIRD_PARTY_NOTICES)
	if value == null:
		return default_value
	return bool(value)


## Writer round-trip: install the key this export generated, then open the
## archive that was just deployed with it and the published archive id.
##
## `open()` compares the header key id and authenticates the TOC, so an OK here
## proves the archive and the key belong to the same export. A failure marks the
## export failed but keeps the structurally valid archive. The key is left
## installed: clearing it with an empty string resets the key state and would
## discard a developer's preview key.
func _verify_written_archive() -> bool:
	var bridge: GDScript = _get_dspkg_static()
	if bridge == null:
		return false

	var installed: bool = bool(bridge.call("set_global_key", _archive_key.hex_encode()))
	if not installed:
		push_error("[DirectStorage Exporter] Failed to install the generated archive key; the written archive cannot be verified.")
		return false

	var key_id: String = String(bridge.call("get_global_key_id"))
	if key_id.is_empty():
		push_error("[DirectStorage Exporter] The generated archive key is not active; the written archive cannot be verified.")
		return false

	var reader: RefCounted = ClassDB.instantiate("DSPkg")
	if reader == null:
		push_error("[DirectStorage Exporter] Failed to create a DSPkg reader for the writer round-trip.")
		return false
	var err: Error = reader.open(_target_dspkg_path, _archive_id)
	if err != OK:
		push_error("[DirectStorage Exporter] The written archive does not open with the generated key and archive id: " + error_string(err))
		return false

	print("[DirectStorage Exporter] Writer round-trip verified (key id: ", key_id, ", entries: ", reader.get_entry_count(), ").")
	return true

## Loads the static key bridge once per export, only when the native class is
## registered.
func _get_dspkg_static() -> GDScript:
	if _dspkg_static != null:
		return _dspkg_static
	if _dspkg_static_unavailable:
		return null
	if not ClassDB.class_exists("DSPkg"):
		_dspkg_static_unavailable = true
		push_warning("[DirectStorage Exporter] The DSPkg class is not available; the generated key cannot be installed.")
		return null
	var script := load(DSPKG_STATIC_PATH) as GDScript
	if script == null:
		_dspkg_static_unavailable = true
		push_error("[DirectStorage Exporter] Failed to load the static DSPkg bridge at: " + DSPKG_STATIC_PATH)
		return null
	_dspkg_static = script
	return script

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

## Early warning when the target drive is nearly full. The final archive size is
## only known at finalize() time, so this cannot abort the export; it estimates
## the artifact set this export rewrites (the previous files are the best
## predictor available at begin time) and names the drive and the free bytes
## before the engine's template/PCK writes consume them.
func _warn_if_low_disk_space(abs_dir: String) -> void:
	const WARN_THRESHOLD_BYTES: int = 1024 * 1024 * 1024
	const HEADROOM_BYTES: int = 256 * 1024 * 1024
	var free_bytes := _get_free_space_bytes(abs_dir)
	if free_bytes < 0:
		return
	var rewrite_bytes := _estimate_rewrite_bytes()
	print("[DirectStorage Exporter] Free space on export drive: ", _format_bytes(free_bytes), " (", abs_dir, ")")
	if rewrite_bytes > 0:
		print("[DirectStorage Exporter] Previous exe/PCK/archive in the target directory total ", _format_bytes(rewrite_bytes), "; this export rewrites them.")
	if free_bytes < WARN_THRESHOLD_BYTES or free_bytes < rewrite_bytes + HEADROOM_BYTES:
		push_warning("[DirectStorage Exporter] Low disk space: only " + _format_bytes(free_bytes) + " free at '" + abs_dir + "'. This export rewrites about " + _format_bytes(rewrite_bytes) + " of exe/PCK/archive plus " + _format_bytes(HEADROOM_BYTES) + " of headroom. Free up space before the export fails with file-write errors.")

## Best-effort size of the artifact set this export overwrites in the target
## directory. Zero means "first export" (nothing to measure), in which case only
## the flat threshold applies. The previous files predict the next export well,
## because an export rewrites the same template, PCK and archive names.
func _estimate_rewrite_bytes() -> int:
	if _target_export_path.is_empty():
		return 0
	var total := _file_size_or_zero(_target_export_path)
	total += _file_size_or_zero(_target_export_path.get_basename() + ".console.exe")
	total += _file_size_or_zero(_target_dspkg_path)
	if not _preset_embeds_pck():
		total += _file_size_or_zero(_target_export_path.get_basename() + ".pck")
	return total

## The embedded-PCK preset writes the pack inside the executable, so the
## estimate must not count a sibling .pck that will never be written.
func _preset_embeds_pck() -> bool:
	var preset: EditorExportPreset = get_export_preset()
	if preset == null:
		return false
	return bool(preset.get("binary_format/embed_pck"))

func _file_size_or_zero(path: String) -> int:
	if path.is_empty() or not FileAccess.file_exists(path):
		return 0
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return 0
	var size := int(file.get_length())
	file.close()
	return size

## True when the engine recorded an export error for THIS run. The platform's
## message list is the same signal EditorExportPlatform::fill_log_messages uses
## to mark the result dialog "Failed.". Only messages added after _export_begin
## count, so an accumulated list (CLI exports, "Export All" reuse) cannot leak
## an earlier run's error into this one.
func _core_export_reports_error() -> bool:
	var platform: EditorExportPlatform = get_export_platform()
	if platform == null:
		return false
	if not platform.has_method("get_message_count") or not platform.has_method("get_message_type"):
		return false
	var count: int = platform.get_message_count()
	for index in range(_platform_messages_at_begin, count):
		if int(platform.get_message_type(index)) >= int(EditorExportPlatform.EXPORT_MESSAGE_ERROR):
			return true
	return false

## An EditorExportPlugin cannot abort the engine's PCK/exe steps, so without
## this the result dialog would report success for an export that deployed no
## archive. An EXPORT_MESSAGE_ERROR makes fill_log_messages report "Failed.";
## the artifact set is already fail-closed (no archive was deployed).
func _mark_export_failed(detail: String = "") -> void:
	var platform: EditorExportPlatform = get_export_platform()
	if platform == null or not platform.has_method("add_message"):
		return
	var text := detail if not detail.is_empty() else (
			"The DirectStorage archive step failed; " + _archive_name + " was not deployed (see the errors above).")
	platform.add_message(EditorExportPlatform.EXPORT_MESSAGE_ERROR, "DirectStorage", text)


func _resolve_pack_path() -> String:
	if _target_export_path.is_empty():
		return ""
	var extension := _target_export_path.get_extension().to_lower()
	if extension == "zip":
		return ""
	if extension == "pck":
		return _target_export_path
	return _target_export_path.get_basename() + ".pck"


func _report_pack_unverified(text: String) -> void:
	push_warning("[DirectStorage Exporter] " + text)
	var platform: EditorExportPlatform = get_export_platform()
	if platform != null and platform.has_method("add_message"):
		platform.add_message(EditorExportPlatform.EXPORT_MESSAGE_WARNING, "DirectStorage", text)


func _verify_pack_protection() -> bool:
	_pack_failure_detail = ""
	var verify: GDScript = _load_verifier()
	if verify == null:
		# A preset that disabled the scan did not promise anything, so a missing
		# helper is a warning there and a failure only where verification was on.
		if not _verify_pack_enabled:
			_report_pack_unverified("Pack verification is disabled and the verifier script is missing; nothing was checked.")
			return true
		_pack_failure_detail = "The exported PCK could not be verified: the pack verifier script is missing. Treat this build as unprotected."
		push_error("[DirectStorage Exporter] " + _pack_failure_detail)
		return false
	if not _verify_pack_enabled:
		var off: Dictionary = verify.call("skipped", "disabled_by_setting")
		print("[DirectStorage Exporter] " + String(verify.call("format_line", off, _target_export_path)))
		_report_pack_unverified("Pack verification is disabled in this preset; the exported PCK was not scanned, so this build is not verified.")
		return true
	var pack_path: String = _resolve_pack_path()
	var result: Dictionary
	if _preset_embeds_pck():
		pack_path = _target_export_path
		result = verify.call("unverified", "embedded_pck")
	elif pack_path.is_empty():
		pack_path = _target_export_path
		result = verify.call("skipped", "zip_export")
	else:
		result = verify.call("verify_pack", pack_path)
	print("[DirectStorage Exporter] " + String(verify.call("format_line", result, pack_path)))
	# ok means "nothing to fail on", not "verified": unverified/skipped are named
	# in the status word and never counted as verified.
	if bool(result.get("ok", false)):
		if String(result.get("status", "")) == "unverified":
			_report_pack_unverified("The PCK is embedded in the executable; the pack scan cannot read it, so PCK protection is not verified for this build.")
		return true
	_pack_failure_detail = "The exported PCK failed verification (reason: %s). Do not distribute this build as a protected build; see the PACK_VERIFY line above." % String(result.get("reason", "unknown"))
	push_error("[DirectStorage Exporter] " + _pack_failure_detail)
	return false

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
