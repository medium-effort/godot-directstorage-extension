extends SceneTree

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 128
const HEIGHT := 96
const MIP_COUNT := 8
var failures := 0

func _initialize() -> void:
	print("[Subresource] mip range verification")
	var footprints := Verification.mip_footprints(WIDTH, HEIGHT, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, MIP_COUNT)
	var payload := Verification.make_pattern_payload(footprints)
	var expected_size := 0
	for footprint in footprints:
		expected_size += int(footprint["size"])
		_check(int(footprint["row_pitch"]) % 256 == 0, "mip " + str(footprint["mip"]) + " row pitch is aligned")
	_check(payload.size() == expected_size, "all mip footprints are contiguous and in bounds")
	for footprint in footprints:
		var mip_name := "Mip " + str(footprint["mip"]) + " (" + str(footprint["width"]) + "x" + str(footprint["height"]) + ")"
		print("[Subresource] ", mip_name, " footprint=", footprint["size"], " bytes")
		_check(int(footprint["size"]) > 0, mip_name + " has non-zero footprint")
	if ClassDB.class_exists("DSContext") and ClassDB.class_exists("DSRequest") and RenderingServer.get_rendering_device() != null:
		_run_gpu_request(footprints, payload)
	else:
		print("[SKIP] GPU subresource submission requires DSContext, DSRequest, and RenderingDevice")
	_finish()

func _run_gpu_request(footprints: Array, payload: PackedByteArray) -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		print("[SKIP] D3D12 DirectStorage backend is unavailable")
		return
	var rd := RenderingServer.get_rendering_device()
	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = WIDTH
	texture_format.height = HEIGHT
	texture_format.mipmaps = MIP_COUNT
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
	_check(texture.is_valid(), "mip-chain texture RID is valid")
	var write_result := Verification.write_gdeflate_payload("user://test_subresource_range.gdeflate", payload)
	if not bool(write_result.get("ok", false)):
		_fail(String(write_result.get("reason", "payload write failed")))
		return
	var request := DSRequest.new()
	request.file_path = "user://test_subresource_range.gdeflate"
	request.size = int(write_result["compressed"].size())
	request.uncompressed_size = payload.size()
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
	request.destination_resource = texture
	request.first_subresource = 0
	request.subresource_count = MIP_COUNT
	request.texture_format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	_check(ctx.enqueue_request(request), "MULTIPLE_SUBRESOURCES_RANGE request accepted")
	ctx.flush_batch()
	_check(ctx.wait_for_completion(5000) and request.status == DSRequest.STATUS_COMPLETED, "mip-chain request completed without subresource errors")

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("[Subresource] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
