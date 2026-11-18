extends SceneTree

## Closure test for DSPreloader.start_scene_prewarm / cancel_scene_prewarm /
## prewarm_scene_async and DSRuntime's Stage 1 -> Stage 2 state machine.
##
## Runs in BOTH modes:
##   - Headless: tests state machine, progress signal pacing, and Stage 1 / Stage 2
##     cancellation (Q42 drain).
##   - D3D12 windowed (Q09): additionally verifies Texture2DRD reuse and
##     loader_cpu_fallbacks == 0.
##
## Run (workspace root):
##   <GODOT_CONSOLE> --headless --path project -s "res://headless_tests/test_prewarm_scene_async.gd"
##   <GODOT_CONSOLE> --rendering-driver d3d12 --audio-driver Dummy --windowed --resolution 64x64 --path project -s "res://headless_tests/test_prewarm_scene_async.gd"

const PreloaderScript := preload("uid://h8ts55gju2q4")

const TAG := "PREWARM-SCENE-ASYNC"
const WATCHDOG_SEC := 30.0
const TEX_W := 64
const TEX_H := 64
const FORCE_ARCHIVE_SETTING := "dsgd_extension/force_archive_resolution"

var _failures := 0
var _cleanup_paths: Array[String] = []
var _force_archive_present := false
var _force_archive_prev: Variant = null


func _initialize() -> void:
	_arm_watchdog()
	print("[%s] Godot %s (RD=%s)" % [
		TAG,
		Engine.get_version_info().get("string", "?"),
		"available" if RenderingServer.get_rendering_device() != null else "headless/null",
	])
	await _run_tests()
	_finish()


func _arm_watchdog() -> void:
	var timer := create_timer(WATCHDOG_SEC)
	timer.timeout.connect(func() -> void:
		printerr("[%s][FAIL] Watchdog timeout after %.1fs" % [TAG, WATCHDOG_SEC])
		_failures += 1
		_finish()
	)


func _run_tests() -> void:
	# Settle autoloads.
	await process_frame
	await process_frame

	var runtime: Node = root.get_node_or_null("DSRuntime")
	_check(runtime != null, "DSRuntime autoload is present")
	if runtime == null:
		return

	var has_gpu := false
	var ctx: Object = Engine.get_singleton("DSContext") if ClassDB.class_exists("DSContext") else null
	if RenderingServer.get_rendering_device() != null and ctx != null:
		if ctx.initialize_ds() and ctx.is_d3d12_backend():
			has_gpu = true

	var tex_virtual := "res://test_assets/test.png"
	var scene_a := "res://test_prewarm_scene_async_a.tscn"
	var scene_b := "res://test_prewarm_scene_async_b.tscn"
	var scene_c := "res://test_prewarm_scene_async_c.tscn"
	_cleanup_paths.append_array([scene_a, scene_b, scene_c])

	if has_gpu and ClassDB.class_exists("DSTex") and ClassDB.class_exists("DSPkg"):
		if not _stage_texture_archive(tex_virtual):
			return
		_write_test_scene(scene_a, tex_virtual)
		_write_test_scene(scene_b, tex_virtual)
		_write_test_scene(scene_c, "")
	else:
		_write_test_scene(scene_a, "")
		_write_test_scene(scene_b, "")
		_write_test_scene(scene_c, "")

	# -----------------------------------------------------------------
	# 1. prewarm_scene_async() happy path + progress pacing
	# -----------------------------------------------------------------
	print("[%s] --- Test 1: prewarm_scene_async() happy path ---" % TAG)
	var fallbacks_before: int = _get_cpu_fallbacks()
	var progress_events: Array[Dictionary] = []
	var on_prog := func(stage: String, progress: float) -> void:
		progress_events.append({ "stage": stage, "progress": progress })

	var packed_a: PackedScene = await PreloaderScript.prewarm_scene_async(
		scene_a,
		PackedStringArray(),
		16,
		true,
		0,
		on_prog
	)
	_check(packed_a != null, "prewarm_scene_async() returned valid PackedScene")
	_check(not progress_events.is_empty(), "on_progress received >= 1 event (count=%d)" % progress_events.size())
	var last_p := -1.0
	var monotonic := true
	for ev in progress_events:
		var p: float = float(ev["progress"])
		if p < last_p:
			monotonic = false
		last_p = p
	_check(monotonic, "progress values are monotonically non-decreasing")
	_check(String(progress_events[progress_events.size() - 1]["stage"]) == "done", "final progress stage is 'done'")

	if has_gpu and packed_a != null:
		var inst: Node = packed_a.instantiate()
		var mesh_inst: MeshInstance3D = inst.get_node_or_null("TestMesh") as MeshInstance3D
		_check(mesh_inst != null, "Instantiated scene has TestMesh")
		if mesh_inst != null and mesh_inst.material_override is StandardMaterial3D:
			var mat := mesh_inst.material_override as StandardMaterial3D
			_check(mat.albedo_texture is Texture2DRD, "Bound scene texture is Texture2DRD (got %s)" % [mat.albedo_texture.get_class() if mat.albedo_texture else "null"])
		inst.free()
		var fallbacks_after: int = _get_cpu_fallbacks()
		_check(fallbacks_after == fallbacks_before, "loader_cpu_fallbacks stayed unchanged (%d -> %d)" % [fallbacks_before, fallbacks_after])

	# -----------------------------------------------------------------
	# 2. cancel_scene_prewarm() during Stage 1 (or immediate cancel)
	# -----------------------------------------------------------------
	print("[%s] --- Test 2: cancel_scene_prewarm() Stage 1 ---" % TAG)
	var tag_b: int = PreloaderScript.start_scene_prewarm(scene_b)
	_check(tag_b > 0, "start_scene_prewarm(scene_b) returned positive tag (%d)" % tag_b)
	if tag_b > 0:
		var fin_b := { "done": false, "packed": null }
		var cb_b := func(t: int, _p: String, packed: PackedScene) -> void:
			if t == tag_b:
				fin_b["done"] = true
				fin_b["packed"] = packed
		runtime.scene_prewarm_finished.connect(cb_b)
		PreloaderScript.cancel_scene_prewarm(tag_b)
		var guard := 0
		while not bool(fin_b["done"]) and guard < 240:
			await process_frame
			guard += 1
		if runtime.scene_prewarm_finished.is_connected(cb_b):
			runtime.scene_prewarm_finished.disconnect(cb_b)
		_check(bool(fin_b["done"]), "Stage 1 cancel emitted scene_prewarm_finished")
		_check(fin_b["packed"] == null, "Stage 1 cancel returned null PackedScene")
		_check(bool(runtime.is_scene_prewarm_complete(tag_b)), "is_scene_prewarm_complete(tag_b) is true")
		_check(
			ResourceLoader.load_threaded_get_status(scene_b) == ResourceLoader.THREAD_LOAD_INVALID_RESOURCE,
			"Stage 1 cancel never initiated load_threaded_request(scene_b)"
		)

	# -----------------------------------------------------------------
	# 3. cancel_scene_prewarm() during Stage 2 (Q42 drain)
	# -----------------------------------------------------------------
	print("[%s] --- Test 3: cancel_scene_prewarm() Stage 2 (Q42 drain) ---" % TAG)
	var tag_c: int = int(runtime.start_scene_prewarm(scene_c, PackedStringArray()))
	_check(tag_c > 0, "start_scene_prewarm(scene_c) returned positive tag (%d)" % tag_c)
	if tag_c > 0:
		var fin_c := { "done": false, "packed": null }
		var cb_c := func(t: int, _p: String, packed: PackedScene) -> void:
			if t == tag_c:
				fin_c["done"] = true
				fin_c["packed"] = packed
		runtime.scene_prewarm_finished.connect(cb_c)
		# Advance one frame so Stage 1 (0 deps) transitions into Stage 2.
		await process_frame
		PreloaderScript.cancel_scene_prewarm(tag_c)
		var guard_c := 0
		while not bool(fin_c["done"]) and guard_c < 240:
			await process_frame
			guard_c += 1
		if runtime.scene_prewarm_finished.is_connected(cb_c):
			runtime.scene_prewarm_finished.disconnect(cb_c)
		_check(bool(fin_c["done"]), "Stage 2 cancel emitted scene_prewarm_finished")
		_check(fin_c["packed"] == null, "Stage 2 cancel returned null PackedScene after Q42 drain")
		_check(bool(runtime.is_scene_prewarm_complete(tag_c)), "is_scene_prewarm_complete(tag_c) is true")


func _stage_texture_archive(tex_virtual: String) -> bool:
	_force_archive_present = ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING)
	_force_archive_prev = ProjectSettings.get_setting(FORCE_ARCHIVE_SETTING) if _force_archive_present else null
	ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, true)

	var dstex_path := "user://test_scene_prewarm_tex.dstex"
	var dspkg_path := "res://textures.dspkg"
	_cleanup_paths.append_array([dstex_path, dspkg_path])

	var payload := PackedByteArray()
	payload.resize(TEX_W * TEX_H * 4)
	payload.fill(200)

	var dstex := DSTex.new()
	var w_err: Error = dstex.write_to_file(dstex_path, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, 1, 1)
	_check(w_err == OK, "DSTex write_to_file succeeded: " + error_string(w_err))
	if w_err != OK:
		return false

	var dspkg := DSPkg.new()
	var c_err: Error = dspkg.create(ProjectSettings.globalize_path(dspkg_path))
	_check(c_err == OK, "DSPkg create succeeded: " + error_string(c_err))
	if c_err != OK:
		return false
	_check(dspkg.add_texture(tex_virtual, dstex_path) == OK, "DSPkg add_texture succeeded")
	var f_err: Error = dspkg.finalize()
	_check(f_err == OK, "DSPkg finalize succeeded: " + error_string(f_err))
	if f_err != OK:
		return false
	return true


func _write_test_scene(scene_path: String, tex_virtual: String) -> void:
	var content: String
	if tex_virtual.is_empty():
		content = """[gd_scene load_steps=2 format=3]

[sub_resource type="StandardMaterial3D" id="StandardMaterial3D_1"]
albedo_color = Color(0.2, 0.6, 0.9, 1)

[node name="Root" type="Node3D"]

[node name="TestMesh" type="MeshInstance3D" parent="."]
material_override = SubResource("StandardMaterial3D_1")
"""
	else:
		content = """[gd_scene load_steps=3 format=3]

[ext_resource type="Texture2D" path="%s" id="1_tex"]

[sub_resource type="StandardMaterial3D" id="StandardMaterial3D_1"]
albedo_texture = ExtResource("1_tex")

[node name="Root" type="Node3D"]

[node name="TestMesh" type="MeshInstance3D" parent="."]
material_override = SubResource("StandardMaterial3D_1")
""" % tex_virtual
	var f := FileAccess.open(scene_path, FileAccess.WRITE)
	_check(f != null, "Opened %s for write" % scene_path)
	if f != null:
		f.store_string(content)
		f.close()


func _get_cpu_fallbacks() -> int:
	if not ClassDB.class_exists("DSProfiler"):
		return 0
	var prof: Object = Engine.get_singleton("DSProfiler")
	if prof == null or not prof.has_method("get_stats"):
		return 0
	var stats: Dictionary = prof.call("get_stats") as Dictionary
	return int(stats.get("loader_cpu_fallbacks", 0))


func _check(cond: bool, msg: String) -> void:
	if cond:
		print("[%s][PASS] %s" % [TAG, msg])
	else:
		_failures += 1
		printerr("[%s][FAIL] %s" % [TAG, msg])


func _finish() -> void:
	if _force_archive_present:
		ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, _force_archive_prev)
	elif ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING):
		ProjectSettings.clear(FORCE_ARCHIVE_SETTING)
	if ClassDB.class_exists("DSTextureCache"):
		var cache: Object = Engine.get_singleton("DSTextureCache")
		if cache != null and cache.has_method("clear"):
			cache.call("clear")
	if ClassDB.class_exists("DSContext"):
		var ctx: Object = Engine.get_singleton("DSContext")
		if ctx != null and ctx.is_initialized():
			ctx.shutdown_ds()
	for p in _cleanup_paths:
		var glob := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(glob):
			DirAccess.remove_absolute(glob)
	print("[%s] Finished: %s" % [TAG, "PASSED" if _failures == 0 else ("FAILED (%d)" % _failures)])
	quit(1 if _failures > 0 else 0)
