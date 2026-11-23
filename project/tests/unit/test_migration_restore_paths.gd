extends GutTest

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const TMP := "res://gut_tmp_restore_paths"
const SENTINEL := "GDS_MIGRATION_SENTINEL"
const PAYLOAD := "PWNED"
const ORIGINAL_TRAV := "ORIGINAL_TRAV_IMPORT"
const ORIGINAL_RESTORE := "RESTORE_ME_IMPORT"

const ORPHAN_DSTEX := "res://.godot/imported/gut_test_restore_orphan.dstex"
const ORPHAN_MD5 := "res://.godot/imported/gut_test_restore_orphan.md5"
const UNRELATED_DSTEX := "res://.godot/imported/gut_test_restore_unrelated.dstex"
const UNRELATED_MD5 := "res://.godot/imported/gut_test_restore_unrelated.md5"


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TMP + "/sub"))
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))


func after_each() -> void:
	for art in [ORPHAN_DSTEX, ORPHAN_MD5, UNRELATED_DSTEX, UNRELATED_MD5]:
		var glob := ProjectSettings.globalize_path(art)
		if FileAccess.file_exists(glob):
			DirAccess.remove_absolute(glob)
	Helpers.rm_dir_recursive(TMP)


func _write(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f, "open %s for write" % path)
	f.store_buffer(text.to_utf8_buffer())
	f.close()


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


func test_restore_rejects_manifest_chosen_paths_and_cleans_orphans() -> void:
	var tmp_abs := ProjectSettings.globalize_path(TMP)
	_write(TMP + "/hero.png", "PNG-HERO")
	_write(TMP + "/hero.png.import", "ORIGINAL_HERO_IMPORT")
	_write(TMP + "/guarded.gd", SENTINEL)
	_write(TMP + "/trav.png", "TRAV")
	_write(TMP + "/trav.png.import", ORIGINAL_TRAV)
	_write(TMP + "/abs.png", "ABS")
	_write(TMP + "/restore_me.png", "RESTORE_ME")
	_write(TMP + "/restore_me.png.import", ORIGINAL_RESTORE)

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
	assert_eq(packer.open(ProjectSettings.globalize_path(zip_path), ZIPPacker.APPEND_CREATE), OK)
	var payload := PAYLOAD.to_utf8_buffer()
	var payloads := {
		"imports/0001.import": payload,
		"imports/0002.import": payload,
		"imports/0003.import": payload,
		"imports/0004.import": payload,
		"imports/0005.import": ORIGINAL_RESTORE.to_utf8_buffer(),
	}
	for e in entries:
		var z := str(e["zip_entry"])
		packer.start_file(z)
		packer.write_file(payloads[z])
		packer.close_file()
	packer.start_file("manifest.json")
	packer.write_file(JSON.stringify({"schema": 1, "entries": entries}, "  ").to_utf8_buffer())
	packer.close_file()
	packer.close()

	var migrated := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"uid://restore_me\"\n"
		+ "path=\"%s\"\n\n" % ORPHAN_DSTEX
		+ "[deps]\n\n"
		+ "source_file=\"%s/restore_me.png\"\n" % TMP
		+ "dest_files=[\"%s\"]\n\n" % ORPHAN_DSTEX
		+ "[params]\n\n"
		+ "compression/mode=0\n"
	)
	_write(TMP + "/restore_me.png.import", migrated)
	_write(ORPHAN_DSTEX, "DTEXORPHAN")
	_write(ORPHAN_MD5, "ORPHANMD5")
	_write(UNRELATED_DSTEX, "DTEXOTHER")
	_write(UNRELATED_MD5, "OTHERMD5")

	var res: Dictionary = CORE.restore_from_zip(zip_path)
	var results: Array = res.get("results", [])
	var reimport_sources: Array = res.get("reimport_sources", [])

	assert_eq(_read(TMP + "/guarded.gd"), SENTINEL)
	assert_false(FileAccess.file_exists(TMP + "/missing.png.import"))
	assert_eq(_read(TMP + "/trav.png.import"), ORIGINAL_TRAV)
	assert_false(FileAccess.file_exists(tmp_abs + "/abs.png.import"))
	assert_eq(_read(TMP + "/restore_me.png.import"), ORIGINAL_RESTORE)
	assert_true(reimport_sources.size() == 1 and str(reimport_sources[0]) == TMP + "/restore_me.png")
	assert_true(bool(res.get("ok", false)))

	var restored := 0
	for r in results:
		if str(r.get("status", "")) == "restored":
			restored += 1
	assert_eq(restored, 1)

	assert_eq(str(_find_result(results, TMP + "/hero.png").get("status", "")), "failed")
	assert_eq(str(_find_result(results, TMP + "/missing.png").get("status", "")), "skipped")
	assert_eq(str(_find_result(results, TMP + "/sub/../trav.png").get("status", "")), "failed")
	assert_eq(str(_find_result(results, tmp_abs + "/abs.png").get("status", "")), "failed")

	var da := DirAccess.open(TMP)
	if da != null:
		for fn in da.get_files():
			assert_false(fn.contains(".tmp."), "no temporary file left behind: " + fn)

	assert_false(FileAccess.file_exists(ORPHAN_DSTEX))
	assert_false(FileAccess.file_exists(ORPHAN_MD5))
	assert_true(FileAccess.file_exists(UNRELATED_DSTEX))
	assert_true(FileAccess.file_exists(UNRELATED_MD5))
