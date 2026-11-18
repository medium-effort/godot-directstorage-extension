extends SceneTree

## Unit test for dspkg_pack_verify.gd and the pack-verification contract.
##
## Hermetic: constructs minimal v2 packs under user://pack_verify/ and removes
## them in teardown. No EditorExportPlugin context or export run is needed.
##
## Run:
##   godot --headless --path project --script headless_tests/test_pack_verification.gd

const VerifyScript := preload("uid://bjyun30ppok5g")
const ScanCore := preload("uid://cgmf75ap4elct")

var _checks: int = 0
var _failures: int = 0


func _init() -> void:
	print("--- Pack verification unit tests ---")
	var tmp_dir := "user://pack_verify"
	DirAccess.make_dir_recursive_absolute(tmp_dir)
	var created: Array[String] = []

	var core_script: GDScript = ScanCore
	var magic_label: String = String(core_script.get_script_constant_map()["KEY_MAGIC_LABEL"])
	var ctx := HashingContext.new()
	ctx.start(HashingContext.HASH_SHA256)
	ctx.update(magic_label.to_utf8_buffer())
	var needle: PackedByteArray = ctx.finish().slice(0, 16)

	# 1. Valid pack, 1 entry, no needle
	var p1 := tmp_dir + "/valid_clean.pck"
	var clean_body := PackedByteArray()
	clean_body.resize(64)
	clean_body.fill(0x5A)
	_write_pack(p1, 0, 1, clean_body)
	created.append(p1)
	var r1: Dictionary = VerifyScript.verify_pack(p1)
	_check("1.valid_ok", r1.get("ok"), true)
	_check("1.valid_status", r1.get("status"), "ok")
	_check("1.valid_reason", r1.get("reason"), "none")
	_check("1.valid_entries", r1.get("entries"), 1)
	_check("1.valid_bytes", r1.get("bytes"), FileAccess.get_size(p1))

	# 2. Same shape + 16 needle bytes in the body -> key_magic_found
	var p2 := tmp_dir + "/valid_with_needle.pck"
	var leak_body := clean_body.duplicate()
	for i in needle.size():
		leak_body[16 + i] = needle[i]
	_write_pack(p2, 0, 1, leak_body)
	created.append(p2)
	var r2: Dictionary = VerifyScript.verify_pack(p2)
	_check("2.needle_ok", r2.get("ok"), false)
	_check("2.needle_reason", r2.get("reason"), "key_magic_found")

	# 3. Control: same size as #2, 16 bytes changed to non-needle -> ok
	var p3 := tmp_dir + "/control_changed.pck"
	var ctrl_body := leak_body.duplicate()
	for i in needle.size():
		ctrl_body[16 + i] = (needle[i] ^ 0xFF)
	_write_pack(p3, 0, 1, ctrl_body)
	created.append(p3)
	var r3: Dictionary = VerifyScript.verify_pack(p3)
	_check("3.control_ok", r3.get("ok"), true)

	# 4. entry_count = 0 -> zero_entries
	var p4 := tmp_dir + "/zero_entries.pck"
	_write_pack(p4, 0, 0, PackedByteArray())
	created.append(p4)
	var r4: Dictionary = VerifyScript.verify_pack(p4)
	_check("4.zero_entries_ok", r4.get("ok"), false)
	_check("4.zero_entries_reason", r4.get("reason"), "zero_entries")

	# 5. 40 bytes of zeros -> bad_header, dir_encrypted=unknown
	var p5 := tmp_dir + "/bad_header.pck"
	var f5 := FileAccess.open(p5, FileAccess.WRITE)
	var z := PackedByteArray()
	z.resize(40)
	f5.store_buffer(z)
	f5.close()
	created.append(p5)
	var r5: Dictionary = VerifyScript.verify_pack(p5)
	_check("5.bad_header_ok", r5.get("ok"), false)
	_check("5.bad_header_reason", r5.get("reason"), "bad_header")
	_check("5.bad_header_dir_encrypted", r5.get("dir_encrypted"), "unknown")

	# 6. Missing path -> unreadable
	var r6: Dictionary = VerifyScript.verify_pack(tmp_dir + "/does_not_exist.pck")
	_check("6.missing_ok", r6.get("ok"), false)
	_check("6.missing_reason", r6.get("reason"), "unreadable")

	# 7. Flags bit 0 set -> dir_encrypted="true", ok=true
	var p7 := tmp_dir + "/dir_encrypted.pck"
	_write_pack(p7, 1, 1, PackedByteArray())
	created.append(p7)
	var r7: Dictionary = VerifyScript.verify_pack(p7)
	_check("7.dir_encrypted_ok", r7.get("ok"), true)
	_check("7.dir_encrypted_flag", r7.get("dir_encrypted"), "true")

	# 8. format_line grammar on #1 and #2
	var line_re := RegEx.new()
	line_re.compile("^PACK_VERIFY status=\\w+ reason=\\w+ dir_encrypted=(true|false|unknown) entries=\\d+ bytes=\\d+ ms=\\d+ file=.+$")
	var line1: String = VerifyScript.format_line(r1, p1)
	var line2: String = VerifyScript.format_line(r2, p2)
	_check("8.format_line_ok", line_re.search(line1) != null, true)
	_check("8.format_line_fail", line_re.search(line2) != null, true)

	# 9. Needle straddling the 4 MiB chunk boundary (SCAN_CHUNK_SIZE - 8)
	var p9 := tmp_dir + "/chunk_boundary.pck"
	var header_and_dir := 96 + 4 + 40
	var target_offset: int = int(ScanCore.SCAN_CHUNK_SIZE) - 8
	var body9 := PackedByteArray()
	body9.resize(target_offset - header_and_dir + needle.size() + 64)
	for i in needle.size():
		body9[target_offset - header_and_dir + i] = needle[i]
	_write_pack(p9, 0, 1, body9)
	created.append(p9)
	var r9: Dictionary = VerifyScript.verify_pack(p9)
	_check("9.boundary_ok", r9.get("ok"), false)
	_check("9.boundary_reason", r9.get("reason"), "key_magic_found")

	for i in range(created.size() - 1, -1, -1):
		DirAccess.remove_absolute(created[i])
	DirAccess.remove_absolute(tmp_dir)

	print("RESULT ", "PASS" if _failures == 0 else "FAIL", " checks=", _checks, " failures=", _failures)
	quit(0 if _failures == 0 else 1)


func _write_pack(path: String, flags: int, entry_count: int, body: PackedByteArray) -> void:
	var f := FileAccess.open(path, FileAccess.WRITE)
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


func _check(p_name: String, p_actual: Variant, p_expected: Variant) -> void:
	_checks += 1
	if p_actual == p_expected:
		print("[PASS] ", p_name, " :: ", p_actual)
	else:
		_failures += 1
		printerr("[FAIL] ", p_name, " :: expected=", p_expected, " actual=", p_actual)
