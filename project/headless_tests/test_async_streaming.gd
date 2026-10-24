extends SceneTree

## Headless coverage for the uncommitted async texture prewarm (streaming) work.
##
## Exercises the whole enqueue -> continue -> complete-later pipeline on the
## available backend (software fallback headless, or D3D12 when present):
## DSContext stream queue + tags, DSTextureCache prewarm/poll/store, the
## DSPreloader GDScript helpers, batching counters, cancel semantics, and a
## sync-loader regression. Cleans up every file it stages.
##
## Run (workspace root):
##   <GODOT_CONSOLE> --headless --path project -s "<ABS>/test_async_streaming.gd"
## Needs the freshly built editor DLL (see .vscode/build_remote.ps1).

const PreloaderScript := preload("res://addons/directstorage_accelerator/scripts/ds_preloader.gd")

const TAG := "ASYNC-STREAM-PROBE"
const TEX_W := 64
const TEX_H := 64
const SET_N := 3
const PROBE_DIR := "res://test_assets/async_stream_probe/"
const DSPKG_PATH := "res://textures.dspkg"
const WATCHDOG_SEC := 120.0
const MAX_PUMP_FRAMES := 600
# Submit-count slack: SET_N textures must take at most this many submits.
const BATCHING_SLACK := 2

var _failures := 0
var _done := false
var _staged: Array[String] = []
var _created_dspkg := false

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

func _log(msg: String) -> void:
	print("[%s] %s" % [TAG, msg])

func _fail(msg: String) -> void:
	_failures += 1
	printerr("[%s][FAIL] %s" % [TAG, msg])

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[%s][PASS] %s" % [TAG, message])
	else:
		_fail(message)

func _arm_watchdog() -> void:
	await create_timer(WATCHDOG_SEC).timeout
	if not _done:
		printerr("[%s][FAIL] watchdog: ordered completion not reached in %ds" % [TAG, int(WATCHDOG_SEC)])
		quit(3)

func _initialize() -> void:
	_arm_watchdog()
	_log("Godot %s" % Engine.get_version_info().get("string", "?"))
	for cls in ["DSContext", "DSTextureCache", "DSTex", "DSPkg"]:
		if not ClassDB.class_exists(cls):
			_fail("Native class missing (stale DLL?): " + cls)
			_finish(2)
			return
	if not ClassDB.class_exists("DSPreloader"):
		_log("NOTE: DSPreloader global class not in cache (headless scan); driving via preload, same code.")
	await _run()
	_finish(1 if _failures > 0 else 0)

func _run() -> void:
	var ctx: Object = Engine.get_singleton("DSContext")
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if ctx == null or cache == null:
		_fail("DSContext/DSTextureCache singletons unavailable.")
		return
	_check(ctx.initialize_ds(), "DSContext.initialize_ds()")
	if not ctx.is_initialized():
		_fail("Context not initialized; aborting.")
		return
	var backend: String = ctx.get_active_backend_name()
	var gpu: bool = ctx.is_d3d12_backend()
	_log("Backend: %s (d3d12=%s)" % [backend, str(gpu)])

	# --- Phase 0: fixtures (archive first: path resolution caches per process).
	_log("--- Phase 0: fixtures ---")
	_force_archive_resolution()
	_check(DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(PROBE_DIR)) == OK,
		"probe dir ready: " + PROBE_DIR)
	var payload := _make_payload()
	var set_pkg: Array[String] = []
	var set_imp: Array[String] = []
	var pkg_dstex: Array[String] = []
	var imp_dstex: Array[String] = []
	for i in SET_N:
		var key := "probe_t%02d" % i
		var dstex := PROBE_DIR + key + ".dstex"
		_write_dstex(dstex, payload)
		pkg_dstex.append(dstex)
		set_pkg.append(PROBE_DIR + "pkg_" + key + ".png")
	for i in SET_N:
		var key := "imp_t%02d" % i
		var dstex := PROBE_DIR + key + ".dstex"
		_write_dstex(dstex, payload)
		imp_dstex.append(dstex)
		set_imp.append(PROBE_DIR + key + ".png")
	_write_dspkg(set_pkg, pkg_dstex)
	_write_import_set(set_imp, imp_dstex)
	_write_probe_scene(set_imp)
	if _failures > 0:
		return

	# --- Phase 1: helper surface (no I/O).
	_log("--- Phase 1: helper surface ---")
	_check(PreloaderScript.is_stream_prewarm_available(), "DSPreloader.is_stream_prewarm_available()")
	var collected: PackedStringArray = PreloaderScript.collect_scene_textures(PROBE_DIR + "probe_scene.tscn")
	_check(set_imp[0] in collected, "collect_scene_textures finds swept texture (%d found)" % collected.size())

	# --- Phase 2: prewarm via the archive leg.
	_log("--- Phase 2: archive-leg prewarm ---")
	var submitted_before: int = ctx.get_async_batches_submitted()
	var tag_pkg: int = PreloaderScript.prewarm_async(PackedStringArray(set_pkg))
	_check(tag_pkg > 0, "prewarm_async returns nonzero tag (%d)" % tag_pkg)
	_check(ctx.get_prewarm_progress(tag_pkg) == Vector2i(0, SET_N),
		"progress starts at (0, %d)" % SET_N)
	_check(await _pump_until(ctx, cache, tag_pkg), "archive prewarm converges")
	_verify_set(ctx, cache, set_pkg, tag_pkg, gpu, "archive")
	var submitted_delta: int = int(ctx.get_async_batches_submitted()) - submitted_before
	_check(submitted_delta >= 1 and submitted_delta <= BATCHING_SLACK,
		"batching: %d textures in %d submit(s)" % [SET_N, submitted_delta])
	_check(ctx.get_in_flight_async_batch_count() == 0, "no batches in flight at rest")
	_check(cache.get_pending_prewarm_count(tag_pkg) == 0, "no pending prewarm textures")

	# --- Phase 3: prewarm via the .import remap leg.
	_log("--- Phase 3: import-leg prewarm ---")
	var tag_imp: int = PreloaderScript.prewarm_async(PackedStringArray(set_imp))
	_check(tag_imp > 0 and tag_imp != tag_pkg, "second prewarm issues a fresh tag (%d)" % tag_imp)
	_check(await _pump_until(ctx, cache, tag_imp), "import prewarm converges")
	_verify_set(ctx, cache, set_imp, tag_imp, gpu, "import")

	# --- Phase 4: await helper with progress callback (drives flush in callback).
	_log("--- Phase 4: await_prewarm_finished ---")
	var set_cb: Array[String] = []
	for i in 2:
		var dstex := PROBE_DIR + "cb_t%02d.dstex" % i
		var png := PROBE_DIR + "cb_t%02d.png" % i
		_write_dstex(dstex, payload)
		_write_import(png, dstex)
		set_cb.append(png)
	var tag_cb: int = PreloaderScript.prewarm_async(PackedStringArray(set_cb))
	var seen: Array = []
	var cb := func(done: int, total: int) -> void:
		seen.append(Vector2i(done, total))
		if ctx.has_stream_pending():
			ctx.flush_stream_async()
		if ctx.is_batch_complete() and ctx.has_pending_results():
			ctx.collect_completed_results()
		cache.poll_completed()
	await PreloaderScript.await_prewarm_finished(tag_cb, cb)
	_check(ctx.is_prewarm_complete(tag_cb), "await_prewarm_finished returns on completion")
	_check(not seen.is_empty() and seen[seen.size() - 1] == Vector2i(2, 2),
		"progress callback reaches (2, 2)")
	_verify_set(ctx, cache, set_cb, tag_cb, gpu, "callback")

	# --- Phase 5: cancel semantics.
	_log("--- Phase 5: cancel ---")
	var set_cancel: Array[String] = []
	for i in 2:
		var dstex := PROBE_DIR + "cx_t%02d.dstex" % i
		var png := PROBE_DIR + "cx_t%02d.png" % i
		_write_dstex(dstex, payload)
		_write_import(png, dstex)
		set_cancel.append(png)
	var tag_cx: int = PreloaderScript.prewarm_async(PackedStringArray(set_cancel))
	ctx.cancel_prewarm(tag_cx)
	_check(await _pump_until(ctx, cache, tag_cx), "cancelled tag converges without hanging")
	ctx.cancel_prewarm(999999999)
	_check(ctx.is_prewarm_complete(999999999), "unknown tag reports complete / cancel is safe")
	PreloaderScript.cancel_prewarm(tag_cx)

	# --- Phase 6: sync regression (stream machinery present).
	_log("--- Phase 6: sync regression ---")
	var sync_tex: Texture2D = ResourceLoader.load(_staged[0], "Texture2D", ResourceLoader.CACHE_MODE_IGNORE)
	_check(sync_tex != null, "sync .dstex load still works")
	if sync_tex != null:
		_check(sync_tex.get_width() == TEX_W and sync_tex.get_height() == TEX_H, "sync texture dimensions intact")
		if not gpu:
			_check(sync_tex is ImageTexture, "headless sync load takes the CPU ImageTexture path")

	ctx.shutdown_ds()
	_log("shutdown clean")

# Pumps one tag the way DSRuntime._process does; true when complete.
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
			_fail("tag %d did not converge in %d frames" % [tag, MAX_PUMP_FRAMES])
			return false
		await process_frame
	# Drain one last harvest so counters settle.
	if ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()
	cache.poll_completed()
	return true

func _verify_set(ctx: Object, cache: Object, paths: Array, tag: int, gpu: bool, label: String) -> void:
	_check(ctx.get_prewarm_progress(tag) == Vector2i(paths.size(), paths.size()),
		"[%s] progress reaches (%d, %d)" % [label, paths.size(), paths.size()])
	for p in paths:
		var tex: Texture2D = cache.get_cached(p, 1)
		_check(tex != null, "[%s] cached: %s" % [label, p])
		if tex != null:
			_check(tex.get_width() == TEX_W and tex.get_height() == TEX_H,
				"[%s] dimensions intact: %s" % [label, p])
			if not gpu:
				_check(tex is ImageTexture, "[%s] headless build is ImageTexture: %s" % [label, p])
			var again: Texture2D = cache.get_cached(p, 1)
			_check(again == tex, "[%s] REUSE bind returns the resident instance" % label)

func _make_payload() -> PackedByteArray:
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

func _write_dstex(dstex_res: String, payload: PackedByteArray) -> void:
	var dstex := DSTex.new()
	var err: Error = dstex.write_to_file(dstex_res, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, 1, 1)
	_check(err == OK, "staged " + dstex_res)
	if err == OK:
		_staged.append(dstex_res)

func _write_dspkg(engine_paths: Array, dstex_paths: Array) -> void:
	var dspkg := DSPkg.new()
	_check(dspkg.create(ProjectSettings.globalize_path(DSPKG_PATH)) == OK, "archive created")
	for i in engine_paths.size():
		_check(dspkg.add_texture(engine_paths[i], dstex_paths[i]) == OK, "archived " + str(engine_paths[i]))
	_check(dspkg.finalize() == OK, "archive finalized (%d entries)" % engine_paths.size())
	_created_dspkg = true

func _write_import(png_res: String, dstex_res: String) -> void:
	var img := Image.create(TEX_W, TEX_H, false, Image.FORMAT_RGBA8)
	_check(img.save_png(ProjectSettings.globalize_path(png_res)) == OK, "staged " + png_res)
	var imp := FileAccess.open(png_res + ".import", FileAccess.WRITE)
	if imp == null:
		_fail("cannot write " + png_res + ".import")
		return
	imp.store_string("[remap]\n\npath=\"" + dstex_res + "\"\n")
	imp.close()
	_staged.append(png_res)
	_staged.append(png_res + ".import")

func _write_import_set(engine_paths: Array, dstex_paths: Array) -> void:
	for i in engine_paths.size():
		_write_import(engine_paths[i], dstex_paths[i])

# Tiny scene referencing the first import-leg texture (dependency walk target).
func _write_probe_scene(engine_paths: Array) -> void:
	var scene_path := PROBE_DIR + "probe_scene.tscn"
	var text: String = "[gd_scene load_steps=2 format=3]\n\n[ext_resource type=\"Texture2D\" path=\"" + engine_paths[0] + "\" id=\"1_tex\"]\n\n[node name=\"Probe\" type=\"Node\"]\n"
	var f := FileAccess.open(scene_path, FileAccess.WRITE)
	if f == null:
		_fail("cannot write " + scene_path)
		return
	f.store_string(text)
	f.close()
	_staged.append(scene_path)

func _finish(code: int) -> void:
	_done = true
	print("[%s] --- cleanup ---" % TAG)
	_restore_archive_resolution()
	if _created_dspkg:
		# This test streams out of res://textures.dspkg, so the archive is open in
		# the backend's IDStorageFile cache: shut the context down before deleting
		# it, or Windows refuses the delete and the stale archive breaks the next
		# run's DSPkg.finalize() ("Could not open file for writing").
		_shutdown_ds()
		_rm(DSPKG_PATH)
		if FileAccess.file_exists(DSPKG_PATH):
			_fail("archive survived cleanup: " + DSPKG_PATH)
		_created_dspkg = false
	for p in _staged:
		_rm(p)
	_staged.clear()
	var abs_dir := ProjectSettings.globalize_path(PROBE_DIR)
	if DirAccess.dir_exists_absolute(abs_dir):
		if DirAccess.get_files_at(PROBE_DIR).is_empty():
			DirAccess.remove_absolute(abs_dir)
			print("[%s] Removed dir %s" % [TAG, PROBE_DIR])
	print("[%s] Result: %s" % [TAG, "PASSED" if _failures == 0 else ("FAILED (%d)" % _failures)])
	# Cleanup runs after the caller computed `code`, so a failure found here (stale
	# archive) must still turn into a non-zero exit.
	quit(code if _failures == 0 else maxi(code, 1))

func _shutdown_ds() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx != null and ctx.is_initialized():
		ctx.shutdown_ds()

func _rm(res_path: String) -> void:
	var abs_path := ProjectSettings.globalize_path(res_path)
	if FileAccess.file_exists(abs_path):
		if DirAccess.remove_absolute(abs_path) != OK:
			printerr("[%s] cleanup missed: %s" % [TAG, res_path])
