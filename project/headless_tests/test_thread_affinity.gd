extends SceneTree

const TEXTURE_WIDTH := 64
const TEXTURE_HEIGHT := 4
# Full mip chain is REQUIRED: Image::create_from_data(..., use_mipmaps=true)
# expects every level down to 1x1 (64x4 -> 7 levels, 1404 bytes). A partial
# chain builds a payload Godot rejects (1404-vs-1280 style size error).
const MIP_COUNT := 7
const ROW_PITCH := 256
const BASE_MIP_BYTES := ROW_PITCH * TEXTURE_HEIGHT
const MIP1_WIDTH := TEXTURE_WIDTH / 2
const MIP1_HEIGHT := TEXTURE_HEIGHT / 2
const MIP1_OFFSET := BASE_MIP_BYTES
const PAYLOAD_SIZE := BASE_MIP_BYTES + ROW_PITCH * MIP1_HEIGHT + ROW_PITCH * 5

var failures := 0

func _initialize() -> void:
	print("[Thread Affinity] Starting GPU destination & thread affinity test")
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest") or not ClassDB.class_exists("GDeflateCompressor"):
		_fail("DSContext, DSRequest, or GDeflateCompressor is not registered.")
		_finish()
		return

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail("RenderingDevice is unavailable; run with --rendering-driver d3d12.")
		_finish()
		return

	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		_fail("DirectStorage Win32 D3D12 backend is not available.")
		_finish()
		return

	_test_gpu_texture_destination(ctx, rd)
	_test_gpu_buffer_destination(ctx, rd)
	await _test_off_thread_enqueue(ctx, rd)
	_test_resource_loader_gpu_texture(rd)
	await _test_worker_loader_fallback()
	_finish()

func _test_gpu_texture_destination(ctx: Object, rd: RenderingDevice) -> void:
	print("[Thread Affinity] Testing GPU texture destination...")
	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = TEXTURE_WIDTH
	texture_format.height = TEXTURE_HEIGHT
	texture_format.mipmaps = MIP_COUNT
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	var texture_rid := rd.texture_create(texture_format, RDTextureView.new(), [])
	_check(texture_rid.is_valid(), "texture_create returned a valid RID")
	if not texture_rid.is_valid():
		return

	var payload := _make_texture_payload()
	var dstex := DSTex.new()
	var dstex_path := "user://test_thread_affinity_texture.dstex"
	var write_err: Error = dstex.write_to_file(dstex_path, payload, TEXTURE_WIDTH, TEXTURE_HEIGHT, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	_check(write_err == OK, "DSTex wrote valid v2 footprint-padded texture: " + error_string(write_err))
	if write_err != OK:
		rd.free_rid(texture_rid)
		return

	var header: Dictionary = dstex.read_header(dstex_path)
	_check(int(header.get("version", 0)) == 2, "DSTex header is version 2")

	var request := DSRequest.new()
	request.file_path = dstex_path
	request.size = int(header.get("compressed_size", 0))
	request.uncompressed_size = int(header.get("original_size", 0))
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
	request.destination_resource = texture_rid
	request.first_subresource = 0
	request.subresource_count = MIP_COUNT
	request.texture_format = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture_rid, 0)

	var enqueued: bool = ctx.enqueue_request(request)
	_check(enqueued, "enqueue_request accepted GPU texture destination on main thread: " + request.error_message)
	if not enqueued:
		rd.free_rid(texture_rid)
		return

	ctx.flush_batch()
	var completed: bool = ctx.wait_for_completion(5000)
	_check(completed and request.status == DSRequest.STATUS_COMPLETED, "GPU texture request completed successfully: status=" + str(request.status))

	if completed and request.status == DSRequest.STATUS_COMPLETED:
		var mip0_data: PackedByteArray = rd.texture_get_data(texture_rid, 0)
		_check(mip0_data.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4, "mip 0 readback has expected size")
		var mip0_match := true
		for y in range(TEXTURE_HEIGHT):
			for x in range(TEXTURE_WIDTH):
				var read_off := (y * TEXTURE_WIDTH + x) * 4
				var src_off := y * ROW_PITCH + x * 4
				for c in range(4):
					if mip0_data[read_off + c] != payload[src_off + c]:
						mip0_match = false
						break
		_check(mip0_match, "mip 0 texels match source payload byte-for-byte")

		if mip0_data.size() >= (TEXTURE_WIDTH * TEXTURE_HEIGHT + MIP1_WIDTH * MIP1_HEIGHT) * 4:
			var mip1_match := true
			var mip1_base := TEXTURE_WIDTH * TEXTURE_HEIGHT * 4
			for y in range(MIP1_HEIGHT):
				for x in range(MIP1_WIDTH):
					var read_off := mip1_base + (y * MIP1_WIDTH + x) * 4
					var src_off := MIP1_OFFSET + y * ROW_PITCH + x * 4
					for c in range(4):
						if mip0_data[read_off + c] != payload[src_off + c]:
							mip1_match = false
							break
			_check(mip1_match, "mip 1 texels match source payload byte-for-byte")
		else:
			print("[INFO] texture_get_data(layer 0) returned base mip only: size=", mip0_data.size())

	rd.free_rid(texture_rid)

func _test_off_thread_enqueue(ctx: Object, rd: RenderingDevice) -> void:
	print("[Thread Affinity] Testing off-render-thread enqueueing (worker thread)...")
	const BUFFER_SIZE := 2048
	var buffer_rid := rd.storage_buffer_create(BUFFER_SIZE)
	_check(buffer_rid.is_valid(), "off-thread test storage_buffer_create returned a valid RID")
	if not buffer_rid.is_valid():
		return

	var raw_data := PackedByteArray()
	raw_data.resize(BUFFER_SIZE)
	for i in range(BUFFER_SIZE):
		raw_data[i] = (i * 31 + 7) % 256

	var compressor := GDeflateCompressor.new()
	var compressed: PackedByteArray = compressor.compress(raw_data, 1)
	var payload_path := "user://test_thread_affinity_offthread.gdeflate"
	var payload_file := FileAccess.open(payload_path, FileAccess.WRITE)
	if payload_file == null:
		_fail("Could not write off-thread payload file")
		rd.free_rid(buffer_rid)
		return
	payload_file.store_buffer(compressed)
	payload_file.close()

	var request := DSRequest.new()
	request.file_path = payload_path
	request.size = compressed.size()
	request.uncompressed_size = raw_data.size()
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_BUFFER
	request.destination_resource = buffer_rid

	# Enqueue from a worker thread to trigger render-thread marshaling in enqueue_request
	var worker_thread := Thread.new()
	worker_thread.start(func(): return ctx.enqueue_request(request))
	while worker_thread.is_alive():
		await process_frame
	var thread_enqueued: bool = bool(worker_thread.wait_to_finish())
	var thread_err := request.error_message

	_check(thread_enqueued, "enqueue_request succeeded from worker thread: " + thread_err)
	if not thread_enqueued:
		rd.free_rid(buffer_rid)
		return

	ctx.flush_batch()
	var completed: bool = ctx.wait_for_completion(5000)
	_check(completed and request.status == DSRequest.STATUS_COMPLETED, "off-thread enqueued request completed successfully")

	if completed and request.status == DSRequest.STATUS_COMPLETED:
		var readback: PackedByteArray = rd.buffer_get_data(buffer_rid)
		_check(readback.size() >= BUFFER_SIZE, "off-thread buffer readback size matches")
		var match_ok := true
		for i in range(BUFFER_SIZE):
			if readback[i] != raw_data[i]:
				match_ok = false
				break
		_check(match_ok, "off-thread enqueued buffer bytes match byte-for-byte")

	rd.free_rid(buffer_rid)

func _test_resource_loader_gpu_texture(rd: RenderingDevice) -> void:
	print("[Thread Affinity] Testing ResourceLoader GPU texture decompression...")
	var dstex_path := "user://test_thread_affinity_texture.dstex"
	var loaded = ResourceLoader.load(dstex_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded != null, "ResourceLoader successfully loaded .dstex texture")
	if loaded == null:
		return

	_check(loaded is Texture2D, "Loaded resource is Texture2D")
	var loaded_rid: RID
	if loaded is Texture2DRD:
		loaded_rid = (loaded as Texture2DRD).texture_rd_rid
	elif loaded.has_method("get_rid"):
		loaded_rid = loaded.get_rid()

	if loaded_rid.is_valid():
		var mip0: PackedByteArray = rd.texture_get_data(loaded_rid, 0)
		_check(mip0.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4, "ResourceLoader texture mip 0 readback has expected size")
		var payload := _make_texture_payload()
		var match_ok := true
		for y in range(TEXTURE_HEIGHT):
			for x in range(TEXTURE_WIDTH):
				var read_off := (y * TEXTURE_WIDTH + x) * 4
				var src_off := y * ROW_PITCH + x * 4
				for c in range(4):
					if mip0[read_off + c] != payload[src_off + c]:
						match_ok = false
						break
		_check(match_ok, "ResourceLoader loaded texture texels match source payload byte-for-byte")
		rd.free_rid(loaded_rid)
	else:
		_fail("Loaded texture did not yield a valid RID for readback")

func _test_worker_loader_fallback() -> void:
	print("[Thread Affinity] Testing sub-thread loader CPU fallback (no render-thread block)...")
	var dstex_path := "user://test_thread_affinity_texture.dstex"
	var req_err: Error = ResourceLoader.load_threaded_request(dstex_path, "", true, ResourceLoader.CACHE_MODE_IGNORE)
	_check(req_err == OK, "load_threaded_request(use_sub_threads=true) accepted: " + error_string(req_err))
	if req_err != OK:
		return
	var start_msec := Time.get_ticks_msec()
	var status := ResourceLoader.load_threaded_get_status(dstex_path)
	var spins := 0
	while status != ResourceLoader.THREAD_LOAD_LOADED and status != ResourceLoader.THREAD_LOAD_FAILED and status != ResourceLoader.THREAD_LOAD_INVALID_RESOURCE and spins < 600:
		await process_frame
		status = ResourceLoader.load_threaded_get_status(dstex_path)
		spins += 1
	var elapsed_msec := Time.get_ticks_msec() - start_msec
	_check(status == ResourceLoader.THREAD_LOAD_LOADED, "sub-thread load reached LOADED: status=" + str(status))
	_check(elapsed_msec < 10000, "sub-thread load completed without render-thread stall (" + str(elapsed_msec) + "ms)")
	if status != ResourceLoader.THREAD_LOAD_LOADED:
		return
	var loaded = ResourceLoader.load_threaded_get(dstex_path)
	_check(loaded is Texture2D, "sub-thread loaded resource is Texture2D")
	_check(loaded is ImageTexture, "sub-thread load falls back to worker-safe ImageTexture")
	_check(not (loaded is Texture2DRD), "sub-thread load never takes GPU Texture2DRD path")
	if loaded is Texture2D:
		_check(loaded.get_width() == TEXTURE_WIDTH and loaded.get_height() == TEXTURE_HEIGHT, "sub-thread fallback texture has expected dimensions")
		var img: Image = loaded.get_image()
		_check(img != null and not img.is_empty(), "sub-thread fallback texture carries CPU pixels")

func _test_gpu_buffer_destination(ctx: Object, rd: RenderingDevice) -> void:
	print("[Thread Affinity] Testing GPU buffer destination...")
	const BUFFER_SIZE := 1024
	var buffer_rid := rd.storage_buffer_create(BUFFER_SIZE)
	_check(buffer_rid.is_valid(), "storage_buffer_create returned a valid RID")
	if not buffer_rid.is_valid():
		return

	var raw_data := PackedByteArray()
	raw_data.resize(BUFFER_SIZE)
	for i in range(BUFFER_SIZE):
		raw_data[i] = (i * 17 + 11) % 256

	var compressor := GDeflateCompressor.new()
	var compressed: PackedByteArray = compressor.compress(raw_data, 1)
	_check(not compressed.is_empty(), "GDeflate compressed buffer payload")
	if compressed.is_empty():
		rd.free_rid(buffer_rid)
		return

	var payload_path := "user://test_thread_affinity_buffer.gdeflate"
	var payload_file := FileAccess.open(payload_path, FileAccess.WRITE)
	if payload_file == null:
		_fail("Could not write buffer payload file")
		rd.free_rid(buffer_rid)
		return
	payload_file.store_buffer(compressed)
	payload_file.close()

	var request := DSRequest.new()
	request.file_path = payload_path
	request.size = compressed.size()
	request.uncompressed_size = raw_data.size()
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_BUFFER
	request.destination_resource = buffer_rid

	var enqueued: bool = ctx.enqueue_request(request)
	_check(enqueued, "enqueue_request accepted GPU buffer destination on main thread: " + request.error_message)
	if not enqueued:
		rd.free_rid(buffer_rid)
		return

	ctx.flush_batch()
	var completed: bool = ctx.wait_for_completion(5000)
	_check(completed and request.status == DSRequest.STATUS_COMPLETED, "GPU buffer request completed successfully: status=" + str(request.status))

	if completed and request.status == DSRequest.STATUS_COMPLETED:
		var readback: PackedByteArray = rd.buffer_get_data(buffer_rid)
		_check(readback.size() >= BUFFER_SIZE, "buffer readback has expected size")
		var buffer_match := true
		for i in range(BUFFER_SIZE):
			if readback[i] != raw_data[i]:
				buffer_match = false
				break
		_check(buffer_match, "buffer bytes match source data byte-for-byte")

	rd.free_rid(buffer_rid)

func _make_texture_payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(PAYLOAD_SIZE)
	var mip_dims := [[64, 4], [32, 2], [16, 1], [8, 1], [4, 1], [2, 1], [1, 1]]
	var mip_base := 0
	for mip in range(mip_dims.size()):
		var w: int = mip_dims[mip][0]
		var h: int = mip_dims[mip][1]
		for y in range(h):
			for x in range(w):
				var pixel_offset := mip_base + y * ROW_PITCH + x * 4
				if mip == 0:
					payload[pixel_offset] = (x * 4) % 256
					payload[pixel_offset + 1] = (y * 63) % 256
					payload[pixel_offset + 2] = 180
				elif mip == 1:
					payload[pixel_offset] = 220
					payload[pixel_offset + 1] = (x * 8) % 256
					payload[pixel_offset + 2] = (y * 100) % 256
				else:
					payload[pixel_offset] = (100 + mip * 13 + x) % 256
					payload[pixel_offset + 1] = (50 + mip * 29 + y * 3) % 256
					payload[pixel_offset + 2] = (mip * 40) % 256
				payload[pixel_offset + 3] = 255
		mip_base += ROW_PITCH * h
	return payload

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	if failures == 0:
		print("[Thread Affinity] ALL TESTS PASSED.")
		quit(0)
	else:
		printerr("[Thread Affinity] COMPLETED WITH ", failures, " FAILURES.")
		quit(1)
