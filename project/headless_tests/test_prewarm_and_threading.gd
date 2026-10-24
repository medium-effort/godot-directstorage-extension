extends SceneTree

const TEXTURE_WIDTH := 64
const TEXTURE_HEIGHT := 64
const MIP_COUNT := 1
const ROW_PITCH := 256
const PAYLOAD_SIZE := ROW_PITCH * TEXTURE_HEIGHT

var failures := 0
var cleanup_paths: Array[String] = []

# Archive resolution: the loader bypasses res://textures.dspkg whenever the
# "editor" build feature is set (ds_resource_format_loader.cpp:156), which is
# true for every run launched from an editor binary - including all CLI test
# runs. Staging an archive therefore requires opting in, and clearing it again.
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

func _shutdown_ds() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx != null and ctx.is_initialized():
		ctx.shutdown_ds()

func _initialize() -> void:
	print("\n=================================================================")
	print("[Test Prewarm & Threading] Starting DirectStorage Verification")
	print("=================================================================")
	_force_archive_resolution()

	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSTex") or not ClassDB.class_exists("DSPkg"):
		_fail("Required classes (DSContext, DSTex, DSPkg) are not registered.")
		_finish()
		return

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail("RenderingDevice is unavailable; ensure running with --rendering-driver d3d12.")
		_finish()
		return

	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.initialize_ds() or not ctx.is_d3d12_backend():
		_fail("DirectStorage Win32 D3D12 backend is not available.")
		_finish()
		return

	print("[Test] DirectStorage D3D12 backend active. Starting test suite...")
	await _run_tests(ctx, rd)
	_finish()

func _run_tests(ctx: Object, rd: RenderingDevice) -> void:
	var payload := _make_texture_payload()
	var dstex_path := "user://test_prewarm_tex.dstex"
	var dspkg_path := "res://textures.dspkg"
	var scene_path := "res://test_prewarm_scene.tscn"
	cleanup_paths = [dstex_path, dspkg_path, scene_path]

	# -------------------------------------------------------------
	# 0. Setup Fixtures
	# -------------------------------------------------------------
	print("\n--- Phase 0: Creating Fixtures ---")
	var dstex := DSTex.new()
	var write_err: Error = dstex.write_to_file(dstex_path, payload, TEXTURE_WIDTH, TEXTURE_HEIGHT, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	_check(write_err == OK, "DSTex write_to_file succeeded: " + error_string(write_err))
	if write_err != OK:
		return

	var dspkg := DSPkg.new()
	var create_err: Error = dspkg.create(ProjectSettings.globalize_path(dspkg_path))
	_check(create_err == OK, "DSPkg create succeeded: " + error_string(create_err))
	if create_err != OK:
		return

	var add_err: Error = dspkg.add_texture("res://textures/test_prewarm_tex.dstex", dstex_path)
	_check(add_err == OK, "DSPkg add_texture succeeded: " + error_string(add_err))
	var fin_err: Error = dspkg.finalize()
	_check(fin_err == OK, "DSPkg finalize succeeded: " + error_string(fin_err))
	if fin_err != OK:
		return

	# Write dummy scene referencing the texture
	var scene_text := """[gd_scene load_steps=3 format=3]

[ext_resource type="Texture2D" path="res://textures/test_prewarm_tex.dstex" id="1_tex"]

[sub_resource type="StandardMaterial3D" id="StandardMaterial3D_1"]
albedo_texture = ExtResource("1_tex")

[node name="TestScene" type="Node3D"]

[node name="MeshInstance" type="MeshInstance3D" parent="."]
material_override = SubResource("StandardMaterial3D_1")
"""
	var scene_file := FileAccess.open(scene_path, FileAccess.WRITE)
	_check(scene_file != null, "Created dummy scene file")
	if scene_file != null:
		scene_file.store_string(scene_text)
		scene_file.close()

	# -------------------------------------------------------------
	# 1. Phase 1: Synchronous Main-Thread Load (Hypothesis 1)
	# -------------------------------------------------------------
	print("\n--- Phase 1: Main-Thread Synchronous Load (Hypothesis 1) ---")
	print("[Test] Calling ResourceLoader.load() synchronously from main thread...")
	var tex_sync = ResourceLoader.load("res://textures/test_prewarm_tex.dstex", "Texture2D", ResourceLoader.CACHE_MODE_IGNORE)
	_check(tex_sync != null, "Synchronous load returned a valid texture")
	if tex_sync != null:
		var tex_class := tex_sync.get_class()
		print("[Test] Synchronous texture class: ", tex_class)
		_check(tex_sync is Texture2DRD, "Hypothesis 1 VERIFIED: Synchronous main-thread load produces Texture2DRD (GPU DirectStorage path)")
		_check(not (tex_sync is ImageTexture), "Hypothesis 1 VERIFIED: Synchronous main-thread load is NOT ImageTexture")
		if tex_sync is Texture2DRD:
			var rd_rid: RID = (tex_sync as Texture2DRD).texture_rd_rid
			_check(rd_rid.is_valid(), "Texture2DRD has valid RenderingDevice RID")
			var mip0: PackedByteArray = rd.texture_get_data(rd_rid, 0)
			_check(mip0.size() >= TEXTURE_WIDTH * TEXTURE_HEIGHT * 4, "Texture2DRD mip 0 readback size valid")
			var match_ok := true
			for y in range(TEXTURE_HEIGHT):
				for x in range(TEXTURE_WIDTH):
					var read_off := (y * TEXTURE_WIDTH + x) * 4
					var src_off := y * ROW_PITCH + x * 4
					for c in range(4):
						if mip0[read_off + c] != payload[src_off + c]:
							match_ok = false
							break
			_check(match_ok, "Texture2DRD texels match source payload byte-for-byte")

	# -------------------------------------------------------------
	# 2. Phase 2: Worker-Thread Background Load (Hypothesis 2)
	# -------------------------------------------------------------
	print("\n--- Phase 2: Worker-Thread Background Load (Hypothesis 2) ---")
	print("[Test] Calling ResourceLoader.load_threaded_request()...")
	var req_err := ResourceLoader.load_threaded_request("res://textures/test_prewarm_tex.dstex", "Texture2D", false, ResourceLoader.CACHE_MODE_IGNORE)
	_check(req_err == OK, "load_threaded_request succeeded: " + error_string(req_err))
	if req_err == OK:
		while ResourceLoader.load_threaded_get_status("res://textures/test_prewarm_tex.dstex") == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
			await process_frame
		var status := ResourceLoader.load_threaded_get_status("res://textures/test_prewarm_tex.dstex")
		_check(status == ResourceLoader.THREAD_LOAD_LOADED, "Threaded load completed successfully")
		var tex_async = ResourceLoader.load_threaded_get("res://textures/test_prewarm_tex.dstex")
		_check(tex_async != null, "Threaded load returned a valid texture")
		if tex_async != null:
			var async_class := tex_async.get_class()
			print("[Test] Threaded texture class: ", async_class)
			_check(tex_async is ImageTexture, "Hypothesis 2 VERIFIED: Background threaded load produces ImageTexture (CPU fallback path)")
			_check(not (tex_async is Texture2DRD), "Hypothesis 2 VERIFIED: Background threaded load is NOT Texture2DRD")
			if tex_async is ImageTexture:
				var img: Image = (tex_async as ImageTexture).get_image()
				_check(img != null and not img.is_empty(), "ImageTexture contains valid Image data")

	# -------------------------------------------------------------
	# 3. Phase 3: Approach B Pre-Warming & Scene Cache-Hit (Hypothesis 3)
	# -------------------------------------------------------------
	print("\n--- Phase 3: Approach B Pre-warm & Scene Binding (Hypothesis 3) ---")
	print("[Test] Step 1: Pre-warming texture into ResourceCache with CACHE_MODE_REUSE...")
	var prewarmed_tex = ResourceLoader.load("res://textures/test_prewarm_tex.dstex", "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	_check(prewarmed_tex != null and prewarmed_tex is Texture2DRD, "Pre-warmed texture is Texture2DRD")

	print("[Test] Step 2: Yielding frame (process_frame) to simulate time-slice...")
	var frame_before := Engine.get_process_frames()
	# process_frame is emitted inside SceneTree::process(), before the engine's
	# iteration counter advances, so a single await can resume within the same
	# iteration (measured: before=0, after one await=0, after two awaits=1).
	# Await two frames so the assertion actually crosses an iteration boundary.
	await process_frame
	await process_frame
	var frame_after := Engine.get_process_frames()
	_check(frame_after > frame_before, "Frame advanced during yield (UI rendered smoothly)")

	print("[Test] Step 3: Loading scene referencing pre-warmed texture...")
	var scene_res: PackedScene = ResourceLoader.load(scene_path, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE)
	_check(scene_res != null, "Scene resource loaded")
	if scene_res != null:
		var scene_node: Node3D = scene_res.instantiate()
		_check(scene_node != null, "Scene instantiated")
		if scene_node != null:
			var mesh_inst: MeshInstance3D = scene_node.get_node_or_null("MeshInstance")
			_check(mesh_inst != null, "Found MeshInstance in scene")
			if mesh_inst != null:
				var mat: StandardMaterial3D = mesh_inst.material_override
				_check(mat != null, "Found StandardMaterial3D on MeshInstance")
				if mat != null:
					var bound_tex := mat.albedo_texture
					_check(bound_tex != null, "Material has albedo_texture bound")
					if bound_tex != null:
						print("[Test] Material bound texture class: ", bound_tex.get_class())
						_check(bound_tex is Texture2DRD, "Hypothesis 3 VERIFIED: Bound scene texture is Texture2DRD")
						_check(bound_tex == prewarmed_tex, "Hypothesis 3 VERIFIED: Bound texture is the exact pre-warmed instance (100% Cache Hit, 0 re-reads!)")
			scene_node.free()

	# -------------------------------------------------------------
	# 4. Phase 4: Time-Slicing Frame Cadence (Hypothesis 4)
	# -------------------------------------------------------------
	print("\n--- Phase 4: Time-Slicing Frame Cadence (Hypothesis 4) ---")
	var frames_pumped := 0
	for i in range(5):
		var f_start := Engine.get_process_frames()
		var _t = ResourceLoader.load("res://textures/test_prewarm_tex.dstex", "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
		await process_frame
		var f_end := Engine.get_process_frames()
		if f_end > f_start:
			frames_pumped += 1
	_check(frames_pumped == 5, "Hypothesis 4 VERIFIED: All 5 time-sliced iterations advanced process_frames (0 freezes)")

func _make_texture_payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(PAYLOAD_SIZE)
	for y in range(TEXTURE_HEIGHT):
		for x in range(TEXTURE_WIDTH):
			var pixel_offset := y * ROW_PITCH + x * 4
			payload[pixel_offset] = (x * 4) % 256
			payload[pixel_offset + 1] = (y * 4) % 256
			payload[pixel_offset + 2] = 128
			payload[pixel_offset + 3] = 255
	return payload

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		failures += 1
		printerr("[FAIL] ", message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("\n--- Cleaning up Fixtures ---")
	_restore_archive_resolution()
	# DirectStorage keeps the archive open in its IDStorageFile cache: shut the
	# context down before deleting, or Windows refuses the delete and the stale
	# res://textures.dspkg breaks the next run's DSPkg.finalize().
	_shutdown_ds()
	for path in cleanup_paths:
		var glob := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(glob):
			DirAccess.remove_absolute(glob)
			print("[Cleanup] Removed: ", path)
	print("\n=================================================================")
	print("[Test Prewarm & Threading] Finished. Result: ", "PASSED (All 4 Hypotheses Verified)" if failures == 0 else ("FAILED (%d errors)" % failures))
	print("=================================================================\n")
	quit(1 if failures > 0 else 0)
