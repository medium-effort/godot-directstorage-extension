class_name VanillaThreadedLoader
extends RefCounted

## Example 1: Standard Godot async loading screen via ResourceLoader.load_threaded_request().

static func run(ctx: DemoLoadContext) -> void:
	ctx.loading_screen.set_phase("Vanilla Threaded Load...", 0.0, "Starting load_threaded_request...")
	var t0 := Time.get_ticks_usec()
	var req_err := ResourceLoader.load_threaded_request(
		ctx.scene_path, "PackedScene", false, ResourceLoader.CACHE_MODE_REUSE
	)
	if req_err != OK:
		ctx.error_msg = "load_threaded_request failed: %s" % error_string(req_err)
		return

	var progress := []
	while true:
		var status := ResourceLoader.load_threaded_get_status(ctx.scene_path, progress)
		if ctx.cancelled:
			ctx.loading_screen.set_phase("Cancelling Vanilla Threaded Load...", 0.0, "Draining background worker...")
			if status == ResourceLoader.THREAD_LOAD_IN_PROGRESS:
				await ctx.next_frame()
				continue
			if status == ResourceLoader.THREAD_LOAD_LOADED:
				var _discard: Resource = ResourceLoader.load_threaded_get(ctx.scene_path)
				_discard = null
			break

		var pct := float(progress[0]) if progress.size() > 0 else 0.0
		ctx.loading_screen.set_phase("Vanilla Threaded Load...", pct, "Progress: %.0f%%" % (pct * 100.0))
		if status == ResourceLoader.THREAD_LOAD_LOADED:
			var t_get0 := Time.get_ticks_usec()
			ctx.loaded_scene = ResourceLoader.load_threaded_get(ctx.scene_path) as PackedScene
			ctx.record_sync_span(t_get0)
			break
		elif status == ResourceLoader.THREAD_LOAD_FAILED or status == ResourceLoader.THREAD_LOAD_INVALID_RESOURCE:
			ctx.error_msg = "Threaded load failed (status=%d)" % status
			break
		await ctx.next_frame()

	ctx.prewarm_ms = float(Time.get_ticks_usec() - t0) / 1000.0
