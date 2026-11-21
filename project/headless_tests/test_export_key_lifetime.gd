@tool
extends SceneTree

## Export key lifetime contract.
##
## The exporter's writer round trip installs a generated archive key as the
## process-wide DSPkg override. It must restore the previous key state on every
## exit and must not leave the raw key in the plugin member afterwards.
##
## Hermetic: one generated .dstex and one encrypted .dspkg under user://, removed
## in teardown. Requires editor context because EditorExportPlugin is
## editor-only:
##   godot --headless --path project --editor --script headless_tests/test_export_key_lifetime.gd

const EXPORT_PLUGIN_UID := "uid://37jisblsb2jo"

var _checks := 0
var _failures := 0
var _created: Array[String] = []
var _dstex_path := "user://export_key_lifetime_texture.dstex"


func _init() -> void:
	call_deferred("quit", 1)
	_run()
	call_deferred("quit", 0 if _failures == 0 else 1)


func _run() -> void:
	print("--- Export key lifetime tests ---")
	var key := PackedByteArray()
	for i in range(32):
		key.append((i * 7 + 3) % 256)

	var archive_path := "user://export_key_lifetime.dspkg"
	if not _make_dstex():
		return
	if not _make_archive(archive_path, key).is_empty():
		_check_archive_cases(key, archive_path)

	_teardown()


func _check_archive_cases(key: PackedByteArray, archive_path: String) -> void:
	# 1. Healthy export: previous global key state comes back and the member is wiped.
	var plugin: Object = _make_plugin(key, archive_path)
	var before := DSPkg.get_global_encryption_key_id()
	var ok: bool = bool(plugin.call("_verify_written_archive"))
	var after := DSPkg.get_global_encryption_key_id()
	_check("baseline.verify_ok", ok, true)
	_check("baseline.key_id_restored", after, before)
	_check("baseline.member_wiped", _is_key_wiped(plugin), true)

	# 2. Control of the same shape: a developer preview key must survive, which
	#    a blind set_global_encryption_key("") would discard.
	var preview_hex := Crypto.new().generate_random_bytes(32).hex_encode()
	DSPkg.set_global_encryption_key(preview_hex)
	var preview_id := DSPkg.compute_key_id(preview_hex)
	var plugin_preview: Object = _make_plugin(key, archive_path)
	var ok_preview: bool = bool(plugin_preview.call("_verify_written_archive"))
	_check("preview.verify_ok", ok_preview, true)
	_check("preview.key_id_preserved", DSPkg.get_global_encryption_key_id(), preview_id)
	_check("preview.member_wiped", _is_key_wiped(plugin_preview), true)

	# 3. Failure leg: a verification that fails before open() must still restore
	#    the key. This stops a "restore only on success" implementation.
	DSPkg.set_global_encryption_key("")
	var bad_path := "user://export_key_lifetime_bad.dspkg"
	var junk := FileAccess.open(bad_path, FileAccess.WRITE)
	if junk == null:
		_check("failure.fixture_written", false, true)
		return
	junk.store_buffer("not a dspkg archive".to_utf8_buffer())
	junk.close()
	_created.append(bad_path)
	var baseline := DSPkg.get_global_encryption_key_id()
	var plugin_bad: Object = _make_plugin(key, bad_path)
	var ok_bad: bool = bool(plugin_bad.call("_verify_written_archive"))
	_check("failure.verify_false", ok_bad, false)
	_check("failure.key_id_restored", DSPkg.get_global_encryption_key_id(), baseline)
	_check("failure.member_wiped", _is_key_wiped(plugin_bad), true)


## The fix wipes the 32-byte buffer in place rather than shrinking it, so a
## zero length is not the contract - all-zero bytes are.
func _is_key_wiped(p_plugin: Object) -> bool:
	var bytes: PackedByteArray = p_plugin.get("_archive_key")
	for b in bytes:
		if b != 0:
			return false
	return true


func _make_plugin(key: PackedByteArray, archive_path: String) -> Object:
	var script := ResourceLoader.load("res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd", "GDScript") as GDScript
	var plugin: Object = script.new()
	plugin.set("_archive_key", key.duplicate())
	plugin.set("_archive_id", 0)
	plugin.set("_target_dspkg_path", archive_path)
	return plugin


func _make_dstex() -> bool:
	if FileAccess.file_exists(_dstex_path):
		_created.append(_dstex_path)
		return true
	var raw := PackedByteArray()
	for i in range(256 * 256 * 4):
		raw.append(i % 256)
	var tex := DSTex.new()
	var err: Error = tex.write_to_file(_dstex_path, raw, 256, 256, Image.FORMAT_RGBA8, 1, 1)
	_check("fixture.dstex_written", err, OK)
	if err != OK:
		return false
	_created.append(_dstex_path)
	return true


func _make_archive(path: String, key: PackedByteArray) -> String:
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(path)
	var writer := DSPkg.new()
	var err: Error = writer.create(path, 0)
	_check("fixture.create", err, OK)
	if err != OK:
		return ""
	# Red runs use the pre-fix DLL, which has only the hex setter. The test
	# fixture (not the product) accepts either shape so the assertion fails for
	# the lifetime reason, not a missing method.
	if writer.has_method("set_encryption_key_bytes"):
		err = writer.set_encryption_key_bytes(key)
	else:
		err = writer.set_encryption_key(key.hex_encode())
	_check("fixture.set_key", err, OK)
	if err != OK:
		return ""
	err = writer.add_texture("res://textures/test_texture.png", _dstex_path)
	_check("fixture.add_texture", err, OK)
	if err != OK:
		return ""
	err = writer.finalize()
	_check("fixture.finalize", err, OK)
	if err != OK:
		return ""
	_created.append(path)
	return path


func _teardown() -> void:
	DSPkg.set_global_encryption_key("")
	for path in _created:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	print("RESULT ", "PASS" if _failures == 0 else "FAIL", " checks=", _checks, " failures=", _failures)


func _check(p_name: String, p_actual: Variant, p_expected: Variant) -> void:
	_checks += 1
	if p_actual == p_expected:
		print("[PASS] ", p_name, " :: ", p_actual)
	else:
		_failures += 1
		printerr("[FAIL] ", p_name, " :: expected=", p_expected, " actual=", p_actual)
