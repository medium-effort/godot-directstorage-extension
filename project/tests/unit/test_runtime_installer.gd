extends GutTest

const Installer = preload("res://addons/directstorage_accelerator/scripts/ds_runtime_installer.gd")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const VERSIONS_JSON_PATH := "res://addons/directstorage_accelerator/runtime_versions.json"
const TMP_DIR := "user://gut_tmp/runtime_installer"


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP_DIR)


func after_each() -> void:
	Helpers.rm_dir_recursive(TMP_DIR)


func test_versions_manifest_and_fallback_parity() -> void:
	var versions := Installer.get_versions()
	assert_true(versions.has("1.3.0"), "baseline 1.3.0 is present")
	for ver in Installer.FALLBACK_VERSIONS:
		assert_true(versions.has(ver), "fallback version %s is present" % ver)
		assert_true(Installer._is_valid_entry(versions[ver]), "entry %s is valid" % ver)

	var text := FileAccess.get_file_as_string(VERSIONS_JSON_PATH)
	assert_false(text.is_empty(), "runtime_versions.json is readable")
	var parsed: Variant = JSON.parse_string(text)
	assert_true(parsed is Dictionary, "runtime_versions.json parses as Dictionary")
	var dict := parsed as Dictionary
	for ver in dict:
		if dict[ver] is Dictionary:
			assert_true(Installer._is_valid_entry(dict[ver]), "manifest entry %s passes _is_valid_entry" % ver)


func test_invalid_entry_rejection() -> void:
	assert_false(Installer._is_valid_entry({
		"url": "http://example.com/pkg.nupkg",
		"sha512": "abc123",
		"size": 1024,
	}), "rejects non-TLS http:// URL")

	assert_false(Installer._is_valid_entry({
		"url": "https://example.com/pkg.nupkg",
		"sha512": "",
		"size": 1024,
	}), "rejects empty sha512")

	assert_false(Installer._is_valid_entry({
		"url": "https://example.com/pkg.nupkg",
		"sha512": "abc123",
		"size": 0,
	}), "rejects non-positive size")


func test_pending_activation_no_pending_and_copy_verified() -> void:
	# When nothing is pending in ADDON_BIN, apply_pending() returns OK cleanly.
	if not FileAccess.file_exists(Installer.PENDING_PATH):
		assert_eq(Installer.apply_pending(), OK, "apply_pending() with no .pending.json returns OK")

	# Exercise _copy_verified and _write_json in user:// without mutating ADDON_BIN.
	var src := TMP_DIR.path_join("dstorage.dll")
	var dst := TMP_DIR.path_join("out/dstorage.dll")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TMP_DIR.path_join("out")))
	var f := FileAccess.open(src, FileAccess.WRITE)
	assert_not_null(f)
	f.store_string("MOCK_DSTORAGE_DLL")
	f.close()

	assert_true(Installer._copy_verified(src, dst), "_copy_verified copies and verifies bytes")
	assert_eq(FileAccess.get_file_as_string(dst), "MOCK_DSTORAGE_DLL")

	var json_path := TMP_DIR.path_join(".runtime.json")
	assert_true(Installer._write_json(json_path, {"active": "1.3.0-test"}))
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(json_path))
	assert_true(parsed is Dictionary and (parsed as Dictionary).get("active") == "1.3.0-test")
