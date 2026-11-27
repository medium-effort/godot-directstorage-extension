# SPDX-FileCopyrightText: 2026 MediumEffort
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

@tool
class_name DSMigrationCore
extends RefCounted

## Core logic for migrating Godot projects to the DirectStorage texture importer.
##
## Scans source images (png, jpg, jpeg, webp, tga, dds), inspects their .import sidecars,
## maps parameters for maximum parity, backs up original .import files into a ZIP
## archive inside the dsgd addon folder, rewrites .import files with UID preservation,
## and provides verified rollback from the ZIP backup.
##
## All methods are static or pure data helpers. Reimport triggers and UI are
## handled by callers (ds_migration_dialog.gd).

const IMPORTER = preload("res://addons/directstorage_accelerator/scripts/ds_importer.gd")
const TARGET_IMPORTER := "directstorage_texture"
const TARGET_VERSION := IMPORTER.FORMAT_VERSION
const SCHEMA := 1
const SUPPORTED_EXTS := ["png", "jpg", "jpeg", "webp", "tga", "dds"]
const SKIP_DIRS := [".godot", ".git", ".migration_backup"]
const ADDON_TREE := "addons/directstorage_accelerator"
const DEFAULT_BACKUP_DIR := "res://addons/directstorage_accelerator/.migration_backup"

## ZIPPacker.ZipAppend.APPEND_CREATE, duplicated as an int because naming the
## class here would make this whole file a parse error on an engine built
## without the zip module (module_zip_enabled=no).
const ZIP_APPEND_CREATE := 0

## Option keys the v5 importer owns. Their presence in a sidecar means the asset already
## speaks v5, so the legacy compression/source key must not override them.
const COLOR_OPTION_KEYS := ["compression/color_space", "compress/normal_map", "compress/channel_pack"]

## Migration actions, in ds_migration_dialog.ACTION_LABELS order.
const ACTION_SKIP := 0
const ACTION_VRAM_COMPRESSED := 1
const ACTION_VRAM_UNCOMPRESSED := 2
const ACTION_NORMAL_MAP := 3

## First 4 bytes of a readable .dstex file ("DTEX").
const DSTEX_MAGIC := [0x44, 0x54, 0x45, 0x58]

## Artifact suffixes cleanup is allowed to delete: a superseded .ctex and the checksum Godot
## writes beside it. Anything else - notably the live .dstex family - is a current destination,
## so an unknown suffix is left on disk (fails closed).
const CLEANUP_SUFFIXES := [".ctex", ".md5"]

const DS_DEFAULTS := {
	"compression/mode": "0",
	"compression/high_quality": "false",
	"compression/color_space": "1",
	"compress/normal_map": "0",
	"compress/channel_pack": "0",
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


# --- Failure-injection seam for the headless write-integrity suite -----------------
# Inert unless a debug build (the editor counts) opts in. Never called by the tool.
static var _test_write_truncate := 0
static var _test_commit_error := OK


## Drops p_truncate bytes from the temporary write while still reporting success, and/or
## makes the commit step fail with p_commit_error. Used only by
## project/headless_tests/test_migration_write_integrity.gd.
static func _set_io_faults_for_testing(p_truncate: int = 0, p_commit_error: int = OK) -> void:
	if not OS.is_debug_build():
		return
	_test_write_truncate = maxi(0, p_truncate)
	_test_commit_error = p_commit_error


static func _atomic_write_bytes(res_path: String, data: PackedByteArray) -> int:
	var abs_dir := ProjectSettings.globalize_path(res_path.get_base_dir())
	if not DirAccess.dir_exists_absolute(abs_dir):
		var mk := DirAccess.make_dir_recursive_absolute(abs_dir)
		if mk != OK:
			return mk
	var da := DirAccess.open(res_path.get_base_dir())
	if da == null:
		return ERR_CANT_OPEN
	var tmp_res := res_path + ".tmp.%d" % randi()
	var f := FileAccess.open(tmp_res, FileAccess.WRITE)
	if f == null:
		return FileAccess.get_open_error()
	var payload := data
	if _test_write_truncate > 0:
		payload = data.slice(0, maxi(0, data.size() - _test_write_truncate))
	var wrote := f.store_buffer(payload)
	f.flush()
	var write_err := f.get_error()
	var written_len := f.get_length()
	f.close()
	# A short write can still report success: the length is the backstop.
	if not wrote or write_err != OK or written_len != data.size():
		da.remove(tmp_res.get_file())
		return write_err if write_err != OK else ERR_FILE_CANT_WRITE

	var dest_file := res_path.get_file()
	var had_dest := FileAccess.file_exists(res_path)
	var bak_file := dest_file + ".bak.%d" % randi()
	while had_dest and FileAccess.file_exists(res_path.get_base_dir().path_join(bak_file)):
		bak_file = dest_file + ".bak.%d" % randi()  # never clobber a previous run's backup

	# DirAccess.rename() is not an atomic replace on Windows: it removes the destination
	# first (dir_access_windows.cpp:340-346). Park the original, move the verified temporary
	# into place, and put the original back if that fails, so no failure path loses it.
	if had_dest:
		var parked := da.rename(dest_file, bak_file)
		if parked != OK:
			da.remove(tmp_res.get_file())
			return parked
	var put := _test_commit_error
	if put == OK:
		put = da.rename(tmp_res.get_file(), dest_file)
	if put != OK:
		da.remove(tmp_res.get_file())
		if had_dest:
			da.rename(bak_file, dest_file)  # best-effort: if this fails the bytes stay in the .bak
		return put
	if had_dest and da.remove(bak_file) != OK:
		push_warning("Migration: could not remove backup %s" % bak_file)
	return OK


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

## v5 colour triple for a normal-map / channel-packing decision. Normal maps are always stored
## linear (Godot's legacy COMPRESS_SOURCE_NORMAL did the same), so the sRGB flag and the channel
## layout follow the normal flag instead of being chosen independently.
static func _color_options(is_normal: bool, optimized_channels: bool) -> Dictionary:
	return {
		"compression/color_space": "0" if is_normal else "1",
		"compress/normal_map": "1" if is_normal else "0",
		"compress/channel_pack": "1" if (is_normal or optimized_channels) else "0",
	}


## v5 colour triple equivalent to a legacy compression/source value:
## 0 = Generic -> linear / not-normal / optimized, 1 = sRGB -> sRGB / not-normal / friendly,
## 2 = Normal Map -> linear / normal / optimized.
static func _legacy_source_options(source: int) -> Dictionary:
	match source:
		0:
			return _color_options(false, true)
		2:
			return _color_options(true, true)
		_:
			return _color_options(false, false)


## True when a sidecar already carries the split v5 colour options.
static func _has_color_options(cp: ConfigFile) -> bool:
	for k in COLOR_OPTION_KEYS:
		if cp.has_section_key("params", k):
			return true
	return false


## DirectStorage parameters a sidecar already carries, over the defaults. Only keys
## DS_DEFAULTS knows are read, so an unknown key keeps its line untouched on rewrite.
static func _existing_ds_params(cp: ConfigFile) -> Dictionary:
	var out := DS_DEFAULTS.duplicate()
	if cp == null or not cp.has_section("params"):
		return out
	for k in DS_DEFAULTS.keys():
		if cp.has_section_key("params", k):
			out[k] = str(cp.get_value("params", k))
	return out


## Maps Godot default texture parameters to DirectStorage parameters.
## Returns [mapped_dict_or_null, reason_string, is_eligible_bool].
static func map_texture_params(params: Dictionary) -> Array:
	var mode := str(params.get("compress/mode", "2")).strip_edges()
	var mapped := DS_DEFAULTS.duplicate()

	# Godot's ResourceImporterTexture reuses these key names with different enum values for
	# compress/normal_map (Detect/Enable/Disabled = 0/1/2 there, Disabled/Enabled = 0/1 here),
	# so only Enable translates to "normal map". Detect cannot be resolved at migration time.
	var src_normal := str(params.get("compress/normal_map", "0")).strip_edges()
	var src_channel := str(params.get("compress/channel_pack", "0")).strip_edges()
	var src_hq := str(params.get("compress/high_quality", "false")).strip_edges()
	mapped.merge(_color_options(src_normal == "1", src_channel == "1"), true)

	# compress/mode: 0=Lossless, 1=Lossy, 2=VRAM Compressed, 3=VRAM Uncompressed, 4=BasisUniversal, 5=BPTC
	if mode == "2":
		mapped["compression/mode"] = "0"
		mapped["compression/high_quality"] = "true" if src_hq == "true" else "false"
		return [mapped, "VRAM Compressed (S3TC) -> DirectStorage VRAM Compressed", true]
	elif mode == "3":
		mapped["compression/mode"] = "1"
		mapped["compression/high_quality"] = "false"
		return [mapped, "VRAM Uncompressed -> DirectStorage Uncompressed", true]
	elif mode == "5":
		mapped["compression/mode"] = "0"
		mapped["compression/high_quality"] = "true"
		return [mapped, "VRAM Compressed (BPTC) -> DirectStorage VRAM Compressed (BC7 via compression/high_quality)", true]
	elif mode in ["0", "1", "4"]:
		var mode_name := "Lossless" if mode == "0" else ("Lossy" if mode == "1" else "Basis Universal")
		return [null, "Incompatible compress/mode=%s (%s) — requires default importer" % [mode, mode_name], false]

	return [mapped, "Default DirectStorage parameters applied", true]


## Checks whether a .dds file is a 2D texture in a pixel format supported by DirectStorage Texture.
## Rejects cubemaps, volume textures, DX10 texture arrays, and unsupported formats without spamming
## push_error() during project scans so incompatible assets stay on Godot's built-in DDS importer.
static func check_dds_compatibility(src_res: String) -> Dictionary:
	var bytes := _read_bytes(src_res)
	if bytes.size() < 128 or bytes.decode_u32(0) != IMPORTER.DDS_MAGIC:
		return {"compatible": false, "compressed": false, "high_quality": false, "reason": "Invalid or unreadable DDS header — kept on Godot's importer"}

	var caps2: int = bytes.decode_u32(112)
	if (caps2 & (IMPORTER.DDS_CAPS2_CUBEMAP | IMPORTER.DDS_CAPS2_VOLUME)) != 0:
		return {"compatible": false, "compressed": false, "high_quality": false, "reason": "DDS cubemap or volume texture — kept on Godot's importer"}

	var fourcc: int = bytes.decode_u32(84)
	if fourcc == IMPORTER.DDS_FOURCC_DX10:
		if bytes.size() < 148:
			return {"compatible": false, "compressed": false, "high_quality": false, "reason": "Truncated DX10 DDS header — kept on Godot's importer"}
		var resource_dimension: int = bytes.decode_u32(132)
		var array_size: int = bytes.decode_u32(140)
		if resource_dimension != IMPORTER.DDS_DIMENSION_TEXTURE2D or array_size > 1:
			return {"compatible": false, "compressed": false, "high_quality": false, "reason": "DDS DX10 non-2D or texture array — kept on Godot's importer"}

	var img := Image.new()
	var err := img.load_dds_from_buffer(bytes)
	if err != OK or img.is_empty():
		return {"compatible": false, "compressed": false, "high_quality": false, "reason": "Unsupported or unreadable DDS payload — kept on Godot's importer"}

	var fmt := img.get_format()
	if img.is_compressed():
		if not IMPORTER.DDS_SUPPORTED_COMPRESSED_FORMATS.has(fmt):
			return {"compatible": false, "compressed": true, "high_quality": false, "reason": "Unsupported compressed DDS format — kept on Godot's importer"}
		var is_bptc := fmt in [Image.FORMAT_BPTC_RGBA, Image.FORMAT_BPTC_RGBF, Image.FORMAT_BPTC_RGBFU]
		return {"compatible": true, "compressed": true, "high_quality": is_bptc, "reason": ""}

	if fmt in [Image.FORMAT_R16I, Image.FORMAT_RG16I, Image.FORMAT_RGBA16I, Image.FORMAT_RGBE9995]:
		return {"compatible": false, "compressed": false, "high_quality": false, "reason": "Unsupported uncompressed DDS pixel format — kept on Godot's importer"}

	return {"compatible": true, "compressed": false, "high_quality": false, "reason": ""}


## Maps a compatible 2D DDS sidecar's parameters to DirectStorage parameters.
static func map_dds_params(params: Dictionary, compat: Dictionary) -> Array:
	var mapped := DS_DEFAULTS.duplicate()
	var src_normal := str(params.get("compress/normal_map", "0")).strip_edges()
	var src_channel := str(params.get("compress/channel_pack", "0")).strip_edges()
	mapped.merge(_color_options(src_normal == "1", src_channel == "1"), true)

	if bool(compat.get("compressed", false)):
		mapped["compression/mode"] = "0"
		mapped["compression/high_quality"] = "true" if bool(compat.get("high_quality", false)) else "false"
		var codec := "BPTC" if bool(compat.get("high_quality", false)) else "S3TC"
		return [mapped, "Compatible 2D DDS (%s) -> DirectStorage VRAM Compressed (passthrough)" % codec, true]

	# Uncompressed DDS: keep VRAM Uncompressed if the Godot DDS sidecar requested it, else VRAM Compressed.
	var dds_mode := str(params.get("compress/mode", "0")).strip_edges()
	if dds_mode == "2":
		mapped["compression/mode"] = "1"
		mapped["compression/high_quality"] = "false"
		return [mapped, "Compatible 2D DDS (Uncompressed) -> DirectStorage Uncompressed", true]

	mapped["compression/mode"] = "0"
	mapped["compression/high_quality"] = "false"
	return [mapped, "Compatible 2D DDS -> DirectStorage VRAM Compressed", true]


## Import parameters one migration action actually decides. A delta, not a full preset: the adopt
## path merges it over DS_DEFAULTS itself, while the upgrade path rewrites only these keys and keeps
## every other [params] line (gdeflate_level, as_image_texture, mipmaps/*, process/*) intact.
static func action_mapped_params(action: int) -> Dictionary:
	match action:
		ACTION_VRAM_UNCOMPRESSED:
			return {"compression/mode": "1", "compression/high_quality": "false"}
		ACTION_NORMAL_MAP:
			return _color_options(true, true)
		ACTION_VRAM_COMPRESSED:
			return {"compression/mode": "0", "compression/high_quality": "false"}
		_:
			return {}


## Default action for a scanned entry, from the parameters the mapper produced.
static func action_for_params(mapped: Dictionary) -> int:
	if str(mapped.get("compress/normal_map", "0")) == "1":
		return ACTION_NORMAL_MAP
	if str(mapped.get("compression/mode", "0")) == "1":
		return ACTION_VRAM_UNCOMPRESSED
	return ACTION_VRAM_COMPRESSED


## Mode an explicit dialog action writes. Rows that are already DirectStorage keep their
## in-place upgrade path, so the action's parameters override the sidecar's instead of
## replacing the whole [params] section.
static func action_mode_for_entry(entry: Dictionary, action: int) -> String:
	if action == ACTION_SKIP:
		return "skip"
	var base := str(entry.get("base_mode", entry.get("mode", "adopt")))
	if base == "upgrade" or base == "repair":
		return "upgrade"
	return "adopt"


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
	# Godot's EditorFileSystem ignores any directory containing .gdignore (and its subtree),
	# so reimport_files() would silently skip assets inside it.
	if FileAccess.file_exists(dir_res.path_join(".gdignore")):
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

			var mapped_res: Array
			if ext == "dds":
				var compat := check_dds_compatibility(src_res)
				if not bool(compat.get("compatible", false)):
					mapped_res = [null, str(compat.get("reason", "Incompatible DDS — kept on Godot's importer")), false]
				else:
					mapped_res = map_dds_params(params, compat)
			else:
				mapped_res = map_texture_params(params)

			var mapped_dict = mapped_res[0]
			var reason_str: String = mapped_res[1]
			var is_eligible: bool = mapped_res[2]

			if not is_eligible:
				plan.append({
					"source": src_res,
					"mode": "skip",
					"base_mode": "skip",
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
					"base_mode": "adopt",
					"eligible": true,
					"reason": reason_str,
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": mapped_dict if mapped_dict is Dictionary else DS_DEFAULTS.duplicate(),
				})
		elif ext == "dds" and not bool(check_dds_compatibility(src_res).get("compatible", false)):
			var dds_compat := check_dds_compatibility(src_res)
			plan.append({
				"source": src_res,
				"mode": "skip",
				"base_mode": "skip",
				"eligible": false,
				"reason": str(dds_compat.get("reason", "Incompatible DDS")) + " (switch Import As to DDS in Import dock)",
				"importer": importer,
				"importer_version": iver,
				"uid": uid,
				"old_dests": old_dests,
				"mapped_params": {},
			})
		elif iver < TARGET_VERSION:
			# An older sidecar still carries compression/source; translate it so the dialog lists
			# the real action (a legacy normal map is not "VRAM Compressed"). Its own mode is part
			# of that answer, so start from the parameters the sidecar states.
			var upgrade_params := _existing_ds_params(cp)
			if cp.has_section("params") and not _has_color_options(cp):
				var legacy_src := int(cp.get_value("params", "compression/source", 1))
				upgrade_params.merge(_legacy_source_options(legacy_src), true)
			plan.append({
				"source": src_res,
				"mode": "upgrade",
				"base_mode": "upgrade",
				"eligible": true,
				"reason": "Upgrade importer v%d -> v%d" % [iver, TARGET_VERSION],
				"importer": importer,
				"importer_version": iver,
				"uid": uid,
				"old_dests": old_dests,
				"mapped_params": upgrade_params,
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
					"base_mode": "repair",
					"eligible": true,
					"reason": "DirectStorage artifact missing or corrupt",
					"importer": importer,
					"importer_version": iver,
					"uid": uid,
					"old_dests": old_dests,
					"mapped_params": _existing_ds_params(cp),
				})
			else:
				plan.append({
					"source": src_res,
					"mode": "up_to_date",
					"base_mode": "up_to_date",
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

	if not ClassDB.class_exists("ZIPPacker"):
		return {"ok": false, "error": "ZIP archive writing is not available in this Godot build (module_zip_enabled=no). Compile Godot with module_zip_enabled=yes."}
	var packer: RefCounted = ClassDB.instantiate("ZIPPacker")
	if packer == null:
		return {"ok": false, "error": "Could not create a ZIPPacker instance."}
	var open_err: Error = packer.open(zip_abs_path, ZIP_APPEND_CREATE)
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
	var legacy_source := ""

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
		elif section == "params" and mode == "upgrade" and s.begins_with("compression/source="):
			legacy_source = s.split("=")[1].strip_edges().trim_prefix("\"").trim_suffix("\"")
			continue # Translated into the v5 triple below
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
		if not legacy_source.is_empty():
			var legacy_has_colors := false
			for k in COLOR_OPTION_KEYS:
				if _params_has_key(out, k):
					legacy_has_colors = true
					break
			if not legacy_has_colors:
				target_params.merge(_legacy_source_options(int(legacy_source)), true)

		# Explicit caller parameters win over the legacy translation and over the sidecar.
		for k in mapped_params.keys():
			target_params[k] = str(mapped_params[k])

		# Override the keys the caller named in place; every other [params] line is preserved.
		var existing_keys := {}
		var cur := ""
		for i2 in out.size():
			var t := str(out[i2]).strip_edges()
			if t.begins_with("[") and t.ends_with("]"):
				cur = t.substr(1, t.length() - 2)
				continue
			if cur != "params" or not t.contains("="):
				continue
			var key := t.split("=")[0].strip_edges()
			existing_keys[key] = true
			if mapped_params.has(key):
				out[i2] = "%s=%s" % [key, str(mapped_params[key])]

		if not params_written and section != "params":
			out.append("")
			out.append("[params]")
			out.append("")
		for k in target_params.keys():
			if not existing_keys.has(k):
				out.append("%s=%s" % [k, target_params[k]])

	return "\n".join(PackedStringArray(out)).to_utf8_buffer()


## True when the [params] section of an already-built line list carries `key`.
static func _params_has_key(lines: Array[String], key: String) -> bool:
	var cur := ""
	for ln in lines:
		var t := str(ln).strip_edges()
		if t.begins_with("[") and t.ends_with("]"):
			cur = t.substr(1, t.length() - 2)
			continue
		if cur == "params" and t.begins_with(key + "="):
			return true
	return false


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


## The subset of `selected` scan entries whose post-reimport verification passed.
## Keeps the scan dictionaries (and therefore old_dests) intact for the cleanup, and attaches the
## verification's `new_dests`: the scan entries are what carry `old_dests`, the verify results are
## what carry `new_dests`, so the current-destination guard needs both halves in one dictionary.
static func filter_verified_entries(selected: Array, verify_results: Array) -> Array:
	var verified := {}
	for v in verify_results:
		if bool(v.get("verified", false)):
			verified[str(v.get("source", ""))] = v

	var out: Array = []
	for e in selected:
		var src := str(e.get("source", ""))
		if not verified.has(src):
			continue
		var kept: Dictionary = e.duplicate()
		kept["new_dests"] = verified[src].get("new_dests", [])
		out.append(kept)
	return out


## Deletes superseded .ctex artifacts for entries whose migration verified. Three guards:
## only a .ctex and the .md5 checksum beside it (never the live .dstex family), only inside
## res://.godot/imported/, and never a path that is still a current destination (new_dests).
static func cleanup_old_artifacts(verified_entries: Array) -> Array[Dictionary]:
	var results: Array[Dictionary] = []
	var allowed_dir := _cleanup_key("res://.godot/imported")

	for e in verified_entries:
		var src_res := str(e.get("source", ""))
		var deleted: Array[String] = []
		var old_dests: Array = e.get("old_dests", [])

		# Current destinations, normalised the same way as the candidates below. A kept artifact
		# also keeps the checksum beside it.
		var keep := {}
		for nd in e.get("new_dests", []):
			var nds := str(nd).strip_edges()
			if nds.is_empty():
				continue
			keep[_cleanup_key(nds)] = true
			var nd_md5 := _md5_sidecar_path(nds)
			if not nd_md5.is_empty():
				keep[_cleanup_key(nd_md5)] = true

		for od in old_dests:
			var ods := str(od).strip_edges()
			if ods.is_empty():
				continue
			# The .md5 sidecar goes through the same predicate as its main artifact.
			for cand in _cleanup_candidates(ods):
				var cand_s := str(cand)
				var abs_path := ProjectSettings.globalize_path(cand_s).replace("\\", "/")
				var low := abs_path.to_lower()
				if not low.begins_with(allowed_dir + "/") or low.contains(".."):
					continue
				if keep.has(low):
					continue
				if not _is_cleanup_artifact(low):
					continue
				if FileAccess.file_exists(abs_path) and DirAccess.remove_absolute(abs_path) == OK:
					deleted.append(cand_s)

		results.append({
			"source": src_res,
			"deleted": deleted,
		})

	return results


## Lower-cased absolute form used by every cleanup path comparison.
static func _cleanup_key(res_path: String) -> String:
	return ProjectSettings.globalize_path(res_path).replace("\\", "/").to_lower()


## The checksum sidecar Godot writes for an imported artifact: `<stem>.md5`, where the stem is
## the destination with its artifact extension removed (`foo-hash.ctex` pairs with `foo-hash.md5`,
## not `foo-hash.ctex.md5`). Returns "" when the destination has no extension.
static func _md5_sidecar_path(dest: String) -> String:
	var base := dest.get_file()
	if base.get_extension().is_empty():
		return ""
	return dest.get_base_dir().path_join(base.get_basename() + ".md5")


## The artifacts a superseded destination may delete: the destination and its checksum. Only a
## .ctex yields candidates - the live .dstex family never does, so cleanup cannot reach it.
static func _cleanup_candidates(dest: String) -> Array[String]:
	var out: Array[String] = []
	if not dest.to_lower().ends_with(".ctex"):
		return out
	out.append(dest)
	var md5 := _md5_sidecar_path(dest)
	if not md5.is_empty():
		out.append(md5)
	return out


## True when a lower-cased absolute path is an artifact cleanup may delete.
static func _is_cleanup_artifact(low_abs_path: String) -> bool:
	for suf in CLEANUP_SUFFIXES:
		if low_abs_path.ends_with(suf):
			return true
	return false


# ==============================================================================
# Rollback / Restore
# ==============================================================================

## Restores original .import files from a ZIP backup and deletes the created .dstex files.
## Caller triggers EditorFileSystem.reimport_files afterwards.
static func restore_from_zip(zip_res_path: String) -> Dictionary:
	if not ClassDB.class_exists("ZIPReader"):
		return {"ok": false, "error": "ZIP archive reading is not available in this Godot build (module_zip_enabled=no). Compile Godot with module_zip_enabled=yes."}
	var zip_abs_path := ProjectSettings.globalize_path(zip_res_path)
	var reader: RefCounted = ClassDB.instantiate("ZIPReader")
	if reader == null:
		return {"ok": false, "error": "Could not create a ZIPReader instance."}
	var err: Error = reader.open(zip_abs_path)
	if err != OK:
		return {"ok": false, "error": "Failed to open ZIP backup: %s" % error_string(err)}

	var manifest_raw: PackedByteArray = reader.read_file("manifest.json")
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

		var original_import_bytes: PackedByteArray = reader.read_file(zip_entry)
		if original_import_bytes.is_empty():
			results.append({"source": src_res, "status": "failed", "reason": "Failed to read %s from ZIP" % zip_entry})
			continue

		# The migrated sidecar is about to be replaced by the pre-migration copy; capture the
		# artifacts it produced first, otherwise the list below describes the restored .ctex world.
		var pre_restore_cp := _load_import(imp_res)
		var dests := _dest_list(pre_restore_cp)

		var write_err := _atomic_write_bytes(imp_res, original_import_bytes)
		if write_err != OK:
			results.append({"source": src_res, "status": "failed", "reason": "Failed to restore %s" % imp_res})
			continue

		# Clean up the .dstex artifacts the migrated sidecar produced, plus the checksum Godot
		# writes beside each one (`<stem>.md5`, not `<artifact>.dstex.md5`).
		for d in dests:
			var abs_d := ProjectSettings.globalize_path(d).replace("\\", "/")
			if abs_d.to_lower().ends_with(".dstex") and abs_d.to_lower().begins_with(allowed_imported_dir + "/") and not abs_d.contains(".."):
				if FileAccess.file_exists(abs_d):
					DirAccess.remove_absolute(abs_d)
				var d_md5 := _md5_sidecar_path(abs_d)
				if not d_md5.is_empty() and FileAccess.file_exists(d_md5):
					DirAccess.remove_absolute(d_md5)

		reimport_sources.append(src_res)
		results.append({"source": src_res, "status": "restored", "reason": "Restored original .import"})

	reader.close()

	return {
		"ok": true,
		"reimport_sources": reimport_sources,
		"results": results,
	}

