extends SceneTree

## Headless closure test for the prewarm-finished signal contract.
##
## `DSRuntime.prewarm_finished` must not fire while completed stream requests
## are still waiting to be built into `DSTextureCache`. This drives the runtime
## into exactly that state deterministically: the autoload's `_process` is
## frozen, the request stream is harvested without calling
## `DSTextureCache.poll_completed`, and the signal is then polled once. Before
## the predicate fix the signal fires there; after it, only once the build queue
## is empty.
##
## The runtime's `_process` is frozen *after* the autoload has settled, because
## issuing a prewarm before `DSRuntime._ready()` does not produce the pending
## build reliably (measured: 0/5 without the settle, 5/5 with it).
##
## Run (workspace root):
##   <GODOT_CONSOLE> --headless --path project -s "<ABS>/project/headless_tests/test_ds_runtime_tick.gd"

const PreloaderScript := preload("uid://h8ts55gju2q4")

const TAG := "DS-RUNTIME-TICK"
const PROBE_DIR := "res://test_assets/ds_runtime_tick/"
const TEX_W := 64
const TEX_H := 64
const WATCHDOG_SEC := 120.0
const MAX_FRAMES := 900
const UNKNOWN_TAG_OFFSET := 1000000

var _failures := 0
var _done := false
var _staged: Array[String] = []
var _finished: Array[int] = []
var _runtime: Node = null


func _initialize() -> void:
	_arm_watchdog()
	print("[%s] Godot %s" % [TAG, Engine.get_version_info().get("string", "?")])
	await _run()
	_finish()


func _run() -> void:
	if not _preconditions():
		return

	# Settle the autoload (see the header note) before freezing anything.
	await process_frame
	await process_frame

	var ctx: Object = Engine.get_singleton("DSContext")
	var cache: Object = Engine.get_singleton("DSTextureCache")
	_runtime = _runtime_node()
	if _runtime == null:
		_fail("DSRuntime/DSBatchManager autoload is not in the tree")
		return
	_runtime.prewarm_finished.connect(_on_prewarm_finished)
	_runtime.set_process(false)
	var alias: Node = root.get_node_or_null("DSBatchManager")
	if alias != null:
		alias.set_process(false)
	_check(not _runtime.is_processing(), "driver _process is stopped for the probe")

	# --- the disagreeing state: requests done, build still pending ----------
	var png: String = _stage_pair("tick")
	var tag: int = PreloaderScript.prewarm_async(PackedStringArray([png]))
	_check(tag > 0, "prewarm_async issues a tag (%d)" % tag)
	if tag <= 0:
		return
	_check(bool(_runtime.is_tracking_prewarm(tag)), "the tag is tracked by the autoload")

	if not await _pump_requests_only(ctx, tag):
		_fail("request leg did not converge")
		return
	_check(bool(ctx.is_prewarm_complete(tag)), "DSContext reports every request terminal")
	var unbuilt: int = int(cache.get_unbuilt_prewarm_count(tag))
	_check(unbuilt > 0, "DSTextureCache still has an unbuilt texture (%d)" % unbuilt)
	_check(not bool(PreloaderScript.is_prewarm_complete(tag)),
		"the documented predicate still reports the batch unfinished")
	_check(_finished.is_empty(), "no prewarm_finished was emitted while the driver was frozen")

	# --- closure assert: the predicate under test --------------------------
	_runtime._poll_prewarm_progress(ctx)
	_check(_finished.is_empty(),
		"prewarm_finished is withheld while a completed texture is still unbuilt")

	# --- the signal must still arrive once the build queue drains ----------
	if not await _drain_builds(cache, tag):
		_fail("build queue did not drain")
		return
	_check(int(cache.get_unbuilt_prewarm_count(tag)) == 0, "the build queue is empty")
	_check(bool(PreloaderScript.is_prewarm_complete(tag)), "the documented predicate reports completion")
	_runtime._poll_prewarm_progress(ctx)
	_check(_finished.size() == 1 and _finished[0] == tag,
		"prewarm_finished is emitted exactly once for the tag after the builds drain")
	_check(not bool(_runtime.is_tracking_prewarm(tag)), "the finished tag is untracked")

	# --- positive control: a tag with nothing pending is still reported ----
	var control_tag: int = tag + UNKNOWN_TAG_OFFSET
	_runtime.track_prewarm(control_tag)
	_check(bool(_runtime.is_tracking_prewarm(control_tag)), "the control tag is tracked")
	_runtime._poll_prewarm_progress(ctx)
	_check(_finished.size() == 2 and _finished[1] == control_tag,
		"an unknown tag with nothing pending is reported immediately (guard is not a wedge)")
	_check(_finished.size() == 2, "no duplicate or extra prewarm_finished events")


func _preconditions() -> bool:
	for cls in ["DSContext", "DSTextureCache", "DSTex"]:
		if not ClassDB.class_exists(cls):
			_fail("native class missing (stale or missing DLL?): " + cls)
			return false
	if DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(PROBE_DIR)) != OK:
		_fail("cannot create probe dir: " + PROBE_DIR)
		return false
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null:
		_fail("DSContext singleton unavailable")
		return false
	if not ctx.initialize_ds() or not ctx.is_initialized():
		_fail("DSContext could not be initialized")
		return false
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache == null:
		_fail("DSTextureCache singleton unavailable")
		return false
	if cache.has_method("is_enabled") and not bool(cache.is_enabled()):
		_fail("DSTextureCache is disabled (directstorage/texture_cache_enabled=false)")
		return false
	if PreloaderScript._has_stream_pump() == false:
		_fail("no stream pump node; prewarm refuses to start")
		return false
	return true


func _runtime_node() -> Node:
	var node: Node = root.get_node_or_null("DSRuntime")
	if node == null:
		node = root.get_node_or_null("DSBatchManager")
	return node


func _on_prewarm_finished(tag: int) -> void:
	_finished.append(tag)


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
		await process_frame
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
		await process_frame
	return true


func _stage_pair(key: String) -> String:
	var dstex_path := PROBE_DIR + key + ".dstex"
	var png_path := PROBE_DIR + key + ".png"
	var dstex := DSTex.new()
	var err: int = dstex.write_to_file(dstex_path, _payload(), TEX_W, TEX_H, Image.FORMAT_RGBA8, 1, 1)
	if err != OK:
		_fail("cannot stage " + dstex_path + " (err " + str(err) + ")")
		return ""
	_staged.append(dstex_path)
	var img := Image.create(TEX_W, TEX_H, false, Image.FORMAT_RGBA8)
	if img.save_png(ProjectSettings.globalize_path(png_path)) != OK:
		_fail("cannot stage " + png_path)
		return ""
	_staged.append(png_path)
	var imp := FileAccess.open(png_path + ".import", FileAccess.WRITE)
	if imp == null:
		_fail("cannot write " + png_path + ".import")
		return ""
	imp.store_string("[remap]\n\npath=\"" + dstex_path + "\"\n")
	imp.close()
	_staged.append(png_path + ".import")
	return png_path


func _payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(TEX_W * TEX_H * 4)
	for y in TEX_H:
		for x in TEX_W:
			var o := (y * TEX_W + x) * 4
			payload[o] = (x * 4) % 256
			payload[o + 1] = (y * 4) % 256
			payload[o + 2] = 128
			payload[o + 3] = 255
	return payload


func _arm_watchdog() -> void:
	await create_timer(WATCHDOG_SEC).timeout
	if not _done:
		printerr("[%s][FAIL] watchdog: test did not finish in %ds" % [TAG, int(WATCHDOG_SEC)])
		quit(3)


func _check(condition: bool, message: String) -> void:
	if condition:
		print("[%s][PASS] %s" % [TAG, message])
	else:
		_fail(message)


func _fail(message: String) -> void:
	_failures += 1
	printerr("[%s][FAIL] %s" % [TAG, message])


func _finish() -> void:
	_done = true
	print("[%s] --- cleanup ---" % TAG)
	if _runtime != null and _runtime.prewarm_finished.is_connected(_on_prewarm_finished):
		_runtime.prewarm_finished.disconnect(_on_prewarm_finished)
	if _runtime != null:
		_runtime.set_process(true)
	var alias: Node = root.get_node_or_null("DSBatchManager")
	if alias != null:
		alias.set_process(true)
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx != null and ctx.is_initialized():
		ctx.shutdown_ds()
	for p in _staged:
		var abs_path := ProjectSettings.globalize_path(p)
		if FileAccess.file_exists(abs_path):
			if DirAccess.remove_absolute(abs_path) != OK:
				printerr("[%s] cleanup missed: %s" % [TAG, p])
	_staged.clear()
	var abs_dir := ProjectSettings.globalize_path(PROBE_DIR)
	if DirAccess.dir_exists_absolute(abs_dir):
		if DirAccess.get_files_at(PROBE_DIR).is_empty():
			DirAccess.remove_absolute(abs_dir)
	print("[%s] Result: %s" % [TAG, "PASSED" if _failures == 0 else ("FAILED (%d)" % _failures)])
	quit(0 if _failures == 0 else 1)
