extends Control

## Wave 0 GPU texture spike for the D3D12 DirectStorage resource-state path.

const TEXTURE_WIDTH := 64
const TEXTURE_HEIGHT := 4
const MIP_COUNT := 2
const ROW_PITCH := 256
const BASE_MIP_BYTES := ROW_PITCH * TEXTURE_HEIGHT
const MIP1_WIDTH := TEXTURE_WIDTH / 2
const MIP1_HEIGHT := TEXTURE_HEIGHT / 2
const MIP1_OFFSET := BASE_MIP_BYTES
const PAYLOAD_SIZE := BASE_MIP_BYTES + ROW_PITCH * MIP1_HEIGHT

var rd: RenderingDevice
var texture_rid: RID
var texture_rd: Texture2DRD
var status_label: Label

func _ready() -> void:
	_build_ui()
	call_deferred("_run_spike")

func _build_ui() -> void:
	var panel := VBoxContainer.new()
	panel.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT, Control.PRESET_MODE_MINSIZE, 24)
	add_child(panel)

	var title := Label.new()
	title.text = "DirectStorage GPU Texture Spike"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	panel.add_child(title)

	status_label = Label.new()
	status_label.text = "Creating empty Godot texture..."
	status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	panel.add_child(status_label)

func _run_spike() -> void:
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest") or not ClassDB.class_exists("GDeflateCompressor"):
		_fail("DSContext, DSRequest, or GDeflateCompressor is not registered.")
		return

	rd = RenderingServer.get_rendering_device()
	if rd == null:
		_fail("RenderingDevice is unavailable; run with --rendering-driver d3d12.")
		return

	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = TEXTURE_WIDTH
	texture_format.height = TEXTURE_HEIGHT
	texture_format.mipmaps = MIP_COUNT
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	texture_rid = rd.texture_create(texture_format, RDTextureView.new(), [])
	if not texture_rid.is_valid():
		_fail("texture_create() returned an invalid RID.")
		return

	texture_rd = Texture2DRD.new()
	texture_rd.texture_rd_rid = texture_rid
	var texture_rect := TextureRect.new()
	texture_rect.texture = texture_rd
	texture_rect.custom_minimum_size = Vector2(TEXTURE_WIDTH * 4, TEXTURE_HEIGHT * 4)
	texture_rect.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	texture_rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	texture_rect.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	texture_rect.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	get_child(0).add_child(texture_rect)

	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		_fail("An initialized Win32 DirectStorage D3D12 backend is required.")
		return

	var payload := _make_texture_payload()
	var compressor := GDeflateCompressor.new()
	var compressed: PackedByteArray = compressor.compress(payload, 1)
	if compressed.is_empty():
		_fail("GDeflate compression produced no payload.")
		return

	var payload_path := "user://test_direct_gpu_texture.gdeflate"
	var payload_file := FileAccess.open(payload_path, FileAccess.WRITE)
	if payload_file == null:
		_fail("Could not create the DirectStorage source payload.")
		return
	payload_file.store_buffer(compressed)
	payload_file.close()

	var request := DSRequest.new()
	request.file_path = payload_path
	request.size = compressed.size()
	request.uncompressed_size = payload.size()
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
	request.destination_resource = texture_rid
	request.first_subresource = 0
	request.subresource_count = MIP_COUNT

	status_label.text = "Submitting MULTIPLE_SUBRESOURCES_RANGE..."
	var enqueued: bool = ctx.enqueue_request(request)
	if not enqueued:
		_fail("enqueue_request() failed: " + request.error_message)
		return

	ctx.flush_batch()
	var fence_complete: bool = ctx.wait_for_completion(5000)
	var succeeded := fence_complete and request.status == DSRequest.STATUS_COMPLETED
	if succeeded:
		status_label.text = "PASS: fence signaled; texture written. Inspect D3D12 debug output for state warnings."
		print("[GPU Texture Spike] PASS status=", request.status, " fence=", ctx.get_completion_fence(), " backend=", ctx.get_active_backend_name())
	else:
		_fail("GPU texture request failed: " + request.error_message)

func _make_texture_payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(PAYLOAD_SIZE)

	# D3D12 texture destinations use a 256-byte row pitch in this test payload.
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

func _fail(message: String) -> void:
	status_label.text = "FAIL: " + message
	printerr("[GPU Texture Spike] ", message)
