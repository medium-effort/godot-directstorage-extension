@tool
extends SceneTree

## Write-integrity regression suite for the migration tool's .import writer.
##
## Run with:
##   godot --headless --path project --script res://headless_tests/test_migration_write_integrity.gd
##
## The helper is path-agnostic, so the fixtures live under user:// and never touch the
## project tree or trigger an editor filesystem rescan.

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")

const TEST_DIR := "user://ds_migration_write_integrity"
const ORIGINAL := "ORIGINAL_IMPORT_CONTENT_0123456789"

var failures := 0


func _init() -> void:
	print("=== Running DirectStorage Migration Write-Integrity Tests ===")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEST_DIR))

	_run("short write keeps the original", _test_short_write_keeps_original())
	_run("failed commit keeps the original", _test_commit_failure_keeps_original())
	_run("successful write replaces the file", _test_successful_write_replaces())

	_clean()
	if failures == 0:
		print("=== ALL MIGRATION WRITE-INTEGRITY TESTS PASSED ===")
		quit(0)
	else:
		print("=== MIGRATION WRITE-INTEGRITY TESTS FAILED ===")
		quit(1)


func _test_short_write_keeps_original() -> bool:
	_clean()
	var path := TEST_DIR.path_join("short.import")
	_write(path, ORIGINAL)

	# Drop 8 bytes from the temporary while still reporting success: the silent
	# short write the length backstop exists for.
	CORE._set_io_faults_for_testing(8, OK)
	var err := CORE._atomic_write_bytes(path, _new_bytes())
	CORE._set_io_faults_for_testing()

	var ok := true
	if err != ERR_FILE_CANT_WRITE:
		print("FAIL: short write returned %d (%s), expected ERR_FILE_CANT_WRITE" % [err, error_string(err)])
		ok = false
	if _read(path) != ORIGINAL:
		print("FAIL: short write replaced the destination (len=%d, expected the original %d bytes)" % [_read(path).length(), ORIGINAL.length()])
		ok = false
	if not _no_leftovers():
		ok = false
	return ok


func _test_commit_failure_keeps_original() -> bool:
	_clean()
	var path := TEST_DIR.path_join("commit.import")
	_write(path, ORIGINAL)

	CORE._set_io_faults_for_testing(0, ERR_BUSY)
	var err := CORE._atomic_write_bytes(path, _new_bytes())
	CORE._set_io_faults_for_testing()

	var ok := true
	if err != ERR_BUSY:
		print("FAIL: failed commit returned %d (%s), expected ERR_BUSY" % [err, error_string(err)])
		ok = false
	if not FileAccess.file_exists(path):
		print("FAIL: failed commit deleted the destination")
		ok = false
	elif _read(path) != ORIGINAL:
		print("FAIL: failed commit changed the destination")
		ok = false
	if not _no_leftovers():
		ok = false
	return ok


func _test_successful_write_replaces() -> bool:
	_clean()
	var path := TEST_DIR.path_join("ok.import")
	_write(path, ORIGINAL)
	var expected := _new_bytes()

	var err := CORE._atomic_write_bytes(path, expected)

	var ok := true
	if err != OK:
		print("FAIL: successful write returned %d (%s)" % [err, error_string(err)])
		ok = false
	if not FileAccess.file_exists(path):
		print("FAIL: successful write left no destination")
		ok = false
	elif FileAccess.get_file_as_bytes(path) != expected:
		print("FAIL: successful write content mismatch (len=%d, expected %d)" % [FileAccess.get_file_as_bytes(path).size(), expected.size()])
		ok = false
	if not _no_leftovers():
		ok = false
	if _files().size() != 1:
		print("FAIL: expected exactly one file in the fixture directory, got %d" % _files().size())
		ok = false
	return ok


func _new_bytes() -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(200)
	data.fill(0x41)
	return data


func _run(label: String, ok: bool) -> void:
	if ok:
		print("PASS: %s" % label)
	else:
		print("FAIL: %s" % label)
		failures += 1


func _write(res_path: String, text: String) -> void:
	var f := FileAccess.open(res_path, FileAccess.WRITE)
	if f == null:
		print("FAIL: could not create fixture %s (%s)" % [res_path, error_string(FileAccess.get_open_error())])
		failures += 1
		return
	f.store_buffer(text.to_utf8_buffer())
	f.close()


func _read(res_path: String) -> String:
	return FileAccess.get_file_as_bytes(res_path).get_string_from_utf8()


func _files() -> PackedStringArray:
	var da := DirAccess.open(TEST_DIR)
	return da.get_files() if da != null else PackedStringArray()


func _no_leftovers() -> bool:
	var bad: Array[String] = []
	for name in _files():
		if name.contains(".tmp.") or name.contains(".bak."):
			bad.append(name)
	if bad.is_empty():
		return true
	print("FAIL: leftover temporaries: %s" % ", ".join(bad))
	return false


func _clean() -> void:
	var da := DirAccess.open(TEST_DIR)
	if da == null:
		return
	for name in da.get_files():
		da.remove(name)
