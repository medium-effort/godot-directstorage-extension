# dsgd - Unofficial DirectStorage binding

This addon exposes DirectStorage-backed texture streaming and asynchronous I/O request APIs to Godot 4.

## Editor Workflow

Enable the `dsgd` plugin (`dsgd - Unofficial DirectStorage binding` in **Project Settings > Plugins**). It registers the `DSRuntime` autoload, the `DirectStorage Texture` importer, and the `DirectStorageExporter` export plugin.

The autoload entry is written to `project.godot` when the plugin is enabled and removed only when you disable the plugin again — commit it with the rest of your project settings. Closing the editor keeps it, because standalone and exported builds need that node to drive the asynchronous prewarm queue.

The importer converts `.png`, `.jpg`, `.jpeg`, and `.webp` sources into `.dstex` (version 2, or version 3 when the source is not a multiple of 4 pixels or a mip is split into chunks) resources with D3D12-aligned footprints (256-byte row pitch alignment and 512-byte mip placement alignment) for direct hardware decompression. Non-multiple-of-4 sources keep their authoring dimensions as the logical size in the v3 header while the payload stores the 4-pixel-aligned footprint; the loader applies a size override so the texture reports and renders at its logical size on the CPU/`ImageTexture` path. GPU-decoded textures wrap the padded storage footprint in `Texture2DRD`, which has no size setter and therefore reports the storage size (for example 100x52); enable `compatibility/as_image_texture` when the authoring size must be reported. Its Import dock options control source compression mode (S3TC, BPTC/BC7, or uncompressed), color space, normal-map processing, channel packing, mipmap generation, a source size limit, channel remapping, alpha-border/premultiply processing, and GDeflate compression level.

Three presets are provided: **3D (Streaming)** (the default), **CPU / ImageTexture (2D, UI, Decals)** (uncompressed, no mipmaps, CPU-loaded), and **Project Default**, which follows `dsgd_extension/default_import_profile`. Existing `.import` files keep their stored values on reimport; the project setting only affects new imports. A pure 2D project should normally use Godot's own Texture2D importer instead of this addon.

### GDeflate compression level

`compression/gdeflate_level` (1-12) trades importer time for a smaller `.dstex`. The level is applied at import only: it changes the stored bytes, not the runtime decompression cost, which is per-byte identical at every level. On the default path the payload is already block-compressed (S3TC/BC1), so the gains flatten quickly. Measured on 8 logical cores, mip 0 of a 4096x4096 albedo (8 MiB S3TC payload) with the addon's own compressor:

| Level | `.dstex` payload | vs level 1 | Compression time |
| --- | --- | --- | --- |
| 1 (default) | 3,240,912 B | - | 187 ms |
| 3 | 3,187,260 B | -1.7% | 193 ms |
| 6 | 3,083,248 B | -4.9% | 238 ms |
| 9 | 2,975,292 B | -8.2% | 1126 ms |
| 12 | 2,959,824 B | -8.7% | 2526 ms |

### Chunked payloads

`compression/chunk_size_kb` (default `8192`) splits each mip into independent GDeflate chunks of at most that many uncompressed KiB; `0` restores one stream per mip. DirectStorage requires every request to fit the staging buffer, so one stream per mip forced `staging_buffer_size_mb` to grow with the largest mip (a 4096x4096 RGBA8 mip is 64 MiB, an 8192x8192 mip is 256 MiB). Chunks decouple the two: the staging floor becomes the largest chunk (8 MiB by default), and the importer warns with the exact number when an asset still exceeds the configured staging size. Chunked textures are stored in v3 `.dstex` files and v5 `.dspkg` archives; the GPU path uploads each chunk as a row-range texture request and the CPU path reassembles the per-chunk streams. Prewarm supports chunked payloads through the System RAM path; direct-to-VRAM GPU prewarm still uses the per-mip path, so it stays on un-chunked textures for now.

End to end for the same asset (decode, 13 mip levels, BC1, `.dstex` write), level 6 costs +11% import time for a 4.7% smaller file, and level 12 costs 6.1x for 8.4%. Levels 9-12 are the libdeflate cliff: avoid them for interactive or CI reimports. The practical range is 1-6, and low-entropy ORM/mask textures gain even less (2.1% at level 12 on a 2048x2048 ORM). Keep the default when reimport time matters, and raise it only when distribution size is the higher priority.

During export, imported `.dstex` files are bundled into `res://textures.dspkg` (version 3 or version 4 with encrypted TOC) along with per-entry GPU mip metadata and entry flags. The runtime resource loader resolves texture paths directly from that archive without requiring individual `.dstex` files in the exported PCK.

### Native library reload (GDExtension)

The native half (`bin/dsgd_64*.dll`) is a GDExtension library, so it can be reloaded without restarting the editor. In Godot 4.7 open **Project Settings > GDExtension** and press the reload button on the `directstorage_accelerator.gdextension` row. The editor also reloads it automatically when you return to the window after rebuilding, and only then - saving the file is not enough.

A reload unloads and reopens the library, so the addon's native singletons (`DSContext`, `DSErrorHandler`, `DSProfiler`, `DSTextureCache`) are rebuilt from scratch; the editor plugin re-initializes the DirectStorage backend as soon as that happens.

- On Windows the editor runs the library from a hidden copy beside it (`bin/~dsgd_64.debug.dll`), which is why rebuilding over a running editor works. The DirectStorage runtime DLLs (`dstorage.dll`, `dstoragecore.dll`) are not copied, so switching runtime versions still needs the staged restart described under **Runtime Install & License**.
- A failed reload (half-written build, missing dependency) leaves the extension unloaded until the next successful reload or an editor restart. The button ignores the result, so check the Output panel. An extension that never loaded at editor start is not listed in that tab at all.
- A reload cannot re-initialize an extension whose minimum initialization level is below the scene level; this addon initializes at scene level, so it reloads cleanly.
- Two copies of the library in the same project cannot both own the DirectStorage classes. The plugin warns at startup with both paths when it detects that, and with the expected manifest when the native half is missing altogether.

## Runtime Install & License

The DirectStorage runtime (`dstorage.dll`, `dstoragecore.dll`) is not bundled with this addon. Enable the plugin, then open **Tools > DSGD Tools... > Manage DirectStorage Runtime...** to download a pinned official Microsoft package, review its license, and choose the active version.

- The license shown is the exact `LICENSE.txt` from the verified package (SHA-512 checked against Microsoft's published hash). Nothing is installed until you accept it.
- Installation is editor-only and staged: the new DLL pair is applied on the next editor or game start. Windows locks loaded modules, and one process cannot mix two DirectStorage versions.
- Only the 1.3 stable runtime is offered by default. Runtimes newer than 1.3 (including the 1.4 preview) stay hidden until you tick **Show preview runtimes** in the manager; the choice is saved in `dsgd_extension/show_preview_runtimes`. A version that is already installed, staged or downloaded is always listed, so turning the option off never makes the active runtime unselectable. Those are development runtimes: the preview license grants no redistribution rights, so do not commit or ship their DLLs.
- Runtime DLLs must be installed before exporting; the export dependencies reference them.

The runtime is Microsoft-licensed and is not bundled or re-licensed by this addon. When you ship `dstorage.dll` / `dstoragecore.dll` with a game, you are responsible for complying with Microsoft's EULA and for including its `LICENSE.txt` and `NOTICES.txt`. The installer extracts both into `bin/.versions/<version>/`, and the export option `dsgd_extension/include_directstorage_license_file` can copy them next to the exported binaries.

## Direct GPU Decompression & Fallback

When running on Windows with a Direct3D 12 `RenderingDevice` (`--rendering-driver d3d12`), DirectStorage decompresses GDeflate mip streams directly into GPU VRAM as `Texture2DRD` resources via compute shaders. If GPU decompression is disabled, unsupported, or if legacy version 1 assets are loaded, the system automatically falls back to multi-threaded CPU software decompression (`ImageTexture`).

*Custom Godot Engine builds:* Direct GPU decompression requires Godot compiled with the D3D12 driver enabled (`d3d12=yes`). If you build Godot from source or strip modules for size, see the custom engine build guide (`docs/custom-godot-builds.md`) for required SCons flags and module guardrails.

The file I/O mode is chosen from the volume that holds the assets. NTFS volumes on NVMe, SATA, or ATA buses use DirectStorage's default unbuffered path (BypassIO when the OS and drivers allow it). Rotational volumes automatically switch to buffered file I/O with `ForceFileBuffering` and BypassIO disabled, so the Windows cache and read-ahead are used; this is the supported configuration for hard drives. The choice is process-wide and is taken from the volume passed to `initialize_ds_for_path()`, or from `res://` for default startup. Initialize DirectStorage before querying capabilities; a capability probe that runs first fixes the I/O mode from `res://`.

### Frame cost on low-end GPUs (measured)

Compression buys disk bandwidth and spends GPU time. On a GTX 1050 Ti at 1920x1080, streaming 32 uncompressed 1024x1024 textures added about 0.02 ms to the median frame, while the same textures as GDeflate added about 0.36 ms at idle and about 1.06 ms while a synthetic GPU load was running; the 95th-percentile frame was 12-15 ms worse during the compressed stream. Throughput moves the other way: the compressed set delivered 2.06 GB/s of logical bytes versus 1.70 GB/s uncompressed at idle, and the two converged (about 1.4-1.5 GB/s) under load.

If you are frame-bound on a low-end or integrated GPU rather than bandwidth-bound, set `dsgd_extension/disable_gpu_decompression` so decoding happens on the CPU, and budget for the tail. The writer's uncompressed fallback only covers textures GDeflate cannot shrink, so it is not a general substitute for compressible art. Reproduce with `project/benchmark/bench_gpu_contention.gd`; these numbers are machine-specific and only comparable within one run set.

## Runtime API

The native classes are documented in Godot's Editor Help and support Ctrl-click navigation from GDScript. The main entry points are:

- `DSContext`: initialize DirectStorage hardware queues and staging buffers, enqueue `DSRequest` objects, execute synchronous (`flush_batch`) or asynchronous (`flush_batch_async`) batches, handle completion signals (`batch_completed`, `batch_failed`), and query drive/GPU capabilities.
- `DSRequest`: configure source file paths, byte ranges, compression format (`COMPRESSION_NONE`, `COMPRESSION_GDEFLATE`), destination target (`DESTINATION_MEMORY`, `DESTINATION_GPU_BUFFER`, `DESTINATION_GPU_TEXTURE`), subresource ranges, request priorities (`PRIORITY_LOW`, `PRIORITY_NORMAL`, `PRIORITY_HIGH`), and query execution status.
- `DSTex`: create or inspect `.dstex` (v1 and v2) files and retrieve per-mip footprint metadata (`mip_offsets`, `mip_sizes`, `mip_uncompressed_sizes`, `mip_row_pitches`).
- `DSPkg`: build or read bundled `.dspkg` archives (v1, v2, v3, and v4) with per-entry GPU mip tables, flags, and optional TOC encryption.
- `GDeflateCompressor` and `GDeflateDecompressor`: encode and decode GDeflate tile-compressed payloads with configurable thread pools.
- `DSTextureCache`: inspect or manage the runtime texture LRU cache.
- `DSErrorHandler` and `DSProfiler`: monitor asynchronous DirectStorage kernel errors, measure physical storage throughput, and inspect logical decompression metrics.

The extension registers `DSContext`, `DSErrorHandler`, `DSProfiler`, and `DSTextureCache` as Engine singletons. Access them via `Engine.get_singleton("DSContext")` or the corresponding singleton name.

## Async Prewarm: Who Owns What

`DSPreloader` drives the streaming queue and reports its state; it deliberately does not make policy decisions for the game.

- **Residency is intentional.** `DSTextureCache` keeps prewarmed textures resident after a scene swap, so memory staying high (roughly one level's worth of textures) is the cache working, not a leak. Resident entries load again with no disk read.
- **Releasing a level's GPU resources is the game's job.** Drop the references the scene held, then call `DSTextureCache.clear_unused()`; entries that still have a live reference are kept. Judge the result by whether you are still hitting your session's own best throughput, not by a fixed millisecond budget — the same byte count is cheap with memory headroom and slow when the system is paging.
- **The addon will not warn you about scene teardown.** A slow quit is the engine freeing the scene graph plus the driver releasing memory; `DSPreloader` does not watch for it and emits nothing. Do not wait for a warning that will never come.
- **Eviction granularity is per call, not per microsecond.** There is no `clear_unused_budget()`, and none is planned: the call itself is not where the time goes. To spread the cost, release references in stages and call `clear_unused()` after each stage.

Diagnostics, so a wedged load is one call instead of three pollers:

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

## Project Settings

The addon exposes configurable project settings under the `dsgd_extension/` namespace in Godot's Project Settings (`Project -> Project Settings... -> General -> Dsgd Extension`):

| Setting | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `dsgd_extension/staging_buffer_size_mb` | `int` | `128` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (clamped range: `1`–`2048` MiB). Increase this if loading very large textures or high-resolution mip streams whose compressed payloads exceed the buffer. |
| `dsgd_extension/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `dsgd_extension/disable_gpu_decompression_metacommand` | `bool` | `false` | When `true`, keeps GPU decompression enabled but forces the reference implementation instead of the driver's optimized metacommand. Independent of `disable_gpu_decompression`, so the two GPU paths can be compared on the same machine; use it when the optimized path misbehaves or measures slower. Applies to the next DirectStorage initialization. |
| `dsgd_extension/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (clamped range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `dsgd_extension/auto_tune_for_integrated_gpu` | `bool` | `true` | When `true`, automatically shrinks the staging buffer and queue (to 64 MiB / 256) on PCs whose GPU shares system memory (see below). Only ever lowers values, never raises them. Set to `false` to use your configured values exactly as written. |
| `dsgd_extension/auto_tune_for_low_vram_gpu` | `bool` | `true` | When `true`, caps the effective staging buffer on discrete GPUs by dedicated VRAM (32 MiB under 2 GiB, 64 MiB under 8 GiB; 8 GiB and above are untouched). Only ever lowers values, never raises them. Set to `false` to use your configured value exactly. |
| `dsgd_extension/texture_cache_enabled` | `bool` | `true` | LRU residency cache (`DSTextureCache`). `true`: `REUSE` loads hit the cache and become engine-resident, so post-prewarm scene loads bind pointer-identical textures with no disk read, and prewarm works. `false`: every load re-reads and re-decompresses and the texture dies with its last reference — GPU decompression still works, but async prewarm refuses to start and existing entries are cleared. Prefer lowering `texture_cache_max_entries` over disabling. |
| `dsgd_extension/texture_cache_max_entries` | `int` | `128` | Maximum number of texture entries held in the LRU cache (range: `16`–`2048`). When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. |
| `dsgd_extension/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |
| `dsgd_extension/sync_flush_timeout_ms` | `int` | `3000` | Maximum timeout in milliseconds (clamped range: `500`–`10000` ms) for synchronous batch flushes (`submit_batch` / `flush_batch`). Dictates the timeout budget for hardware queue execution and fence completion, with up to half (`sync_timeout_ms / 2`) allocated for draining prior in-flight batches beforehand. Synchronous flushes should never be called inside gameplay process loops (`_process`). |
| `dsgd_extension/show_preview_runtimes` | `bool` | `false` | Editor-tool preference, not a runtime setting: when `true`, the Manage DirectStorage Runtime dialog lists runtime versions newer than 1.3 (for example the 1.4 preview). The dialog's **Show preview runtimes** checkbox is the normal way to set it. Versions already installed, staged or downloaded are listed regardless of this value. |

### Notes on Queue Capacity and Timeouts

- **Queue Capacity & Streaming Bursts**: `dsgd_extension/queue_capacity` defines the hardware queue slot capacity (default `512`, clamped range: `128`–`8192`). Textures with full mip chains expand into multiple subresource requests (e.g., 10–12 mips per texture). The streaming engine (`DSContext::flush_stream_async`) automatically budgets burst popping based on subresource counts and applies transient backpressure requeuing to prevent queue exhaustion during bulk scene prewarming.
- **Synchronous Timeout Budget**: During synchronous fallback loads (e.g. scene loading uncached textures via `ResourceLoader`), `dsgd_extension/sync_flush_timeout_ms` ensures adequate headroom for NVMe DMA transfers, GDeflate hardware decompression, and GPU barrier completion under high engine contention.

### Integrated GPUs and Auto-Tune (What UMA Means)

Most gaming desktops have a **discrete GPU**: a separate graphics card with its own private memory (dedicated VRAM, usually several GB). Laptops, handhelds, and mini-PCs often have an **integrated GPU** (Intel UHD/Iris, AMD "Radeon Graphics", Apple Silicon) with no private memory. These use a **unified memory architecture (UMA)**: the GPU borrows ordinary system RAM, typically just a few hundred MB carved out plus whatever it can share.

Why that matters for DirectStorage settings:

- The **staging buffer** (`128` MiB by default) is scratch memory DirectStorage uses while moving data. On a discrete GPU it lives in VRAM where there is plenty of room. On an integrated GPU it comes out of the same shared RAM the game, the OS, and the prewarmed textures all compete for, so a big staging buffer adds memory pressure for no speed gain.
- **GPU decompression still works** on integrated GPUs (the log will say `enabled (GPU-optimized/fallback)`), but "direct to VRAM" really means "direct to a shared-memory allocation" — there is no separate VRAM to be direct *to*.
- **Prewarm auto-routing**: `prewarm_gpu_async()` calls automatically route to System RAM prewarm (`prewarm_async()`) on integrated GPUs when auto-tune is enabled. This prevents 3D/Copy-queue render contention and hard freezes during animated loading screens while preserving direct-to-VRAM DMA on discrete graphics cards.

`dsgd_extension/auto_tune_for_integrated_gpu` (default `true`) handles this automatically: when the extension detects an integrated/UMA adapter (queried directly via Direct3D 12 hardware UMA architecture, or under 1 GiB of dedicated VRAM reported by Windows), it quietly lowers the *effective* staging buffer to 64 MiB and the queue to 256 at startup, routes GPU prewarms to System RAM, and says so once in the log (`auto-tune clamped staging 128->64 MiB, queue 512->256`). It only ever lowers, never raises - if you already set smaller values, they pass through untouched, and your saved settings are never rewritten. Discrete-GPU PCs are completely unaffected. Turn it off (`false`) if you want full manual control, e.g. when benchmarking exact configured values.

`dsgd_extension/auto_tune_for_low_vram_gpu` (default `true`) is a separate policy for discrete GPUs. The DirectStorage factory can hold up to four staging buffers at the configured size, so the 128 MiB default is up to ~512 MiB of residency; this setting caps the *effective* staging by dedicated VRAM - 64 MiB at 2-8 GiB, 32 MiB under 2 GiB, and untouched at 8 GiB or more - and says so once in the log (`Low-VRAM GPU detected (...): auto-tune clamped staging 128->64 MiB`). It never changes the queue, never raises a value, and leaves a lower configured setting alone. Set it to `false` to keep your configured size on every GPU.

## Export Preset Options

The exporter adds six options to every Windows Desktop preset, on the **Options** tab (`Project -> Export... -> Windows Desktop -> Options`):

| Export Option | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `dsgd_extension/bundle_textures_dspkg` | `bool` | `true` | Bundles exported textures into `textures.dspkg` for unbuffered DirectStorage DMA loading. |
| `dsgd_extension/enforce_archive_pairing` | `bool` | `true` | Publishes a random 64-bit archive id per export and compares it, in full, when the runtime opens the archive. Set to `false` only for local tooling: it publishes `0`, which skips the check in that build, and writes an archive carrying id `0` that any build expecting a real id rejects. |
| `dsgd_extension/steampipe_1mb_alignment` | `bool` | `false` | Aligns texture payloads to 1 MiB chunks for SteamPipe differential patching efficiency. |
| `dsgd_extension/include_dsgd_license_file` | `bool` | `true` | Writes `dsgd_LICENSE.txt` next to the exported binaries, containing the project license and the third-party components of `dsgd_64.dll`. Disable when you carry those attributions in-game instead; the source texts stay in the addon folder. |
| `dsgd_extension/include_directstorage_license_file` | `bool` | `true` | Writes Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt` for the active runtime version as `DIRECTSTORAGE_LICENSE.txt`. |
| `dsgd_extension/merge_license_files` | `bool` | `true` | When both license files are included, appends the DirectStorage runtime texts to `dsgd_LICENSE.txt` and skips the separate runtime file. No effect when fewer than two files are included. |

Encryption is not one of those options. It comes entirely from the same preset's **Encryption** tab (`Project -> Export... -> Windows Desktop -> Encryption`), next to Options. The add-on keeps no encryption toggle and no key field of its own, and caches nothing from the preset, because one plugin instance serves every preset of an "Export All" run.

### What the Encryption tab contributes

The exporter reads four controls from that tab, once, when the export starts:

| Encryption tab control | Preset getter | Role |
| :--- | :--- | :--- |
| **Encrypt Exported PCK** | `get_encrypt_pck()` | Master switch. Off means a plaintext archive, whatever the key and filters say. |
| **Encryption Key (256-bits as hexadecimal):** | `get_encryption_key()` | Tested for emptiness only. The archive key is never derived from it. |
| **Filters to include files/folders** | `get_encryption_in_filter()` | Must match `res://.dspkg/toc.key`. |
| **Filters to exclude files/folders** | `get_encryption_ex_filter()` | Must not match it. Exclude wins over include. |

Both filter lists are comma-separated globs, matched against the full `res://` path and against the path without the prefix, so `res://.dspkg/toc.key` and `.dspkg/*` both work. The key itself lives in `.godot/export_credentials.cfg` (`script_encryption_key`), not in `export_presets.cfg`, so it does not travel with the repository.

### Outcomes

Those inputs, together with the bundling toggle, resolve to one outcome per export:

| Preset state | Outcome | What ships | At startup |
| :--- | :---: | :--- | :--- |
| bundling off (`bundle_textures_dspkg = false`) | `disabled` | No archive. Every `.dstex` stays in the PCK. | Normal CPU decompression. |
| bundling on, **Encrypt Exported PCK** off | `plaintext` | Archive written without a key; no key entry is injected. The TOC is readable by anyone. | Archive loads, no protection. |
| bundling on, **Encrypt Exported PCK** on, key set, key path encrypted by the filters | `protected` | Archive written with a random 32-byte key for this export, then injected as a 48-byte PCK entry (16-byte derived magic + 32-byte key). | Archive loads after the key check. |
| bundling on, **Encrypt Exported PCK** on, key empty or key path not encrypted | `failed` | No archive, no key entry, and every `.dstex` removed from the PCK. | Player fails with `DSPKG_BOOT status=fail reason=missing_archive`. |

`failed` is the outcome that tends to surprise. The export itself still completes and reports through a `push_error` line in the export log rather than an error dialog, and the texture payloads have already been removed from the PCK, so the game hard-fails at startup instead of quietly falling back to CPU decompression. A project with no `.dstex` has nothing to remove and still boots on CPU. Fix the preset and re-export.

The filters are mandatory because Godot decides encryption per file from the include and exclude globs, so **Encrypt Exported PCK** on its own encrypts nothing. An empty include filter would store the injected key in the clear, which is why that configuration is rejected as `failed` instead of shipped.

### Engine controls that do not affect the archive

Two more controls sit on that tab and are never read by the exporter:

- **Encrypt Index (File Names and Info)** changes how the PCK index is stored, not how the archive is built.
- **Initialization vector seed** only makes encrypted files byte-stable across exports: the AES-256-CFB IV is derived from the file content plus the seed instead of being random. It cannot make a DirectStorage build reproducible, because the archive key and the archive id are regenerated on every export.

### Encryption architecture and key provisioning

- **Prerequisite**: the Godot export templates must be built with the same PCK key. Stock Godot cannot mount a custom-key pack, so plan the template build with your PCK key before enabling encryption.
- **TOC encryption and authentication**: the archive's table of contents (TOC: entry names, dimensions, offsets) is encrypted and authenticated with AEAD (BLAKE2b key-id derivation, XChaCha20, Poly1305 MAC via Monocypher).
- **Payload blocks stay unencrypted** so DirectStorage can hand the compressed GDeflate streams straight to the GPU decompressor. The payload bytes are therefore not secret: they are recoverable from the archive without any key.
- **Key delivery**: the archive key is random per export, is not compiled into published binaries, and reaches the game only as the `res://.dspkg/toc.key` entry inside the encrypted PCK. `DSPkg.open()` reads that entry, checks the 16-byte magic, and installs the key only after it reproduces the archive header's key id and decrypts the TOC. Shipped games do not need to call `DSPkg.set_global_encryption_key()`.
- **Verification**: a build counts as protected only after the exported player prints one `DSPKG_BOOT status=ok mode=protected … key_id=<16 hex>` line and the add-on's pack scanner (`tools/dspkg_pack_scan.gd`, also available as **Project > Tools > Verify exported pack (DSPkg)**) reports `DSPKG_SCAN status=ok` on the shipped pack.
  - Boot check: `<export-dir>\game.exe --headless -- --dspkg-validate=protected`. A normal successful boot prints nothing; the switch is what turns the line on. A failed boot prints `DSPKG_BOOT status=fail reason=<code>` every time, and `key_id=none` means no key was committed.
  - Reference: [`docs/verifying-an-exported-build.md`](../../../docs/verifying-an-exported-build.md) covers the fields, the five failure reasons and the CI shape. That file lives in this repository next to the add-on; a project that only copied `addons/` does not have it, and the editor scan above needs nothing else.

### Legacy: baking the key into the DLL

A source build can compile the archive key into the extension binary (`DSPKG_ENCRYPTION_KEY_FILE`). This is not the delivery path described above and is not used by published builds; it exists for experiments and for source builds that have no encrypted PCK to carry a key.

- **Weaker**: the key ships inside the binary, so anyone holding the DLL holds the key, and every copy of the game shares one archive key. A PCK-delivered key is unique per export.
- **Precedence**: a valid `res://.dspkg/toc.key` wins; the baked key is used only when no readable key entry exists.
- **Distribution**: published binaries carry no key in the DLL (`dspkg-key-id=none`); the build audit treats a binary that carries a key, or one with no tag, as a violation for distributed targets.

## Trademarks and Attribution

This addon/extension (DSGD) is an independent project and is not affiliated with, endorsed by, or sponsored by Microsoft. Microsoft and DirectStorage are trademarks of the Microsoft group of companies. The DirectStorage runtime is installed under Microsoft's license and is not bundled with this addon/extension; see Runtime Install & License.

Third-party license texts and attributions are in `THIRD_PARTY_NOTICES.txt`; the project's own license is in `LICENSE`. Both ship with the addon and are copied next to exported binaries as `dsgd_LICENSE.txt` (with the DirectStorage runtime texts appended when merging is enabled) or as a separate `DIRECTSTORAGE_LICENSE.txt`. The exporter removes and rewrites those files on every export, so the directory always matches the preset.



