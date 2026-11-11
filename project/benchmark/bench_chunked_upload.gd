extends SceneTree

## Chunked-upload benchmark (opt-in, never a test).
##
## Question: how fast can DirectStorage fill VRAM from chunked .dstex files, and
## how do chunk size, staging buffer size and queue capacity move that number?
##
## Why this exists: the sync loader path loads one texture at a time, so it
## cannot show staging pressure or per-batch request fan-out. This bench builds
## the same requests the loader does - one DSRequest per chunk, with the row band
## from set_texture_region - for every fixture at once, then flushes one batch.
## That is the shape a loading screen produces.
##
## Fixtures are synthetic (a correlated ramp, roughly 13x smaller compressed), so
## treat the numbers as relative comparisons within one sitting, never as a
## throughput claim for real art.
##
## Windowed D3D12 only: Win32Backend::initialize() refuses to run without a live
## D3D12 RenderingDevice.
##
## Generate (writes res://.bench_tmp; delete it afterwards):
##   godot --path project --rendering-driver d3d12 -s res://benchmark/bench_chunked_upload.gd -- ^
##     --generate --chunk-list=8192,1024,256 --count=8
##
## Sweep (round-robin arms, min/median over repeats):
##   godot --path project --rendering-driver d3d12 -s res://benchmark/bench_chunked_upload.gd -- ^
##     --chunk-list=8192,1024,256 --staging=16,32,64,128 --count=8 --repeats=3
##
## Queue-capacity stress arm (expect admission rejects / CPU fallback near the
## limit; that is what the queue-capacity contract work is about):
##   ... --chunk-list=256 --staging=32 --count=8 --capacity=128

const WIDTH := 2048
const HEIGHT := 2048
const FORMAT := Image.FORMAT_RGBA8
const BYTES_PER_TEXTURE := WIDTH * HEIGHT * 4
const MAX_REQUESTS_DEFAULT := 256

var _exit := 0
var _args := {}
var _ctx: Variant = null
var _cache: Variant = null
var _rd: RenderingDevice = null


func _initialize() -> void:
	_args = _parse(OS.get_cmdline_user_args())
	if _args.has("generate"):
		_generate()
	else:
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
	printerr("[BenchChunked] FAIL: ", message)
	_exit = 1


func _base_dir() -> String:
	return String(_args.get("base", "res://.bench_tmp"))


func _chunk_dir(chunk_kb: int) -> String:
	return _base_dir().path_join("chunk_%d" % chunk_kb)


func _fixture_path(chunk_kb: int, index: int) -> String:
	return _chunk_dir(chunk_kb).path_join("tex_%04d.dstex" % index)


func _payload() -> PackedByteArray:
	var payload := PackedByteArray()
	payload.resize(BYTES_PER_TEXTURE)
	var state := 0x1234567
	for i in BYTES_PER_TEXTURE:
		if (i & 63) == 0:
			state = (state * 1103515245 + 12345) & 0x7FFFFFFF
		payload[i] = ((i >> 3) + ((state >> 17) & 0x1F)) & 0xFF
	return payload


func _generate() -> void:
	if not ClassDB.class_exists("DSTex"):
		_fail("DSTex is not registered")
		return
	var chunks := _ints(String(_args.get("chunk-list", "8192,1024,256")), [8192])
	var count := maxi(1, int(_args.get("count", 8)))
	var level := int(_args.get("level", 3))
	var dstex: Variant = ClassDB.instantiate("DSTex")
	var payload := _payload()
	for chunk_kb in chunks:
		var dir := _chunk_dir(int(chunk_kb))
		var mk := DirAccess.make_dir_recursive_absolute(dir)
		if mk != OK and mk != ERR_ALREADY_EXISTS:
			_fail("cannot create %s (error %d)" % [dir, mk])
			return
		var started := Time.get_ticks_msec()
		for i in count:
			var err: int = dstex.write_to_file(_fixture_path(int(chunk_kb), i), payload,
				WIDTH, HEIGHT, FORMAT, 1, level, false, false, 0, 0, 0, int(chunk_kb))
			if err != OK:
				_fail("write failed for chunk_kb=%d index=%d (error %d)" % [chunk_kb, i, err])
				return
		var header: Dictionary = dstex.read_header(_fixture_path(int(chunk_kb), 0))
		print("[BenchChunked] generated chunk_kb=", chunk_kb, " count=", count,
			" ms=", Time.get_ticks_msec() - started,
			" chunked=", header.get("chunked", false),
			" chunks=", header.get("mip_chunk_counts", PackedInt32Array()),
			" min_staging_bytes=", header.get("min_staging_bytes", 0))


# One entry per request this texture needs, mirroring the loader's chunk loop
# (ds_resource_format_loader.cpp): chunked mips get one request per chunk with a
# texel-row band, single-chunk mips get one whole-subresource request.
func _request_plan(header: Dictionary) -> Array:
	var plan: Array = []
	var mip_count := int(header.get("mip_count", 0))
	if mip_count != 1:
		return []
	var mip_offsets: PackedInt64Array = header.get("mip_offsets", PackedInt64Array())
	var mip_sizes: PackedInt32Array = header.get("mip_sizes", PackedInt32Array())
	var mip_uncompressed: PackedInt32Array = header.get("mip_uncompressed_sizes", PackedInt32Array())
	var mip_pitches: PackedInt32Array = header.get("mip_row_pitches", PackedInt32Array())
	var chunk_counts: PackedInt32Array = header.get("mip_chunk_counts", PackedInt32Array())
	var chunk_offsets: PackedInt64Array = header.get("chunk_offsets", PackedInt64Array())
	var chunk_sizes: PackedInt32Array = header.get("chunk_compressed_sizes", PackedInt32Array())
	var chunk_uncompressed: PackedInt32Array = header.get("chunk_uncompressed_sizes", PackedInt32Array())

	var chunk_count := 1
	if chunk_counts.size() > 0:
		chunk_count = chunk_counts[0]
	if chunk_count <= 1:
		plan.append({
			"offset": mip_offsets[0],
			"size": mip_sizes[0],
			"uncompressed": mip_uncompressed[0],
			"top": 0,
			"bottom": 0,
		})
		return plan

	var pitch: int = mip_pitches[0] if mip_pitches.size() > 0 else 0
	if pitch <= 0:
		return []
	# The chunk table counts storage rows (block rows for the 4x4 formats); the
	# request destination box is in texel rows. Mirrors
	# DSTex::get_chunk_texel_region so the bench plans the requests the loader
	# actually emits.
	var row_texels := _row_texels(FORMAT)
	var row := 0
	for c in chunk_count:
		var uncompressed: int = chunk_uncompressed[c]
		if uncompressed % pitch != 0:
			return []
		var rows := uncompressed / pitch
		plan.append({
			"offset": chunk_offsets[c],
			"size": chunk_sizes[c],
			"uncompressed": uncompressed,
			"top": row * row_texels,
			"bottom": mini(HEIGHT, (row + rows) * row_texels),
		})
		row += rows
	return plan


## Texel rows covered by one storage row of a format: 4 for the 4x4 block
## formats, 1 otherwise. Mirrors the block-format list in dstex_format.cpp.
static func _row_texels(format: int) -> int:
	var block_formats := [
		Image.FORMAT_DXT1, Image.FORMAT_DXT3, Image.FORMAT_DXT5,
		Image.FORMAT_RGTC_R, Image.FORMAT_RGTC_RG,
		Image.FORMAT_BPTC_RGBA, Image.FORMAT_BPTC_RGBF, Image.FORMAT_BPTC_RGBFU,
	]
	return 4 if format in block_formats else 1


func _seeded_shuffle(items: Array, seed_value: int) -> void:
	var rng := RandomNumberGenerator.new()
	rng.seed = seed_value
	for i in range(items.size() - 1, 0, -1):
		var j := rng.randi_range(0, i)
		var tmp = items[i]
		items[i] = items[j]
		items[j] = tmp


func _sweep() -> void:
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSTex"):
		_fail("DSContext/DSTex are not registered (GDExtension not loaded)")
		return
	_ctx = Engine.get_singleton("DSContext")
	_cache = Engine.get_singleton("DSTextureCache")
	_rd = RenderingServer.get_rendering_device()
	if _ctx == null or _cache == null or _rd == null:
		_fail("DSContext, DSTextureCache or RenderingDevice is unavailable (windowed D3D12 required)")
		return

	var chunks := _ints(String(_args.get("chunk-list", "8192,1024,256")), [8192])
	var stagings := _ints(String(_args.get("staging", "32")), [32])
	var count := maxi(1, int(_args.get("count", 8)))
	var repeats := maxi(1, int(_args.get("repeats", 3)))
	var max_requests := maxi(1, int(_args.get("max-requests", MAX_REQUESTS_DEFAULT)))
	var capacity := maxi(128, int(_args.get("capacity", int(ProjectSettings.get_setting("dsgd_extension/queue_capacity", 512)))))
	var dstex: Variant = ClassDB.instantiate("DSTex")

	# Resolve fixtures and their request plan once per chunk size.
	var paths_by_chunk := {}
	var plans_by_chunk := {}
	var codec_by_chunk := {}
	for chunk_kb in chunks:
		var paths: Array[String] = []
		for i in count:
			var path := _fixture_path(int(chunk_kb), i)
			if not FileAccess.file_exists(path):
				_fail("missing fixture %s (run with --generate first)" % path)
				return
			paths.append(path)
		var header: Dictionary = dstex.read_header(paths[0])
		var plan := _request_plan(header)
		if plan.is_empty():
			_fail("fixture %s has no usable single-mip chunk plan" % paths[0])
			return
		paths_by_chunk[int(chunk_kb)] = paths
		plans_by_chunk[int(chunk_kb)] = plan
		codec_by_chunk[int(chunk_kb)] = int(header.get("compression_format", 1))
		print("[BenchChunked] fixture chunk_kb=", chunk_kb, " requests_per_texture=", plan.size(),
			" chunks=", header.get("mip_chunk_counts", PackedInt32Array()),
			" min_staging_bytes=", header.get("min_staging_bytes", 0))

	var arms: Array = []
	for chunk_kb in chunks:
		for staging_mb in stagings:
			arms.append([int(chunk_kb), int(staging_mb)])
	var results := {}
	for arm in arms:
		results["%d:%d" % [arm[0], arm[1]]] = []

	var warm: Array = arms[0]
	_measure(warm[0], warm[1], paths_by_chunk[warm[0]], plans_by_chunk[warm[0]],
		codec_by_chunk[warm[0]], count, max_requests, capacity, "warmup", false, results)

	var seed := int(_args.get("seed", 20261111))
	for rep in repeats:
		var order: Array = arms.duplicate()
		_seeded_shuffle(order, seed + rep)
		for arm in order:
			_measure(arm[0], arm[1], paths_by_chunk[arm[0]], plans_by_chunk[arm[0]],
				codec_by_chunk[arm[0]], count, max_requests, capacity, "rep%d" % rep, true, results)

	print("[BenchChunked] summary (min/median of ", repeats, " repeats)")
	for arm in arms:
		var key := "%d:%d" % [arm[0], arm[1]]
		var samples: Array = results[key]
		if samples.is_empty():
			continue
		samples.sort()
		var best: float = samples[0]
		var median: float = samples[samples.size() / 2]
		var used := clampi(max_requests / plans_by_chunk[arm[0]].size(), 1, count)
		var bytes_mib := float(used * BYTES_PER_TEXTURE) / 1048576.0
		print("[BenchChunked] RESULT chunk_kb=", arm[0], " staging_mb=", arm[1],
			" capacity=", capacity,
			" requests_per_texture=", plans_by_chunk[arm[0]].size(),
			" textures=", used,
			" min_ms=", snappedf(best, 0.01),
			" median_ms=", snappedf(median, 0.01),
			" min_mib_per_s=", snappedf(bytes_mib / maxf(best / 1000.0, 0.000001), 0.1))
	_ctx.call("shutdown_ds")


func _measure(chunk_kb: int, staging_mb: int, paths: Array[String], plan: Array,
		codec: int, count: int, max_requests: int, capacity: int, label: String,
		record: bool, results: Dictionary) -> void:
	_cache.call("clear")
	_ctx.call("shutdown_ds")
	ProjectSettings.set_setting("dsgd_extension/staging_buffer_size_mb", staging_mb)
	ProjectSettings.set_setting("dsgd_extension/queue_capacity", capacity)
	if not bool(_ctx.call("initialize_ds")):
		_fail("initialize_ds failed at staging=%d MiB" % staging_mb)
		return
	var backend: String = String(_ctx.call("get_active_backend_name"))
	if not bool(_ctx.call("is_d3d12_backend")):
		_fail("expected a D3D12 backend, got '%s'" % backend)
		return

	var chunks_per_texture := plan.size()
	var used := clampi(max_requests / chunks_per_texture, 1, count)

	var texture_format := RDTextureFormat.new()
	texture_format.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	texture_format.width = WIDTH
	texture_format.height = HEIGHT
	texture_format.mipmaps = 1
	texture_format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT

	var textures: Array[RID] = []
	for i in used:
		var texture := _rd.texture_create(texture_format, RDTextureView.new(), [])
		if not texture.is_valid():
			_fail("GPU texture creation failed at %d/%d" % [i, used])
			for rid in textures:
				_rd.free_rid(rid)
			return
		textures.append(texture)

	var requests: Array = []
	var rejected := 0
	var started := Time.get_ticks_usec()
	for i in used:
		for entry in plan:
			var request := DSRequest.new()
			request.file_path = paths[i]
			request.offset = int(entry["offset"])
			request.size = int(entry["size"])
			request.uncompressed_size = int(entry["uncompressed"])
			request.compression_format = codec
			request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
			request.destination_resource = textures[i]
			request.first_subresource = 0
			request.subresource_count = 1
			request.priority = DSRequest.PRIORITY_NORMAL
			if int(entry["bottom"]) > 0:
				request.set_texture_region(int(entry["top"]), int(entry["bottom"]))
			if not bool(_ctx.call("enqueue_request", request, false)):
				rejected += 1
			requests.append(request)
	_ctx.call("flush_batch")
	var elapsed_ms := float(Time.get_ticks_usec() - started) / 1000.0

	var completed := 0
	var failed := 0
	var first_error := ""
	for request in requests:
		if int(request.status) == DSRequest.STATUS_COMPLETED:
			completed += 1
		else:
			failed += 1
			if first_error.is_empty():
				first_error = String(request.error_message)
	for rid in textures:
		_rd.free_rid(rid)

	var bytes_mib := float(used * BYTES_PER_TEXTURE) / 1048576.0
	print("[BenchChunked] ", label, " chunk_kb=", chunk_kb, " staging_mb=", staging_mb,
		" capacity=", capacity, " backend=", backend,
		" requests=", requests.size(), " rejected=", rejected, " completed=", completed,
		" failed=", failed,
		" ms=", snappedf(elapsed_ms, 0.01),
		" mib_per_s=", snappedf(bytes_mib / maxf(elapsed_ms / 1000.0, 0.000001), 0.1),
		(" err=" + first_error) if not first_error.is_empty() else "")
	if record and failed == 0 and rejected == 0:
		results["%d:%d" % [chunk_kb, staging_mb]].append(elapsed_ms)
	if failed > 0 or rejected > 0:
		_exit = 1
