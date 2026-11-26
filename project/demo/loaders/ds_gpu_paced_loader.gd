class_name DSGpuPacedLoader
extends RefCounted

## Example 4 (two-stage loading-screen pattern):
##   1. Offload collect_scene_textures() to WorkerThreadPool.
##   2. Run DSPreloader.start_scene_prewarm(ctx.scene_path, PackedStringArray(), 16, true, 0, deps)
##      (full loading-screen GPU throughput + Q40-compliant use_sub_threads = false
##      CACHE_MODE_REUSE threaded scene load via DSRuntime).

static func run(ctx: DemoLoadContext) -> void:
	ctx.loading_screen.set_phase("Collecting scene textures (worker thread)...", 0.0, "Offloading .import/.dstex scan...")

	var t_pre0 := Time.get_ticks_usec()
	var deps_out := {"deps": PackedStringArray()}
	var task_id := WorkerThreadPool.add_task(func() -> void:
		deps_out["deps"] = DSPreloader.collect_scene_textures(ctx.scene_path)
	)
	while not WorkerThreadPool.is_task_completed(task_id):
		await ctx.next_frame()
	WorkerThreadPool.wait_for_task_completion(task_id)

	if ctx.cancelled:
		ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
		return

	var deps: PackedStringArray = deps_out["deps"]
	if deps.is_empty():
		ctx.error_msg = "DSPreloader.collect_scene_textures returned 0 textures."
		ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
		return

	var runtime: Node = ctx.tree.root.get_node_or_null("DSRuntime")
	if runtime == null:
		ctx.error_msg = "DSRuntime autoload not found; cannot run start_scene_prewarm()."
		ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
		return

	ctx.active_scene_tag = DSPreloader.start_scene_prewarm(
		ctx.scene_path, PackedStringArray(), 16, true, 0, deps
	)
	if ctx.active_scene_tag <= 0:
		ctx.error_msg = "DSPreloader.start_scene_prewarm failed to start (tag=0)."
		ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
		return

	var prewarm_recorded := false
	var t_scene0 := 0
	while not bool(runtime.call("is_scene_prewarm_complete", ctx.active_scene_tag)):
		var state: Dictionary = runtime.call("get_scene_prewarm_state", ctx.active_scene_tag)
		var stage := String(state.get("stage", "textures"))
		var stage_prog := float(state.get("progress", 0.0))
		var tex_tag := int(state.get("tex_tag", 0))

		if stage == "textures":
			var prog := DSPreloader.get_prewarm_progress(tex_tag) if tex_tag > 0 else Vector2i(0, deps.size())
			var tex_state := DSPreloader.get_prewarm_state(tex_tag) if tex_tag > 0 else {}
			var frac := float(prog.x) / float(maxi(prog.y, 1)) * 0.75
			var stats := "%d / %d | pending=%d | unbuilt_gpu=%d" % [
				prog.x, prog.y, int(tex_state.get("pending", 0)), int(tex_state.get("unbuilt_gpu", 0))
			]
			ctx.loading_screen.set_phase("DS GPU Prewarm...", frac, stats)
		elif stage == "scene":
			if not prewarm_recorded:
				ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
				prewarm_recorded = true
				t_scene0 = Time.get_ticks_usec()
				ctx.loading_screen.set_cancel_enabled(false)
			var scene_pct := clampf((stage_prog - 0.5) * 2.0, 0.0, 1.0)
			ctx.loading_screen.set_phase(
				"Threaded PackedScene Load (CACHE_MODE_REUSE)...",
				0.75 + scene_pct * 0.25,
				"Scene: %.0f%%" % (scene_pct * 100.0)
			)
		elif stage == "cancelling":
			if not prewarm_recorded:
				ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
				prewarm_recorded = true
			ctx.loading_screen.set_cancel_enabled(false)
			ctx.loading_screen.set_phase("Cancelling DS Scene Load...", 0.0, "Draining threaded scene load...")

		await ctx.next_frame()

	if not prewarm_recorded:
		ctx.prewarm_ms = float(Time.get_ticks_usec() - t_pre0) / 1000.0
	if t_scene0 > 0:
		ctx.instantiate_ms += float(Time.get_ticks_usec() - t_scene0) / 1000.0

	ctx.loaded_scene = runtime.call("take_scene_prewarm_result", ctx.active_scene_tag) as PackedScene
	ctx.active_scene_tag = 0

	if not ctx.cancelled and ctx.loaded_scene == null:
		ctx.error_msg = "Threaded DS scene load failed or returned null."
