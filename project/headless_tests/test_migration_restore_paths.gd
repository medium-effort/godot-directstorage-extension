@tool
extends SceneTree

## Restore-path contract: a backup manifest may only name a destination in the
## "<res:// project image>.import" class, and only for a source that still exists
## and matches its recorded SHA-256. Nothing else may be written or deleted.

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")
const TMP := "res://test_migration_restore_tmp"
const SENTINEL := "GDS_MIGRATION_SENTINEL"
const PAYLOAD := "PWNED"
const ORIGINAL_TRAV := "ORIGINAL_TRAV_IMPORT"
const ORIGINAL_RESTORE := "RESTORE_ME_IMPORT"

var _failures := 0


func _init() -> void:
	print("=== DirectStorage Migration Restore Path Tests ===")
	_test_restore_rejects_manifest_chosen_paths()
	if _failures == 0:
		print("=== MIGRATION RESTORE PATH TESTS PASSED ===")
		quit(0)
	else:
		print("=== MIGRATION RESTORE PATH TESTS FAILED (%d) ===" % _failures)
		quit(1)


func _check(cond: bool, msg: String) -> void:
	if cond:
		print("PASS: " + msg)
	else:
		_failures += 1
		print("FAIL: " + msg)


func _write(path: String, text: String) -> bool:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		print("SETUP FAIL: open %s err=%d" % [path, FileAccess.get_open_error()])
		return false
	f.store_buffer(text.to_utf8_buffer())
	f.close()
	return true


func _read(path: String) -> String:
	if not FileAccess.file_exists(path):
		return "<missing>"
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return "<open-failed>"
	return f.get_buffer(int(f.get_length())).get_string_from_utf8()


func _find_result(results: Array, source: String) -> Dictionary:
	for r in results:
		if str(r.get("source", "")) == source:
			return r
	return {}


func _test_restore_rejects_manifest_chosen_paths() -> void:
	var tmp_abs := ProjectSettings.globalize_path(TMP)
	DirAccess.make_dir_recursive_absolute(tmp_abs)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TMP + "/sub"))

	var setup_ok := true
	setup_ok = _write(TMP + "/hero.png", "PNG-HERO") and setup_ok
	setup_ok = _write(TMP + "/hero.png.import", "ORIGINAL_HERO_IMPORT") and setup_ok
	setup_ok = _write(TMP + "/guarded.gd", SENTINEL) and setup_ok
	setup_ok = _write(TMP + "/trav.png", "TRAV") and setup_ok
	setup_ok = _write(TMP + "/trav.png.import", ORIGINAL_TRAV) and setup_ok
	setup_ok = _write(TMP + "/abs.png", "ABS") and setup_ok
	setup_ok = _write(TMP + "/restore_me.png", "RESTORE_ME") and setup_ok
	setup_ok = _write(TMP + "/restore_me.png.import", ORIGINAL_RESTORE) and setup_ok
	if not setup_ok:
		_cleanup(tmp_abs)
		return

	var entries := [
		{
			"source": TMP + "/hero.png",
			"import": TMP + "/guarded.gd",
			"zip_entry": "imports/0001.import",
			"source_sha256": CORE._sha256_bytes("PNG-HERO".to_utf8_buffer()),
		},
		{
			"source": TMP + "/missing.png",
			"import": TMP + "/missing.png.import",
			"zip_entry": "imports/0002.import",
			"source_sha256": "deadbeef",
		},
		{
			"source": TMP + "/sub/../trav.png",
			"import": TMP + "/sub/../trav.png.import",
			"zip_entry": "imports/0003.import",
			"source_sha256": CORE._sha256_bytes("TRAV".to_utf8_buffer()),
		},
		{
			"source": tmp_abs + "/abs.png",
			"import": tmp_abs + "/abs.png.import",
			"zip_entry": "imports/0004.import",
			"source_sha256": CORE._sha256_bytes("ABS".to_utf8_buffer()),
		},
		{
			"source": TMP + "/restore_me.png",
			"import": TMP + "/restore_me.png.import",
			"zip_entry": "imports/0005.import",
			"source_sha256": CORE._sha256_bytes("RESTORE_ME".to_utf8_buffer()),
		},
	]

	var zip_path := TMP + "/evil_backup.zip"
	var packer := ZIPPacker.new()
	if packer.open(ProjectSettings.globalize_path(zip_path), ZIPPacker.APPEND_CREATE) != OK:
		print("SETUP FAIL: ZIPPacker.open")
		_cleanup(tmp_abs)
		return
	var payload := PAYLOAD.to_utf8_buffer()
	var payloads := {
		"imports/0001.import": payload,
		"imports/0002.import": payload,
		"imports/0003.import": payload,
		"imports/0004.import": payload,
		"imports/0005.import": ORIGINAL_RESTORE.to_utf8_buffer(),
	}
	for e in entries:
		var z: String = str(e["zip_entry"])
		var bytes: PackedByteArray = payloads[z]
		packer.start_file(z)
		packer.write_file(bytes)
		packer.close_file()
	packer.start_file("manifest.json")
	packer.write_file(JSON.stringify({"schema": 1, "entries": entries}, "  ").to_utf8_buffer())
	packer.close_file()
	packer.close()

	# Pre-restore state: the asset is currently migrated, so its sidecar names a .dstex that the
	# rollback is supposed to delete. The ZIP above still holds the original texture sidecar.
	var dstex := "res://.godot/imported/test_restore_orphan.dstex"
	var unrelated_dstex := "res://.godot/imported/test_restore_unrelated.dstex"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))
	var migrated := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"uid://restore_me\"\n"
		+ "path=\"%s\"\n\n" % dstex
		+ "[deps]\n\n"
		+ "source_file=\"%s/restore_me.png\"\n" % TMP
		+ "dest_files=[\"%s\"]\n\n" % dstex
		+ "[params]\n\n"
		+ "compression/mode=0\n"
	)
	if not _write(TMP + "/restore_me.png.import", migrated):
		_cleanup(tmp_abs)
		return
	_write(dstex, "DTEXORPHAN")
	_write(dstex + ".md5", "ORPHANMD5")
	_write(unrelated_dstex, "DTEXOTHER")

	var res: Dictionary = CORE.restore_from_zip(zip_path)
	var results: Array = res.get("results", [])
	var reimport_sources: Array = res.get("reimport_sources", [])

	_check(_read(TMP + "/guarded.gd") == SENTINEL, "a manifest entry cannot overwrite a project script")
	_check(not FileAccess.file_exists(TMP + "/missing.png.import"), "a missing source is skipped, not restored")
	_check(_read(TMP + "/trav.png.import") == ORIGINAL_TRAV, "a traversal destination cannot overwrite the real sidecar")
	_check(not FileAccess.file_exists(tmp_abs + "/abs.png.import"), "an absolute destination is rejected")
	_check(_read(TMP + "/restore_me.png.import") == ORIGINAL_RESTORE, "a valid entry still restores")
	_check(
		reimport_sources.size() == 1 and str(reimport_sources[0]) == TMP + "/restore_me.png",
		"only the restored source is queued for reimport"
	)
	_check(bool(res.get("ok", false)), "the restore call reports the ZIP as processed")

	var restored := 0
	for r in results:
		if str(r.get("status", "")) == "restored":
			restored += 1
	_check(restored == 1, "exactly one entry restores")

	_check(str(_find_result(results, TMP + "/hero.png").get("status", "")) == "failed", "script destination is reported failed")
	_check(str(_find_result(results, TMP + "/missing.png").get("status", "")) == "skipped", "missing source is reported skipped")
	_check(str(_find_result(results, TMP + "/sub/../trav.png").get("status", "")) == "failed", "traversal destination is reported failed")
	_check(str(_find_result(results, tmp_abs + "/abs.png").get("status", "")) == "failed", "absolute destination is reported failed")

	var leftovers: Array[String] = []
	var da := DirAccess.open(TMP)
	if da != null:
		for fn in da.get_files():
			if fn.contains(".tmp."):
				leftovers.append(fn)
	_check(leftovers.is_empty(), "a rejected entry leaves no temporary file")

	# The rollback must delete the artifacts the migration created, and only those.
	_check(not FileAccess.file_exists(dstex), "the rollback deletes the .dstex the migration created")
	_check(not FileAccess.file_exists(dstex + ".md5"), "the rollback deletes that .dstex.md5")
	_check(FileAccess.file_exists(unrelated_dstex), "the rollback leaves an unrelated .dstex alone")

	_cleanup(tmp_abs)


func _cleanup(tmp_abs: String) -> void:
	var names := [
		"hero.png", "hero.png.import", "guarded.gd", "guarded.gd.uid",
		"trav.png", "trav.png.import", "abs.png", "abs.png.import",
		"missing.png.import", "restore_me.png", "restore_me.png.import", "evil_backup.zip",
	]
	for n in names:
		var p: String = TMP + "/" + str(n)
		if FileAccess.file_exists(p):
			var rm := DirAccess.remove_absolute(ProjectSettings.globalize_path(p))
			if rm != OK:
				push_warning("cleanup: could not remove %s (err %d)" % [p, rm])
	for art in ["res://.godot/imported/test_restore_orphan.dstex",
			"res://.godot/imported/test_restore_orphan.dstex.md5",
			"res://.godot/imported/test_restore_unrelated.dstex"]:
		if FileAccess.file_exists(art):
			var arm := DirAccess.remove_absolute(ProjectSettings.globalize_path(art))
			if arm != OK:
				push_warning("cleanup: could not remove %s (err %d)" % [art, arm])
	var da := DirAccess.open(TMP)
	if da != null:
		for fn in da.get_files():
			if fn.contains(".tmp."):
				DirAccess.remove_absolute(ProjectSettings.globalize_path(TMP + "/" + fn))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(TMP + "/sub"))
	DirAccess.remove_absolute(tmp_abs)
	print("cleanup: TMP exists=%s" % str(DirAccess.dir_exists_absolute(tmp_abs)))
