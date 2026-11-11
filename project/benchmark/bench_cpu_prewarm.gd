extends SceneTree

## Bulk System-RAM prewarm benchmark (opt-in, never a test).
##
## Question: how fast does the CPU prewarm path read + decompress chunked
## textures, and how do chunk size and staging size move it? This is the path a
## loading screen uses (prewarm_async -> flush_stream_async -> poll_completed),
## and it is the only way to measure the runtime's built-in CPU decompression
## pool, which the GPU-upload benchmark never touches.
##
## Unlike the GPU-upload bench, this path is frame-driven by design (DSRuntime
## submits/polls once per frame, and the pump posts at most one GPU chunk per
## call), so the loop awaits process_frame. A `-s` script that never yields
## stalls at progress (0, N) - that is why the first version of this experiment
## looked frozen.
##
## Fixtures come from the chunked-upload bench, which already writes the same
## chunked .dstex layout:
##   godot --path project --rendering-driver d3d12 -s res://benchmark/bench_chunked_upload.gd -- ^
##     --generate --chunk-list=8192,1024,256 --count=8
##
## Sweep (windowed D3D12 so the Win32 backend and its CPU pool are active):
##   godot --path project --rendering-driver d3d12 -s res://benchmark/bench_cpu_prewarm.gd -- ^
##     --chunk-list=8192,1024,256 --staging=16,32,64,128 --count=8 --repeats=2
##
## --mode=autoload (default) lets DSRuntime pump; --mode=manual disables it and
## calls flush_stream_async/wait_for_completion/poll_completed itself. Use manual
## to attribute a stall to the prewarm queue rather than to a host project's
## autoload timing.

const BYTES_PER_TEXTURE := 2048 * 2048 * 4

var _args := {}
var _exit := 0
var _ctx: Variant = null
var _cache: Variant = null


func _initialize() -> void:
	_args = _parse(OS.get_cmdline_user_args())
	await _sweep()
	quit(_exit)


func _parse(argv: PackedStringArray) -> Dictionary:
	var out := {}
	for raw in argv:
		var token := String(raw)
		if not token.begins_with("--"):
			continue
		token = token.substr(2)
		var eq := token.find("=")
		if eq == -1:
			out[token] = true
		else:
			out[token.substr(0, eq)] = token.substr(eq + 1)
	return out


func _ints(text: String, fallback: Array) -> Array:
	if text.is_empty():
		return fallback
	var out: Array = []
	for part in text.split(",", false):
		out.append(int(part.strip_edges()))
	return out if not out.is_empty() else fallback


func _fail(message: String) -> void:
	printerr("[BenchCpuPrewarm] FAIL: ", message)
	_exit = 1


func _seeded_shuffle(items: Array, seed_value: int) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = seed_value
	for i in range(items.size() - 1, 0, -1):
		var j := rng.randi_range(0, i)
		var tmp = items[i]
		items[i] = items[j]
		items[j] = tmp


func _sweep() -> void:
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSTextureCache"):
		_fail("DSContext/DSTextureCache are not registered (GDExtension not loaded)")
		return
	_ctx = Engine.get_singleton("DSContext")
	_cache = Engine.get_singleton("DSTextureCache")
	if _ctx == null or _cache == null:
		_fail("DSContext or DSTextureCache singleton is missing")
		return
	if RenderingServer.get_rendering_device() == null:
		_fail("no RenderingDevice (windowed D3D12 is required)")
		return

	var base := String(_args.get("base", "res://.bench_tmp"))
	var chunks := _ints(String(_args.get("chunk-list", "8192,1024,256")), [8192])
	var stagings := _ints(String(_args.get("staging", "32")), [32])
	var count := maxi(1, int(_args.get("count", 8)))
	var repeats := maxi(1, int(_args.get("repeats", 2)))
	var mode := String(_args.get("mode", "autoload"))
	var timeout_sec := maxf(5.0, float(_args.get("timeout-sec", 120)))

	var paths_by_chunk := {}
	for chunk_kb in chunks:
		var paths: Array[String] = []
		for i in count:
			var path := base.path_join("chunk_%d" % int(chunk_kb)).path_join("tex_%04d.dstex" % i)
			if not FileAccess.file_exists(path):
				_fail("missing fixture %s (run bench_chunked_upload.gd --generate first)" % path)
				return
			paths.append(path)
		paths_by_chunk[int(chunk_kb)] = PackedStringArray(paths)

	var arms: Array = []
	for chunk_kb in chunks:
		for staging_mb in stagings:
			arms.append([int(chunk_kb), int(staging_mb)])
	var results := {}
	for arm in arms:
		results["%d:%d" % [arm[0], arm[1]]] = []

	print("[BenchCpuPrewarm] mode=", mode, " count=", count, " repeats=", repeats,
		" chunks=", chunks, " stagings=", stagings, " timeout_sec=", timeout_sec)
	var warm: Array = arms[0]
	await _measure(warm[0], warm[1], paths_by_chunk[warm[0]], mode, timeout_sec, "warmup", false, results)

	var seed := int(_args.get("seed", 90210))
	for rep in repeats:
		var order: Array = arms.duplicate()
		_seeded_shuffle(order, seed + rep)
		for arm in order:
			await _measure(arm[0], arm[1], paths_by_chunk[arm[0]], mode, timeout_sec, "rep%d" % rep, true, results)

	print("[BenchCpuPrewarm] summary (min/median of ", repeats, " repeats)")
	for arm in arms:
		var key := "%d:%d" % [arm[0], arm[1]]
		var samples: Array = results[key]
		if samples.is_empty():
			continue
		samples.sort()
		var best: float = samples[0]
		var median: float = samples[samples.size() / 2]
		var mib := float(count * BYTES_PER_TEXTURE) / 1048576.0
		print("[BenchCpuPrewarm] RESULT chunk_kb=", arm[0], " staging_mb=", arm[1],
			" min_ms=", snappedf(best, 0.01),
			" median_ms=", snappedf(median, 0.01),
			" min_mib_per_s=", snappedf(mib / maxf(best / 1000.0, 0.000001), 0.1))
	_ctx.call("shutdown_ds")


func _pump_diagnostics(tag: int) -> String:
	return (" progress=" + str(_ctx.call("get_prewarm_progress", tag)) +
		" stream_pending=" + str(_ctx.call("has_stream_pending")) +
		" stream_count=" + str(_ctx.call("get_stream_pending_count")) +
		" batch_complete=" + str(_ctx.call("is_batch_complete")) +
		" pending=" + str(_cache.call("get_pending_prewarm_count", tag)) +
		" unbuilt=" + str(_cache.call("get_unbuilt_prewarm_count", tag)) +
		" unbuilt_cpu=" + str(_cache.call("get_unbuilt_cpu_prewarm_count", tag)))


func _measure(chunk_kb: int, staging_mb: int, paths: PackedStringArray, mode: String,
		timeout_sec: float, label: String, record: bool, results: Dictionary) -> void:
	# Manual mode owns the pump; park the host autoload so both do not flush.
	if mode == "manual":
		var runtime := root.get_node_or_null("DSRuntime")
		if runtime != null:
			runtime.set_process(false)

	_cache.call("clear")
	_ctx.call("shutdown_ds")
	ProjectSettings.set_setting("dsgd_extension/staging_buffer_size_mb", staging_mb)
	if not bool(_ctx.call("initialize_ds")):
		_fail("initialize_ds failed at staging=%d MiB" % staging_mb)
		return
	var backend: String = String(_ctx.call("get_active_backend_name"))
	if not bool(_ctx.call("is_d3d12_backend")):
		_fail("expected a D3D12 backend, got '%s'" % backend)
		return

	var tag := int(_cache.call("prewarm_async", paths))
	if tag == 0:
		_fail("prewarm_async returned tag 0 (backend=%s)" % backend)
		return

	var started := Time.get_ticks_usec()
	var frames := 0
	var timed_out := false
	var last_progress := Vector2i.ZERO
	while true:
		if mode == "manual":
			_ctx.call("flush_stream_async")
			_ctx.call("wait_for_completion", 100)
			_cache.call("poll_completed", tag, 0, 0)
		# The prewarm pump is frame-driven; this is the yield the first version
		# of this experiment was missing.
		await process_frame
		frames += 1
		var progress: Vector2i = _ctx.call("get_prewarm_progress", tag)
		last_progress = progress
		if bool(_ctx.call("is_prewarm_complete", tag)) and progress.y > 0 and progress.x == progress.y:
			break
		if float(Time.get_ticks_usec() - started) / 1000000.0 > timeout_sec:
			timed_out = true
			break

	var elapsed_ms := float(Time.get_ticks_usec() - started) / 1000.0
	var mib := float(paths.size() * BYTES_PER_TEXTURE) / 1048576.0
	var ok := not timed_out and last_progress.y > 0 and last_progress.x == last_progress.y
	print("[BenchCpuPrewarm] ", label, " chunk_kb=", chunk_kb, " staging_mb=", staging_mb,
		" backend=", backend, " textures=", paths.size(), " frames=", frames,
		" progress=", last_progress, " timed_out=", timed_out,
		" ms=", snappedf(elapsed_ms, 0.01),
		" mib_per_s=", snappedf(mib / maxf(elapsed_ms / 1000.0, 0.000001), 0.1),
		("" if ok else " diagnostics:" + _pump_diagnostics(tag)))
	if record and ok:
		results["%d:%d" % [chunk_kb, staging_mb]].append(elapsed_ms)
	if not ok:
		_exit = 1
