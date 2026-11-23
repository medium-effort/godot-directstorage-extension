extends GutTest

const PreloaderScript := preload("uid://h8ts55gju2q4")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const PROBE_DIR := "res://gut_tmp_ds_preloader/"
const DSPKG_PATH := "res://textures.dspkg"
const FORCE_ARCHIVE_SETTING := "dsgd_extension/force_archive_resolution"
const TEX_W := 64
const TEX_H := 64
const SET_N := 3
const MAX_PUMP_FRAMES := 600

var _had_force_archive := false
var _prev_force_archive: Variant = null


func before_each() -> void:
	Helpers.ensure_clean_dir(PROBE_DIR)
	_had_force_archive = ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING)
	_prev_force_archive = ProjectSettings.get_setting(FORCE_ARCHIVE_SETTING) if _had_force_archive else null
	ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, true)


func after_each() -> void:
	Helpers.restore_setting(FORCE_ARCHIVE_SETTING, _had_force_archive, _prev_force_archive)
	if Engine.has_singleton("DSTextureCache"):
		Engine.get_singleton("DSTextureCache").clear()
	if Engine.has_singleton("DSContext"):
		var ctx := Engine.get_singleton("DSContext")
		if ctx != null and ctx.is_initialized():
			ctx.shutdown_ds()
	var abs_pkg := ProjectSettings.globalize_path(DSPKG_PATH)
	if FileAccess.file_exists(abs_pkg):
		DirAccess.remove_absolute(abs_pkg)
	Helpers.rm_dir_recursive(PROBE_DIR)


func test_async_prewarm_archive_import_and_cancel() -> void:
	var ctx := Engine.get_singleton("DSContext")
	var cache := Engine.get_singleton("DSTextureCache")
	assert_not_null(ctx)
	assert_not_null(cache)
	assert_true(ctx.initialize_ds())
	var gpu: bool = bool(ctx.is_d3d12_backend())

	var payload := PackedByteArray()
	payload.resize(TEX_W * TEX_H * 4)
	payload.fill(180)

	var set_pkg: Array[String] = []
	var set_imp: Array[String] = []
	var pkg_dstex: Array[String] = []
	for i in SET_N:
		var dstex := PROBE_DIR + "pkg_%02d.dstex" % i
		_write_dstex(dstex, payload)
		pkg_dstex.append(dstex)
		set_pkg.append(PROBE_DIR + "pkg_%02d.png" % i)
	for i in SET_N:
		var dstex := PROBE_DIR + "imp_%02d.dstex" % i
		var png := PROBE_DIR + "imp_%02d.png" % i
		_write_dstex(dstex, payload)
		_write_import(png, dstex)
		set_imp.append(png)

	var dspkg := DSPkg.new()
	assert_eq(dspkg.create(ProjectSettings.globalize_path(DSPKG_PATH)), OK)
	for i in set_pkg.size():
		assert_eq(dspkg.add_texture(set_pkg[i], pkg_dstex[i]), OK)
	assert_eq(dspkg.finalize(), OK)

	var scene_path := PROBE_DIR + "probe_scene.tscn"
	_write_scene(scene_path, set_imp[0])

	assert_true(PreloaderScript.is_stream_prewarm_available())
	var collected: PackedStringArray = PreloaderScript.collect_scene_textures(scene_path)
	assert_true(set_imp[0] in collected)

	var tag_pkg := PreloaderScript.prewarm_async(PackedStringArray(set_pkg))
	assert_gt(tag_pkg, 0)
	assert_true(await _pump_until(ctx, cache, tag_pkg))
	for p in set_pkg:
		var tex: Texture2D = cache.get_cached(p, 1)
		assert_not_null(tex)
		if not gpu:
			assert_true(tex is ImageTexture)

	var tag_imp := PreloaderScript.prewarm_async(PackedStringArray(set_imp))
	assert_gt(tag_imp, 0)
	assert_ne(tag_imp, tag_pkg)
	assert_true(await _pump_until(ctx, cache, tag_imp))

	var unknown_state: Dictionary = PreloaderScript.get_prewarm_state(999999999)
	assert_true(bool(unknown_state.get("complete", false)))
	assert_eq(unknown_state.get("progress", Vector2i(-1, -1)), Vector2i(0, 0))

	ctx.cancel_prewarm(tag_imp)
	ctx.cancel_prewarm(999999999)
	assert_true(ctx.is_prewarm_complete(999999999))


func test_prewarm_scene_async_and_stage_cancels() -> void:
	await get_tree().process_frame
	await get_tree().process_frame

	var runtime := get_tree().root.get_node_or_null("DSRuntime")
	assert_not_null(runtime)

	var scene_a := PROBE_DIR + "scene_a.tscn"
	var scene_b := PROBE_DIR + "scene_b.tscn"
	var scene_c := PROBE_DIR + "scene_c.tscn"
	_write_scene(scene_a, "")
	_write_scene(scene_b, "")
	_write_scene(scene_c, "")

	var progress_events: Array[Dictionary] = []
	var on_prog := func(stage: String, progress: float) -> void:
		progress_events.append({"stage": stage, "progress": progress})

	var packed_a: PackedScene = await PreloaderScript.prewarm_scene_async(
		scene_a, PackedStringArray(), 16, true, 0, on_prog
	)
	assert_not_null(packed_a)
	assert_false(progress_events.is_empty())
	assert_eq(String(progress_events[progress_events.size() - 1]["stage"]), "done")

	var tag_b := PreloaderScript.start_scene_prewarm(scene_b)
	assert_gt(tag_b, 0)
	var fin_b := {"done": false, "packed": null}
	var cb_b := func(t: int, _p: String, packed: PackedScene) -> void:
		if t == tag_b:
			fin_b["done"] = true
			fin_b["packed"] = packed
	runtime.scene_prewarm_finished.connect(cb_b)
	PreloaderScript.cancel_scene_prewarm(tag_b)
	var guard := 0
	while not bool(fin_b["done"]) and guard < 240:
		await get_tree().process_frame
		guard += 1
	if runtime.scene_prewarm_finished.is_connected(cb_b):
		runtime.scene_prewarm_finished.disconnect(cb_b)
	assert_true(bool(fin_b["done"]))
	assert_null(fin_b["packed"])
	assert_true(bool(runtime.is_scene_prewarm_complete(tag_b)))

	var tag_c := int(runtime.start_scene_prewarm(scene_c, PackedStringArray()))
	assert_gt(tag_c, 0)
	var fin_c := {"done": false, "packed": null}
	var cb_c := func(t: int, _p: String, packed: PackedScene) -> void:
		if t == tag_c:
			fin_c["done"] = true
			fin_c["packed"] = packed
	runtime.scene_prewarm_finished.connect(cb_c)
	await get_tree().process_frame
	PreloaderScript.cancel_scene_prewarm(tag_c)
	var guard_c := 0
	while not bool(fin_c["done"]) and guard_c < 240:
		await get_tree().process_frame
		guard_c += 1
	if runtime.scene_prewarm_finished.is_connected(cb_c):
		runtime.scene_prewarm_finished.disconnect(cb_c)
	assert_true(bool(fin_c["done"]))
	assert_null(fin_c["packed"])
	assert_true(bool(runtime.is_scene_prewarm_complete(tag_c)))


func _pump_until(ctx: Object, cache: Object, tag: int) -> bool:
	var frames := 0
	while not ctx.is_prewarm_complete(tag):
		if ctx.is_batch_complete() and ctx.has_pending_results():
			ctx.collect_completed_results()
		if ctx.has_stream_pending():
			ctx.flush_stream_async()
		cache.poll_completed()
		frames += 1
		if frames > MAX_PUMP_FRAMES:
			return false
		await get_tree().process_frame
	if ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()
	cache.poll_completed()
	return true


func _write_dstex(dstex_res: String, payload: PackedByteArray) -> void:
	var dstex := DSTex.new()
	assert_eq(dstex.write_to_file(dstex_res, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, 1, 1), OK)


func _write_import(png_res: String, dstex_res: String) -> void:
	var img := Image.create(TEX_W, TEX_H, false, Image.FORMAT_RGBA8)
	assert_eq(img.save_png(ProjectSettings.globalize_path(png_res)), OK)
	var imp := FileAccess.open(png_res + ".import", FileAccess.WRITE)
	assert_not_null(imp)
	imp.store_string("[remap]\n\npath=\"" + dstex_res + "\"\n")
	imp.close()


func _write_scene(scene_path: String, tex_virtual: String) -> void:
	var content: String
	if tex_virtual.is_empty():
		content = "[gd_scene load_steps=2 format=3]\n\n[sub_resource type=\"StandardMaterial3D\" id=\"StandardMaterial3D_1\"]\nalbedo_color = Color(0.2, 0.6, 0.9, 1)\n\n[node name=\"Root\" type=\"Node3D\"]\n\n[node name=\"TestMesh\" type=\"MeshInstance3D\" parent=\".\"]\nmaterial_override = SubResource(\"StandardMaterial3D_1\")\n"
	else:
		content = "[gd_scene load_steps=3 format=3]\n\n[ext_resource type=\"Texture2D\" path=\"%s\" id=\"1_tex\"]\n\n[sub_resource type=\"StandardMaterial3D\" id=\"StandardMaterial3D_1\"]\nalbedo_texture = ExtResource(\"1_tex\")\n\n[node name=\"Root\" type=\"Node3D\"]\n\n[node name=\"TestMesh\" type=\"MeshInstance3D\" parent=\".\"]\nmaterial_override = SubResource(\"StandardMaterial3D_1\")\n" % tex_virtual
	var f := FileAccess.open(scene_path, FileAccess.WRITE)
	assert_not_null(f)
	f.store_string(content)
	f.close()
