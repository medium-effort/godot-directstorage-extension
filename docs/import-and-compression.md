# Texture Import & Compression (.dstex)

This guide covers how `dsgd` imports `.dstex` textures, handles non-multiple-of-4 dimensions, tunes GDeflate compression levels, and splits large mips into chunked payloads.

For full tables of **DSTexture Import Settings** and **Project Settings (`dsgd_extension/*`)**, see the [Addon Reference](../project/addons/directstorage_accelerator/README.md).

---

## 1. Editor Workflow & `.dstex` Format

When `dsgd` plugin is enabled, it registers the `DSRuntime` autoload, the `DirectStorage Texture` importer, and the `dsgd Extension` export plugin.

The autoload entry is written to `project.godot` when the plugin is enabled and removed only when you disable the plugin again - commit it with the rest of your project settings. Closing the editor keeps it, because standalone and exported builds need that node to drive the asynchronous prewarm queue.

On Windows, enabling the plugin automatically opens the Runtime Manager dialog if `dstoragecore.dll` is not installed yet; on startup the plugin also checks `.gdextension` manifest health and warns in the Output panel if the native library is missing, duplicated, or loaded from an unexpected path. Whenever you open a scene in the editor, the plugin scans `Light3D.light_projector` and `Decal` nodes and prints a yellow Output warning listing any textures bound to `Texture2DRD` that should be re-imported with `compatibility/as_image_texture`.

The importer converts `.png`, `.jpg`, `.jpeg`, `.webp`, `.tga`, and `.dds` sources into `.dstex` (version 2, or version 3 when the source is not a multiple of 4 pixels, a mip is split into chunks, `compression/disk_compression` is `None` or GDeflate does not shrink the payload, or `depth_or_layers > 0`) resources with D3D12-aligned footprints (256-byte row pitch alignment and 512-byte mip placement alignment) for direct hardware decompression. Non-multiple-of-4 sources keep their authoring dimensions as the logical size in the v3 header while the payload stores the 4-pixel-aligned footprint; the loader applies a size override so the texture reports and renders at its logical size on the CPU/`ImageTexture` path. GPU-decoded textures wrap the padded storage footprint in `Texture2DRD`, which has no size setter and therefore reports the storage size (for example `100x52`); enable `compatibility/as_image_texture` when the authoring size must be reported.

Three presets are provided: **3D (Streaming)** (the default), **CPU / ImageTexture (2D, UI, Decals)** (uncompressed, no mipmaps, CPU-loaded), and **Project Default**, which follows `dsgd_extension/default_import_profile`. Existing `.import` files keep their stored values on reimport; the project setting only affects new imports. A pure 2D project should normally use Godot's own `Texture2D` importer instead of this addon.

---

## 2. GDeflate Compression Level

`compression/gdeflate_level` (`1–12`, default `3`) trades **import time** for a smaller `.dstex` file on disk. It does **not** affect runtime decompression speed - GPU and CPU decode cost is identical per byte at every level.

As a rule of thumb:

- **Level `3` (Default)**: Virtually the same import speed as level `1`, while capturing a free size win:
  - **Photographic 3D textures (S3TC/BC1/BC5)**: ~1–2% smaller than level 1 (albedo gains most, ORM/masks gain least).
  - **Stylized / flat-color 3D art (S3TC)**: ~3–15% smaller than level 1.
  - **Large uncompressed textures (`compression/mode = 1`)**: ~6–27% smaller than level 1 (tiny UI icons only save a few bytes).
- **Level `6`**: Roughly **1.5–2x slower** to import for another **2–10%** size reduction. Worth considering for release-only packaging of large stylized or uncompressed assets.
- **Levels `9–12`**: **6–12x slower** to import (the libdeflate cliff) for only a few extra percent. Avoid these for day-to-day work or CI reimports unless distribution size is critical.

For full per-asset measurement tables and timings, see [GDeflate Compression Level Benchmarks](for_developers/gdeflate-compression-benchmarks.md).

---

## 3. Chunked Payloads

`compression/chunk_size_kb` (default `8192`) splits each mip into independent GDeflate chunks of at most that many uncompressed KiB; `0` restores the legacy one-stream-per-mip layout, which forces the Windows backend to split each mip at load time and to size the staging buffer for a whole mip.

DirectStorage requires every request to fit the staging buffer, so one stream per mip forced `staging_buffer_size_mb` to grow with the largest mip (a 4096x4096 RGBA8 mip is 64 MiB; an 8192x8192 mip is 256 MiB). Chunks decouple the two: the staging floor becomes the largest chunk (8 MiB by default), and the importer warns with the exact number when an asset still exceeds the configured staging size.

Chunk count also spends the queue budget: each chunk is one request, and each submitted batch appends one signal and one set-event, so at most `queue_capacity - 2` requests fit in a batch; the loader falls back to CPU decompression above that and warns. Chunked textures are stored in v3 `.dstex` files and v5 `.dspkg` archives; the GPU path uploads each chunk as a row-range texture request and the CPU path reassembles the per-chunk streams. Prewarm supports chunked payloads on both paths: the System RAM path expands one request per chunk, and direct-to-VRAM GPU prewarm emits per-chunk row-range requests in the same shape (`GDeflate` and `NONE` raw slices; textures whose fan-out exceeds `queue_capacity - 2` stay on the System RAM path).

---

## Related Documents

- [Runtime Texture Streaming, Prewarm & Caching](runtime-streaming-and-prewarm.md) - GPU/CPU fallback, volume I/O, `DSPreloader`, `DSTextureCache`, and Runtime API
- [Diagnostics, Profiling & Benchmarking](diagnostics-and-profiling.md) - `DSProfiler`, `DSContext` capability/loader diagnostics, `DSRuntime` slice telemetry, and mode-switching
- [Addon Reference](../project/addons/directstorage_accelerator/README.md) - Full `.dstex` import settings, `dsgd_extension/*` Project Settings, and export preset options
- [Binary Format Version History](for_developers/format-versions.md) - `.dstex` (v1–v3) and `.dspkg` (v1–v5) container layouts
- [GDeflate Compression Level Benchmarks](for_developers/gdeflate-compression-benchmarks.md) - Per-asset measurement tables and timings
