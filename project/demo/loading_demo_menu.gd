extends Control

## TPS-demo-style main menu for exercising and comparing 5 loading strategies:
##   0: Vanilla Load (Sync ResourceLoader.load on .ctex scene)
##   1: Vanilla Optimized (ResourceLoader.load_threaded_request on .ctex scene)
##   2: DS (CPU Prewarm) (DSPreloader.prewarm_async -> CACHE_MODE_REUSE)
##   3: DS (GPU Prewarm) (DSPreloader.prewarm_gpu_async -> CACHE_MODE_REUSE)
##   4: DS (GPU Prewarm + Threaded Scene) (DSPreloader.start_scene_prewarm -> CACHE_MODE_REUSE)

enum LoadMode {
	VANILLA_SYNC = 0,
	VANILLA_THREADED = 1,
	DS_CPU_PREWARM = 2,
	DS_GPU_PREWARM = 3,
	DS_GPU_PACED = 4,
}

const MODE_LABELS := {
	LoadMode.VANILLA_SYNC: "Vanilla Load (Sync)",
	LoadMode.VANILLA_THREADED: "Vanilla Optimized (Threaded)",
	LoadMode.DS_CPU_PREWARM: "DS (CPU Prewarm)",
	LoadMode.DS_GPU_PREWARM: "DS (GPU Prewarm)",
	LoadMode.DS_GPU_PACED: "DS (GPU Prewarm + Threaded Scene)",
}

const MAIN_AB_SCENE := "res://main.tscn"

@onready var _backend_label: Label = %BackendLabel
@onready var _asset_status_label: Label = %AssetStatusLabel
@onready var _mode_select: OptionButton = %ModeSelect
@onready var _launch_button: Button = %LaunchButton
@onready var _clear_cache_button: Button = %ClearCacheButton
@onready var _back_ab_button: Button = %BackToABButton
@onready var _results_box: RichTextLabel = %ResultsBox
@onready var _loading_screen: Control = %LoadingScreen
@onready var _menu_root: Control = %MenuRoot

var _active_level: Node = null
var _busy: bool = false
var _active_ctx: DemoLoadContext = null


func _ready() -> void:
	_loading_screen.visible = false
	_loading_screen.cancel_requested.connect(_on_cancel_requested)
	_launch_button.pressed.connect(_on_launch_pressed)
	_clear_cache_button.pressed.connect(_on_clear_cache_pressed)
	_back_ab_button.pressed.connect(_on_back_to_ab_pressed)

	_update_backend_banner()
	_refresh_asset_status()
	call_deferred("_warmup_3d_shaders")


func _warmup_3d_shaders() -> void:
	if _busy:
		return
	_busy = true
	_launch_button.disabled = true
	var vp := SubViewport.new()
	vp.size = Vector2i(64, 64)
	vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	var sky := Sky.new()
	sky.sky_material = ProceduralSkyMaterial.new()
	var env := Environment.new()
	env.background_mode = Environment.BG_SKY
	env.sky = sky
	env.tonemap_mode = Environment.TONE_MAPPER_AGX
	env.ssao_enabled = true
	env.glow_enabled = true
	env.fog_enabled = true
	env.volumetric_fog_enabled = true
	env.adjustment_enabled = true
	var we := WorldEnvironment.new()
	we.environment = env
	var sun := DirectionalLight3D.new()
	var spot := SpotLight3D.new()
	spot.shadow_enabled = true
	spot.light_volumetric_fog_energy = 5.0
	var omni := OmniLight3D.new()
	omni.light_volumetric_fog_energy = 8.0
	var probe := ReflectionProbe.new()
	probe.box_projection = true
	probe.size = Vector3(10.0, 10.0, 10.0)
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 3.0)
	var img := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	img.fill(Color.WHITE)
	var dummy_tex := ImageTexture.create_from_image(img)
	var mat := StandardMaterial3D.new()
	mat.albedo_texture = dummy_tex
	mat.normal_enabled = true
	mat.normal_texture = dummy_tex
	mat.roughness_texture = dummy_tex
	mat.metallic = 0.75
	mat.emission_enabled = true
	mat.emission_texture = dummy_tex
	mat.emission_energy_multiplier = 0.8
	var mesh_inst := MeshInstance3D.new()
	mesh_inst.mesh = BoxMesh.new()
	mesh_inst.material_override = mat

	var mat_opaque := StandardMaterial3D.new()
	mat_opaque.albedo_texture = dummy_tex
	mat_opaque.normal_enabled = true
	mat_opaque.normal_texture = dummy_tex
	mat_opaque.roughness_texture = dummy_tex
	var mesh_opaque := MeshInstance3D.new()
	mesh_opaque.mesh = BoxMesh.new()
	mesh_opaque.material_override = mat_opaque

	var mat_scissor := StandardMaterial3D.new()
	mat_scissor.albedo_texture = dummy_tex
	mat_scissor.normal_enabled = true
	mat_scissor.normal_texture = dummy_tex
	mat_scissor.roughness_texture = dummy_tex
	mat_scissor.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA_SCISSOR
	mat_scissor.alpha_scissor_threshold = 0.45
	mat_scissor.cull_mode = BaseMaterial3D.CULL_DISABLED
	var mesh_scissor := MeshInstance3D.new()
	mesh_scissor.mesh = BoxMesh.new()
	mesh_scissor.material_override = mat_scissor

	var mat_alpha := StandardMaterial3D.new()
	mat_alpha.albedo_texture = dummy_tex
	mat_alpha.normal_enabled = true
	mat_alpha.normal_texture = dummy_tex
	mat_alpha.roughness_texture = dummy_tex
	mat_alpha.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	mat_alpha.albedo_color = Color(0.55, 0.85, 1.0, 0.65)
	mat_alpha.emission_enabled = true
	mat_alpha.emission = Color(0.2, 0.55, 0.9)
	mat_alpha.emission_energy_multiplier = 0.5
	var mesh_alpha := MeshInstance3D.new()
	mesh_alpha.mesh = BoxMesh.new()
	mesh_alpha.material_override = mat_alpha

	var dust_quad := QuadMesh.new()
	dust_quad.size = Vector2(0.08, 0.08)
	var dust_mat := StandardMaterial3D.new()
	dust_mat.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	dust_mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	dust_mat.albedo_color = Color(1.0, 0.88, 0.72, 0.45)
	dust_mat.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	dust_quad.material = dust_mat
	var particles := GPUParticles3D.new()
	particles.amount = 8
	particles.process_material = ParticleProcessMaterial.new()
	particles.draw_pass_1 = dust_quad

	vp.add_child(we)
	vp.add_child(sun)
	vp.add_child(spot)
	vp.add_child(omni)
	vp.add_child(probe)
	vp.add_child(cam)
	vp.add_child(mesh_inst)
	vp.add_child(mesh_opaque)
	vp.add_child(mesh_scissor)
	vp.add_child(mesh_alpha)
	vp.add_child(particles)
	add_child(vp)
	await get_tree().process_frame
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	vp.queue_free()
	_log("[color=gray]3D StandardMaterial3D shaders warmed up.[/color]")
	_log("[color=gray]Note: Editor runs read loose files (not .dspkg); repeat Vanilla runs also hit Windows' warm OS page cache.[/color]")
	_busy = false
	_refresh_asset_status()


func _update_backend_banner() -> void:
	if not ClassDB.class_exists("DSContext"):
		_backend_label.text = "DSContext not loaded — build GDExtension first."
		return
	var ctx := Engine.get_singleton("DSContext")
	if not ctx.is_initialized():
		ctx.initialize_ds()
	var driver := RenderingServer.get_current_rendering_driver_name()
	var backend := String(ctx.get_active_backend_name())
	var gpu_prewarm := DSPreloader.is_gpu_prewarm_available()
	_backend_label.text = "Driver: %s | DS Backend: %s | GPU Prewarm: %s" % [
		driver, backend, str(gpu_prewarm)
	]


func _refresh_asset_status() -> void:
	if DemoAssetGenerator.assets_imported():
		_asset_status_label.text = "Demo assets ready (%d x %dx%d textures in res://demo_generated/)." % [
			DemoAssetGenerator.TEX_COUNT, DemoAssetGenerator.TEX_SIZE, DemoAssetGenerator.TEX_SIZE
		]
		_launch_button.disabled = false
	elif DemoAssetGenerator.textures_imported():
		_asset_status_label.text = "Demo textures imported, but level scenes (.scn) are missing — stop the running game first, then open res://demo/editor_scripts/generate_demo_assets_tool.gd in the Godot Editor and press Ctrl+Shift+X (File > Run)."
		_launch_button.disabled = true
	elif DemoAssetGenerator.assets_exist():
		_asset_status_label.text = "Demo assets generated but not yet imported into .godot/imported/ — open project in Godot Editor or run --headless --editor --import."
		_launch_button.disabled = true
	else:
		_asset_status_label.text = "Demo assets not generated yet — stop the running game first, then open res://demo/editor_scripts/generate_demo_assets_tool.gd in the Godot Editor and press Ctrl+Shift+X (File > Run)."
		_launch_button.disabled = true


func _on_back_to_ab_pressed() -> void:
	if _busy:
		return
	get_tree().change_scene_to_file(MAIN_AB_SCENE)


func _on_clear_cache_pressed() -> void:
	if _busy:
		return
	_busy = true
	_launch_button.disabled = true
	await _clear_all_caches()
	_log("[color=gray]Cleared engine references and DSTextureCache.[/color]")
	_busy = false
	_refresh_asset_status()


func _on_cancel_requested() -> void:
	if _active_ctx == null or _active_ctx.cancelled:
		return
	_active_ctx.cancelled = true
	if _active_ctx.active_scene_tag > 0:
		DSPreloader.cancel_scene_prewarm(_active_ctx.active_scene_tag)
	elif _active_ctx.active_tag > 0:
		DSPreloader.cancel_prewarm(_active_ctx.active_tag)
		_active_ctx.active_tag = 0


func _on_launch_pressed() -> void:
	if _busy or not DemoAssetGenerator.assets_imported():
		return
	_busy = true

	var mode := _mode_select.get_selected_id() as LoadMode
	var mode_name := String(MODE_LABELS.get(mode, "Unknown"))
	var is_ds := not (mode in [LoadMode.VANILLA_SYNC, LoadMode.VANILLA_THREADED])
	var target_scene := DemoAssetGenerator.DS_SCENE_PATH if is_ds else DemoAssetGenerator.VANILLA_SCENE_PATH
	if ResourceLoader.has_cached(target_scene):
		_log("[color=yellow]Warning: %s was still in ResourceLoader cache before launch.[/color]" % target_scene)

	_loading_screen.reset_view(mode_name)
	_loading_screen.set_cancel_enabled(false)
	_menu_root.visible = false
	_loading_screen.visible = true
	await get_tree().process_frame

	var before_report := _snapshot_capability_report()
	var t_start_usec := Time.get_ticks_usec()
	_active_ctx = DemoLoadContext.new(get_tree(), target_scene, _loading_screen)
	_loading_screen.set_cancel_enabled(mode != LoadMode.VANILLA_SYNC)

	match mode:
		LoadMode.VANILLA_SYNC:
			VanillaSyncLoader.run(_active_ctx)
		LoadMode.VANILLA_THREADED:
			await VanillaThreadedLoader.run(_active_ctx)
		LoadMode.DS_CPU_PREWARM:
			await DSPrewarmLoader.run(_active_ctx, false)
		LoadMode.DS_GPU_PREWARM:
			await DSPrewarmLoader.run(_active_ctx, true)
		LoadMode.DS_GPU_PACED:
			await DSGpuPacedLoader.run(_active_ctx)

	if not _active_ctx.error_msg.is_empty():
		_log("[color=red]%s[/color]" % _active_ctx.error_msg)

	if _active_ctx.cancelled or _active_ctx.loaded_scene == null:
		var was_cancelled := _active_ctx.cancelled
		_active_ctx = null
		await _clear_all_caches()
		_loading_screen.visible = false
		_menu_root.visible = true
		if was_cancelled:
			_log("[color=yellow]Cancelled %s; caches cleared.[/color]" % mode_name)
		_busy = false
		_refresh_asset_status()
		return

	_loading_screen.set_cancel_enabled(false)
	_loading_screen.set_phase("Instantiating & Shader Warmup Frame...", 1.0, "Attaching scene...")
	var t_tree0 := Time.get_ticks_usec()
	_active_level = _active_ctx.loaded_scene.instantiate()
	_active_ctx.loaded_scene = null
	get_tree().root.add_child(_active_level)
	await _active_ctx.next_frame()
	await _active_ctx.await_frame_post_draw()
	_active_ctx.instantiate_ms += float(Time.get_ticks_usec() - t_tree0) / 1000.0

	var total_ms := float(Time.get_ticks_usec() - t_start_usec) / 1000.0
	var prewarm_ms := _active_ctx.prewarm_ms
	var instantiate_ms := _active_ctx.instantiate_ms
	var max_hitch_ms := _active_ctx.max_hitch_ms
	_active_ctx = null

	if _active_level.has_method("configure_hud"):
		_active_level.call("configure_hud", mode_name, total_ms)
	if _active_level.has_signal("return_requested"):
		_active_level.connect("return_requested", _on_level_return_requested)

	_loading_screen.visible = false

	var after_report := _snapshot_capability_report()
	var d_cpu := _sum_cpu_fallbacks(after_report) - _sum_cpu_fallbacks(before_report)
	var disk_mib := DemoAssetGenerator.get_variant_disk_mib(is_ds)
	var tex_counts := _count_scene_texture_types(_active_level)
	var ds_cached := int(Engine.get_singleton("DSTextureCache").call("get_cached_count")) if Engine.has_singleton("DSTextureCache") else 0

	_log("[b]%s[/b]: total [b]%.2f ms[/b] (load/prewarm %.2f ms + inst/frame %.2f ms) | [color=orange]max_hitch %.2f ms[/color] | disk %.1f MiB | tex(rd=%d img=%d ctex=%d) ds_cache=%d cpu_fb=%d" % [
		mode_name, total_ms, prewarm_ms, instantiate_ms, max_hitch_ms, disk_mib,
		int(tex_counts["gpu_rd"]), int(tex_counts["img_tex"]), int(tex_counts["ctex"]),
		ds_cached, d_cpu
	])
	_busy = false


func _on_level_return_requested() -> void:
	if _busy:
		return
	_busy = true
	if _active_level != null:
		_active_level.queue_free()
		_active_level = null
	await _clear_all_caches()
	_menu_root.visible = true
	_refresh_asset_status()
	_busy = false


func _clear_all_caches() -> void:
	for _i in 3:
		await get_tree().process_frame
	var before_count := -1
	var after_count := -1
	if Engine.has_singleton("DSTextureCache"):
		var cache := Engine.get_singleton("DSTextureCache")
		if cache.has_method("get_cached_count"):
			before_count = int(cache.call("get_cached_count"))
		if cache.has_method("clear_unused"):
			cache.call("clear_unused")
		elif cache.has_method("clear"):
			cache.call("clear")
		if cache.has_method("get_cached_count"):
			after_count = int(cache.call("get_cached_count"))
	for _i in 3:
		await get_tree().process_frame
	if before_count >= 0 and after_count >= 0:
		_log("[color=gray]DSTextureCache cleared: %d -> %d entries.[/color]" % [before_count, after_count])


func _snapshot_capability_report() -> Dictionary:
	if not Engine.has_singleton("DSContext"):
		return {}
	var ctx := Engine.get_singleton("DSContext")
	if ctx.has_method("get_capability_report"):
		var rep: Variant = ctx.call("get_capability_report")
		if rep is Dictionary:
			return rep as Dictionary
	return {}


func _sum_cpu_fallbacks(report: Dictionary) -> int:
	var raw: Variant = report.get("loader_cpu_fallbacks", {})
	if raw is int:
		return int(raw)
	if raw is Dictionary:
		var total := 0
		for v in (raw as Dictionary).values():
			total += int(v)
		return total
	return 0


func _count_scene_texture_types(root: Node) -> Dictionary:
	var counts := {"gpu_rd": 0, "img_tex": 0, "ctex": 0}
	if root == null:
		return counts
	var seen := {}
	for child in root.get_children():
		if not (child is MeshInstance3D):
			continue
		var mat := (child as MeshInstance3D).material_override as StandardMaterial3D
		if mat == null:
			continue
		for tex: Texture2D in [mat.albedo_texture, mat.normal_texture, mat.roughness_texture, mat.emission_texture]:
			if tex == null:
				continue
			var id: int = tex.get_instance_id()
			if seen.has(id):
				continue
			seen[id] = true
			if tex is Texture2DRD:
				counts["gpu_rd"] = int(counts["gpu_rd"]) + 1
			elif tex is ImageTexture:
				counts["img_tex"] = int(counts["img_tex"]) + 1
			elif tex is CompressedTexture2D:
				counts["ctex"] = int(counts["ctex"]) + 1
	return counts


func _log(bbcode: String) -> void:
	_results_box.append_text(bbcode + "\n")
