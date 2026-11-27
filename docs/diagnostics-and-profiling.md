# Diagnostics, Profiling & Benchmarking

This guide covers how to monitor DirectStorage performance at runtime, inspect hardware and loader fallbacks, read `DSRuntime` backpressure telemetry, and structure A/B benchmarks or archive tests.

---

## 1. Runtime Profiling (`DSProfiler` & `DSProfilerResult`)

`DSProfiler` is registered as an Engine singleton (`Engine.get_singleton("DSProfiler")`).

### Quick HUD / Debug Overlay Metrics

For an in-game debug label, combine `DSProfiler`, `DSContext`, and `DSTextureCache`:

```gdscript
if Engine.has_singleton("DSContext"):
	var ctx := Engine.get_singleton("DSContext")
	if ctx and ctx.is_initialized():
		var backend: String = ctx.get_active_backend_name()
		var gpu_tier: int = ctx.get_gpu_decompression_support_tier()
		var staging_mb: int = ctx.get_staging_buffer_size_mb()

if Engine.has_singleton("DSProfiler"):
	var profiler := Engine.get_singleton("DSProfiler")
	var throughput: float = profiler.get_last_throughput_mbps()
	var last_bytes: int = profiler.get_last_bytes()
	var last_us: int = profiler.get_last_duration_us()
	var queue_depth: int = profiler.get_queue_depth()

if Engine.has_singleton("DSTextureCache"):
	var cache := Engine.get_singleton("DSTextureCache")
	var cached: int = cache.get_cached_count()
	var max_entries: int = cache.get_max_entries()
```

### Batch History (`get_last_result()` / `get_result_history()`)

`DSProfiler.get_last_result()` returns the most recent `DSProfilerResult`, and `DSProfiler.get_result_history()` returns up to 1024 `DSProfilerResult` objects in chronological order (oldest to newest):

- `get_compressed_bytes_read()` - physical bytes read from storage for the batch.
- `get_uncompressed_bytes_produced()` - logical uncompressed bytes produced after decompression.
- `get_request_count()` - number of storage requests in the batch.
- `get_physical_read_duration_usec()` - measured batch read/execution duration in microseconds.
- `get_decompression_duration_usec()` - decompression duration in microseconds (`-1` when not separately timed).
- `get_error_state()` - batch error code (`0` on success).
- `get_start_time_usec()` / `get_end_time_usec()` - monotonic wall-clock brackets in microseconds (`0` for legacy callers).
- `get_logical_throughput_mbps()` / `get_logical_throughput_mibps()` - logical throughput in MB/s (10^6) or MiB/s (2^20).
- `get_physical_throughput_mbps()` / `get_physical_throughput_mibps()` - physical throughput in MB/s (10^6) or MiB/s (2^20).

**Pipelined batch throughput pitfall**: When multiple async batches overlap in flight, summing `get_physical_read_duration_usec()` across every sample double-counts the overlapping time. Merge each sample's `[get_start_time_usec(), get_end_time_usec()]` interval (union of intervals) to compute effective wall-clock throughput across a multi-batch load.

### Frame-Time Impact Window (`get_frame_window_stats()`)

`DSRuntime` feeds `DSProfiler.record_frame(delta)` every frame and brackets active streaming loads with `begin_load_window()` / `end_load_window()` (capped at 36,000 frames so an abandoned load never freezes the idle baseline). Call `DSProfiler.get_frame_window_stats()` to compare frame times before and during a load:

- `active` (`bool`): whether a load window is currently open.
- `mean_frame_before_usec` (`float`): rolling idle baseline mean over up to 60 pre-load frames.
- `mean_frame_during_usec` (`float`): mean frame time during the current or last completed load window.
- `frame_count` (`int`): frames in the current or last completed window.
- `completed_windows` (`int`): total completed load windows.

### Godot Debugger Performance Monitors

`DSProfiler` automatically registers five custom monitors at startup under Godot's **Debugger > Monitors** tab (and exposes `register_performance_monitors()` / `unregister_performance_monitors()` for manual control):

- `DirectStorage/Throughput (MB/s)`
- `DirectStorage/Requests/Second`
- `DirectStorage/Avg Latency (ms)`
- `DirectStorage/Queue Depth`
- `DirectStorage/GPU Decompression Active`

---

## 2. Capability, Hardware & Loader Diagnostics (`DSContext`)

### Full Snapshot (`get_capability_report()`)

`DSContext.get_capability_report()` returns a `Dictionary` combining runtime, GPU, and loader telemetry:

- **SDK vs. installed runtime**: `directstorage_version` (the SDK version the extension was built against) vs. `runtime_version`, `runtime_loaded`, and `runtime_capabilities` (the `dstoragecore.dll` actually present/loaded on Windows).
- **GPU decompression**: `gpu_decompression_available`, `gpu_decompression_enabled`, `gpu_decompression_tier`, `gpu_decompression_queue_type` (`"compute"`, `"copy"`, or `"none"`), `gpu_decompression_optimized`, `gpu_decompression_shader_fallback`, and `cpu_decompression_fallback_available`.
- **Adapter & backend**: `active_backend`, `adapter_name`, `adapter_vendor`, `dedicated_vram_mb`, `shared_system_mb`, `software_adapter`, `is_uma`, `is_cache_coherent_uma`, `integrated_gpu_guess`, `supported_destination_types`, and optional `destination_handoff` diagnostics when present.
- **Loader outcome counters**: `loader_gpu_loads`, `loader_cache_hits`, and `loader_cpu_fallbacks`.

**Diffing loader counters around a scene load**: `loader_gpu_loads`, `loader_cache_hits`, and `loader_cpu_fallbacks` are process-lifetime atomics with no reset API. Snapshot `get_capability_report()` before and after a scene load and diff the values. `loader_cpu_fallbacks` is a `Dictionary` keyed by fallback reason (only non-zero reasons are present):

- `image_texture_opt_in` (expected for `Decal` or `Light3D.light_projector` textures imported with `compatibility/as_image_texture = true`)
- `embedded_in_pck`, `gpu_disabled`, `legacy_v1`, `sub_threads`, `worker_thread`, `separate_thread`, `no_d3d12`, `no_rendering_device`, `no_mips`, `chunk_layout`, `queue_budget`, `gpu_failed`

### Focused Single-Purpose Queries

When you do not need the full capability report (which queries D3D12 adapter/compression state):

- `get_init_status()` - returns `DSContext.InitStatus`: `INIT_STATUS_UNINITIALIZED` (`0`), `INIT_STATUS_DIRECTSTORAGE_D3D12` (`1`), `INIT_STATUS_SOFTWARE_FALLBACK` (`2`), or `INIT_STATUS_FAILED` (`3`).
- `get_drive_info(path = "res://")`, `is_current_drive_supported()`, `is_path_supported(path)` - inspect Windows volume bus type, filesystem, and DirectStorage support for a path.
- `get_gpu_adapter_info()`, `is_integrated_gpu()`, `is_uma()` - query D3D12 adapter identity and UMA/VRAM details.
- `get_runtime_info()` - returns installed Windows DirectStorage runtime details (returns `{}` off Windows).

### DirectStorage Error Inspection (`DSErrorHandler`)

`DSErrorHandler` is registered as an Engine singleton (`Engine.get_singleton("DSErrorHandler")`) and records asynchronous DirectStorage queue errors:

```gdscript
if Engine.has_singleton("DSErrorHandler"):
	var err_handler := Engine.get_singleton("DSErrorHandler")
	err_handler.process_queue_errors() # Windows only: drains IDStorageQueue error records
	if err_handler.has_error():
		push_warning("DirectStorage error: %s" % err_handler.get_last_error())
		err_handler.clear_error()
```

- Static helpers: `DSErrorHandler.describe_hresult(hresult)` and `DSErrorHandler.describe_command_type(command_type)` format Windows `HRESULT` values and DirectStorage command types into readable strings.

---

## 3. Backpressure Slice Telemetry (`DSRuntime`)

While `DSRuntime` drains async prewarm batches, it classifies each CPU/GPU ingestion slice as `"warm"`, `"mild"`, or `"heavy"` using a frame-time EMA and the device's idle baseline.

On `/root/DSRuntime`:

- `set_slice_debug(enabled: bool)` - enables transition and drain-summary prints (`DSRuntime prewarm drained: warm=... mild=... heavy=...`). Off by default.
- `reset_slice_stats()` - clears the per-batch `warm`/`mild`/`heavy` counters while preserving the learned idle frame-time baseline.
- `get_slice_stats()` - returns `{"warm": int, "mild": int, "heavy": int, "label": String}`.

---

## 4. Benchmarking & A/B Mode-Switching Patterns

### Switching Backends in One Process

Benchmark harnesses can compare `SoftwareFallback` (CPU), D3D12 CPU-memory, and D3D12 GPU decompression in a single process by shutting down and re-initializing `DSContext`:

```gdscript
var ctx := Engine.get_singleton("DSContext")
if ctx.is_initialized():
	ctx.shutdown_ds()

# Keep ProjectSettings in sync: dsgd_extension/force_software_fallback overrides the setter if set.
ProjectSettings.set_setting("dsgd_extension/force_software_fallback", use_software_fallback)
ProjectSettings.set_setting("dsgd_extension/disable_gpu_decompression", disable_gpu)
ProjectSettings.set_setting("dsgd_extension/auto_tune_for_integrated_gpu", false)

ctx.set_force_software_fallback(use_software_fallback)
ctx.set_disable_gpu_decompression(disable_gpu)
ctx.set_auto_tune_for_integrated_gpu(false)
ctx.initialize_ds()
```

Verify the resulting state with `ctx.get_init_status()` and `ctx.is_gpu_decompression_enabled()`.

### Testing `.dspkg` Archives Without Exporting

By default, editor runs (`OS.has_feature("editor")`) crawl loose `.dstex` files instead of opening `textures.dspkg`. For local archive benchmarks or testing:

- `dsgd_extension/force_archive_resolution` (`bool`, default `false`) - enables `.dspkg` resolution during editor/dev runs.
- `dsgd_extension/archive_name` (`String`, default `"textures.dspkg"`) - selects a specific archive filename (for example switching between `textures.dspkg` and `textures_raw.dspkg`).

### Measuring Cache Eviction & VRAM Cleanly

To measure a clean eviction pass after freeing a benchmark scene:

1. Clear `PackedScene` / node references, call `queue_free()`, and wait two `process_frame`s plus `RenderingServer.frame_post_draw`.
2. Call `DSTextureCache.clear_unused()`.
3. Wait several frames (for example 10 `process_frame`s + `RenderingServer.frame_post_draw`) so the D3D12 fence ring drains.

Assert `DSTextureCache.get_cached_count() == 0` to verify cache eviction. Godot's `RenderingDevice` retains a baseline working-set allocation after eviction, so process-wide `RENDER_VIDEO_MEM_USED` will not return all the way to its pre-load baseline.

---

## Related Documents

- [Benchmarks & Performance Analysis](benchmarks.md) - Measured cold-boot, background-streaming, and memory-reclaim results (`arena_v2` and `bistro`)
- [Runtime Texture Streaming, Prewarm & Caching](runtime-streaming-and-prewarm.md) - `DSPreloader` streaming patterns, background pacing, and `DSTextureCache` residency
- [Verifying an Exported Build](verifying-an-exported-build.md) - `DSPKG_BOOT` sentinel and pack verification for exported builds
- [Addon Reference](../project/addons/directstorage_accelerator/README.md) - `dsgd_extension/*` Project Settings and export options
- [Export & Archive Behaviour](for_developers/export-and-archive-behaviour.md) - Developer internals for export packaging and `DSPkg` builder options
