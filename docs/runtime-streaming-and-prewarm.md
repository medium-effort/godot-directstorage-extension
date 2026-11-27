# Runtime Texture Streaming, Prewarm & Caching

This guide covers how `dsgd` streams and decompresses textures at runtime (GPU vs. CPU fallback, render-thread affinity, volume I/O, and frame-cost trade-offs), how `DSPreloader` and `DSTextureCache` handle async prewarming and VRAM residency, and how the native runtime API and export archive discovery fit together.

---

## 1. Direct GPU Decompression & Fallbacks

When running on Windows with a Direct3D 12 `RenderingDevice` (`--rendering-driver d3d12`), DirectStorage decompresses GDeflate mip streams directly into GPU VRAM as `Texture2DRD` resources via compute shaders. If GPU decompression is disabled, unsupported, or if legacy version 1 assets are loaded, the system automatically falls back to multi-threaded CPU software decompression (`ImageTexture`).

- **Render-thread affinity**: The GPU path requires the render thread. Under the default `rendering/driver/threads/thread_model` (`Safe`) the main thread *is* the render thread, so a `ResourceLoader.load()` on the main thread uploads straight to VRAM. Background loads (`load_threaded_request()`, `use_sub_threads`) always run on a worker thread and decompress on the CPU, and under the experimental `Separate` model every load does. Neither is silent: `DSContext.get_capability_report()` carries `loader_gpu_loads`, `loader_cache_hits`, and `loader_cpu_fallbacks` (per-reason counts), and the first thread-affinity fallback logs one warning per session naming the two ways to reach the GPU path - a main-thread load, or `DSPreloader.prewarm_gpu_async()`. A texture that was prewarmed is served from `DSTextureCache` on the next `ResourceLoader` load, so a worker-thread load of a prewarmed asset is a cache hit and pays no CPU decompression.
- **Custom Godot Engine builds**: Direct GPU decompression requires Godot compiled with the D3D12 driver enabled (`d3d12=yes`). See [Custom Godot Builds](custom-godot-builds.md).
- **File I/O mode by volume**: By default (`dsgd_extension/allow_directstorage_without_bypassio = true`), DirectStorage initializes on any local Windows volume (UNC network shares are excluded). BypassIO is used on NVMe + NTFS volumes when allowed by the OS and drivers; other non-rotational local volumes (SATA/ATA/USB/SD/exFAT/RAID/virtual disks) use standard unbuffered I/O while keeping GPU GDeflate; rotational volumes automatically switch to buffered I/O (`ForceFileBuffering`) with BypassIO disabled. Setting `dsgd_extension/allow_directstorage_without_bypassio = false` restores the legacy NVMe/SATA/ATA + NTFS restriction. The choice is process-wide and is taken from the volume passed to `initialize_ds_for_path()`, or from `res://` for default startup. Initialize DirectStorage before querying capabilities; a capability probe that runs first fixes the I/O mode from `res://`.

### Frame cost on low-end GPUs (measured)

Compression buys disk bandwidth and spends GPU time. On a GTX 1050 Ti at 1920x1080, streaming 32 uncompressed 1024x1024 textures added about 0.02 ms to the median frame, while the same textures as GDeflate added about 0.36 ms at idle and about 1.06 ms while a synthetic GPU load was running; the 95th-percentile frame was 12–15 ms worse during the compressed stream. Throughput moved the other way: the compressed set delivered 2.06 GB/s of logical bytes versus 1.70 GB/s uncompressed at idle, and the two converged (about 1.4–1.5 GB/s) under load.

If you are frame-bound on a low-end or integrated GPU rather than bandwidth-bound, set `dsgd_extension/disable_gpu_decompression` so decoding happens on the CPU, and budget for the tail. Reproduce with [`project/benchmark/bench_gpu_contention.gd`](../project/benchmark/bench_gpu_contention.gd); these numbers are machine-specific and only comparable within one run set.

---

## 2. Async Texture Prewarm

For level transitions, textures can be streamed ahead of the scene bind with zero main-thread waits. The GDScript helpers live in [`ds_preloader.gd`](../project/addons/directstorage_accelerator/scripts/ds_preloader.gd) (`DSPreloader`), and the per-frame drain lives in the `DSRuntime` autoload (`collect` -> `flush_stream_async()` -> `poll_completed()` -> progress).

### Option A: One-Call Scene Prewarm (`prewarm_scene_async`)

For most level transitions, `DSPreloader.prewarm_scene_async()` handles both texture prewarming and threaded scene loading in one `await`:

```gdscript
var packed: PackedScene = await DSPreloader.prewarm_scene_async(
    "res://levels/level1.tscn",
    PackedStringArray(), # extra_scan_dirs
    16,                  # chunk_size
    true,                # prefer_gpu (auto-routed to System RAM on integrated GPUs)
    0,                   # max_chunk_bytes (0 = native 16 MiB cap; use DSPreloader.BACKGROUND_GPU_CHUNK_BYTES for background streaming)
    func(stage: String, progress: float) -> void:
        # stage is "textures" (0.0..0.5), "scene" (0.5..1.0), or "done" (1.0)
        progress_bar.value = progress * 100.0
)
```

- **Stage 1 (`"textures"`, `0.0..0.5`)**: collects scene texture dependencies (or uses `precollected_deps` if provided) and streams them into `DSTextureCache`.
- **Stage 2 (`"scene"`, `0.5..1.0`)**: loads the `PackedScene` in the background via Godot's threaded loader so dependencies bind from cache with zero CPU fallbacks, then emits `"done"` (`1.0`).
- Returns `null` on failure or cancellation; falls back to `ResourceLoader.load(..., CACHE_MODE_REUSE)` if the `DSRuntime` autoload is unavailable.
- For cancellable flows without `await`, use `DSPreloader.start_scene_prewarm()` (returns a `scene_tag`) and `DSPreloader.cancel_scene_prewarm(scene_tag)`.

If you also want a loading screen overlay handled for you, `DSPreloader.load_scene_with_loading_screen()` wraps `prewarm_scene_async()`, keeps the overlay visible across `instantiate()` + `add_child()` + one drawn frame so driver PSO/shader compilation finishes before gameplay is revealed, and falls back to a frame-budgeted sync `ResourceLoader.load()` loop (60 FPS) when async prewarm is unavailable.

### Option B: Manual Collect + Tag-Based Prewarm

When you need direct control over the texture list or want to manage a prewarm tag yourself:

1. **Collect** prewarmable engine paths (recursive `ResourceLoader.get_dependencies()` + `DirAccess` sweep, projector/EXR filtered; `DSPreloader` collects `.png`, `.jpg`, `.jpeg`, and `.webp` authoring paths, while `.tga` and `.dds` load through `ResourceLoader` and are not in `DSPreloader.PREWARMABLE_EXTENSIONS`):
   ```gdscript
   var deps: PackedStringArray = DSPreloader.collect_scene_textures("res://levels/level1.tscn")
   ```
2. **Prewarm** (non-blocking, returns a stream tag; `DSPreloader.prewarm_async()` and `DSPreloader.prewarm_gpu_async()` register progress tracking via `DSRuntime.track_prewarm(tag)` automatically, whereas manual `track_prewarm` opt-in is only needed when calling `DSTextureCache.prewarm_async()` directly), then **bind with `CACHE_MODE_REUSE`** so the scene resolver reuses the resident textures with zero disk re-reads:
   ```gdscript
   var tag: int = DSPreloader.prewarm_async(deps) # or DSPreloader.prewarm_gpu_async(deps) on Windows D3D12
   await DSPreloader.await_prewarm_finished(tag) # or poll DSPreloader.get_prewarm_progress(tag) for a bar
   var packed := ResourceLoader.load("res://levels/level1.tscn", "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
   ```
   `CACHE_MODE_IGNORE` and `CACHE_MODE_REPLACE` bypass the cache and waste the prewarm - always use `CACHE_MODE_REUSE` for the post-prewarm scene load.

### Cancellable Two-Stage Scene Prewarm (`start_scene_prewarm`)

When you want non-blocking progress polling and mid-load cancellation without awaiting `prewarm_scene_async()`, use `DSPreloader.start_scene_prewarm()` with `/root/DSRuntime` (see [`project/demo/loaders/ds_gpu_paced_loader.gd`](../project/demo/loaders/ds_gpu_paced_loader.gd)):

```gdscript
var scene_tag: int = DSPreloader.start_scene_prewarm(
	scene_path, PackedStringArray(), 16, true, 0, deps
)
while not DSRuntime.is_scene_prewarm_complete(scene_tag):
	var state: Dictionary = DSRuntime.get_scene_prewarm_state(scene_tag)
	# state["stage"] is "textures", "scene", "cancelling", or "done"
	# state["progress"] is 0.0..1.0; state["tex_tag"] holds the Stage 1 texture tag
	await get_tree().process_frame

var packed: PackedScene = DSRuntime.take_scene_prewarm_result(scene_tag)
```

- `DSRuntime` also emits `scene_prewarm_progress(scene_tag, stage, progress)` and `scene_prewarm_finished(scene_tag, scene_path, packed_scene)`.
- `take_scene_prewarm_result(scene_tag)` returns the loaded `PackedScene` and clears `DSRuntime`'s internal reference so the runtime does not retain it after handoff.
- `DSPreloader.cancel_scene_prewarm(scene_tag)` cancels Stage 1 texture streaming immediately; during Stage 2, it transitions to `"cancelling"` and drains `ResourceLoader.load_threaded_get()` safely on completion before emitting `scene_prewarm_finished(..., null)`.

### Active-Gameplay Background Streaming (`set_background_pacing`)

By default, `DSRuntime` runs in **loading-screen mode** (`DSRuntime.set_background_pacing(false)`), maximizing throughput (up to 8 GPU chunks per frame within a 5 ms budget). When streaming during **active gameplay or a live 3D menu**, switch to background pacing so frame pacing stays smooth:

```gdscript
DSRuntime.set_background_pacing(true)
var tag: int = DSPreloader.prewarm_gpu_async(
	deps,
	1,                                    # chunk_size = 1
	true,                                 # prefer_gpu
	DSPreloader.BACKGROUND_GPU_CHUNK_BYTES # 2 MiB per-frame D3D12 allocation cap
)
# ...
# When a loading screen takes over (e.g. player clicks Play):
DSRuntime.set_background_pacing(false)
```

- With `set_background_pacing(true)`, `DSRuntime` gates GPU chunk pumping on in-flight request completion, uses VSync/`Engine.max_fps`-aware frame-headroom thresholds, and runs an AIMD-paced CPU/RAM pump.
- When GPU prewarm is unavailable and background pacing is active, `DSRuntime.start_scene_prewarm()` automatically selects `DSTextureCache.prewarm_cpu_paced_async(deps)`. Manual callers can also call `DSTextureCache.prewarm_cpu_paced_async(deps)` directly (remembering to call `DSRuntime.track_prewarm(tag)` if progress signals are needed).

### Off-Thread Discovery & Archive TOC Fast Path (`WorkerThreadPool` + `extra_scan_dirs`)

On large scenes, `DSPreloader.collect_scene_textures()` and native prewarm enqueuing can hitch Frame 1/2 if run synchronously on the main thread:

- **Offload collection to `WorkerThreadPool`**: run `DSPreloader.collect_scene_textures(...)` inside `WorkerThreadPool.add_task(...)` and poll `is_task_completed()` per frame before starting the prewarm. `DSRuntime.start_scene_prewarm()` already offloads the native prewarm enqueue step onto `WorkerThreadPool` automatically; if you call `DSTextureCache.prewarm_gpu_async()` / `prewarm_async()` on a worker thread yourself, keep `DSRuntime.track_prewarm(tag)` on the main thread.
- **Pass `extra_scan_dirs` for the `.dspkg` TOC fast path**: in exported builds (or when `dsgd_extension/force_archive_resolution` is enabled), passing non-empty `extra_scan_dirs` lets `collect_scene_textures()` scan the open `DSPkg` table of contents directly in under 0.5 ms instead of a 100+ ms recursive `ResourceLoader.get_dependencies()` walk. On v3+ archives, `as_image_texture` exclusion flags are read straight from the TOC with zero `.import` disk reads. (`dsgd_extension/skip_archive_import_check = true` bypasses the exclusion check for legacy v1/v2 archives when no excluded imports exist.)

### Speculative Preloading, Tag Adoption & Cancellation

For live menus or hub worlds that speculatively preload the next level:

- **Wait for the first rendered frame**: defer speculative preloads until after boot splash and `await RenderingServer.frame_post_draw`.
- **Adopt instead of restarting**: if the player starts the level while a speculative prewarm is already in flight, adopt the active tag (or `scene_tag`), flip `DSRuntime.set_background_pacing(false)` so the remaining assets finish at full loading-screen speed, and await completion.
- **Cancel and drain cleanly**: on Cancel, Quit, or Load Error, call `DSPreloader.cancel_prewarm(tag)` (or `DSPreloader.cancel_scene_prewarm(scene_tag)`). If your game also started its own `ResourceLoader.load_threaded_request()`, drain it with `ResourceLoader.load_threaded_get()` and drop the reference before calling `DSTextureCache.clear_unused()`.

---

## 3. Cache Ownership & Residency

`DSPreloader` drives the streaming queue and reports its state; it deliberately does not make policy decisions for the game.

- **Residency is intentional.** `DSTextureCache` keeps prewarmed textures resident after a scene swap, so memory staying high (roughly one level's worth of textures) is the cache working, not a leak. Resident entries load again with no disk read.
- **Releasing a level's GPU resources is the game's job.** Drop the references the scene held (`PackedScene` and node references), `queue_free()` the old scene, wait for `process_frame` plus `RenderingServer.frame_post_draw`, and then call `DSTextureCache.clear_unused()`; entries that still have a live reference are kept. Judge the result by whether you are still hitting your session's own best throughput, not by a fixed millisecond budget - the same byte count is cheap with memory headroom and slow when the system is paging.
- **The addon will not warn you about scene teardown.** A slow quit is the engine freeing the scene graph plus the driver releasing memory; `DSPreloader` does not watch for it and emits nothing. Do not wait for a warning that will never come.
- **Eviction granularity is per call, not per microsecond.** There is no `clear_unused_budget()`, and none is planned: the call itself is not where the time goes. To spread the cost, release references in stages and call `clear_unused()` after each stage.

---

## 4. Stall Detection & Diagnostics

Use `stall_timeout_msec` in `await_prewarm_finished()` so a wedged load is one call instead of three pollers:

```gdscript
# Returns false (and cancels the tag) instead of polling forever.
var ok: bool = await DSPreloader.await_prewarm_finished(tag, on_progress, 8000, on_stall)
if not ok:
    # {"progress": Vector2i, "complete": bool, "pending": int, "unbuilt": int,
    #  "unbuilt_cpu": int, "unbuilt_gpu": int}
    push_warning("prewarm wedged: %s" % DSPreloader.get_prewarm_state(tag))
    # fall back to the synchronous load path here
```

`stall_timeout_msec = 0` keeps the historical "poll until done" behaviour, so existing callers need no change. The stall check watches progress *and* the pending/unbuilt counters, because a batch can finish every request and still be building textures.

---

## 5. Runtime API & Export Resolution

The native classes are documented in Godot's Editor Help (`doc_classes/`) and support Ctrl-click navigation from GDScript:

- `DSContext`: initialize DirectStorage hardware queues and staging buffers, enqueue `DSRequest` objects, execute synchronous (`flush_batch`) or asynchronous (`flush_batch_async`) batches, handle completion signals (`batch_completed`, `batch_failed`), and query drive/GPU capabilities.
- `DSRequest`: configure source file paths, byte ranges, compression format (`COMPRESSION_NONE`, `COMPRESSION_GDEFLATE`), destination target (`DESTINATION_MEMORY`, `DESTINATION_GPU_BUFFER`, `DESTINATION_GPU_TEXTURE`), subresource ranges, request priorities (`PRIORITY_LOW`, `PRIORITY_NORMAL`, `PRIORITY_HIGH`), and query execution status.
- `DSTex`: create or inspect `.dstex` (v1, v2, and v3) files and retrieve per-mip footprint metadata (`mip_offsets`, `mip_sizes`, `mip_uncompressed_sizes`, `mip_row_pitches`).
- `DSPkg`: build or read bundled `.dspkg` archives (v1–v5) with per-entry GPU mip tables, flags, and optional TOC encryption.
- `GDeflateCompressor` and `GDeflateDecompressor`: encode and decode GDeflate tile-compressed payloads with configurable thread pools.
- `DSTextureCache`: inspect or manage the runtime texture LRU cache.
- `DSErrorHandler` and `DSProfiler`: monitor asynchronous DirectStorage kernel errors, measure physical storage throughput, and inspect logical decompression metrics.

`DSContext`, `DSErrorHandler`, `DSProfiler`, and `DSTextureCache` are registered as Engine singletons (`Engine.get_singleton("DSContext")`, etc.).

### Low-Level D3D12 GPU Destination State Contract

This note applies to developers calling the low-level `DSRequest` / `DSContext` GPU destination APIs directly (`DESTINATION_GPU_TEXTURE`, `DESTINATION_GPU_BUFFER`). Standard `ResourceLoader` and `DSPreloader` users do not need to manage D3D12 resource states.

DirectStorage requires destination textures to be in the D3D12 `COMMON` state. Godot only creates a fresh texture in `COMMON` when Enhanced Barriers are available (or cross-family relaxed casting applies). On adapters without either, a freshly created texture starts in `COPY_DEST` instead, and GPU destination behavior is unverified there; use a memory destination (`DESTINATION_MEMORY`) on those adapters when strict compliance matters. For exact method contracts, see the in-editor Help (`doc_classes/`) for `DSRequest.set_destination_resource()` and `DSContext.create_destination_texture()`.

### Export Packaging & Runtime Archive Discovery

During export, [`ds_export_plugin.gd`](../project/addons/directstorage_accelerator/scripts/ds_export_plugin.gd) intercepts `.dstex`-backed textures, indexes each entry under both its logical `res://` path and its `.godot/imported/*.dstex` remap path, calls `skip()` to strip texture payloads from the `.pck` (while preserving `.import` files), aligns multi-mip payloads to 512-byte D3D12 boundaries, and writes `textures.dspkg` beside the exported executable.

At runtime, `DSRuntime` discovers the archive (`textures.dspkg`, `res://`, or `OS.get_executable_path().get_base_dir()`) with a 64-bit archive id pairing check, and `DSResourceFormatLoader` intercepts standard `ResourceLoader.load()` and `load_threaded_request()` calls transparently. For the full export decision table, fail-closed rules, and archive layout contract, see [Export & Archive Behaviour](for_developers/export-and-archive-behaviour.md).

---

## Related Documents

- [Diagnostics, Profiling & Benchmarking](diagnostics-and-profiling.md) - `DSProfiler`, `DSContext` capability/loader diagnostics, `DSRuntime` slice telemetry, and mode-switching
- [Benchmarks & Performance Analysis](benchmarks.md) - Measured cold-boot, background-streaming, and memory-reclaim results (`arena_v2` and `bistro`)
- [Texture Import & Compression (.dstex)](import-and-compression.md) - `.dstex` workflow, GDeflate level guidance, and chunked payloads
- [Addon Reference](../project/addons/directstorage_accelerator/README.md) - `texture_cache_enabled`, `texture_cache_max_entries`, and auto-tune settings
- [Custom Godot Builds](custom-godot-builds.md) - Building Godot from source with D3D12 enabled
- [Export & Archive Behaviour](for_developers/export-and-archive-behaviour.md) - Developer internals for export packaging and archive layout
- [Demo Project (`project/README.md`)](../project/README.md) - F5 A/B benchmark and 5-mode loading demo setup (EditorScript / headless CLI procedural asset generation)
