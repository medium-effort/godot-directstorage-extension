# DirectStorage Accelerator

This addon exposes DirectStorage-backed texture streaming and asynchronous I/O request APIs to Godot 4.

## Editor Workflow

Enable the `DirectStorage Accelerator` plugin. It registers the `DSBatchManager` autoload, the `DirectStorage Texture` importer, and the `DirectStorageExporter` export plugin.

The importer converts `.png`, `.jpg`, `.jpeg`, and `.webp` sources into `.dstex` (version 2) resources with D3D12-aligned footprints (256-byte row pitch alignment and 512-byte mip placement alignment) for direct hardware decompression. Its Import dock options control source compression mode (S3TC vs uncompressed), source color space, and GDeflate compression level.

During export, imported `.dstex` files are bundled into `res://textures.dspkg` (version 3) along with per-entry GPU mip metadata and entry flags. The runtime resource loader resolves texture paths directly from that archive without requiring individual `.dstex` files in the exported PCK.

## Direct GPU Decompression & Fallback

When running on Windows with a Direct3D 12 `RenderingDevice` (`--rendering-driver d3d12`), DirectStorage decompresses GDeflate mip streams directly into GPU VRAM as `Texture2DRD` resources via compute shaders. If GPU decompression is disabled, unsupported, or if legacy version 1 assets are loaded, the system automatically falls back to multi-threaded CPU software decompression (`ImageTexture`).

## Runtime API

The native classes are documented in Godot's Editor Help and support Ctrl-click navigation from GDScript. The main entry points are:

- `DSContext`: initialize DirectStorage hardware queues and staging buffers, enqueue `DSRequest` objects, execute synchronous (`flush_batch`) or asynchronous (`flush_batch_async`) batches, handle completion signals (`batch_completed`, `batch_failed`), and query drive/GPU capabilities.
- `DSRequest`: configure source file paths, byte ranges, compression format (`COMPRESSION_NONE`, `COMPRESSION_GDEFLATE`), destination target (`DESTINATION_MEMORY`, `DESTINATION_GPU_BUFFER`, `DESTINATION_GPU_TEXTURE`), subresource ranges, request priorities (`PRIORITY_LOW`, `PRIORITY_NORMAL`, `PRIORITY_HIGH`), and query execution status.
- `DSTex`: create or inspect `.dstex` (v1 and v2) files and retrieve per-mip footprint metadata (`mip_offsets`, `mip_sizes`, `mip_uncompressed_sizes`, `mip_row_pitches`).
- `DSPkg`: build or read bundled `.dspkg` archives (v1, v2, and v3) with per-entry GPU mip tables and flags.
- `GDeflateCompressor` and `GDeflateDecompressor`: encode and decode GDeflate tile-compressed payloads with configurable thread pools.
- `DSTextureCache`: inspect or manage the runtime texture LRU cache.
- `DSErrorHandler` and `DSProfiler`: monitor asynchronous DirectStorage kernel errors, measure physical storage throughput, and inspect logical decompression metrics.

The extension registers `DSContext`, `DSErrorHandler`, `DSProfiler`, and `DSTextureCache` as Engine singletons. Access them via `Engine.get_singleton("DSContext")` or the corresponding singleton name.

## Project Settings

The addon exposes configurable project settings under the `directstorage/` namespace in Godot's Project Settings (`Project -> Project Settings... -> General -> DirectStorage`):

| Setting | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/staging_buffer_size_mb` | `int` | `128` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (clamped range: `1`–`2048` MiB). Increase this if loading very large textures or high-resolution mip streams whose compressed payloads exceed the buffer. |
| `directstorage/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `directstorage/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (clamped range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `directstorage/texture_cache_enabled` | `bool` | `true` | Enables or disables the internal runtime texture LRU cache (`DSTextureCache`). When disabled, loaded textures bypass the resident cache. |
| `directstorage/texture_cache_max_entries` | `int` | `128` | Maximum number of texture entries held in the LRU cache (range: `16`–`2048`). When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. |
| `directstorage/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |
| `directstorage/sync_flush_timeout_ms` | `int` | `3000` | Maximum timeout in milliseconds (clamped range: `500`–`10000` ms) for synchronous batch flushes (`submit_batch` / `flush_batch`). Dictates the timeout budget for hardware queue execution and fence completion, with up to half (`sync_timeout_ms / 2`) allocated for draining prior in-flight batches beforehand. Synchronous flushes should never be called inside gameplay process loops (`_process`). |

### Notes on Queue Capacity and Timeouts

- **Queue Capacity & Streaming Bursts**: `directstorage/queue_capacity` defines the hardware queue slot capacity (default `512`, clamped range: `128`–`8192`). Textures with full mip chains expand into multiple subresource requests (e.g., 10–12 mips per texture). The streaming engine (`DSContext::flush_stream_async`) automatically budgets burst popping based on subresource counts and applies transient backpressure requeuing to prevent queue exhaustion during bulk scene prewarming.
- **Synchronous Timeout Budget**: During synchronous fallback loads (e.g. scene loading uncached textures via `ResourceLoader`), `directstorage/sync_flush_timeout_ms` ensures adequate headroom for NVMe DMA transfers, GDeflate hardware decompression, and GPU barrier completion under high engine contention.


