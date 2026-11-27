# Benchmarks & Performance Analysis

This document summarizes the measured benchmark results behind the headline claims in the [repository README](../README.md), using two 5-iteration exported release runs (`arena_v2` and `bistro`, `2track` suite, captured 2026-11-24).

> [!NOTE]
> The standalone `dsgd-benchmark` harness repository used to generate these reports is currently a work in progress and will be published separately later.

---

## 1. Headline Claims Mapped to Measured Results

| Headline Claim | Measured Baseline | Measured DirectStorage (GPU GDeflate) | Ratio | Preset & Metric |
| :--- | ---: | ---: | :---: | :--- |
| **Up to 2.2x faster cold boots** | `2384.8 ms` (Stock Vanilla) | `1051.0 ms` | **2.27x** | `arena_v2` Flow 1 True Cold Boot (`1.74x` vs. Tuned 16-Thread Vanilla at `1823.8 ms`) |
| **Up to 4.1x faster texture streaming** | `353.5 MiB/s` (Stock Vanilla) | `1474.4 MiB/s` (`2115.4 MiB/s` DS Batch) | **4.17x** | `bistro` Flow 1 Phase 2 Logical Throughput (`1.85x` vs. Tuned Vanilla at `797.7 MiB/s`) |
| **Up to 1.8x smaller exported texture footprint** | `917.4 MiB` (S3TC `.ctex`) | `500.9 MiB` (`textures.dspkg`) | **1.83x** | `bistro` `main` (274 textures; `zone_b` 136 textures: `485.3 -> 242.1 MiB`, `2.00x` smaller) |

On `bistro`, total cold boot drops from `2669.3 ms` to `1642.5 ms` (**1.63x** vs. Stock Vanilla, **1.49x** vs. Tuned Vanilla at `2455.1 ms`). On `arena_v2`, Phase 2 logical streaming reaches `1491.3 MiB/s` (`1978.9 MiB/s` DS Batch) vs. `583.1 MiB/s` Stock (**2.56x**) and `812.8 MiB/s` Tuned (**1.83x**), and exported texture footprint shrinks from `1337.7 MiB` to `758.6 MiB` (**1.76x** smaller).

---

## 2. Test Environment, Configuration & Datasets

| Item | Configuration / Value |
| :--- | :--- |
| **Execution & Build** | `exported_release`, `dsgd_64.dll` (`template_release`, `is_dev_build=false`, commit `f79fa1d`), Godot 4.7, 1080p exclusive fullscreen |
| **Extension Settings** | `staging_buffer_size_mb=64`, `queue_capacity=1024`, `texture_cache_max_entries=2048` |
| **CPU** | 12th Gen Intel Core i3-12100F (8 logical cores) |
| **GPU** | NVIDIA GeForce GTX 1050 Ti (4004 MiB dedicated VRAM, 16258 MiB shared, `UMA=false`, `d3d12`, GPU GDeflate `tier=2`, optimized compute queue) |
| **Storage (`res://`)** | `C:\` NVMe SSD, NTFS, Windows BypassIO `Enabled`, `directstorage_supported=true` |
| **Methodology** | 5 iterations per mode (median reported, min-max range shown); OS file cache purged between cold runs via `FILE_FLAG_NO_BUFFERING` (`--per-set-cleanup`) |
| **Host Contention Caveat** | Windows Defender (`MsMpEng`) active among top processes (~532-547 MiB RAM; pre-run idle CPU 1.6%-2.1%, GPU 0.0%) |
| **Self-Verification** | **2021 / 2021** assertions passed on both `arena_v2` and `bistro` (zero unexpected CPU fallbacks, zero draw-time PSO compilations, material texture-type validation) |

### Datasets

- **`arena_v2`**: `main` has 329 textures (`1337.7 MiB` uncompressed S3TC -> `758.6 MiB` GDeflate, calibrated ratio `1.48x-3.71x`, including 5 `Decal`/`SpotLight3D` `ImageTexture` compat assets and 682 checked material slots); `zone_b` has 93 textures (`280.3 MiB` raw -> `158.9 MiB` GDeflate). Light scene graph (`Ph 3 Bind` ~56-67 ms), so total load time is texture-I/O dominated.
- **`bistro`**: `main` has 274 textures (`917.4 MiB` uncompressed S3TC -> `500.9 MiB` GDeflate, calibrated ratio `1.00x-369.77x`, 3284 checked material slots); `zone_b` has 136 textures (`485.3 MiB` raw -> `242.1 MiB` GDeflate). Heavy real-world geometry and material scene graph (`Ph 3 Bind` ~919-990 ms), showing how DirectStorage behaves when non-texture scene loading (loading/uploading external `.res` `ArrayMesh` geometry plus building the material/node graph) is a major share of total boot time.

---

## 3. Cold Boot & Size vs. Speed Comparison (Flow 1: OS Cache Purged)

Track 1 uses uncompressed S3TC payloads (`0%` compression, equal disk size) to isolate pure I/O and transport differences. Track 2 compresses `.dspkg` payloads with GDeflate.

### `arena_v2` Cold Boot (5-Iteration Median)

| Pipeline | Disk (MiB) | Size vs. Vanilla | Ph 1 Disc (ms) | Ph 2 Stream (ms) | Ph 3 Bind (ms) | Ph 4 Inst+PSO (ms) | Total (ms) | Ph 2 Logical (MiB/s) | DS Batch (MiB/s) | vs. Vanilla Stock |
| :--- | ---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| T1 Vanilla Stock (`load_threaded_request`, `.ctex`) | 1337.7 | 1.00x | 0.0 | 2294.0 | 0.0 | 90.8 | 2384.8 (2312.6-2754.4) | 583.1 | - | 1.00x |
| T1 Vanilla Tuned (16-Thread + Cache, `.ctex`) | 1337.7 | 1.00x | 23.3 | 1645.7 | 58.3 | 96.5 | 1823.8 (1821.8-1825.2) | 812.8 | - | 1.31x |
| T1 DS CPU Fallback (`SoftwareFallback`, Raw) | 1339.0 | 1.00x | 4.7 | 1139.7 | 64.8 | 90.5 | 1299.7 (1292.3-1314.1) | 1173.7 | 1262.1 | 1.83x |
| T1 DS RAM/UMA (D3D12 Queue -> CPU, Raw) | 1339.0 | 1.00x | 3.9 | 1226.3 | 56.1 | 93.9 | 1380.1 (1374.1-1396.9) | 1090.8 | 1182.3 | 1.73x |
| T1 DS GPU Raw (DirectStorage DMA, Raw) | 1339.0 | 1.00x | 3.7 | 1136.3 | 64.7 | 87.7 | 1292.2 (1288.1-1424.6) | 1177.3 | 1192.8 | 1.85x |
| T2 DS CPU Fallback (`SoftwareFallback`, GDeflate) | 758.6 | 1.76x smaller | 3.3 | 1538.1 | 66.5 | 90.4 | 1698.2 (1682.7-1741.3) | 869.7 | 1084.0 | 1.40x |
| T2 DS RAM/UMA (D3D12 Queue + CPU GDeflate) | 758.6 | 1.76x smaller | 2.8 | 1686.2 | 56.0 | 92.8 | 1837.7 (1671.0-1966.7) | 793.3 | 1240.6 | 1.30x |
| **T2 DS GPU GDeflate (DirectStorage DMA + Compute)** | **758.6** | **1.76x smaller** | **3.4** | **897.0** | **57.9** | **92.8** | **1051.0 (1036.8-1053.4)** | **1491.3** | **1978.9** | **2.27x** |

### `bistro` Cold Boot (5-Iteration Median)

| Pipeline | Disk (MiB) | Size vs. Vanilla | Ph 1 Disc (ms) | Ph 2 Stream (ms) | Ph 3 Bind (ms) | Ph 4 Inst+PSO (ms) | Total (ms) | Ph 2 Logical (MiB/s) | DS Batch (MiB/s) | vs. Vanilla Stock |
| :--- | ---: | :---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| T1 Vanilla Stock (`load_threaded_request`, `.ctex`) | 917.4 | 1.00x | 0.0 | 2595.2 | 0.1 | 74.1 | 2669.3 (2637.7-2848.3) | 353.5 | - | 1.00x |
| T1 Vanilla Tuned (16-Thread + Cache, `.ctex`) | 917.4 | 1.00x | 311.4 | 1150.0 | 918.8 | 74.9 | 2455.1 (2337.0-2537.3) | 797.7 | - | 1.09x |
| T1 DS CPU Fallback (`SoftwareFallback`, Raw) | 918.3 | 1.00x | 3.4 | 771.9 | 949.9 | 71.7 | 1796.9 (1712.7-1891.3) | 1188.5 | 1298.5 | 1.49x |
| T1 DS RAM/UMA (D3D12 Queue -> CPU, Raw) | 918.3 | 1.00x | 3.4 | 844.9 | 946.5 | 72.5 | 1867.3 (1763.9-1978.8) | 1085.8 | 1105.8 | 1.43x |
| T1 DS GPU Raw (DirectStorage DMA, Raw) | 918.3 | 1.00x | 3.8 | 797.0 | 989.7 | 70.2 | 1860.7 (1815.3-2041.2) | 1151.1 | 1173.6 | 1.43x |
| T2 DS CPU Fallback (`SoftwareFallback`, GDeflate) | 500.9 | 1.83x smaller | 2.9 | 1295.3 | 953.0 | 72.8 | 2324.0 (2231.4-2465.1) | 708.2 | 862.3 | 1.15x |
| T2 DS RAM/UMA (D3D12 Queue + CPU GDeflate) | 500.9 | 1.83x smaller | 2.9 | 1373.1 | 938.9 | 71.1 | 2386.0 (2315.7-3309.5) | 668.1 | 793.2 | 1.12x |
| **T2 DS GPU GDeflate (DirectStorage DMA + Compute)** | **500.9** | **1.83x smaller** | **2.7** | **622.2** | **947.2** | **70.4** | **1642.5 (1535.9-1689.8)** | **1474.4** | **2115.4** | **1.63x** |

> [!NOTE]
> Stock Vanilla (`load_threaded_request`) parses the `.tscn` inside `Ph 2 Stream` (leaving `Ph 3 Bind` near `0.0 ms`), so use `Total (ms)` for cross-mode comparison.

### Key Takeaways

- **Pure transport win (Track 1)**: Even at equal disk size, `DS GPU Raw` cuts cold boot by **1.85x** on `arena_v2` and **1.43x** on `bistro`. `DSPkg` TOC discovery also drops `Ph 1 Disc` on `bistro` from `311.4 ms` (Tuned Vanilla) to `2.7-3.8 ms`.
- **GPU vs. CPU GDeflate tradeoff**: On GPU, reading ~44% fewer bytes from NVMe plus compute decompression beats uncompressed reads (`1292.2 -> 1051.0 ms` on `arena_v2`; `1860.7 -> 1642.5 ms` on `bistro`). On CPU-only paths (`DS CPU Fallback`, `DS RAM/UMA`), GDeflate trades load time for smaller installs (`1299.7 -> 1698.2 ms` on `arena_v2`), which is why Linux/macOS exports default `store_uncompressed_textures = true`.
- **Scene-bind ceiling on `bistro`**: `Ph 2 Stream` drops from `1150.0 ms` (Tuned Vanilla) / `2595.2 ms` (Stock Vanilla) to `622.2 ms`, while `Ph 3 Bind` remains ~`947 ms` across all prewarmed modes. DirectStorage accelerates texture streaming, not non-texture scene loading (external `.res` mesh I/O/upload and material/node construction).

---

## 4. Player Lifecycle Flows (Cold Boot, Warm OS Cache, Level Swap, Respawn)

| Player Flow | Preset | Vanilla Stock | Vanilla Tuned | DS GPU Raw | DS CPU GDeflate | DS GPU GDeflate |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| **Flow 1: True Cold Boot** (Cold OS Cache) | `arena_v2` | 2384.8 ms (1337.7 MiB) | 1823.8 ms (1337.7 MiB) | 1292.2 ms (1339.0 MiB) | 1698.2 ms (758.6 MiB) | **1051.0 ms** (758.6 MiB) |
| **Flow 1: True Cold Boot** (Cold OS Cache) | `bistro` | 2669.3 ms (917.4 MiB) | 2455.1 ms (917.4 MiB) | 1860.7 ms (918.3 MiB) | 2324.0 ms (500.9 MiB) | **1642.5 ms** (500.9 MiB) |
| **Flow 2: Quit & Relaunch** (Warm OS Cache) | `arena_v2` | 1040.0 ms (1337.7 MiB) | **751.1 ms** (1337.7 MiB) | 1293.5 ms (1339.0 MiB) | 1656.3 ms (758.6 MiB) | 1049.6 ms (758.6 MiB) |
| **Flow 2: Quit & Relaunch** (Warm OS Cache) | `bistro` | 1421.5 ms (917.4 MiB) | **1104.5 ms** (917.4 MiB) | 1533.9 ms (918.3 MiB) | 1826.8 ms (500.9 MiB) | 1350.4 ms (500.9 MiB) |
| **Flow 3: Level Swap** (Cold `zone_b`, cleared cache) | `arena_v2` | 488.3 ms (280.3 MiB) | 393.4 ms (280.3 MiB) | 327.3 ms (280.7 MiB) | 400.1 ms (158.9 MiB) | **277.7 ms** (158.9 MiB) |
| **Flow 3: Level Swap** (Cold `zone_b`, cleared cache) | `bistro` | 1904.4 ms (485.3 MiB) | 1811.1 ms (485.3 MiB) | 1469.9 ms (485.8 MiB) | 1697.6 ms (242.1 MiB) | **1363.7 ms** (242.1 MiB) |
| **Flow 4: Same-Level Respawn** (`zone_b` -> `zone_b`) | `arena_v2` | 253.4 ms (280.3 MiB) | 76.2 ms (0.0 MiB) | **71.0 ms** (0.0 MiB) | 73.2 ms (0.0 MiB) | 72.2 ms (0.0 MiB) |
| **Flow 4: Same-Level Respawn** (`zone_b` -> `zone_b`) | `bistro` | 1230.0 ms (485.3 MiB) | **799.2 ms** (0.0 MiB) | 877.6 ms (0.0 MiB) | 862.1 ms (0.0 MiB) | 898.5 ms (0.0 MiB) |

- **Why Vanilla Tuned wins Flow 2 (Warm OS Cache)**: Vanilla `.ctex` stores uncompressed S3TC on disk, so when the dataset fits in Windows Standby RAM, Vanilla copies uncompressed blocks straight from memory with zero decompression. DirectStorage Mode 5 executes unbuffered NVMe DMA (`BypassIO`) + GPU GDeflate compute decompression on every cold launch.
- **Flow 4 (Same-Level Respawn)**: Stock Vanilla relies on Godot's weak `ResourceCache`, so freeing `zone_b` drops its textures (`280.3` / `485.3 MiB` re-read on respawn). `DSTextureCache` keeps strong references (`0.0 MiB` disk read), cutting `arena_v2` respawn from `253.4 ms` to `72.2 ms` (**3.51x**).

---

## 5. Open-World Background Zone Streaming Under Active 3D Gameplay (Scenario B)

Streams `zone_b` into a live orbiting 3D scene (`arena_v2` also runs a deterministic CPU gameplay simulation).

- **B1 (Uncapped `Engine.max_fps = 0`)**: Both Vanilla Tuned (`concurrency=16`) and DS Mode 5 (`background_pacing=false`, `chunk_size=16`) run unthrottled for maximum background throughput.
- **B2 (60 FPS Paced `Engine.max_fps = 60`)**: Both Vanilla Tuned (`concurrency=4`, `2 ms` harvest budget) and DS Mode 5 (`DSRuntime.set_background_pacing(true)`, `chunk_size=1`, `2 MiB` cap) pace work to protect 60 FPS.

### `arena_v2` Background Streaming

| Scenario & Mode | Base FPS | Stream FPS | 1% Low FPS | Worst Spike (B1 Stream / B2 Inst) | Dropped | Stream Wall (ms) | Proc CPU (ms) | Slices (W/M/H) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| **B1** Vanilla Stock | 84.9 | 90.0 | 52.5 | 19.0 ms (`11.8 / 19.0`) | 1 (>16.6ms) | 401.4 | 1390.6 | - |
| **B1** Vanilla Tuned | 84.2 | 22.3 | 6.2 | 160.0 ms (`160.0 / 18.9`) | 5 (>16.6ms) | 374.9 | 1812.5 | - |
| **B1** DS GPU Raw | 84.7 | 88.0 | 56.1 | 17.8 ms (`17.8 / 10.0`) | 1 (>16.6ms) | 296.4 | 1453.1 | 21 / 0 / 0 |
| **B1** DS CPU GDeflate | 84.6 | 70.8 | 35.7 | 28.0 ms (`28.0 / 10.1`) | 7 (>16.6ms) | 351.7 | 10781.3 | 10 / 5 / 0 |
| **B1** DS GPU GDeflate | 84.8 | 57.3 | 26.2 | 38.2 ms (`38.2 / 19.2`) | 7 (>16.6ms) | 256.6 | 1218.8 | 2 / 3 / 2 |
| **B2 (60 FPS)** Vanilla Stock | 61.2 | 62.5 | 40.8 | 24.5 ms (`17.1 / 24.5`) | 1 (>18.3ms) | 411.9 | 1687.5 | - |
| **B2 (60 FPS)** Vanilla Tuned | 60.7 | 48.0 | 24.2 | 41.3 ms (`41.3 / 24.5`) | 8 (>18.3ms) | 471.3 | 1718.8 | - |
| **B2 (60 FPS)** DS GPU Raw | 60.7 | 61.4 | 42.0 | 23.8 ms (`23.8 / 15.7`) | 5 (>18.3ms) | 723.6 | 1312.5 | 40 / 0 / 0 |
| **B2 (60 FPS)** DS CPU GDeflate | 60.7 | 60.9 | 42.2 | 23.7 ms (`23.7 / 15.5`) | 1 (>18.3ms) | 1139.8 | 9281.3 | 31 / 0 / 0 |
| **B2 (60 FPS)** DS GPU GDeflate | 60.7 | 61.3 | 42.8 | 23.3 ms (`23.3 / 15.7`) | 5 (>18.3ms) | 773.2 | 1250.0 | 41 / 0 / 0 |

### `bistro` Background Streaming

| Scenario & Mode | Base FPS | Stream FPS | 1% Low FPS | Worst Spike (B1 Stream / B2 Inst) | Dropped | Stream Wall (ms) | Proc CPU (ms) | Slices (W/M/H) |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| **B1** Vanilla Stock | 134.2 | 116.7 | 99.9 | 69.6 ms (`10.0 / 69.6`) | 2 (>16.6ms) | 3319.3 | 2078.1 | - |
| **B1** Vanilla Tuned | 133.2 | 101.7 | 88.0 | 313.4 ms (`313.4 / 70.4`) | 3 (>16.6ms) | 4658.5 | 2968.8 | - |
| **B1** DS GPU Raw | 133.3 | 110.8 | 81.8 | 68.3 ms (`14.0 / 68.3`) | 2 (>16.6ms) | 4477.9 | 3031.3 | 56 / 0 / 0 |
| **B1** DS CPU GDeflate | 133.1 | 110.4 | 89.5 | 64.5 ms (`40.3 / 64.5`) | 3 (>16.6ms) | 4409.3 | 14390.6 | 71 / 0 / 0 |
| **B1** DS GPU GDeflate | 134.0 | 105.5 | 29.3 | 70.4 ms (`54.5 / 70.4`) | 9 (>16.6ms) | 4458.0 | 3000.0 | 2 / 5 / 5 |
| **B2 (60 FPS)** Vanilla Stock | 60.7 | 59.2 | 39.4 | 65.9 ms (`16.8 / 65.9`) | 2 (>18.3ms) | 3045.1 | 2078.1 | - |
| **B2 (60 FPS)** Vanilla Tuned | 60.7 | 55.3 | 38.4 | 332.6 ms (`332.6 / 71.1`) | 3 (>18.3ms) | 4495.2 | 2906.3 | - |
| **B2 (60 FPS)** DS GPU Raw | 60.7 | 59.4 | 50.0 | 71.9 ms (`20.0 / 71.9`) | 10 (>18.3ms) | 4966.9 | 2468.8 | 69 / 0 / 0 |
| **B2 (60 FPS)** DS CPU GDeflate | 60.7 | 59.6 | 56.6 | 58.5 ms (`18.2 / 58.5`) | 2 (>18.3ms) | 5711.0 | 10796.9 | 66 / 0 / 0 |
| **B2 (60 FPS)** DS GPU GDeflate | 60.7 | 59.5 | 49.5 | 68.2 ms (`20.2 / 68.2`) | 11 (>18.3ms) | 5012.4 | 2812.5 | 71 / 0 / 0 |

### Key Takeaways

- **Why background pacing matters**: Uncapped `DS GPU GDeflate` (`B1`) finishes fastest (`256.6 ms` on `arena_v2`), but GPU compute decompression competes with 3D rendering on a GTX 1050 Ti (`84.8 -> 57.3 FPS`, 1% low `26.2 FPS`). Enabling `DSRuntime.set_background_pacing(true)` with `chunk_size = 1` and a `2 MiB` cap (`B2`) keeps `arena_v2` locked at `61.3 FPS` (`42.8 FPS` 1% low, `23.3 ms` worst spike, `41 / 0 / 0` slices), compared to Tuned Vanilla's `48.0 FPS` / `24.2 FPS` 1% low / `41.3 ms` spike.
- **CPU offload during gameplay**: In `B2`, `DS GPU GDeflate` uses **7.4x less** process CPU time than `DS CPU GDeflate` on `arena_v2` (`1250.0 ms` vs. `9281.3 ms`) and **3.8x less** on `bistro` (`2812.5 ms` vs. `10796.9 ms`).
- **Isolating streaming vs. instantiation spikes**: On `bistro` `B2`, the ~`58-72 ms` worst frame across all modes occurs in `B2 Inst` (main-thread scene instantiation), while the `B1 Stream` phase stays at `17.4-20.2 ms` across DirectStorage modes vs. `332.6 ms` for Tuned Vanilla.

---

## 6. Cache Residency & Memory Reclamation Verification (Scenario C, Mode 5)

| Metric | `arena_v2` | `bistro` |
| :--- | ---: | ---: |
| Expected / Cached Textures After Prewarm | 329 / 329 | 274 / 274 |
| Warm Same-Scene Re-Bind Total / Bind (ms) | 124.33 ms / 57.45 ms | 744.45 ms / 677.48 ms |
| Warm Same-Scene Disk Bytes Read | 0 B | 0 B |
| `DSTextureCache.clear_unused()` Evicted / Duration | 329 entries / 2011 usec | 274 entries / 1340 usec |
| Render-Thread RID Drain (10 frames + `frame_post_draw`) | 13.86 ms | 12.44 ms |
| `DSTextureCache.get_cached_count()` After Eviction | 0 | 0 |
| Texture VRAM Baseline / Loaded / Post-Evict (MiB) | 44.7 / 1975.3 / 591.2 MiB | 44.7 / 1038.5 / 76.8 MiB |
| Texture VRAM Delta / Reclaimed (MiB) | +1930.6 MiB / -1384.1 MiB | +993.9 MiB / -961.7 MiB |
| Total Device VRAM Baseline / Loaded / Post-Evict (MiB) | 78.2 / 2059.5 / 664.8 MiB | 78.2 / 2088.4 / 134.8 MiB |
| Total Device VRAM Delta / Reclaimed (MiB) | +1981.3 MiB / -1394.7 MiB | +2010.1 MiB / -1953.6 MiB |

`DSTextureCache.get_cached_count()` returns cleanly to `0` on both presets; Godot's `RenderingDevice` retains a baseline working-set pool after eviction.

---

## Related Documents

- [Runtime Texture Streaming, Prewarm & Caching](runtime-streaming-and-prewarm.md) - `DSPreloader`, `set_background_pacing()`, and `DSTextureCache` residency
- [Diagnostics, Profiling & Benchmarking](diagnostics-and-profiling.md) - `DSProfiler`, `DSContext` diagnostics, `DSRuntime` slice telemetry, and mode-switching
- [GDeflate Compression Level Benchmarks](for_developers/gdeflate-compression-benchmarks.md) - Per-asset GDeflate level 1-12 ratios and import timings
- [Demo Project (`project/README.md`)](../project/README.md) - F5 A/B benchmark and 5-mode loading demo setup
