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
   - When protection is requested but cannot be delivered (no PCK key, or the key path is not covered by the encryption filters), the export fails closed: every payload is skipped and no archive is written.
4. **Hardware Placement Alignment**:
   In accordance with Direct3D 12 placement constraints, multi-mip texture payloads in `.dspkg` archives are padded to 512-byte boundaries (`D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT`). This allows DirectStorage unbuffered NVMe DMA requests to transfer individual subresources directly into D3D12 texture footprints.
5. **Final Deployment**:
   During `_export_end()`, the finalized archive is renamed into place in the export destination directory, right beside the game executable, and Godot deploys the native binaries flat in the same directory:
   ```text
   <export_dir>/
   ├── my_game.exe
   ├── my_game.pck             (lightweight: scripts, scenes, materials, .import metadata)
   ├── textures.dspkg          (standalone DirectStorage texture archive)
   ├── dsgd_64.dll
   ├── dstorage.dll
   └── dstoragecore.dll
   ```
   A failed export never leaves a previous build's archive behind: the stale file is deleted so the game fails loudly instead of loading old textures. When textures were already removed from the PCK in plaintext mode, the partial archive is deployed instead of deleted, so those bytes are not lost.

---

## 2. Runtime Resolution & Transparent Loading

At runtime, the extension handles texture resolution transparently via [`DSResourceFormatLoader`](src/integration/ds_resource_format_loader.cpp):

- **Automatic Archive Discovery**:
  The `DSRuntime` autoload checks the published archive name (default `textures.dspkg`), the `res://` path, and the directory containing the running executable (`OS.get_executable_path().get_base_dir()`), in that order.
- **Pairing Check**:
  The archive is opened with the random 64-bit archive id published by the export, so a pack and an archive from different exports are rejected at boot instead of loading mismatched content.
- **Seamless Engine Integration**:
  Standard Godot resource loading calls—such as `ResourceLoader.load("res://characters/hero.png")` or scene dependencies instantiated via `ResourceLoader.load_threaded_request("res://level/level.tscn")`—automatically pull and decompress their texture payloads directly from the archive through DirectStorage without requiring manual package opening or custom batch streaming scripts.

---

## 3. Async Texture Prewarm (True Streaming)

For level transitions, textures can be streamed ahead of the scene bind with zero main-thread waits. Helpers live in [`ds_preloader.gd`](project/addons/directstorage_accelerator/scripts/ds_preloader.gd) (`DSPreloader`); the per-frame drain lives in the `DSRuntime` autoload (collect → `flush_stream_async()` → `poll_completed()` → progress).

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
| `directstorage/staging_buffer_size_mb` | `int` | `128` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (editor range: `16`–`2048` MiB). Increase this if loading very large textures or high-resolution mip streams whose compressed payloads exceed the buffer. |
| `directstorage/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `directstorage/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (editor range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `directstorage/auto_tune_for_integrated_gpu` | `bool` | `true` | When `true`, quietly lowers the staging buffer and queue (to 64 MiB / 256) on integrated/UMA GPUs. Only ever lowers values, never raises them. |
| `directstorage/texture_cache_enabled` | `bool` | `true` | LRU residency cache (`DSTextureCache`). `true`: `REUSE` loads hit the cache and become engine-resident, so post-prewarm scene loads bind pointer-identical textures with no disk read, and prewarm works. `false`: every load re-reads and re-decompresses and the texture dies with its last reference — GPU decompression still works, but async prewarm refuses to start and existing entries are cleared. Prefer lowering `texture_cache_max_entries` over disabling. |
| `directstorage/texture_cache_max_entries` | `int` | `128` | Maximum number of texture entries held in the LRU cache (editor range: `16`–`2048`). When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. |
| `directstorage/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |

---

## 5. Export Preset Options Reference

When configuring a Windows Desktop export preset (`Project -> Export... -> Windows Desktop -> Options`), the following custom options are provided under the `directstorage/` category:

| Export Option | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `directstorage/bundle_textures_dspkg` | `bool` | `true` | When `true`, all exported `.dstex` textures are extracted from the `.pck` via `skip()` and bundled into a standalone DirectStorage archive (`textures.dspkg`) deployed alongside the executable for unbuffered NVMe DMA loading. When `false`, texture payloads remain embedded inside the `.pck` and fall back to CPU software decompression at runtime. |
| `directstorage/enforce_archive_pairing` | `bool` | `true` | When `true`, publishes a random 64-bit archive id per export and writes the same value into the archive header; at runtime the archive is opened with that id and a mismatch fails the boot instead of loading a stale or foreign archive. An archive whose header id is `0` is rejected by such a build. When `false`, the published id is `0`, which skips the check in that build (local tooling only). |
| `directstorage/steampipe_1mb_alignment` | `bool` | `false` | When `true`, packs texture payloads inside `textures.dspkg` into 1 MiB (`1,048,576` bytes) chunks with 512-byte GPU sub-alignment, padding to 1 MiB boundaries only when a texture would cross a chunk boundary. This follows Valve's best practice for SteamPipe, keeping delta patch downloads isolated to modified 1 MiB chunks while eliminating wasted file-size of zero-padding compared to strict per-texture alignment. Note: strict 1 MiB per-texture alignment is available via `DSPkg::set_bin_packing_enabled(false)` for backwards compatibility or specialized workloads where total chunk isolation is required. |

TOC encryption is not an add-on option. It is decided entirely by the same preset's **Encryption** tab (`Project -> Export... -> Windows Desktop -> Encryption`), from **Encrypt Exported PCK**, **Encryption Key (256-bits as hexadecimal):**, and the **Filters to include files/folders** / **Filters to exclude files/folders** pair. With all four set correctly the exporter writes a v4 archive with an encrypted TOC and delivers a random archive key inside the encrypted PCK; with PCK encryption off the archive is plaintext; with PCK encryption requested but the key undeliverable the export fails closed (no archive, no payload in the PCK, and the player then fails with `DSPKG_BOOT status=fail reason=missing_archive`).

---

## 6. Version 4 Archives & Encryption

The `.dspkg` pipeline supports Version 4 archives featuring table-of-contents structure encryption and authentication:

- **TOC Encryption & Authentication**: The archive entry table (names, dimensions, offsets) is encrypted and authenticated with AEAD (BLAKE2b key-id derivation, XChaCha20, Poly1305 MAC via Monocypher).
- **Payload blocks stay unencrypted** so DirectStorage can hand the compressed GDeflate streams straight to the GPU decompressor. Payload bytes are therefore not secret and can be recovered from the archive without a key.
- **Key delivery**: the exporter generates a random 32-byte archive key per export and injects it as the 48-byte PCK entry `res://.dspkg/toc.key` (16-byte derived magic + 32-byte key) inside the encrypted pack. The extension DLL holds no archive key, and the shipped game needs no key call: `DSPkg.open()` acquires and validates the entry itself.
- **Archive id pairing**: each export also publishes a random 64-bit archive id, compared in full when the archive is opened; an archive whose header id is `0` is rejected by a paired build. The PCK and the archive must ship as a pair from the same export.
- **Security Notice**: Avoid saving plaintext encryption keys in export preset configuration files if the preset is committed to public source control. The PCK key lives in `.godot/export_credentials.cfg`, which is gitignored and therefore does not travel with the repository.

---

## 7. Publishing for Best Protection

The strongest configuration this add-on supports today is a Godot template built with your PCK key plus an encrypted PCK that carries the archive key. It is a sequence, because each step consumes the previous one's output.

### 7.1 The PCK dependency

The archive key is only as private as the pack that carries it. If the PCK is not encrypted, the key entry is readable and the archive can be opened by anyone holding the pack. There is no way for the add-on to protect the key without an encrypted PCK, and the add-on cannot encrypt the PCK for you: that is Godot's export step, driven by the preset and by the key compiled into the templates.

So:

1. Generate a 64-hex PCK key and keep it out of the repository.
2. Build the Godot editor/templates with that key (`SCRIPT_AES256_ENCRYPTION_KEY`). A stock template cannot mount a custom-key pack.
3. Set the key in the export preset (stored in `.godot/export_credentials.cfg`).
4. Set **Filters to include files/folders** on the Encryption tab to cover `res://.dspkg/toc.key`.
5. Export.
6. Boot the exported player and run the pack scan.

### 7.2 Turning on PCK encryption alone protects nothing

`encrypt_pck` on its own **encrypts nothing**. Godot decides per file from the include and exclude globs, so with an empty include filter the injected key entry would land in the pack unencrypted. The exporter checks the preset before it injects the key and refuses to deliver protection when the filter misses: the outcome is `failed`, which writes no archive and leaves no payload in the PCK. It cannot stop Godot from writing a pack, so the scan is the check on the result.

Set **Filters to include files/folders** to `.dspkg/*` (or any glob that matches `res://.dspkg/toc.key`) and leave **Filters to exclude files/folders** empty.

### 7.3 Boot the exported player

```powershell
<export_dir>\my_game.exe --headless -- --dspkg-validate=protected
```

Require exactly one line:

```text
DSPKG_BOOT status=ok mode=protected entries=<n> key_id=<16 hex>
```

`mode` is the mode you asked to validate; the validate switch on its own turns the line on (a normal successful boot prints nothing). `key_id` is the evidence — `none` means no key was committed. A missing line is a failure; the exit code alone is not a verdict. Add `DSPKG_NO_ALERT=1` if the runner has a display, so no dialog can block it. The switches, the field meanings, the failure reasons and the CI shape are in [docs/verifying-an-exported-build.md](docs/verifying-an-exported-build.md); those checks apply to any export, bundled or not, encrypted or not.

### 7.4 Scan the pack

In the editor: **Project > Tools > Verify exported pack (DSPkg)**, then pick the `.pck` next to the game
executable. The same scan from a terminal or CI:

```powershell
godot --headless --path project --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack "<export_dir>/my_game.pck"
```

Require:

```text
DSPKG_SCAN status=ok entries=<n> file=<path>
```

The scanner validates the pack header and searches the raw bytes for the 16-byte key magic. `reason=key_magic_found` means the key entry shipped in the clear. A relative `--pack` path is resolved against the `project/` directory that `--path` selects, so pass an absolute path when the pack lives elsewhere.

The editor dialog also reports `Directory encrypted: yes/no`. `status=ok` does not imply encryption — a plaintext pack passes the same scan — so check that line when the build was meant to be protected. The command line keeps its grammar unchanged for automation.

### 7.5 Deployment checklist

- [ ] Templates built with the same PCK key as the preset (and rebuilt after any key change).
- [ ] `encrypt_pck` on, key set, include filter covering `res://.dspkg/toc.key`.
- [ ] Export log shows the protected outcome, not plaintext and not a fail-closed error.
- [ ] Exactly one `DSPKG_BOOT status=ok mode=protected … key_id=<16 hex>` line from the exported player.
- [ ] `DSPKG_SCAN status=ok` on the shipped pack.
- [ ] `my_game.exe`, `my_game.pck`, `textures.dspkg`, and the extension DLLs deployed together.
- [ ] `dsgd_64.dll` reports `dspkg-key-id=none` (the shipped binaries carry no key in the DLL).

### 7.6 Patches: the PCK and the archive are a pair

Both the archive key and the archive id are generated per export. Any patch that rebuilds `my_game.pck` must redistribute `textures.dspkg` from the same export, even when no texture changed, because the pack's key entry and the archive's id must match. Shipping one without the other produces `DSPKG_BOOT status=fail reason=archive_pairing_mismatch` (stale archive) or `reason=archive_unauthorized` (missing key).

Unchanged textures keep identical payload bytes, so the archive delta stays small; only the header and encrypted table of contents change.

### 7.7 What this does not give you

The payload bytes inside the archive are compressed GDeflate streams, not encrypted data. Someone who knows the format can recover them without any key. The protection covers the archive's table of contents and the delivery of its key; it does not cover the texture data itself, and the boot sentinel and scanner are diagnostics rather than enforcement. The user-facing guide with the permitted wording is [`docs/encryption-guide.md`](docs/encryption-guide.md); the two verification checks are in [`docs/verifying-an-exported-build.md`](docs/verifying-an-exported-build.md); the internal contract is [`.agents/guides_and_docs/encryption-key-handling.md`](.agents/guides_and_docs/encryption-key-handling.md).
