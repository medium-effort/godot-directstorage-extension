class_name GDeflateTextureBenchmarkCommon
extends RefCounted

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 1024
const HEIGHT := 1024
const COUNT := 100

static func make_fixture(path: String) -> Dictionary:
	var footprints := Verification.mip_footprints(WIDTH, HEIGHT, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, 1)
	var payload := Verification.make_pattern_payload(footprints)
	var result := Verification.write_gdeflate_payload(path, payload, 1)
	result["payload"] = payload
	result["path"] = path
	result["count"] = COUNT
	return result

static func print_environment(ctx: Variant) -> void:
	print("[Benchmark] backend=", ctx.get_active_backend_name(), " d3d12=", ctx.is_d3d12_backend(), " gpu_decompression=", ctx.is_gpu_decompression_available())

static func finish(loop: SceneTree, failures: int) -> void:
	loop.quit(1 if failures > 0 else 0)

static func run_gpu_texture_batch(ctx: Variant, rd: RenderingDevice, fixture: Dictionary) -> Dictionary:
	var textures: Array[RID] = []
	var requests: Array = []
	var format := RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	for _index in int(fixture["count"]):
		var texture_format := RDTextureFormat.new()
		texture_format.format = format
		texture_format.width = WIDTH
		texture_format.height = HEIGHT
		texture_format.mipmaps = 1
		texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
		if not texture.is_valid():
			return {"ok": false, "reason": "GPU texture creation failed"}
		textures.append(texture)
		var request := DSRequest.new()
		request.file_path = fixture["path"]
		request.size = int(fixture["compressed"].size())
		request.uncompressed_size = fixture["payload"].size()
		request.compression_format = DSRequest.COMPRESSION_GDEFLATE
		request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
		request.destination_resource = texture
		request.first_subresource = 0
		request.subresource_count = 1
		request.texture_format = format
		if not ctx.enqueue_request(request):
			return {"ok": false, "reason": "GPU texture request was rejected"}
		requests.append(request)
	var start_usec := Time.get_ticks_usec()
	ctx.flush_batch()
	var completed: bool = bool(ctx.wait_for_completion(30000))
	var failed := 0
	for request in requests:
		if request.status != DSRequest.STATUS_COMPLETED:
			failed += 1
	return {
		"ok": completed and failed == 0,
		"elapsed_ms": float(Time.get_ticks_usec() - start_usec) / 1000.0,
		"failed": failed
	}
