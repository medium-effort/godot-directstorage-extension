class_name DSPreloader
extends RefCounted

## DirectStorage Async Prewarm Helpers.
##
## Static GDScript surface over the native async streaming pipeline:
## [code]DSTextureCache.prewarm_gpu_async()[/code] (GPU DMA to VRAM) /
## [code]DSTextureCache.prewarm_async()[/code] (CPU memory streaming) +
## [code]DSRuntime[/code] autoload [code]_process[/code]
## (collect -> [code]flush_stream_async()[/code] -> [code]pump_gpu_prewarm()[/code] ->
## [code]poll_completed()[/code] -> per-tag progress) + [code]DSContext[/code] tag queries.
##
## Canonical flow (see [method collect_scene_textures] and
## [method load_scene_with_loading_screen]):
## [codeblock]
## var deps: PackedStringArray = DSPreloader.collect_scene_textures("res://levels/level1.tscn")
## var tag: int = DSPreloader.prewarm_gpu_async(deps)
## await DSPreloader.await_prewarm_finished(tag) # or poll get_prewarm_progress(tag) for a bar
## var packed := ResourceLoader.load("res://levels/level1.tscn", "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
## [/codeblock]
## Requirements:
## - Pass engine authoring paths only ([code].png/.jpg/.jpeg/.webp[/code]); the
##   [code].import[/code] remap and the [code]textures.dspkg[/code] lookup are
##   resolved internally. Never pass a [code].dstex[/code] path here.
## - Pass [code]CACHE_MODE_REUSE[/code] when loading the scene after a
##   prewarm so the scene binds the prewarmed textures without reading
##   the disk again. [code]CACHE_MODE_IGNORE[/code] /
##   [code]CACHE_MODE_REPLACE[/code] skip the cache and waste the prewarm.
## - Keep the loading screen visible across [code]instantiate()[/code] +
##   [code]add_child()[/code] + one drawn frame so shader compilation
##   finishes before gameplay is revealed.
## - Everything here runs on the main thread. Scene-tree attachment
##   ([code]add_child()[/code]) is main-thread-only.

## Texture extensions accepted by the native preloader
## (mirrors [code]DSPreloader::is_engine_source_path[/code]).
const PREWARMABLE_EXTENSIONS: PackedStringArray = ["png", "jpg", "jpeg", "webp"]

## Extensions excluded as format-incompatible: EXR/HDR images need float
## formats the streaming pipeline does not produce.
const EXCLUDED_EXTENSIONS: PackedStringArray = ["exr", "hdr"]

## Resource extensions recursed into by [method collect_scene_textures] when
## walking [code]ResourceLoader.get_dependencies()[/code].
const TRAVERSABLE_EXTENSIONS: PackedStringArray = ["tscn", "scn", "tres", "res", "material", "godot", "glb", "gltf"]

## Default main-thread budget per frame for the sync fallback loop, leaving
## headroom for 60 FPS loading-screen rendering.
const FALLBACK_FRAME_BUDGET_USEC: int = 8000

## Recommended GPU prewarm chunk byte cap (4 MiB) for background streaming
## during active gameplay; keeps per-frame D3D12 allocation bursts bounded.
const BACKGROUND_GPU_CHUNK_BYTES: int = 4 * 1024 * 1024

## Name of the autoload that drives the async stream queue, and the legacy
## autoload older addon versions installed. One of them must exist in the tree
## or nothing ever calls [code]flush_stream_async()[/code] /
## [code]poll_completed()[/code].
const STREAM_PUMP_AUTOLOAD: String = "DSRuntime"
const STREAM_PUMP_LEGACY_AUTOLOAD: String = "DSBatchManager"

## One-shot guard for the "requests enqueued but no driver" diagnostic, so a
## loading screen that asks every frame logs the cause once instead of flooding.
static var _missing_pump_warned: bool = false


## Returns the per-frame stream driver node ([code]/root/DSRuntime[/code], or
## the legacy [code]/root/DSBatchManager[/code]), or [code]null[/code] when
## neither is in the tree or there is no [SceneTree] main loop.
static func _get_stream_pump() -> Node:
	var loop: MainLoop = Engine.get_main_loop()
	if not (loop is SceneTree):
		return null
	var root_node: Window = (loop as SceneTree).root
	if root_node == null:
		return null
	var pump: Node = root_node.get_node_or_null(STREAM_PUMP_AUTOLOAD)
	if pump == null:
		pump = root_node.get_node_or_null(STREAM_PUMP_LEGACY_AUTOLOAD)
	return pump


## True when a stream driver node is in the tree. The queue is only submitted
## and harvested from [code]_process()[/code] of that node, so the native
## capability flags alone do not make async prewarm usable.
static func _has_stream_pump() -> bool:
	return _get_stream_pump() != null


## One loud, once-per-process diagnostic for the state that used to look like a
## silent hang: the native pipeline reports itself available, but no autoload
## submits or harvests the stream queue. Callers see
## [method is_stream_prewarm_available] return [code]false[/code] and
## [method prewarm_async] / [method prewarm_gpu_async] return [code]0[/code].
static func _warn_missing_stream_pump() -> void:
	if _missing_pump_warned:
		return
	_missing_pump_warned = true
	push_error(
		"[DirectStorage] DSPreloader: async prewarm unavailable - neither /root/%s nor /root/%s exists, so nothing submits or harvests the stream queue (requests would sit there forever). Enable the dsgd plugin in Project Settings > Plugins so it registers the autoload, or add '%s' to autoload manually." % [STREAM_PUMP_AUTOLOAD, STREAM_PUMP_LEGACY_AUTOLOAD, STREAM_PUMP_AUTOLOAD]
	)


## Normalizes a raw dependency entry returned by [code]ResourceLoader.get_dependencies()[/code]
## into a clean [code]res://[/code] or [code]user://[/code] engine path.
##
## In Godot 4.x, dependencies are formatted as:
##   "uid://<id>::<type>::<fallback_path>"
##   "uid://<id>::::<fallback_path>"
##   "<path>::<type>"
##   "<path>"
##   "uid://<id>"
## If a raw UID is passed, it is resolved safely through [ResourceUID].
static func _resolve_dependency_path(raw_dep: String) -> String:
	var path: String = raw_dep.strip_edges()
	if path.is_empty():
		return ""

	if path.contains("::"):
		# Section 3 is fallback path per ResourceFormatLoader spec: uid::type::path or uid::::path
		var fallback: String = path.get_slice("::", 2).strip_edges()
		if not fallback.is_empty() and (fallback.begins_with("res://") or fallback.begins_with("user://")):
			return fallback
		# Section 1 if no valid fallback path in section 3 (e.g. "res://script.gd::Script")
		var first: String = path.get_slice("::", 0).strip_edges()
		path = first

	# If the path is a UID, safely resolve it via ResourceUID
	if path.begins_with("uid://"):
		var id: int = ResourceUID.text_to_id(path)
		if id != ResourceUID.INVALID_ID and ResourceUID.has_id(id):
			var resolved: String = ResourceUID.get_id_path(id)
			if not resolved.is_empty():
				return resolved

	return path


## Collects prewarmable texture paths for a scene: recursive
## [code]ResourceLoader.get_dependencies()[/code] walk plus a [code]DirAccess[/code]
## sweep of the scene's own directory and [param extra_scan_dirs] (catches
## dynamic [code]load()[/code] strings, unopened glTF sub-resources and
## conditional effect textures that dependency headers miss).
## Skips [code].exr/.hdr[/code] files and textures imported with
## [code]compatibility/as_image_texture[/code] (for example Light3D projector
## or Decal textures that need a CPU [code]ImageTexture[/code]).
## Returns a sorted, de-duplicated [PackedStringArray] of engine paths.
## Resolves the physical path of textures.dspkg in exported builds (exe dir).
## In editor mode, returns empty string so live imported assets are crawled instead.
static func _resolve_dspkg_path() -> String:
	var force_archive := bool(ProjectSettings.get_setting("dsgd_extension/force_archive_resolution", false))
	if Engine.is_editor_hint() or (OS.has_feature("editor") and not force_archive):
		return ""
	var archive_name := String(ProjectSettings.get_setting("dsgd_extension/archive_name", "textures.dspkg"))
	if archive_name.is_empty():
		archive_name = "textures.dspkg"
	var logical := "res://" + archive_name
	if FileAccess.file_exists(logical):
		return logical
	if FileAccess.file_exists("res://textures.dspkg"):
		return "res://textures.dspkg"
	var exe_dir := OS.get_executable_path().get_base_dir()
	if not exe_dir.is_empty():
		var exe_custom := exe_dir.path_join(archive_name)
		if FileAccess.file_exists(exe_custom):
			return exe_custom
		var exe_default := exe_dir.path_join("textures.dspkg")
		if FileAccess.file_exists(exe_default):
			return exe_default
	var globalized_custom := ProjectSettings.globalize_path(logical)
	if FileAccess.file_exists(globalized_custom):
		return globalized_custom
	var globalized_default := ProjectSettings.globalize_path("res://textures.dspkg")
	if FileAccess.file_exists(globalized_default):
		return globalized_default
	return ""


static func collect_scene_textures(scene_path: String, extra_scan_dirs: PackedStringArray = PackedStringArray()) -> PackedStringArray:
	var found: Dictionary = {}
	var root_scene: String = _resolve_dependency_path(scene_path)
	var dirs: Dictionary = {}
	if not root_scene.is_empty():
		dirs[root_scene.get_base_dir()] = true
	for extra_dir in extra_scan_dirs:
		var normalized: String = extra_dir.strip_edges()
		if not normalized.is_empty():
			dirs[normalized] = true

	# Fast-path for exported builds / packages: when textures.dspkg exists and coverage dirs are provided,
	# query DSPkg entries directly (<0.5 ms) and avoid the slow 100+ ms recursive dependency crawl.
	var pkg_path: String = _resolve_dspkg_path()
	var skip_import_check := bool(ProjectSettings.get_setting("dsgd_extension/skip_archive_import_check", false))
	if not extra_scan_dirs.is_empty() and not pkg_path.is_empty() and ClassDB.class_exists("DSPkg"):
		var dspkg: RefCounted = ClassDB.instantiate("DSPkg")
		if dspkg != null and dspkg.has_method("open") and dspkg.has_method("get_entries"):
			var archive_id: int = int(ProjectSettings.get_setting("dsgd_extension/archive_id", 0))
			if dspkg.call("open", pkg_path, archive_id) == OK:
				var pkg_entries: Array = dspkg.call("get_entries")
				for entry in pkg_entries:
					var entry_str := String(entry)
					for dir in dirs.keys():
						if entry_str.begins_with(dir) and _is_prewarmable_texture(entry_str) and (skip_import_check or not _is_excluded_import(entry_str)):
							found[entry_str] = true
							break
				if not found.is_empty():
					var out := PackedStringArray(found.keys())
					out.sort()
					return out

	var visited: Dictionary = {}
	var stack: Array[String] = []
	if not root_scene.is_empty():
		stack.push_back(root_scene)
	while not stack.is_empty():
		var current: String = stack.pop_back()
		if current.is_empty() or visited.has(current):
			continue
		visited[current] = true
		if not ResourceLoader.exists(current):
			continue
		var deps: PackedStringArray = ResourceLoader.get_dependencies(current)
		for dep in deps:
			var dep_path: String = _resolve_dependency_path(dep)
			if dep_path.is_empty() or visited.has(dep_path):
				continue
			if _is_prewarmable_texture(dep_path):
				if not _is_excluded_import(dep_path):
					found[dep_path] = true
			elif _is_traversable_resource(dep_path):
				stack.push_back(dep_path)

	# In exported builds with textures.dspkg, loose PNGs don't exist on disk;
	# query DSPkg entries directly so packaged textures in the target directories are included:
	if not pkg_path.is_empty() and ClassDB.class_exists("DSPkg"):
		var dspkg: RefCounted = ClassDB.instantiate("DSPkg")
		if dspkg != null and dspkg.has_method("open") and dspkg.has_method("get_entries"):
			var archive_id: int = int(ProjectSettings.get_setting("dsgd_extension/archive_id", 0))
			if dspkg.call("open", pkg_path, archive_id) == OK:
				var pkg_entries: Array = dspkg.call("get_entries")
				for entry in pkg_entries:
					var entry_str := String(entry)
					for dir in dirs.keys():
						if entry_str.begins_with(dir) and _is_prewarmable_texture(entry_str) and (skip_import_check or not _is_excluded_import(entry_str)):
							found[entry_str] = true
							break
	for dir in dirs.keys():
		_sweep_directory(dir, found)
	var out := PackedStringArray(found.keys())
	out.sort()
	return out


## Returns [code]true[/code] when async stream prewarm is available:
## [code]flush_stream_async/has_stream_pending[/code] on [code]DSContext[/code],
## [code]prewarm_async[/code] on [code]DSTextureCache[/code], and a stream
## driver node ([code]/root/DSRuntime[/code] or the legacy
## [code]/root/DSBatchManager[/code]) in the tree. When [code]false[/code],
## callers must use the frame-budgeted sync fallback in
## [method load_scene_with_loading_screen]; regular
## [code]ResourceLoader.load()[/code] calls keep working untouched.
##
## The driver check is the important half: without it this reported
## [code]true[/code] for a project whose autoload was missing, so callers chose
## the async path and then waited forever on a queue nobody submitted.
static func is_stream_prewarm_available() -> bool:
	if not ClassDB.class_exists("DSContext"):
		return false
	if not ClassDB.class_exists("DSTextureCache"):
		return false
	var ctx: Object = Engine.get_singleton("DSContext")
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if ctx == null or cache == null:
		return false
	if not (ctx.has_method("flush_stream_async") and ctx.has_method("has_stream_pending") and cache.has_method("prewarm_async")):
		return false
	if not _has_stream_pump():
		_warn_missing_stream_pump()
		return false
	return true


## Enqueues [param paths] (engine texture paths) for async prewarm and returns
## the stream tag. Non-blocking: requests sit in the stream queue until the
## [code]DSRuntime[/code] autoload drains them via
## [code]flush_stream_async()[/code]. Tag discovery is opt-in on the runtime
## coordinator, so this also calls [code]track_prewarm(tag)[/code] immediately —
## without it the tag still flows through flush/collect but emits no
## [code]prewarm_progress[/code] / [code]prewarm_finished[/code] signals.
## Returns 0 when the native singletons or the stream driver are unavailable;
## a tag returned without a driver would never complete.
static func prewarm_async(paths: PackedStringArray) -> int:
	if not ClassDB.class_exists("DSTextureCache"):
		push_warning("[DirectStorage] DSPreloader: DSTextureCache class unavailable; prewarm_async aborted.")
		return 0
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache == null or not cache.has_method("prewarm_async"):
		push_warning("[DirectStorage] DSPreloader: DSTextureCache singleton unavailable; prewarm_async aborted.")
		return 0
	if not _has_stream_pump():
		_warn_missing_stream_pump()
		return 0
	var tag: int = int(cache.call("prewarm_async", paths))
	_track_batch_manager_tag(tag)
	return tag


## Returns [code]true[/code] when hardware GPU prewarming to VRAM is supported and active
## (requires an initialized D3D12 DirectStorage backend with GPU decompression enabled and
## a live D3D12 [RenderingDevice]).
static func is_gpu_prewarm_available() -> bool:
	if not ClassDB.class_exists("DSTextureCache"):
		return false
	var cache: Object = Engine.get_singleton("DSTextureCache")
	return cache != null and cache.has_method("is_gpu_prewarm_available") and bool(cache.call("is_gpu_prewarm_available"))


## Enqueues [param paths] for GPU-accelerated asynchronous streaming prewarm and returns
## the stream tag. When running under the D3D12 rendering driver with GPU decompression enabled,
## eligible textures are allocated directly as [Texture2DRD] resources on the render thread
## and filled via DirectStorage DMA decompression straight into VRAM. Ineligible textures or
## non-D3D12 platforms automatically fall back to the CPU [ImageTexture] memory stream path.
##
## A texture is eligible when its payload carries v2 per-mip footprint metadata, it was not
## imported with [code]compatibility/as_image_texture[/code], and its format is a color format
## with an sRGB counterpart (BC1, BC2, BC3, BC7, the RGBA8/BGRA8 family) or one of the linear
## block formats used by 3D material data (BC4, BC5 - normal, roughness, AO and height maps).
## Uncompressed and float data formats (R8, RG8, RGB8, the R16 family, float formats, BC6H)
## always stay on the memory path, so those arrive as [ImageTexture] even when GPU prewarm is
## active.
##
## Non-multiple-of-4 textures are GPU-eligible, but the resulting [Texture2DRD] reports the
## padded storage size because the engine exposes no size setter. Enable
## [code]compatibility/as_image_texture[/code] when [method Texture2D.get_width] must report the
## authoring size.
##
## Textures are allocated in render-thread chunks of [param chunk_size] and up to
## [param max_chunk_bytes] (when > 0; default 0 uses the native 16 MiB cap, pass
## [constant BACKGROUND_GPU_CHUNK_BYTES] for background gameplay streaming).
## Returns 0 when native singletons or the stream driver are unavailable.
static func prewarm_gpu_async(paths: PackedStringArray, chunk_size: int = 16, prefer_gpu: bool = true, max_chunk_bytes: int = 0) -> int:
	if not ClassDB.class_exists("DSTextureCache"):
		push_warning("[DirectStorage] DSPreloader: DSTextureCache class unavailable; prewarm_gpu_async aborted.")
		return 0
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache == null:
		push_warning("[DirectStorage] DSPreloader: DSTextureCache singleton unavailable; prewarm_gpu_async aborted.")
		return 0
	if not _has_stream_pump():
		_warn_missing_stream_pump()
		return 0
	var tag: int = 0
	if cache.has_method("prewarm_gpu_async"):
		if max_chunk_bytes > 0:
			tag = int(cache.call("prewarm_gpu_async", paths, chunk_size, prefer_gpu, max_chunk_bytes))
		else:
			tag = int(cache.call("prewarm_gpu_async", paths, chunk_size, prefer_gpu))
	elif cache.has_method("prewarm_async"):
		tag = int(cache.call("prewarm_async", paths))
	_track_batch_manager_tag(tag)
	return tag


## Polls [code]DSContext.get_prewarm_progress(tag)[/code] for progress-bar
## display. Returns [code]Vector2i(done, total)[/code]; [code](0, 0)[/code]
## when the context is unavailable or the tag is unknown.
static func get_prewarm_progress(tag: int) -> Vector2i:
	if not ClassDB.class_exists("DSContext"):
		return Vector2i(0, 0)
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.has_method("get_prewarm_progress"):
		return Vector2i(0, 0)
	return ctx.call("get_prewarm_progress", tag) as Vector2i


## Returns [code]true[/code] when every stream request for [param tag] is
## terminal and all completed textures have been built into DSTextureCache.
## Unknown tags report [code]true[/code] (nothing pending).
static func is_prewarm_complete(tag: int) -> bool:
	if not ClassDB.class_exists("DSContext"):
		return true
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null or not ctx.has_method("is_prewarm_complete"):
		return true
	var complete: bool = bool(ctx.call("is_prewarm_complete", tag))
	if complete and ClassDB.class_exists("DSTextureCache"):
		var cache: Object = Engine.get_singleton("DSTextureCache")
		if cache != null and cache.has_method("get_unbuilt_prewarm_count"):
			return int(cache.call("get_unbuilt_prewarm_count", tag)) == 0
	return complete


## Cancels the prewarm for [param tag]: queued requests are marked failed and
## requests already submitted to the backend are cancelled there. The tag reports
## complete once the next completion harvest observes those terminal states, so a
## pump tick is still needed after this call. Does nothing when the context is
## unavailable.
static func cancel_prewarm(tag: int) -> void:
	if not ClassDB.class_exists("DSContext"):
		return
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx != null and ctx.has_method("cancel_prewarm"):
		ctx.call("cancel_prewarm", tag)


## One-call snapshot of prewarm batch [param tag], so a caller does not have to
## assemble three pollers itself just to log what happened.
##
## Keys: [code]progress[/code] ([Vector2i] done/total), [code]complete[/code]
## ([bool]), and the per-tag texture counters [code]pending[/code],
## [code]unbuilt[/code], [code]unbuilt_cpu[/code], [code]unbuilt_gpu[/code]
## ([int]). Every counter is [code]has_method[/code]-guarded, so an older native
## binary reports [code]0[/code] instead of erroring.
##
## An unknown tag reports [code]complete = true[/code], [code]progress = (0, 0)[/code]
## and zero counters, matching [method is_prewarm_complete], which treats a tag
## that was never issued (or was already retired) as "nothing pending" rather
## than "wedged".
static func get_prewarm_state(tag: int) -> Dictionary:
	var state: Dictionary = {
		"progress": get_prewarm_progress(tag),
		"complete": is_prewarm_complete(tag),
		"pending": 0,
		"unbuilt": 0,
		"unbuilt_cpu": 0,
		"unbuilt_gpu": 0,
	}
	if not ClassDB.class_exists("DSTextureCache"):
		return state
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache == null:
		return state
	var counters: Dictionary = {
		"pending": "get_pending_prewarm_count",
		"unbuilt": "get_unbuilt_prewarm_count",
		"unbuilt_cpu": "get_unbuilt_cpu_prewarm_count",
		"unbuilt_gpu": "get_unbuilt_gpu_prewarm_count",
	}
	for key in counters:
		var method: String = counters[key]
		if cache.has_method(method):
			state[key] = int(cache.call(method, tag))
	return state


## Compact liveness fingerprint for [param tag]: request progress plus the
## pending/unbuilt texture counters. Progress alone is not enough, because
## [method is_prewarm_complete] also waits for the build queue, so a batch can
## legitimately sit at [code]done == total[/code] for a while with textures
## still being built - that must not read as a stall.
static func _prewarm_liveness(tag: int) -> Vector4i:
	var state: Dictionary = get_prewarm_state(tag)
	var progress: Vector2i = state["progress"]
	return Vector4i(progress.x, progress.y, int(state["pending"]), int(state["unbuilt"]))


## Emits the stall diagnostic: one line with the full prewarm state plus the
## most likely cause, so the caller only has to decide policy (fall back, retry,
## report). The batch is cancelled by the caller right after this.
static func _warn_prewarm_stall(tag: int, progress: Vector2i, stall_timeout_msec: int) -> void:
	var state: Dictionary = get_prewarm_state(tag)
	var cause := "the stream driver is still in the tree, so the queue is not making progress; check DSErrorHandler for a device-lost or queue error"
	if not _has_stream_pump():
		cause = "no /root/%s or /root/%s in the scene tree, so nothing submits the stream queue; enable the DirectStorage Accelerator plugin so it registers the autoload" % [STREAM_PUMP_AUTOLOAD, STREAM_PUMP_LEGACY_AUTOLOAD]
	push_warning(
		"[DirectStorage] DSPreloader: prewarm tag %d stalled - no progress for %d ms, cancelling it so the caller can fall back. state: progress=(%d, %d) complete=%s pending=%d unbuilt=%d (cpu=%d gpu=%d). cause: %s"
		% [
			tag,
			stall_timeout_msec,
			progress.x,
			progress.y,
			str(bool(state["complete"])),
			int(state["pending"]),
			int(state["unbuilt"]),
			int(state["unbuilt_cpu"]),
			int(state["unbuilt_gpu"]),
			cause,
		]
	)


## Awaits prewarm completion without blocking: yields one
## [code]process_frame[/code] per iteration and invokes
## [param on_progress] as [code]on_progress.call(done, total)[/code] for bar
## updates. Returns [code]true[/code] when the batch completed, [code]false[/code]
## when the main loop is not a [SceneTree] or when the batch was declared
## stalled and cancelled.
##
## [param stall_timeout_msec] is opt-in and defaults to [code]0[/code], which
## keeps the historical behaviour exactly: poll until complete, no timeout.
## When greater than zero, the call tracks a liveness fingerprint (progress plus
## the pending/unbuilt counters) and, after that many milliseconds without any
## change, emits one warning with a state dump, calls [method cancel_prewarm] so
## the tag terminates on the next completion harvest, invokes [param on_stall] as
## [code]on_stall.call(tag, done, total)[/code] when valid, and returns
## [code]false[/code]. Callers that ignore the return value keep working.
##
## Prefer awaiting the batch manager's [code]prewarm_finished[/code] signal when
## the autoload is present; use this poll when driving progress from a static
## context.
static func await_prewarm_finished(tag: int, on_progress: Callable = Callable(), stall_timeout_msec: int = 0, on_stall: Callable = Callable()) -> bool:
	if on_progress.is_valid():
		var initial: Vector2i = get_prewarm_progress(tag)
		on_progress.call(initial.x, initial.y)
	var loop: MainLoop = Engine.get_main_loop()
	var last_alive_msec: int = Time.get_ticks_msec()
	var last_alive: Vector4i = _prewarm_liveness(tag)
	while not is_prewarm_complete(tag):
		if not (loop is SceneTree):
			return false
		await (loop as SceneTree).process_frame
		var manager: Node = _get_stream_pump()
		if manager == null and ClassDB.class_exists("DSTextureCache"):
			var cache: Object = Engine.get_singleton("DSTextureCache")
			if cache != null:
				if cache.has_method("pump_gpu_prewarm"):
					cache.call("pump_gpu_prewarm")
				if cache.has_method("poll_completed"):
					cache.call("poll_completed", tag, 0, 2000)
		var progress: Vector2i = get_prewarm_progress(tag)
		if on_progress.is_valid():
			on_progress.call(progress.x, progress.y)
		if stall_timeout_msec <= 0:
			continue
		var now: int = Time.get_ticks_msec()
		var liveness: Vector4i = _prewarm_liveness(tag)
		if liveness != last_alive:
			last_alive = liveness
			last_alive_msec = now
			continue
		if now - last_alive_msec < stall_timeout_msec:
			continue
		_warn_prewarm_stall(tag, progress, stall_timeout_msec)
		cancel_prewarm(tag)
		if on_stall.is_valid():
			on_stall.call(tag, progress.x, progress.y)
		return false
	if on_progress.is_valid():
		var final_progress: Vector2i = get_prewarm_progress(tag)
		on_progress.call(final_progress.x, final_progress.y)
	return true


## Full loading-screen sample: prewarm -> cached scene bind -> instantiate ->
## attach -> one drawn frame, all under the loading overlay.
## [param tree]: active [SceneTree]. [param scene_path]: target
## [code].tscn[/code]. [param loading_screen]: overlay kept visible across
## [code]instantiate()[/code] + [code]add_child()[/code] + one drawn frame so
## shader compilation finishes before gameplay is revealed. [param progress_bar]:
## optional [Range] (e.g. [ProgressBar]) driven with the prewarm/tag or
## fallback budget loop. [param extra_scan_dirs]: extra [code]DirAccess[/code]
## sweep roots for dynamic assets. Returns the instanced scene root, or
## [code]null[/code] on failure.
## When stream prewarm is unavailable, falls back to the frame-budgeted
## sync [code]ResourceLoader.load()[/code] loop ([param frame_budget_usec]
## per frame, loading animation stays at 60 FPS).
static func load_scene_with_loading_screen(tree: SceneTree, scene_path: String, loading_screen: CanvasItem, progress_bar: Range = null, extra_scan_dirs: PackedStringArray = PackedStringArray(), frame_budget_usec: int = FALLBACK_FRAME_BUDGET_USEC, prefer_gpu: bool = true) -> Node:
	loading_screen.visible = true
	var deps: PackedStringArray = collect_scene_textures(scene_path, extra_scan_dirs)
	if is_stream_prewarm_available():
		var tag: int = prewarm_gpu_async(deps, 16, prefer_gpu) if prefer_gpu and is_gpu_prewarm_available() else prewarm_async(deps)
		if progress_bar != null:
			progress_bar.min_value = 0.0
			progress_bar.max_value = 100.0
			progress_bar.value = 0.0
		var bar: Range = progress_bar
		var on_progress := func(done: int, total: int) -> void:
			if bar != null and total > 0:
				bar.value = (float(done) / float(total)) * 100.0
		await await_prewarm_finished(tag, on_progress)
	else:
		await _fallback_budget_prewarm(deps, progress_bar, frame_budget_usec)
	# REUSE bind: textures resolve from the prewarmed cache without reading
	# the disk again. IGNORE/REPLACE would bypass the cache and waste the
	# prewarm.
	var packed: PackedScene = ResourceLoader.load(scene_path, "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
	if packed == null:
		push_warning("[DirectStorage] DSPreloader: failed to load scene '", scene_path, "'.")
		loading_screen.visible = false
		return null
	var instance: Node = packed.instantiate()
	var parent: Node = tree.current_scene if tree.current_scene != null else tree.root
	parent.add_child(instance)
	# Hold one drawn frame for shaders: process_frame lets the tree settle,
	# then frame_post_draw guarantees the driver compiled/uploaded before
	# reveal.
	await tree.process_frame
	await RenderingServer.frame_post_draw
	loading_screen.visible = false
	return instance


## Opt-in tag tracking on the batch manager: tracked tags emit
## [code]prewarm_progress[/code] on change and one
## [code]prewarm_finished[/code]. No-op when the autoload is absent.
static func _track_batch_manager_tag(tag: int) -> void:
	if tag <= 0:
		return
	var manager: Node = _get_stream_pump()
	if manager != null and manager.has_method("track_prewarm"):
		manager.call("track_prewarm", tag)


## Frame-budgeted sync fallback: loads each texture with
## [code]CACHE_MODE_REUSE[/code], consuming at most [param budget_usec]
## microseconds of main time per frame so the loading animation holds 60 FPS.
static func _fallback_budget_prewarm(paths: PackedStringArray, progress_bar: Range, budget_usec: int) -> void:
	var loop: MainLoop = Engine.get_main_loop()
	if not (loop is SceneTree):
		return
	var tree: SceneTree = loop as SceneTree
	var total: int = paths.size()
	if progress_bar != null:
		progress_bar.min_value = 0.0
		progress_bar.max_value = 100.0
		progress_bar.value = 0.0 if total > 0 else 100.0
	var idx: int = 0
	while idx < total:
		var frame_start: int = Time.get_ticks_usec()
		var frame_first: int = idx
		while idx < total and (Time.get_ticks_usec() - frame_start < budget_usec or idx == frame_first):
			ResourceLoader.load(paths[idx], "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
			idx += 1
		if progress_bar != null:
			progress_bar.value = (float(idx) / float(total)) * 100.0
		await tree.process_frame


## True for engine authoring extensions the native preloader accepts.
static func _is_prewarmable_texture(path: String) -> bool:
	var ext: String = path.get_extension().to_lower()
	return ext in PREWARMABLE_EXTENSIONS


## True for resources worth recursing into with [code]get_dependencies()[/code].
static func _is_traversable_resource(path: String) -> bool:
	var ext: String = path.get_extension().to_lower()
	return ext in TRAVERSABLE_EXTENSIONS


## Skipped-texture filter: rejects float-format
## [code].exr/.hdr[/code] paths and textures imported with
## [code]compatibility/as_image_texture[/code] (Light3D projector / Decal
## textures requiring CPU [code]ImageTexture[/code]). Those load correctly
## through the synchronous loader later; they are simply not stream-prewarmed.
static func _is_excluded_import(path: String) -> bool:
	var ext: String = path.get_extension().to_lower()
	if ext in EXCLUDED_EXTENSIONS:
		return true
	var import_path: String = path + ".import"
	if not FileAccess.file_exists(import_path):
		return false
	var cfg := ConfigFile.new()
	if cfg.load(import_path) != OK:
		return false
	return bool(cfg.get_value("params", "compatibility/as_image_texture", false))


## Recursively sweeps [param directory] for prewarmable textures, applying
## the same projector/EXR filter as the dependency walk. Missing or
## unreadable directories are silently skipped (dynamic-asset catch-all must
## never fail the prewarm).
static func _sweep_directory(directory: String, found: Dictionary) -> void:
	var stack: Array[String] = [directory]
	var visited_dirs: Dictionary = {}
	while not stack.is_empty():
		var current: String = stack.pop_back()
		if current.is_empty() or visited_dirs.has(current):
			continue
		visited_dirs[current] = true
		var access := DirAccess.open(current)
		if access == null:
			continue
		access.list_dir_begin()
		var entry: String = access.get_next()
		while entry != "":
			if entry != "." and entry != "..":
				var full: String = current.path_join(entry)
				if access.current_is_dir():
					if not visited_dirs.has(full):
						stack.push_back(full)
				elif _is_prewarmable_texture(full) and not found.has(full) and not _is_excluded_import(full):
					found[full] = true
			entry = access.get_next()
		access.list_dir_end()
