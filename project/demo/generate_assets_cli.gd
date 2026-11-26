extends SceneTree

## Headless CLI entry point for DemoAssetGenerator:
##   --textures : generate procedural PNGs and .import sidecars into res://demo_generated/
##   --scenes   : build demo_level_vanilla.scn and demo_level_ds.scn (after editor import)

func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	var do_textures := args.has("--textures")
	var do_scenes := args.has("--scenes")
	if not do_textures and not do_scenes:
		do_textures = true

	if do_textures:
		var err := DemoAssetGenerator.generate_source_textures()
		if err != OK:
			push_error("DemoAssetGenerator.generate_source_textures failed: %s" % error_string(err))
			quit(1)
			return
		print("[demo_gen] Generated %d procedural textures in %s" % [DemoAssetGenerator.TEX_COUNT, DemoAssetGenerator.GENERATED_ROOT])

	if do_scenes:
		var err := DemoAssetGenerator.build_level_scenes()
		if err != OK:
			push_error("DemoAssetGenerator.build_level_scenes failed: %s" % error_string(err))
			quit(1)
			return
		print("[demo_gen] Built level scenes in %s" % DemoAssetGenerator.GENERATED_ROOT)

	quit(0)
