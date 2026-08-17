extends SceneTree

const Verification = preload("uid://ctg5hksd01gao")
var failures := 0

func _initialize() -> void:
	print("[Footprint] row-pitch alignment verification")
	var importer_script = load("uid://kqg57i3wmiw")
	if importer_script == null:
		_fail("DirectStorage texture importer script is unavailable")
	else:
		var importer = importer_script
		for size in [Vector2i(513, 513), Vector2i(1024, 768), Vector2i(319, 127), Vector2i(2049, 17)]:
			var layout: Array = importer._get_mip_row_layout(size.x, size.y, Image.FORMAT_RGBA8)
			var row_pitch: int = importer._align_up(int(layout[0]), Verification.ROW_PITCH_ALIGNMENT)
			_check(row_pitch % 256 == 0, str(size) + " row pitch is 256-byte aligned")
			_check(row_pitch >= int(layout[0]), str(size) + " row pitch covers every source row")
			print("[Footprint] ", size, " row_bytes=", layout[0], " rows=", layout[1], " row_pitch=", row_pitch)
			var padded_input := PackedByteArray()
			padded_input.resize(size.x * size.y * 4)
			var padded: PackedByteArray = importer._pad_texture_data(padded_input, size.x, size.y, Image.FORMAT_RGBA8, 1)
			_check(padded.size() == row_pitch * size.y, str(size) + " padded footprint has the expected size")
	_finish()

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
