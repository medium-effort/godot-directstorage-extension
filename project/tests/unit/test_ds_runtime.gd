extends GutTest

const PreloaderScript := preload("uid://h8ts55gju2q4")
const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const PROBE_DIR := "res://gut_tmp_ds_runtime/"
const TEX_W := 64
const TEX_H := 64
const MAX_FRAMES := 900
const UNKNOWN_TAG_OFFSET := 1000000

var _finished: Array[int] = []
var _runtime: Node = null


func before_each() -> void:
	_finished.clear()
	Helpers.ensure_clean_dir(PROBE_DIR)


func after_each() -> void:
	if _runtime != null and _runtime.prewarm_finished.is_connected(_on_prewarm_finished):
		_runtime.prewarm_finished.disconnect(_on_prewarm_finished)
	if _runtime != null:
		_runtime.set_process(true)
	var root := get_tree().root
	var alias := root.get_node_or_null("DSBatchManager")
	if alias != null:
		alias.set_process(true)
	if Engine.has_singleton("DSTextureCache"):
		Engine.get_singleton("DSTextureCache").clear()
	if Engine.has_singleton("DSContext"):
		var ctx := Engine.get_singleton("DSContext")
		if ctx != null and ctx.is_initialized():
			ctx.shutdown_ds()
	Helpers.rm_dir_recursive(PROBE_DIR)


func _on_prewarm_finished(tag: int) -> void:
	_finished.append(tag)


func test_prewarm_finished_withheld_until_builds_drain() -> void:
	for cls in ["DSContext", "DSTextureCache", "DSTex"]:
		assert_true(ClassDB.class_exists(cls), "ClassDB has " + cls)

	var ctx := Engine.get_singleton("DSContext")
	var cache := Engine.get_singleton("DSTextureCache")
	assert_not_null(ctx)
	assert_not_null(cache)
	assert_true(ctx.initialize_ds(), "DSContext initializes")

	await get_tree().process_frame
	await get_tree().process_frame

	var root := get_tree().root
	_runtime = root.get_node_or_null("DSRuntime")
	if _runtime == null:
		_runtime = root.get_node_or_null("DSBatchManager")
	assert_not_null(_runtime, "DSRuntime autoload present")

	_runtime.prewarm_finished.connect(_on_prewarm_finished)
	_runtime.set_process(false)
	var alias := root.get_node_or_null("DSBatchManager")
	if alias != null:
		alias.set_process(false)

	var png := _stage_pair("tick")
	var tag := PreloaderScript.prewarm_async(PackedStringArray([png]))
	assert_gt(tag, 0, "prewarm_async issues a tag")
	assert_true(bool(_runtime.is_tracking_prewarm(tag)))

	assert_true(await _pump_requests_only(ctx, tag), "request leg converges")
	assert_true(bool(ctx.is_prewarm_complete(tag)))
	assert_gt(int(cache.get_unbuilt_prewarm_count(tag)), 0, "unbuilt texture still pending")
	assert_false(bool(PreloaderScript.is_prewarm_complete(tag)))
	assert_true(_finished.is_empty())

	_runtime._poll_prewarm_progress(ctx)
	assert_true(_finished.is_empty(), "prewarm_finished withheld while unbuilt")

	assert_true(await _drain_builds(cache, tag), "build queue drains")
	assert_eq(int(cache.get_unbuilt_prewarm_count(tag)), 0)
	assert_true(bool(PreloaderScript.is_prewarm_complete(tag)))
	_runtime._poll_prewarm_progress(ctx)
	assert_eq(_finished, [tag])
	assert_false(bool(_runtime.is_tracking_prewarm(tag)))

	var control_tag := tag + UNKNOWN_TAG_OFFSET
	_runtime.track_prewarm(control_tag)
	assert_true(bool(_runtime.is_tracking_prewarm(control_tag)))
	_runtime._poll_prewarm_progress(ctx)
	assert_eq(_finished, [tag, control_tag])


func test_adaptive_gpu_backpressure_stride_state_machine() -> void:
	var root := get_tree().root
	var rt := root.get_node_or_null("DSRuntime")
	assert_not_null(rt, "DSRuntime autoload present")

	assert_eq(rt._effective_gpu_slice_label("warm", 8.0), "warm")
	assert_eq(rt._effective_gpu_slice_label("warm", 20.0), "mild")
	assert_eq(rt._effective_gpu_slice_label("warm", 40.0), "heavy")
	assert_eq(rt._effective_gpu_slice_label("mild", 8.0), "mild")
	assert_eq(rt._effective_gpu_slice_label("heavy", 8.0), "heavy")

	rt._gpu_pump_frames_since = 0
	assert_true(rt._should_pump_gpu_now("warm", 8.0), "warm pumps every frame")

	rt._gpu_pump_frames_since = 0
	var mild_pumps := 0
	for _i in range(rt.GPU_PUMP_FRAME_STRIDE_MILD):
		if rt._should_pump_gpu_now("mild", 8.0):
			mild_pumps += 1
	assert_eq(mild_pumps, 1, "mild pumps once per GPU_PUMP_FRAME_STRIDE_MILD")

	rt._gpu_pump_frames_since = 0
	var heavy_pumps := 0
	for _i in range(rt.GPU_PUMP_FRAME_STRIDE_HEAVY):
		if rt._should_pump_gpu_now("heavy", 8.0):
			heavy_pumps += 1
	assert_eq(heavy_pumps, 1, "heavy is paced, never stopped")


func _pump_requests_only(ctx: Object, tag: int) -> bool:
	var frames := 0
	while not bool(ctx.is_prewarm_complete(tag)):
		if ctx.is_batch_complete() and ctx.has_pending_results():
			ctx.collect_completed_results()
		if ctx.has_stream_pending():
			ctx.flush_stream_async()
		frames += 1
		if frames > MAX_FRAMES:
			return false
		await get_tree().process_frame
	if ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()
	return true


func _drain_builds(cache: Object, tag: int) -> bool:
	var frames := 0
	while int(cache.get_unbuilt_prewarm_count(tag)) > 0:
		cache.poll_completed(0, 32, 8000)
		frames += 1
		if frames > MAX_FRAMES:
			return false
		await get_tree().process_frame
	return true


func _stage_pair(key: String) -> String:
	var dstex_path := PROBE_DIR + key + ".dstex"
	var png_path := PROBE_DIR + key + ".png"
	var payload := PackedByteArray()
	payload.resize(TEX_W * TEX_H * 4)
	payload.fill(128)
	var dstex := DSTex.new()
	assert_eq(dstex.write_to_file(dstex_path, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, 1, 1), OK)
	var img := Image.create(TEX_W, TEX_H, false, Image.FORMAT_RGBA8)
	assert_eq(img.save_png(ProjectSettings.globalize_path(png_path)), OK)
	var imp := FileAccess.open(png_path + ".import", FileAccess.WRITE)
	assert_not_null(imp)
	imp.store_string("[remap]\n\npath=\"" + dstex_path + "\"\n")
	imp.close()
	return png_path
