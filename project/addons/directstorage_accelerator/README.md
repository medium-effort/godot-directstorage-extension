# dsgd - Unofficial DirectStorage binding

This addon exposes DirectStorage-backed texture streaming and asynchronous I/O request APIs to Godot 4.

This document covers the **Installation Quick Start**, **DSTexture Import Settings**, **Project Settings**, and **Export Preset Options** that Godot's editor UI does not display in hover tooltips for addons, along with **Trademarks and Attribution**. For editor workflow, runtime installation, GPU decompression details, runtime API, and building from source, see the [repository README](https://github.com/medium-effort/godot-directstorage-extension/blob/main/README.md).

## Contents

- [Installation & Quick Start](#installation--quick-start)
- [DSTexture Import Settings](#dstexture-import-settings)
- [Project Settings](#project-settings)
  - [Notes on Queue Capacity and Timeouts](#notes-on-queue-capacity-and-timeouts)
  - [Integrated GPUs and Auto-Tune (What UMA Means)](#integrated-gpus-and-auto-tune-what-uma-means)
- [Export Preset Options](#export-preset-options)
  - [What the Encryption tab contributes](#what-the-encryption-tab-contributes)
  - [Outcomes](#outcomes)
  - [Engine controls that do not affect the archive](#engine-controls-that-do-not-affect-the-archive)
- [Trademarks and Attribution](#trademarks-and-attribution)
  - [License](#license)
  - [Trademarks and Notices](#trademarks-and-notices)

## Installation & Quick Start

1. **Switch to D3D12 (Windows)**: If your project is not already using Direct3D 12, set **Project Settings > General > Rendering > Rendering Device > Driver.windows** (`rendering/rendering_device/driver.windows`) to `d3d12` and restart the editor.
2. **Install the Addon**: Place `directstorage_accelerator/` in your project's `addons/` directory and enable **dsgd - Unofficial DirectStorage binding** in **Project Settings > Plugins** (this registers the `DSRuntime` autoload).
3. **Install DirectStorage Runtime (Windows)**: Open **Project > Tools > dsgd Tools... (DirectStorage) > Manage DirectStorage Runtime...** to download, review Microsoft's license, and install `dstorage.dll` and `dstoragecore.dll` (the dialog opens automatically on Windows when you enable the plugin if `dstoragecore.dll` is not installed yet). The plugin also checks `.gdextension` manifest health on startup and warns in the Output panel if the native library is missing, duplicated, or loaded from an unexpected path. The runtime DLLs are not bundled in this repository and must be installed before exporting on Windows.
4. **Import & Export**: Newly imported textures use the `.dstex` importer (`3D (Streaming)` preset by default) and load transparently through `ResourceLoader.load()`. On export, the `dsgd` export plugin (`dsgd Extension`) packs them into `textures.dspkg` automatically.


## DSTexture Import Settings

The **DirectStorage Texture** importer converts `.png`, `.jpg`, `.jpeg`, `.webp`, `.tga`, and `.dds` images into `.dstex` compressed texture resources for DirectStorage-accelerated streaming.

Three presets are provided in the Import dock:

- **3D (Streaming)** - the product default (VRAM compressed, mipmaps enabled, GDeflate level 3).
- **CPU / ImageTexture (2D, UI, Decals)** - uncompressed, no mipmaps, `fix_alpha_border` enabled, CPU-loaded. A pure 2D project should normally use Godot's built-in `Texture2D` importer instead.
- **Project Default** (shown in the Import dock as `Project Default (3D)` or `Project Default (2D)`) - follows `dsgd_extension/default_import_profile` in Project Settings. Existing `.import` files keep their stored values on reimport; changing the project setting only affects new imports.

- **`compression/disk_compression`** (`int`, default: `1`)

  Selects the on-disk / DirectStorage stream compression codec applied to the GPU-ready payload in `.dstex`: **None (`0`)** stores the raw 256-byte-pitch-aligned footprint verbatim (`DSCompressionFormat::NONE`, v3 header) with zero runtime decompression cost; **GDeflate (`1`)** compresses each mip or chunk with Microsoft GDeflate for smaller disk footprints and hardware-accelerated GPU decompression on DirectStorage-capable Windows systems.

- **`compression/gdeflate_level`** (`int`, default: `3`)

  Microsoft GDeflate compression level (`1`–`12`). Defaults to `3`, which is close to free in encode time and captures a meaningful share of the reduction (~1–2% on block-compressed payloads, ~6–27% on uncompressed payloads). Higher values shrink files at the cost of slower import times, steeply so from `9` upward. Runtime decompression cost is per-byte identical at every level.

- **`compression/chunk_size_kb`** (`int`, default: `8192`)

  Splits each mip into independent GDeflate chunks of at most this many uncompressed KiB (`0` keeps one stream per mip). Chunks bound the largest DirectStorage request so large textures do not force the staging buffer to hold a whole mip. Block-compressed formats keep every chunk boundary on a 4-row block boundary.

- **`compression/color_space`** (`int`, default: `1`)

  Selects the color space stored in the `.dstex` header: **Linear (`0`)** for normal maps, roughness, metallic, masks, or lookup tables (creates a linear UNORM GPU format); **sRGB (`1`)** for standard color textures such as albedo/diffuse maps (creates the sRGB counterpart GPU format so hardware sampling decodes to linear). Independent from channel packing.

- **`compress/normal_map`** (`int`, default: `0`)

  Normal-map processing: **Disabled (`0`)** treats the image as ordinary color/data; **Enabled (`1`)** normalizes the base level, renormalizes mip levels, and prioritizes red/green channels during block compression. Produces BC7 when `compression/high_quality` is enabled, or BC5/BC3 when disabled. Usage-based auto-detection is not available to a custom importer, so enable this explicitly for normal maps.

- **`compress/channel_pack`** (`int`, default: `0`)

  Channel packing during block compression (applies only when `compression/mode` is **VRAM Compressed**): **sRGB Friendly (`0`)** detects used channels as sRGB color, avoiding packing that would shift values when sampled as sRGB; **Optimized (`1`)** detects used channels as generic data for more aggressive packing (smaller files, not safe for sRGB-sampled color textures).

- **`compression/high_quality`** (`bool`, default: `false`)

  When `compression/mode` is **VRAM Compressed**, uses BPTC/BC7 instead of S3TC. Preserves alpha and normal-map detail better, but takes twice the VRAM of BC1 and encodes on the CPU during import. With BPTC, `compress/normal_map` produces BC7 rather than BC5; disable this option if BC5 normal maps are preferred.

- **`compression/mode`** (`int`, default: `0`)

  **VRAM Compressed (`0`)**: compresses mipmaps using S3TC (BC1/BC3/BC4/BC5) or, with `compression/high_quality`, BPTC (BC7) prior to GDeflate compression. Reduces VRAM consumption and optimizes streaming bandwidth (recommended for 3D textures). **VRAM Uncompressed (`1`)**: retains raw pixel data (e.g. RGBA8) with mipmaps, compressed only on disk with lossless GDeflate. Preserves pixel fidelity with zero block artifacts (pixel art, crisp UI, LUTs) at higher runtime memory cost.

- **`mipmaps/generate`** (`bool`, default: `true`)

  Generates a full mip chain when `true`. Disable for 2D/UI textures always sampled at 1:1 to write a single-mip `.dstex` and save roughly one third of payload and memory.

- **`process/size_limit`** (`int`, default: `0`)

  If `> 0`, resizes the image so its longer edge is at most this many pixels before mip generation and compression. `0` applies only the 16384-pixel container cap. Useful for mobile targets (`4096`/`8192`) and avoiding oversized staging buffers.

- **`process/channel_remap/red`** (`int`, default: `0`)

  Source for the red output channel: Red, Green, Blue, Alpha, Inverted Red/Green/Blue/Alpha, Unused, Zero, or One. Use with the green/blue/alpha siblings to pack PBR masks or flip a normal map's green channel. Applied before normalization and mip generation via `DSTex.remap_channels`.

- **`process/channel_remap/green`** (`int`, default: `1`)

  Source for the green output channel. See `process/channel_remap/red`.

- **`process/channel_remap/blue`** (`int`, default: `2`)

  Source for the blue output channel. See `process/channel_remap/red`.

- **`process/channel_remap/alpha`** (`int`, default: `3`)

  Source for the alpha output channel. See `process/channel_remap/red`.

- **`process/fix_alpha_border`** (`bool`, default: `false`)

  Bleeds surrounding color into the transparent border of RGBA8 images before compression, reducing bilinear-filter halos. Enabled by the CPU/ImageTexture (2D) preset.

- **`process/premult_alpha`** (`bool`, default: `false`)

  Premultiplies RGB by alpha before compression. Requires materials configured for premultiplied alpha.

- **`compatibility/as_image_texture`** (`bool`, default: `false`)

  Forces the texture to load into CPU memory as an `ImageTexture` even when GPU streaming prewarm (`DSPreloader.prewarm_gpu_async`) is active. Standard `ResourceLoader` loads and CPU prewarm (`DSPreloader.prewarm_async`) already create `ImageTexture`s automatically. Enable this when using D3D12 GPU prewarm on assets that need CPU pixel access (`Light3D.light_projector`, `Decal` textures, `Texture2D.get_image()`), or when `Texture2D.get_width()` must report the exact authoring size for non-multiple-of-4 sources (the GPU path wraps the 4-pixel-aligned padded size in `Texture2DRD`). Note: uncompressed and float formats (`R8`, `RG8`, `RGB8`, `R16*`, float formats, `BC6H`) always prewarm into `ImageTexture` regardless; color formats with sRGB counterparts (`BC1`, `BC2`, `BC3`, `BC7`, `RGBA8`/`BGRA8`) and linear block formats (`BC4`, `BC5`) use the GPU path.

> **Automatic Scene Compatibility Check**: When you open a scene in the Godot editor, the plugin scans `Light3D.light_projector` and `Decal` nodes and prints a yellow Output warning naming any textures bound to `Texture2DRD` that should be re-imported with `compatibility/as_image_texture`.

## Project Settings

The addon exposes configurable project settings under the `dsgd_extension/` namespace in Godot's Project Settings (`Project -> Project Settings... -> General -> Dsgd Extension`):

- **`dsgd_extension/default_import_profile`** (`int`, default: `0`)

  Default profile used by the **Project Default** importer preset for newly imported images (`0` = 3D Streaming, `1` = CPU / ImageTexture). Existing `.import` files keep their stored values.

- **`dsgd_extension/staging_buffer_size_mb`** (`int`, default: `32`)

  Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (editor slider range: `16`–`2048` MiB in steps of `16`; runtime clamped range: `1`–`2048` MiB). Chunked textures bound a request at one chunk (8 MiB by default), so the default is sized for that; raise it only for legacy un-chunked assets whose mip exceeds the buffer.

- **`dsgd_extension/disable_gpu_decompression`** (`bool`, default: `false`)

  When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling.

- **`dsgd_extension/disable_gpu_decompression_metacommand`** (`bool`, default: `false`)

  When `true`, keeps GPU decompression enabled but forces the reference implementation instead of the driver's optimized metacommand. Independent of `disable_gpu_decompression`, so the two GPU paths can be compared on the same machine; use it when the optimized path misbehaves or measures slower. Applies to the next DirectStorage initialization.

- **`dsgd_extension/force_disable_bypass_io`** (`bool`, default: `false`)

  Disables Windows BypassIO in `DSTORAGE_CONFIGURATION` while keeping DirectStorage active (uses standard Win32 I/O and retains GPU GDeflate decompression). Applies on the next `initialize_ds()`.

- **`dsgd_extension/force_software_fallback`** (`bool`, default: `false`)

  **Advanced.** When `true`, skips `Win32Backend` and uses `SoftwareFallback` (CPU GDeflate via libdeflate, `ImageTexture` upload) even on a DirectStorage-capable Windows D3D12 host. Useful for troubleshooting buggy storage drivers, reproducing fallback behavior in exported builds, and benchmarking both CPU GDeflate paths under the same rendering driver. Discards all DirectStorage acceleration. Applies on the next `initialize_ds()`; when present, this project setting overrides `DSContext.set_force_software_fallback()`.

- **`dsgd_extension/allow_directstorage_without_bypassio`** (`bool`, default: `true`)

  **Advanced.** When `true`, `Win32Backend` initializes on any local Windows volume (USB/SD/exFAT/RAID/virtual disks), disabling BypassIO where unavailable and using buffered I/O only on rotational media. This keeps D3D12 GPU GDeflate instead of forcing CPU decompression on those volumes. Set `false` to restore the older NVMe/SATA/ATA + NTFS restriction (useful as a rollback or for A/B testing). UNC network shares are always excluded.

- **`dsgd_extension/max_cpu_threads`** (`int`, default: `0`)

  Maximum CPU threads used by DirectStorage work. `0` (default) means auto: logical processor count capped at 16, and never above Godot's `threading/worker_pool/max_threads` when set. An explicit value (`1`–`32`) caps DirectStorage's built-in CPU decompression pool, the fallback backend's workers, GDeflate tile decompression, and preloader CPU offload batches. Replaces `cpu_decompression_threads`; an old value in `project.godot` is migrated automatically. Changing it requires a restart, because the budget and the DirectStorage configuration are resolved once at initialization.

- **`dsgd_extension/debug_break_on_error`** (`bool`, default: `false`)

  Adds `DSTORAGE_DEBUG_BREAK_ON_ERROR` in debug/dev builds only, so a bad read breaks into the debugger at the failing enqueue instead of being attributed to the following batch boundary. Requires a debugger attached: `DebugBreak()` without one terminates the process. Has no effect in release builds. Applies to the next initialization.

- **`dsgd_extension/queue_capacity`** (`int`, default: `512`)

  Maximum request capacity allocated for the hardware DirectStorage queue (clamped range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests.

- **`dsgd_extension/auto_tune_for_integrated_gpu`** (`bool`, default: `true`)

  When `true`, automatically lowers the effective staging buffer (64 MiB), queue (256), and `texture_cache_max_entries` (128) on PCs whose GPU shares system memory (see below), and routes GPU prewarms to System RAM. Only ever lowers values, never raises them. Set to `false` to use your configured values exactly as written.

- **`dsgd_extension/auto_tune_for_low_vram_gpu`** (`bool`, default: `true`)

  Player-hardware VRAM guardrail for discrete GPUs (see below). When `true`, caps `texture_cache_max_entries` (`128` under `3584` MiB, `256` on `3584`–`7167` MiB; `7168` MiB and above are untouched) and clamps any project-raised `staging_buffer_size_mb` (`32` MiB under `2048` MiB, `64` MiB on `2048`–`7167` MiB; `7168` MiB and above are untouched). Only ever lowers values, never raises them. Set to `false` to use your configured values exactly as written.

- **`dsgd_extension/texture_cache_enabled`** (`bool`, default: `true`)

  LRU residency cache (`DSTextureCache`). `true`: `REUSE` loads hit the cache and become engine-resident, so post-prewarm scene loads bind pointer-identical textures with no disk read, and prewarm works. `false`: every load re-reads and re-decompresses and the texture dies with its last reference - GPU decompression still works, but async prewarm refuses to start and existing entries are cleared. Prefer lowering `texture_cache_max_entries` over disabling.

- **`dsgd_extension/texture_cache_max_entries`** (`int`, default: `512`)

  Maximum number of texture entries held in the LRU cache (range: `16`–`2048`). Auto-tune reduces it to `128` on integrated GPUs and discrete GPUs under `3584` MiB, and to `256` on `3584`–`7167` MiB discrete GPUs; `7168` MiB and above keep the full value. When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM; evicting a prewarmed entry before its batch finishes emits a one-time warning. A prewarm batch may temporarily hold up to 25% more entries (at least one) so it does not evict its own waiting textures; beyond that bounded cap the oldest unconsumed prewarm entry is evicted instead of growing further, and the over-capacity warning is emitted once.

- **`dsgd_extension/shutdown_drain_timeout_ms`** (`int`, default: `10000`)

  Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. Read at runtime if set in `project.godot` / `ProjectSettings`, but not registered in the Project Settings UI by default.

- **`dsgd_extension/sync_flush_timeout_ms`** (`int`, default: `3000`)

  Maximum timeout in milliseconds (clamped range: `500`–`10000` ms) for synchronous batch flushes (`submit_batch` / `flush_batch`). Dictates the timeout budget for hardware queue execution and fence completion, with up to half (`sync_timeout_ms / 2`) allocated for draining prior in-flight batches beforehand. Synchronous flushes should never be called inside gameplay process loops (`_process`). Read at runtime if set in `project.godot` / `ProjectSettings`, but not registered in the Project Settings UI by default.

- **`dsgd_extension/show_preview_runtimes`** (`bool`, default: `false`)

  Editor-tool preference, not a runtime setting: when `true`, the Manage DirectStorage Runtime dialog lists runtime versions newer than 1.3 (for example the 1.4 preview). The dialog's **Show preview runtimes** checkbox is the normal way to set it. Versions already installed, staged or downloaded are listed regardless of this value.

- **`dsgd_extension/archive_name`** (`String`, default: `"textures.dspkg"`)

  Overrides the `.dspkg` archive filename looked up at runtime. Export writes it as an internal setting; test and benchmark harnesses can also set it via `ProjectSettings` to switch between archives (for example `textures.dspkg` vs. `textures_raw.dspkg`). Read at runtime, not registered in the Project Settings UI by default.

- **`dsgd_extension/force_archive_resolution`** (`bool`, default: `false`)

  Allows editor/dev runs (`OS.has_feature("editor")`) to resolve a `.dspkg` archive instead of crawling loose `.dstex` files, for testing or benchmarking archives without exporting. Read at runtime, not registered in the Project Settings UI by default.

- **`dsgd_extension/skip_archive_import_check`** (`bool`, default: `false`)

  Skips per-entry `.import` / TOC `as_image_texture` exclusion checks during `DSPreloader.collect_scene_textures()` archive scans. Read at runtime, not registered in the Project Settings UI by default.

### Notes on Queue Capacity and Timeouts

- **Queue Capacity & Streaming Bursts**: `dsgd_extension/queue_capacity` defines the hardware queue slot capacity (default `512`, clamped range: `128`–`8192`). Textures with full mip chains expand into multiple subresource requests (e.g., 10–12 mips per texture). The streaming engine (`DSContext::flush_stream_async`) automatically budgets burst popping based on subresource counts and applies transient backpressure requeuing to prevent queue exhaustion during bulk scene prewarming.
- **Synchronous Timeout Budget**: During synchronous fallback loads (e.g. scene loading uncached textures via `ResourceLoader`), `dsgd_extension/sync_flush_timeout_ms` ensures adequate headroom for NVMe DMA transfers, GDeflate hardware decompression, and GPU barrier completion under high engine contention.

### Integrated GPUs and Auto-Tune (What UMA Means)

Most gaming desktops have a **discrete GPU**: a separate graphics card with its own private memory (dedicated VRAM, usually several GB). Laptops, handhelds, and mini-PCs often have an **integrated GPU** (Intel UHD/Iris, AMD "Radeon Graphics", Apple Silicon) with no private memory. These use a **unified memory architecture (UMA)**: the GPU borrows ordinary system RAM, typically just a few hundred MB carved out plus whatever it can share.

Why that matters for DirectStorage settings:

- The **staging buffer** (`32` MiB by default; up to four internal buffers at that size) is scratch memory DirectStorage uses while moving data. On a discrete GPU it lives in VRAM where there is plenty of room. On an integrated GPU it comes out of the same shared RAM the game, the OS, and the prewarmed textures all compete for, so a big staging buffer adds memory pressure for no speed gain.
- **GPU decompression still works** on integrated GPUs (the log will say `enabled (GPU-optimized/fallback)`), but "direct to VRAM" really means "direct to a shared-memory allocation" - there is no separate VRAM to be direct *to*.
- **Prewarm auto-routing**: `prewarm_gpu_async()` calls automatically route to System RAM prewarm (`prewarm_async()`) on integrated GPUs when auto-tune is enabled. This prevents 3D/Copy-queue render contention and hard freezes during animated loading screens while preserving direct-to-VRAM DMA on discrete graphics cards.

`dsgd_extension/auto_tune_for_integrated_gpu` (default `true`) handles this automatically: when the extension detects an integrated/UMA adapter (queried directly via Direct3D 12 hardware UMA architecture, or under 1 GiB of dedicated VRAM reported by Windows), it quietly caps the *effective* staging buffer to 64 MiB, the queue to 256, and `texture_cache_max_entries` to 128 at startup, routes GPU prewarms to System RAM, and logs any staging/queue clamp once (`auto-tune clamped staging ...->64 MiB, queue 512->256`). It only ever lowers, never raises - if you already set smaller values, they pass through untouched, and your saved settings are never rewritten. Discrete-GPU PCs are completely unaffected. Turn it off (`false`) if you want full manual control, e.g. when benchmarking exact configured values.

`dsgd_extension/auto_tune_for_low_vram_gpu` (default `true`) is a separate player-hardware guardrail for discrete GPUs (using DXGI-safe boundaries so nominal 4 GiB / 8 GiB cards reporting ~4004 / ~7900 MiB stay in their intended tiers). It does two things:
- **Texture cache residency**: caps `texture_cache_max_entries` (default `512`) to `128` under `3584` MiB and `256` on `3584`–`7167` MiB cards (`7168` MiB and above are untouched) so the larger default does not cause VRAM pressure on small cards.
- **Staging buffer safety net**: the DirectStorage factory can hold up to four staging buffers at the configured size (~4x residency). At the `32` MiB default this cap is a no-op, but if a project raises `staging_buffer_size_mb` for higher-VRAM target specs (for example `128` MiB = ~512 MiB residency), this clamps it on players' smaller discrete GPUs (`64` MiB on `2048`–`7167` MiB cards, `32` MiB under `2048` MiB, untouched at `7168` MiB and above) and logs the before/after values once.

It never changes the queue, never raises a value, and leaves lower configured settings alone. Set it to `false` to keep your configured values verbatim on every GPU.

## Export Preset Options

The exporter supports **Windows, Linux, and macOS** desktop presets, adding **eight** options on Windows Desktop and **six** on Linux/macOS (omitting the two Windows DirectStorage runtime license options) on the **Options** tab (`Project -> Export... -> <Desktop Preset> -> Options`):

- **`dsgd_extension/bundle_textures_dspkg`** (`bool`, default: `true`)

  Bundles exported textures into `textures.dspkg` for unbuffered DirectStorage DMA loading.

- **`dsgd_extension/verify_pack_on_export`** (`bool`, default: `true`)

  Scans a protected standalone `.pck` after export to verify its header, non-empty directory, and absence of plaintext key-file magic (`PACK_VERIFY status=ok`). Embedded PCKs print `status=unverified`; turning this off prints `status=skipped` with a warning and leaves the build unverified.

- **`dsgd_extension/enforce_archive_pairing`** (`bool`, default: `true`)

  Publishes a random 64-bit archive id per export and compares it, in full, when the runtime opens the archive. Set to `false` only for local tooling: it publishes `0`, which skips the check in that build, and writes an archive carrying id `0` that any build expecting a real id rejects.

- **`dsgd_extension/steampipe_1mb_alignment`** (`bool`, default: `false`)

  Aligns texture payloads to 1 MiB chunks for SteamPipe differential patching efficiency.

- **`dsgd_extension/store_uncompressed_textures`** (`bool`, default: `false` on Windows / `true` on Linux, macOS)

  Repacks GDeflate `.dstex` payloads uncompressed (`NONE`) into `textures.dspkg` (v5) or into the PCK when bundling is disabled, avoiding CPU GDeflate decompression overhead on non-DirectStorage platforms.

- **`dsgd_extension/include_dsgd_license_file`** (`bool`, default: `true`)

  Writes `dsgd_LICENSE.txt` next to the exported binaries, containing the project license and the third-party components of `dsgd_64.dll`. Disable when you carry those attributions in-game instead; the source texts stay in the addon folder.

- **`dsgd_extension/include_directstorage_license_file`** (`bool`, default: `true`)

  *(Windows only)* Writes Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt` for the active runtime version as `DIRECTSTORAGE_LICENSE.txt`.

- **`dsgd_extension/merge_license_files`** (`bool`, default: `true`)

  *(Windows only)* When both license files are included, appends the DirectStorage runtime texts to `dsgd_LICENSE.txt` and skips the separate runtime file. No effect when fewer than two files are included.

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

## Trademarks and Attribution

### License

`dsgd` is licensed under the **Mozilla Public License, v. 2.0 (MPL-2.0)** (`LICENSE`), except for the four shipped runtime GDScript helpers (`ds_runtime.gd`, `ds_preloader.gd`, `ds_batch_manager.gd`, and `ds_dspkg_static.gd`), which are licensed under the **MIT License** (`LICENSE-MIT`) so games can freely localize, tune, or adapt them.

- **Closed-source and commercial games are permitted:** You can bundle `dsgd` with proprietary games without disclosing your game's own code or assets; only modifications made to `dsgd`'s own MPL-2.0 files must be shared under MPL-2.0.
- **Export compliance (MPL-2.0 Section 3.2(a)):** Keep `dsgd_extension/include_dsgd_license_file` enabled (default) so `dsgd_LICENSE.txt` is written beside exported binaries. If you disable it to display attributions in-game, include the MPL-2.0 license notice, the DSGD source URL (`https://github.com/medium-effort/godot-directstorage-extension`), and the attributions from `THIRD_PARTY_NOTICES.txt` (plus Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt` when shipping `dstorage.dll` / `dstoragecore.dll`).

### Trademarks and Notices

This addon/extension (DSGD) is an independent project and is not affiliated with, endorsed by, or sponsored by Microsoft. Microsoft and DirectStorage are trademarks of the Microsoft group of companies. The DirectStorage runtime is installed under Microsoft's license and is not bundled with this addon/extension; see the [repository README](../../../README.md#installation--quick-start).

Third-party license texts and attributions are in `THIRD_PARTY_NOTICES.txt`; the project's own licenses are in `LICENSE` (MPL-2.0) and `LICENSE-MIT` (MIT). They ship with the addon and are copied next to exported binaries as `dsgd_LICENSE.txt` (with the DirectStorage runtime texts appended when merging is enabled) or as a separate `DIRECTSTORAGE_LICENSE.txt`. The exporter removes and rewrites those files on every export, so the directory always matches the preset.
