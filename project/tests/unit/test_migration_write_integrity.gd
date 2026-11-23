extends GutTest

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const TEST_DIR := "user://gut_tmp/migration_write_integrity"
const ORIGINAL := "ORIGINAL_IMPORT_CONTENT_0123456789"


func before_each() -> void:
	Helpers.ensure_clean_dir(TEST_DIR)


func after_each() -> void:
	CORE._set_io_faults_for_testing()
	Helpers.rm_dir_recursive(TEST_DIR)


func test_short_write_keeps_original() -> void:
	var path := TEST_DIR.path_join("short.import")
	_write(path, ORIGINAL)

	CORE._set_io_faults_for_testing(8, OK)
	var err := CORE._atomic_write_bytes(path, _new_bytes())
	CORE._set_io_faults_for_testing()

	assert_eq(err, ERR_FILE_CANT_WRITE)
	assert_eq(_read(path), ORIGINAL)
	assert_true(_no_leftovers())


func test_commit_failure_keeps_original() -> void:
	var path := TEST_DIR.path_join("commit.import")
	_write(path, ORIGINAL)

	CORE._set_io_faults_for_testing(0, ERR_BUSY)
	var err := CORE._atomic_write_bytes(path, _new_bytes())
	CORE._set_io_faults_for_testing()

	assert_eq(err, ERR_BUSY)
	assert_true(FileAccess.file_exists(path))
	assert_eq(_read(path), ORIGINAL)
	assert_true(_no_leftovers())


func test_successful_write_replaces() -> void:
	var path := TEST_DIR.path_join("ok.import")
	_write(path, ORIGINAL)
	var expected := _new_bytes()

	var err := CORE._atomic_write_bytes(path, expected)

	assert_eq(err, OK)
	assert_true(FileAccess.file_exists(path))
	assert_eq(FileAccess.get_file_as_bytes(path), expected)
	assert_true(_no_leftovers())
	assert_eq(_files().size(), 1)


func _new_bytes() -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(200)
	data.fill(0x41)
	return data


func _write(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f, "open %s for write" % path)
	f.store_buffer(text.to_utf8_buffer())
	f.close()


func _read(path: String) -> String:
	return FileAccess.get_file_as_bytes(path).get_string_from_utf8()


func _files() -> PackedStringArray:
	var da := DirAccess.open(TEST_DIR)
	return da.get_files() if da != null else PackedStringArray()


func _no_leftovers() -> bool:
	for name in _files():
		if name.contains(".tmp.") or name.contains(".bak."):
			return false
	return true
