class_name DSPreloader
extends RefCounted

## DirectStorage Async Prewarm Helpers.
##
## Static GDScript surface over the native async streaming pipeline:
## [code]DSTextureCache.prewarm_async()[/code] (enqueue, engine paths only) +
## [code]DSBatchManager[/code] autoload [code]_process[/code]
## (collect -> [code]flush_stream_async()[/code] -> [code]poll_completed()[/code] ->
## per-tag progress) + [code]DSContext[/code] tag queries.
##
## Canonical flow (see [method collect_scene_textures] and
## [method load_scene_with_loading_screen]):
## [codeblock]
## var deps: PackedStringArray = DSPreloader.collect_scene_textures("res://levels/level1.tscn")
## var tag: int = DSPreloader.prewarm_async(deps)
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
## Resolves the physical path of textures.dspkg in both editor (res://) and exported builds (exe dir).
static func _resolve_dspkg_path() -> String:
	const logical := "res://textures.dspkg"
	if FileAccess.file_exists(logical):
		return logical
	var exe_dir := OS.get_executable_path().get_base_dir()
	if not exe_dir.is_empty():
		var exe_archive := exe_dir.path_join("textures.dspkg")
		if FileAccess.file_exists(exe_archive):
			return exe_archive
	var globalized := ProjectSettings.globalize_path(logical)
	if FileAccess.file_exists(globalized):
		return globalized
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
	if not extra_scan_dirs.is_empty() and not pkg_path.is_empty() and ClassDB.class_exists("DSPkg"):
		var dspkg: RefCounted = ClassDB.instantiate("DSPkg")
		if dspkg != null and dspkg.has_method("open") and dspkg.has_method("get_entries"):
			if dspkg.call("open", pkg_path) == OK:
				var pkg_entries: Array = dspkg.call("get_entries")
				for entry in pkg_entries:
					var entry_str := String(entry)
					for dir in dirs.keys():
						if entry_str.begins_with(dir) and _is_prewarmable_texture(entry_str) and not _is_excluded_import(entry_str):
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
			if dspkg.call("open", pkg_path) == OK:
				var pkg_entries: Array = dspkg.call("get_entries")
				for entry in pkg_entries:
					var entry_str := String(entry)
					for dir in dirs.keys():
						if entry_str.begins_with(dir) and _is_prewarmable_texture(entry_str) and not _is_excluded_import(entry_str):
							found[entry_str] = true
							break
	for dir in dirs.keys():
		_sweep_directory(dir, found)
	var out := PackedStringArray(found.keys())
	out.sort()
	return out


## Returns [code]true[/code] when async stream prewarm is available
## ([code]flush_stream_async/has_stream_pending[/code] on [code]DSContext[/code]
## plus [code]prewarm_async[/code] on [code]DSTextureCache[/code]). When
## [code]false[/code], callers must use the frame-budgeted sync fallback in
## [method load_scene_with_loading_screen]; regular
## [code]ResourceLoader.load()[/code] calls keep working untouched.
static func is_stream_prewarm_available() -> bool:
	if not ClassDB.class_exists("DSContext"):
		return false
	if not ClassDB.class_exists("DSTextureCache"):
		return false
	var ctx: Object = Engine.get_singleton("DSContext")
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if ctx == null or cache == null:
		return false
	return ctx.has_method("flush_stream_async") and ctx.has_method("has_stream_pending") and cache.has_method("prewarm_async")


## Enqueues [param paths] (engine texture paths) for async prewarm and returns
## the stream tag. Non-blocking: requests sit in the stream queue until the
## [code]DSBatchManager[/code] autoload drains them via
## [code]flush_stream_async()[/code]. Tag discovery is opt-in on the batch
## manager, so this also calls [code]track_prewarm(tag)[/code] immediately —
## without it the tag still flows through flush/collect but emits no
## [code]prewarm_progress[/code] / [code]prewarm_finished[/code] signals.
## Returns 0 when the native singletons are unavailable.
static func prewarm_async(paths: PackedStringArray) -> int:
	if not ClassDB.class_exists("DSTextureCache"):
		push_warning("[DirectStorage] DSPreloader: DSTextureCache class unavailable; prewarm_async aborted.")
		return 0
	var cache: Object = Engine.get_singleton("DSTextureCache")
	if cache == null or not cache.has_method("prewarm_async"):
		push_warning("[DirectStorage] DSPreloader: DSTextureCache singleton unavailable; prewarm_async aborted.")
		return 0
	var tag: int = int(cache.call("prewarm_async", paths))
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


## Cancels the prewarm for [param tag]: queued requests fail with a
## "cancelled" message so [method is_prewarm_complete] returns [code]true[/code].
## Does nothing when the context is unavailable.
static func cancel_prewarm(tag: int) -> void:
	if not ClassDB.class_exists("DSContext"):
		return
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx != null and ctx.has_method("cancel_prewarm"):
		ctx.call("cancel_prewarm", tag)


## Awaits prewarm completion without blocking: yields one
## [code]process_frame[/code] per iteration and invokes
## [param on_progress] as [code]on_progress.call(done, total)[/code] for bar
## updates. Prefer awaiting the batch manager's
## [code]prewarm_finished[/code] signal when the autoload is present; use
## this poll when driving progress from a static context.
static func await_prewarm_finished(tag: int, on_progress: Callable = Callable()) -> void:
	if on_progress.is_valid():
		var initial: Vector2i = get_prewarm_progress(tag)
		on_progress.call(initial.x, initial.y)
	var loop: MainLoop = Engine.get_main_loop()
	while not is_prewarm_complete(tag):
		if loop is SceneTree:
			await (loop as SceneTree).process_frame
		else:
			return
		var manager: Node = (loop as SceneTree).root.get_node_or_null("DSBatchManager") if (loop is SceneTree) else null
		if manager == null and ClassDB.class_exists("DSTextureCache"):
			var cache: Object = Engine.get_singleton("DSTextureCache")
			if cache != null and cache.has_method("poll_completed"):
				cache.call("poll_completed", tag, 0, 2000)
		if on_progress.is_valid():
			var progress: Vector2i = get_prewarm_progress(tag)
			on_progress.call(progress.x, progress.y)
	if on_progress.is_valid():
		var final_progress: Vector2i = get_prewarm_progress(tag)
		on_progress.call(final_progress.x, final_progress.y)


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
static func load_scene_with_loading_screen(tree: SceneTree, scene_path: String, loading_screen: CanvasItem, progress_bar: Range = null, extra_scan_dirs: PackedStringArray = PackedStringArray(), frame_budget_usec: int = FALLBACK_FRAME_BUDGET_USEC) -> Node:
	loading_screen.visible = true
	var deps: PackedStringArray = collect_scene_textures(scene_path, extra_scan_dirs)
	if is_stream_prewarm_available():
		var tag: int = prewarm_async(deps)
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
	var loop: MainLoop = Engine.get_main_loop()
	if not (loop is SceneTree):
		return
	var manager: Node = (loop as SceneTree).root.get_node_or_null("DSBatchManager")
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
