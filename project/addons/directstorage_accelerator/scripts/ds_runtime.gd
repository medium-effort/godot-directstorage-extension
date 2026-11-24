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
## Per-scene prewarm progress: (scene_tag: int, stage: String, progress: float).
## Emitted at most once per frame when `stage` changes or `progress` advances by >= 0.005.
signal scene_prewarm_progress(scene_tag: int, stage: String, progress: float)
## Fired once per `start_scene_prewarm()` tag when the chained texture prewarm
## + threaded scene load completes (`packed_scene` non-null) or is cancelled/fails (`null`).
signal scene_prewarm_finished(scene_tag: int, scene_path: String, packed_scene: PackedScene)

enum _ScenePrewarmStage {
	STAGE_TEXTURE_PREWARM,
	STAGE_THREADED_SCENE_LOAD,
	STAGE_CANCELLING_SCENE_LOAD,
}
const SCENE_PROGRESS_EPSILON: float = 0.005

## Tags under progress watch: tag (int) -> last emitted Vector2i(done, total).
var _tracked_tags: Dictionary = {}
## Tags already reported finished (prevents duplicate `prewarm_finished`).
var _finished_tags: Dictionary = {}
## Active scene prewarms keyed by scene_tag (int).
var _scene_prewarms: Dictionary = {}
## Completed PackedScene results waiting for take_scene_prewarm_result().
var _scene_prewarm_results: Dictionary = {}
## Finished/cancelled scene prewarm tags.
var _scene_prewarm_done: Dictionary = {}
var _next_scene_tag: int = 1
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
## Single-flight WorkerThreadPool task IDs for the two main-thread stalls in
## Mode 4 (Win32 DESTINATION_MEMORY) streaming; -1 = idle. Neither task ever
## touches RenderingServer / call_on_render_thread(), so both are safe to join
## from the main thread during teardown.
var _stream_flush_task_id: int = -1
var _cpu_pump_task_id: int = -1
## Probed once: older extension binaries lack has_stream_gpu_pending(), in
## which case stream flushes stay on the main thread.
var _stream_gpu_api_probed: bool = false
var _has_stream_gpu_pending: bool = false
## Cached DSTextureCache capability probe (avoids per-frame `has_method` /
## `call` reflection; probed once, compatible with older builds lacking
## `get_unbuilt_prewarm_count` / `poll_completed`).
var _cache_caps_probed: bool = false
var _has_get_unbuilt: bool = false
var _has_get_unbuilt_cpu: bool = false
var _has_get_unbuilt_gpu: bool = false
var _has_poll: bool = false
var _has_pump_gpu: bool = false
var _has_gpu_requests_in_flight: bool = false
var _has_set_background_pacing: bool = false
var _has_pump_cpu: bool = false
var _has_get_queued_cpu: bool = false
var _has_get_in_flight_cpu: bool = false
var _has_get_in_flight_cpu_bytes: bool = false
## Streaming profile: false = loading screen (max throughput), true = active
## gameplay background streaming (completion-gated GPU + adaptive headroom).
var _background_pacing: bool = false
var _cpu_poll_cooldown_frames: int = 0
var _gpu_pump_cooldown_frames: int = 0
var _cpu_pump_cooldown_frames: int = 0
var _gpu_window_chunks: int = 1
var _cpu_window_init: bool = false
var _cpu_window_items: int = 4
var _cpu_window_bytes: int = 4 * 1024 * 1024
const GPU_LOADING_PUMP_MAX_CHUNKS_PER_FRAME: int = 8
const GPU_LOADING_PUMP_BUDGET_USEC: int = 5000
## Background CPU pump ceilings. One dispatch is capped at 16 MiB so the
## Win32 backend pipelines across both batch slots (up to 32 MiB in flight)
## instead of stalling one slot and bursting a full 64 MiB window.
const CPU_BG_WINDOW_MAX_ITEMS: int = 32
const CPU_BG_WINDOW_MAX_BYTES: int = 32 * 1024 * 1024
const CPU_BG_PUMP_MAX_BYTES_PER_CALL: int = 16 * 1024 * 1024
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
## Frames between GPU prewarm chunk pumps, per slice label. `_process` is the
## only caller of `pump_gpu_prewarm()` (DSPreloader has no other pump), so
## "heavy" has to stay paced rather than stopped: a hard stop wedges the GPU
## chunk queue for as long as the frame cost stays above the heavy threshold,
## and every caller waiting on the tag (e.g.
## `DSPreloaderScript.await_prewarm_finished()`, which only pumps when the
## autoload is absent) then waits forever. 8 frames is a floor, not a target.
const GPU_PUMP_FRAME_STRIDE_HEAVY: int = 8
const GPU_PUMP_FRAME_STRIDE_MILD: int = 3
const GPU_FRAME_BUDGET_MILD_MS: float = 16.6
const GPU_FRAME_BUDGET_HEAVY_MS: float = 33.3
var _gpu_pump_frames_since: int = 0


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


## Starts a chained scene prewarm (Stage 1: DirectStorage texture prewarm,
## Stage 2: ResourceLoader.load_threaded_request with use_sub_threads = false
## and CACHE_MODE_REUSE). Returns a positive `scene_tag` or `0` on invalid input.
func start_scene_prewarm(scene_path: String, deps: PackedStringArray, chunk_size: int = 16, prefer_gpu: bool = true, max_chunk_bytes: int = 0) -> int:
	if scene_path.is_empty():
		push_warning("[DirectStorage] DSRuntime: start_scene_prewarm called with empty scene_path.")
		return 0
	var scene_tag: int = _next_scene_tag
	_next_scene_tag += 1
	var prep_state := { "tex_tag": 0 }
	var prep_task_id: int = -1
	if not deps.is_empty() and PRELOADER_SCRIPT.is_stream_prewarm_available():
		var use_gpu: bool = prefer_gpu and PRELOADER_SCRIPT.is_gpu_prewarm_available()
		var use_paced_cpu: bool = (not use_gpu) and _background_pacing
		var cache: Object = Engine.get_singleton("DSTextureCache") if ClassDB.class_exists("DSTextureCache") else null
		if cache != null:
			prep_task_id = WorkerThreadPool.add_task(func() -> void:
				var tag: int = 0
				if use_gpu and cache.has_method("prewarm_gpu_async"):
					if max_chunk_bytes > 0:
						tag = int(cache.call("prewarm_gpu_async", deps, chunk_size, prefer_gpu, max_chunk_bytes))
					else:
						tag = int(cache.call("prewarm_gpu_async", deps, chunk_size, prefer_gpu))
				elif use_paced_cpu and cache.has_method("prewarm_cpu_paced_async"):
					tag = int(cache.call("prewarm_cpu_paced_async", deps))
				elif cache.has_method("prewarm_async"):
					tag = int(cache.call("prewarm_async", deps))
				prep_state["tex_tag"] = tag
			, false, "DSScenePrewarmPrep")
	_scene_prewarms[scene_tag] = {
		"scene_path": scene_path,
		"tex_tag": 0,
		"prep_task_id": prep_task_id,
		"prep_state": prep_state,
		"stage": _ScenePrewarmStage.STAGE_TEXTURE_PREWARM,
		"stage_name": "textures",
		"last_stage": "",
		"last_progress": -1.0,
		"cancelled": false,
	}
	return scene_tag


## Cancels an active scene prewarm. During Stage 1, cancels the texture prewarm
## and never starts Stage 2. During Stage 2, transitions to a Q42 drain state
## until load_threaded_get_status() finishes, then releases the result cleanly.
func cancel_scene_prewarm(scene_tag: int) -> void:
	if not _scene_prewarms.has(scene_tag):
		return
	var entry: Dictionary = _scene_prewarms[scene_tag]
	entry["cancelled"] = true
	var stage: int = int(entry["stage"])
	if stage == _ScenePrewarmStage.STAGE_TEXTURE_PREWARM:
		var tex_tag: int = int(entry["tex_tag"])
		if tex_tag > 0:
			PRELOADER_SCRIPT.cancel_prewarm(tex_tag)
	elif stage == _ScenePrewarmStage.STAGE_THREADED_SCENE_LOAD:
		entry["stage"] = _ScenePrewarmStage.STAGE_CANCELLING_SCENE_LOAD
		entry["stage_name"] = "cancelling"


## Returns true when `scene_tag` is no longer active (completed, cancelled, or unknown).
func is_scene_prewarm_complete(scene_tag: int) -> bool:
	return not _scene_prewarms.has(scene_tag)


## Returns a snapshot dictionary for `scene_tag`.
func get_scene_prewarm_state(scene_tag: int) -> Dictionary:
	if _scene_prewarms.has(scene_tag):
		var entry: Dictionary = _scene_prewarms[scene_tag]
		return {
			"active": true,
			"complete": false,
			"stage": String(entry["stage_name"]),
			"progress": maxf(0.0, float(entry["last_progress"])),
			"cancelled": bool(entry["cancelled"]),
			"tex_tag": int(entry["tex_tag"]),
			"scene_path": String(entry["scene_path"]),
		}
	return {
		"active": false,
		"complete": true,
		"stage": "done" if _scene_prewarm_done.has(scene_tag) else "",
		"progress": 1.0 if _scene_prewarm_results.has(scene_tag) else 0.0,
		"cancelled": false,
		"tex_tag": 0,
		"scene_path": "",
	}


## Retrieves and clears the completed PackedScene for `scene_tag` so DSRuntime
## does not retain a strong reference after handoff.
func take_scene_prewarm_result(scene_tag: int) -> PackedScene:
	var packed: PackedScene = _scene_prewarm_results.get(scene_tag, null) as PackedScene
	_scene_prewarm_results.erase(scene_tag)
	return packed


## Enables/disables backpressure slice logging. Off by default so regular
## loads stay silent; the loading benchmark turns it on for the load only.
func set_slice_debug(enabled: bool) -> void:
	_slice_debug = enabled


## Enables/disables active-gameplay background streaming pacing.
## When false (default, loading screens), DSRuntime maximizes throughput;
## when true, DSRuntime gates GPU pumping on in-flight request completion
## and uses adaptive frame-headroom thresholds.
func set_background_pacing(enabled: bool) -> void:
	if _background_pacing == enabled:
		return
	_background_pacing = enabled
	_gpu_pump_frames_since = 0
	_cpu_poll_cooldown_frames = 0
	_gpu_pump_cooldown_frames = 0
	_gpu_window_chunks = 1
	_cpu_window_init = false
	_cpu_pump_cooldown_frames = 0
	if ClassDB.class_exists("DSTextureCache"):
		var cache = Engine.get_singleton("DSTextureCache")
		if cache != null and cache.has_method("set_background_pacing"):
			cache.set_background_pacing(enabled)


## Returns true when active-gameplay background pacing is enabled.
func is_background_pacing() -> bool:
	return _background_pacing


## Clears per-batch slice counters (called at benchmark start).
## Baseline + EMA intentionally survive so Play inherits menu calibration.
func reset_slice_stats() -> void:
	_warm_slices = 0
	_mild_slices = 0
	_heavy_slices = 0
	_drain_reported = true
	_smooth_streak = 2
	_gpu_pump_frames_since = 0
	_cpu_poll_cooldown_frames = 0
	_gpu_pump_cooldown_frames = 0
	_gpu_window_chunks = 1
	_cpu_window_init = false
	_cpu_pump_cooldown_frames = 0


func _is_frame_capped() -> bool:
	if Engine.max_fps > 0:
		return true
	if DisplayServer.get_name() != "headless":
		return DisplayServer.window_get_vsync_mode() != DisplayServer.VSYNC_DISABLED
	return false


func _get_target_frame_ms() -> float:
	if Engine.max_fps > 0:
		return 1000.0 / float(Engine.max_fps)
	if DisplayServer.get_name() != "headless":
		var hz: float = DisplayServer.screen_get_refresh_rate()
		if hz > 1.0:
			return 1000.0 / hz
	return GPU_FRAME_BUDGET_MILD_MS


## Resolves the effective GPU prewarm backpressure label, escalating
## immediately when a single frame exceeds 60 FPS / 30 FPS (or adaptive
## background) budgets so GPU chunk pumping backs off before the EMA catches up.
func _effective_gpu_slice_label(slice_label: String, frame_ms: float = 0.0, mild_budget_ms: float = GPU_FRAME_BUDGET_MILD_MS, heavy_budget_ms: float = GPU_FRAME_BUDGET_HEAVY_MS) -> String:
	if slice_label == "heavy" or frame_ms > heavy_budget_ms:
		return "heavy"
	if slice_label == "mild" or frame_ms > mild_budget_ms:
		return "mild"
	return "warm"


## Decides whether this frame pumps one GPU prewarm chunk and advances the
## pacing counter. Pure state arithmetic (no engine calls) so the cadence can be
## asserted without frame timing: "heavy" is paced, never stopped.
func _should_pump_gpu_now(slice_label: String, frame_ms: float = 0.0, mild_budget_ms: float = GPU_FRAME_BUDGET_MILD_MS, heavy_budget_ms: float = GPU_FRAME_BUDGET_HEAVY_MS) -> bool:
	var effective := _effective_gpu_slice_label(slice_label, frame_ms, mild_budget_ms, heavy_budget_ms)
	var stride: int = 1
	if effective == "heavy":
		stride = GPU_PUMP_FRAME_STRIDE_HEAVY
	elif effective == "mild":
		stride = GPU_PUMP_FRAME_STRIDE_MILD
	_gpu_pump_frames_since += 1
	if _gpu_pump_frames_since < stride:
		return false
	_gpu_pump_frames_since = 0
	return true


## CPU pump AIMD state step. No engine calls, so GUT can assert window
## transitions without frame timing. Backoff runs on every overshoot; additive
## increase requires did_pump so idle frames waiting on an in-flight Win32
## batch cannot ramp the window straight back to the ceiling.
func _step_cpu_aimd(p_slice_label: String, p_frame_ms: float, p_mild_at: float, p_heavy_at: float, p_cores: int, p_did_pump: bool) -> void:
	if p_slice_label == "heavy" or p_frame_ms > p_heavy_at:
		_cpu_window_items = maxi(1, _cpu_window_items / 2)
		_cpu_window_bytes = maxi(1024 * 1024, _cpu_window_bytes / 2)
		_cpu_pump_cooldown_frames = maxi(_cpu_pump_cooldown_frames, 1)
	elif p_slice_label == "mild" or p_frame_ms > p_mild_at:
		_cpu_window_items = maxi(2, int(float(_cpu_window_items) * 0.75))
		_cpu_window_bytes = maxi(2 * 1024 * 1024, int(float(_cpu_window_bytes) * 0.75))
	elif p_slice_label == "warm" and p_frame_ms <= p_mild_at and _smooth_streak >= 2 and p_did_pump:
		_cpu_window_items = mini(CPU_BG_WINDOW_MAX_ITEMS, _cpu_window_items + maxi(1, p_cores / 4))
		_cpu_window_bytes = mini(CPU_BG_WINDOW_MAX_BYTES, _cpu_window_bytes + 2 * 1024 * 1024)


func _record_slice_stat(label: String) -> void:
	if label == "heavy":
		_heavy_slices += 1
	elif label == "mild":
		_mild_slices += 1
	else:
		_warm_slices += 1
	_drain_reported = false


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
		_poll_scene_prewarms()
		return
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.is_initialized():
		_poll_scene_prewarms()
		return
	# Compatibility guard: if the loaded extension build lacks the stream
	# API, there is nothing safe to submit; stay silent after one warning
	# so regular sync loads keep working.
	if not ctx.has_method("flush_stream_async") or not ctx.has_method("has_stream_pending"):
		if not _stream_api_missing_warned:
			_stream_api_missing_warned = true
			push_warning("[DirectStorage] DSRuntime: stream API (flush_stream_async/has_stream_pending) missing; async prewarm disabled, sync loads unaffected.")
		_poll_legacy_completions(ctx)
		_poll_scene_prewarms()
		return
	if not _stream_gpu_api_probed:
		_stream_gpu_api_probed = true
		_has_stream_gpu_pending = ctx.has_method("has_stream_gpu_pending")
	# 1. Harvest already-finished batches first (non-blocking). This frees
	#    batch slots so the stream flush below can submit in the same frame
	#    instead of hitting ERR_BUSY and waiting a frame. A finished sibling must
	#    be reclaimed even while its sibling slot is still in flight (finding 1.1).
	if ctx.has_method("has_harvestable_results"):
		if ctx.has_harvestable_results():
			ctx.collect_completed_results()
	elif ctx.is_batch_complete() and ctx.has_pending_results():
		ctx.collect_completed_results()
	# 2. Stream-only flush (non-blocking; ERR_BUSY keeps the queue intact for
	#    retry next frame). Never routes through the sync flush path.
	#    Memory-only flushes go to one WorkerThreadPool task: Win32Backend
	#    now sizes destination buffers outside batch_mutex, so the worker-side
	#    resize cannot block main-thread result polling. GPU destinations stay
	#    synchronous - their render-thread waits must not be joined from the
	#    main thread at teardown.
	if _stream_flush_task_id != -1 and WorkerThreadPool.is_task_completed(_stream_flush_task_id):
		WorkerThreadPool.wait_for_task_completion(_stream_flush_task_id)
		_stream_flush_task_id = -1
	if ctx.has_stream_pending():
		if _stream_flush_task_id == -1:
			if _has_stream_gpu_pending and not bool(ctx.has_stream_gpu_pending()):
				_stream_flush_task_id = WorkerThreadPool.add_task(_background_stream_flush, false, "DS_StreamFlush")
			else:
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
				_has_get_unbuilt_gpu = cache.has_method("get_unbuilt_gpu_prewarm_count")
				_has_poll = cache.has_method("poll_completed")
				_has_pump_gpu = cache.has_method("pump_gpu_prewarm")
				_has_gpu_requests_in_flight = cache.has_method("has_gpu_requests_in_flight")
				_has_set_background_pacing = cache.has_method("set_background_pacing")
				_has_pump_cpu = cache.has_method("pump_cpu_prewarm")
				_has_get_queued_cpu = cache.has_method("get_queued_cpu_prewarm_count")
				_has_get_in_flight_cpu = cache.has_method("get_in_flight_cpu_prewarm_count")
				_has_get_in_flight_cpu_bytes = cache.has_method("get_in_flight_cpu_prewarm_bytes")
				if _has_set_background_pacing:
					cache.set_background_pacing(_background_pacing)
				_cache_caps_probed = true
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
			var gpu_mild_budget_ms: float = GPU_FRAME_BUDGET_MILD_MS
			var gpu_heavy_budget_ms: float = GPU_FRAME_BUDGET_HEAVY_MS
			if _background_pacing:
				var target_frame_ms: float = _get_target_frame_ms()
				if _is_frame_capped():
					# When max_fps or VSync is active, delta * 1000.0 includes
					# frame-pacer sleep (~target_frame_ms + OS timer jitter).
					# Trigger mild/heavy backoff only on actual overshoot above
					# target_frame_ms so smooth 60/120/144 Hz frames stay "warm".
					warm_at = maxf(target_frame_ms * 1.05, target_frame_ms + 1.0)
					mild_at = maxf(target_frame_ms * 1.10, target_frame_ms + 1.5)
					heavy_at = maxf(mild_at + 4.0, target_frame_ms * 1.35)
				else:
					if _baseline_init:
						mild_at = minf(target_frame_ms * 0.65, maxf(6.0, _baseline_ema_ms + 4.0))
					else:
						mild_at = target_frame_ms * 0.65
					heavy_at = maxf(mild_at + 4.0, target_frame_ms * 0.95)
					warm_at = maxf(4.0, mild_at * 0.75)
				gpu_mild_budget_ms = mild_at
				gpu_heavy_budget_ms = heavy_at
			elif _baseline_init:
				heavy_at = maxf(45.0, _baseline_ema_ms * 2.5)
				mild_at = maxf(20.0, _baseline_ema_ms * 1.5)
				warm_at = maxf(10.0, _baseline_ema_ms * 1.1)
			var slice_items: int = 2
			var slice_usec: int = 4000
			var slice_label: String = "mild"
			# Loading screens have no frame to protect: scale the slice width with
			# the CPU count. Background pacing keeps its fixed 2-3 item slices.
			var loading_items: int = clampi(OS.get_processor_count(), 6, 24)
			if _slice_ema_ms > heavy_at:
				slice_items = 1
				slice_usec = 4000
				slice_label = "heavy"
				_smooth_streak = 0
			elif _slice_ema_ms > mild_at:
				slice_items = 2
				slice_usec = 4000
				slice_label = "mild"
				_smooth_streak = 0
			else:
				_smooth_streak += 1
				if _smooth_streak >= 2:
					if _slice_ema_ms < warm_at:
						if _background_pacing:
							slice_items = 3
							slice_usec = 4000
						else:
							slice_items = loading_items
							slice_usec = 8000
					else:
						if _background_pacing:
							slice_items = 2
							slice_usec = 4000
						else:
							slice_items = maxi(3, loading_items / 2)
							slice_usec = 6000
					slice_label = "warm"
			if slice_label != _slice_label:
				if _slice_debug:
					print("[DirectStorage] DSRuntime slice: %s -> %s (ema %.1f ms)" % [_slice_label, slice_label, _slice_ema_ms])
				_slice_label = slice_label
			# Phase-2 GPU prewarm: in background mode, gate on GPU request
			# completion and adaptive frame headroom; in loading-screen mode,
			# pump multiple chunks per frame up to GPU_LOADING_PUMP_BUDGET_USEC.
			var has_gpu_unbuilt := false
			if _has_get_unbuilt_gpu:
				has_gpu_unbuilt = int(cache.get_unbuilt_gpu_prewarm_count(0)) > 0
			var gpu_slice_label := _effective_gpu_slice_label(slice_label, frame_ms, gpu_mild_budget_ms, gpu_heavy_budget_ms)
			if _has_pump_gpu:
				if _background_pacing:
					var gpu_in_flight := _has_gpu_requests_in_flight and bool(cache.has_gpu_requests_in_flight(0))
					if has_gpu_unbuilt:
						if gpu_slice_label == "heavy":
							_gpu_window_chunks = 1
							_gpu_pump_cooldown_frames = maxi(_gpu_pump_cooldown_frames, 1)
						elif gpu_slice_label == "mild":
							_gpu_window_chunks = maxi(1, _gpu_window_chunks - 1)
							if _is_frame_capped() and frame_ms > gpu_mild_budget_ms:
								_gpu_pump_cooldown_frames = maxi(_gpu_pump_cooldown_frames, 1)
						elif not gpu_in_flight and _smooth_streak >= 2 and frame_ms <= gpu_mild_budget_ms:
							_gpu_window_chunks = mini(4, _gpu_window_chunks + 1)
					if _gpu_pump_cooldown_frames > 0:
						_gpu_pump_cooldown_frames -= 1
					elif not gpu_in_flight:
						if gpu_slice_label == "warm":
							var bg_pump_start_usec: int = Time.get_ticks_usec()
							for _p in _gpu_window_chunks:
								cache.pump_gpu_prewarm()
								if Time.get_ticks_usec() - bg_pump_start_usec >= 2000:
									break
						elif _should_pump_gpu_now(slice_label, frame_ms, gpu_mild_budget_ms, gpu_heavy_budget_ms):
							cache.pump_gpu_prewarm()
				else:
					var pump_start_usec: int = Time.get_ticks_usec()
					for _p in GPU_LOADING_PUMP_MAX_CHUNKS_PER_FRAME:
						cache.pump_gpu_prewarm()
						if Time.get_ticks_usec() - pump_start_usec >= GPU_LOADING_PUMP_BUDGET_USEC:
							break
			# Adaptive AIMD CPU/RAM prewarm pump: scales up on warm frames /
			# high-core CPUs and backs off only when frame_ms or slice EMA
			# overshoots target_frame_ms.
			if _has_pump_cpu and _has_get_queued_cpu:
				var queued_cpu: int = int(cache.get_queued_cpu_prewarm_count(0))
				if queued_cpu > 0:
					if _cpu_pump_task_id != -1 and WorkerThreadPool.is_task_completed(_cpu_pump_task_id):
						WorkerThreadPool.wait_for_task_completion(_cpu_pump_task_id)
						_cpu_pump_task_id = -1
					if _background_pacing:
						var cores: int = maxi(2, OS.get_processor_count())
						if not _cpu_window_init:
							_cpu_window_items = clampi(cores / 2, 2, 16)
							_cpu_window_bytes = clampi(cores / 4, 1, 8) * 2 * 1024 * 1024
							_cpu_window_init = true
						# Probe headroom before AIMD changes the window. Additive
						# increase only happens when this frame actually pumps.
						var in_flight_items: int = int(cache.get_in_flight_cpu_prewarm_count(0)) if _has_get_in_flight_cpu else 0
						var in_flight_bytes: int = int(cache.get_in_flight_cpu_prewarm_bytes(0)) if _has_get_in_flight_cpu_bytes else 0
						var avail_items: int = _cpu_window_items - in_flight_items
						var avail_bytes: int = _cpu_window_bytes - in_flight_bytes
						var did_pump: bool = _cpu_pump_cooldown_frames == 0 and _cpu_pump_task_id == -1 and avail_items > 0 and avail_bytes > 0
						_step_cpu_aimd(slice_label, frame_ms, mild_at, heavy_at, cores, did_pump)
						if _cpu_pump_cooldown_frames > 0:
							_cpu_pump_cooldown_frames -= 1
						else:
							# Recompute after backoff and cap one dispatch to 16 MiB.
							avail_items = _cpu_window_items - in_flight_items
							avail_bytes = _cpu_window_bytes - in_flight_bytes
							if avail_items > 0 and avail_bytes > 0 and _cpu_pump_task_id == -1:
								var pump_bytes: int = mini(avail_bytes, CPU_BG_PUMP_MAX_BYTES_PER_CALL)
								_cpu_pump_task_id = WorkerThreadPool.add_task(_background_cpu_pump.bind(avail_items, pump_bytes), false, "DS_CpuPrewarmPump")
					else:
						if _cpu_pump_task_id == -1:
							_cpu_pump_task_id = WorkerThreadPool.add_task(_background_cpu_pump.bind(queued_cpu, 0), false, "DS_CpuPrewarmPump")
			# Main-thread texture drain: Texture2DRD wrappers must be
			# instantiated on the main or render thread (GODOT_QUIRKS Sec02 & Sec25).
			# Fast (<2us per texture): drain completed GPU textures within budget.
			# Never gated on _poll_task_id so the main thread is never starved.
			if _has_poll:
				cache.poll_completed(0, 32, 2000, true)
			var reaped_cpu_task := false
			if _poll_task_id != -1 and WorkerThreadPool.is_task_completed(_poll_task_id):
				WorkerThreadPool.wait_for_task_completion(_poll_task_id)
				_poll_task_id = -1
				reaped_cpu_task = true
			if _background_pacing and reaped_cpu_task:
				_cpu_poll_cooldown_frames = 1
			var has_cpu_unbuilt := false
			if _has_get_unbuilt_cpu:
				has_cpu_unbuilt = int(cache.get_unbuilt_cpu_prewarm_count(0)) > 0
			elif _has_get_unbuilt:
				has_cpu_unbuilt = int(cache.get_unbuilt_prewarm_count(0)) > 0
			var recorded_slice := false
			if _poll_task_id == -1 and has_cpu_unbuilt:
				if _background_pacing and _cpu_poll_cooldown_frames > 0:
					_cpu_poll_cooldown_frames -= 1
				elif DisplayServer.get_name() == "headless" or RenderingServer.get_current_rendering_driver_name().to_lower() == "dummy" or RenderingServer.get_current_rendering_method().to_lower() == "dummy":
					# RendererDummy::TextureStorage::texture_owner is not thread-safe on
					# Godot < 4.8 (godot#124139). Keep CPU builds inline on the main thread
					# (p_max_usec = 0 avoids parallel_first worker fan-out).
					cache.poll_completed(0, slice_items, 0)
					_record_slice_stat(slice_label)
					recorded_slice = true
				else:
					_poll_task_id = WorkerThreadPool.add_task(_background_poll_slice.bind(slice_items, slice_usec), false, "DS_PollCompleted")
					_record_slice_stat(slice_label)
					recorded_slice = true
			if not recorded_slice and has_gpu_unbuilt:
				_record_slice_stat(gpu_slice_label)
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
		var streaming_active: bool = not _tracked_tags.is_empty() or not _scene_prewarms.is_empty() or ctx.has_stream_pending()
		if streaming_active and not _frame_window_open:
			_frame_window_open = true
			_profiler.begin_load_window()
		elif not streaming_active and _frame_window_open:
			_frame_window_open = false
			_profiler.end_load_window()
	# 5. Emit per-tag progress / finished for tracked tags.
	_poll_prewarm_progress(ctx)
	# 6. Advance chained scene prewarms (Stage 1 textures -> Stage 2 threaded load).
	_poll_scene_prewarms()


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


func _emit_scene_prewarm_progress(scene_tag: int, entry: Dictionary, stage_name: String, progress: float) -> void:
	var clamped: float = clampf(progress, 0.0, 1.0)
	var last_stage: String = String(entry["last_stage"])
	var last_progress: float = float(entry["last_progress"])
	if stage_name != last_stage or absf(clamped - last_progress) >= SCENE_PROGRESS_EPSILON:
		entry["last_stage"] = stage_name
		entry["last_progress"] = clamped
		scene_prewarm_progress.emit(scene_tag, stage_name, clamped)


func _finish_scene_prewarm(scene_tag: int, scene_path: String, packed: PackedScene) -> void:
	if packed != null:
		_scene_prewarm_results[scene_tag] = packed
	_scene_prewarm_done[scene_tag] = true
	_scene_prewarms.erase(scene_tag)
	scene_prewarm_finished.emit(scene_tag, scene_path, packed)


## Advances active scene prewarms: Stage 1 (DirectStorage texture prewarm)
## waits for `is_prewarm_complete(tex_tag)` before starting Stage 2
## (`ResourceLoader.load_threaded_request` with `use_sub_threads = false` per Q40
## and `CACHE_MODE_REUSE` per Q14). Stage 2 cancellations drain via Q42.
func _poll_scene_prewarms() -> void:
	if _scene_prewarms.is_empty():
		return
	for scene_tag in _scene_prewarms.keys():
		if not _scene_prewarms.has(scene_tag):
			continue
		var entry: Dictionary = _scene_prewarms[scene_tag]
		var scene_path: String = String(entry["scene_path"])
		var stage: int = int(entry["stage"])
		if stage == _ScenePrewarmStage.STAGE_TEXTURE_PREWARM:
			var prep_task_id: int = int(entry.get("prep_task_id", -1))
			if prep_task_id != -1:
				if not WorkerThreadPool.is_task_completed(prep_task_id):
					continue
				WorkerThreadPool.wait_for_task_completion(prep_task_id)
				entry["prep_task_id"] = -1
				var prep_state: Dictionary = entry.get("prep_state", {})
				var resolved_tag: int = int(prep_state.get("tex_tag", 0))
				entry["tex_tag"] = resolved_tag
				if resolved_tag > 0:
					track_prewarm(resolved_tag)
					if bool(entry["cancelled"]):
						PRELOADER_SCRIPT.cancel_prewarm(resolved_tag)
			var tex_tag: int = int(entry["tex_tag"])
			if bool(entry["cancelled"]):
				if tex_tag <= 0 or PRELOADER_SCRIPT.is_prewarm_complete(tex_tag):
					_finish_scene_prewarm(scene_tag, scene_path, null)
				continue
			var frac: float = 0.0
			if tex_tag > 0:
				var tex_prog: Vector2i = PRELOADER_SCRIPT.get_prewarm_progress(tex_tag)
				if tex_prog.y > 0:
					frac = clampf(float(tex_prog.x) / float(tex_prog.y), 0.0, 1.0)
			_emit_scene_prewarm_progress(scene_tag, entry, "textures", frac * 0.5)
			if tex_tag <= 0 or PRELOADER_SCRIPT.is_prewarm_complete(tex_tag):
				var err: Error = ResourceLoader.load_threaded_request(scene_path, "PackedScene", false, ResourceLoader.CACHE_MODE_REUSE)
				if err != OK:
					push_warning("[DirectStorage] DSRuntime: load_threaded_request failed for '", scene_path, "' (err=", err, ").")
					_finish_scene_prewarm(scene_tag, scene_path, null)
					continue
				entry["stage"] = _ScenePrewarmStage.STAGE_THREADED_SCENE_LOAD
				entry["stage_name"] = "scene"
				_emit_scene_prewarm_progress(scene_tag, entry, "scene", 0.5)
		elif stage == _ScenePrewarmStage.STAGE_THREADED_SCENE_LOAD:
			var progress_arr: Array = []
			var status: ResourceLoader.ThreadLoadStatus = ResourceLoader.load_threaded_get_status(scene_path, progress_arr)
			if status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
				var p: float = float(progress_arr[0]) if not progress_arr.is_empty() else 0.0
				_emit_scene_prewarm_progress(scene_tag, entry, "scene", 0.5 + clampf(p, 0.0, 1.0) * 0.5)
			elif status == ResourceLoader.THREAD_LOAD_LOADED:
				var packed: PackedScene = ResourceLoader.load_threaded_get(scene_path) as PackedScene
				_emit_scene_prewarm_progress(scene_tag, entry, "done", 1.0)
				_finish_scene_prewarm(scene_tag, scene_path, packed)
			else:
				push_warning("[DirectStorage] DSRuntime: threaded scene load failed for '", scene_path, "' (status=", status, ").")
				_finish_scene_prewarm(scene_tag, scene_path, null)
		elif stage == _ScenePrewarmStage.STAGE_CANCELLING_SCENE_LOAD:
			var status: ResourceLoader.ThreadLoadStatus = ResourceLoader.load_threaded_get_status(scene_path)
			if status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
				var cur_prog: float = float(entry["last_progress"])
				_emit_scene_prewarm_progress(scene_tag, entry, "cancelling", cur_prog if cur_prog >= 0.0 else 0.5)
			else:
				if status == ResourceLoader.THREAD_LOAD_LOADED:
					var discarded: Resource = ResourceLoader.load_threaded_get(scene_path)
					discarded = null
				_finish_scene_prewarm(scene_tag, scene_path, null)


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


## Background stream flush for memory-only batches. Deliberately touches no
## RenderingServer / RenderingDevice API: GPU destinations stay on the main
## thread, so joining this task during teardown cannot deadlock.
func _background_stream_flush() -> void:
	if not ClassDB.class_exists("DSContext"):
		return
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.is_initialized():
		return
	if not ctx.has_method("flush_stream_async"):
		return
	ctx.flush_stream_async()


## Background CPU prewarm pump. Only stages requests; all texture building
## and GPU/render-thread work stays on the main-thread ingestion path.
func _background_cpu_pump(p_items: int, p_bytes: int) -> void:
	if not ClassDB.class_exists("DSTextureCache"):
		return
	var cache = Engine.get_singleton("DSTextureCache")
	if cache == null or not cache.has_method("pump_cpu_prewarm"):
		return
	cache.pump_cpu_prewarm(p_items, p_bytes)


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
		for scene_tag in _scene_prewarms.keys():
			var entry: Dictionary = _scene_prewarms[scene_tag]
			var prep_id: int = int(entry.get("prep_task_id", -1))
			if prep_id != -1:
				WorkerThreadPool.wait_for_task_completion(prep_id)
				entry["prep_task_id"] = -1
		if _poll_task_id != -1:
			WorkerThreadPool.wait_for_task_completion(_poll_task_id)
			_poll_task_id = -1
		# Both offload tasks are memory-only: no render-thread wait inside
		# them, so joining here cannot deadlock single-threaded rendering.
		if _cpu_pump_task_id != -1:
			WorkerThreadPool.wait_for_task_completion(_cpu_pump_task_id)
			_cpu_pump_task_id = -1
		if _stream_flush_task_id != -1:
			WorkerThreadPool.wait_for_task_completion(_stream_flush_task_id)
			_stream_flush_task_id = -1
		if ClassDB.class_exists("DSContext"):
			var ctx = Engine.get_singleton("DSContext")
			if ctx and ctx.is_initialized():
				ctx.shutdown_ds()
