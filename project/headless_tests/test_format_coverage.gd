extends SceneTree

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 64
const HEIGHT := 64
var failures := 0

func _initialize() -> void:
	print("[Formats] BC format coverage verification")
	var formats := [
		{"name": "BC1", "format": RenderingDevice.DATA_FORMAT_BC1_RGB_UNORM_BLOCK},
		{"name": "BC3", "format": RenderingDevice.DATA_FORMAT_BC3_UNORM_BLOCK},
		{"name": "BC5", "format": RenderingDevice.DATA_FORMAT_BC5_UNORM_BLOCK},
		{"name": "BC6H", "format": RenderingDevice.DATA_FORMAT_BC6H_SFLOAT_BLOCK},
		{"name": "BC7", "format": RenderingDevice.DATA_FORMAT_BC7_UNORM_BLOCK}
	]
	for item in formats:
		var footprints := Verification.mip_footprints(WIDTH, HEIGHT, int(item["format"]), 1)
		var footprint: Dictionary = footprints[0]
		_check(int(footprint["row_pitch"]) % 256 == 0, str(item["name"]) + " row pitch is aligned")
		_check(int(footprint["size"]) > 0, str(item["name"]) + " footprint is non-zero")
		print("[Formats] ", item["name"], " row_bytes=", footprint["row_bytes"], " footprint=", footprint["size"])
	if ClassDB.class_exists("DSContext") and ClassDB.class_exists("DSRequest") and RenderingServer.get_rendering_device() != null:
		_run_gpu_formats(formats)
	else:
		print("[SKIP] Native GPU format submission requires DSContext, DSRequest, and RenderingDevice")
	_finish()

func _run_gpu_formats(formats: Array) -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		print("[SKIP] D3D12 DirectStorage backend is unavailable")
		return
	var rd := RenderingServer.get_rendering_device()
	for item in formats:
		var format: int = int(item["format"])
		var footprints := Verification.mip_footprints(WIDTH, HEIGHT, format, 1)
		var payload := Verification.make_pattern_payload(footprints)
		var write_result := Verification.write_gdeflate_payload("user://test_format_" + str(item["name"]) + ".gdeflate", payload)
		if not bool(write_result.get("ok", false)):
			_fail(str(item["name"]) + " payload creation failed: " + String(write_result.get("reason", "unknown error")))
			continue
		var texture_format := RDTextureFormat.new()
		texture_format.format = format
		texture_format.width = WIDTH
		texture_format.height = HEIGHT
		texture_format.mipmaps = 1
		texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
		_check(texture.is_valid(), str(item["name"]) + " texture RID is valid")
		var request := DSRequest.new()
		request.file_path = "user://test_format_" + str(item["name"]) + ".gdeflate"
		request.size = int(write_result["compressed"].size())
		request.uncompressed_size = payload.size()
		request.compression_format = DSRequest.COMPRESSION_GDEFLATE
		request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
		request.destination_resource = texture
		request.first_subresource = 0
		request.subresource_count = 1
		request.texture_format = format
		_check(ctx.enqueue_request(request), str(item["name"]) + " GPU request accepted")
		ctx.flush_batch()
		_check(ctx.wait_for_completion(5000) and request.status == DSRequest.STATUS_COMPLETED, str(item["name"]) + " GPU request completed; inspect output visually")

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("[Formats] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures, " visual=manual inspection required")
	quit(1 if failures > 0 else 0)
