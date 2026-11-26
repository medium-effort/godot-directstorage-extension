class_name VanillaSyncLoader
extends RefCounted

## Example 0: Synchronous ResourceLoader.load() (blocks main thread until finished).

static func run(ctx: DemoLoadContext) -> void:
	ctx.loading_screen.set_phase("Vanilla Sync Load (blocking main thread)...", 0.1, "Reading .ctex scene...")

	var t0 := Time.get_ticks_usec()
	ctx.loaded_scene = ResourceLoader.load(
		ctx.scene_path, "PackedScene", ResourceLoader.CACHE_MODE_REUSE
	) as PackedScene
	ctx.prewarm_ms = ctx.record_sync_span(t0)
	ctx.loading_screen.set_phase("Instantiating...", 0.9, "Done reading")
