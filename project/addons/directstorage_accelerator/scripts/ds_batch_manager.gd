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
var _has_poll: bool = false


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
func _process(_delta: float) -> void:
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
	#    runs on WorkerThreadPool at full CPU speed so the main thread spends
	#    0.0 ms on texture creation; evict-path RID frees defer to the render
	#    thread internally (Sec27).
	if ClassDB.class_exists("DSTextureCache"):
		var cache = Engine.get_singleton("DSTextureCache")
		if cache != null:
			if not _cache_caps_probed:
				_has_get_unbuilt = cache.has_method("get_unbuilt_prewarm_count")
				_has_poll = cache.has_method("poll_completed")
				_cache_caps_probed = true
			if _poll_task_id != -1 and WorkerThreadPool.is_task_completed(_poll_task_id):
				WorkerThreadPool.wait_for_task_completion(_poll_task_id)
				_poll_task_id = -1
			if _poll_task_id == -1 and _has_get_unbuilt and int(cache.get_unbuilt_prewarm_count(0)) > 0:
				_poll_task_id = WorkerThreadPool.add_task(_background_poll_completed, false, "DS_PollCompleted")
			if _poll_task_id == -1 and _has_poll:
				cache.poll_completed(0, 0, 2000)
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
		if progress != last:
			_tracked_tags[tag] = progress
			prewarm_progress.emit(tag, progress.x, progress.y)
		if ctx.is_prewarm_complete(tag):
			_tracked_tags.erase(tag)
			_finished_tags[tag] = true
			prewarm_finished.emit(tag)


## Background texture ingestion: drains all terminal prewarm requests at
## full CPU speed without a microsecond budget. Runs on WorkerThreadPool;
## safe because `build_texture()` produces `ImageTexture` only (Sec25) and
## `store()` evictions defer RID frees to the render thread (Sec27).
func _background_poll_completed() -> void:
	if not ClassDB.class_exists("DSTextureCache"):
		return
	var cache = Engine.get_singleton("DSTextureCache")
	if cache == null:
		return
	# Caps probed on the main thread in _process before this task is spawned;
	# direct calls here avoid per-iteration has_method/call reflection.
	while int(cache.get_unbuilt_prewarm_count(0)) > 0:
		var built: int = int(cache.poll_completed(0, 64, 0))
		if built <= 0:
			break


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
