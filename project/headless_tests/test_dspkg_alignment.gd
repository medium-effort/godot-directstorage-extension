extends SceneTree

const TEXTURE_WIDTH := 64
const TEXTURE_HEIGHT := 4
const MIP_COUNT := 2
const ROW_PITCH := 256
const BASE_MIP_BYTES := ROW_PITCH * TEXTURE_HEIGHT
const MIP1_WIDTH := TEXTURE_WIDTH / 2
const MIP1_HEIGHT := TEXTURE_HEIGHT / 2
const MIP1_OFFSET := BASE_MIP_BYTES
const PAYLOAD_SIZE := BASE_MIP_BYTES + ROW_PITCH * MIP1_HEIGHT

var failures := 0

# Archive resolution: the loader bypasses res://textures.dspkg whenever the
# "editor" build feature is set (ds_resource_format_loader.cpp:156), which is
# true for every run launched from an editor binary - including all CLI test
# runs. Loading through the archive therefore requires opting in.
const FORCE_ARCHIVE_SETTING := "directstorage/force_archive_resolution"
var _force_archive_present := false
var _force_archive_prev: Variant = null

func _force_archive_resolution() -> void:
	_force_archive_present = ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING)
	_force_archive_prev = ProjectSettings.get_setting(FORCE_ARCHIVE_SETTING) if _force_archive_present else null
	ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, true)

func _restore_archive_resolution() -> void:
	if _force_archive_present:
		ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, _force_archive_prev)
	else:
		ProjectSettings.clear(FORCE_ARCHIVE_SETTING)

func _initialize() -> void:
	print("[DSPkg Alignment] Starting multi-mip DSPkg 512-byte alignment test")
	_force_archive_resolution()
	if not ClassDB.class_exists("DSPkg") or not ClassDB.class_exists("DSTex") or not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest"):
		_fail("Required classes (DSPkg, DSTex, DSContext, DSRequest) are not registered.")
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

	_test_dspkg_multi_mip_alignment_and_streaming(ctx, rd)
	_finish()

func _test_dspkg_multi_mip_alignment_and_streaming(ctx: Object, rd: RenderingDevice) -> void:
	print("[DSPkg Alignment] Creating multi-mip texture payload...")
	var payload := _make_texture_payload()

	var dstex := DSTex.new()
	var dstex_path := "user://test_dspkg_alignment_source.dstex"
	var write_err: Error = dstex.write_to_file(dstex_path, payload, TEXTURE_WIDTH, TEXTURE_HEIGHT, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	_check(write_err == OK, "DSTex write_to_file succeeded: " + error_string(write_err))
	if write_err != OK:
		return

	var dstex_info: Dictionary = dstex.read_from_file(dstex_path)
	_check(not dstex_info.is_empty(), "DSTex read_from_file succeeded")
	_check(int(dstex_info.get("header", {}).get("version", 0)) == 2, "DSTex version is 2")

	print("[DSPkg Alignment] Packaging into .dspkg archive...")
	var dspkg_path := "user://test_dspkg_alignment_archive.dspkg"
	var dspkg := DSPkg.new()
	var create_err: Error = dspkg.create(dspkg_path)
	_check(create_err == OK, "DSPkg create succeeded: " + error_string(create_err))

	var add_err: Error = dspkg.add_texture("res://textures/test_alignment_tex.dstex", dstex_path)
	_check(add_err == OK, "DSPkg add_texture succeeded: " + error_string(add_err))

	var fin_err: Error = dspkg.finalize()
	_check(fin_err == OK, "DSPkg finalize succeeded: " + error_string(fin_err))
	if fin_err != OK:
		return

	print("[DSPkg Alignment] Verifying archive metadata and 512-byte mip alignment...")
	var reader := DSPkg.new()
	var open_err: Error = reader.open(dspkg_path)
	_check(open_err == OK, "DSPkg open archive succeeded: " + error_string(open_err))
	if open_err != OK:
		return

	_check(reader.has_texture("res://textures/test_alignment_tex.dstex"), "Archive contains expected texture path")
	var entry_info: Dictionary = reader.get_entry_info("res://textures/test_alignment_tex.dstex")
	_check(not entry_info.is_empty(), "get_entry_info returned valid dictionary")

	var read_entry: Dictionary = reader.read_texture("res://textures/test_alignment_tex.dstex")
	_check(not read_entry.is_empty() and read_entry.has("compressed_data"), "read_texture returned compressed payload")
	_check(read_entry.has("mip_data") and read_entry["mip_data"].size() == MIP_COUNT, "read_texture populated mip_data array with all mips")

	var mip_offsets: PackedInt64Array = entry_info.get("mip_offsets", PackedInt64Array())
	var mip_sizes: PackedInt32Array = entry_info.get("mip_sizes", PackedInt32Array())
	_check(mip_offsets.size() == MIP_COUNT, "entry_info has 2 mip offsets: count=" + str(mip_offsets.size()))
	_check(mip_sizes.size() == MIP_COUNT, "entry_info has 2 mip sizes: count=" + str(mip_sizes.size()))

	for i in range(mip_offsets.size()):
		var off := mip_offsets[i]
		var is_aligned := (off % 512) == 0
		_check(is_aligned, "Mip " + str(i) + " offset " + str(off) + " is aligned to 512 bytes (offset % 512 == 0)")

	if mip_offsets.size() >= 2:
		var mip0_end := mip_offsets[0] + mip_sizes[0]
		_check(mip_offsets[1] >= mip0_end, "Mip 1 offset (" + str(mip_offsets[1]) + ") is >= Mip 0 end (" + str(mip0_end) + ")")
		_check(mip_offsets[1] % 512 == 0, "Mip 1 offset is aligned to 512 bytes")

	print("[DSPkg Alignment] Testing DirectStorage GPU texture streaming from .dspkg...")
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

	var request := DSRequest.new()
	request.file_path = dspkg_path
	request.offset = mip_offsets[0]
	var total_span: int = (mip_offsets[1] + mip_sizes[1]) - mip_offsets[0]
	request.size = total_span
	request.uncompressed_size = int(entry_info.get("original_size", 0))
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
	request.destination_resource = texture_rid
	request.first_subresource = 0
	request.subresource_count = MIP_COUNT
	request.texture_format = rd.get_driver_resource(RenderingDevice.DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture_rid, 0)

	var enqueued: bool = ctx.enqueue_request(request)
	_check(enqueued, "enqueue_request accepted GPU texture request from .dspkg: " + request.error_message)
	if not enqueued:
		rd.free_rid(texture_rid)
		return

	ctx.flush_batch()
	var completed: bool = ctx.wait_for_completion(5000)
	_check(completed and request.status == DSRequest.STATUS_COMPLETED, "GPU texture request from .dspkg completed: status=" + str(request.status))

	if completed and request.status == DSRequest.STATUS_COMPLETED:
		var mip0_data: PackedByteArray = rd.texture_get_data(texture_rid, 0)
		_check(mip0_data.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4, "mip 0 readback size is valid: " + str(mip0_data.size()))
		var mip0_match := true
		for y in range(TEXTURE_HEIGHT):
			for x in range(TEXTURE_WIDTH):
				var read_off := (y * TEXTURE_WIDTH + x) * 4
				var src_off := y * ROW_PITCH + x * 4
				for c in range(4):
					if mip0_data[read_off + c] != payload[src_off + c]:
						mip0_match = false
						break
		_check(mip0_match, "mip 0 texels streamed from .dspkg match source payload byte-for-byte")

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
			_check(mip1_match, "mip 1 texels streamed from .dspkg match source payload byte-for-byte")

	rd.free_rid(texture_rid)

	print("[DSPkg Alignment] Testing ResourceLoader integration with res://textures.dspkg...")
	var project_dspkg_path := "res://textures.dspkg"
	var copy_err := DirAccess.copy_absolute(dspkg_path, project_dspkg_path)
	if copy_err == OK:
		var loaded_tex = ResourceLoader.load("res://textures/test_alignment_tex.dstex")
		_check(loaded_tex != null, "ResourceLoader loaded texture from res://textures.dspkg")
		if loaded_tex != null:
			var mip0: PackedByteArray
			if loaded_tex is Texture2DRD:
				var rd_rid: RID = (loaded_tex as Texture2DRD).texture_rd_rid
				_check(rd_rid.is_valid(), "Texture2DRD texture_rd_rid is valid")
				if rd_rid.is_valid():
					mip0 = rd.texture_get_data(rd_rid, 0)
			elif loaded_tex is ImageTexture or loaded_tex.has_method("get_image"):
				var img: Image = loaded_tex.get_image()
				if img != null:
					mip0 = img.get_data()
			elif loaded_tex.has_method("get_rid"):
				var r: RID = loaded_tex.get_rid()
				if r.is_valid():
					mip0 = rd.texture_get_data(r, 0)

			_check(mip0.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4, "ResourceLoader texture mip 0 readback size valid: " + str(mip0.size()))
			var match_ok := mip0.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4
			if match_ok:
				for y in range(TEXTURE_HEIGHT):
					for x in range(TEXTURE_WIDTH):
						var read_off := (y * TEXTURE_WIDTH + x) * 4
						var src_off := y * ROW_PITCH + x * 4
						for c in range(4):
							if mip0[read_off + c] != payload[src_off + c]:
								match_ok = false
								break
			_check(match_ok, "ResourceLoader loaded texture texels match source payload byte-for-byte")
			if loaded_tex is Texture2DRD and (loaded_tex as Texture2DRD).texture_rd_rid.is_valid():
				rd.free_rid((loaded_tex as Texture2DRD).texture_rd_rid)
	else:
		printerr("[WARN] Could not copy dspkg to res://textures.dspkg: ", copy_err)

	# Clean up files. DirectStorage keeps the archive open in its IDStorageFile
	# cache, so the context must be shut down before deleting it - otherwise
	# Windows refuses the delete and res://textures.dspkg survives into the next
	# run, where DSPkg.finalize() then fails with "Could not open file for
	# writing". remove_absolute() also needs an OS path, not the res:// form.
	_shutdown_ds()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(project_dspkg_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(dstex_path))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(dspkg_path))

func _shutdown_ds() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx != null and ctx.is_initialized():
		ctx.shutdown_ds()

func _make_texture_payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(PAYLOAD_SIZE)
	for y in range(TEXTURE_HEIGHT):
		for x in range(TEXTURE_WIDTH):
			var pixel_offset := y * ROW_PITCH + x * 4
			payload[pixel_offset] = (x * 4) % 256
			payload[pixel_offset + 1] = (y * 63) % 256
			payload[pixel_offset + 2] = 180
			payload[pixel_offset + 3] = 255
	for y in range(MIP1_HEIGHT):
		for x in range(MIP1_WIDTH):
			var pixel_offset := MIP1_OFFSET + y * ROW_PITCH + x * 4
			payload[pixel_offset] = 220
			payload[pixel_offset + 1] = (x * 8) % 256
			payload[pixel_offset + 2] = (y * 100) % 256
			payload[pixel_offset + 3] = 255
	return payload

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] " + message)
	else:
		failures += 1
		printerr("[FAIL] " + message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] " + message)

func _finish() -> void:
	_restore_archive_resolution()
	if failures == 0:
		print("[DSPkg Alignment] ALL TESTS PASSED!")
		quit(0)
	else:
		printerr("[DSPkg Alignment] COMPLETED WITH " + str(failures) + " FAILURE(S)")
		quit(1)
