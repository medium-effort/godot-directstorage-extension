# DirectStorage Accelerator

This addon exposes DirectStorage-backed texture streaming and asynchronous I/O request APIs to Godot 4.

## Editor Workflow

Enable the `DirectStorage Accelerator` plugin. It registers the `DSBatchManager` autoload, the `DirectStorage Texture` importer, and the `DirectStorageExporter` export plugin.

The importer converts `.png`, `.jpg`, `.jpeg`, and `.webp` sources into `.dstex` (version 2) resources with D3D12-aligned footprints (256-byte row pitch alignment and 512-byte mip placement alignment) for direct hardware decompression. Its Import dock options control source compression mode (S3TC vs uncompressed), source color space, and GDeflate compression level.

During export, imported `.dstex` files are bundled into `res://textures.dspkg` (version 2) along with per-entry GPU mip metadata. The runtime resource loader resolves texture paths directly from that archive without requiring individual `.dstex` files in the exported PCK.

## Direct GPU Decompression & Fallback

When running on Windows with a Direct3D 12 `RenderingDevice` (`--rendering-driver d3d12`), DirectStorage decompresses GDeflate mip streams directly into GPU VRAM as `Texture2DRD` resources via compute shaders. If GPU decompression is disabled, unsupported, or if legacy version 1 assets are loaded, the system automatically falls back to multi-threaded CPU software decompression (`ImageTexture`).

## Runtime API

The native classes are documented in Godot's Editor Help and support Ctrl-click navigation from GDScript. The main entry points are:

- `DSContext`: initialize DirectStorage hardware queues and staging buffers, enqueue `DSRequest` objects, execute synchronous (`flush_batch`) or asynchronous (`flush_batch_async`) batches, handle completion signals (`batch_completed`, `batch_failed`), and query drive/GPU capabilities.
- `DSRequest`: configure source file paths, byte ranges, compression format (`COMPRESSION_NONE`, `COMPRESSION_GDEFLATE`), destination target (`DESTINATION_MEMORY`, `DESTINATION_GPU_BUFFER`, `DESTINATION_GPU_TEXTURE`), subresource ranges, request priorities (`PRIORITY_LOW`, `PRIORITY_NORMAL`, `PRIORITY_HIGH`), and query execution status.
- `DSTex`: create or inspect `.dstex` (v1 and v2) files and retrieve per-mip footprint metadata (`mip_offsets`, `mip_sizes`, `mip_uncompressed_sizes`, `mip_row_pitches`).
- `DSPkg`: build or read bundled `.dspkg` archives (v1 and v2) with per-entry GPU mip tables.
- `GDeflateCompressor` and `GDeflateDecompressor`: encode and decode GDeflate tile-compressed payloads with configurable thread pools.
- `DSTextureCache`: inspect or manage the runtime texture LRU cache.
- `DSErrorHandler` and `DSProfiler`: monitor asynchronous DirectStorage kernel errors, measure physical storage throughput, and inspect logical decompression metrics.

The extension registers `DSContext`, `DSErrorHandler`, `DSProfiler`, and `DSTextureCache` as Engine singletons. Access them via `Engine.get_singleton("DSContext")` or the corresponding singleton name.
