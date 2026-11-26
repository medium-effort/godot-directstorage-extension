extends Control

## Self-contained F5 A/B comparison scene:
##   DirectStorage (.dstex via DSResourceFormatLoader -> Texture2DRD / ImageTexture)
##   vs. Standard Godot CPU load (ResourceLoader.load pre-mipped Image .res -> ImageTexture).
##
## Caches deterministic high-entropy FastNoiseLite fixtures in user://ds_ab_bench/
## (outside res://, so both gitignored and gdignored) guarded by an atomic
## .ready.json stamp so partial/broken runs are never treated as cached.

const TMP_DIR := "user://ds_ab_bench"
const STAMP_PATH := TMP_DIR + "/.ready.json"
const FIXTURE_SCHEMA := 3
const TEX_W := 2048
const TEX_H := 2048
const TEX_COUNT := 3
const PASSES := 5

@onready var _backend_label: Label = %BackendLabel
@onready var _status_label: Label = %StatusLabel
@onready var _results_box: RichTextLabel = %ResultsBox
@onready var _preview_rect: TextureRect = %PreviewRect
@onready var _run_button: Button = %RunButton
@onready var _loading_demo_button: Button = %LoadingDemoButton

var _running := false


func _ready() -> void:
	_run_button.pressed.connect(_on_run_pressed)
	_loading_demo_button.pressed.connect(_on_loading_demo_pressed)
	_update_backend_banner()


func _on_loading_demo_pressed() -> void:
	if _running:
		return
	get_tree().change_scene_to_file("res://demo/loading_demo_menu.tscn")


func _on_run_pressed() -> void:
	if _running:
		return
	_running = true
	_run_button.disabled = true
	_results_box.clear()
	await _run_benchmark()
	_running = false
	_run_button.disabled = false


func _update_backend_banner() -> void:
	if not ClassDB.class_exists("DSContext"):
		_backend_label.text = "DSContext not loaded — build GDExtension first."
		_run_button.disabled = true
		return
	var ctx := Engine.get_singleton("DSContext")
	if not ctx.is_initialized():
		ctx.initialize_ds()
	var render_driver: String = RenderingServer.get_current_rendering_driver_name()
	var backend: String = String(ctx.get_active_backend_name())
	var ds_d3d12: bool = bool(ctx.is_d3d12_backend())
	var gpu_decomp: bool = bool(ctx.is_gpu_decompression_available())
	_backend_label.text = "Render Driver: %s | DS Backend: %s (DS D3D12: %s | GPU Decompress: %s)" % [
		render_driver, backend, str(ds_d3d12), str(gpu_decomp)
	]
	_status_label.text = "Ready (%d passes x %d textures @ %dx%d RGBA8)" % [PASSES, TEX_COUNT, TEX_W, TEX_H]


func _run_benchmark() -> void:
	var dstex_inst: Variant = ClassDB.instantiate("DSTex")
	if dstex_inst == null:
		_log("[color=red]DSTex class missing.[/color]")
		return

	var expected_stamp := {
		"schema": FIXTURE_SCHEMA,
		"w": TEX_W,
		"h": TEX_H,
		"count": TEX_COUNT,
		"passes": PASSES,
	}
	var pass_fixtures := _load_cached_fixtures(expected_stamp)
	if pass_fixtures.is_empty():
		_cleanup_tmp()
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TMP_DIR))
		var bytes_per_tex := 0
		for p in PASSES:
			var dstex_paths: Array[String] = []
			var res_paths: Array[String] = []
			for i in TEX_COUNT:
				_status_label.text = "Generating fixture %d/%d (pass %d/%d)..." % [i + 1, TEX_COUNT, p + 1, PASSES]
				await get_tree().process_frame
				var img := await _make_pbr_style_image(TEX_W, TEX_H, p * 100 + i + 1)
				if img == null or img.is_empty():
					_log("[color=red]Failed generating procedural texture[/color]")
					_cleanup_tmp()
					return
				img.generate_mipmaps()
				bytes_per_tex = img.get_data().size()
				var res_path := "%s/p%d_t%d.res" % [TMP_DIR, p, i]
				if ResourceSaver.save(img, res_path, ResourceSaver.FLAG_COMPRESS) != OK:
					_log("[color=red]Failed writing .res fixture[/color]")
					_cleanup_tmp()
					return
				var dstex_path := "%s/p%d_t%d.dstex" % [TMP_DIR, p, i]
				var mip_count: int = (img.get_mipmap_count() + 1) if img.has_mipmaps() else 1
				var err: Error = dstex_inst.write_to_file(
					dstex_path, img.get_data(), TEX_W, TEX_H, Image.FORMAT_RGBA8, mip_count, 1
				)
				if err != OK:
					_log("[color=red]Failed writing %s (%s)[/color]" % [dstex_path, error_string(err)])
					_cleanup_tmp()
					return
				dstex_paths.append(dstex_path)
				res_paths.append(res_path)
			pass_fixtures.append({"dstex": dstex_paths, "res": res_paths})
			await get_tree().process_frame

		expected_stamp["bytes_per_tex"] = bytes_per_tex
		var stamp_file := FileAccess.open(STAMP_PATH, FileAccess.WRITE)
		if stamp_file != null:
			stamp_file.store_string(JSON.stringify(expected_stamp))
			stamp_file.close()
	else:
		_log("[color=gray]Using cached fixtures from %s[/color]" % TMP_DIR)

	var ds_times_ms: Array[float] = []
	var cpu_times_ms: Array[float] = []
	var total_mib := float(int(expected_stamp.get("bytes_per_tex", TEX_W * TEX_H * 4)) * TEX_COUNT) / (1024.0 * 1024.0)

	# Uncounted warmup pass to stabilize GPU clocks and D3D12 pipelines.
	_status_label.text = "Warming up..."
	await _clear_caches()
	_run_ds_leg(pass_fixtures[0]["dstex"])
	await _clear_caches()
	_run_cpu_leg(pass_fixtures[0]["res"])

	for p in PASSES:
		var ds_first := (p % 2 == 0)
		var order := ["ds", "cpu"] if ds_first else ["cpu", "ds"]
		for leg in order:
			await _clear_caches()
			_status_label.text = "Pass %d/%d — running %s..." % [p + 1, PASSES, leg.to_upper()]
			await get_tree().process_frame

			var elapsed_ms := 0.0
			if leg == "ds":
				elapsed_ms = _run_ds_leg(pass_fixtures[p]["dstex"])
				ds_times_ms.append(elapsed_ms)
			else:
				elapsed_ms = _run_cpu_leg(pass_fixtures[p]["res"])
				cpu_times_ms.append(elapsed_ms)

			var mib_s := total_mib / maxf(elapsed_ms / 1000.0, 0.000001)
			_log("Pass %d %-3s: %7.2f ms  (%7.1f MiB/s)" % [p + 1, leg.to_upper(), elapsed_ms, mib_s])
			await get_tree().process_frame

	var ds_med := _median(ds_times_ms)
	var cpu_med := _median(cpu_times_ms)
	var ds_bw := total_mib / maxf(ds_med / 1000.0, 0.000001)
	var cpu_bw := total_mib / maxf(cpu_med / 1000.0, 0.000001)
	var ratio := cpu_med / maxf(ds_med, 0.001)

	var ds_disk_mib := _sum_file_mib(pass_fixtures[0]["dstex"])
	var cpu_disk_mib := _sum_file_mib(pass_fixtures[0]["res"])
	var ds_saved := total_mib / maxf(ds_disk_mib, 0.0001)
	var cpu_saved := total_mib / maxf(cpu_disk_mib, 0.0001)

	_log("")
	_log("[b]Summary (%.1f MiB uncompressed per leg, median of %d passes)[/b]" % [total_mib, PASSES])
	_log("  DirectStorage : %7.2f ms  (%7.1f MiB/s) | Disk: %.2f MiB (%.2fx saved)" % [ds_med, ds_bw, ds_disk_mib, ds_saved])
	_log("  Standard CPU  : %7.2f ms  (%7.1f MiB/s) | Disk: %.2f MiB (%.2fx saved)" % [cpu_med, cpu_bw, cpu_disk_mib, cpu_saved])
	_log("  Speedup       : [b]%.2fx[/b]" % ratio)
	_status_label.text = "Done (fixtures cached in %s)." % TMP_DIR


func _load_cached_fixtures(expected_stamp: Dictionary) -> Array:
	if not FileAccess.file_exists(STAMP_PATH):
		return []
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(STAMP_PATH))
	if not (parsed is Dictionary):
		return []
	var stamp := parsed as Dictionary
	for k in expected_stamp:
		if int(stamp.get(k, -1)) != int(expected_stamp[k]):
			return []
	var bpt := int(stamp.get("bytes_per_tex", 0))
	if bpt <= 0:
		return []
	expected_stamp["bytes_per_tex"] = bpt

	var pass_fixtures: Array = []
	for p in PASSES:
		var dstex_paths: Array[String] = []
		var res_paths: Array[String] = []
		for i in TEX_COUNT:
			var dstex_path := "%s/p%d_t%d.dstex" % [TMP_DIR, p, i]
			var res_path := "%s/p%d_t%d.res" % [TMP_DIR, p, i]
			if not FileAccess.file_exists(dstex_path) or FileAccess.get_size(dstex_path) <= 0:
				return []
			if not FileAccess.file_exists(res_path) or FileAccess.get_size(res_path) <= 0:
				return []
			dstex_paths.append(dstex_path)
			res_paths.append(res_path)
		pass_fixtures.append({"dstex": dstex_paths, "res": res_paths})
	return pass_fixtures


func _run_ds_leg(paths: Array[String]) -> float:
	var loaded: Array[Texture2D] = []
	var t0 := Time.get_ticks_usec()
	for p in paths:
		var tex := ResourceLoader.load(p, "Texture2D", ResourceLoader.CACHE_MODE_IGNORE) as Texture2D
		if tex != null:
			loaded.append(tex)
	var elapsed_ms := float(Time.get_ticks_usec() - t0) / 1000.0
	if not loaded.is_empty():
		_preview_rect.texture = loaded[loaded.size() - 1]
	loaded.clear()
	return elapsed_ms


func _run_cpu_leg(paths: Array[String]) -> float:
	var loaded: Array[Texture2D] = []
	var t0 := Time.get_ticks_usec()
	for p in paths:
		var img := ResourceLoader.load(p, "Image", ResourceLoader.CACHE_MODE_IGNORE) as Image
		if img != null and not img.is_empty():
			loaded.append(ImageTexture.create_from_image(img))
	var elapsed_ms := float(Time.get_ticks_usec() - t0) / 1000.0
	if not loaded.is_empty():
		_preview_rect.texture = loaded[loaded.size() - 1]
	loaded.clear()
	return elapsed_ms


func _clear_caches() -> void:
	_preview_rect.texture = null
	if Engine.has_singleton("DSTextureCache"):
		Engine.get_singleton("DSTextureCache").clear()
	for _i in 6:
		await get_tree().process_frame


func _make_pbr_style_image(w: int, h: int, seed_val: int) -> Image:
	var noise := FastNoiseLite.new()
	noise.seed = seed_val
	noise.noise_type = FastNoiseLite.TYPE_SIMPLEX_SMOOTH
	noise.frequency = 0.008 + float(seed_val % 5) * 0.002
	noise.fractal_type = FastNoiseLite.FRACTAL_FBM
	noise.fractal_octaves = 3
	noise.fractal_gain = 0.55

	var grad := Gradient.new()
	if seed_val % 2 == 0:
		grad.colors = PackedColorArray([Color(0.16, 0.17, 0.19), Color(0.38, 0.36, 0.33), Color(0.62, 0.58, 0.52)])
		grad.offsets = PackedFloat32Array([0.0, 0.45, 1.0])
	else:
		grad.colors = PackedColorArray([Color(0.12, 0.19, 0.18), Color(0.24, 0.42, 0.36), Color(0.55, 0.62, 0.48)])
		grad.offsets = PackedFloat32Array([0.0, 0.5, 1.0])

	var tex := NoiseTexture2D.new()
	tex.width = w
	tex.height = h
	tex.generate_mipmaps = false
	tex.seamless = false
	tex.noise = noise
	tex.color_ramp = grad
	await tex.changed
	var img := tex.get_image()
	if img != null and img.get_format() != Image.FORMAT_RGBA8:
		img.convert(Image.FORMAT_RGBA8)
	return img


func _sum_file_mib(paths: Array[String]) -> float:
	var total := 0
	for p in paths:
		total += maxi(0, FileAccess.get_size(p))
	return float(total) / (1024.0 * 1024.0)


func _median(values: Array[float]) -> float:
	var sorted := values.duplicate()
	sorted.sort()
	var n := sorted.size()
	if n == 0:
		return 0.0
	var mid := n >> 1
	if n % 2 == 1:
		return sorted[mid]
	return (sorted[mid - 1] + sorted[mid]) * 0.5


func _log(bbcode: String) -> void:
	_results_box.append_text(bbcode + "\n")


func _cleanup_tmp() -> void:
	var abs_dir := ProjectSettings.globalize_path(TMP_DIR)
	if not DirAccess.dir_exists_absolute(abs_dir):
		return
	var da := DirAccess.open(abs_dir)
	if da == null:
		return
	da.list_dir_begin()
	var entry := da.get_next()
	while not entry.is_empty():
		if entry != "." and entry != ".." and not da.current_is_dir():
			DirAccess.remove_absolute(abs_dir.path_join(entry))
		entry = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs_dir)
