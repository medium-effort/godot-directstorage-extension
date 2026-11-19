extends SceneTree

## Verifies the D3D12 footprint contract that the importer relies on: writing a
## natural (unpadded) mip chain through DSTex::write_to_file() must produce
## 256-byte row-pitch-aligned mip metadata. The importer hands the writer the
## raw Image::get_data() buffer and never pads rows itself, so the writer is the
## only place this alignment can come from.

const Verification = preload("uid://ctg5hksd01gao")
const TMP_DIR := "user://footprint_calc"
var failures := 0

func _initialize() -> void:
	print("[Footprint] row-pitch alignment verification")
	var dstex = ClassDB.instantiate("DSTex")
	if dstex == null:
		_fail("DSTex class is unavailable")
		_finish()
		return
	DirAccess.make_dir_recursive_absolute(TMP_DIR)
	for size in [Vector2i(513, 513), Vector2i(1024, 768), Vector2i(319, 127), Vector2i(2049, 17)]:
		_check_size(dstex, size.x, size.y)
	_finish()

func _check_size(dstex, width: int, height: int) -> void:
	var label := str(Vector2i(width, height))
	var row_bytes := width * 4
	var natural := PackedByteArray()
	natural.resize(row_bytes * height)
	var path := TMP_DIR + "/%dx%d.dstex" % [width, height]
	# Uncompressed write: level 0 keeps the payload a verbatim footprint, so the
	# assertions below describe the bytes a loader would upload.
	var err: Error = dstex.write_to_file(path, natural, width, height, Image.FORMAT_RGBA8, 1, 0, false, false, width, height, 0, 0)
	if err != OK:
		_fail(label + " natural-buffer write failed (" + error_string(err) + ")")
		return
	var header: Dictionary = dstex.read_header(path)
	if header.is_empty():
		_fail(label + " header unreadable")
		return
	var row_pitch: int = int(header["mip_row_pitches"][0])
	var footprint: int = int(header["mip_uncompressed_sizes"][0])
	_check(row_pitch % Verification.ROW_PITCH_ALIGNMENT == 0, label + " row pitch is 256-byte aligned")
	_check(row_pitch >= row_bytes, label + " row pitch covers every source row")
	_check(footprint == row_pitch * height, label + " footprint is row_pitch * row_count")
	print("[Footprint] ", label, " row_bytes=", row_bytes, " rows=", height, " row_pitch=", row_pitch)
	DirAccess.remove_absolute(ProjectSettings.globalize_path(path))

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("[Footprint] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
