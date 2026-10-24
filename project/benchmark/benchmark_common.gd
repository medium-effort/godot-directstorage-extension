class_name GDeflateTextureBenchmarkCommon
extends RefCounted

## Fixture caveats — read before quoting any number this bench prints.
##
## 1. TWO ARTEFACTS PER FIXTURE, ON PURPOSE. `path` is a raw GDeflate stream and
##    is only used by the CPU reference loop in bench_throughput.gd. The GPU path
##    needs `dstex_path`: DSRequest -> DESTINATION_GPU_TEXTURE is rejected with
##    "GPU texture destinations require v2 footprint-padded formats" unless the
##    backend can parse per-mip offsets and row pitches out of a .dstex header
##    (ds_win32_backend.cpp get_texture_mip_entries). A raw GDeflate blob carries
##    no footprint, and the backend deliberately refuses to guess one: guessing
##    width * bytes_per_pixel would break on BC7/BC5 and on any width whose
##    copyable pitch differs from the natural row size.
## 2. THE PAYLOAD IS A MODULO-256 RAMP, NOT IMAGE DATA. It compresses into very
##    long matches, so both the CPU and the GPU numbers come out optimistically
##    high and are NOT representative of real BC7/RGBA art. Use this as a
##    relative regression check between builds, never as a throughput claim.
##    For a realistic fixture, feed image bytes (see the R0 gate harness) and
##    expect much lower numbers.
## 3. THE GPU NUMBER IS THE PRODUCTION PATH, not a pure DirectStorage API number:
##    it includes the DSTex header parse, the per-request file open, the mip
##    split into child requests and the GPU texture allocation.

const Verification = preload("uid://ctg5hksd01gao")
const WIDTH := 1024
const HEIGHT := 1024
const COUNT := 100

static func make_fixture(path: String) -> Dictionary:
	var footprints := Verification.mip_footprints(WIDTH, HEIGHT, RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, 1)
	var payload := Verification.make_pattern_payload(footprints)
	var result := Verification.write_gdeflate_payload(path, payload, 1)
	if not bool(result.get("ok", false)):
		return result

	# GPU texture destinations are only accepted when the backend can parse mip
	# footprints out of the file, which it does for .dstex only (backend
	# get_texture_mip_entries). Write a real single-mip .dstex next to the raw
	# stream; the raw one stays for the CPU reference loop. RGBA8 at 1024 wide is
	# already 256-byte row-pitch aligned, so the natural layout is also the
	# footprint-padded one.
	var dstex_path := path.get_basename() + ".dstex"
	if not ClassDB.class_exists("DSTex"):
		return {"ok": false, "reason": "DSTex is not registered"}
	var dstex: Variant = ClassDB.instantiate("DSTex")
	var write_error: int = dstex.write_to_file(dstex_path, payload, WIDTH, HEIGHT,
		Image.FORMAT_RGBA8, 1, 1, false, false)
	if write_error != OK:
		return {"ok": false, "reason": "DSTex fixture write failed with error %d" % write_error}

	result["dstex_path"] = dstex_path
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
	# Must be the .dstex, not the raw stream: see caveat 1 at the top of this file.
	var dstex_path: String = String(fixture.get("dstex_path", ""))
	if dstex_path.is_empty() or not FileAccess.file_exists(dstex_path):
		return {"ok": false, "reason": "fixture has no .dstex; GPU texture requests need v2 footprint metadata"}
	var dstex_bytes := FileAccess.get_file_as_bytes(dstex_path).size()
	for _index in int(fixture["count"]):
		var texture_format := RDTextureFormat.new()
		texture_format.format = format
		texture_format.width = WIDTH
		texture_format.height = HEIGHT
		texture_format.mipmaps = 1
		texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
		var texture := rd.texture_create(texture_format, RDTextureView.new(), [])
		if not texture.is_valid():
			_free_textures(rd, textures)
			return {"ok": false, "reason": "GPU texture creation failed"}
		textures.append(texture)
		var request := DSRequest.new()
		request.file_path = dstex_path
		# Superseded by the parsed mip entry once the backend splits the request,
		# but the pre-split validation still requires both to be non-zero.
		request.size = dstex_bytes
		request.uncompressed_size = fixture["payload"].size()
		request.compression_format = DSRequest.COMPRESSION_GDEFLATE
		request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
		request.destination_resource = texture
		request.first_subresource = 0
		request.subresource_count = 1
		# Do NOT set texture_format here. The backend compares it against
		# DRIVER_RESOURCE_TEXTURE_DATA_FORMAT (a driver-native value) and skips the
		# check when it is 0. GDScript cannot query that value safely (render-thread
		# only), so the correct request is "unspecified" - the same thing the .dstex
		# loader path does. Setting the Godot DATA_FORMAT_* enum here fails with
		# "GPU texture format metadata does not match the destination resource".
		if not ctx.enqueue_request(request):
			_free_textures(rd, textures)
			return {"ok": false, "reason": "GPU texture request was rejected: " + String(request.get_error_message())}
		requests.append(request)
	var start_usec := Time.get_ticks_usec()
	ctx.flush_batch()
	var completed: bool = bool(ctx.wait_for_completion(30000))
	var failed := 0
	for request in requests:
		if request.status != DSRequest.STATUS_COMPLETED:
			failed += 1
	if completed:
		# Only after the completion fence: freeing while hardware still owns the
		# texture would page-fault the DMA engine.
		_free_textures(rd, textures)
	else:
		# Timed out, so the batch may still own these textures: they are
		# deliberately not freed, because a use-after-free here page-faults the
		# DMA engine. RenderingDevice will report these RIDs as leaked at
		# shutdown - that is expected on this failure path, not a regression.
		push_warning("GPU texture batch did not complete; leaving %d destination texture(s) allocated" % textures.size())
	return {
		"ok": completed and failed == 0,
		"elapsed_ms": float(Time.get_ticks_usec() - start_usec) / 1000.0,
		"failed": failed
	}


static func _free_textures(rd: RenderingDevice, textures: Array[RID]) -> void:
	for texture in textures:
		if texture.is_valid():
			rd.free_rid(texture)
	textures.clear()
