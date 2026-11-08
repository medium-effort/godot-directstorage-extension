extends SceneTree

## Backend I/O-mode A/B probe (benchmark only; not shipped behaviour).
##
## Drives identical DESTINATION_MEMORY requests through whichever backend
## DSContext selected, so the same fixture file can be measured with:
##   * DirectStorage default (BypassIO where available, unbuffered otherwise)
##   * DirectStorage ForceFileBuffering (debug seam DSGD_BENCH_FILE_BUFFERING=1)
##   * FallbackBackend (debug seam DSGD_BENCH_FORCE_FALLBACK=1)
##
## Win32Backend::initialize() refuses to run without a live D3D12
## RenderingDevice, so this scene MUST run windowed with --rendering-driver
## d3d12. A headless run silently lands on the software backend;
## --expect-backend turns that into a hard failure instead of bogus numbers.
##
## One arm per process: DirectStorage configuration is process-global and can
## only be applied before DStorageGetFactory.
##
## Example:
##   godot --path project --rendering-driver d3d12 ^
##     -s res://benchmark/bench_backend_modes.gd -- ^
##     --fixture=H:/test_here/bench_io.bin --pattern=seq --bytes=536870912 ^
##     --request-kb=1024 --batch=32 --iterations=2 ^
##     --expect-backend=Win32DirectStorage

const KB := 1024
const SECTOR := 4096
const DESTINATION_MEMORY := 0
const DESTINATION_GPU_TEXTURE := 2
const COMPRESSION_NONE := 0
const COMPRESSION_GDEFLATE := 1
const PRIORITY_NORMAL := 0

var _errors := 0
var _results: Array = []


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())

	var template_path := String(args.get("make-dstex-template", ""))
	if not template_path.is_empty():
		_make_dstex_template(template_path, maxi(16, int(args.get("texture-size", 1024))))
		return

	var print_drive := String(args.get("print-drive", ""))
	if not print_drive.is_empty():
		var probe_ctx: Variant = Engine.get_singleton("DSContext")
		if probe_ctx == null:
			_fail("DSContext singleton is missing")
			return
		print("[BenchIO] drive_info=", probe_ctx.get_drive_info(print_drive))
		quit(0)
		return

	var destination := String(args.get("destination", "memory"))
	var fixture := String(args.get("fixture", ""))
	if destination == "gpu-texture":
		if String(args.get("fixture-dir", "")).is_empty():
			_fail("missing --fixture-dir=<absolute dir> for --destination=gpu-texture")
			return
	elif fixture.is_empty():
		_fail("missing --fixture=<absolute path>")
		return
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest"):
		_fail("DSContext/DSRequest are not registered (GDExtension not loaded)")
		return

	var ctx: Variant = Engine.get_singleton("DSContext")
	if ctx == null:
		_fail("DSContext singleton is missing")
		return

	# Force a clean selection for this process before reading the backend name.
	ctx.shutdown_ds()
	var init_path := String(args.get("init-path", ""))
	var ok := false
	if init_path.is_empty():
		ok = bool(ctx.initialize_ds())
	else:
		# 0 == OK. Path-aware initialization is what exercises the production
		# media detection (rotational -> ForceFileBuffering) without an env seam.
		ok = int(ctx.initialize_ds_for_path(init_path)) == 0
	if not ok or not ctx.is_initialized():
		_fail("initialize_ds failed")
		return

	var backend: String = ctx.get_active_backend_name()
	var expect := String(args.get("expect-backend", ""))
	var driver := RenderingServer.get_current_rendering_driver_name()
	print("[BenchIO] backend=", backend, " driver=", driver,
		" d3d12=", ctx.is_d3d12_backend(),
		" gpu_decompression=", ctx.is_gpu_decompression_available())
	if not expect.is_empty() and backend != expect:
		_fail("expected backend '%s' but got '%s' (check the env seam and the D3D12 windowed launch)" % [expect, backend])
		return

	if destination == "gpu-texture":
		_run_gpu(ctx, args)
		return

	var file := FileAccess.open(fixture, FileAccess.READ)
	if file == null:
		_fail("cannot open fixture '%s' (error %d)" % [fixture, FileAccess.get_open_error()])
		return
	var file_size := file.get_length()
	file = null

	var pattern := String(args.get("pattern", "seq"))
	if pattern != "seq" and pattern != "rand":
		_fail("--pattern must be seq or rand")
		return
	var request_size := maxi(SECTOR, int(args.get("request-kb", 1024)) * KB)
	request_size -= request_size % SECTOR
	var batch := clampi(int(args.get("batch", 32)), 1, 1024)
	var iterations := maxi(1, int(args.get("iterations", 2)))
	var total_bytes := int(args.get("bytes", 512 * 1024 * KB))
	total_bytes -= total_bytes % request_size
	if total_bytes <= 0 or total_bytes > file_size:
		_fail("--bytes=%d does not fit fixture size %d" % [total_bytes, file_size])
		return
	var seed := int(args.get("seed", 123456789))

	print("[BenchIO] fixture=", fixture, " file_size=", file_size,
		" pattern=", pattern, " request_bytes=", request_size,
		" batch=", batch, " iterations=", iterations, " total_bytes=", total_bytes,
		" staging_mb=", ProjectSettings.get_setting("directstorage/staging_buffer_size_mb", -1),
		" queue_capacity=", ProjectSettings.get_setting("directstorage/queue_capacity", -1))

	for iteration in iterations:
		# Same seed for every pass: pass 1 must re-read exactly the blocks pass 0
		# touched, otherwise a warm-cache arm and a cold-cache arm measure
		# different offsets and the comparison is meaningless.
		var pass_result := _run_pass(ctx, fixture, file_size, pattern, request_size, batch,
			total_bytes, seed)
		if pass_result.is_empty():
			return
		pass_result["pass"] = iteration
		pass_result["backend"] = backend
		pass_result["pattern"] = pattern
		_results.append(pass_result)
		print("[BenchIO] backend=%s pass=%d pattern=%s request_kb=%d batch=%d bytes=%d seconds=%.4f mb_s=%.2f requests=%d batches=%d min_batch_ms=%.3f median_batch_ms=%.3f max_batch_ms=%.3f" % [
			backend, iteration, pattern, request_size / KB, batch,
			int(pass_result["bytes"]), float(pass_result["seconds"]),
			float(pass_result["mb_s"]), int(pass_result["requests"]),
			int(pass_result["batches"]), float(pass_result["min_batch_ms"]),
			float(pass_result["median_batch_ms"]), float(pass_result["max_batch_ms"])])

	var out_path := String(args.get("out", "user://bench_backend_modes.json"))
	var out := FileAccess.open(out_path, FileAccess.WRITE)
	if out != null:
		out.store_string(JSON.stringify({
			"backend": backend,
			"driver": driver,
			"fixture": fixture,
			"file_size": file_size,
			"runs": _results,
		}, "  "))
		out = null
		print("[BenchIO] wrote ", ProjectSettings.globalize_path(out_path))

	quit(0 if _errors == 0 else 1)


func _run_pass(ctx: Variant, fixture: String, file_size: int, pattern: String,
		request_size: int, batch: int, total_bytes: int, seed: int) -> Dictionary:
	var count := total_bytes / request_size
	var offsets := _make_offsets(pattern, count, request_size, file_size, seed)
	var batch_times_ms: Array[float] = []
	var processed := 0

	while processed < count:
		var n := mini(batch, count - processed)
		var requests: Array = []
		requests.resize(n)
		for i in n:
			var req: Variant = ClassDB.instantiate("DSRequest")
			req.set_file_path(fixture)
			req.set_offset(offsets[processed + i])
			req.set_size(request_size)
			req.set_uncompressed_size(request_size)
			req.set_compression_format(COMPRESSION_NONE)
			req.set_destination_type(DESTINATION_MEMORY)
			req.set_priority(PRIORITY_NORMAL)
			req.set_destination_buffer(PackedByteArray())
			requests[i] = req

		var start_usec := Time.get_ticks_usec()
		for req in requests:
			if not ctx.enqueue_request(req):
				_fail("enqueue_request failed: " + String(req.get_error_message()))
				return {}
		ctx.flush_batch()
		if not ctx.is_batch_complete():
			ctx.wait_for_completion(15000)
		var elapsed_usec := Time.get_ticks_usec() - start_usec

		for req in requests:
			if int(req.get_status()) != int(DSRequest.STATUS_COMPLETED):
				_fail("request finished with status %d: %s" % [
					int(req.get_status()), String(req.get_error_message())])
				return {}

		batch_times_ms.append(float(elapsed_usec) / 1000.0)
		processed += n
	ctx.collect_completed_results()

	var total_seconds := 0.0
	for t in batch_times_ms:
		total_seconds += t / 1000.0
	batch_times_ms.sort()
	var median := 0.0
	if not batch_times_ms.is_empty():
		median = batch_times_ms[batch_times_ms.size() / 2]

	return {
		"bytes": count * request_size,
		"requests": count,
		"batches": batch_times_ms.size(),
		"seconds": total_seconds,
		"mb_s": (float(count * request_size) / (1024.0 * 1024.0)) / maxf(total_seconds, 0.000001),
		"min_batch_ms": batch_times_ms[0] if not batch_times_ms.is_empty() else 0.0,
		"median_batch_ms": median,
		"max_batch_ms": batch_times_ms[batch_times_ms.size() - 1] if not batch_times_ms.is_empty() else 0.0,
	}


func _make_dstex_template(path: String, size: int) -> void:
	if not ClassDB.class_exists("DSTex"):
		_fail("DSTex is not registered")
		return
	# Incompressible pixels on purpose: a ramp would compress to almost nothing
	# and make the GPU-decompression arm meaningless.
	var bytes := PackedByteArray()
	bytes.resize(size * size * 4)
	var rng := 0x12345678
	for i in range(0, bytes.size(), 4):
		rng ^= (rng << 13) & 0xFFFFFFFF
		rng ^= rng >> 17
		rng ^= (rng << 5) & 0xFFFFFFFF
		bytes[i] = rng & 0xFF
		bytes[i + 1] = (rng >> 8) & 0xFF
		bytes[i + 2] = (rng >> 16) & 0xFF
		bytes[i + 3] = (rng >> 24) & 0xFF
	var dstex: Variant = ClassDB.instantiate("DSTex")
	var err: int = dstex.write_to_file(path, bytes, size, size, Image.FORMAT_RGBA8, 1, 1, false, false)
	if err != OK:
		_fail("DSTex.write_to_file failed with error %d" % err)
		return
	var file := FileAccess.open(path, FileAccess.READ)
	var length := file.get_length()
	file = null
	print("[BenchIO] dstex_template=", path, " size=", length,
		" aligned4096=", length % SECTOR == 0, " texture=", size, "x", size)
	quit(0)


func _run_gpu(ctx: Variant, args: Dictionary) -> void:
	var dir := String(args.get("fixture-dir", ""))
	var count := maxi(1, int(args.get("count", 64)))
	var size := maxi(16, int(args.get("texture-size", 1024)))
	var batch := clampi(int(args.get("batch", 16)), 1, 256)
	var iterations := maxi(1, int(args.get("iterations", 2)))
	var files: Array[String] = []
	var file_sizes: Array[int] = []
	var file_bytes := 0
	for i in count:
		var fpath := dir.path_join("tex_%04d.dstex" % i)
		if not FileAccess.file_exists(fpath):
			_fail("missing GPU fixture " + fpath)
			return
		var file := FileAccess.open(fpath, FileAccess.READ)
		file_sizes.append(file.get_length())
		file = null
		files.append(fpath)
		file_bytes += file_sizes[file_sizes.size() - 1]

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail("RenderingDevice is unavailable (windowed D3D12 required)")
		return

	var textures: Array[RID] = []
	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = size
	texture_format.height = size
	texture_format.mipmaps = 1
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT
	for i in count:
		var tex := rd.texture_create(texture_format, RDTextureView.new(), [])
		if not tex.is_valid():
			_fail("GPU texture creation failed at %d" % i)
			return
		textures.append(tex)

	var logical_bytes := count * size * size * 4
	# Shuffled read order: after an unbuffered write of a set larger than the
	# drive's own cache, sequential order would still hit the tail of that cache.
	var order := _make_order(count, int(args.get("seed", 123456789)))
	print("[BenchIO] destination=gpu-texture textures=", count, " texture_bytes=", logical_bytes,
		" file_bytes=", file_bytes, " batch=", batch, " iterations=", iterations,
		" staging_mb=", ProjectSettings.get_setting("directstorage/staging_buffer_size_mb", -1))

	for iteration in iterations:
		var pass_result := _run_gpu_pass(ctx, files, file_sizes, textures, order, batch, size,
			logical_bytes, file_bytes)
		if pass_result.is_empty():
			return
		var backend: String = ctx.get_active_backend_name()
		pass_result["pass"] = iteration
		pass_result["backend"] = backend
		pass_result["pattern"] = "gpu-texture"
		_results.append(pass_result)
		print("[BenchIO] backend=%s pass=%d pattern=gpu-texture texture=%dx%d batch=%d bytes=%d file_bytes=%d seconds=%.4f mb_s=%.2f file_mb_s=%.2f requests=%d batches=%d min_batch_ms=%.3f median_batch_ms=%.3f max_batch_ms=%.3f" % [
			backend, iteration, size, size, batch, logical_bytes, file_bytes,
			float(pass_result["seconds"]), float(pass_result["mb_s"]),
			float(pass_result["file_mb_s"]), int(pass_result["requests"]),
			int(pass_result["batches"]), float(pass_result["min_batch_ms"]),
			float(pass_result["median_batch_ms"]), float(pass_result["max_batch_ms"])])

	# Safe only after the final completion fence: freeing while hardware still
	# owns a texture page-faults the DMA engine.
	for tex in textures:
		if tex.is_valid():
			rd.free_rid(tex)
	textures.clear()

	var out_path := String(args.get("out", "user://bench_backend_modes.json"))
	var out := FileAccess.open(out_path, FileAccess.WRITE)
	if out != null:
		out.store_string(JSON.stringify({
			"backend": ctx.get_active_backend_name(),
			"destination": "gpu-texture",
			"texture_size": size,
			"file_bytes": file_bytes,
			"runs": _results,
		}, "  "))
		out = null
		print("[BenchIO] wrote ", ProjectSettings.globalize_path(out_path))

	quit(0 if _errors == 0 else 1)


func _run_gpu_pass(ctx: Variant, files: Array[String], file_sizes: Array[int],
		textures: Array[RID], order: PackedInt32Array, batch: int, size: int,
		logical_bytes: int, file_bytes: int) -> Dictionary:
	var count := files.size()
	var uncompressed := size * size * 4
	var batch_times_ms: Array[float] = []
	var processed := 0

	while processed < count:
		var n := mini(batch, count - processed)
		var requests: Array = []
		for i in n:
			var idx := order[processed + i]
			var req: Variant = ClassDB.instantiate("DSRequest")
			req.set_file_path(files[idx])
			# Pre-split validation only; the backend replaces both from the
			# parsed .dstex mip entry once it splits the request.
			req.set_size(file_sizes[idx])
			req.set_uncompressed_size(uncompressed)
			req.set_compression_format(COMPRESSION_GDEFLATE)
			req.set_destination_type(DESTINATION_GPU_TEXTURE)
			req.set_destination_resource(textures[idx])
			req.set_first_subresource(0)
			req.set_subresource_count(1)
			req.set_priority(PRIORITY_NORMAL)
			requests.append(req)

		var start_usec := Time.get_ticks_usec()
		for req in requests:
			if not ctx.enqueue_request(req):
				_fail("GPU enqueue failed: " + String(req.get_error_message()))
				return {}
		ctx.flush_batch()
		if not ctx.is_batch_complete():
			ctx.wait_for_completion(30000)
		var elapsed_usec := Time.get_ticks_usec() - start_usec

		for req in requests:
			if int(req.get_status()) != int(DSRequest.STATUS_COMPLETED):
				_fail("GPU request finished with status %d: %s" % [
					int(req.get_status()), String(req.get_error_message())])
				return {}

		batch_times_ms.append(float(elapsed_usec) / 1000.0)
		processed += n
	ctx.collect_completed_results()

	var total_seconds := 0.0
	for t in batch_times_ms:
		total_seconds += t / 1000.0
	batch_times_ms.sort()
	var median := 0.0
	if not batch_times_ms.is_empty():
		median = batch_times_ms[batch_times_ms.size() / 2]

	return {
		"bytes": logical_bytes,
		"file_bytes": file_bytes,
		"requests": count,
		"batches": batch_times_ms.size(),
		"seconds": total_seconds,
		"mb_s": (float(logical_bytes) / (1024.0 * 1024.0)) / maxf(total_seconds, 0.000001),
		"file_mb_s": (float(file_bytes) / (1024.0 * 1024.0)) / maxf(total_seconds, 0.000001),
		"min_batch_ms": batch_times_ms[0] if not batch_times_ms.is_empty() else 0.0,
		"median_batch_ms": median,
		"max_batch_ms": batch_times_ms[batch_times_ms.size() - 1] if not batch_times_ms.is_empty() else 0.0,
	}


func _make_order(count: int, seed: int) -> PackedInt32Array:
	var order := PackedInt32Array()
	order.resize(count)
	for i in count:
		order[i] = i
	var rng := seed & 0xFFFFFFFF
	if rng == 0:
		rng = 0x9E3779B9
	for i in range(count - 1, 0, -1):
		rng ^= (rng << 13) & 0xFFFFFFFF
		rng ^= rng >> 17
		rng ^= (rng << 5) & 0xFFFFFFFF
		var j := (rng & 0x7FFFFFFF) % (i + 1)
		var tmp := order[i]
		order[i] = order[j]
		order[j] = tmp
	return order


func _make_offsets(pattern: String, count: int, request_size: int,
		file_size: int, seed: int) -> PackedInt64Array:
	var offsets := PackedInt64Array()
	offsets.resize(count)
	if pattern == "seq":
		for i in count:
			offsets[i] = i * request_size
		return offsets

	# Deterministic xorshift32 over 4 KiB-aligned slots across the whole file.
	var slots := file_size / SECTOR
	var rng := seed & 0xFFFFFFFF
	if rng == 0:
		rng = 0x9E3779B9
	var max_slot := maxi(0, slots - (request_size / SECTOR))
	for i in count:
		rng ^= (rng << 13) & 0xFFFFFFFF
		rng ^= rng >> 17
		rng ^= (rng << 5) & 0xFFFFFFFF
		var slot := (rng & 0x7FFFFFFF) % maxi(1, max_slot + 1)
		offsets[i] = slot * SECTOR
	return offsets


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


func _fail(message: String) -> void:
	_errors += 1
	printerr("[BenchIO] FAIL ", message)
	quit(1)
