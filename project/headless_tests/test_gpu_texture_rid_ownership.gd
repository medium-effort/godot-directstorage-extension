extends SceneTree

## GPU texture RID ownership suite (windowed D3D12).
##
## Regression coverage for "Texture2DRD is a non-owning view"
## (GODOT_QUIRKS.md section 27): the loader creates the device texture, so
## whoever does not hand it to DSTextureCache must release it, or VRAM leaks one
## RID per load.
##
## Not part of gdextest: that suite runs headless, where there is no
## RenderingDevice, so the GPU branch is unreachable there. Run it through
## project/headless_tests/run_gpu_texture_rid_ownership.ps1, which starts one process per
## scenario and decides pass/fail from the GPU_RID_* markers.
##
## Scenarios (`-- --scenario=<name>`):
##   oracle          detector self-check: a freed RD RID reports invalid, a
##                   live one reports valid. Without this the "0 leaked"
##                   result of any other scenario would be unfalsifiable.
##   reuse           control: CACHE_MODE_REUSE still returns Texture2DRD, i.e.
##                   the GPU path is actually alive in this environment.
##   ignore          CACHE_MODE_IGNORE must not orphan the RID that the loader
##                   created (it is owned by DSTextureOwner instead).
##   disabled_cache  second hole: cache disabled + default mode must not orphan
##                   it either (store() refuses when !enabled).
##   sim_fix         dry run proving the check turns green once each orphaned
##                   RID is released by hand - no product change needed.
##   prewarm_disabled_cache
##                   preloader half: with the cache disabled, prewarm_gpu_async
##                   must refuse up front (tag 0, one warning) instead of
##                   allocating VRAM and dropping every texture afterwards.
##   teardown_immediate
##                   drop an uncached texture and quit immediately: the engine's
##                   exit census must report no leaked RID.
##   teardown_prewarm_pending
##                   leave a GPU prewarm destination RID in the preloader's pending
##                   table and quit without harvesting: the runner's exit-census
##                   check decides whether module teardown released it.
##   census_control  oracle liveness: leak exactly one texture RID on purpose and
##                   require the exit census to report it, so a `census=0` verdict
##                   elsewhere cannot come from a census that never fires.
##   deferral_retry  a RID released while its request is in flight must be
##                   deferred, then freed by the frame-drawn retry.
##   hammer          200 uncached GPU loads: no RID and no side-table entry may
##                   accumulate.
##
## The invariant is fix-agnostic: after the returned texture's last Ref is
## dropped, no device RID it owned may still be alive. Each process starts by
## deleting fixtures left by an earlier run (a fresh process holds no
## IDStorageFile handles yet).
##
## Hangs are covered by the runner's OS-level timeout (sync _initialize cannot
## pump a watchdog timer).

const LOAD_COUNT := 8
# Stress count for the uncached-owner path: one owner attach + release per
# texture, so this mainly proves nothing accumulates in the side table.
const HAMMER_COUNT := 200
# Deferral scenario: a large single-mip fixture keeps the second request in
# flight long enough to observe the deferred branch deterministically.
const BIG_TEX := 2048
const BIG_PATH := "user://gpu_texture_rid_ownership/big.dstex"
const DEFERRAL_MAX_FRAMES := 240
const TEX_W := 64
const TEX_H := 4
# Full chain is required: Image::create_from_data(..., use_mipmaps=true)
# expects every level down to 1x1 (see project/headless_tests/test_thread_affinity.gd).
const MIP_COUNT := 7
const ROW_PITCH := 256
const FIXTURE_DIR := "user://gpu_texture_rid_ownership"
const FIXTURE_PATH := "user://gpu_texture_rid_ownership/fixture.dstex"
const FIXTURE_PATH_B := "user://gpu_texture_rid_ownership/fixture_b.dstex"

var scenario := "ignore"
var failures := 0
var skip_reason := ""

# Census reported to the runner (GPU_RID_COUNTS).
var requested := 0
var loaded_count := 0
var gpu_textures := 0
var image_textures := 0
var orphans_observed := 0
var leaked_rids := 0
var warmup_orphans := 0
var prewarm_tag := -1
var cpu_prewarm_tag := -1
var cpu_prewarm_stored := -1
var prewarm_pending := -1
var extra := ""


func _initialize() -> void:
	scenario = _arg_value("scenario", "ignore")
	print("GPU_RID_SCENARIO=", scenario)

	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_skip("RenderingDevice unavailable; run with --rendering-driver d3d12 and a window")
		return

	var ctx: Variant = Engine.get_singleton("DSContext")
	if ctx == null:
		_skip("DSContext singleton missing (extension not loaded)")
		return
	if not ctx.is_initialized():
		ctx.initialize_ds()
	if not ctx.is_d3d12_backend() or not ctx.is_gpu_decompression_enabled():
		_skip("DirectStorage D3D12 GPU decompression unavailable (backend=" + str(ctx.get_active_backend_name()) + ")")
		return

	print("[env] backend=", ctx.get_active_backend_name(),
		" d3d12=", ctx.is_d3d12_backend(),
		" gpu_decompression=", ctx.is_gpu_decompression_enabled())

	var cache: Variant = Engine.get_singleton("DSTextureCache")

	_cleanup_stale_fixtures()

	match scenario:
		"oracle":
			_scenario_oracle(rd)
		"reuse":
			_scenario_reuse(rd, cache)
		"ignore":
			_scenario_uncached(rd, cache, ResourceLoader.CACHE_MODE_IGNORE, false, false, LOAD_COUNT)
		"disabled_cache":
			_scenario_uncached(rd, cache, ResourceLoader.CACHE_MODE_REUSE, true, false, LOAD_COUNT)
		"sim_fix":
			_scenario_uncached(rd, cache, ResourceLoader.CACHE_MODE_IGNORE, false, true, LOAD_COUNT)
		"hammer":
			_scenario_uncached(rd, cache, ResourceLoader.CACHE_MODE_IGNORE, false, false, HAMMER_COUNT)
		"teardown_immediate":
			_scenario_teardown_immediate(rd, cache)
		"teardown_prewarm_pending":
			_scenario_teardown_prewarm_pending(cache)
		"census_control":
			_scenario_census_control(rd)
		"deferral_retry":
			await _scenario_deferral_retry(rd, cache)
		"prewarm_disabled_cache":
			_scenario_prewarm_disabled(cache)
		_:
			_fail("unknown scenario '" + scenario + "'")

	_emit_result()


## Detector self-check. Proves the oracle can see a free (so a later
## "leaked=0" is real) and can see a live RID (so it is not always false).
func _scenario_oracle(rd: RenderingDevice) -> void:
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	fmt.width = 8
	fmt.height = 8
	fmt.mipmaps = 1
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT

	var freed: RID = rd.texture_create(fmt, RDTextureView.new(), [])
	if not freed.is_valid():
		_fail("oracle: texture_create failed")
		return
	rd.free_rid(freed)
	if rd.texture_is_valid(freed):
		_fail("oracle: a freed RID still reports valid - the detector cannot see frees")
	else:
		print("[PASS] oracle: freed RID reports invalid")

	var live: RID = rd.texture_create(fmt, RDTextureView.new(), [])
	if not live.is_valid():
		_fail("oracle: second texture_create failed")
		return
	if rd.texture_is_valid(live):
		print("[PASS] oracle: a live (unfreed) RID reports valid")
	else:
		_fail("oracle: a live RID reports invalid - the detector would miss the leak")
	rd.free_rid(live)

	extra = "oracle_ok=" + str(failures == 0)


## Control: the GPU path must still be reachable, otherwise every other
## scenario would pass vacuously (CPU fallback never owns a device RID).
func _scenario_reuse(rd: RenderingDevice, cache: Variant) -> void:
	if cache == null:
		_fail("reuse: DSTextureCache singleton missing")
		return
	cache.set_enabled(true)
	if not _write_fixture():
		return

	requested = LOAD_COUNT
	for i in LOAD_COUNT:
		var tex: Variant = ResourceLoader.load(FIXTURE_PATH, "", ResourceLoader.CACHE_MODE_REUSE)
		if tex == null or not (tex is Texture2D):
			_fail("reuse: load " + str(i) + " returned no Texture2D")
			continue
		loaded_count += 1
		if tex is Texture2DRD:
			gpu_textures += 1
			if not (tex as Texture2DRD).texture_rd_rid.is_valid():
				_fail("reuse: Texture2DRD carries an invalid RD RID")
		else:
			image_textures += 1
		tex = null

	var cached_after_store: int = int(cache.get_cached_count())
	if cached_after_store <= 0:
		_fail("reuse: cache took ownership of nothing; store() did not run")
	if gpu_textures != LOAD_COUNT:
		_fail("reuse: GPU Texture2DRD path is inactive (" + str(gpu_textures) + "/" + str(LOAD_COUNT) + " loads)")
	extra = "cached_after_store=" + str(cached_after_store)

	cache.clear()
	_remove_fixture()
	# Deliberately no leak assertion here: store() makes the texture engine-
	# resident via take_over_path, so exit residuals are the documented
	# upstream Texture2DRD behaviour, not this bug.


## Preloader half of the fix, two phases.
## A: with a disabled cache nothing can be stored, so prewarm_gpu_async must
##    refuse up front instead of allocating VRAM, running GPU decompression and
##    dropping every texture at completion (one warning per texture). The runner
##    also checks the single warning and the exit census.
## B: the CPU prewarm_async refuse-on-disabled path plus the capped slice. Two
##    textures are enqueued while the cache is enabled, then the cache is
##    disabled mid-flight and poll_completed(tag, max_items=1) is driven twice:
##    each call must consume exactly ONE entry (the `processed` budget) and
##    report 0 stored (the store() refusal), which is the pair of behaviours the
##    #0136 review asked us to prove.
func _scenario_prewarm_disabled(cache: Variant) -> void:
	if cache == null:
		_fail("prewarm: DSTextureCache singleton missing")
		return
	cache.set_enabled(false)
	if not _write_fixture():
		return

	var ctx: Variant = Engine.get_singleton("DSContext")
	var paths := PackedStringArray([FIXTURE_PATH, FIXTURE_PATH_B])

	# --- Phase A: disabled cache refuses both prewarm entry points ---
	prewarm_tag = int(cache.prewarm_gpu_async(paths, 16, true))
	if prewarm_tag != 0:
		_fail("prewarm: cache disabled but prewarm_gpu_async still started (tag=" + str(prewarm_tag) + ")")

	# Pumping must be a no-op and no entry may be pending or unbuilt.
	cache.pump_gpu_prewarm()
	cache.poll_completed(0, 0, 0)
	var pending := int(cache.get_pending_prewarm_count(0))
	var unbuilt := int(cache.get_unbuilt_prewarm_count(0))
	if pending != 0:
		_fail("prewarm: " + str(pending) + " entries pending after a refused call")
	if unbuilt != 0:
		_fail("prewarm: " + str(unbuilt) + " entries unbuilt after a refused call")

	cpu_prewarm_tag = int(cache.prewarm_async(paths))
	if cpu_prewarm_tag != 0:
		_fail("prewarm: cache disabled but prewarm_async still started (tag=" + str(cpu_prewarm_tag) + ")")

	extra = "prewarm_tag=" + str(prewarm_tag) + " pending=" + str(pending) + " unbuilt=" + str(unbuilt) + " cpu_tag_disabled=" + str(cpu_prewarm_tag)

	# --- Phase B: enqueue while enabled, then disable before harvesting ---
	cache.set_enabled(true)
	var cpu_tag := int(cache.prewarm_async(paths))
	if cpu_tag == 0:
		_fail("prewarm: prewarm_async returned tag 0 with the cache enabled")
		cache.set_enabled(true)
		_remove_fixture()
		return
	var cpu_unbuilt := int(cache.get_unbuilt_cpu_prewarm_count(cpu_tag))
	if cpu_unbuilt != 2:
		_fail("prewarm: expected 2 unbuilt CPU entries, saw " + str(cpu_unbuilt) + " (budget test needs two)")

	# Mid-flight disable: requests were enqueued while enabled, the stores below
	# will be refused. set_enabled(false) also drains the (still empty) cache.
	cache.set_enabled(false)

	cpu_prewarm_tag = cpu_tag
	var saw_capped_slice := false
	var slice_stored := 0
	var slice_max := 0
	# Consumption metric: get_unbuilt_prewarm_count() is s.pending.size() for the
	# tag (entries not yet consumed). get_pending_prewarm_count() counts entries
	# whose REQUESTS are non-terminal, which drops to 0 as soon as the stream
	# finishes - it cannot measure what a poll slice consumed.
	var deadline := Time.get_ticks_msec() + 5000
	while Time.get_ticks_msec() < deadline:
		var before := int(cache.get_unbuilt_prewarm_count(cpu_tag))
		if before == 0:
			break
		if ctx != null:
			ctx.flush_stream_async()
			ctx.wait_for_completion(100)
			ctx.collect_completed_results()
		slice_stored += int(cache.poll_completed(cpu_tag, 1, 0))
		var after := int(cache.get_unbuilt_prewarm_count(cpu_tag))
		var consumed := before - after
		slice_max = maxi(slice_max, consumed)
		if consumed > 1:
			_fail("prewarm: max_items=1 slice consumed " + str(consumed) + " entries")
			break
		if before == 2 and consumed == 1:
			saw_capped_slice = true

	var cpu_leftover := int(cache.get_unbuilt_prewarm_count(cpu_tag))
	cpu_prewarm_stored = slice_stored
	if not saw_capped_slice:
		_fail("prewarm: no max_items=1 slice consumed exactly one of two entries")
	if cpu_leftover != 0:
		_fail("prewarm: CPU prewarm did not drain (" + str(cpu_leftover) + " pending)")
	if slice_stored != 0:
		_fail("prewarm: cache disabled but poll_completed reported " + str(slice_stored) + " stored")
	extra += " cpu_tag=" + str(cpu_tag) + " cpu_unbuilt=" + str(cpu_unbuilt) + " cpu_stored=" + str(slice_stored) + " slice_max=" + str(slice_max)

	cache.set_enabled(true)
	_remove_fixture()


## ignore / disabled_cache / sim_fix. p_simulate_fix frees every orphaned RID,
## which is what a post-fix run must do (by whichever mechanism).
func _scenario_uncached(rd: RenderingDevice, cache: Variant, p_cache_mode: int, p_disable_cache: bool, p_simulate_fix: bool, p_count: int) -> void:
	if cache == null:
		_fail("uncached: DSTextureCache singleton missing")
		return
	cache.set_enabled(not p_disable_cache)
	extra = "cache_enabled=" + str(not p_disable_cache) + " simulated_fix=" + str(p_simulate_fix)
	if not _write_fixture():
		return

	# Warm-up outside the census (first load pays file/queue setup). It also
	# proves free_rid() retires a leaked RID in this very process, so a later
	# leaked=0 cannot be an artifact of a dead oracle.
	warmup_orphans = _load_and_release(rd, p_cache_mode, true, "warmup", false)

	requested = p_count
	for i in p_count:
		_load_and_release(rd, p_cache_mode, p_simulate_fix, str(i), true)

	if loaded_count != requested:
		_fail("uncached: " + str(loaded_count) + "/" + str(requested) + " loads produced a Texture2D")
	if p_simulate_fix:
		if leaked_rids != 0:
			_fail("sim_fix: freeing the RIDs did not clear the leak (" + str(leaked_rids) + " still alive)")
	elif leaked_rids > 0:
		_fail(str(leaked_rids) + "/" + str(requested) + " uncached loads orphaned a GPU texture RID")

	_remove_fixture()


## Loads once, classifies the result, drops it, and checks whether the device
## RID survived. Optionally frees it (post-fix expectation / warm-up cleanup).
## p_counted=false runs it as an uncounted warm-up. Returns 1 when the load
## orphaned a RID, else 0.
func _load_and_release(rd: RenderingDevice, p_cache_mode: int, p_free_leaked: bool, p_label: String, p_counted: bool) -> int:
	var tex: Variant = ResourceLoader.load(FIXTURE_PATH, "", p_cache_mode)
	if tex == null or not (tex is Texture2D):
		_fail("load " + p_label + " returned no Texture2D")
		return 0

	var rid := RID()
	if tex is Texture2DRD:
		if p_counted:
			gpu_textures += 1
		rid = (tex as Texture2DRD).texture_rd_rid
	else:
		# ImageTexture owns a RenderingServer RID that frees itself on drop.
		if p_counted:
			image_textures += 1
	if p_counted:
		loaded_count += 1

	tex = null # drop the only owner

	if not rid.is_valid() or not rd.texture_is_valid(rid):
		return 0

	if p_counted:
		orphans_observed += 1
	print("[orphan] load ", p_label, ": RD texture RID " + str(rid) + " is still alive after the texture was released")
	if p_free_leaked:
		rd.free_rid(rid)
		if rd.texture_is_valid(rid):
			_fail("load " + p_label + ": free_rid did not retire the RID")
			leaked_rids += 1
		return 1

	if p_counted:
		leaked_rids += 1
	else:
		warmup_orphans += 1
	return 1


## Teardown ordering: drop an uncached GPU texture and quit in the same breath.
## The owner must release the RID before the process exits - nothing later in
## the frame, and no drain tick, may be relied on to tidy up. The runner adds
## the decisive check: the engine's exit census must report zero leaked RIDs.
func _scenario_teardown_immediate(rd: RenderingDevice, cache: Variant) -> void:
	if cache == null:
		_fail("teardown: DSTextureCache singleton missing")
		return
	# Disabled cache => the texture is owned by DSTextureOwner, not the cache.
	cache.set_enabled(false)
	if not _write_fixture():
		return

	requested = 1
	_load_and_release(rd, ResourceLoader.CACHE_MODE_REUSE, false, "teardown", true)
	if leaked_rids > 0:
		_fail("teardown: the uncached texture's RID outlived the texture")
	# Deliberately left for teardown: fixture still on disk, cache still disabled.
	extra = "cache_disabled_at_exit=true fixture_left_on_disk=true"


## Preloader half of teardown ownership: a GPU prewarm destination RID that is
## allocated but not yet harvested must be released when the module tears down,
## not dropped. The scenario only has to prove the RID exists and then quit; the
## runner decides the verdict from the engine's exit census, because a RID that
## survives to RenderingDevice::finalize() is invisible from in here.
func _scenario_teardown_prewarm_pending(cache: Variant) -> void:
	if cache == null:
		_fail("teardown_prewarm: DSTextureCache singleton missing")
		return
	# Pause the tree before allocating. This is load-bearing: DSRuntime._process
	# calls poll_completed(0, ...) at the start of every frame, so an entry that is
	# already terminal gets harvested into the cache on frame 1 - and the cache then
	# frees its RID cleanly, so the leak never reaches the exit census. A paused
	# tree is also the honest model of the finding: the game exits with a prewarm
	# batch still pending, before any frame driver has harvested it.
	paused = true
	cache.set_enabled(true)
	if not _write_fixture():
		return

	# prewarm_gpu_async kicks its first chunk itself, so the render-thread callback
	# has already run by the time it returns.
	var paths := PackedStringArray([FIXTURE_PATH])
	var tag := int(cache.prewarm_gpu_async(paths, 16, true))
	prewarm_tag = tag
	if tag == 0:
		_fail("teardown_prewarm: prewarm_gpu_async refused with the cache enabled")
		return

	# The landmark is a NON-TERMINAL request: alloc_gpu_item_on_render_thread()
	# creates the texture and writes it into the request before enqueueing it, so a
	# request in that state proves the RID was allocated and is still in
	# state().pending, which also keeps a green run from being vacuous.
	var pending := int(cache.get_pending_prewarm_count(tag))
	prewarm_pending = pending
	if pending <= 0:
		_fail("teardown_prewarm: no GPU prewarm request reached the stream, so the leak precondition was never met")
		return

	# Deliberately no poll_completed(), clear() or set_enabled(false): the RID must
	# be reaped by the real teardown path, not tidied up by the scenario.
	extra = "cache_enabled_at_exit=true pending_rids_held=true"


## Oracle liveness for every other census assertion in this file: those all want
## `census == 0`, which an oracle that never prints anything would also satisfy.
## Leak one texture RID on purpose and let the engine's exit census report it.
func _scenario_census_control(rd: RenderingDevice) -> void:
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM
	fmt.width = 8
	fmt.height = 8
	fmt.mipmaps = 1
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT

	var leaked: RID = rd.texture_create(fmt, RDTextureView.new(), [])
	if not leaked.is_valid():
		_fail("census_control: texture_create failed")
		return
	if not rd.texture_is_valid(leaked):
		_fail("census_control: a freshly created RID reports invalid, so the control cannot leak anything")
		return
	# Deliberately never freed: this RID is the census probe.
	extra = "deliberate_leak=" + str(leaked)


## Deferral path (#0126): an RID released while its own request is still in
## flight must be queued, survive the release, and then be freed by the
## frame-drawn retry once the request retires. Drives the uncached
## (DSTextureOwner) path so the release goes through release_owned_rid().
func _scenario_deferral_retry(rd: RenderingDevice, cache: Variant) -> void:
	var ctx: Variant = Engine.get_singleton("DSContext")
	if ctx == null:
		_fail("deferral: DSContext singleton missing")
		return
	if not _write_big_fixture():
		return

	var dstex := DSTex.new()
	var header: Dictionary = dstex.read_header(BIG_PATH)
	if header.is_empty():
		_fail("deferral: read_header returned empty for the big fixture")
		return
	if int(header.get("version", 0)) != 2:
		_fail("deferral: big fixture is not v2 (the GPU path needs v2 footprints)")
		return
	var mip_offsets: PackedInt64Array = header.get("mip_offsets", PackedInt64Array())
	var mip_sizes: PackedInt32Array = header.get("mip_sizes", PackedInt32Array())
	if mip_offsets.is_empty() or mip_sizes.is_empty():
		_fail("deferral: big fixture carries no mip table")
		return

	# Uncached load -> Texture2DRD owned by DSTextureOwner.
	var tex: Variant = ResourceLoader.load(BIG_PATH, "", ResourceLoader.CACHE_MODE_IGNORE)
	if tex == null or not (tex is Texture2DRD):
		var tex_class := "null"
		if tex != null:
			tex_class = tex.get_class()
		_fail("deferral: uncached load did not produce a Texture2DRD (class=" + tex_class + ")")
		return
	var rid: RID = (tex as Texture2DRD).texture_rd_rid
	if not rid.is_valid():
		tex = null
		_fail("deferral: Texture2DRD carries no device RID")
		return

	# Second request against the SAME RID, submitted without waiting.
	var request := DSRequest.new()
	request.file_path = BIG_PATH
	request.offset = int(mip_offsets[0])
	request.size = int(mip_sizes[0])
	request.uncompressed_size = int(header.get("original_size", 0))
	request.compression_format = DSRequest.COMPRESSION_GDEFLATE
	request.destination_type = DSRequest.DESTINATION_GPU_TEXTURE
	request.destination_resource = rid
	request.first_subresource = 0
	request.subresource_count = int(header.get("mip_count", 1))
	# texture_format stays 0 on purpose: the backend skips the driver-format
	# equality check at 0 (see #0125).
	var accepted: bool = ctx.enqueue_request(request)
	if not accepted:
		tex = null
		_fail("deferral: enqueue_request rejected the second request: " + str(request.get_error_message()))
		return
	ctx.flush_batch_async()

	# Drop the only owner while that request is still in flight.
	tex = null
	var deferred := rd.texture_is_valid(rid)
	print("[deferral] released the texture with its request in flight; RID alive after release=", deferred)
	if not deferred:
		_fail("deferral: the RID was freed immediately, so the request had already retired (fixture too small?)")
		return

	# The frame-drawn retry must free it once the request retires.
	var frames := 0
	while frames < DEFERRAL_MAX_FRAMES and rd.texture_is_valid(rid):
		ctx.collect_completed_results()
		await process_frame
		frames += 1

	if rd.texture_is_valid(rid):
		rd.free_rid(rid) # keep the exit census clean before failing
		_fail("deferral: the RID survived " + str(frames) + " frames after its request retired (retry did not fire)")
		return

	print("[PASS] deferral: RID deferred while in flight, then freed by the retry after ", frames, " frame(s)")
	extra = "deferred=true retry_frames=" + str(frames)


func _write_big_fixture() -> bool:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)
	var noise := FastNoiseLite.new()
	noise.seed = 20261025
	noise.frequency = 0.01
	noise.fractal_octaves = 4
	var image := noise.get_image(BIG_TEX, BIG_TEX, false, false, true)
	if image == null or image.is_empty():
		_fail("deferral: FastNoiseLite produced no image")
		return false
	image.convert(Image.FORMAT_RGBA8)
	var dstex := DSTex.new()
	var err: Error = dstex.write_to_file(BIG_PATH, image.get_data(), BIG_TEX, BIG_TEX, Image.FORMAT_RGBA8, 1, 1)
	if err != OK:
		_fail("deferral: DSTex.write_to_file failed: " + error_string(err))
		return false
	return true


func _write_fixture() -> bool:
	DirAccess.make_dir_recursive_absolute(FIXTURE_DIR)
	var dstex := DSTex.new()
	var payload := _make_texture_payload()
	var err: Error = dstex.write_to_file(FIXTURE_PATH, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	if err != OK:
		_fail("DSTex.write_to_file failed: " + error_string(err))
		return false
	# Second identical payload: the prewarm budget scenario needs two entries to
	# prove that a max_items=1 slice stops after one.
	var err_b: Error = dstex.write_to_file(FIXTURE_PATH_B, payload, TEX_W, TEX_H, Image.FORMAT_RGBA8, MIP_COUNT, 1)
	if err_b != OK:
		_fail("DSTex.write_to_file (second fixture) failed: " + error_string(err_b))
		return false

	var header: Dictionary = dstex.read_header(FIXTURE_PATH)
	if header.is_empty():
		_fail("DSTex.read_header returned empty for the fixture")
		return false
	if int(header.get("version", 0)) != 2:
		_fail("fixture is not v2 (GPU path needs v2 footprints); version=" + str(header.get("version", 0)))
		return false
	var as_image_texture: bool = bool(header.get("as_image_texture", false))
	if as_image_texture:
		_fail("fixture carries the as_image_texture flag; GPU path would be bypassed")
		return false
	var mip_count: int = int(header.get("mip_count", 0))
	if mip_count != MIP_COUNT:
		_fail("fixture mip_count=" + str(mip_count) + ", expected " + str(MIP_COUNT))
		return false
	return true


func _remove_fixture() -> void:
	if FileAccess.file_exists(FIXTURE_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(FIXTURE_PATH))
	if FileAccess.file_exists(FIXTURE_PATH_B):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(FIXTURE_PATH_B))


## Best-effort removal of fixtures left behind by an earlier run (including the
## 16 MiB deferral fixture). A fresh process holds no IDStorageFile handles yet,
## so this normally succeeds; every fixture is rewritten by the scenario anyway,
## and a failure here is never part of the verdict.
func _cleanup_stale_fixtures() -> void:
	for path in [FIXTURE_PATH, FIXTURE_PATH_B, BIG_PATH]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


## 64x4 RGBA8, 7 mips, 256-byte row-pitch padded per mip. Same shape as the
## known-good fixture in project/headless_tests/test_thread_affinity.gd.
func _make_texture_payload() -> PackedByteArray:
	var mip_dims := [[64, 4], [32, 2], [16, 1], [8, 1], [4, 1], [2, 1], [1, 1]]
	var total := 0
	for dims in mip_dims:
		total += ROW_PITCH * int(dims[1])

	var payload := PackedByteArray()
	payload.resize(total)
	var mip_base := 0
	for mip in range(mip_dims.size()):
		var w: int = mip_dims[mip][0]
		var h: int = mip_dims[mip][1]
		for y in range(h):
			for x in range(w):
				var pixel_offset := mip_base + y * ROW_PITCH + x * 4
				payload[pixel_offset] = (x * 7 + mip * 31) % 256
				payload[pixel_offset + 1] = (y * 63 + mip * 17) % 256
				payload[pixel_offset + 2] = (mip * 40 + 180) % 256
				payload[pixel_offset + 3] = 255
		mip_base += ROW_PITCH * h
	return payload


func _arg_value(p_key: String, p_default: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--" + p_key + "="):
			return arg.substr(("--" + p_key + "=").length())
		if arg.begins_with(p_key + "="):
			return arg.substr((p_key + "=").length())
	return p_default


func _skip(p_reason: String) -> void:
	skip_reason = p_reason
	print("[SKIP] ", p_reason)
	print("GPU_RID_SCENARIO=", scenario)
	print("GPU_RID_RESULT scenario=", scenario, " pass=false skip=true reason=", p_reason)
	quit(3)


func _fail(p_message: String) -> void:
	failures += 1
	printerr("[FAIL] ", p_message)


func _emit_result() -> void:
	print("GPU_RID_COUNTS scenario=", scenario,
		" requested=", requested,
		" loaded=", loaded_count,
		" gpu=", gpu_textures,
		" image=", image_textures,
		" orphans=", orphans_observed,
		" leaked=", leaked_rids,
		" warmup_orphans=", warmup_orphans,
		" prewarm_tag=", prewarm_tag,
		" cpu_prewarm_tag=", cpu_prewarm_tag,
		" cpu_prewarm_stored=", cpu_prewarm_stored,
		" prewarm_pending=", prewarm_pending,
		" extra=", extra)
	if failures == 0:
		print("[PASS] ", scenario, ": no orphaned texture RID")
	else:
		printerr("[FAIL] ", scenario, ": ", failures, " assertion(s) failed")
	print("GPU_RID_RESULT scenario=", scenario, " pass=", str(failures == 0), " skip=false reason=", extra)
	quit(0 if failures == 0 else 1)
