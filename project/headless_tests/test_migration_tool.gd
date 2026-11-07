@tool
extends SceneTree

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")

const TEST_DIR := "res://test_migration_tmp"
const TEST_IMG := "res://test_migration_tmp/hero.png"
const TEST_IMPORT := "res://test_migration_tmp/hero.png.import"
const TEST_CTEX := "res://.godot/imported/test_hero.ctex"
const TEST_UID := "uid://ds_migration_test_uid_98765"


func _init() -> void:
	print("=== Running DirectStorage Texture Migration Tool Tests ===")
	var ok := true

	if not _test_parameter_mapping():
		print("FAIL: Parameter mapping test failed.")
		ok = false

	if not _test_full_migration_lifecycle():
		print("FAIL: Full migration lifecycle test failed.")
		ok = false

	if ok:
		print("=== ALL MIGRATION TESTS PASSED ===")
		quit(0)
	else:
		print("=== MIGRATION TESTS FAILED ===")
		quit(1)


func _test_parameter_mapping() -> bool:
	print("--- Test 1: Parameter Mapping Parity ---")
	# 1. Mode 2 -> VRAM Compressed (Mode 0)
	var res2: Array = CORE.map_texture_params({"compress/mode": "2"})
	if res2[0] == null or res2[0].get("compression/mode") != "0" or not res2[2]:
		print("FAIL: Mode 2 did not map to DirectStorage compression/mode=0")
		return false

	# 2. Mode 3 -> VRAM Uncompressed (Mode 1)
	var res3: Array = CORE.map_texture_params({"compress/mode": "3"})
	if res3[0] == null or res3[0].get("compression/mode") != "1" or not res3[2]:
		print("FAIL: Mode 3 did not map to DirectStorage compression/mode=1")
		return false

	# 3. Mode 5 -> BPTC mapped to Mode 0
	var res5: Array = CORE.map_texture_params({"compress/mode": "5"})
	if res5[0] == null or res5[0].get("compression/mode") != "0" or not res5[2]:
		print("FAIL: Mode 5 did not map to DirectStorage compression/mode=0")
		return false

	# 4. Incompatible modes (0, 1, 4) -> eligible = false
	for m in ["0", "1", "4"]:
		var res_m: Array = CORE.map_texture_params({"compress/mode": m})
		if res_m[0] != null or res_m[2] != false:
			print("FAIL: Incompatible mode %s was unexpectedly marked eligible" % m)
			return false

	print("PASS: Parameter mapping behaves as expected.")
	return true


func _test_full_migration_lifecycle() -> bool:
	print("--- Test 2: Full Migration Lifecycle (Scan, Backup, Apply, Cleanup, Restore) ---")

	# Setup fixture directory and files
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TEST_DIR))
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))

	# Create dummy PNG image (minimum 8-byte PNG header)
	var img_file := FileAccess.open(TEST_IMG, FileAccess.WRITE)
	if img_file == null:
		print("FAIL: Could not create test image file")
		return false
	img_file.store_buffer(PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0]))
	img_file.close()

	# Create initial .import file (Godot texture importer)
	var import_content := (
		"[remap]\n\n"
		+ "importer=\"texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"CompressedTexture2D\"\n"
		+ "uid=\"%s\"\n" % TEST_UID
		+ "path=\"%s\"\n\n" % TEST_CTEX
		+ "[deps]\n\n"
		+ "source_file=\"%s\"\n" % TEST_IMG
		+ "dest_files=[\"%s\"]\n\n" % TEST_CTEX
		+ "[params]\n\n"
		+ "compress/mode=2\n"
		+ "compress/high_quality=false\n"
		+ "mipmaps/generate=true\n"
	)
	var imp_file := FileAccess.open(TEST_IMPORT, FileAccess.WRITE)
	if imp_file == null:
		print("FAIL: Could not create test .import file")
		return false
	imp_file.store_string(import_content)
	imp_file.close()

	# Create dummy .ctex artifact
	var ctex_file := FileAccess.open(TEST_CTEX, FileAccess.WRITE)
	if ctex_file != null:
		ctex_file.store_string("CTEX_DUMMY_DATA")
		ctex_file.close()

	# 1. Scan
	print("Scanning project...")
	var plan: Array[Dictionary] = CORE.scan()
	var test_entry: Dictionary = {}
	for e in plan:
		if str(e.get("source", "")) == TEST_IMG:
			test_entry = e
			break

	if test_entry.is_empty():
		print("FAIL: Scan did not find %s" % TEST_IMG)
		_cleanup_fixtures()
		return false

	if str(test_entry.get("mode", "")) != "adopt" or not bool(test_entry.get("eligible", false)):
		print("FAIL: Entry mode is not adopt / eligible. Got: %s" % test_entry.get("mode"))
		_cleanup_fixtures()
		return false

	if str(test_entry.get("uid", "")) != TEST_UID:
		print("FAIL: Scan extracted wrong UID: %s" % test_entry.get("uid"))
		_cleanup_fixtures()
		return false

	print("PASS: Scan correctly classified test texture.")

	# 2. Create ZIP backup
	print("Creating ZIP backup...")
	var backup_res: Dictionary = CORE.create_zip_backup([test_entry])
	if not bool(backup_res.get("ok", false)):
		print("FAIL: Backup failed: %s" % backup_res.get("error"))
		_cleanup_fixtures()
		return false

	var zip_path: String = backup_res.get("zip_res_path", "")
	if not FileAccess.file_exists(zip_path):
		print("FAIL: Backup ZIP file does not exist at %s" % zip_path)
		_cleanup_fixtures()
		return false

	# Verify .gdignore was placed in backup directory
	if not FileAccess.file_exists(CORE.DEFAULT_BACKUP_DIR.path_join(".gdignore")):
		print("FAIL: .gdignore was not created in backup directory")
		_cleanup_fixtures()
		return false

	print("PASS: ZIP backup created successfully at %s" % zip_path)

	# 3. Apply migration (mutating .import in place)
	print("Applying migration...")
	var apply_res: Dictionary = CORE.apply_migration([test_entry])
	if not bool(apply_res.get("ok", false)):
		print("FAIL: Apply migration returned errors")
		_cleanup_fixtures()
		return false

	# Read modified .import and verify
	var post_cp := ConfigFile.new()
	if post_cp.load(TEST_IMPORT) != OK:
		print("FAIL: Could not load rewritten .import")
		_cleanup_fixtures()
		return false

	var new_importer := str(post_cp.get_value("remap", "importer", ""))
	var new_ver := int(post_cp.get_value("remap", "importer_version", -1))
	var new_uid := str(post_cp.get_value("remap", "uid", ""))
	var new_mode := str(post_cp.get_value("params", "compression/mode", ""))

	if new_importer != CORE.TARGET_IMPORTER:
		print("FAIL: Rewritten importer is '%s', expected '%s'" % [new_importer, CORE.TARGET_IMPORTER])
		_cleanup_fixtures()
		return false

	if new_ver != 0:
		print("FAIL: Rewritten importer_version is %d, expected 0" % new_ver)
		_cleanup_fixtures()
		return false

	if new_uid != TEST_UID:
		print("FAIL: UID was NOT preserved! Got: %s, expected: %s" % [new_uid, TEST_UID])
		_cleanup_fixtures()
		return false

	if new_mode != "0":
		print("FAIL: DirectStorage compression/mode is '%s', expected '0'" % new_mode)
		_cleanup_fixtures()
		return false

	print("PASS: .import was rewritten correctly with 100% UID preservation.")

	# 4. Old artifact cleanup
	print("Testing old artifact cleanup...")
	var del_results := CORE.cleanup_old_artifacts([test_entry])
	if FileAccess.file_exists(TEST_CTEX):
		print("FAIL: Replaced .ctex artifact was not deleted")
		_cleanup_fixtures()
		return false
	print("PASS: Replaced .ctex artifact safely removed.")

	# 5. Restore from ZIP backup
	print("Testing rollback from ZIP backup...")
	var restore_res := CORE.restore_from_zip(zip_path)
	if not bool(restore_res.get("ok", false)):
		print("FAIL: Restore failed: %s" % restore_res.get("error"))
		_cleanup_fixtures()
		return false

	# Check restored .import file
	var restored_cp := ConfigFile.new()
	if restored_cp.load(TEST_IMPORT) != OK:
		print("FAIL: Could not load restored .import")
		_cleanup_fixtures()
		return false

	var restored_importer := str(restored_cp.get_value("remap", "importer", ""))
	var restored_uid := str(restored_cp.get_value("remap", "uid", ""))

	if restored_importer != "texture":
		print("FAIL: Restored importer is '%s', expected 'texture'" % restored_importer)
		_cleanup_fixtures()
		return false

	if restored_uid != TEST_UID:
		print("FAIL: Restored UID mismatch: %s" % restored_uid)
		_cleanup_fixtures()
		return false

	print("PASS: Rollback restored original .import cleanly.")

	_cleanup_fixtures(zip_path)
	return true


func _cleanup_fixtures(zip_path: String = "") -> void:
	if FileAccess.file_exists(TEST_IMG):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(TEST_IMG))
	if FileAccess.file_exists(TEST_IMPORT):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(TEST_IMPORT))
	if FileAccess.file_exists(TEST_CTEX):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(TEST_CTEX))
	var dir_abs := ProjectSettings.globalize_path(TEST_DIR)
	if DirAccess.dir_exists_absolute(dir_abs):
		DirAccess.remove_absolute(dir_abs)
	if not zip_path.is_empty() and FileAccess.file_exists(zip_path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(zip_path))

