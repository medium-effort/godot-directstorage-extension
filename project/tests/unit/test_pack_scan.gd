extends GutTest

const VerifyScript := preload("uid://bjyun30ppok5g")
const ScanCore := preload("uid://cgmf75ap4elct")
const Helpers := preload("res://tests/unit/_gut_helpers.gd")

const TMP_DIR := "user://gut_tmp/pack_scan"


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP_DIR)


func after_each() -> void:
	Helpers.rm_dir_recursive(TMP_DIR)


func _needle() -> PackedByteArray:
	var core_script: GDScript = ScanCore
	var magic_label := String(core_script.get_script_constant_map()["KEY_MAGIC_LABEL"])
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(magic_label.to_utf8_buffer())
	return ctx.finish().slice(0, 16)


func _write_pack(path: String, flags: int, entry_count: int, body: PackedByteArray) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(f, "open pack %s for write" % path)
	f.store_32(0x43504447)
	f.store_32(2)
	f.store_32(4)
	f.store_32(7)
	f.store_32(2)
	f.store_32(flags)
	f.store_64(0)
	f.store_64(96)
	for i in 14:
		f.store_32(0)
	f.store_32(entry_count)
	var dir_bytes := PackedByteArray()
	dir_bytes.resize(entry_count * 40)
	f.store_buffer(dir_bytes)
	if not body.is_empty():
		f.store_buffer(body)
	f.close()


func test_magic_parity_across_cpp_exporter_and_scanner() -> void:
	var repo_root := ProjectSettings.globalize_path("res://").path_join("..").simplify_path()
	var cpp_text := FileAccess.get_file_as_string(repo_root.path_join("src/formats/dspkg_format.cpp"))
	var exporter_text := FileAccess.get_file_as_string(repo_root.path_join("project/addons/directstorage_accelerator/scripts/ds_export_plugin.gd"))
	var scanner_text := FileAccess.get_file_as_string(repo_root.path_join("project/addons/directstorage_accelerator/tools/dspkg_pack_scan_core.gd"))

	assert_false(cpp_text.is_empty(), "dspkg_format.cpp readable")
	assert_false(exporter_text.is_empty(), "ds_export_plugin.gd readable")
	assert_false(scanner_text.is_empty(), "dspkg_pack_scan_core.gd readable")

	var cpp_label := _capture(cpp_text, "DSPKG_TOC_KEY_MAGIC_LABEL\\s*=\\s*\"([^\"]+)\"")
	var cpp_size := _capture(cpp_text, "DSPKG_TOC_KEY_MAGIC_SIZE\\s*=\\s*(\\d+)")
	var exporter_label := _capture(exporter_text, "KEY_MAGIC_LABEL\\s*:\\s*String\\s*=\\s*\"([^\"]+)\"")
	var scanner_label := _capture(scanner_text, "KEY_MAGIC_LABEL\\s*:\\s*=\\s*\"([^\"]+)\"")
	var scanner_size := _capture(scanner_text, "KEY_MAGIC_SIZE\\s*:\\s*=\\s*(\\d+)")

	assert_false(cpp_label.is_empty())
	assert_eq(cpp_label, exporter_label)
	assert_eq(cpp_label, scanner_label)
	assert_eq(cpp_size, "16")
	assert_eq(scanner_size, "16")
	assert_true(cpp_text.contains("sha256_buffer()"))
	assert_true(cpp_text.contains("memcpy(derived.data(), digest.ptr(), DSPKG_TOC_KEY_MAGIC_SIZE)"))


func test_pack_verify_verdicts() -> void:
	var needle := _needle()
	var clean_body := PackedByteArray()
	clean_body.resize(64)
	clean_body.fill(0x5A)

	var p_ok := TMP_DIR.path_join("valid_clean.pck")
	_write_pack(p_ok, 0, 1, clean_body)
	var r1: Dictionary = VerifyScript.verify_pack(p_ok)
	assert_true(r1.get("ok"))
	assert_eq(r1.get("status"), "ok")
	assert_eq(r1.get("reason"), "none")
	assert_eq(r1.get("entries"), 1)
	assert_eq(r1.get("bytes"), FileAccess.get_size(p_ok))

	var p_leak := TMP_DIR.path_join("valid_with_needle.pck")
	var leak_body := clean_body.duplicate()
	for i in needle.size():
		leak_body[16 + i] = needle[i]
	_write_pack(p_leak, 0, 1, leak_body)
	var r2: Dictionary = VerifyScript.verify_pack(p_leak)
	assert_false(r2.get("ok"))
	assert_eq(r2.get("reason"), "key_magic_found")

	var p_ctrl := TMP_DIR.path_join("control_changed.pck")
	var ctrl_body := leak_body.duplicate()
	for i in needle.size():
		ctrl_body[16 + i] = needle[i] ^ 0xFF
	_write_pack(p_ctrl, 0, 1, ctrl_body)
	var r3: Dictionary = VerifyScript.verify_pack(p_ctrl)
	assert_true(r3.get("ok"))

	var p_zero := TMP_DIR.path_join("zero_entries.pck")
	_write_pack(p_zero, 0, 0, PackedByteArray())
	var r4: Dictionary = VerifyScript.verify_pack(p_zero)
	assert_false(r4.get("ok"))
	assert_eq(r4.get("reason"), "zero_entries")

	var p_bad := TMP_DIR.path_join("bad_header.pck")
	var f5 := FileAccess.open(p_bad, FileAccess.WRITE)
	var z := PackedByteArray()
	z.resize(40)
	f5.store_buffer(z)
	f5.close()
	var r5: Dictionary = VerifyScript.verify_pack(p_bad)
	assert_false(r5.get("ok"))
	assert_eq(r5.get("reason"), "bad_header")
	assert_eq(r5.get("dir_encrypted"), "unknown")

	var r6: Dictionary = VerifyScript.verify_pack(TMP_DIR.path_join("does_not_exist.pck"))
	assert_false(r6.get("ok"))
	assert_eq(r6.get("reason"), "unreadable")

	var p_enc := TMP_DIR.path_join("dir_encrypted.pck")
	_write_pack(p_enc, 1, 1, PackedByteArray())
	var r7: Dictionary = VerifyScript.verify_pack(p_enc)
	assert_true(r7.get("ok"))
	assert_eq(r7.get("dir_encrypted"), "true")

	var line_re := RegEx.new()
	line_re.compile("^PACK_VERIFY status=\\w+ reason=\\w+ dir_encrypted=(true|false|unknown) entries=\\d+ bytes=\\d+ ms=\\d+ file=.+$")
	assert_not_null(line_re.search(VerifyScript.format_line(r1, p_ok)))
	assert_not_null(line_re.search(VerifyScript.format_line(r2, p_leak)))


func test_chunk_boundary_needle_detection() -> void:
	var needle := _needle()
	var p_boundary := TMP_DIR.path_join("chunk_boundary.pck")
	var header_and_dir := 96 + 4 + 40
	var target_offset: int = int(ScanCore.SCAN_CHUNK_SIZE) - 8
	var body := PackedByteArray()
	body.resize(target_offset - header_and_dir + needle.size() + 64)
	for i in needle.size():
		body[target_offset - header_and_dir + i] = needle[i]
	_write_pack(p_boundary, 0, 1, body)
	var res: Dictionary = VerifyScript.verify_pack(p_boundary)
	assert_false(res.get("ok"))
	assert_eq(res.get("reason"), "key_magic_found")


static func _capture(text: String, pattern: String) -> String:
	var regex := RegEx.new()
	if regex.compile(pattern) != OK:
		return ""
	var found := regex.search(text)
	if found == null:
		return ""
	return found.get_string(1)
