extends SceneTree

var failures := 0

func _initialize() -> void:
	print("[V1] legacy .dstex CPU fallback verification")
	var path := "res://test_assets/v1_compat.dstex"
	for argument in OS.get_cmdline_args():
		if argument.begins_with("--v1-path="):
			path = argument.trim_prefix("--v1-path=")
	if not FileAccess.file_exists(path):
		print("[SKIP] v1 fixture is unavailable: ", path)
		_finish()
		return
	if not ClassDB.class_exists("DSTex"):
		_fail("DSTex class is not registered")
		_finish()
		return
	var dstex: Variant = ClassDB.instantiate("DSTex")
	var header: Dictionary = dstex.read_header(path)
	_check(int(header.get("version", 0)) == 1, "fixture is a version 1 .dstex")
	var decoded: Dictionary = dstex.read_from_file(path)
	_check(decoded.has("compressed_data"), "v1 reader exposes the compressed payload")
	if int(header.get("mip_count", 0)) == 1 and ClassDB.class_exists("GDeflateDecompressor"):
		var decompressor: Variant = ClassDB.instantiate("GDeflateDecompressor")
		var raw: PackedByteArray = decompressor.decompress(decoded["compressed_data"], int(header.get("original_size", 0)))
		_check(raw.size() == int(header.get("original_size", 0)), "single-mip v1 payload uses the CPU decompression fallback")
	else:
		print("[SKIP] CPU payload check requires a single-mip fixture and GDeflateDecompressor")
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
	print("[V1] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
