# DirectStorage Accelerator

This addon exposes DirectStorage-backed texture streaming and asynchronous I/O request APIs to Godot 4.

## Editor Workflow

Enable the `DirectStorage Accelerator` plugin. It registers the `DSRuntime` autoload, the `DirectStorage Texture` importer, and the `DirectStorageExporter` export plugin.

The autoload entry is written to `project.godot` when the plugin is enabled and removed only when you disable the plugin again — commit it with the rest of your project settings. Closing the editor keeps it, because standalone and exported builds need that node to drive the asynchronous prewarm queue.

The importer converts `.png`, `.jpg`, `.jpeg`, and `.webp` sources into `.dstex` (version 2) resources with D3D12-aligned footprints (256-byte row pitch alignment and 512-byte mip placement alignment) for direct hardware decompression. Its Import dock options control source compression mode (S3TC, BPTC/BC7, or uncompressed), source color space, mipmap generation, a source size limit, alpha-border/premultiply processing, and GDeflate compression level.

Three presets are provided: **3D (Streaming)** (the default), **CPU / ImageTexture (2D, UI, Decals)** (uncompressed, no mipmaps, CPU-loaded), and **Project Default**, which follows `directstorage/import/default_profile`. Existing `.import` files keep their stored values on reimport; the project setting only affects new imports. A pure 2D project should normally use Godot's own Texture2D importer instead of this addon.

During export, imported `.dstex` files are bundled into `res://textures.dspkg` (version 3 or version 4 with encrypted TOC) along with per-entry GPU mip metadata and entry flags. The runtime resource loader resolves texture paths directly from that archive without requiring individual `.dstex` files in the exported PCK.

## Runtime Install & License

The DirectStorage runtime (`dstorage.dll`, `dstoragecore.dll`) is not bundled with this addon. Enable the plugin, then open **Tools > DSGD Tools... > Manage DirectStorage Runtime...** to download a pinned official Microsoft package, review its license, and choose the active version.

- The license shown is the exact `LICENSE.txt` from the verified package (SHA-512 checked against Microsoft's published hash). Nothing is installed until you accept it.
- Installation is editor-only and staged: the new DLL pair is applied on the next editor or game start. Windows locks loaded modules, and one process cannot mix two DirectStorage versions.
- Pick the 1.4 preview to test its newer capabilities, then switch back to the 1.3 stable runtime if the preview regresses or an asset is incompatible. Restart after switching.
- Runtime DLLs must be installed before exporting; the export dependencies reference them.

## Direct GPU Decompression & Fallback

When running on Windows with a Direct3D 12 `RenderingDevice` (`--rendering-driver d3d12`), DirectStorage decompresses GDeflate mip streams directly into GPU VRAM as `Texture2DRD` resources via compute shaders. If GPU decompression is disabled, unsupported, or if legacy version 1 assets are loaded, the system automatically falls back to multi-threaded CPU software decompression (`ImageTexture`).

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

The addon exposes configurable project settings under the `directstorage/` namespace in Godot's Project Settings (`Project -> Project Settings... -> General -> DirectStorage`):

| Setting | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/staging_buffer_size_mb` | `int` | `128` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (clamped range: `1`–`2048` MiB). Increase this if loading very large textures or high-resolution mip streams whose compressed payloads exceed the buffer. |
| `directstorage/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `directstorage/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (clamped range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `directstorage/auto_tune_for_integrated_gpu` | `bool` | `true` | When `true`, automatically shrinks the staging buffer and queue (to 64 MiB / 256) on PCs whose GPU shares system memory (see below). Only ever lowers values, never raises them. Set to `false` to use your configured values exactly as written. |
| `directstorage/texture_cache_enabled` | `bool` | `true` | LRU residency cache (`DSTextureCache`). `true`: `REUSE` loads hit the cache and become engine-resident, so post-prewarm scene loads bind pointer-identical textures with no disk read, and prewarm works. `false`: every load re-reads and re-decompresses and the texture dies with its last reference — GPU decompression still works, but async prewarm refuses to start and existing entries are cleared. Prefer lowering `texture_cache_max_entries` over disabling. |
| `directstorage/texture_cache_max_entries` | `int` | `128` | Maximum number of texture entries held in the LRU cache (range: `16`–`2048`). When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. |
| `directstorage/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |
| `directstorage/sync_flush_timeout_ms` | `int` | `3000` | Maximum timeout in milliseconds (clamped range: `500`–`10000` ms) for synchronous batch flushes (`submit_batch` / `flush_batch`). Dictates the timeout budget for hardware queue execution and fence completion, with up to half (`sync_timeout_ms / 2`) allocated for draining prior in-flight batches beforehand. Synchronous flushes should never be called inside gameplay process loops (`_process`). |

### Notes on Queue Capacity and Timeouts

- **Queue Capacity & Streaming Bursts**: `directstorage/queue_capacity` defines the hardware queue slot capacity (default `512`, clamped range: `128`–`8192`). Textures with full mip chains expand into multiple subresource requests (e.g., 10–12 mips per texture). The streaming engine (`DSContext::flush_stream_async`) automatically budgets burst popping based on subresource counts and applies transient backpressure requeuing to prevent queue exhaustion during bulk scene prewarming.
- **Synchronous Timeout Budget**: During synchronous fallback loads (e.g. scene loading uncached textures via `ResourceLoader`), `directstorage/sync_flush_timeout_ms` ensures adequate headroom for NVMe DMA transfers, GDeflate hardware decompression, and GPU barrier completion under high engine contention.

### Integrated GPUs and Auto-Tune (What UMA Means)

Most gaming desktops have a **discrete GPU**: a separate graphics card with its own private memory (dedicated VRAM, usually several GB). Laptops, handhelds, and mini-PCs often have an **integrated GPU** (Intel UHD/Iris, AMD "Radeon Graphics", Apple Silicon) with no private memory. These use a **unified memory architecture (UMA)**: the GPU borrows ordinary system RAM, typically just a few hundred MB carved out plus whatever it can share.

Why that matters for DirectStorage settings:

- The **staging buffer** (`128` MiB by default) is scratch memory DirectStorage uses while moving data. On a discrete GPU it lives in VRAM where there is plenty of room. On an integrated GPU it comes out of the same shared RAM the game, the OS, and the prewarmed textures all compete for, so a big staging buffer adds memory pressure for no speed gain.
- **GPU decompression still works** on integrated GPUs (the log will say `enabled (GPU-optimized/fallback)`), but "direct to VRAM" really means "direct to a shared-memory allocation" — there is no separate VRAM to be direct *to*.
- **Prewarm auto-routing**: `prewarm_gpu_async()` calls automatically route to System RAM prewarm (`prewarm_async()`) on integrated GPUs when auto-tune is enabled. This prevents 3D/Copy-queue render contention and hard freezes during animated loading screens while preserving direct-to-VRAM DMA on discrete graphics cards.

`directstorage/auto_tune_for_integrated_gpu` (default `true`) handles this automatically: when the extension detects an integrated/UMA adapter (queried directly via Direct3D 12 hardware UMA architecture, or under 1 GiB of dedicated VRAM reported by Windows), it quietly lowers the *effective* staging buffer to 64 MiB and the queue to 256 at startup, routes GPU prewarms to System RAM, and says so once in the log (`auto-tune clamped staging 128->64 MiB, queue 512->256`). It only ever lowers, never raises — if you already set smaller values, they pass through untouched, and your saved settings are never rewritten. Discrete-GPU PCs are completely unaffected. Turn it off (`false`) if you want full manual control, e.g. when benchmarking exact configured values.

## Export Preset Options

The exporter adds three options to every Windows Desktop preset, on the **Options** tab (`Project -> Export... -> Windows Desktop -> Options`):

| Export Option | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/bundle_textures_dspkg` | `bool` | `true` | Bundles exported textures into `textures.dspkg` for unbuffered DirectStorage DMA loading. |
| `directstorage/enforce_archive_pairing` | `bool` | `true` | Publishes a random 64-bit archive id per export and compares it, in full, when the runtime opens the archive. Set to `false` only for local tooling: it publishes `0`, which skips the check in that build, and writes an archive carrying id `0` that any build expecting a real id rejects. |
| `directstorage/steampipe_1mb_alignment` | `bool` | `false` | Aligns texture payloads to 1 MiB chunks for SteamPipe differential patching efficiency. |

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



