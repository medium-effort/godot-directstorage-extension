extends SceneTree

## Headless closure test for the frame-time window seen by DSProfiler.
##
## The profiler's aggregation and bindings are covered natively; this test pins
## the GDScript glue in DSRuntime._process: a real prewarm must open a frame
## window and close it when the load finishes, so `completed_windows` and
## `frame_count` actually move. Without this, a broken idle predicate would
## leave the metrics silently empty and no test would notice.
##
## The autoload's `_process` is left RUNNING on purpose: it is the code under
## test (it flushes the stream and brackets the window). DSPreloader.prewarm_async
## tracks the tag itself, so the window opens on the next frame regardless of how
## fast the fallback backend finishes.
##
## Run (workspace root):
##   <GODOT_CONSOLE> --headless --path project -s "<ABS>/project/headless_tests/test_frame_window_metrics.gd"

const PreloaderScript := preload("uid://h8ts55gju2q4")

## A context stand-in that exposes neither prewarm query method: the version-skew
## shape DSRuntime's progress poll has to survive. A build that cannot answer the
## queries can never prove a tag finished.
class StubCtx extends RefCounted:
	func is_initialized() -> bool:
		return true

const TAG := "DS-FRAME-WINDOW"
const PROBE_DIR := "res://test_assets/frame_window/"
const TEX_W := 64
const TEX_H := 64
const WATCHDOG_SEC := 120.0
const MAX_FRAMES := 900
## Idle frames awaited before the load, so the baseline mean is non-zero.
const SETTLE_FRAMES := 5

var _failures := 0
var _done := false
var _staged: Array[String] = []
var _profiler: Object = null


func _initialize() -> void:
	_arm_watchdog()
	print("[%s] Godot %s" % [TAG, Engine.get_version_info().get("string", "?")])
	await _run()
	_finish()


func _run() -> void:
	if not _preconditions():
		return

	# Idle frames define the "before" baseline. More than one, because the
	# window opens on the frame AFTER the prewarm is queued.
	for i in SETTLE_FRAMES:
		await process_frame
	var baseline: Dictionary = _profiler.call("get_frame_window_stats")
	_check(baseline.has("mean_frame_before_usec"), "profiler responds before the load")

	var png: String = _stage_pair()
	if png.is_empty():
		return

	var tag: int = PreloaderScript.prewarm_async(PackedStringArray([png]))
	_check(tag > 0, "prewarm_async issues a tag (%d)" % tag)
	if tag <= 0:
		return
	var runtime: Node = _runtime_node()
	if runtime == null:
		_fail("DSRuntime autoload is not in the tree")
		return
	_check(bool(runtime.is_tracking_prewarm(tag)),
		"prewarm_async tracks its tag on the runtime (window precondition)")

	# Let the autoload's _process drive the load; wait for the window to close.
	var frames := 0
	var windows_before: int = int(baseline.get("completed_windows", 0))
	while frames < MAX_FRAMES:
		var stats: Dictionary = _profiler.call("get_frame_window_stats")
		if int(stats.get("completed_windows", 0)) > windows_before:
			break
		frames += 1
		await process_frame

	var stats: Dictionary = _profiler.call("get_frame_window_stats")
	var completed: int = int(stats.get("completed_windows", 0))
	_check(completed > windows_before,
		"the runtime opened and closed a frame window for the prewarm (windows %d -> %d)" % [windows_before, completed])
	_check(not bool(stats.get("active", true)), "the window is closed after the load finishes")
	_check(int(stats.get("frame_count", 0)) > 0,
		"the closed window counted frames (%d)" % int(stats.get("frame_count", 0)))
	_check(float(stats.get("mean_frame_before_usec", 0.0)) > 0.0,
		"the before mean is populated (%.1f us)" % float(stats.get("mean_frame_before_usec", 0.0)))
	_check(float(stats.get("mean_frame_during_usec", 0.0)) > 0.0,
		"the during mean is populated (%.1f us)" % float(stats.get("mean_frame_during_usec", 0.0)))

	# Guard: a tag that was never issued still erases itself, because the
	# predicate reports unknown tags as complete. It passes today; it fails if
	# that unknown-tag contract ever changes and the tag starts holding the window.
	var unknown_tag := 987654321
	runtime.track_prewarm(unknown_tag)
	_check(bool(runtime.is_tracking_prewarm(unknown_tag)), "an unknown tag is accepted for tracking")
	for _i in 3:
		await process_frame
	_check(not bool(runtime.is_tracking_prewarm(unknown_tag)), "the runtime drops an unknown tag")
	var after_unknown: Dictionary = _profiler.call("get_frame_window_stats")
	_check(not bool(after_unknown.get("active", true)), "an unknown tag leaves no window open")

	# Closure: a context that cannot answer the prewarm queries can never prove a
	# tag finished, so the poll must drop the watch instead of leaving
	# streaming_active true for the session (which holds the frame window open).
	var orphan_tag := 636363
	runtime.track_prewarm(orphan_tag)
	_check(bool(runtime.is_tracking_prewarm(orphan_tag)), "a tag is tracked before the API gate runs")
	runtime._poll_prewarm_progress(StubCtx.new())
	_check(not bool(runtime.is_tracking_prewarm(orphan_tag)),
		"a tag whose progress cannot be polled is dropped instead of holding the window")
	for _i in 2:
		await process_frame
	var after_gate: Dictionary = _profiler.call("get_frame_window_stats")
	_check(not bool(after_gate.get("active", true)), "the dropped tag leaves no window open")


func _preconditions() -> bool:
	for cls in ["DSContext", "DSTextureCache", "DSTex", "DSProfiler"]:
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
		_fail("DSTextureCache is disabled (dsgd_extension/texture_cache_enabled=false)")
		return false
	if PreloaderScript._has_stream_pump() == false:
		_fail("no stream pump node; prewarm refuses to start")
		return false
	_profiler = Engine.get_singleton("DSProfiler")
	if _profiler == null:
		_fail("DSProfiler singleton unavailable")
		return false
	for method in ["record_frame", "begin_load_window", "end_load_window", "get_frame_window_stats"]:
		if not _profiler.has_method(method):
			_fail("DSProfiler is missing the frame-window method: " + method)
			return false
	return true


func _runtime_node() -> Node:
	var node: Node = root.get_node_or_null("DSRuntime")
	if node == null:
		node = root.get_node_or_null("DSBatchManager")
	return node


func _stage_pair() -> String:
	var dstex_path := PROBE_DIR + "fw.dstex"
	var png_path := PROBE_DIR + "fw.png"
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
