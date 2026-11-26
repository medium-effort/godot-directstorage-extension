class_name DSPrewarmLoader
extends RefCounted

## Example 2 & 3: DirectStorage CPU or GPU prewarm followed by CACHE_MODE_REUSE scene load.

static func run(ctx: DemoLoadContext, use_gpu: bool) -> void:
	ctx.loading_screen.set_phase("Collecting scene textures...", 0.0, "Scanning dependencies...")

	var t_pre0 := Time.get_ticks_usec()
	var deps := DSPreloader.collect_scene_textures(ctx.scene_path)
	if deps.is_empty():
		ctx.error_msg = "DSPreloader.collect_scene_textures returned 0 textures."
		return

	ctx.active_tag = DSPreloader.prewarm_gpu_async(deps) if use_gpu else DSPreloader.prewarm_async(deps)
	if ctx.active_tag <= 0:
		ctx.error_msg = "Prewarm failed to start (tag=0)."
		return

	while not ctx.cancelled and not DSPreloader.is_prewarm_complete(ctx.active_tag):
		var prog := DSPreloader.get_prewarm_progress(ctx.active_tag)
		var frac := float(prog.x) / float(maxi(prog.y, 1))
		var state := DSPreloader.get_prewarm_state(ctx.active_tag)
		var stats := "%d / %d | pending=%d | unbuilt_gpu=%d | unbuilt_cpu=%d" % [
			prog.x, prog.y,
			int(state.get("pending", 0)),
			int(state.get("unbuilt_gpu", 0)),
			int(state.get("unbuilt_cpu", 0))
		]
		ctx.loading_screen.set_phase("DirectStorage %s Prewarm..." % ("GPU" if use_gpu else "CPU"), frac, stats)
		await ctx.next_frame()

	ctx.active_tag = 0
	ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0

	if not ctx.cancelled:
		ctx.loading_screen.set_cancel_enabled(false)
		ctx.loading_screen.set_phase("Loading PackedScene (CACHE_MODE_REUSE)...", 0.95, "Binding prewarmed textures...")
		var t_inst0 := Time.get_ticks_usec()
		ctx.loaded_scene = ResourceLoader.load(
			ctx.scene_path, "PackedScene", ResourceLoader.CACHE_MODE_REUSE
		) as PackedScene
		ctx.instantiate_ms += ctx.record_sync_span(t_inst0)
