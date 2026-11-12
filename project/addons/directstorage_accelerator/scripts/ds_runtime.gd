extends Node

const RUNTIME_INSTALLER_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_runtime_installer.gd"

## Shared completion predicate. A tracked prewarm tag is only finished once its
## request stream is terminal AND every completed texture has been built into
## DSTextureCache; `DSPreloader.is_prewarm_complete()` is the documented promise
## (`ds_preloader.gd`), so the autoload's signal is derived from it instead of
## re-implementing half of it.
const PRELOADER_SCRIPT := preload("uid://h8ts55gju2q4")

## DirectStorage Runtime Coordinator & Watchdog Autoload
##
## Author: MediumEffort
## Description: Owns the DirectStorage startup watchdog (native binary and asset
## archive integrity checks), drives the async texture prewarm queue once per frame
## without blocking the main thread, and reports per-tag progress.
##
## - `_process` only submits the prewarm queue via `flush_stream_async()`
##   (non-blocking: when the backend is busy the queue is kept for retry
##   next frame). It never blocks waiting for completion, so regular
##   synchronous loads are unaffected.
## - Finished batches are collected without blocking, and completed prewarm
##   textures are built into usable textures via
##   `DSTextureCache.poll_completed()`.
## - Per-tag progress surfaces as `prewarm_progress(tag, done, total)` /
##   `prewarm_finished(tag)`, polled from `get_prewarm_progress(tag)` /
##   `is_prewarm_complete(tag)`. Tags are tracked via `track_prewarm(tag)`
##   (call after `prewarm_async()` returns a tag); untracked tags still load
##   in the background, they just emit no progress signals.
## - When the extension is missing or uninitialized, `_process` does nothing,
##   so regular `ResourceLoader.load()` calls keep working untouched.
## - Exported builds print one `DSPKG_BOOT` verdict line: a failure line on
##   every boot, the success line only when the boot was asked to validate or
##   to be verbose. The release gate reads that line instead of the exit code.

## Machine-readable boot verdict. A failure prints on every boot; the success
## line prints only in validate/verbose mode so a normal boot stays silent.
const BOOT_SENTINEL: String = "DSPKG_BOOT"
## Command-line switch that turns on the success line and pins the expected
## archive mode. It only changes verbosity and the reported mode: the normal
## boot path runs unchanged.
const BOOT_VALIDATE_ARG: String = "--dspkg-validate="
const BOOT_MODE_PROTECTED: String = "protected"
const BOOT_MODE_UNPROTECTED: String = "unprotected"
## Static bridge to the native key bindings. Loaded through `load()` only after
## `ClassDB` reports the class, so this script parses without the native binary.
const DSPKG_STATIC_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_dspkg_static.gd"

## Guards the success line so it is printed at most once per process. CI treats
## a duplicate `status=ok` line as a failure, so a second boot hook must stay
## silent rather than repeat the verdict.
static var _boot_ok_reported: bool = false

## Exit code recorded by `_fail_boot`, re-applied at teardown. A failure verdict
## is a `quit(code)` during the autoload's `_ready()`, and the game's own main
## scene can still call `quit(0)` afterwards: `SceneTree.quit()` only sets the
## process exit code, so the last caller wins and the failure code is erased.
## Re-applying it from `_exit_tree` makes the code stick without changing the
## diagnostic contract - later nodes still run, only the code is protected.
## Negative means no failure was reported.
var _boot_failure_code: int = -1

## Per-tag prewarm progress: (tag: int, done: int, total: int).
signal prewarm_progress(tag: int, done: int, total: int)
## Fired once per tracked tag when `DSPreloader.is_prewarm_complete(tag)` turns
## true: every stream request is terminal AND every completed texture has been
## built into `DSTextureCache` (an offloaded build may outlive request completion).
signal prewarm_finished(tag: int)

## Tags under progress watch: tag (int) -> last emitted Vector2i(done, total).
var _tracked_tags: Dictionary = {}
## Tags already reported finished (prevents duplicate `prewarm_finished`).
var _finished_tags: Dictionary = {}
## True while DSProfiler has a frame-time window open for the current load.
var _frame_window_open: bool = false
## Lazily resolved DSProfiler singleton + frame-window capability (probed once,
## compatible with older builds that lack the methods).
var _profiler: Object = null
var _profiler_probed: bool = false
var _has_frame_window: bool = false
## Set once the missing-stream-API warning has been emitted (avoids spamming).
var _stream_api_missing_warned: bool = false
## Set once the missing-prewarm-progress-API warning has been emitted (avoids spamming).
var _prewarm_api_missing_warned: bool = false
## Background texture-ingestion task (`WorkerThreadPool` task ID, -1 = idle).
## `poll_completed()` builds `ImageTexture` only (Quirks Sec25 worker-safe);
## evict-path RID frees defer via `call_on_render_thread` (Sec27).
var _poll_task_id: int = -1
## Cached DSTextureCache capability probe (avoids per-frame `has_method` /
## `call` reflection; probed once, compatible with older builds lacking
## `get_unbuilt_prewarm_count` / `poll_completed`).
var _cache_caps_probed: bool = false
var _has_get_unbuilt: bool = false
var _has_get_unbuilt_cpu: bool = false
var _has_poll: bool = false
var _has_pump_gpu: bool = false
## Adaptive slice backpressure
## EMA of main-thread frame time; slice budgets passed by value to the
## worker via bind() so no cross-thread mutable read occurs.
## `_baseline_ema_ms` tracks the device's natural menu-idle frame cost
## (updated only while nothing streams); slice thresholds derive from it
## so a 30 FPS iGPU doesn't pin to "heavy" while fast dGPUs keep the
## original absolute thresholds. Baseline intentionally survives
## `reset_slice_stats()` so Play inherits the menu calibration.
var _slice_ema_ms: float = 0.0
var _slice_ema_init: bool = false
var _baseline_ema_ms: float = 0.0
var _baseline_init: bool = false
var _smooth_streak: int = 2
## Backpressure path tracking: transition-only log + per-batch slice counts
## (no per-frame spam). Silent by default; Settings enables `_slice_debug`
## during loading benchmarks and prints the summary in its own profiler.
var _slice_label: String = "warm"
var _warm_slices: int = 0
var _mild_slices: int = 0
var _heavy_slices: int = 0
var _drain_reported: bool = true
var _slice_debug: bool = false


## Starts tracking progress for a prewarm tag returned by `prewarm_async()`.
## Tracked tags emit `prewarm_progress` on change and one `prewarm_finished`.
func track_prewarm(tag: int) -> void:
	if tag <= 0:
		return
	if _finished_tags.has(tag):
		return
	if not _tracked_tags.has(tag):
		_tracked_tags[tag] = Vector2i(-1, -1)


## Stops tracking a prewarm tag (no further progress/finished signals).
## The underlying requests keep flowing; only signal emission stops.
func untrack_prewarm(tag: int) -> void:
	_tracked_tags.erase(tag)
	_finished_tags.erase(tag)


## Returns true while a tag is under progress watch.
func is_tracking_prewarm(tag: int) -> bool:
	return _tracked_tags.has(tag)


## Enables/disables backpressure slice logging. Off by default so regular
## loads stay silent; the loading benchmark turns it on for the load only.
func set_slice_debug(enabled: bool) -> void:
	_slice_debug = enabled


## Clears per-batch slice counters (called at benchmark start).
## Baseline + EMA intentionally survive so Play inherits menu calibration.
func reset_slice_stats() -> void:
	_warm_slices = 0
	_mild_slices = 0
	_heavy_slices = 0
	_drain_reported = true
	_smooth_streak = 2


## Returns per-batch slice counts for the benchmark profiler.
func get_slice_stats() -> Dictionary:
	return {"warm": _warm_slices, "mild": _mild_slices, "heavy": _heavy_slices, "label": _slice_label}


func _enter_tree() -> void:
	# Provide an in-tree alias for legacy scripts querying /root/DSBatchManager:
	if name == "DSRuntime":
		var loop: MainLoop = Engine.get_main_loop()
		if loop is SceneTree:
			var root: Window = (loop as SceneTree).root
			if root and not root.has_node("DSBatchManager"):
				var alias := Node.new()
				alias.name = "DSBatchManager"
				alias.set_script(load("res://addons/directstorage_accelerator/scripts/ds_batch_manager.gd"))
				root.add_child.call_deferred(alias)


## Initializes [DSContext] and reports the selected backend when the autoload
## enters the scene tree.
func _ready() -> void:
	# If this is a secondary alias node while DSRuntime is active, skip duplicate init
	if name != "DSRuntime":
		var loop: MainLoop = Engine.get_main_loop()
		if loop is SceneTree:
			var root: Window = (loop as SceneTree).root
			if root and root.has_node("DSRuntime"):
				var primary: Node = root.get_node_or_null("DSRuntime")
				if primary != null and primary != self:
					return

	if ClassDB.class_exists("DSContext"):
		# Apply a staged runtime before the first DirectStorage call. No-op
		# outside the editor and when nothing is pending.
		if OS.has_feature("editor") and ResourceLoader.exists(RUNTIME_INSTALLER_PATH):
			var installer: Script = load(RUNTIME_INSTALLER_PATH)
			if installer and installer.has_method("apply_pending"):
				installer.apply_pending()
		var ctx = Engine.get_singleton("DSContext")
		if ctx:
			ctx.initialize_ds()
			print("[DirectStorage] Autoload initialized. Active Backend: ", ctx.get_active_backend_name())
	else:
		push_error("[DirectStorage] GDExtension ClassDB entry 'DSContext' not found. Ensure GDExtension library is built and deployed.")
		if not OS.has_feature("editor"):
			_fail_boot(
				"missing_extension",
				"A required game component is missing:\n" +
				"'directstorage_accelerator' library was not found.\n\n" +
				"Please verify game file integrity.",
				"Missing Component"
			)
			return

	# Verification of bundled texture archive when exported with bundle_dspkg.
	# Deliberately loader-independent (no C++ popup during resource loads):
	# the autoload owns the player-facing missing-archive and decryption UX at startup.
	var archive_entries: int = 0
	if not OS.has_feature("editor"):
		if bool(ProjectSettings.get_setting("dsgd_extension/bundle_dspkg", false)):
			var archive_name := String(ProjectSettings.get_setting("dsgd_extension/archive_name", "textures.dspkg"))
			if archive_name.is_empty():
				archive_name = "textures.dspkg"
			var exe_dir := OS.get_executable_path().get_base_dir()
			var candidates: Array[String] = ["res://" + archive_name]
			if archive_name != "textures.dspkg":
				candidates.append("res://textures.dspkg")
			if not exe_dir.is_empty():
				candidates.append(exe_dir.path_join(archive_name))
				if archive_name != "textures.dspkg":
					candidates.append(exe_dir.path_join("textures.dspkg"))
			var glob_path := ProjectSettings.globalize_path("res://" + archive_name)
			if not candidates.has(glob_path):
				candidates.append(glob_path)
			if archive_name != "textures.dspkg":
				var glob_default := ProjectSettings.globalize_path("res://textures.dspkg")
				if not candidates.has(glob_default):
					candidates.append(glob_default)

			var candidate_found := false
			var candidate_path := ""
			for candidate in candidates:
				if not candidate.is_empty() and FileAccess.file_exists(candidate):
					candidate_found = true
					candidate_path = candidate
					break

			if not candidate_found:
				var candidate_list := "'" + "', '".join(candidates) + "'"
				print("[DirectStorage] Archive '", archive_name, "' not found. Searched candidates: ", candidate_list, ".")
				var err_msg := "Missing required game asset archive: '" + archive_name + "'.\n\nPlease verify game file integrity or reinstall the game."
				push_error("[DirectStorage] FATAL: " + err_msg)
				_fail_boot("missing_archive", err_msg, "Missing Game Files")
				return
			if ClassDB.class_exists("DSPkg"):
				var probe: RefCounted = ClassDB.instantiate("DSPkg")
				if probe != null and probe.has_method("open"):
					# The native open() acquires the key from res://.dspkg/toc.key
					# itself; the runtime never supplies one. The archive id comes
					# from the export contract and rejects a mixed-export pairing.
					var archive_id: int = int(ProjectSettings.get_setting("dsgd_extension/archive_id", 0))
					var open_err: Error = probe.call("open", candidate_path, archive_id)
					if open_err != OK:
						var reason := "archive_unreadable"
						var title := "Corrupt Game Files"
						var open_msg := "Failed to open game asset archive: '" + archive_name + "' (" + error_string(open_err) + ").\n\nPlease verify game file integrity or reinstall the game."
						if open_err == ERR_UNAUTHORIZED:
							reason = "archive_unauthorized"
							title = "Decryption Error"
							open_msg = "Failed to decrypt game asset archive: '" + archive_name + "'.\n\nNo valid decryption key was found. Please verify game file integrity or reinstall the game."
						elif open_err == ERR_FILE_UNRECOGNIZED:
							reason = "archive_pairing_mismatch"
							title = "Mismatched Game Files"
							open_msg = "Game asset archive does not match this build: '" + archive_name + "'.\n\nThe archive and the game files come from different exports. Reinstall the game or re-export it."
						push_error("[DirectStorage] FATAL: " + open_msg)
						_fail_boot(reason, open_msg, title)
						return
					if probe.has_method("get_entry_count"):
						archive_entries = int(probe.call("get_entry_count"))
		else:
			print("[DirectStorage] Textures are packaged in PCK (bundle_dspkg disabled); operating in CPU decompression mode.")
		_report_boot_ok(archive_entries)


## Prints the machine-readable success verdict when the boot was asked to
## validate (`--dspkg-validate=<mode>` or `DSPKG_VALIDATE`) or to be verbose
## (`DSPKG_BOOT_VERBOSE=1`). A plain successful boot stays silent.
##
## Only exported players report: the editor has no bundled archive to verify,
## and an `ok` line there would describe a boot the release gate never ran.
## The static guard keeps the verdict to one line per process because CI
## treats a duplicate `status=ok` as a failure.
func _report_boot_ok(p_entry_count: int) -> void:
	if _requested_validate_mode().is_empty() and OS.get_environment("DSPKG_BOOT_VERBOSE") != "1":
		return
	if _boot_ok_reported:
		return
	_boot_ok_reported = true
	_emit_boot_line(
		"%s status=ok mode=%s entries=%d key_id=%s"
		% [BOOT_SENTINEL, _boot_mode(), p_entry_count, _boot_key_id()]
	)


## Returns the validate mode requested by the caller, or an empty string when
## the boot was not asked to validate. Only user arguments are scanned:
## `OS.get_cmdline_args()` is empty in an exported player, so the switch would
## never be seen there. `DSPKG_VALIDATE` is the environment fallback.
func _requested_validate_mode() -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(BOOT_VALIDATE_ARG):
			var arg_mode := arg.substr(BOOT_VALIDATE_ARG.length()).strip_edges()
			if _is_validate_mode(arg_mode):
				return arg_mode
	var env_mode := OS.get_environment("DSPKG_VALIDATE").strip_edges()
	if _is_validate_mode(env_mode):
		return env_mode
	return ""


## True for the two archive modes the sentinel can report.
func _is_validate_mode(mode: String) -> bool:
	return mode == BOOT_MODE_PROTECTED or mode == BOOT_MODE_UNPROTECTED


## Archive mode reported by the sentinel: the validate argument when present
## (the gate states the mode it expects), otherwise what the export published
## through `dsgd_extension/protected`.
func _boot_mode() -> String:
	var requested := _requested_validate_mode()
	if not requested.is_empty():
		return requested
	if bool(ProjectSettings.get_setting("dsgd_extension/protected", false)):
		return BOOT_MODE_PROTECTED
	return BOOT_MODE_UNPROTECTED


## Key id of the key committed while the archive was opened, or `none` for a
## plaintext archive (no key is involved, and a source build's baked key is not
## part of the shipped contract).
func _boot_key_id() -> String:
	if not bool(ProjectSettings.get_setting("dsgd_extension/protected", false)):
		return "none"
	if not ClassDB.class_exists("DSPkg"):
		return "none"
	var bridge := load(DSPKG_STATIC_PATH) as GDScript
	if bridge == null:
		return "none"
	var key_id := String(bridge.call("get_global_key_id"))
	return "none" if key_id.is_empty() else key_id


## Writes one sentinel line to stdout and, when `DSPKG_VALIDATE_OUT` is set, to
## that file as well. The line is emitted before any alert so a headless gate
## reads the verdict even when a dialog is about to block or the process quits.
func _emit_boot_line(line: String) -> void:
	print(line)
	var sink := OS.get_environment("DSPKG_VALIDATE_OUT").strip_edges()
	if sink.is_empty():
		return
	var file := FileAccess.open(sink, FileAccess.READ_WRITE)
	if file == null:
		file = FileAccess.open(sink, FileAccess.WRITE)
	if file == null:
		push_warning("[DirectStorage] Cannot append the boot sentinel to DSPKG_VALIDATE_OUT: " + sink)
		return
	file.seek_end()
	file.store_line(line)
	file.close()


## Reports an unrecoverable startup failure and exits.
##
## The sentinel line and the `DSPKG_VALIDATE_OUT` entry are written before any
## alert: CI reads them, and a dialog would only block. Headless runs must never
## call `OS.alert()`: it waits for a human to dismiss it (Godot quirk Q64), so
## CI and `--headless` runs would hang until an external timeout kills them.
## `DSPKG_NO_ALERT=1` forces that path even when a display is present.
##
## The verdict is diagnostic: this only quits with the code, it never pauses the
## tree or frees siblings, so later autoloads and the main scene still run and a
## later `quit(0)` can override the exit code. That override is why the code is
## recorded here and re-applied in `_exit_tree`.
func _fail_boot(reason: String, message: String, title: String, code: int = 1) -> void:
	_emit_boot_line("%s status=fail reason=%s mode=%s" % [BOOT_SENTINEL, reason, _boot_mode()])
	printerr("[DirectStorage] ", message.replace("\n", " "))
	if not _is_non_interactive():
		if get_tree() and get_tree().root:
			get_tree().root.hide()
		OS.alert(message, title)
	if get_tree():
		_boot_failure_code = code
		get_tree().quit(code)


## Re-applies the failure exit code after everything else has run. The engine
## reads the process code once at shutdown, so writing it here is the last word;
## see `_boot_failure_code` for why the first write does not survive.
func _exit_tree() -> void:
	if _boot_failure_code <= 0:
		return
	var loop: MainLoop = Engine.get_main_loop()
	if loop is SceneTree:
		(loop as SceneTree).quit(_boot_failure_code)


## True when no human is available to dismiss an `OS.alert()` dialog.
##
## A validation run is machine-driven by definition, so `--dspkg-validate=*`
## (or `DSPKG_VALIDATE`) counts as non-interactive even when a display server
## is present: the gate would otherwise block on a modal nobody dismisses.
func _is_non_interactive() -> bool:
	if OS.get_environment("DSPKG_NO_ALERT") == "1":
		return true
	if not _requested_validate_mode().is_empty():
		return true
	return DisplayServer.get_name() == "headless"


## Drives the async stream queue once per frame. Stream-only: never touches
## the sync loader queue (`flush_batch` / `flush_batch_async` are NEVER
## called here) and never blocks (`wait_for_completion` is NEVER called).
func _process(delta: float) -> void:
	# If this is a secondary alias node while DSRuntime is active, skip duplicate process
	if name != "DSRuntime":
		var loop: MainLoop = Engine.get_main_loop()
		if loop is SceneTree:
			var root: Window = (loop as SceneTree).root
			if root and root.has_node("DSRuntime"):
				var primary: Node = root.get_node_or_null("DSRuntime")
				if primary != null and primary != self:
					return

	if not ClassDB.class_exists("DSContext"):
		return
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.is_initialized():
		return
	# Compatibility guard: if the loaded extension build lacks the stream
	# API, there is nothing safe to submit; stay silent after one warning
	# so regular sync loads keep working.
	if not ctx.has_method("flush_stream_async") or not ctx.has_method("has_stream_pending"):
		if not _stream_api_missing_warned:
			_stream_api_missing_warned = true
			push_warning("[DirectStorage] DSRuntime: stream API (flush_stream_async/has_stream_pending) missing; async prewarm disabled, sync loads unaffected.")
		_poll_legacy_completions(ctx)
		return
	# 1. Harvest already-finished batches first (non-blocking). This frees
	#    batch slots so the stream flush below can submit in the same frame
	#    instead of hitting ERR_BUSY and waiting a frame.
	if ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()
	# 2. Stream-only flush (non-blocking; ERR_BUSY keeps the queue intact for
	#    retry next frame). Never routes through the sync flush path.
	if ctx.has_stream_pending():
		ctx.flush_stream_async()
	# 3. Build textures for terminal prewarm requests (always ImageTexture,
	#    pipeline-safe on worker threads per GODOT_QUIRKS Sec25). Ingestion
	#    runs on WorkerThreadPool in one bounded slice per task (Q13/Q41:
	#    usec is the real guard, items is a cap for small textures) so the
	#    Render Thread drains D3D12 alloc/uploads within budget; evict-path
	#    RID frees defer to the render thread internally (Sec27).
	if ClassDB.class_exists("DSTextureCache"):
		var cache = Engine.get_singleton("DSTextureCache")
		if cache != null:
			if not _cache_caps_probed:
				_has_get_unbuilt = cache.has_method("get_unbuilt_prewarm_count")
				_has_get_unbuilt_cpu = cache.has_method("get_unbuilt_cpu_prewarm_count")
				_has_poll = cache.has_method("poll_completed")
				_has_pump_gpu = cache.has_method("pump_gpu_prewarm")
				_cache_caps_probed = true
			# Phase-2 GPU prewarm: at most one render-thread chunk per
			# frame; no-op while idle, in flight, or without D3D12.
			# Compatible with older builds lacking the method.
			if _has_pump_gpu:
				cache.pump_gpu_prewarm()
			# Main-thread texture drain: Texture2DRD wrappers must be
			# instantiated on the main or render thread (GODOT_QUIRKS Sec02 & Sec25).
			# Fast (<2us per texture): drain completed GPU textures within budget.
			# Never gated on _poll_task_id so the main thread is never starved.
			if _has_poll:
				cache.poll_completed(0, 32, 2000)
			# Adaptive backpressure: EMA over clamped delta + 2-frame streak
			# gate (Rev1: immediate delta is noisy under the 120-148ms clamp).
			var frame_ms: float = delta * 1000.0
			if not _slice_ema_init:
				_slice_ema_ms = frame_ms
				_slice_ema_init = true
			else:
				_slice_ema_ms = 0.5 * frame_ms + 0.5 * _slice_ema_ms
			# Calibrate while idle (no tracked tags, nothing queued): slow
			# alpha so load hitches never leak into the baseline.
			if _tracked_tags.is_empty() and not ctx.has_stream_pending():
				if not _baseline_init:
					_baseline_ema_ms = frame_ms
					_baseline_init = true
				else:
					_baseline_ema_ms = 0.05 * frame_ms + 0.95 * _baseline_ema_ms
			# Device-relative thresholds: fast devices reproduce the original
			# absolute bands (45/20/10 ms); slow devices scale with their own
			# menu-idle baseline instead of thrashing mild<->heavy.
			var heavy_at: float = 45.0
			var mild_at: float = 20.0
			var warm_at: float = 10.0
			if _baseline_init:
				heavy_at = maxf(45.0, _baseline_ema_ms * 2.5)
				mild_at = maxf(20.0, _baseline_ema_ms * 1.5)
				warm_at = maxf(10.0, _baseline_ema_ms * 1.1)
			var slice_items: int = 3
			var slice_usec: int = 4000
			var slice_label: String = "mild"
			if _slice_ema_ms > heavy_at:
				slice_items = 1
				slice_usec = 4000
				slice_label = "heavy"
				_smooth_streak = 0
			elif _slice_ema_ms > mild_at:
				slice_items = 3
				slice_usec = 4000
				slice_label = "mild"
				_smooth_streak = 0
			else:
				_smooth_streak += 1
				if _smooth_streak >= 2:
					if _slice_ema_ms < warm_at:
						slice_items = 12
						slice_usec = 8000
					else:
						slice_items = 8
						slice_usec = 8000
					slice_label = "warm"
			if slice_label != _slice_label:
				if _slice_debug:
					print("[DirectStorage] DSRuntime slice: %s -> %s (ema %.1f ms)" % [_slice_label, slice_label, _slice_ema_ms])
				_slice_label = slice_label
			if _poll_task_id != -1 and WorkerThreadPool.is_task_completed(_poll_task_id):
				WorkerThreadPool.wait_for_task_completion(_poll_task_id)
				_poll_task_id = -1
			var has_cpu_unbuilt := false
			if _has_get_unbuilt_cpu:
				has_cpu_unbuilt = int(cache.get_unbuilt_cpu_prewarm_count(0)) > 0
			elif _has_get_unbuilt:
				has_cpu_unbuilt = int(cache.get_unbuilt_prewarm_count(0)) > 0
			if _poll_task_id == -1 and has_cpu_unbuilt:
				_poll_task_id = WorkerThreadPool.add_task(_background_poll_slice.bind(slice_items, slice_usec), false, "DS_PollCompleted")
				if slice_label == "heavy":
					_heavy_slices += 1
				elif slice_label == "mild":
					_mild_slices += 1
				else:
					_warm_slices += 1
				_drain_reported = false
			if not _drain_reported and _poll_task_id == -1 and _has_get_unbuilt and int(cache.get_unbuilt_prewarm_count(0)) == 0:
				if _slice_debug:
					print("[DirectStorage] DSRuntime prewarm drained: warm=%d mild=%d heavy=%d" % [_warm_slices, _mild_slices, _heavy_slices])
				_drain_reported = true
				# Counters intentionally NOT reset here; Settings resets them at
				# benchmark start and reads them in its end-of-load summary.
	# 4. Frame-time impact: sample every frame and bracket the streaming window
	# with DSProfiler, so the before/during means cover exactly the frames the
	# load occupies. Byte-throughput metrics cannot answer "did this hitch?".
	if not _profiler_probed:
		_profiler_probed = true
		if ClassDB.class_exists("DSProfiler"):
			_profiler = Engine.get_singleton("DSProfiler")
			_has_frame_window = _profiler != null and _profiler.has_method("record_frame") and _profiler.has_method("begin_load_window") and _profiler.has_method("end_load_window")
	if _has_frame_window:
		_profiler.record_frame(delta)
		var streaming_active: bool = not _tracked_tags.is_empty() or ctx.has_stream_pending()
		if streaming_active and not _frame_window_open:
			_frame_window_open = true
			_profiler.begin_load_window()
		elif not streaming_active and _frame_window_open:
			_frame_window_open = false
			_profiler.end_load_window()
	# 5. Emit per-tag progress / finished for tracked tags.
	_poll_prewarm_progress(ctx)


## Polls tracked tags and emits `prewarm_progress` / `prewarm_finished`.
## Completion is decided by `DSPreloader.is_prewarm_complete()`, so a tag whose
## requests are done but whose cache builds are still pending is not finished yet.
func _poll_prewarm_progress(ctx: Object) -> void:
	if _tracked_tags.is_empty():
		return
	if not ctx.has_method("get_prewarm_progress") or not ctx.has_method("is_prewarm_complete"):
		# A build without the prewarm API can never prove a tag finished, and a
		# tracked tag keeps `streaming_active` true, which would hold the
		# frame-time window open for the rest of the session. Progress is
		# unobservable here anyway, so drop the watch after one warning.
		if not _prewarm_api_missing_warned:
			_prewarm_api_missing_warned = true
			push_warning("[DirectStorage] DSRuntime: prewarm progress API missing; clearing the tag watch (progress cannot be polled).")
		_tracked_tags.clear()
		return
	for tag in _tracked_tags.keys():
		var progress: Vector2i = ctx.get_prewarm_progress(tag)
		var last: Vector2i = _tracked_tags[tag]
		if last.x >= 0 and progress.x < last.x:
			progress.x = last.x
		if last.y >= 0 and progress.y < last.y:
			progress.y = last.y
		if progress != last:
			_tracked_tags[tag] = progress
			prewarm_progress.emit(tag, progress.x, progress.y)
		if PRELOADER_SCRIPT.is_prewarm_complete(tag):
			_tracked_tags.erase(tag)
			_finished_tags[tag] = true
			prewarm_finished.emit(tag)


## Background texture ingestion: exactly one bounded slice per task.
## Runs on WorkerThreadPool; budgets arrive by value via bind() (no
## cross-thread mutable read). Safe because `build_texture()` produces
## `ImageTexture` only (Sec25) and `store()` evictions defer RID frees to
## the render thread (Sec27). Requires C++ `poll_completed` to stop on
## EITHER limit (items OR usec, Q41); usec is the real hitch guard.
func _background_poll_slice(p_items: int, p_usec: int) -> void:
	if not ClassDB.class_exists("DSTextureCache"):
		return
	var cache = Engine.get_singleton("DSTextureCache")
	if cache == null:
		return
	# One adaptive slice per frame lets the Render Thread absorb D3D12
	# texture allocations/uploads without stalling presentation:
	cache.poll_completed(0, p_items, p_usec)


## Collects finished batches without submitting anything new. Used when the
## stream API is unavailable so processes that poll for completion on their
## own still finish.
func _poll_legacy_completions(ctx: Object) -> void:
	if not ctx.has_method("is_batch_complete"):
		return
	if not ctx.has_method("has_pending_results"):
		return
	if not ctx.has_method("collect_completed_results"):
		return
	if ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()


## Shuts down [DSContext] before the autoload is deleted.
func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		if _poll_task_id != -1:
			WorkerThreadPool.wait_for_task_completion(_poll_task_id)
			_poll_task_id = -1
		if ClassDB.class_exists("DSContext"):
			var ctx = Engine.get_singleton("DSContext")
			if ctx and ctx.is_initialized():
				ctx.shutdown_ds()
