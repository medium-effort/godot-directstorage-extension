extends Control

## DirectStorage 3D Heavy Asset Benchmark (Steampunk Underwater Explorer)
##
## Author: MediumEffort
## Description: Benchmarks loading the raw GLB model with standard file I/O versus
## DirectStorage file reads. Texture-only streaming is covered by the separate
## test_mcp_directstorage_interactive scene.

@export var model_path: String = "res://test_assets/steampunk_underwater_explorer.glb"

@onready var model_holder: Node3D = %ModelHolder
@onready var camera_3d: Camera3D = %Camera3D
@onready var camera_pivot: Node3D = %CameraPivot
@onready var status_label: Label = %StatusLabel
@onready var backend_label: Label = %BackendLabel
@onready var results_label: Label = %ResultsLabel

@onready var spinning_cube: MeshInstance3D = %SpinningCube
@onready var btn_standard: Button = %BtnStandard
@onready var btn_directstorage: Button = %BtnDirectStorage
@onready var btn_comparison: Button = %BtnComparison
@onready var btn_clear_cache: Button = %BtnClearCache

var standard_load_time_ms: float = 0.0
var standard_click_to_render_ms: float = 0.0
var standard_freeze_ms: float = 0.0

var ds_load_time_ms: float = 0.0
var ds_click_to_render_ms: float = 0.0
var ds_freeze_ms: float = 0.0
var ds_throughput_mbps: float = 0.0
var directstorage_model_bytes: PackedByteArray

var is_benchmarking: bool = false
var camera_orbit_speed: float = 0.3

func _ready() -> void:
	btn_standard.pressed.connect(run_standard_benchmark)
	btn_directstorage.pressed.connect(run_directstorage_benchmark)
	btn_comparison.pressed.connect(run_comparison_benchmark)
	btn_clear_cache.pressed.connect(clear_cache_and_model)
	
	update_backend_status()

	if DisplayServer.get_name() == "headless":
		run_comparison_benchmark()

func _process(delta: float) -> void:
	if camera_pivot:
		camera_pivot.rotate_y(camera_orbit_speed * delta)
	if spinning_cube:
		spinning_cube.rotate_x(1.5 * delta)
		spinning_cube.rotate_y(2.0 * delta)

func update_backend_status() -> void:
	if not ClassDB.class_exists("DSContext"):
		status_label.text = "Status: ERROR - GDExtension DSContext Missing!"
		backend_label.text = "Active Backend: None"
		return

	var ctx = Engine.get_singleton("DSContext")
	if ctx:
		if not ctx.is_initialized():
			ctx.initialize_ds()
		var backend_name: String = ctx.get_active_backend_name()
		var decompression_info: String = ""
		if backend_name == "Win32DirectStorage" and ctx.has_method("get_gpu_decompression_queue_type"):
			var qtype: String = ctx.get_gpu_decompression_queue_type()
			var opt: bool = ctx.is_gpu_decompression_optimized()
			var mode: String = "GPU Optimized" if opt else "Shader Fallback"
			if qtype != "none":
				mode += " - " + qtype.capitalize() + " Queue"
			decompression_info = " (" + mode + ")"
		backend_label.text = "Active Backend: " + backend_name + decompression_info + " | Asset: Raw GLB"
		status_label.text = "Status: Ready for 3D Model Load Benchmark"
	else:
		status_label.text = "Status: DSContext Singleton Null"

func clear_cache_and_model() -> void:
	for child in model_holder.get_children():
		child.queue_free()

	var cache = Engine.get_singleton("DSTextureCache")
	if cache:
		cache.clear()

	results_label.text = "Model unloaded and VRAM cache cleared."
	status_label.text = "Status: Idle / Cache Cleared"
	print("[DS 3D Benchmark] Model cache cleared.")

func run_standard_benchmark() -> void:
	if is_benchmarking:
		return
	is_benchmarking = true
	var click_time_usec := Time.get_ticks_usec()

	status_label.text = "Status: Running Standard Raw GLB Load..."
	print("[DS 3D Benchmark] Starting Standard Raw GLB Load Benchmark...")

	clear_cache_and_model()
	await get_tree().process_frame

	var start_load_usec := Time.get_ticks_usec()
	_spawn_3d_explorer_sync()
	standard_load_time_ms = (Time.get_ticks_usec() - start_load_usec) / 1000.0
	standard_freeze_ms = standard_load_time_ms

	await get_tree().process_frame
	standard_click_to_render_ms = (Time.get_ticks_usec() - click_time_usec) / 1000.0

	status_label.text = "Status: Standard Load Complete"
	_update_results_display()
	print("[DS 3D Benchmark] Standard Load Complete: Load=", String.num(standard_load_time_ms, 2), " ms | Freeze=", String.num(standard_freeze_ms, 2), " ms | Click to Render=", String.num(standard_click_to_render_ms, 2), " ms")

	is_benchmarking = false

func run_directstorage_benchmark() -> void:
	if is_benchmarking:
		return
	is_benchmarking = true
	var click_time_usec := Time.get_ticks_usec()

	status_label.text = "Status: Running DirectStorage Raw GLB Load..."
	print("[DS 3D Benchmark] Starting DirectStorage Raw GLB Load Benchmark...")

	clear_cache_and_model()
	await get_tree().process_frame

	var ctx = Engine.get_singleton("DSContext")
	if ctx and not ctx.is_initialized():
		ctx.initialize_ds()
	if ctx == null:
		status_label.text = "Status: DirectStorage Context Unavailable"
		is_benchmarking = false
		return
	if ctx and ctx.get_active_backend_name() == "Win32DirectStorage":
		var error_handler = Engine.get_singleton("DSErrorHandler")
		var staging_mb: int = ctx.get_staging_buffer_size_mb()
		if staging_mb < 128 and (error_handler == null or not error_handler.has_method("describe_hresult") or not error_handler.has_method("process_queue_errors")):
			status_label.text = "Status: DirectStorage extension must be rebuilt for CPU fallback"
			push_warning("[DS 3D Benchmark] DirectStorage fallback is unavailable because the loaded GDExtension does not contain the diagnostics-safe request path. Rebuild the extension before testing with a staging buffer below 128 MiB.")
			is_benchmarking = false
			return

	var start_freeze_usec := Time.get_ticks_usec()
	var start_load_usec := Time.get_ticks_usec()
	directstorage_model_bytes = _read_model_with_directstorage(ctx)
	ds_load_time_ms = (Time.get_ticks_usec() - start_load_usec) / 1000.0
	if directstorage_model_bytes.is_empty():
		status_label.text = "Status: DirectStorage GLB Read Failed"
		is_benchmarking = false
		return
	var profiler = Engine.get_singleton("DSProfiler")
	if profiler:
		ds_throughput_mbps = profiler.get_last_throughput_mbps()

	_spawn_3d_explorer_from_bytes(directstorage_model_bytes)
	var end_freeze_usec := Time.get_ticks_usec()
	ds_freeze_ms = (end_freeze_usec - start_freeze_usec) / 1000.0

	await get_tree().process_frame
	ds_click_to_render_ms = (Time.get_ticks_usec() - click_time_usec) / 1000.0

	status_label.text = "Status: DirectStorage Raw GLB Load Complete"
	_update_results_display()
	print("[DS 3D Benchmark] DirectStorage Load Complete: Load=", String.num(ds_load_time_ms, 2), " ms | Freeze=", String.num(ds_freeze_ms, 2), " ms | Click to Render=", String.num(ds_click_to_render_ms, 2), " ms")

	is_benchmarking = false

func run_comparison_benchmark() -> void:
	print("==========================================================")
	print("=== Starting 3D Heavy Asset DirectStorage Comparison Benchmark ===")
	print("==========================================================")

	await run_standard_benchmark()
	await get_tree().create_timer(0.5).timeout
	await run_directstorage_benchmark()

	print("==========================================================")
	print("=== BENCHMARK SUMMARY ===")
	print("Benchmark Mode                   : Blocking raw-model load")
	print("Standard Model Load Time         : ", String.num(standard_load_time_ms, 2), " ms")
	print("DirectStorage Model Load Time    : ", String.num(ds_load_time_ms, 2), " ms")
	print("Standard I/O Freeze / Hitch Time : ", String.num(standard_freeze_ms, 2), " ms")
	print("DirectStorage Freeze / Hitch Time: ", String.num(ds_freeze_ms, 2), " ms")
	print("Standard Click-to-Render Latency : ", String.num(standard_click_to_render_ms, 2), " ms")
	print("DirectStorage Click-to-Render    : ", String.num(ds_click_to_render_ms, 2), " ms")
	if ds_click_to_render_ms > 0:
		var speedup: float = standard_click_to_render_ms / ds_click_to_render_ms
		print("DirectStorage Speedup Factor     : ", String.num(speedup, 2), "x faster")
	print("DirectStorage Measured Throughput : ", String.num(ds_throughput_mbps, 2), " MB/s")
	print("==========================================================")

	if DisplayServer.get_name() == "headless":
		get_tree().quit()

func _spawn_3d_explorer_sync() -> void:
	if not FileAccess.file_exists(model_path):
		print("[DS 3D Benchmark] ERROR: Model file not found: ", model_path)
		return

	var gltf_doc := GLTFDocument.new()
	var gltf_state := GLTFState.new()
	var parse_error := gltf_doc.append_from_file(model_path, gltf_state)
	if parse_error != OK:
		print("[DS 3D Benchmark] ERROR: Standard GLB parse failed: ", parse_error)
		return

	var node := gltf_doc.generate_scene(gltf_state) as Node3D
	if node:
		model_holder.add_child(node)
		node.position = Vector3(0, -1.0, 0)
		node.scale = Vector3(0.5, 0.5, 0.5)

func _spawn_3d_explorer_from_bytes(bytes: PackedByteArray) -> void:
	var gltf_doc := GLTFDocument.new()
	var gltf_state := GLTFState.new()
	var parse_error := gltf_doc.append_from_buffer(bytes, model_path.get_base_dir(), gltf_state)
	if parse_error != OK:
		print("[DS 3D Benchmark] ERROR: DirectStorage GLB parse failed: ", parse_error)
		return

	var node := gltf_doc.generate_scene(gltf_state) as Node3D
	if node:
		model_holder.add_child(node)
		node.position = Vector3(0, -1.0, 0)
		node.scale = Vector3(0.5, 0.5, 0.5)

func _read_model_with_directstorage(ctx) -> PackedByteArray:
	if not FileAccess.file_exists(model_path):
		print("[DS 3D Benchmark] ERROR: Model file not found: ", model_path)
		return PackedByteArray()

	var file := FileAccess.open(model_path, FileAccess.READ)
	if file == null:
		print("[DS 3D Benchmark] ERROR: Could not open model file: ", model_path)
		return PackedByteArray()

	var file_size := file.get_length()
	file = null
	if file_size <= 0 or file_size > 0xffffffff:
		print("[DS 3D Benchmark] ERROR: Invalid GLB size: ", file_size)
		return PackedByteArray()

	if not ClassDB.class_exists("DSRequest"):
		print("[DS 3D Benchmark] ERROR: DSRequest class is unavailable.")
		return PackedByteArray()

	var request = ClassDB.instantiate("DSRequest")
	request.set("file_path", model_path)
	request.set("offset", 0)
	request.set("size", file_size)
	request.set("uncompressed_size", file_size)
	request.set("cancellation_tag", Time.get_ticks_usec())
	request.set("compression_format", 0)

	if not ctx.enqueue_request(request):
		print("[DS 3D Benchmark] ERROR: DirectStorage rejected GLB request: ", request.get("error_message"))
		return PackedByteArray()

	ctx.flush_batch()
	if request.get("status") != 2:
		print("[DS 3D Benchmark] ERROR: DirectStorage GLB request failed: ", request.get("error_message"))
		return PackedByteArray()

	return request.get("destination_buffer")

func _update_results_display() -> void:
	var text := "--- BENCHMARK RESULTS (Blocking Raw GLB Load) ---\n"
	text += "[Standard I/O]\n"
	text += "  Model Load Time        : " + String.num(standard_load_time_ms, 2) + " ms\n"
	text += "  Click-to-Render Latency: " + String.num(standard_click_to_render_ms, 2) + " ms\n"
	text += "  Main Thread Freeze     : " + String.num(standard_freeze_ms, 2) + " ms\n\n"

	text += "[DirectStorage I/O]\n"
	text += "  Model Load Time        : " + String.num(ds_load_time_ms, 2) + " ms\n"
	text += "  Click-to-Render Latency: " + String.num(ds_click_to_render_ms, 2) + " ms\n"
	text += "  Main Thread Freeze     : " + String.num(ds_freeze_ms, 2) + " ms\n"
	text += "  DirectStorage Throughput: " + String.num(ds_throughput_mbps, 2) + " MB/s\n"

	if ds_click_to_render_ms > 0 and standard_click_to_render_ms > 0:
		var speedup: float = standard_click_to_render_ms / ds_click_to_render_ms
		text += "  Speedup Factor         : " + String.num(speedup, 2) + "x faster\n"

	results_label.text = text
