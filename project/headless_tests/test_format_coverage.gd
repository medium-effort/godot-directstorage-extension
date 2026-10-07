extends SceneTree

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 64
const HEIGHT := 64
var failures := 0

func _initialize() -> void:
	print("[Formats] Comprehensive format coverage verification")
	var formats := [
		{"name": "BC1", "format": RenderingDevice.DATA_FORMAT_BC1_RGB_UNORM_BLOCK, "image_format": Image.FORMAT_DXT1},
		{"name": "BC3", "format": RenderingDevice.DATA_FORMAT_BC3_UNORM_BLOCK, "image_format": Image.FORMAT_DXT5},
		{"name": "BC5", "format": RenderingDevice.DATA_FORMAT_BC5_UNORM_BLOCK, "image_format": Image.FORMAT_RGTC_RG},
		{"name": "BC6H", "format": RenderingDevice.DATA_FORMAT_BC6H_SFLOAT_BLOCK, "image_format": Image.FORMAT_BPTC_RGBF},
		{"name": "BC7", "format": RenderingDevice.DATA_FORMAT_BC7_UNORM_BLOCK, "image_format": Image.FORMAT_BPTC_RGBA},
		{"name": "R8", "format": RenderingDevice.DATA_FORMAT_R8_UNORM, "image_format": Image.FORMAT_R8},
		{"name": "RG8", "format": RenderingDevice.DATA_FORMAT_R8G8_UNORM, "image_format": Image.FORMAT_RG8},
		{"name": "RGBA8", "format": RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, "image_format": Image.FORMAT_RGBA8},
		{"name": "R16", "format": RenderingDevice.DATA_FORMAT_R16_UNORM, "image_format": Image.FORMAT_R16},
		{"name": "R32F", "format": RenderingDevice.DATA_FORMAT_R32_SFLOAT, "image_format": Image.FORMAT_RF}
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
		var image_fmt: int = int(item["image_format"])
		var img := Image.create(WIDTH, HEIGHT, false, image_fmt)
		var raw_payload := img.get_data()
		for i in raw_payload.size():
			raw_payload[i] = (i * 17) % 256

		var dstex := DSTex.new()
		var dstex_path := "user://test_format_" + str(item["name"]) + ".dstex"
		var write_err: Error = dstex.write_to_file(dstex_path, raw_payload, WIDTH, HEIGHT, image_fmt, 1, 1)
		if write_err != OK:
			_fail(str(item["name"]) + " dstex write failed: " + error_string(write_err))
			continue

		var header: Dictionary = dstex.read_header(dstex_path)

		var texture_format := RDTextureFormat.new()
		texture_format.format = format
		texture_format.width = WIDTH
		texture_format.height = HEIGHT
		texture_format.mipmaps = 1
		texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		# Apply shareable format policy exactly matching loader/preloader (Q44)
		if ClassDB.class_exists("DSTex"):
			var srgb_fmt: int = DSTex.get_srgb_rd_format(format)
			if srgb_fmt != RenderingDevice.DATA_FORMAT_MAX and srgb_fmt != format:
				texture_format.add_shareable_format(format)
				texture_format.add_shareable_format(srgb_fmt)
		var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
		_check(texture.is_valid(), str(item["name"]) + " texture RID is valid")
		var request := DSRequest.new()
		request.file_path = dstex_path
		request.size = int(header.get("compressed_size", 0))
		request.uncompressed_size = int(header.get("original_size", 0))
		request.compression_format = DSRequest.COMPRESSION_GDEFLATE
		request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
		request.destination_resource = texture
		request.first_subresource = 0
		request.subresource_count = 1
		request.texture_format = int(rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture, 0))
		var accepted: bool = ctx.enqueue_request(request)
		_check(accepted, str(item["name"]) + " GPU request accepted (error=" + str(request.get_error_message()) + ")")
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
