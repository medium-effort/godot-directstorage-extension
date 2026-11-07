@tool
class_name DSMigrationCore
extends RefCounted

## Core logic for migrating Godot projects to the DirectStorage texture importer.
##
## Scans source images (png, jpg, jpeg, webp), inspects their .import sidecars,
## maps parameters for maximum parity, backs up original .import files into a ZIP
## archive inside the dsgd addon folder, rewrites .import files with UID preservation,
## and provides verified rollback from the ZIP backup.
##
## All methods are static or pure data helpers. Reimport triggers and UI are
## handled by callers (ds_migration_dialog.gd).

const TARGET_IMPORTER := "directstorage_texture"
const TARGET_VERSION := 4
const SCHEMA := 1
const SUPPORTED_EXTS := ["png", "jpg", "jpeg", "webp"]
const SKIP_DIRS := [".godot", ".git", ".migration_backup"]
const ADDON_TREE := "addons/directstorage_accelerator"
const DEFAULT_BACKUP_DIR := "res://addons/directstorage_accelerator/.migration_backup"

## First 4 bytes of a readable .dstex file ("DTEX").
const DSTEX_MAGIC := [0x44, 0x54, 0x45, 0x58]

const DS_DEFAULTS := {
	"compression/mode": "0",
	"compression/source": "1",
	"compression/gdeflate_level": "1",
	"compatibility/as_image_texture": "false",
}


# ==============================================================================
# Hashing & File I/O Helpers
# ==============================================================================

static func _sha256_bytes(data: PackedByteArray) -> String:
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(data)
	return ctx.finish().hex_encode()


static func _read_bytes(res_path: String) -> PackedByteArray:
	var f := FileAccess.open(res_path, FileAccess.READ)
	if f == null:
		return PackedByteArray()
	return f.get_buffer(int(f.get_length()))


static func _atomic_write_bytes(res_path: String, data: PackedByteArray) -> int:
	var abs_dir := ProjectSettings.globalize_path(res_path.get_base_dir())
	if not DirAccess.dir_exists_absolute(abs_dir):
		var mk := DirAccess.make_dir_recursive_absolute(abs_dir)
		if mk != OK:
			return mk
	var tmp_res := res_path + ".tmp.%d" % randi()
	var f := FileAccess.open(tmp_res, FileAccess.WRITE)
	if f == null:
		return FileAccess.get_open_error()
	f.store_buffer(data)
	f.flush()
	f.close()
	var da := DirAccess.open(res_path.get_base_dir())
	if da == null:
		return ERR_CANT_OPEN
	if FileAccess.file_exists(res_path):
		var rm := da.remove(res_path.get_file())
		if rm != OK:
			da.remove(tmp_res.get_file())
			return rm
	return da.rename(tmp_res.get_file(), res_path.get_file())


## True when p is a canonical res:// path with no traversal segment.
static func _is_canonical_res_path(p: String) -> bool:
	if p.is_empty() or not p.begins_with("res://"):
		return false
	if p.contains("\\"):
		return false
	for seg in p.trim_prefix("res://").split("/", true):
		if seg.is_empty() or seg == "." or seg == "..":
			return false
	return p.simplify_path() == p


## Returns the validated .import destination for a backup manifest entry, or "" when the
## entry does not describe "<res:// project image>.import".
static func _validate_restore_target(src_res: String, imp_res: String) -> String:
	if not _is_canonical_res_path(src_res):
		return ""
	if not src_res.get_extension().to_lower() in SUPPORTED_EXTS:
		return ""
	if imp_res != src_res + ".import":
		return ""
	if not _is_canonical_res_path(imp_res):
		return ""
	return imp_res


static func _load_import(imp_res: String) -> ConfigFile:
	var cp := ConfigFile.new()
	if cp.load(imp_res) != OK:
		return null
	return cp


static func _dest_list(cp: ConfigFile) -> Array[String]:
	var out: Array[String] = []
	if cp == null:
		return out
	if cp.has_section_key("remap", "path"):
		var p := str(cp.get_value("remap", "path", "")).strip_edges()
		if not p.is_empty() and not out.has(p):
			out.append(p)
	if cp.has_section("deps"):
		var v = cp.get_value("deps", "dest_files", [])
		if v is Array:
			for d in v:
				var ds := str(d).strip_edges()
				if not ds.is_empty() and not out.has(ds):
					out.append(ds)
		var files_v = cp.get_value("deps", "files", [])
		if files_v is Array:
			for d in files_v:
				var ds := str(d).strip_edges()
				if not ds.is_empty() and not out.has(ds):
					out.append(ds)
	return out


static func _dstex_readable(res_path: String) -> bool:
	if not res_path.to_lower().ends_with(".dstex"):
		return false
	var data := _read_bytes(res_path)
	if data.size() < 4:
		return false
	for i in 4:
		if data[i] != DSTEX_MAGIC[i]:
			return false
	return true


static func _should_skip_dir(rel: String, excludes: Array = []) -> bool:
	var flat := rel.replace("\\", "/").trim_prefix("/")
	for p in flat.split("/"):
		if p in SKIP_DIRS or (p.length() > 0 and p.begins_with(".")):
			return true
	if flat.begins_with(ADDON_TREE):
		return true
	for ex in excludes:
		var ex_s := str(ex).replace("\\", "/").trim_prefix("/").trim_prefix("res://")
		if not ex_s.is_empty() and flat.begins_with(ex_s):
			return true
	return false


# ==============================================================================
# Parameter Mapping Parity
# ==============================================================================

## Maps Godot default texture parameters to DirectStorage parameters.
## Returns [mapped_dict_or_null, reason_string, is_eligible_bool].
static func map_texture_params(params: Dictionary) -> Array:
	var mode := str(params.get("compress/mode", "2")).strip_edges()
	var mapped := DS_DEFAULTS.duplicate()

	# compress/mode: 0=Lossless, 1=Lossy, 2=VRAM Compressed, 3=VRAM Uncompressed, 4=BasisUniversal, 5=BPTC
	if mode == "2":
		mapped["compression/mode"] = "0"
		return [mapped, "VRAM Compressed (S3TC) -> DirectStorage VRAM Compressed", true]
	elif mode == "3":
		mapped["compression/mode"] = "1"
		return [mapped, "VRAM Uncompressed -> DirectStorage Uncompressed", true]
	elif mode == "5":
		mapped["compression/mode"] = "0"
		return [mapped, "VRAM Compressed (BPTC) -> DirectStorage VRAM Compressed (note: BPTC mapped to S3TC/BC)", true]
	elif mode in ["0", "1", "4"]:
		var mode_name := "Lossless" if mode == "0" else ("Lossy" if mode == "1" else "Basis Universal")
		return [null, "Incompatible compress/mode=%s (%s) — requires default importer" % [mode, mode_name], false]

	return [mapped, "Default DirectStorage parameters applied", true]


# ==============================================================================
# Project Scanning & Asset Classification
# ==============================================================================

## Scans the project from res:// and classifies every supported image file.
## Returns an array of entry dictionaries sorted alphabetically by source path.
static func scan(excludes: Array = []) -> Array[Dictionary]:
	var plan: Array[Dictionary] = []
	_walk_dir("res://", excludes, plan)
	plan.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return str(a.get("source", "")) < str(b.get("source", ""))
	)
	return plan


static func _walk_dir(dir_res: String, excludes: Array, plan: Array[Dictionary]) -> void:
	var rel := dir_res.trim_prefix("res://").trim_prefix("/").trim_prefix("\\")
	if rel != "" and _should_skip_dir(rel, excludes):
		return
	var da := DirAccess.open(dir_res)
	if da == null:
		return

	for d in da.get_directories():
		_walk_dir(dir_res.path_join(d), excludes, plan)

	for fn in da.get_files():
		var ext := fn.get_extension().to_lower()
		if not ext in SUPPORTED_EXTS:
			continue
		var src_res := dir_res.path_join(fn)
		var imp_res := src_res + ".import"

		if not FileAccess.file_exists(imp_res):
			plan.append({
				"source": src_res,
				"mode": "skip",
				"eligible": false,
				"reason": "Missing .import sidecar",
				"importer": "",
				"importer_version": 0,
				"uid": "",
				"old_dests": [],
				"mapped_params": {},
			})
			continue

		var cp := _load_import(imp_res)
		if cp == null or not cp.has_section("remap"):
			plan.append({
				"source": src_res,
				"mode": "skip",
				"eligible": false,
				"reason": "Unreadable .import sidecar",
				"importer": "",
				"importer_version": 0,
				"uid": "",
				"old_dests": [],
				"mapped_params": {},
			})
			continue

		var importer := str(cp.get_value("remap", "importer", "")).strip_edges()
		var iver := int(cp.get_value("remap", "importer_version", 0))
		var uid := str(cp.get_value("remap", "uid", "")).strip_edges()
		var old_dests := _dest_list(cp)

		if importer != TARGET_IMPORTER:
			var params := {}
			if cp.has_section("params"):
				for k in cp.get_section_keys("params"):
					params[k] = str(cp.get_value("params", k, ""))
			var mapped_res: Array = map_texture_params(params)
			var mapped_dict = mapped_res[0]
			var reason_str: String = mapped_res[1]
			var is_eligible: bool = mapped_res[2]

			if not is_eligible:
				plan.append({
					"source": src_res,
					"mode": "skip",
					"eligible": false,
					"reason": reason_str,
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": {},
				})
			else:
				plan.append({
					"source": src_res,
					"mode": "adopt",
					"eligible": true,
					"reason": reason_str,
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": mapped_dict if mapped_dict is Dictionary else DS_DEFAULTS.duplicate(),
				})
		elif iver < TARGET_VERSION:
			plan.append({
				"source": src_res,
				"mode": "upgrade",
				"eligible": true,
				"reason": "importer_version %d < %d" % [iver, TARGET_VERSION],
				"importer": importer,
				"importer_version": iver,
				"uid": uid,
				"old_dests": old_dests,
				"mapped_params": DS_DEFAULTS.duplicate(),
			})
		else:
			# Up to date importer version: check if artifact exists and is valid
			var missing := false
			if old_dests.is_empty():
				missing = true
			else:
				for d in old_dests:
					if not FileAccess.file_exists(d) or not _dstex_readable(d):
						missing = true
						break
			if missing:
				plan.append({
					"source": src_res,
					"mode": "repair",
					"eligible": true,
					"reason": "DirectStorage artifact missing or corrupt",
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": DS_DEFAULTS.duplicate(),
				})
			else:
				plan.append({
					"source": src_res,
					"mode": "up_to_date",
					"eligible": false,
					"reason": "Already on DirectStorage Texture",
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": {},
				})


# ==============================================================================
# ZIP Backup Creation & Restore
# ==============================================================================

## Creates a ZIP backup containing the exact original .import files and a manifest.json.
## The backup is saved inside backup_dir (default: res://addons/directstorage_accelerator/.migration_backup).
static func create_zip_backup(entries_to_backup: Array, backup_dir: String = DEFAULT_BACKUP_DIR) -> Dictionary:
	var abs_backup_dir := ProjectSettings.globalize_path(backup_dir)
	if not DirAccess.dir_exists_absolute(abs_backup_dir):
		var mk := DirAccess.make_dir_recursive_absolute(abs_backup_dir)
		if mk != OK:
			return {"ok": false, "error": "Failed to create backup directory: %s" % error_string(mk)}

	# Ensure .gdignore is present inside backup_dir so Godot never scans it
	var gdignore_path := backup_dir.path_join(".gdignore")
	if not FileAccess.file_exists(gdignore_path):
		var gf := FileAccess.open(gdignore_path, FileAccess.WRITE)
		if gf != null:
			gf.store_string("")
			gf.close()

	var dt := Time.get_datetime_dict_from_system()
	var time_str := "%04d%02d%02d_%02d%02d%02d" % [dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second]
	var zip_name := "backup_%s.zip" % time_str
	var zip_res_path := backup_dir.path_join(zip_name)
	var zip_abs_path := ProjectSettings.globalize_path(zip_res_path)

	var packer := ZIPPacker.new()
	var open_err := packer.open(zip_abs_path, ZIPPacker.APPEND_CREATE)
	if open_err != OK:
		return {"ok": false, "error": "Failed to open ZIPPacker at %s: %s" % [zip_res_path, error_string(open_err)]}

	var manifest_entries: Array = []
	var counter := 0

	for e in entries_to_backup:
		var src_res := str(e.get("source", ""))
		var imp_res := src_res + ".import"
		var imp_bytes := _read_bytes(imp_res)
		var src_bytes := _read_bytes(src_res)
		if imp_bytes.is_empty():
			continue

		counter += 1
		var zip_entry_name := "imports/%04d.import" % counter

		packer.start_file(zip_entry_name)
		packer.write_file(imp_bytes)
		packer.close_file()

		manifest_entries.append({
			"source": src_res,
			"import": imp_res,
			"zip_entry": zip_entry_name,
			"uid": str(e.get("uid", "")),
			"importer": str(e.get("importer", "")),
			"importer_version": int(e.get("importer_version", 0)),
			"source_sha256": _sha256_bytes(src_bytes),
			"import_sha256": _sha256_bytes(imp_bytes),
			"old_dests": e.get("old_dests", []),
			"mode": str(e.get("mode", "")),
			"status": "backed_up",
		})

	var manifest := {
		"schema": SCHEMA,
		"tool_version": "1.0.0",
		"created_unix": Time.get_unix_time_from_system(),
		"created_datetime": Time.get_datetime_string_from_system(),
		"project_root": "res://",
		"target_importer": TARGET_IMPORTER,
		"target_importer_version": TARGET_VERSION,
		"entries": manifest_entries,
	}

	var manifest_bytes := JSON.stringify(manifest, "  ").to_utf8_buffer()
	packer.start_file("manifest.json")
	packer.write_file(manifest_bytes)
	packer.close_file()

	packer.close()

	return {
		"ok": true,
		"zip_res_path": zip_res_path,
		"zip_abs_path": zip_abs_path,
		"count": manifest_entries.size(),
		"manifest": manifest,
	}


## Lists all existing backup ZIP files in backup_dir, sorted newest first.
static func get_available_backups(backup_dir: String = DEFAULT_BACKUP_DIR) -> Array[String]:
	var out: Array[String] = []
	var da := DirAccess.open(backup_dir)
	if da == null:
		return out
	for fn in da.get_files():
		if fn.ends_with(".zip") and fn.begins_with("backup_"):
			out.append(backup_dir.path_join(fn))
	out.sort()
	out.reverse()
	return out


# ==============================================================================
# .import Mutation & Migration Execution
# ==============================================================================

## Rewrites a .import file's bytes with in-place UID preservation and DirectStorage params.
static func rewrite_import(raw: PackedByteArray, mode: String, mapped_params: Dictionary = {}) -> PackedByteArray:
	var lines := raw.get_string_from_utf8().split("\n")
	var out: Array[String] = []
	var section := ""
	var params_written := false

	var target_params := DS_DEFAULTS.duplicate()
	for k in mapped_params.keys():
		target_params[k] = str(mapped_params[k])

	for i in lines.size():
		var ln: String = lines[i]
		var s := ln.strip_edges()

		if s.begins_with("[") and s.ends_with("]"):
			if section == "params" and mode == "adopt" and not params_written:
				for k in target_params.keys():
					out.append("%s=%s" % [k, target_params[k]])
				params_written = true
			section = s.substr(1, s.length() - 2)
			out.append(ln)
			continue

		if section == "remap":
			if ln.lstrip(" \t").begins_with("importer=") and mode == "adopt":
				out.append("importer=\"directstorage_texture\"")
			elif ln.lstrip(" \t").begins_with("type=") and mode == "adopt":
				out.append("type=\"Texture2D\"")
			elif ln.lstrip(" \t").begins_with("importer_version=") and (mode == "adopt" or mode == "upgrade"):
				# Setting version to 0 forces Godot's filesystem cache to invalidate and reimport
				out.append("importer_version=0")
			else:
				out.append(ln)
		elif section == "params" and mode == "adopt":
			continue # Drop old texture params
		else:
			out.append(ln)

	# If [params] was the last section, or if there was no [params] section
	if mode == "adopt" and not params_written:
		if section != "params":
			out.append("")
			out.append("[params]")
			out.append("")
		for k in target_params.keys():
			out.append("%s=%s" % [k, target_params[k]])
		params_written = true

	if mode == "upgrade":
		var existing_keys := {}
		for ln2 in out:
			var t := str(ln2).strip_edges()
			if not t.begins_with("[") and t.contains("="):
				existing_keys[t.split("=")[0].strip_edges()] = true
		for k in target_params.keys():
			if not existing_keys.has(k):
				out.append("%s=%s" % [k, target_params[k]])

	return "\n".join(PackedStringArray(out)).to_utf8_buffer()


## Applies migration to selected entries: rewrites .import files.
## Returns {"ok": true, "reimport_sources": Array[String], "failed": Array[Dictionary]}.
static func apply_migration(entries: Array) -> Dictionary:
	var reimport_sources: Array[String] = []
	var failed: Array[Dictionary] = []

	for e in entries:
		if not bool(e.get("eligible", false)):
			continue
		var src_res := str(e.get("source", ""))
		var imp_res := src_res + ".import"
		var raw := _read_bytes(imp_res)
		if raw.is_empty():
			failed.append({"source": src_res, "error": "Could not read .import"})
			continue

		var mode := str(e.get("mode", "adopt"))
		var mapped_params: Dictionary = e.get("mapped_params", {})
		var rewritten := rewrite_import(raw, mode, mapped_params)

		var err := _atomic_write_bytes(imp_res, rewritten)
		if err != OK:
			failed.append({"source": src_res, "error": "Atomic write failed: %s" % error_string(err)})
			continue

		# Verify UID preservation
		var post_cp := _load_import(imp_res)
		var post_uid := str(post_cp.get_value("remap", "uid", "")) if post_cp != null else ""
		if not post_uid.is_empty() and not str(e.get("uid", "")).is_empty():
			if post_uid != str(e.get("uid", "")):
				failed.append({"source": src_res, "error": "UID mismatch after rewrite"})
				continue

		reimport_sources.append(src_res)

	return {
		"ok": failed.is_empty(),
		"reimport_sources": reimport_sources,
		"failed": failed,
	}


# ==============================================================================
# Verification & Old Artifact Cleanup
# ==============================================================================

## Verifies migrated entries after EditorFileSystem has reimported them.
static func verify_migration(entries: Array) -> Array[Dictionary]:
	var results: Array[Dictionary] = []

	for e in entries:
		var src_res := str(e.get("source", ""))
		var imp_res := src_res + ".import"
		var cp := _load_import(imp_res)
		if cp == null:
			results.append({"source": src_res, "verified": false, "detail": "Unreadable .import after reimport"})
			continue

		var importer := str(cp.get_value("remap", "importer", ""))
		var iver := int(cp.get_value("remap", "importer_version", -1))
		var uid := str(cp.get_value("remap", "uid", ""))
		var dests := _dest_list(cp)

		var checks: Array[String] = []
		if importer != TARGET_IMPORTER:
			checks.append("importer=%s (expected %s)" % [importer, TARGET_IMPORTER])
		if iver != TARGET_VERSION:
			checks.append("importer_version=%d (expected %d)" % [iver, TARGET_VERSION])
		if not str(e.get("uid", "")).is_empty() and uid != str(e.get("uid", "")):
			checks.append("UID changed")
		if dests.is_empty():
			checks.append("No destination artifacts listed")

		for d in dests:
			if not FileAccess.file_exists(d):
				checks.append("Missing artifact: %s" % d.get_file())
			elif not _dstex_readable(d):
				checks.append("Corrupt or invalid .dstex: %s" % d.get_file())

		var ok := checks.is_empty()
		results.append({
			"source": src_res,
			"verified": ok,
			"detail": "Verified OK (.dstex valid, UID preserved)" if ok else "; ".join(PackedStringArray(checks)),
			"new_dests": dests,
		})

	return results


## Automatically deletes replaced old artifacts (e.g. .ctex, .ctex.md5) for verified entries.
## Strictly guarded to files inside res://.godot/imported/.
static func cleanup_old_artifacts(verified_entries: Array) -> Array[Dictionary]:
	var results: Array[Dictionary] = []
	var allowed_dir := ProjectSettings.globalize_path("res://.godot/imported").replace("\\", "/").to_lower()

	for e in verified_entries:
		var src_res := str(e.get("source", ""))
		var deleted: Array[String] = []
		var old_dests: Array = e.get("old_dests", [])

		for od in old_dests:
			var ods := str(od).strip_edges()
			if ods.is_empty():
				continue
			var abs_path := ProjectSettings.globalize_path(ods).replace("\\", "/")
			if not abs_path.to_lower().begins_with(allowed_dir + "/") or abs_path.contains(".."):
				continue
			if FileAccess.file_exists(abs_path):
				if DirAccess.remove_absolute(abs_path) == OK:
					deleted.append(ods)
			# Also clean matching .md5 sidecar if present
			var md5_path := abs_path + ".md5"
			if FileAccess.file_exists(md5_path):
				DirAccess.remove_absolute(md5_path)

		results.append({
			"source": src_res,
			"deleted": deleted,
		})

	return results


# ==============================================================================
# Rollback / Restore
# ==============================================================================

## Restores original .import files from a ZIP backup and deletes the created .dstex files.
## Caller triggers EditorFileSystem.reimport_files afterwards.
static func restore_from_zip(zip_res_path: String) -> Dictionary:
	var zip_abs_path := ProjectSettings.globalize_path(zip_res_path)
	var reader := ZIPReader.new()
	var err := reader.open(zip_abs_path)
	if err != OK:
		return {"ok": false, "error": "Failed to open ZIP backup: %s" % error_string(err)}

	var manifest_raw := reader.read_file("manifest.json")
	if manifest_raw.is_empty():
		reader.close()
		return {"ok": false, "error": "Backup ZIP missing manifest.json"}

	var manifest = JSON.parse_string(manifest_raw.get_string_from_utf8())
	if not manifest is Dictionary or not manifest.has("entries"):
		reader.close()
		return {"ok": false, "error": "Invalid manifest.json in backup ZIP"}

	var reimport_sources: Array[String] = []
	var results: Array[Dictionary] = []
	var allowed_imported_dir := ProjectSettings.globalize_path("res://.godot/imported").replace("\\", "/").to_lower()

	for entry in manifest["entries"]:
		var src_res := str(entry.get("source", ""))
		var imp_res := _validate_restore_target(src_res, str(entry.get("import", src_res + ".import")))
		if imp_res.is_empty():
			results.append({
				"source": src_res,
				"status": "failed",
				"reason": "Rejected manifest entry: destination is not a project image .import sidecar",
			})
			continue
		var zip_entry := str(entry.get("zip_entry", ""))

		# A source that no longer exists cannot be proven unmodified: skip, never restore blind.
		if not FileAccess.file_exists(src_res):
			results.append({
				"source": src_res,
				"status": "skipped",
				"reason": "Source image missing since backup",
			})
			continue
		var cur_src := _read_bytes(src_res)
		if _sha256_bytes(cur_src) != str(entry.get("source_sha256", "")):
			results.append({"source": src_res, "status": "skipped", "reason": "Source image modified since backup"})
			continue

		var original_import_bytes := reader.read_file(zip_entry)
		if original_import_bytes.is_empty():
			results.append({"source": src_res, "status": "failed", "reason": "Failed to read %s from ZIP" % zip_entry})
			continue

		var write_err := _atomic_write_bytes(imp_res, original_import_bytes)
		if write_err != OK:
			results.append({"source": src_res, "status": "failed", "reason": "Failed to restore %s" % imp_res})
			continue

		# Clean up any generated .dstex artifacts created during DirectStorage import
		var current_cp := _load_import(imp_res)
		var dests := _dest_list(current_cp)
		for d in dests:
			var abs_d := ProjectSettings.globalize_path(d).replace("\\", "/")
			if abs_d.to_lower().ends_with(".dstex") and abs_d.to_lower().begins_with(allowed_imported_dir + "/") and not abs_d.contains(".."):
				if FileAccess.file_exists(abs_d):
					DirAccess.remove_absolute(abs_d)
				var d_md5 := abs_d + ".md5"
				if FileAccess.file_exists(d_md5):
					DirAccess.remove_absolute(d_md5)

		reimport_sources.append(src_res)
		results.append({"source": src_res, "status": "restored", "reason": "Restored original .import"})

	reader.close()

	return {
		"ok": true,
		"reimport_sources": reimport_sources,
		"results": results,
	}

