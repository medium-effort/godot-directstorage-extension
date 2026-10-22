extends SceneTree

# Corruption fuzz harness: truncation, header fuzz, arithmetic/bounds
# overflow, row_pitch alignment, and ResourceLoader null-safety for
# .dstex / .dspkg. Asserts graceful handling (empty Dictionary / Error /
# null Resource), never abort. Exit 0 on success, 1 on any failure.
#
# Layout reference (little-endian):
#   DSTex header (48 B): magic u32@0 (0x58455444 "DTEX"), version u16@4,
#     flags u16@6, width u32@8, height u32@12, pixel_format u32@16,
#     mip_count u32@20, original_size u64@24, compressed_size u64@32,
#     reserved u64@40. V2 mip entry (24 B) at 48 + 24*i: offset u64@+0,
#     compressed_size u32@+8, uncompressed_size u32@+12, row_pitch u32@+16.
#   DSPkg header (64 B v4, 28 B legacy v1-v3): magic u32@0 (0x474B5044 "DPKG"), version u16@4,
#     flags u16@6, entry_count u32@8, data_offset u64@12, reserved u64@20.

const TEX_W := 64
const TEX_H := 4
const MIP_COUNT := 2
const ROW_PITCH := 256
const BASE_MIP_BYTES := ROW_PITCH * TEX_H
const MIP1_OFFSET := BASE_MIP_BYTES
const PAYLOAD_SIZE := BASE_MIP_BYTES + ROW_PITCH * (TEX_H / 2)

const DSTEX_HEADER_SIZE := 48
const DSTEX_V2_ENTRY_SIZE := 24
const DSTEX_OFF_VERSION := 4
const DSTEX_OFF_MIP_COUNT := 20
const DSTEX_OFF_ENTRY0 := 48
const DSTEX_ENTRY_OFF_OFFSET := 0
const DSTEX_ENTRY_OFF_CSIZE := 8
const DSTEX_ENTRY_OFF_PITCH := 16

const DSPKG_HEADER_SIZE := 64
const DSPKG_LEGACY_HEADER_SIZE := 28
const DSPKG_OFF_VERSION := 4
const DSPKG_OFF_ENTRY_COUNT := 8
const DSPKG_OFF_DATA_OFFSET := 12

const TMP_DIR := "res://headless_tests/.tmp_corruption_fuzz"
const VALID_DSTEX_USER := "user://fuzz_valid_source.dstex"
const VALID_DSPKG_USER := "user://fuzz_valid_archive.dspkg"

var failures := 0
var passes := 0
var _case_seq := 0
var _valid_dstex_bytes := PackedByteArray()
var _valid_dspkg_bytes := PackedByteArray()

func _initialize() -> void:
	print("[Corruption Fuzz] Starting format corruption fuzz harness")
	if not ClassDB.class_exists("DSTex") or not ClassDB.class_exists("DSPkg"):
		_fail("Required classes (DSTex, DSPkg) are not registered; build the extension first.")
		_finish()
		return
	if not _prepare_tmp_dir():
		_finish()
		return
	if not _build_valid_fixtures():
		_cleanup()
		_finish()
		return

	_test_truncation()
	_test_header_fuzz()
	_test_overflow_bounds()
	_test_row_pitch_alignment()
	_test_loader_valid_control()

	_cleanup()
	_finish()

# ---------------------------------------------------------------- fixtures ---

func _prepare_tmp_dir() -> bool:
	var parent := DirAccess.open("res://headless_tests")
	if parent == null:
		_fail("Cannot open res://headless_tests for tmp fixture dir setup.")
		return false
	if not parent.dir_exists(".tmp_corruption_fuzz"):
		var mk: Error = parent.make_dir(".tmp_corruption_fuzz")
		if mk != OK:
			_fail("Cannot create tmp fixture dir " + TMP_DIR + ": " + error_string(mk))
			return false
	return true

func _build_valid_fixtures() -> bool:
	var payload := _make_texture_payload()
	var dstex = _new_dstex()
	var werr: Error = dstex.write_to_file(VALID_DSTEX_USER, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	_check(werr == OK, "Fixture: DSTex.write_to_file succeeded: " + error_string(werr))
	if werr != OK:
		return false
	_valid_dstex_bytes = FileAccess.get_file_as_bytes(VALID_DSTEX_USER)
	_check(_valid_dstex_bytes.size() > DSTEX_HEADER_SIZE, "Fixture: valid .dstex has header + payload (" + str(_valid_dstex_bytes.size()) + " B)")
	if _valid_dstex_bytes.size() <= DSTEX_HEADER_SIZE:
		return false
	var probe = _new_dstex()
	var hdr: Dictionary = probe.read_header(VALID_DSTEX_USER)
	_check(not hdr.is_empty() and int(hdr.get("version", 0)) == 2, "Fixture: valid .dstex header parses as version 2")
	if hdr.is_empty():
		return false

	var dspkg = _new_dspkg()
	var cerr: Error = dspkg.create(VALID_DSPKG_USER)
	_check(cerr == OK, "Fixture: DSPkg.create succeeded: " + error_string(cerr))
	if cerr != OK:
		return false
	var aerr: Error = dspkg.add_texture("res://textures/fuzz_valid_tex.dstex", VALID_DSTEX_USER)
	_check(aerr == OK, "Fixture: DSPkg.add_texture succeeded: " + error_string(aerr))
	if aerr != OK:
		return false
	var ferr: Error = dspkg.finalize()
	_check(ferr == OK, "Fixture: DSPkg.finalize succeeded: " + error_string(ferr))
	if ferr != OK:
		return false
	_valid_dspkg_bytes = FileAccess.get_file_as_bytes(VALID_DSPKG_USER)
	_check(_valid_dspkg_bytes.size() > DSPKG_HEADER_SIZE, "Fixture: valid .dspkg has header + entries (" + str(_valid_dspkg_bytes.size()) + " B)")
	if _valid_dspkg_bytes.size() <= DSPKG_HEADER_SIZE:
		return false
	var reader = _new_dspkg()
	var oerr: Error = reader.open(VALID_DSPKG_USER)
	_check(oerr == OK, "Fixture: valid .dspkg opens cleanly: " + error_string(oerr))
	return oerr == OK

func _make_texture_payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(PAYLOAD_SIZE)
	for y in range(TEX_H):
		for x in range(TEX_W):
			var off := y * ROW_PITCH + x * 4
			payload[off] = (x * 4) % 256
			payload[off + 1] = (y * 63) % 256
			payload[off + 2] = 180
			payload[off + 3] = 255
	for y in range(TEX_H / 2):
		for x in range(TEX_W / 2):
			var off := MIP1_OFFSET + y * ROW_PITCH + x * 4
			payload[off] = 220
			payload[off + 1] = (x * 8) % 256
			payload[off + 2] = (y * 100) % 256
			payload[off + 3] = 255
	return payload

# ----------------------------------------------------------------- helpers ---

func _next_case_path(stem: String, ext: String) -> String:
	_case_seq += 1
	return TMP_DIR + "/fuzz_%03d_%s.%s" % [_case_seq, stem, ext]

func _write_bytes(path: String, data: PackedByteArray) -> bool:
	var f := FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		_fail("Cannot open for writing: " + path + " (" + error_string(FileAccess.get_open_error()) + ")")
		return false
	f.store_buffer(data)
	f.close()
	return true

func _patched(src: PackedByteArray, off: int, le_bytes: PackedByteArray) -> PackedByteArray:
	var out := src.duplicate()
	for i in range(le_bytes.size()):
		out[off + i] = le_bytes[i]
	return out

func _le16(v: int) -> PackedByteArray:
	return PackedByteArray([v & 0xFF, (v >> 8) & 0xFF])

func _le32(v: int) -> PackedByteArray:
	return PackedByteArray([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])

func _le64(v: int) -> PackedByteArray:
	var out := PackedByteArray()
	for i in range(8):
		out.append((v >> (8 * i)) & 0xFF)
	return out

func _ff(n: int) -> PackedByteArray:
	var out := PackedByteArray()
	out.resize(n)
	out.fill(0xFF)
	return out

func _truncated(src: PackedByteArray, n: int) -> PackedByteArray:
	return src.slice(0, n)

# ------------------------------------------------------- per-artifact probes ---

# Late-bound instantiation keeps this script parseable even when the
# GDExtension is not loaded (clean FAIL + quit(1) via the _initialize
# ClassDB gate instead of a parse-time abort).
func _new_dstex():
	return ClassDB.instantiate("DSTex")

func _new_dspkg():
	return ClassDB.instantiate("DSPkg")

func _probe_dstex(path: String, label: String, expect_reject: bool) -> void:
	var probe = _new_dstex()
	var hdr: Dictionary = probe.read_header(path)
	var full: Dictionary = probe.read_from_file(path)
	if expect_reject:
		_check(hdr.is_empty(), label + ": read_header rejects (empty Dictionary)")
		_check(full.is_empty(), label + ": read_from_file rejects (empty Dictionary)")
	else:
		_check(not hdr.is_empty(), label + ": read_header accepts valid input")
		_check(not full.is_empty(), label + ": read_from_file accepts valid input")
	var loaded = ResourceLoader.load(path)
	if expect_reject:
		_check(loaded == null, label + ": ResourceLoader.load returns null safely (no crash/leak)")
	else:
		print("[INFO] " + label + ": ResourceLoader.load control result type=" + str(typeof(loaded)))

func _probe_dspkg(path: String, label: String, expect_reject: bool) -> void:
	var reader = _new_dspkg()
	var oerr: Error = reader.open(path)
	if expect_reject:
		_check(oerr != OK, label + ": DSPkg.open rejects with error " + error_string(oerr))
	else:
		_check(oerr == OK, label + ": DSPkg.open accepts valid input: " + error_string(oerr))
	var loaded = ResourceLoader.load(path)
	if expect_reject:
		_check(loaded == null, label + ": ResourceLoader.load returns null safely (no crash/leak)")
	else:
		print("[INFO] " + label + ": ResourceLoader.load control result type=" + str(typeof(loaded)))

# -------------------------------------------------------------- category 1 ---

func _test_truncation() -> void:
	print("[Corruption Fuzz] Category 1: truncation")
	var dstex_sizes := [0, 4, DSTEX_HEADER_SIZE - 1, _valid_dstex_bytes.size() / 2]
	for n in dstex_sizes:
		var p := _next_case_path("trunc_dstex_%db" % n, "dstex")
		if _write_bytes(p, _truncated(_valid_dstex_bytes, n)):
			_probe_dstex(p, "DSTex truncated to %d B" % n, true)
	var dspkg_sizes := [0, 4, DSPKG_LEGACY_HEADER_SIZE - 1, DSPKG_HEADER_SIZE - 1, _valid_dspkg_bytes.size() / 2]
	for n in dspkg_sizes:
		var p := _next_case_path("trunc_dspkg_%db" % n, "dspkg")
		if _write_bytes(p, _truncated(_valid_dspkg_bytes, n)):
			_probe_dspkg(p, "DSPkg truncated to %d B" % n, true)

# -------------------------------------------------------------- category 2 ---

func _test_header_fuzz() -> void:
	print("[Corruption Fuzz] Category 2: header corruption")
	# DSTex magic: zeroed, all-FF, single-byte flip.
	var magic_cases := [
		["magic_zero", PackedByteArray([0, 0, 0, 0])],
		["magic_ff", PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF])],
		["magic_flip", PackedByteArray([0, 0, 0, 0])],
	]
	# Build single-byte flip from the real magic byte.
	var flip := _valid_dstex_bytes.duplicate()
	flip[0] = flip[0] ^ 0x01
	magic_cases[2][1] = flip.slice(0, 4)
	for entry in magic_cases:
		var p := _next_case_path("dstex_" + str(entry[0]), "dstex")
		if _write_bytes(p, _patched(_valid_dstex_bytes, 0, entry[1])):
			_probe_dstex(p, "DSTex " + str(entry[0]), true)
	# DSTex version: 0, 3, 99, 0xFFFF (1 and 2 are the valid legacy/GPU versions).
	for v in [0, 3, 99, 0xFFFF]:
		var p := _next_case_path("dstex_ver_%d" % v, "dstex")
		if _write_bytes(p, _patched(_valid_dstex_bytes, DSTEX_OFF_VERSION, _le16(v))):
			_probe_dstex(p, "DSTex version=%d" % v, true)
	# DSTex mip_count: 0, 255, 0xFFFFFFFF (cap is 16).
	for m in [0, 255, 0xFFFFFFFF]:
		var p := _next_case_path("dstex_mips_%d" % m, "dstex")
		if _write_bytes(p, _patched(_valid_dstex_bytes, DSTEX_OFF_MIP_COUNT, _le32(m))):
			_probe_dstex(p, "DSTex mip_count=%d" % m, true)
	# DSPkg magic.
	var pkg_magic := [
		["magic_zero", PackedByteArray([0, 0, 0, 0])],
		["magic_ff", PackedByteArray([0xFF, 0xFF, 0xFF, 0xFF])],
	]
	var pflip := _valid_dspkg_bytes.duplicate()
	pflip[1] = pflip[1] ^ 0x40
	pkg_magic.append(["magic_flip", pflip.slice(0, 4)])
	for entry in pkg_magic:
		var p := _next_case_path("dspkg_" + str(entry[0]), "dspkg")
		if _write_bytes(p, _patched(_valid_dspkg_bytes, 0, entry[1])):
			_probe_dspkg(p, "DSPkg " + str(entry[0]), true)
	# DSPkg version: only 1, 2, 3, 4 are valid.
	for v in [0, 5, 99, 0xFFFF]:
		var p := _next_case_path("dspkg_ver_%d" % v, "dspkg")
		if _write_bytes(p, _patched(_valid_dspkg_bytes, DSPKG_OFF_VERSION, _le16(v))):
			_probe_dspkg(p, "DSPkg version=%d" % v, true)
	# DSPkg version downgrade mutation probe with encrypted flags (v4 -> v2, v3).
	for v in [2, 3]:
		var downgraded := _valid_dspkg_bytes.duplicate()
		downgraded[DSPKG_OFF_VERSION] = v & 0xFF
		downgraded[DSPKG_OFF_VERSION + 1] = (v >> 8) & 0xFF
		downgraded[6] = 3
		downgraded[7] = 0
		downgraded[52] = 0x01
		var p := _next_case_path("dspkg_downgrade_v%d" % v, "dspkg")
		if _write_bytes(p, downgraded):
			_probe_dspkg(p, "DSPkg version downgrade to v%d with encrypted flags" % v, true)

# -------------------------------------------------------------- category 3 ---

func _test_overflow_bounds() -> void:
	print("[Corruption Fuzz] Category 3: offset/size overflow + past-EOF")
	var e0 := DSTEX_OFF_ENTRY0 + DSTEX_ENTRY_OFF_OFFSET
	# Mip 0 data offset = UINT64_MAX.
	var p1 := _next_case_path("dstex_off_u64max", "dstex")
	if _write_bytes(p1, _patched(_valid_dstex_bytes, e0, _ff(8))):
		_probe_dstex(p1, "DSTex mip0 offset=UINT64_MAX", true)
	# Mip 0 data offset just past EOF.
	var past_eof := _valid_dstex_bytes.size() + 0x10000
	var p2 := _next_case_path("dstex_off_pasteof", "dstex")
	if _write_bytes(p2, _patched(_valid_dstex_bytes, e0, _le64(past_eof))):
		_probe_dstex(p2, "DSTex mip0 offset past EOF (%d)" % past_eof, true)
	# Mip 0 compressed_size = 0xFFFFFFFF (blows past EOF).
	var p3 := _next_case_path("dstex_size_f32", "dstex")
	if _write_bytes(p3, _patched(_valid_dstex_bytes, DSTEX_OFF_ENTRY0 + DSTEX_ENTRY_OFF_CSIZE, _ff(4))):
		_probe_dstex(p3, "DSTex mip0 compressed_size=0xFFFFFFFF", true)
	# Mip 0 offset+size sum overflows: offset near UINT64_MAX - small, size large.
	var near_max := PackedByteArray([0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF])
	var p4 := _next_case_path("dstex_offsum_overflow", "dstex")
	var combo := _patched(_valid_dstex_bytes, e0, near_max)
	combo = _patched(combo, DSTEX_OFF_ENTRY0 + DSTEX_ENTRY_OFF_CSIZE, _ff(4))
	if _write_bytes(p4, combo):
		_probe_dstex(p4, "DSTex mip0 offset+size arithmetic overflow", true)
	# DSPkg data_offset = UINT64_MAX.
	var q1 := _next_case_path("dspkg_dataoff_u64max", "dspkg")
	if _write_bytes(q1, _patched(_valid_dspkg_bytes, DSPKG_OFF_DATA_OFFSET, _ff(8))):
		_probe_dspkg(q1, "DSPkg data_offset=UINT64_MAX", true)
	# DSPkg data_offset past EOF.
	var qpast := _valid_dspkg_bytes.size() + 0x10000
	var q2 := _next_case_path("dspkg_dataoff_pasteof", "dspkg")
	if _write_bytes(q2, _patched(_valid_dspkg_bytes, DSPKG_OFF_DATA_OFFSET, _le64(qpast))):
		_probe_dspkg(q2, "DSPkg data_offset past EOF (%d)" % qpast, true)
	# DSPkg entry_count = 0xFFFFFFFF (entry-table bounds blowup without OOM/abort).
	var q3 := _next_case_path("dspkg_entries_f32", "dspkg")
	if _write_bytes(q3, _patched(_valid_dspkg_bytes, DSPKG_OFF_ENTRY_COUNT, _ff(4))):
		_probe_dspkg(q3, "DSPkg entry_count=0xFFFFFFFF", true)

# -------------------------------------------------------------- category 4 ---

func _test_row_pitch_alignment() -> void:
	print("[Corruption Fuzz] Category 4: row_pitch alignment (must be % 256 == 0)")
	# Contract: misaligned pitch is rejected or safely handled, never abort.
	for pitch in [255, 257, 100]:
		var p := _next_case_path("dstex_pitch_%d" % pitch, "dstex")
		if not _write_bytes(p, _patched(_valid_dstex_bytes, DSTEX_OFF_ENTRY0 + DSTEX_ENTRY_OFF_PITCH, _le32(pitch))):
			continue
		var probe = _new_dstex()
		var hdr: Dictionary = probe.read_header(p)
		if hdr.is_empty():
			_check(true, "DSTex row_pitch=%d rejected by read_header" % pitch)
			var loaded = ResourceLoader.load(p)
			_check(loaded == null, "DSTex row_pitch=%d ResourceLoader.load returns null safely" % pitch)
		else:
			print("[INFO] DSTex row_pitch=%d accepted by read_header; checking safe handling" % pitch)
			var loaded = ResourceLoader.load(p)
			# Survival to this point proves no abort; either outcome is safe.
			_check(true, "DSTex row_pitch=%d safely handled (loader returned, no crash)" % pitch)

# -------------------------------------------------------------- category 5 ---

func _test_loader_valid_control() -> void:
	print("[Corruption Fuzz] Category 5: ResourceLoader control on valid fixtures")
	# Control: parser-level acceptance of the unmodified fixtures (no crash path).
	var probe = _new_dstex()
	_probe_dstex_control_only(VALID_DSTEX_USER, "Control valid .dstex parses")
	var reader = _new_dspkg()
	var oerr: Error = reader.open(VALID_DSPKG_USER)
	_check(oerr == OK, "Control valid .dspkg opens: " + error_string(oerr))
	# Exercising ResourceLoader.load on valid inputs must not crash either.
	# Result value is informational headless (GPU texture creation may be
	# unavailable); survival is the assertion.
	var l1 = ResourceLoader.load(VALID_DSTEX_USER)
	_check(true, "Control ResourceLoader.load(valid .dstex) returned without crash")
	print("[INFO] Control valid .dstex loader result type=" + str(typeof(l1)))
	var l2 = ResourceLoader.load(VALID_DSPKG_USER)
	_check(true, "Control ResourceLoader.load(valid .dspkg) returned without crash")
	print("[INFO] Control valid .dspkg loader result type=" + str(typeof(l2)))
	# Explicit null-safety: loading a nonexistent corrupt-path string is safe.
	var l3 = ResourceLoader.load(TMP_DIR + "/fuzz_does_not_exist.dstex")
	_check(l3 == null, "Control ResourceLoader.load(missing path) returns null safely")

func _probe_dstex_control_only(path: String, label: String) -> void:
	var probe = _new_dstex()
	_check(not probe.read_header(path).is_empty(), label + ": read_header accepts")
	_check(not probe.read_from_file(path).is_empty(), label + ": read_from_file accepts")

# ------------------------------------------------------------------- cleanup ---

func _cleanup() -> void:
	for f in [VALID_DSTEX_USER, VALID_DSPKG_USER]:
		if FileAccess.file_exists(f):
			DirAccess.remove_absolute(f)
	var dir := DirAccess.open(TMP_DIR)
	if dir != null:
		for fn in dir.get_files():
			DirAccess.remove_absolute(TMP_DIR + "/" + fn)
		DirAccess.remove_absolute(TMP_DIR)
		print("[Corruption Fuzz] Cleaned tmp fixtures under " + TMP_DIR)

# ------------------------------------------------------------------- outcome ---

func _check(condition: bool, message: String) -> void:
	if condition:
		passes += 1
		print("[PASS] " + message)
	else:
		failures += 1
		printerr("[FAIL] " + message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] " + message)

func _finish() -> void:
	print("[Corruption Fuzz] SUMMARY: %d passed, %d failed" % [passes, failures])
	if failures == 0:
		print("[Corruption Fuzz] ALL TESTS PASSED!")
		quit(0)
	else:
		printerr("[Corruption Fuzz] COMPLETED WITH " + str(failures) + " FAILURE(S)")
		quit(1)
