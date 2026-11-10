extends SceneTree

## GPU-contention streaming benchmark (opt-in, never a test).
##
## Question: does streaming cost frames while the GPU is already busy?
## bench_throughput.gd measures MB/s; this measures frame time per phase.
##
## Methodology (added after the first, inconclusive sweep):
##   * --repeats copies of the same stream, each bracketed by idle gap frames,
##     reported per repeat plus the median delta across repeats.
##   * --warmup-frames runs first; the first --discard-frames are thrown away so
##     pipeline/window settling cannot poison the baseline.
##   * Median and p95 frame time per phase (not means): a few outliers decide
##     the mean on a short stream.
##   * --window-size sizes the window (and therefore the synthetic load);
##     raise --gpu-load to saturate a fast GPU.
##
## One arm per process (DirectStorage configuration is process-global). Run the
## same command with --gpu-load=0 and --gpu-load=N on one machine, in one sitting.
##
## Win32Backend::initialize() refuses to run without a live D3D12
## RenderingDevice, so this MUST run windowed with --rendering-driver d3d12.
## --expect-backend turns a silent software fallback into a hard failure.
##
## Fixtures are single-mip .dstex files. The codec is read from each header, so
## both GDeflate and the writer's uncompressed (NONE) fallback are valid.
##
## Example:
##   godot --path project --rendering-driver d3d12 -s res://benchmark/bench_gpu_contention.gd -- ^
##     --fixture-dir=H:/test_here/gpu_fixtures --count=32 --batch=4 ^
##     --gpu-load=512 --window-size=1920x1080 --repeats=3 ^
##     --expect-backend=Win32DirectStorage --out=user://bench_gpu_contention.json
##
## Compare within one sitting only: numbers are machine- and driver-specific.

const LOAD_SHADER := preload("res://benchmark/shaders/load_fractal.gdshader")

const COMPRESSION_NONE := 0
const COMPRESSION_GDEFLATE := 1
const DESTINATION_GPU_TEXTURE := 2
const PRIORITY_NORMAL := 0

var _errors := 0
var _ctx: Variant = null
var _profiler: Object = null
var _runtime: Node = null
var _runtime_was_processing := false
var _canvas: ColorRect = null
var _textures: Array[RID] = []
var _rd: RenderingDevice = null
var _last_usec := 0


func _initialize() -> void:
	await _run()
	_cleanup()
	quit(0 if _errors == 0 else 1)


func _run() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	var dir := String(args.get("fixture-dir", ""))
	if dir.is_empty():
		_fail("missing --fixture-dir=<absolute dir> holding tex_%04d.dstex files")
		return
	var count := maxi(1, int(args.get("count", 32)))
	var batch := clampi(int(args.get("batch", 4)), 1, 64)
	var gpu_load := maxi(0, int(args.get("gpu-load", 0)))
	var warmup_frames := maxi(0, int(args.get("warmup-frames", 300)))
	var discard_frames := clampi(int(args.get("discard-frames", 30)), 0, warmup_frames)
	var gap_frames := maxi(1, int(args.get("gap-frames", 60)))
	var repeats := clampi(int(args.get("repeats", 3)), 1, 10)
	var max_frames := maxi(60, int(args.get("max-frames", 20000)))
	var window_size := _parse_size(String(args.get("window-size", "1920x1080")))
	var expect := String(args.get("expect-backend", ""))
	var out_path := String(args.get("out", "user://bench_gpu_contention.json"))

	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest"):
		_fail("DSContext/DSRequest are not registered (GDExtension not loaded)")
		return

	_ctx = Engine.get_singleton("DSContext")
	if _ctx == null:
		_fail("DSContext singleton is missing")
		return
	_ctx.shutdown_ds()
	if not bool(_ctx.initialize_ds()) or not bool(_ctx.is_initialized()):
		_fail("initialize_ds failed")
		return

	var backend: String = _ctx.get_active_backend_name()
	var driver := RenderingServer.get_current_rendering_driver_name()
	var d3d12: bool = _ctx.is_d3d12_backend()
	print("[BenchContention] backend=", backend, " driver=", driver, " d3d12=", d3d12,
		" gpu_decompression=", _ctx.is_gpu_decompression_available(),
		" gpu_load=", gpu_load)
	if not expect.is_empty() and backend != expect:
		_fail("expected backend '%s' but got '%s' (windowed D3D12 launch + no env seam)" % [expect, backend])
		return
	if not d3d12:
		_fail("D3D12 backend is required; run windowed with --rendering-driver d3d12")
		return

	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		_fail("RenderingDevice is unavailable (windowed D3D12 required)")
		return

	# Frame-time deltas need an uncapped frame rate; with VSync on every arm
	# would flatten to 16.7 ms and the load would be invisible.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	root.size = window_size
	DisplayServer.window_set_size(window_size)

	_profiler = Engine.get_singleton("DSProfiler")
	if _profiler == null or not _profiler.has_method("record_frame") or not _profiler.has_method("begin_load_window"):
		_fail("DSProfiler frame-window API is missing (rebuild the extension)")
		return
	# The DSRuntime autoload would sample the same frames; this benchmark owns
	# the sampler and flushes the stream itself.
	_runtime = root.get_node_or_null("DSRuntime")
	if _runtime != null:
		_runtime_was_processing = _runtime.is_processing()
		_runtime.set_process(false)

	var fixtures: Array[String] = []
	var payloads: Array[Dictionary] = []
	for i in count:
		var path := dir.path_join("tex_%04d.dstex" % i)
		if not FileAccess.file_exists(path):
			_fail("missing GPU fixture " + path)
			return
		var header: Dictionary = DSTex.new().read_header(path)
		if header.is_empty() or int(header.get("mip_count", 0)) < 1:
			_fail("cannot parse fixture header: " + path)
			return
		fixtures.append(path)
		payloads.append({
			"offset": int((header["mip_offsets"] as PackedInt64Array)[0]),
			"size": int((header["mip_sizes"] as PackedInt32Array)[0]),
			"uncompressed": int((header["mip_uncompressed_sizes"] as PackedInt32Array)[0]),
			"codec": int(header.get("compression_format", COMPRESSION_GDEFLATE)),
			"width": int(header.get("width", 0)),
			"height": int(header.get("height", 0)),
		})

	var codec_none := 0
	var codec_gdeflate := 0
	for p in payloads:
		if int(p["codec"]) == COMPRESSION_NONE:
			codec_none += 1
		else:
			codec_gdeflate += 1
	var first := payloads[0]
	print("[BenchContention] fixtures=", fixtures.size(), " codec_none=", codec_none,
		" codec_gdeflate=", codec_gdeflate, " size=", first["width"], "x", first["height"],
		" window=", window_size.x, "x", window_size.y, " batch=", batch,
		" repeats=", repeats, " warmup=", warmup_frames, " discard=", discard_frames,
		" gap=", gap_frames,
		" staging_mb=", ProjectSettings.get_setting("dsgd_extension/staging_buffer_size_mb", -1),
		" queue_capacity=", ProjectSettings.get_setting("dsgd_extension/queue_capacity", -1))

	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = int(first["width"])
	texture_format.height = int(first["height"])
	texture_format.mipmaps = 1
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	for i in fixtures.size():
		var tex := _rd.texture_create(texture_format, RDTextureView.new(), [])
		if not tex.is_valid():
			_fail("GPU texture creation failed at %d" % i)
			return
		_textures.append(tex)

	if gpu_load > 0:
		_install_gpu_load(gpu_load)

	# Warmup: settle the pipeline and the window, then discard the first frames.
	var discard: Array[float] = []
	_last_usec = Time.get_ticks_usec()
	for i in warmup_frames:
		var delta := _tick_delta_usec()
		_profiler.record_frame(delta / 1000000.0)
		if i >= discard_frames:
			discard.append(delta)
		await process_frame

	var logical_bytes := 0
	for p in payloads:
		logical_bytes += int(p["uncompressed"])

	var repeat_results: Array = []
	var deltas: Array[float] = []
	var p95_deltas: Array[float] = []
	var previous_order_seed := int(args.get("seed", 123456789))
	for rep in repeats:
		# Idle gap: rebuild a current baseline for this repeat, so a repeat is
		# never compared against a baseline from minutes ago.
		var before: Array[float] = []
		for g in gap_frames:
			var gap_delta := _tick_delta_usec()
			_profiler.record_frame(gap_delta / 1000000.0)
			before.append(gap_delta)
			await process_frame

		var order := _make_order(fixtures.size(), previous_order_seed + rep)
		var during: Array[float] = []
		var stream_start_usec := Time.get_ticks_usec()
		_profiler.call("begin_load_window")
		var converged: bool = await _stream_once(order, payloads, fixtures, batch, max_frames, during)
		_profiler.call("end_load_window")
		var stream_seconds := float(Time.get_ticks_usec() - stream_start_usec) / 1000000.0
		if not converged:
			_fail("repeat %d: stream did not converge within %d frames" % [rep, max_frames])
			break

		var before_med := _median(before)
		var during_med := _median(during)
		var during_p95 := _percentile(_sorted_copy(during), 0.95)
		var delta_med := during_med - before_med
		deltas.append(delta_med)
		p95_deltas.append(during_p95 - before_med)
		var entry := {
			"repeat": rep,
			"before_med_usec": before_med,
			"during_med_usec": during_med,
			"during_p95_usec": during_p95,
			"delta_med_usec": delta_med,
			"delta_p95_usec": during_p95 - before_med,
			"frames": during.size(),
			"before_frames": before.size(),
			"stream_seconds": stream_seconds,
			"logical_mb_s": (float(logical_bytes) / (1024.0 * 1024.0)) / maxf(stream_seconds, 0.000001),
		}
		repeat_results.append(entry)
		print("[BenchContention] repeat=%d before_med=%.1f during_med=%.1f during_p95=%.1f delta_med=%.1f frames=%d stream_s=%.4f mb_s=%.2f" % [
			rep, before_med, during_med, during_p95, delta_med, during.size(),
			stream_seconds, float(entry["logical_mb_s"])])

	var delta_median := _median(deltas)
	var p95_delta_median := _median(p95_deltas)
	var result := {
		"backend": backend,
		"driver": driver,
		"gpu_load": gpu_load,
		"fixtures": fixtures.size(),
		"batch": batch,
		"codec_none": codec_none,
		"codec_gdeflate": codec_gdeflate,
		"window": [window_size.x, window_size.y],
		"warmup_frames": warmup_frames,
		"discard_frames": discard_frames,
		"gap_frames": gap_frames,
		"repeats": repeat_results,
		"delta_med_usec_median": delta_median,
		"delta_p95_usec_median": p95_delta_median,
		"discarded_warmup_med_usec": _median(discard),
	}
	print("[BenchContention] SUMMARY gpu_load=%d codec_none=%d codec_gdeflate=%d repeats=%d delta_med_us=%.1f delta_p95_us=%.1f" % [
		gpu_load, codec_none, codec_gdeflate, repeat_results.size(), delta_median, p95_delta_median])

	var out := FileAccess.open(out_path, FileAccess.WRITE)
	if out == null:
		_fail("cannot write %s" % out_path)
		return
	out.store_string(JSON.stringify(result, "  "))
	out = null
	print("[BenchContention] wrote ", ProjectSettings.globalize_path(out_path))


## Streams every fixture once. `r_during` receives one frame delta per frame so
## the caller can take percentiles; returns false when it did not converge.
func _stream_once(p_order: Array, p_payloads: Array[Dictionary], p_fixtures: Array[String],
		p_batch: int, p_max_frames: int, r_during: Array[float]) -> bool:
	var queued := 0
	var frames := 0
	_last_usec = Time.get_ticks_usec()
	while true:
		if queued < p_fixtures.size():
			var n := mini(p_batch, p_fixtures.size() - queued)
			var all_accepted := true
			for i in n:
				var idx: int = p_order[queued + i]
				var payload: Dictionary = p_payloads[idx]
				var req: Variant = ClassDB.instantiate("DSRequest")
				req.set_file_path(p_fixtures[idx])
				req.set_offset(payload["offset"])
				req.set_size(payload["size"])
				req.set_uncompressed_size(payload["uncompressed"])
				req.set_compression_format(payload["codec"])
				req.set_destination_type(DESTINATION_GPU_TEXTURE)
				req.set_destination_resource(_textures[idx])
				req.set_first_subresource(0)
				req.set_subresource_count(1)
				# 0 => the backend skips its driver-format equality check.
				req.set_texture_format(0)
				req.set_priority(PRIORITY_NORMAL)
				if not bool(_ctx.enqueue_request(req, true)):
					_fail("enqueue_request failed: " + String(req.get_error_message()))
					all_accepted = false
					break
			if not all_accepted:
				return false
			var flush_err: int = int(_ctx.flush_stream_async())
			if flush_err != OK and flush_err != ERR_BUSY:
				_fail("flush_stream_async failed with error %d" % flush_err)
				return false
			# OK or ERR_BUSY both mean the requests belong to the stream queue:
			# a transient rejection keeps them staged, so advancing here is what
			# avoids re-enqueueing duplicates.
			queued += n

		if bool(_ctx.is_batch_complete()) and bool(_ctx.has_pending_results()):
			_ctx.collect_completed_results()

		var delta := _tick_delta_usec()
		_profiler.record_frame(delta / 1000000.0)
		r_during.append(delta)
		frames += 1

		if queued >= p_fixtures.size() and not bool(_ctx.has_stream_pending()) and bool(_ctx.is_batch_complete()):
			return true
		if frames > p_max_frames:
			return false
		await process_frame
	return false


func _install_gpu_load(iterations: int) -> void:
	# Full-window fragment work. A Control parented to the root Window needs no
	# scene tree plumbing, and the shader is the whole workload.
	_canvas = ColorRect.new()
	_canvas.name = "BenchGpuLoad"
	_canvas.set_anchors_preset(Control.PRESET_FULL_RECT)
	_canvas.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var material := ShaderMaterial.new()
	material.shader = LOAD_SHADER
	material.set_shader_parameter("iterations", iterations)
	_canvas.material = material
	root.add_child(_canvas)


func _tick_delta_usec() -> float:
	var now := Time.get_ticks_usec()
	var delta := float(now - _last_usec)
	_last_usec = now
	return delta


func _make_order(count: int, seed: int) -> Array:
	# Shuffled streaming order: sequential order would hit the tail of the last
	# write's cache and flatter the numbers on every arm.
	var order := []
	for i in count:
		order.append(i)
	var rng := seed & 0xFFFFFFFF
	if rng == 0:
		rng = 0x9E3779B9
	for i in range(order.size() - 1, 0, -1):
		rng ^= (rng << 13) & 0xFFFFFFFF
		rng ^= rng >> 17
		rng ^= (rng << 5) & 0xFFFFFFFF
		var j := (rng & 0x7FFFFFFF) % (i + 1)
		var tmp: int = order[i]
		order[i] = order[j]
		order[j] = tmp
	return order


func _sorted_copy(p_values: Array[float]) -> Array[float]:
	var copy := p_values.duplicate()
	copy.sort()
	return copy


func _median(p_values: Array[float]) -> float:
	if p_values.is_empty():
		return 0.0
	return _percentile(_sorted_copy(p_values), 0.5)


func _percentile(p_sorted: Array[float], p_ratio: float) -> float:
	if p_sorted.is_empty():
		return 0.0
	var idx := clampi(int(round(float(p_sorted.size() - 1) * p_ratio)), 0, p_sorted.size() - 1)
	return p_sorted[idx]


func _parse_size(p_text: String) -> Vector2i:
	var parts := p_text.to_lower().split("x")
	if parts.size() != 2:
		return Vector2i(1920, 1080)
	var w := maxi(320, int(parts[0]))
	var h := maxi(200, int(parts[1]))
	return Vector2i(w, h)


func _parse_args(argv: PackedStringArray) -> Dictionary:
	var parsed := {}
	for raw in argv:
		var arg := String(raw)
		if not arg.begins_with("--"):
			continue
		arg = arg.substr(2)
		var eq := arg.find("=")
		if eq == -1:
			parsed[arg] = "1"
		else:
			parsed[arg.substr(0, eq)] = arg.substr(eq + 1)
	return parsed


func _cleanup() -> void:
	if _rd != null:
		for tex in _textures:
			if _rd.texture_is_valid(tex):
				_rd.free_rid(tex)
	_textures.clear()
	if _canvas != null:
		_canvas.queue_free()
		_canvas = null
	if _runtime != null:
		_runtime.set_process(_runtime_was_processing)
		_runtime = null
	if _ctx != null and bool(_ctx.is_initialized()):
		_ctx.shutdown_ds()


func _fail(message: String) -> void:
	_errors += 1
	printerr("[BenchContention] FAIL ", message)
