extends GutTest

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const IMPORTER_SCRIPT_PATH := "res://addons/directstorage_accelerator/scripts/ds_importer.gd"
const CORE_SCRIPT_PATH := "res://addons/directstorage_accelerator/tools/ds_migration_core.gd"

const TEST_DIR := "res://gut_tmp_migration_core"
const TEST_IMG := TEST_DIR + "/hero.png"
const TEST_IMPORT := TEST_DIR + "/hero.png.import"
const TEST_CTEX := "res://.godot/imported/gut_test_hero.ctex"
const TEST_UID := "uid://ds_migration_test_uid_98765"

const V5_DIR := "res://gut_tmp_migration_v5"
const V5_PNG := V5_DIR + "/v5.png"
const V5_IMPORT := V5_DIR + "/v5.png.import"
const V5_ARTIFACT := "res://.godot/imported/gut_test_migration_v5.dstex"
const V5_UID := "uid://ds_migration_v5_uid_98765"
const V4_PNG := V5_DIR + "/v4.png"
const V4_IMPORT := V5_DIR + "/v4.png.import"
const V4_ARTIFACT := "res://.godot/imported/gut_test_migration_v4.dstex"
const V4_UID := "uid://ds_migration_v4_uid_98765"

var _zip_to_clean := ""
var _extra_artifacts: Array[String] = []


func before_each() -> void:
	_zip_to_clean = ""
	_extra_artifacts.clear()
	Helpers.ensure_clean_dir(TEST_DIR)
	Helpers.ensure_clean_dir(V5_DIR)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path("res://.godot/imported"))


func after_each() -> void:
	var fixed := [TEST_CTEX, V5_ARTIFACT, V4_ARTIFACT]
	for p in fixed + _extra_artifacts:
		var glob := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(glob):
			DirAccess.remove_absolute(glob)
	if not _zip_to_clean.is_empty():
		var zglob := ProjectSettings.globalize_path(_zip_to_clean)
		if FileAccess.file_exists(zglob):
			DirAccess.remove_absolute(zglob)
	Helpers.rm_dir_recursive(TEST_DIR)
	Helpers.rm_dir_recursive(V5_DIR)


func test_parameter_mapping() -> void:
	var res2: Array = CORE.map_texture_params({"compress/mode": "2"})
	assert_not_null(res2[0])
	assert_eq(res2[0].get("compression/mode"), "0")
	assert_true(res2[2])

	var res3: Array = CORE.map_texture_params({"compress/mode": "3"})
	assert_not_null(res3[0])
	assert_eq(res3[0].get("compression/mode"), "1")
	assert_true(res3[2])

	var res5: Array = CORE.map_texture_params({"compress/mode": "5"})
	assert_not_null(res5[0])
	assert_eq(res5[0].get("compression/mode"), "0")
	assert_true(res5[2])

	for m in ["0", "1", "4"]:
		var res_m: Array = CORE.map_texture_params({"compress/mode": m})
		assert_null(res_m[0])
		assert_false(res_m[2])


func test_full_migration_lifecycle() -> void:
	_write_bytes(TEST_IMG, PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0]))
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
	_write_text(TEST_IMPORT, import_content)
	_write_text(TEST_CTEX, "CTEX_DUMMY_DATA")

	var plan: Array[Dictionary] = CORE.scan()
	var test_entry := _find_entry(plan, TEST_IMG)
	assert_false(test_entry.is_empty())
	assert_eq(str(test_entry.get("mode", "")), "adopt")
	assert_true(bool(test_entry.get("eligible", false)))
	assert_eq(str(test_entry.get("uid", "")), TEST_UID)

	var backup_res: Dictionary = CORE.create_zip_backup([test_entry])
	assert_true(bool(backup_res.get("ok", false)))
	_zip_to_clean = str(backup_res.get("zip_res_path", ""))
	assert_true(FileAccess.file_exists(_zip_to_clean))
	assert_true(FileAccess.file_exists(CORE.DEFAULT_BACKUP_DIR.path_join(".gdignore")))

	var apply_res: Dictionary = CORE.apply_migration([test_entry])
	assert_true(bool(apply_res.get("ok", false)))

	var post_cp := ConfigFile.new()
	assert_eq(post_cp.load(TEST_IMPORT), OK)
	assert_eq(str(post_cp.get_value("remap", "importer", "")), CORE.TARGET_IMPORTER)
	assert_eq(int(post_cp.get_value("remap", "importer_version", -1)), 0)
	assert_eq(str(post_cp.get_value("remap", "uid", "")), TEST_UID)
	assert_eq(str(post_cp.get_value("params", "compression/mode", "")), "0")

	CORE.cleanup_old_artifacts([test_entry])
	assert_false(FileAccess.file_exists(TEST_CTEX))

	var restore_res := CORE.restore_from_zip(_zip_to_clean)
	assert_true(bool(restore_res.get("ok", false)))
	var restored_cp := ConfigFile.new()
	assert_eq(restored_cp.load(TEST_IMPORT), OK)
	assert_eq(str(restored_cp.get_value("remap", "importer", "")), "texture")
	assert_eq(str(restored_cp.get_value("remap", "uid", "")), TEST_UID)


func test_importer_contract_parity() -> void:
	var importer_script := load(IMPORTER_SCRIPT_PATH) as Script
	assert_not_null(importer_script)
	var importer_consts := importer_script.get_script_constant_map()
	assert_true(importer_consts.has("FORMAT_VERSION"))
	assert_eq(int(importer_consts["FORMAT_VERSION"]), 5)

	var core_script := load(CORE_SCRIPT_PATH) as Script
	assert_not_null(core_script)
	var core_consts := core_script.get_script_constant_map()
	assert_true(core_consts.has("TARGET_VERSION"))
	assert_eq(int(core_consts["TARGET_VERSION"]), 5)


func test_v5_verification_and_scan() -> void:
	_stage_v5_fixtures()
	var plan: Array = CORE.scan()
	var v5_entry := _find_entry(plan, V5_PNG)
	var v4_entry := _find_entry(plan, V4_PNG)
	assert_false(v5_entry.is_empty())
	assert_false(v4_entry.is_empty())

	var results: Array = CORE.verify_migration([
		{"source": V5_PNG, "uid": V5_UID},
		{"source": V4_PNG, "uid": V4_UID},
	])
	var v5_res := _find_entry(results, V5_PNG)
	var v4_res := _find_entry(results, V4_PNG)
	assert_true(bool(v5_res.get("verified", false)))
	assert_false(bool(v4_res.get("verified", true)))
	assert_true(str(v4_res.get("detail", "")).contains("importer_version=4 (expected 5)"))

	assert_eq(str(v4_entry.get("mode", "")), "upgrade")
	assert_true(bool(v4_entry.get("eligible", false)))
	assert_eq(str(v5_entry.get("mode", "")), "up_to_date")
	assert_eq(int(v5_entry.get("importer_version", -1)), 5)

	var mapped: Dictionary = v4_entry.get("mapped_params", {})
	assert_eq(str(mapped.get("compress/normal_map", "")), "1")
	assert_eq(str(mapped.get("compression/color_space", "")), "0")


func test_v5_option_emission() -> void:
	var normal_params: Dictionary = CORE.map_texture_params({"compress/mode": "2", "compress/normal_map": "1"})[0]
	assert_eq(str(normal_params.get("compression/color_space", "")), "0")
	assert_eq(str(normal_params.get("compress/normal_map", "")), "1")
	assert_eq(str(normal_params.get("compress/channel_pack", "")), "1")
	assert_false(normal_params.has("compression/source"))

	var disabled_params: Dictionary = CORE.map_texture_params({"compress/mode": "2", "compress/normal_map": "2", "compress/channel_pack": "1"})[0]
	assert_eq(str(disabled_params.get("compress/normal_map", "")), "0")
	assert_eq(str(disabled_params.get("compression/color_space", "")), "1")
	assert_eq(str(disabled_params.get("compress/channel_pack", "")), "1")

	var bptc_params: Dictionary = CORE.map_texture_params({"compress/mode": "5"})[0]
	assert_eq(str(bptc_params.get("compression/mode", "")), "0")
	assert_eq(str(bptc_params.get("compression/high_quality", "")), "true")

	var uncomp_params: Dictionary = CORE.map_texture_params({"compress/mode": "3", "compress/high_quality": "true"})[0]
	assert_eq(str(uncomp_params.get("compression/high_quality", "")), "false")

	var upgraded_text := CORE.rewrite_import(_v4_import_text().to_utf8_buffer(), "upgrade", {}).get_string_from_utf8()
	assert_true(upgraded_text.contains("compress/normal_map=1"))
	assert_true(upgraded_text.contains("compression/color_space=0"))
	assert_true(upgraded_text.contains("compress/channel_pack=1"))
	assert_false(upgraded_text.contains("compression/source="))
	assert_eq(upgraded_text.count("compression/mode=0"), 1)
	assert_true(upgraded_text.contains("importer_version=0"))

	var adopt_params: Dictionary = CORE.action_mapped_params(3)
	assert_eq(str(adopt_params.get("compress/normal_map", "")), "1")
	assert_eq(str(adopt_params.get("compression/color_space", "")), "0")
	var adopted_text := CORE.rewrite_import(_v5_import_text().to_utf8_buffer(), "adopt", adopt_params).get_string_from_utf8()
	for key in ["compression/color_space=0", "compress/normal_map=1", "compress/channel_pack=1"]:
		assert_true(adopted_text.contains(key))
	assert_false(adopted_text.contains("compression/source="))
	assert_true(adopted_text.contains("uid=\"%s\"" % V5_UID))
	assert_true(adopted_text.contains("importer_version=0"))

	assert_eq(CORE.action_for_params(adopt_params), 3)
	assert_eq(CORE.action_for_params({"compression/mode": "1"}), 2)
	assert_eq(CORE.action_for_params({}), 1)


func test_cleanup_scope() -> void:
	var superseded := "res://.godot/imported/gut_cleanup_old.ctex"
	var superseded_md5 := "res://.godot/imported/gut_cleanup_old.md5"
	var live := "res://.godot/imported/gut_cleanup_live.dstex"
	var live_md5 := "res://.godot/imported/gut_cleanup_live.md5"
	var keep_me := "res://.godot/imported/gut_cleanup_keep.ctex"
	var keep_me_md5 := "res://.godot/imported/gut_cleanup_keep.md5"
	var unverified := "res://.godot/imported/gut_cleanup_unverified.ctex"
	var outside := V5_DIR + "/gut_cleanup_outside.ctex"
	_extra_artifacts = [superseded, superseded_md5, live, live_md5, keep_me, keep_me_md5, unverified, outside]
	for p in _extra_artifacts:
		_write_text(p, "CLEANUP_FIXTURE")

	var del_live := CORE.cleanup_old_artifacts([{"source": "res://test_assets/test.png", "old_dests": [live]}])
	assert_true(FileAccess.file_exists(live))
	assert_true(FileAccess.file_exists(live_md5))
	assert_true((del_live[0].get("deleted", []) as Array).is_empty())

	var del_ctex := CORE.cleanup_old_artifacts([{"source": "res://test_assets/test.png", "old_dests": [superseded]}])
	assert_false(FileAccess.file_exists(superseded))
	assert_false(FileAccess.file_exists(superseded_md5))
	assert_eq((del_ctex[0].get("deleted", []) as Array).size(), 2)

	var del_keep := CORE.cleanup_old_artifacts([{"source": "res://test_assets/test.png", "old_dests": [keep_me], "new_dests": [keep_me]}])
	assert_true(FileAccess.file_exists(keep_me))
	assert_true(FileAccess.file_exists(keep_me_md5))
	assert_true((del_keep[0].get("deleted", []) as Array).is_empty())

	var del_out := CORE.cleanup_old_artifacts([{"source": "res://test_assets/test.png", "old_dests": [outside]}])
	assert_true(FileAccess.file_exists(outside))
	assert_true((del_out[0].get("deleted", []) as Array).is_empty())

	_write_text(superseded, "CLEANUP_FIXTURE")
	_write_text(superseded_md5, "CLEANUP_FIXTURE")
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
	var filtered := CORE.filter_verified_entries(selected, verify_results)
	assert_eq(filtered.size(), 2)
	assert_true(filtered[0].has("old_dests"))
	assert_true((filtered[0].get("new_dests", []) as Array).has(live))
	assert_false(selected[0].has("new_dests"))

	CORE.cleanup_old_artifacts(filtered)
	assert_true(FileAccess.file_exists(unverified))
	assert_false(FileAccess.file_exists(superseded))
	assert_false(FileAccess.file_exists(superseded_md5))


func test_upgrade_parameter_preservation() -> void:
	_stage_v5_fixtures()
	var v4u_png := V5_DIR + "/v4u.png"
	var v4u_import := V5_DIR + "/v4u.png.import"
	var v4u_artifact := "res://.godot/imported/gut_test_migration_v4u.dstex"
	var repair_png := V5_DIR + "/repair.png"
	var repair_import := V5_DIR + "/repair.png.import"
	_extra_artifacts = [v4u_artifact]

	var png_bytes := PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0])
	_write_bytes(v4u_png, png_bytes)
	_write_bytes(repair_png, png_bytes)
	_write_text(v4u_artifact, "DTEX")

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
		+ "path=\"res://.godot/imported/gut_test_migration_repair_missing.dstex\"\n\n"
		+ "[deps]\n\n"
		+ "source_file=\"%s\"\n" % repair_png
		+ "dest_files=[\"res://.godot/imported/gut_test_migration_repair_missing.dstex\"]\n\n"
		+ "[params]\n\n"
		+ "compression/mode=1\n"
		+ "compression/high_quality=false\n"
	)
	_write_text(v4u_import, v4u_text)
	_write_text(repair_import, repair_text)

	var plan: Array = CORE.scan()
	var up := _find_entry(plan, v4u_png)
	var rep := _find_entry(plan, repair_png)
	assert_eq(str(up.get("mode", "")), "upgrade")
	assert_eq(str(up.get("mapped_params", {}).get("compression/mode", "")), "1")
	assert_eq(CORE.action_for_params(up.get("mapped_params", {})), 2)
	assert_eq(str(rep.get("mode", "")), "repair")
	assert_eq(CORE.action_for_params(rep.get("mapped_params", {})), 2)
	assert_eq(str(up.get("base_mode", "")), "upgrade")
	assert_eq(str(rep.get("base_mode", "")), "repair")

	var overridden := CORE.rewrite_import(v4u_text.to_utf8_buffer(), "upgrade", {"compression/mode": "1"}).get_string_from_utf8()
	assert_eq(_count_key(overridden, "compression/mode"), 1)
	assert_eq(_param_value(overridden, "compression/mode"), "1")
	assert_eq(_param_value(overridden, "compression/gdeflate_level"), "7")
	assert_eq(_param_value(overridden, "compatibility/as_image_texture"), "true")
	assert_eq(_param_value(overridden, "mipmaps/generate"), "false")
	assert_true(overridden.contains("importer_version=0"))

	assert_eq(CORE.action_mode_for_entry({"mode": "upgrade", "base_mode": "upgrade"}, 2), "upgrade")
	assert_eq(CORE.action_mode_for_entry({"mode": "skip", "base_mode": "upgrade"}, 2), "upgrade")
	assert_eq(CORE.action_mode_for_entry({"mode": "repair", "base_mode": "repair"}, 3), "upgrade")
	assert_eq(CORE.action_mode_for_entry({"mode": "adopt", "base_mode": "adopt"}, 2), "adopt")
	assert_eq(CORE.action_mode_for_entry({"base_mode": "adopt"}, 0), "skip")


func _write_bytes(path: String, bytes: PackedByteArray) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f)
	f.store_buffer(bytes)
	f.close()


func _write_text(path: String, text: String) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f)
	f.store_string(text)
	f.close()


func _find_entry(entries: Array, source: String) -> Dictionary:
	for e in entries:
		if str(e.get("source", "")) == source:
			return e
	return {}


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


func _stage_v5_fixtures() -> void:
	var png_bytes := PackedByteArray([137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 0])
	_write_bytes(V5_PNG, png_bytes)
	_write_bytes(V4_PNG, png_bytes)
	var artifact_bytes := "DTEX".to_utf8_buffer() + PackedByteArray([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])
	_write_bytes(V5_ARTIFACT, artifact_bytes)
	_write_bytes(V4_ARTIFACT, artifact_bytes)
	_write_text(V5_IMPORT, _v5_import_text())
	_write_text(V4_IMPORT, _v4_import_text())
