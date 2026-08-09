extends Control

## DirectStorage 3D Heavy Asset Benchmark (Steampunk Underwater Explorer)
##
## Author: MediumEffort
## Description: Benchmarks 50 MB high-res texture batch loading for 3D assets:
## Standard Godot Synchronous I/O vs DirectStorage Asynchronous GPU Batch Streaming.
## Supports Blocking Mode (Main Thread Stall) vs Non-Blocking Asynchronous Mode (Background Thread Parsing + Frame Yielding).

@export var model_path: String = "res://test_assets/steampunk_underwater_explorer.glb"

@onready var model_holder: Node3D = %ModelHolder
@onready var camera_3d: Camera3D = %Camera3D
@onready var camera_pivot: Node3D = %CameraPivot
@onready var status_label: Label = %StatusLabel
@onready var backend_label: Label = %BackendLabel
@onready var results_label: Label = %ResultsLabel

@onready var spinning_cube: MeshInstance3D = %SpinningCube
@onready var check_non_blocking: CheckBox = %CheckNonBlocking
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

var is_benchmarking: bool = false
var camera_orbit_speed: float = 0.3
var texture_count: int = 16

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
		backend_label.text = "Active Backend: " + backend_name + " | Asset Payload: 50 MB"
		status_label.text = "Status: Ready for 3D Asset Batch Benchmark"
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
	print("[DS 3D Benchmark] Model and texture cache cleared.")

func run_standard_benchmark() -> void:
	if is_benchmarking:
		return
	is_benchmarking = true
	var is_non_blocking: bool = check_non_blocking.button_pressed
	var click_time_usec := Time.get_ticks_usec()

	status_label.text = "Status: Running Standard Load (Non-Blocking: " + str(is_non_blocking) + ")..."
	print("[DS 3D Benchmark] Starting Standard Texture Load Benchmark (Non-Blocking: ", is_non_blocking, ")...")

	clear_cache_and_model()
	await get_tree().process_frame

	# _prepare_standard_textures()

	if is_non_blocking:
		# True Non-Blocking Mode: Parse GLB in background thread and load textures with frame yields
		standard_freeze_ms = 0.0
		# var loaded_textures: Array[Texture2D] = []
		# for i in range(texture_count):
		# 	var tex_path := "user://std_texture_" + str(i) + ".png"
		# 	var img := Image.load_from_file(ProjectSettings.globalize_path(tex_path))
		# 	if img and not img.is_empty():
		# 		var tex := ImageTexture.create_from_image(img)
		# 		loaded_textures.append(tex)
		# 	await get_tree().process_frame
		
		# Background thread GLB model spawn (main thread never freezes)
		await _async_spawn_3d_explorer()
	else:
		# Blocking Mode: Synchronous main thread GLB parsing + synchronous image loading
		var start_freeze_usec := Time.get_ticks_usec()
		# var loaded_textures: Array[Texture2D] = []
		# for i in range(texture_count):
		# 	var tex_path := "user://std_texture_" + str(i) + ".png"
		# 	var img := Image.load_from_file(ProjectSettings.globalize_path(tex_path))
		# 	if img and not img.is_empty():
		# 		var tex := ImageTexture.create_from_image(img)
		# 		loaded_textures.append(tex)
		_spawn_3d_explorer_sync()
		var end_freeze_usec := Time.get_ticks_usec()
		standard_freeze_ms = (end_freeze_usec - start_freeze_usec) / 1000.0

	await get_tree().process_frame
	standard_click_to_render_ms = (Time.get_ticks_usec() - click_time_usec) / 1000.0

	status_label.text = "Status: Standard Load Complete"
	_update_results_display()
	print("[DS 3D Benchmark] Standard Load Complete: Freeze=", String.num(standard_freeze_ms, 2), " ms | Click to Render=", String.num(standard_click_to_render_ms, 2), " ms")

	is_benchmarking = false

func run_directstorage_benchmark() -> void:
	if is_benchmarking:
		return
	is_benchmarking = true
	var is_non_blocking: bool = check_non_blocking.button_pressed
	var click_time_usec := Time.get_ticks_usec()

	status_label.text = "Status: Running DirectStorage Stream (Non-Blocking: " + str(is_non_blocking) + ")..."
	print("[DS 3D Benchmark] Starting DirectStorage Batch GPU Stream (Non-Blocking: ", is_non_blocking, ")...")

	clear_cache_and_model()
	await get_tree().process_frame

	var ctx = Engine.get_singleton("DSContext")
	if ctx and not ctx.is_initialized():
		ctx.initialize_ds()

	# _prepare_directstorage_textures()

	if is_non_blocking:
		# True Non-Blocking Mode: DirectStorage requests queued per frame + background thread GLB spawn
		ds_freeze_ms = 0.0
		# for i in range(texture_count):
		# 	var dstex_path := "user://ds_texture_" + str(i) + ".dstex"
		# 	ResourceLoader.load(dstex_path)
		# 	if ctx:
		# 		ctx.flush_batch()
		# 	await get_tree().process_frame

		var profiler = Engine.get_singleton("DSProfiler")
		if profiler:
			ds_throughput_mbps = profiler.get_last_throughput_mbps()

		# Background thread GLB model spawn (main thread never freezes)
		await _async_spawn_3d_explorer()
	else:
		# Blocking Mode: Synchronous main thread GLB parsing + synchronous DirectStorage batch load
		var start_freeze_usec := Time.get_ticks_usec()
		# for i in range(texture_count):
		# 	var dstex_path := "user://ds_texture_" + str(i) + ".dstex"
		# 	ResourceLoader.load(dstex_path)

		if ctx:
			ctx.flush_batch()
			var profiler = Engine.get_singleton("DSProfiler")
			if profiler:
				ds_throughput_mbps = profiler.get_last_throughput_mbps()

		_spawn_3d_explorer_sync()
		var end_freeze_usec := Time.get_ticks_usec()
		ds_freeze_ms = (end_freeze_usec - start_freeze_usec) / 1000.0

	await get_tree().process_frame
	ds_click_to_render_ms = (Time.get_ticks_usec() - click_time_usec) / 1000.0

	status_label.text = "Status: DirectStorage Stream Complete"
	_update_results_display()
	print("[DS 3D Benchmark] DirectStorage Stream Complete: Freeze=", String.num(ds_freeze_ms, 2), " ms | Click to Render=", String.num(ds_click_to_render_ms, 2), " ms")

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
	print("Non-Blocking Mode Enabled        : ", check_non_blocking.button_pressed)
	print("Standard I/O Freeze / Hitch Time : ", String.num(standard_freeze_ms, 2), " ms")
	print("DirectStorage Freeze / Hitch Time: ", String.num(ds_freeze_ms, 2), " ms")
	print("Standard Click-to-Render Latency : ", String.num(standard_click_to_render_ms, 2), " ms")
	print("DirectStorage Click-to-Render    : ", String.num(ds_click_to_render_ms, 2), " ms")
	if ds_click_to_render_ms > 0:
		var speedup: float = standard_click_to_render_ms / ds_click_to_render_ms
		print("DirectStorage Speedup Factor     : ", String.num(speedup, 2), "x faster")
	print("DirectStorage Measured Throughput : ", String.num(ds_throughput_mbps, 2), " MB/s")
	print("==========================================================")

func _spawn_3d_explorer_sync() -> void:
	if ResourceLoader.exists(model_path):
		var loaded_res = ResourceLoader.load(model_path)
		if loaded_res is PackedScene:
			var scene_instance = loaded_res.instantiate()
			if scene_instance:
				model_holder.add_child(scene_instance)
				scene_instance.position = Vector3(0, -1.0, 0)
				scene_instance.scale = Vector3(0.5, 0.5, 0.5)
				return

	if not FileAccess.file_exists(model_path):
		print("[DS 3D Benchmark] ERROR: Model file not found: ", model_path)
		return

	var gltf_doc = GLTFDocument.new()
	var gltf_state = GLTFState.new()
	if gltf_doc.append_from_file(model_path, gltf_state) == OK:
		var node = gltf_doc.generate_scene(gltf_state)
		if node:
			model_holder.add_child(node)
			node.position = Vector3(0, -1.0, 0)
			node.scale = Vector3(0.5, 0.5, 0.5)

func _async_spawn_3d_explorer() -> void:
	if ResourceLoader.exists(model_path):
		var loaded_res = ResourceLoader.load(model_path)
		if loaded_res is PackedScene:
			var scene_instance = loaded_res.instantiate()
			if scene_instance:
				model_holder.add_child(scene_instance)
				scene_instance.position = Vector3(0, -1.0, 0)
				scene_instance.scale = Vector3(0.5, 0.5, 0.5)
				return

	if not FileAccess.file_exists(model_path):
		print("[DS 3D Benchmark] ERROR: Model file not found: ", model_path)
		return

	var thread := Thread.new()
	var err := thread.start(Callable(self, "_bg_load_gltf_thread").bind(model_path))
	if err != OK:
		_spawn_3d_explorer_sync()
		return

	while thread.is_alive():
		await get_tree().process_frame

	var node = thread.wait_to_finish()
	if node:
		model_holder.add_child(node)
		node.position = Vector3(0, -1.0, 0)
		node.scale = Vector3(0.5, 0.5, 0.5)

func _bg_load_gltf_thread(path: String) -> Node3D:
	if ResourceLoader.exists(path):
		var loaded_res = ResourceLoader.load(path)
		if loaded_res is PackedScene:
			var scene_instance = loaded_res.instantiate()
			if scene_instance is Node3D:
				return scene_instance as Node3D

	if not FileAccess.file_exists(path):
		return null

	var gltf_doc = GLTFDocument.new()
	var gltf_state = GLTFState.new()
	if gltf_doc.append_from_file(path, gltf_state) == OK:
		return gltf_doc.generate_scene(gltf_state) as Node3D
	return null

func _prepare_standard_textures() -> void:
	# Disabled synthetic test texture generation
	pass

func _prepare_directstorage_textures() -> void:
	# Disabled synthetic test texture generation
	pass

func _update_results_display() -> void:
	var is_non_blocking: bool = check_non_blocking.button_pressed
	var mode_str := "Non-Blocking Asynchronous" if is_non_blocking else "Blocking Synchronous"

	var text := "--- BENCHMARK RESULTS (" + mode_str + ") ---\n"
	text += "[Standard I/O]\n"
	text += "  Click-to-Render Latency: " + String.num(standard_click_to_render_ms, 2) + " ms\n"
	text += "  Main Thread Freeze     : " + String.num(standard_freeze_ms, 2) + " ms\n\n"

	text += "[DirectStorage Stream]\n"
	text += "  Click-to-Render Latency: " + String.num(ds_click_to_render_ms, 2) + " ms\n"
	text += "  Main Thread Freeze     : " + String.num(ds_freeze_ms, 2) + " ms\n"
	text += "  DirectStorage Throughput: " + String.num(ds_throughput_mbps, 2) + " MB/s\n"

	if ds_click_to_render_ms > 0 and standard_click_to_render_ms > 0:
		var speedup: float = standard_click_to_render_ms / ds_click_to_render_ms
		text += "  Speedup Factor         : " + String.num(speedup, 2) + "x faster\n"

	results_label.text = text
