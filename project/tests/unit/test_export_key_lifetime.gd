extends GutTest

const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const TMP_DIR := "user://gut_tmp/export_key_lifetime"
const EXPORT_PLUGIN_PATH := "res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd"


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP_DIR)


func after_each() -> void:
	if ClassDB.class_exists("DSPkg"):
		DSPkg.set_global_encryption_key("")
	Helpers.rm_dir_recursive(TMP_DIR)


func test_export_key_lifetime_restores_global_key_and_wipes_member() -> void:
	if not ClassDB.can_instantiate("EditorExportPlugin"):
		pending("EditorExportPlugin requires --editor context")
		return

	var dstex_path := TMP_DIR.path_join("texture.dstex")
	var archive_path := TMP_DIR.path_join("export.dspkg")
	var bad_path := TMP_DIR.path_join("bad.dspkg")

	var raw := PackedByteArray()
	for i in range(256 * 256 * 4):
		raw.append(i % 256)
	var tex := DSTex.new()
	assert_eq(tex.write_to_file(dstex_path, raw, 256, 256, Image.FORMAT_RGBA8, 1, 1), OK)

	var key := PackedByteArray()
	for i in range(32):
		key.append((i * 7 + 3) % 256)

	var writer := DSPkg.new()
	assert_eq(writer.create(archive_path, 0), OK)
	assert_eq(writer.set_encryption_key_bytes(key), OK)
	assert_eq(writer.add_texture("res://textures/test_texture.png", dstex_path), OK)
	assert_eq(writer.finalize(), OK)

	var script := ResourceLoader.load(EXPORT_PLUGIN_PATH, "GDScript") as GDScript
	assert_not_null(script)

	# 1. Healthy export restores prior global key state and wipes _archive_key.
	var plugin := _make_plugin(script, key, archive_path)
	var before := DSPkg.get_global_encryption_key_id()
	assert_true(bool(plugin.call("_verify_written_archive")))
	assert_eq(DSPkg.get_global_encryption_key_id(), before)
	assert_true(_is_key_wiped(plugin))

	# 2. Preview key survives round-trip.
	var preview_hex := Crypto.new().generate_random_bytes(32).hex_encode()
	DSPkg.set_global_encryption_key(preview_hex)
	var preview_id := DSPkg.compute_key_id(preview_hex)
	var plugin_preview := _make_plugin(script, key, archive_path)
	assert_true(bool(plugin_preview.call("_verify_written_archive")))
	assert_eq(DSPkg.get_global_encryption_key_id(), preview_id)
	assert_true(_is_key_wiped(plugin_preview))

	# 3. Failure leg still restores key and wipes _archive_key.
	DSPkg.set_global_encryption_key("")
	var junk := FileAccess.open(bad_path, FileAccess.WRITE)
	junk.store_buffer("not a dspkg archive".to_utf8_buffer())
	junk.close()
	var baseline := DSPkg.get_global_encryption_key_id()
	var plugin_bad := _make_plugin(script, key, bad_path)
	assert_false(bool(plugin_bad.call("_verify_written_archive")))
	assert_eq(DSPkg.get_global_encryption_key_id(), baseline)
	assert_true(_is_key_wiped(plugin_bad))


func _make_plugin(script: GDScript, key: PackedByteArray, archive_path: String) -> Object:
	var plugin: Object = script.new()
	plugin.set("_archive_key", key.duplicate())
	plugin.set("_archive_id", 0)
	plugin.set("_target_dspkg_path", archive_path)
	return plugin


func _is_key_wiped(plugin: Object) -> bool:
	var bytes: PackedByteArray = plugin.get("_archive_key")
	for b in bytes:
		if b != 0:
			return false
	return true
