extends SceneTree

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 5000
const HEIGHT := 5000
const MIP_COUNT := 14
var failures := 0

func _initialize() -> void:
	print("[Oversized] staging-buffer split verification")
	var staging_mb := 128
	if ProjectSettings.has_setting("directstorage/staging_buffer_size_mb"):
		staging_mb = int(ProjectSettings.get_setting("directstorage/staging_buffer_size_mb"))
	var staging_bytes := maxi(1, staging_mb) * 1024 * 1024
	var footprints := Verification.mip_footprints(WIDTH, HEIGHT, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, MIP_COUNT)
	var total := 0
	var requests := 0
	for footprint in footprints:
		total += int(footprint["size"])
		if int(footprint["size"]) > 0:
			requests += 1
			_check(int(footprint["size"]) <= staging_bytes, "mip " + str(footprint["mip"]) + " fits one TEXTURE_REGION staging request")
	_check(total > staging_bytes, "5000x5000 mip chain exceeds the configured staging buffer")
	_check(requests == footprints.size(), "oversized chain is represented as one request per mip")
	print("[Oversized] footprint=", total, " staging=", staging_bytes, " requests=", requests)
	if Verification.command_line_has("--run-large"):
		_run_gpu_mip_requests(footprints)
	else:
		print("[SKIP] Actual 128+ MiB upload is opt-in via --run-large to avoid allocating test memory by default")
	_finish()

func _run_gpu_mip_requests(footprints: Array) -> void:
	if not (ClassDB.class_exists("DSContext") and ClassDB.class_exists("DSRequest")):
		print("[SKIP] Large GPU request requires DSContext and DSRequest")
		return
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		print("[SKIP] D3D12 DirectStorage backend is unavailable")
		return
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		print("[SKIP] RenderingDevice is unavailable")
		return
	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = WIDTH
	texture_format.height = HEIGHT
	texture_format.mipmaps = MIP_COUNT
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
	_check(texture.is_valid(), "oversized texture RID is valid")
	var requests: Array = []
	for footprint in footprints:
		var payload := Verification.make_pattern_payload([footprint])
		var path := "user://test_oversized_mip_" + str(footprint["mip"]) + ".gdeflate"
		var write_result := Verification.write_gdeflate_payload(path, payload)
		if not bool(write_result.get("ok", false)):
			_fail("mip " + str(footprint["mip"]) + " payload creation failed")
			continue
		var request := DSRequest.new()
		request.file_path = path
		request.size = int(write_result["compressed"].size())
		request.uncompressed_size = payload.size()
		request.compression_format = DSRequest.COMPRESSION_GDEFLATE
		request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
		request.destination_resource = texture
		request.first_subresource = int(footprint["mip"])
		request.subresource_count = 1
		request.texture_format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
		_check(ctx.enqueue_request(request), "mip " + str(footprint["mip"]) + " TEXTURE_REGION request accepted")
		requests.append(request)
	ctx.flush_batch()
	_check(ctx.wait_for_completion(30000), "all oversized mip requests completed")
	for request in requests:
		_check(request.status == DSRequest.STATUS_COMPLETED, "oversized mip request completed")

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("[Oversized] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
