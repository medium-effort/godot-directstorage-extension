@tool
extends SceneTree

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")

const TEST_DIR := "res://test_migration_tmp"
const TEST_IMG := "res://test_migration_tmp/hero.png"
const TEST_IMPORT := "res://test_migration_tmp/hero.png.import"
const TEST_CTEX := "res://.godot/imported/test_hero.ctex"
const TEST_UID := "uid://ds_migration_test_uid_98765"

# Importer-contract fixtures. The v5 sidecar is what Godot writes after a reimport with the
# current importer; the v4 sidecar is the legacy shape whose compression/source key the
# migration tool must translate on upgrade.
const IMPORTER_SCRIPT_PATH := "res://addons/directstorage_accelerator/scripts/ds_importer.gd"
const CORE_SCRIPT_PATH := "res://addons/directstorage_accelerator/tools/ds_migration_core.gd"
const V5_DIR := "res://test_migration_v5_tmp"
const V5_PNG := "res://test_migration_v5_tmp/v5.png"
const V5_IMPORT := "res://test_migration_v5_tmp/v5.png.import"
const V5_ARTIFACT := "res://.godot/imported/test_migration_v5.dstex"
const V5_UID := "uid://ds_migration_v5_uid_98765"
const V4_PNG := "res://test_migration_v5_tmp/v4.png"
const V4_IMPORT := "res://test_migration_v5_tmp/v4.png.import"
const V4_ARTIFACT := "res://.godot/imported/test_migration_v4.dstex"
const V4_UID := "uid://ds_migration_v4_uid_98765"


func _init() -> void:
	print("=== Running DirectStorage Texture Migration Tool Tests ===")
	var ok := true

	if not _test_parameter_mapping():
		print("FAIL: Parameter mapping test failed.")
		ok = false

	if not _test_full_migration_lifecycle():
		print("FAIL: Full migration lifecycle test failed.")
		ok = false

	if not _test_importer_contract_parity():
		print("FAIL: Importer contract parity test failed.")
		ok = false

	if not _test_v5_verification_and_scan():
		print("FAIL: v5 verification and scan classification test failed.")
		ok = false

	if not _test_v5_option_emission():
		print("FAIL: v5 option emission test failed.")
		ok = false

	if not _test_cleanup_scope():
		print("FAIL: cleanup scope test failed.")
		ok = false

	if not _test_upgrade_parameter_preservation():
		print("FAIL: upgrade parameter preservation test failed.")
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


# ==============================================================================
# Importer-Contract Parity (v5)
# ==============================================================================

func _test_importer_contract_parity() -> bool:
	print("--- Test 3: Importer Contract Parity ---")
	# The importer cannot be instantiated outside editor context ("Class 'EditorImportPlugin'
	# can only be instantiated by editor"), so the declared version is read from the script's
	# constant map instead of an instance.
	var importer_script: Script = load(IMPORTER_SCRIPT_PATH)
	if importer_script == null:
		print("FAIL: Could not load %s" % IMPORTER_SCRIPT_PATH)
		return false

	var importer_consts: Dictionary = importer_script.get_script_constant_map()
	if not importer_consts.has("FORMAT_VERSION"):
		print("FAIL: importer does not declare FORMAT_VERSION")
		return false
	var format_version := int(importer_consts["FORMAT_VERSION"])
	if format_version != 5:
		print("FAIL: importer FORMAT_VERSION is %d, expected 5" % format_version)
		return false

	var core_script: Script = load(CORE_SCRIPT_PATH)
	if core_script == null:
		print("FAIL: Could not load %s" % CORE_SCRIPT_PATH)
		return false
	var core_consts: Dictionary = core_script.get_script_constant_map()
	if not core_consts.has("TARGET_VERSION"):
		print("FAIL: DSMigrationCore does not declare TARGET_VERSION")
		return false
	var target_version := int(core_consts["TARGET_VERSION"])
	if target_version != format_version:
		print("FAIL: TARGET_VERSION (%d) != importer FORMAT_VERSION (%d)" % [target_version, format_version])
		return false
	if target_version != 5:
		print("FAIL: TARGET_VERSION is %d, expected 5; an importer bump must update this suite and its fixtures" % target_version)
		return false

	print("PASS: migration target and importer format version agree at 5.")
	return true


func _test_v5_verification_and_scan() -> bool:
	print("--- Test 4: v5 Verification and Scan Classification ---")
	_cleanup_v5_fixtures()
	if not _stage_v5_fixtures():
		_cleanup_v5_fixtures()
		return false

	var core_script: Script = load(CORE_SCRIPT_PATH)
	var plan: Array = core_script.callv("scan", [])
	var v5_entry := _find_entry(plan, V5_PNG)
	var v4_entry := _find_entry(plan, V4_PNG)
	if v5_entry.is_empty() or v4_entry.is_empty():
		print("FAIL: scan() did not classify the staged fixtures (plan size %d)" % plan.size())
		_cleanup_v5_fixtures()
		return false

	# 1. A sidecar written by the current importer must verify clean.
	var results: Array = core_script.callv("verify_migration", [[
		{"source": V5_PNG, "uid": V5_UID},
		{"source": V4_PNG, "uid": V4_UID},
	]])
	var v5_result := _find_entry(results, V5_PNG)
	var v4_result := _find_entry(results, V4_PNG)
	if v5_result.is_empty() or v4_result.is_empty():
		print("FAIL: verify_migration() returned no result for the staged fixtures")
		_cleanup_v5_fixtures()
		return false
	if not bool(v5_result.get("verified", false)):
		print("FAIL: v5 sidecar failed verification: %s" % str(v5_result.get("detail", "")))
		_cleanup_v5_fixtures()
		return false

	# 2. A sidecar from the previous importer version must be rejected, not silently accepted.
	if bool(v4_result.get("verified", true)):
		print("FAIL: v4 sidecar was accepted by verify_migration()")
		_cleanup_v5_fixtures()
		return false
	var expected_detail := "importer_version=4 (expected 5)"
	if not str(v4_result.get("detail", "")).contains(expected_detail):
		print("FAIL: v4 rejection detail does not name the version mismatch: %s" % str(v4_result.get("detail", "")))
		_cleanup_v5_fixtures()
		return false

	# 3. A version-4 asset must be offered as an upgrade.
	if str(v4_entry.get("mode", "")) != "upgrade" or not bool(v4_entry.get("eligible", false)):
		print("FAIL: v4 asset classified '%s' (eligible=%s), expected upgrade" % [v4_entry.get("mode", ""), v4_entry.get("eligible", false)])
		_cleanup_v5_fixtures()
		return false

	# 4. Positive control: a current asset stays up_to_date.
	if str(v5_entry.get("mode", "")) != "up_to_date":
		print("FAIL: v5 asset classified '%s', expected up_to_date" % str(v5_entry.get("mode", "")))
		_cleanup_v5_fixtures()
		return false
	if int(v5_entry.get("importer_version", -1)) != 5:
		print("FAIL: v5 fixture reports importer_version=%s" % str(v5_entry.get("importer_version", "")))
		_cleanup_v5_fixtures()
		return false

	# 5. The upgrade entry's default parameters must describe its own legacy sidecar, so the
	#    dialog lists a normal map as a normal map instead of "VRAM Compressed".
	var mapped: Dictionary = v4_entry.get("mapped_params", {})
	if str(mapped.get("compress/normal_map", "")) != "1" or str(mapped.get("compression/color_space", "")) != "0":
		print("FAIL: upgrade entry parameters do not reflect compression/source=2: %s" % str(mapped))
		_cleanup_v5_fixtures()
		return false

	_cleanup_v5_fixtures()
	print("PASS: v5 verification, upgrade classification and upgrade defaults behave as specified.")
	return true


func _test_v5_option_emission() -> bool:
	print("--- Test 5: v5 Option Emission ---")
	var core_script: Script = load(CORE_SCRIPT_PATH)

	# 1. Godot's Enable(1) normal map -> linear + normal + optimized, without the legacy key.
	var normal_result: Array = core_script.callv("map_texture_params", [{"compress/mode": "2", "compress/normal_map": "1"}])
	var normal_params: Dictionary = normal_result[0]
	if str(normal_params.get("compression/color_space", "")) != "0" \
			or str(normal_params.get("compress/normal_map", "")) != "1" \
			or str(normal_params.get("compress/channel_pack", "")) != "1" \
			or normal_params.has("compression/source"):
		print("FAIL: Godot normal map did not map to linear/normal/optimized: %s" % str(normal_params))
		return false

	# 2. Godot's Disabled(2) must not invert into "enabled".
	var disabled_result: Array = core_script.callv("map_texture_params", [{"compress/mode": "2", "compress/normal_map": "2", "compress/channel_pack": "1"}])
	var disabled_params: Dictionary = disabled_result[0]
	if str(disabled_params.get("compress/normal_map", "")) != "0" \
			or str(disabled_params.get("compression/color_space", "")) != "1" \
			or str(disabled_params.get("compress/channel_pack", "")) != "1":
		print("FAIL: Godot Disabled(2) normal map inverted or lost channel packing: %s" % str(disabled_params))
		return false

	# 3. BPTC mode -> BPTC output.
	var bptc_result: Array = core_script.callv("map_texture_params", [{"compress/mode": "5"}])
	var bptc_params: Dictionary = bptc_result[0]
	if str(bptc_params.get("compression/mode", "")) != "0" or str(bptc_params.get("compression/high_quality", "")) != "true":
		print("FAIL: BPTC mode did not request compression/high_quality: %s" % str(bptc_params))
		return false

	# 4. Uncompressed output must not claim BPTC.
	var uncompressed_result: Array = core_script.callv("map_texture_params", [{"compress/mode": "3", "compress/high_quality": "true"}])
	var uncompressed_params: Dictionary = uncompressed_result[0]
	if str(uncompressed_params.get("compression/high_quality", "")) != "false":
		print("FAIL: uncompressed mode claimed high quality: %s" % str(uncompressed_params))
		return false

	# 5. Upgrading a legacy normal-map sidecar translates and removes compression/source.
	var upgraded := CORE.rewrite_import(_v4_import_text().to_utf8_buffer(), "upgrade", {})
	var upgraded_text := upgraded.get_string_from_utf8()
	if not upgraded_text.contains("compress/normal_map=1") \
			or not upgraded_text.contains("compression/color_space=0") \
			or not upgraded_text.contains("compress/channel_pack=1"):
		print("FAIL: upgrade did not translate compression/source=2 into the v5 triple:\n%s" % upgraded_text)
		return false
	if upgraded_text.contains("compression/source="):
		print("FAIL: upgrade left the legacy compression/source key behind:\n%s" % upgraded_text)
		return false
	if upgraded_text.count("compression/mode=0") != 1:
		print("FAIL: upgrade did not preserve exactly one compression/mode key:\n%s" % upgraded_text)
		return false
	if not upgraded_text.contains("importer_version=0"):
		print("FAIL: upgrade did not force a reimport via importer_version=0:\n%s" % upgraded_text)
		return false

	# 6. The action table emits the same contract the dialog uses.
	var normal_action := 3
	var adopt_params: Variant = core_script.callv("action_mapped_params", [normal_action])
	if not (adopt_params is Dictionary):
		print("FAIL: DSMigrationCore.action_mapped_params() is missing")
		return false
	if str(adopt_params.get("compress/normal_map", "")) != "1" or str(adopt_params.get("compression/color_space", "")) != "0":
		print("FAIL: Normal Map action does not emit a linear normal map: %s" % str(adopt_params))
		return false
	var adopted := CORE.rewrite_import(_v5_import_text().to_utf8_buffer(), "adopt", adopt_params)
	var adopted_text := adopted.get_string_from_utf8()
	for key in ["compression/color_space=0", "compress/normal_map=1", "compress/channel_pack=1"]:
		if not adopted_text.contains(key):
			print("FAIL: adopt output missing '%s':\n%s" % [key, adopted_text])
			return false
	if adopted_text.contains("compression/source="):
		print("FAIL: adopt output still writes compression/source:\n%s" % adopted_text)
		return false
	if not adopted_text.contains("uid=\"%s\"" % V5_UID) or not adopted_text.contains("importer_version=0"):
		print("FAIL: adopt output lost the UID or the forced reimport:\n%s" % adopted_text)
		return false

	# 7. Default action derivation used by the dialog tree.
	var derived_normal: Variant = core_script.callv("action_for_params", [adopt_params])
	var derived_uncompressed: Variant = core_script.callv("action_for_params", [{"compression/mode": "1"}])
	var derived_default: Variant = core_script.callv("action_for_params", [{}])
	if derived_normal != 3 or derived_uncompressed != 2 or derived_default != 1:
		print("FAIL: action_for_params() returned %s/%s/%s, expected 3/2/1" % [str(derived_normal), str(derived_uncompressed), str(derived_default)])
		return false

	print("PASS: v5 option emission, upgrade translation, adopt output and action table agree.")
	return true


func _test_cleanup_scope() -> bool:
	print("--- Test 6: Cleanup Scope ---")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))

	# Godot names an artifact's checksum from its stem: `x.ctex` pairs with `x.md5`.
	var superseded := "res://.godot/imported/test_cleanup_old.ctex"
	var superseded_md5 := "res://.godot/imported/test_cleanup_old.md5"
	var live := "res://.godot/imported/test_cleanup_live.dstex"
	var live_md5 := "res://.godot/imported/test_cleanup_live.md5"
	var keep_me := "res://.godot/imported/test_cleanup_keep.ctex"
	var keep_me_md5 := "res://.godot/imported/test_cleanup_keep.md5"
	var unverified := "res://.godot/imported/test_cleanup_unverified.ctex"
	var outside := "res://test_migration_v5_tmp/test_cleanup_outside.ctex"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://test_migration_v5_tmp"))
	for p in [superseded, superseded_md5, live, live_md5, keep_me, keep_me_md5, unverified, outside]:
		_write_text_file(p, "CLEANUP_FIXTURE")

	var ok := true

	# 1. A live .dstex is a current destination, never cleanup material.
	var del_live: Array = CORE.cleanup_old_artifacts([{
		"source": "res://test_assets/test.png",
		"old_dests": [live],
	}])
	var deleted_live: Array = del_live[0].get("deleted", [])
	ok = _check(FileAccess.file_exists(live), "live .dstex survives cleanup") and ok
	ok = _check(FileAccess.file_exists(live_md5), "live .dstex checksum survives cleanup") and ok
	ok = _check(deleted_live.is_empty(), "nothing reported deleted for a live .dstex") and ok

	# 2. Positive control: a superseded .ctex and its checksum are still removed, and reported.
	var del_ctex: Array = CORE.cleanup_old_artifacts([{
		"source": "res://test_assets/test.png",
		"old_dests": [superseded],
	}])
	var deleted_ctex: Array = del_ctex[0].get("deleted", [])
	ok = _check(not FileAccess.file_exists(superseded), "superseded .ctex is deleted") and ok
	ok = _check(not FileAccess.file_exists(superseded_md5), "superseded .ctex checksum is deleted") and ok
	ok = _check(deleted_ctex.size() == 2, "both deleted paths are reported") and ok

	# 3. The new_dests keep-set: a .ctex that is a current destination survives with its checksum.
	var del_keep: Array = CORE.cleanup_old_artifacts([{
		"source": "res://test_assets/test.png",
		"old_dests": [keep_me],
		"new_dests": [keep_me],
	}])
	var deleted_keep: Array = del_keep[0].get("deleted", [])
	ok = _check(FileAccess.file_exists(keep_me), "a path listed in new_dests is kept") and ok
	ok = _check(FileAccess.file_exists(keep_me_md5), "a kept artifact keeps its checksum") and ok
	ok = _check(deleted_keep.is_empty(), "a kept path is not reported deleted") and ok

	# 4. Containment: nothing outside res://.godot/imported is ever touched.
	var del_out: Array = CORE.cleanup_old_artifacts([{
		"source": "res://test_assets/test.png",
		"old_dests": [outside],
	}])
	var deleted_out: Array = del_out[0].get("deleted", [])
	ok = _check(FileAccess.file_exists(outside), "a .ctex outside .godot/imported is untouched") and ok
	ok = _check(deleted_out.is_empty(), "an outside path is not reported deleted") and ok

	# 5. The verified-subset filter keeps exactly the verified entries, intact, and carries the
	#    verification's new_dests so the keep-set sees what the production call path supplies.
	var selected := [
		{"source": "res://a.png", "old_dests": [superseded]},
		{"source": "res://b.png", "old_dests": [unverified]},
		{"source": "res://c.png", "old_dests": [outside]},
	]
	var verify_results := [
		{"source": "res://a.png", "verified": true, "new_dests": [live]},
		{"source": "res://b.png", "verified": false, "new_dests": [live]},
		{"source": "res://c.png", "verified": true, "new_dests": []},
	]
	var filtered: Array = CORE.filter_verified_entries(selected, verify_results)
	ok = _check(filtered.size() == 2, "filter keeps exactly the verified entries") and ok
	ok = _check(not filtered.is_empty() and filtered[0].has("old_dests"), "filter keeps the scan entries intact") and ok
	ok = _check(not filtered.is_empty() and filtered[0].get("new_dests", []).has(live), "filter carries the verification's new_dests") and ok
	ok = _check(not selected[0].has("new_dests"), "filter does not mutate the caller's entry") and ok

	# 6. Composition: the caller's real path leaves the unverified entry alone.
	_write_text_file(superseded, "CLEANUP_FIXTURE")
	_write_text_file(superseded_md5, "CLEANUP_FIXTURE")
	_write_text_file(unverified, "CLEANUP_FIXTURE")
	CORE.cleanup_old_artifacts(CORE.filter_verified_entries(selected, verify_results))
	ok = _check(FileAccess.file_exists(unverified), "an unverified entry's .ctex is not cleaned") and ok
	ok = _check(not FileAccess.file_exists(superseded), "a verified entry's superseded .ctex is cleaned") and ok
	ok = _check(not FileAccess.file_exists(superseded_md5), "the cleaned .ctex takes its checksum with it") and ok

	for p in [superseded, superseded_md5, live, live_md5, keep_me, keep_me_md5, unverified, outside]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(p))

	if ok:
		print("PASS: cleanup removes only verified, superseded .ctex artifacts.")
	return ok


func _test_upgrade_parameter_preservation() -> bool:
	print("--- Test 7: Upgrade Parameter Preservation ---")
	_cleanup_v5_fixtures()
	if not _stage_v5_fixtures():
		_cleanup_v5_fixtures()
		return false

	var v4u_png := "res://test_migration_v5_tmp/v4u.png"
	var v4u_import := "res://test_migration_v5_tmp/v4u.png.import"
	var v4u_artifact := "res://.godot/imported/test_migration_v4u.dstex"
	var repair_png := "res://test_migration_v5_tmp/repair.png"
	var repair_import := "res://test_migration_v5_tmp/repair.png.import"

	var png_bytes := PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0])
	var ok := true
	if not _write_bytes_file(v4u_png, png_bytes):
		_cleanup_v5_fixtures()
		return false
	if not _write_bytes_file(repair_png, png_bytes):
		_cleanup_v5_fixtures()
		return false
	_write_text_file(v4u_artifact, "DTEX")

	var v4u_text := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=4\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"uid://ds_migration_v4u_uid\"\n"
		+ "path=\"%s\"\n\n" % v4u_artifact
		+ "[deps]\n\n"
		+ "files=[\"%s\"]\n" % v4u_artifact
		+ "source_file=\"%s\"\n" % v4u_png
		+ "dest_files=[\"%s\"]\n\n" % v4u_artifact
		+ "[params]\n\n"
		+ "compression/mode=1\n"
		+ "compression/gdeflate_level=7\n"
		+ "compatibility/as_image_texture=true\n"
		+ "mipmaps/generate=false\n"
	)
	var repair_text := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"uid://ds_migration_repair_uid\"\n"
		+ "path=\"res://.godot/imported/test_migration_repair_missing.dstex\"\n\n"
		+ "[deps]\n\n"
		+ "source_file=\"%s\"\n" % repair_png
		+ "dest_files=[\"res://.godot/imported/test_migration_repair_missing.dstex\"]\n\n"
		+ "[params]\n\n"
		+ "compression/mode=1\n"
		+ "compression/high_quality=false\n"
	)
	_write_text_file(v4u_import, v4u_text)
	_write_text_file(repair_import, repair_text)

	# 1. scan() must label each row from the parameters its own sidecar states.
	var core_script: Script = load(CORE_SCRIPT_PATH)
	var plan: Array = core_script.callv("scan", [])
	var up := _find_entry(plan, v4u_png)
	var rep := _find_entry(plan, repair_png)
	ok = _check(not up.is_empty() and str(up.get("mode", "")) == "upgrade", "v4 mode=1 sidecar scans as upgrade") and ok
	ok = _check(str(up.get("mapped_params", {}).get("compression/mode", "")) == "1", "upgrade scan keeps the sidecar's compression/mode") and ok
	ok = _check(int(core_script.callv("action_for_params", [up.get("mapped_params", {})])) == 2, "upgrade row labels as VRAM Uncompressed") and ok
	ok = _check(not rep.is_empty() and str(rep.get("mode", "")) == "repair", "missing-artifact v5 sidecar scans as repair") and ok
	ok = _check(int(core_script.callv("action_for_params", [rep.get("mapped_params", {})])) == 2, "repair row labels from its own params") and ok
	ok = _check(str(up.get("base_mode", "")) == "upgrade" and str(rep.get("base_mode", "")) == "repair", "scan records base_mode") and ok

	# 2. An override rewrites in place and leaves every unlisted parameter alone.
	var overridden: String = CORE.rewrite_import(v4u_text.to_utf8_buffer(), "upgrade", {"compression/mode": "1"}).get_string_from_utf8()
	ok = _check(_count_key(overridden, "compression/mode") == 1, "override rewrites in place, no duplicate key") and ok
	ok = _check(_param_value(overridden, "compression/mode") == "1", "the caller's mode wins") and ok
	ok = _check(_param_value(overridden, "compression/gdeflate_level") == "7", "unlisted gdeflate_level preserved") and ok
	ok = _check(_param_value(overridden, "compatibility/as_image_texture") == "true", "as_image_texture preserved") and ok
	ok = _check(_param_value(overridden, "mipmaps/generate") == "false", "mipmaps/generate preserved") and ok
	ok = _check(overridden.contains("importer_version=0"), "upgrade forces a reimport") and ok

	# 3. The user's choice beats the legacy compression/source translation.
	var trap_text := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=4\n"
		+ "uid=\"uid://ds_migration_trap_uid\"\n\n"
		+ "[params]\n\n"
		+ "compression/mode=1\n"
		+ "compression/source=0\n"
		+ "compression/gdeflate_level=1\n"
	)
	var trapped: String = CORE.rewrite_import(trap_text.to_utf8_buffer(), "upgrade", CORE.action_mapped_params(CORE.ACTION_NORMAL_MAP)).get_string_from_utf8()
	ok = _check(_param_value(trapped, "compress/normal_map") == "1", "a user normal map beats the legacy source") and ok
	ok = _check(_param_value(trapped, "compression/color_space") == "0", "a user normal map sets linear colour space") and ok
	ok = _check(not trapped.contains("compression/source="), "the legacy source key is removed in the trap case") and ok
	ok = _check(_param_value(trapped, "compression/mode") == "1", "a normal-map action leaves an uncompressed asset's mode alone") and ok

	# 4. The mode predicate used by the dialog.
	ok = _check(CORE.action_mode_for_entry({"mode": "upgrade", "base_mode": "upgrade"}, 2) == "upgrade", "an upgrade row stays on upgrade") and ok
	ok = _check(CORE.action_mode_for_entry({"mode": "skip", "base_mode": "upgrade"}, 2) == "upgrade", "skip then action restores upgrade") and ok
	ok = _check(CORE.action_mode_for_entry({"mode": "repair", "base_mode": "repair"}, 3) == "upgrade", "repair plus action promotes to upgrade") and ok
	ok = _check(CORE.action_mode_for_entry({"mode": "adopt", "base_mode": "adopt"}, 2) == "adopt", "an adopt row stays adopt") and ok
	ok = _check(CORE.action_mode_for_entry({"base_mode": "adopt"}, 0) == "skip", "the skip action writes skip") and ok

	# 5. An untouched row only moves importer_version - no parameter churn.
	var custom_v5 := (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"uid://ds_migration_custom_uid\"\n\n"
		+ "[params]\n\n"
		+ "compression/mode=0\n"
		+ "compression/high_quality=false\n"
		+ "compression/color_space=1\n"
		+ "compress/normal_map=0\n"
		+ "compress/channel_pack=0\n"
		+ "compression/gdeflate_level=3\n"
		+ "compatibility/as_image_texture=true\n"
		+ "mipmaps/generate=true\n"
		+ "process/size_limit=0\n"
	)
	var own_params := {
		"compression/mode": "0",
		"compression/high_quality": "false",
		"compression/color_space": "1",
		"compress/normal_map": "0",
		"compress/channel_pack": "0",
		"compression/gdeflate_level": "3",
		"compatibility/as_image_texture": "true",
	}
	var untouched: String = CORE.rewrite_import(custom_v5.to_utf8_buffer(), "upgrade", own_params).get_string_from_utf8()
	var diff := _changed_lines(custom_v5, untouched)
	ok = _check(diff.size() == 1, "an untouched upgrade changes exactly one line") and ok
	ok = _check(diff.size() == 1 and diff[0].contains("importer_version"), "the one changed line is importer_version") and ok

	# 6. Composition: the dialog's output for an upgrade row keeps unlisted keys.
	var composed_mode: String = CORE.action_mode_for_entry({"mode": "upgrade", "base_mode": "upgrade"}, CORE.ACTION_VRAM_UNCOMPRESSED)
	var composed: String = CORE.rewrite_import(custom_v5.to_utf8_buffer(), composed_mode, CORE.action_mapped_params(CORE.ACTION_VRAM_UNCOMPRESSED)).get_string_from_utf8()
	ok = _check(composed_mode == "upgrade", "composition keeps the upgrade mode") and ok
	ok = _check(_param_value(composed, "compression/mode") == "1", "composition applies the chosen action") and ok
	ok = _check(_param_value(composed, "compression/gdeflate_level") == "3", "composition keeps the asset's gdeflate_level") and ok
	ok = _check(_param_value(composed, "compatibility/as_image_texture") == "true", "composition keeps the asset's as_image_texture") and ok
	ok = _check(_param_value(composed, "mipmaps/generate") == "true", "composition keeps mipmaps/generate") and ok
	ok = _check(_param_value(composed, "process/size_limit") == "0", "composition keeps process/size_limit") and ok

	for p in [v4u_png, v4u_import, v4u_artifact, repair_png, repair_import]:
		if FileAccess.file_exists(p):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(p))

	if ok:
		print("PASS: upgrade rows preserve their own parameters and only change what was chosen.")
	_cleanup_v5_fixtures()
	return ok


func _check(cond: bool, msg: String) -> bool:
	if cond:
		print("PASS: " + msg)
	else:
		print("FAIL: " + msg)
	return cond


func _write_bytes_file(path: String, bytes: PackedByteArray) -> bool:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		print("FAIL: could not create %s" % path)
		return false
	f.store_buffer(bytes)
	f.close()
	return true


## Value of `key` inside [params], or "" when absent.
func _param_value(text: String, key: String) -> String:
	var in_params := false
	for ln in text.split("\n"):
		var t := str(ln).strip_edges()
		if t.begins_with("[") and t.ends_with("]"):
			in_params = (t == "[params]")
			continue
		if in_params and t.begins_with(key + "="):
			return t.substr(key.length() + 1).strip_edges()
	return ""


## How many times `key` appears inside [params].
func _count_key(text: String, key: String) -> int:
	var count := 0
	var in_params := false
	for ln in text.split("\n"):
		var t := str(ln).strip_edges()
		if t.begins_with("[") and t.ends_with("]"):
			in_params = (t == "[params]")
			continue
		if in_params and t.begins_with(key + "="):
			count += 1
	return count


## Lines that differ between two documents, as "after" text.
func _changed_lines(before: String, after: String) -> Array[String]:
	var a := before.split("\n")
	var b := after.split("\n")
	var out: Array[String] = []
	for i in mini(a.size(), b.size()):
		if str(a[i]) != str(b[i]):
			out.append(str(b[i]))
	if a.size() != b.size():
		out.append("line-count %d -> %d" % [a.size(), b.size()])
	return out


func _find_entry(entries: Array, source: String) -> Dictionary:
	for e in entries:
		if str(e.get("source", "")) == source:
			return e
	return {}


func _v4_import_text() -> String:
	return (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=4\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"%s\"\n" % V4_UID
		+ "path=\"%s\"\n\n" % V4_ARTIFACT
		+ "[deps]\n\n"
		+ "files=[\"%s\"]\n" % V4_ARTIFACT
		+ "source_file=\"%s\"\n" % V4_PNG
		+ "dest_files=[\"%s\"]\n\n" % V4_ARTIFACT
		+ "[params]\n\n"
		+ "compression/mode=0\n"
		+ "compression/source=2\n"
		+ "compression/gdeflate_level=1\n"
		+ "compatibility/as_image_texture=false\n"
	)


func _v5_import_text() -> String:
	return (
		"[remap]\n\n"
		+ "importer=\"directstorage_texture\"\n"
		+ "importer_version=5\n"
		+ "type=\"Texture2D\"\n"
		+ "uid=\"%s\"\n" % V5_UID
		+ "path=\"%s\"\n\n" % V5_ARTIFACT
		+ "[deps]\n\n"
		+ "files=[\"%s\"]\n" % V5_ARTIFACT
		+ "source_file=\"%s\"\n" % V5_PNG
		+ "dest_files=[\"%s\"]\n\n" % V5_ARTIFACT
		+ "[params]\n\n"
		+ "compression/mode=0\n"
		+ "compression/high_quality=false\n"
		+ "compression/color_space=1\n"
		+ "compress/normal_map=0\n"
		+ "compress/channel_pack=0\n"
		+ "compression/gdeflate_level=1\n"
		+ "compatibility/as_image_texture=false\n"
	)


func _stage_v5_fixtures() -> bool:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(V5_DIR))
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))

	var png_bytes := PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0])
	for path in [V5_PNG, V4_PNG]:
		var img := FileAccess.open(path, FileAccess.WRITE)
		if img == null:
			print("FAIL: Could not create %s" % path)
			return false
		img.store_buffer(png_bytes)
		img.close()

	var artifact_bytes := "DTEX".to_utf8_buffer() + PackedByteArray([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
	for path in [V5_ARTIFACT, V4_ARTIFACT]:
		var art := FileAccess.open(path, FileAccess.WRITE)
		if art == null:
			print("FAIL: Could not create %s" % path)
			return false
		art.store_buffer(artifact_bytes)
		art.close()

	if not _write_text_file(V5_IMPORT, _v5_import_text()):
		return false
	if not _write_text_file(V4_IMPORT, _v4_import_text()):
		return false
	return true


func _write_text_file(path: String, text: String) -> bool:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		print("FAIL: Could not create %s" % path)
		return false
	f.store_string(text)
	f.close()
	return true


func _cleanup_v5_fixtures() -> void:
	for path in [V5_PNG, V5_IMPORT, V4_PNG, V4_IMPORT, V5_ARTIFACT, V4_ARTIFACT]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	var dir_abs := ProjectSettings.globalize_path(V5_DIR)
	if DirAccess.dir_exists_absolute(dir_abs):
		DirAccess.remove_absolute(dir_abs)


