# DirectStorage Texture Packaging (`.dspkg`) on Export

This section outlines how textures are automatically bundled into standalone `.dspkg` packages during project export.

---

## 1. Automatic Export Pipeline (`ds_export_plugin.gd`)

When exporting a project with the `directstorage_accelerator` addon enabled, Godot's export pipeline automatically runs [`ds_export_plugin.gd`](project/addons/directstorage_accelerator/scripts/ds_export_plugin.gd):

1. **Asset Interception**:
   During `_export_file()`, the exporter identifies all texture assets imported by the DirectStorage importer (`directstorage_texture`) or targeting `.dstex` containers.
2. **Dual-Key Indexing**:
   Every texture entry is indexed in the `.dspkg` archive under **two separate keys**:
   - The original logical resource path: `res://characters/hero.png`
   - The engine's remap cache path: `res://.godot/imported/hero.png-<hash>.dstex`
   This guarantees that both high-level scripts referencing source `.png` assets and low-level engine dependencies querying `.dstex` cache paths resolve to the exact same archive entry.
3. **Exclusion from the `.pck` (`skip()`)**:
   The exporter calls `skip()` on all texture binary payloads. This strips the heavy compressed texture data completely out of the Godot `.pck` file, preventing asset duplication and keeping the `.pck` minimal.
   - **Important**: `.import` files are explicitly preserved in the `.pck` so Godot's internal remap tables remain intact.
4. **Hardware Placement Alignment**:
   In accordance with Direct3D 12 placement constraints, multi-mip texture payloads in `.dspkg` archives are padded to 512-byte boundaries (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`). This allows DirectStorage unbuffered NVMe DMA requests to transfer individual subresources directly into D3D12 texture footprints.
5. **Final Deployment**:
   During `_export_end()`, the finalized archive is copied to the export destination directory right beside the game executable, and native binaries are deployed to their GDExtension runtime subfolder:
   ```text
   <export_dir>/
   ├── my_game.exe
   ├── my_game.pck             (lightweight: scripts, scenes, materials, .import metadata)
   ├── textures.dspkg          (standalone DirectStorage texture archive)
   └── addons/
       └── directstorage_accelerator/
           └── bin/
               ├── directstorage_accelerator.windows.template_release.x86_64.dll
               ├── dstorage.dll
               └── dstoragecore.dll
   ```

---

## 2. Runtime Resolution & Transparent Loading

At runtime, the extension handles texture resolution transparently via [`DSResourceFormatLoader`](src/integration/ds_resource_format_loader.cpp):

- **Automatic Archive Discovery**:
  The loader checks `res://textures.dspkg` and falls back to inspecting the directory containing the running executable (`OS.get_executable_path().get_base_dir().path_join("textures.dspkg")`).
- **Seamless Engine Integration**:
  Standard Godot resource loading calls—such as `ResourceLoader.load("res://characters/hero.png")` or scene dependencies instantiated via `ResourceLoader.load_threaded_request("res://level/level.tscn")`—automatically pull and decompress their texture payloads directly from `textures.dspkg` through DirectStorage without requiring manual package opening or custom batch streaming scripts.

---

## 3. Async Texture Prewarm (True Streaming)

For level transitions, textures can be streamed ahead of the scene bind with zero main-thread waits. Helpers live in [`ds_preloader.gd`](project/addons/directstorage_accelerator/scripts/ds_preloader.gd) (`DSPreloader`); the per-frame drain lives in the `DSBatchManager` autoload (collect → `flush_stream_async()` → `poll_completed()` → progress).

1. **Collect** prewarmable engine paths (recursive `ResourceLoader.get_dependencies()` + `DirAccess` sweep, projector/EXR filtered):
   ```gdscript
   var deps: PackedStringArray = DSPreloader.collect_scene_textures("res://levels/level1.tscn")
   ```
2. **Prewarm** (non-blocking, returns a stream tag; progress tracking is opt-in via `track_prewarm`), then **bind with `CACHE_MODE_REUSE`** so the scene resolver reuses the resident textures with zero disk re-reads:
   ```gdscript
   var tag: int = DSPreloader.prewarm_async(deps)
   await DSPreloader.await_prewarm_finished(tag) # or poll DSPreloader.get_prewarm_progress(tag) for a bar
   var packed := ResourceLoader.load("res://levels/level1.tscn", "PackedScene", ResourceLoader.CACHE_MODE_REUSE) as PackedScene
   ```
   `CACHE_MODE_IGNORE` / `CACHE_MODE_REPLACE` bypass the cache and waste the prewarm — always use `CACHE_MODE_REUSE` for the post-prewarm scene load.
3. **Loading screen over PSO**: keep the overlay visible across `instantiate()` + `add_child()` + one drawn frame (see `DSPreloader.load_scene_with_loading_screen()`), so scene-graph build and driver PSO/shader compilation finish before gameplay is revealed. When extension async is unavailable, the same helper falls back to a frame-budgeted sync `ResourceLoader.load()` loop that holds 60 FPS.

---

## 4. Project Settings Reference

The extension provides configurable project settings in Godot under the `directstorage/` namespace (`Project -> Project Settings... -> General -> DirectStorage`):

| Setting | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/staging_buffer_size_mb` | `int` | `128` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (range: `16`–`1024` MiB). Increase this if loading very large textures or high-resolution mip streams whose compressed payloads exceed the buffer. |
| `directstorage/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `directstorage/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (range: `64`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `directstorage/texture_cache_enabled` | `bool` | `true` | Enables or disables the internal runtime texture LRU cache (`DSTextureCache`). When disabled, loaded textures bypass the resident cache. |
| `directstorage/texture_cache_max_entries` | `int` | `128` | Maximum number of texture entries held in the LRU cache (range: `16`–`2048`). When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. |
| `directstorage/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |


---

## 5. Export Preset Options Reference

When configuring a Windows Desktop export preset (`Project -> Export... -> Windows Desktop -> Options`), the following custom options are provided under the `directstorage/` category:

| Export Option | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/bundle_textures_dspkg` | `bool` | `true` | When `true`, all exported `.dstex` textures are extracted from the `.pck` via `skip()` and bundled into a standalone DirectStorage archive (`textures.dspkg`) deployed alongside the executable for unbuffered NVMe DMA loading. When `false`, texture payloads remain embedded inside the `.pck` and fall back to CPU software decompression at runtime. |
| `directstorage/enforce_archive_pairing` | `bool` | `true` | When `true`, computes a two-tier archive fingerprint `(project_guid << 32) | manifest_hash` from sorted `(path, size)` pairs and embeds it in both the `.pck` and `textures.dspkg`. At runtime, mounting a mismatched or stale archive triggers an immediate failure to protect against corrupt reads or memory errors. |
| `directstorage/steampipe_1mb_alignment` | `bool` | `false` | When `true`, aligns each top-level texture payload inside `textures.dspkg` to a 1 MiB (`1,048,576` bytes) boundary instead of the standard 512-byte GPU placement boundary. This follows Valve's **Game Content Structure - Best Practices** for SteamPipe: an update or size change to one texture will never shift downstream textures across 1 MB chunk boundaries, minimizing delta patch download sizes for players on Steam. |
