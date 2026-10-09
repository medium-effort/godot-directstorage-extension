extends Node

## DirectStorage Per-Frame Stream Batch Manager Autoload
##
## Author: MediumEffort
## Description: Drives the async texture prewarm queue once per frame without
## blocking the main thread, then reports per-tag progress.
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

## Per-tag prewarm progress: (tag: int, done: int, total: int).
signal prewarm_progress(tag: int, done: int, total: int)
## Fired once per tracked tag when `is_prewarm_complete(tag)` turns true.
signal prewarm_finished(tag: int)

## Tags under progress watch: tag (int) -> last emitted Vector2i(done, total).
var _tracked_tags: Dictionary = {}
## Tags already reported finished (prevents duplicate `prewarm_finished`).
var _finished_tags: Dictionary = {}
## Set once the missing-stream-API warning has been emitted (avoids spamming).
var _stream_api_missing_warned: bool = false
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
## Adaptive slice backpressure (Rev1 plan: Q13/Q41).
## EMA of main-thread frame time; slice budgets passed by value to the
## worker via bind() so no cross-thread mutable read occurs.
var _slice_ema_ms: float = 0.0
var _slice_ema_init: bool = false
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
func reset_slice_stats() -> void:
	_warm_slices = 0
	_mild_slices = 0
	_heavy_slices = 0
	_drain_reported = true
	_smooth_streak = 2


## Returns per-batch slice counts for the benchmark profiler.
func get_slice_stats() -> Dictionary:
	return {"warm": _warm_slices, "mild": _mild_slices, "heavy": _heavy_slices, "label": _slice_label}


## Initializes [DSContext] and reports the selected backend when the autoload
## enters the scene tree.
func _ready() -> void:
	if ClassDB.class_exists("DSContext"):
		var ctx = Engine.get_singleton("DSContext")
		if ctx:
			ctx.initialize_ds()
			print("[DirectStorage] Autoload initialized. Active Backend: ", ctx.get_active_backend_name())
	else:
		push_warning("[DirectStorage] GDExtension ClassDB entry 'DSContext' not found. Ensure GDExtension library built.")


## Drives the async stream queue once per frame. Stream-only: never touches
## the sync loader queue (`flush_batch` / `flush_batch_async` are NEVER
## called here) and never blocks (`wait_for_completion` is NEVER called).
func _process(delta: float) -> void:
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
			push_warning("[DirectStorage] DSBatchManager: stream API (flush_stream_async/has_stream_pending) missing; async prewarm disabled, sync loads unaffected.")
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
			var slice_items: int = 3
			var slice_usec: int = 4000
			var slice_label: String = "mild"
			if _slice_ema_ms > 45.0:
				slice_items = 1
				slice_usec = 4000
				slice_label = "heavy"
				_smooth_streak = 0
			elif _slice_ema_ms > 20.0:
				slice_items = 3
				slice_usec = 4000
				slice_label = "mild"
				_smooth_streak = 0
			else:
				_smooth_streak += 1
				if _smooth_streak >= 2:
					if _slice_ema_ms < 10.0:
						slice_items = 12
						slice_usec = 8000
					else:
						slice_items = 8
						slice_usec = 8000
					slice_label = "warm"
			if slice_label != _slice_label:
				if _slice_debug:
					print("[DirectStorage] DSBatchManager slice: %s -> %s (ema %.1f ms)" % [_slice_label, slice_label, _slice_ema_ms])
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
					print("[DirectStorage] DSBatchManager prewarm drained: warm=%d mild=%d heavy=%d" % [_warm_slices, _mild_slices, _heavy_slices])
				_drain_reported = true
				# Counters intentionally NOT reset here; Settings resets them at
				# benchmark start and reads them in its end-of-load summary.
	# 4. Emit per-tag progress / finished for tracked tags.
	_poll_prewarm_progress(ctx)


## Polls tracked tags and emits `prewarm_progress` / `prewarm_finished`.
func _poll_prewarm_progress(ctx: Object) -> void:
	if _tracked_tags.is_empty():
		return
	if not ctx.has_method("get_prewarm_progress") or not ctx.has_method("is_prewarm_complete"):
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
		if ctx.is_prewarm_complete(tag):
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
