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

The extension provides configurable project settings in Godot under the `dsgd_extension/` namespace (`Project -> Project Settings... -> General -> Dsgd Extension`):

| Setting | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `dsgd_extension/staging_buffer_size_mb` | `int` | `32` | Staging buffer size in megabytes allocated by DirectStorage for unbuffered DMA transfers (editor range: `16`-`2048` MiB). Chunked textures bound a request at one chunk (8 MiB by default), so the default is sized for that; raise it only for legacy un-chunked assets whose mip exceeds the buffer. |
| `dsgd_extension/disable_gpu_decompression` | `bool` | `false` | When `true`, disables D3D12 GPU compute decompression and forces fallback to multi-threaded CPU software decompression. Useful for debugging, compatibility testing, or profiling. |
| `dsgd_extension/disable_gpu_decompression_metacommand` | `bool` | `false` | When `true`, keeps GPU decompression enabled but forces the reference implementation instead of the driver's optimized metacommand. Independent of `disable_gpu_decompression`, so the two GPU paths can be compared on the same machine; use it when the optimized path misbehaves or measures slower. Applies to the next DirectStorage initialization. |
| `dsgd_extension/queue_capacity` | `int` | `512` | Maximum request capacity allocated for the hardware DirectStorage queue (editor range: `128`–`8192`). Dictates the upper bound of simultaneous in-flight storage requests. |
| `dsgd_extension/auto_tune_for_integrated_gpu` | `bool` | `true` | When `true`, lowers the effective staging buffer (to 64 MiB), queue (to 256), and `texture_cache_max_entries` (to 128) on integrated/UMA GPUs, and routes GPU prewarms to System RAM. Only ever lowers values, never raises them. |
| `dsgd_extension/auto_tune_for_low_vram_gpu` | `bool` | `true` | Player-hardware VRAM guardrail for discrete GPUs. When `true`, caps `texture_cache_max_entries` (`128` under 4 GiB, `256` on 4–8 GiB) and clamps any project-raised `staging_buffer_size_mb` (`32` MiB under 2 GiB, `64` MiB under 8 GiB, since up to 4 internal staging buffers multiply residency by ~4x). Untouched at 8 GiB and above; only ever lowers values, never raises them. |
| `dsgd_extension/texture_cache_enabled` | `bool` | `true` | LRU residency cache (`DSTextureCache`). `true`: `REUSE` loads hit the cache and become engine-resident, so post-prewarm scene loads bind pointer-identical textures with no disk read, and prewarm works. `false`: every load re-reads and re-decompresses and the texture dies with its last reference — GPU decompression still works, but async prewarm refuses to start and existing entries are cleared. Prefer lowering `texture_cache_max_entries` over disabling. |
| `dsgd_extension/texture_cache_max_entries` | `int` | `512` | Maximum number of texture entries held in the LRU cache (editor range: `16`–`2048`). Auto-tune reduces it to `128` on integrated GPUs and discrete GPUs under 4 GiB, and to `256` on 4–8 GiB discrete GPUs; 8 GiB and above keep the full value. When the limit is reached, oldest unreferenced textures are evicted to reclaim VRAM. A prewarm batch may temporarily hold up to 25% more entries (at least one) so it does not evict its own waiting textures; beyond that bounded cap the oldest unconsumed prewarm entry is evicted, and the over-capacity warning is emitted once. |
| `dsgd_extension/shutdown_drain_timeout_ms` | `int` | `10000` | Maximum time in milliseconds (clamped to a minimum of `100` ms) to wait during application exit or module shutdown for active NVMe DMA and GPU requests to drain before fail-safe cleanup, preventing device-removed crashes or driver hangs. |

---

## 5. Export Preset Options Reference

When configuring a Windows Desktop export preset (`Project -> Export... -> Windows Desktop -> Options`), the following custom options are provided under the `dsgd_extension/` category:

| Export Option | Type | Default | Description |
| :--- | :---: | :---: | :--- |
| `dsgd_extension/bundle_textures_dspkg` | `bool` | `true` | When `true`, all exported `.dstex` textures are extracted from the `.pck` via `skip()` and bundled into a standalone DirectStorage archive (`textures.dspkg`) deployed alongside the executable for unbuffered NVMe DMA loading. When `false`, texture payloads remain embedded inside the `.pck` and fall back to CPU software decompression at runtime. |
| `dsgd_extension/verify_pack_on_export` | `bool` | `true` | When `true`, after a protected standalone `.pck` export completes, reads the written `.pck` back and verifies its header, non-empty directory, and absence of the plaintext key-file magic (`PACK_VERIFY status=ok`); embedded PCKs print `status=unverified` with a warning. When `false`, skips the scan (`PACK_VERIFY status=skipped reason=disabled_by_setting`) with a warning and makes no verification claim. |
| `dsgd_extension/enforce_archive_pairing` | `bool` | `true` | When `true`, publishes a random 64-bit archive id per export and writes the same value into the archive header; at runtime the archive is opened with that id and a mismatch fails the boot instead of loading a stale or foreign archive. An archive whose header id is `0` is rejected by such a build. When `false`, the published id is `0`, which skips the check in that build (local tooling only). |
| `dsgd_extension/steampipe_1mb_alignment` | `bool` | `false` | When `true`, packs texture payloads inside `textures.dspkg` into 1 MiB (`1,048,576` bytes) chunks with 512-byte GPU sub-alignment, padding to 1 MiB boundaries only when a texture would cross a chunk boundary. This follows Valve's best practice for SteamPipe, keeping delta patch downloads isolated to modified 1 MiB chunks while eliminating wasted file-size of zero-padding compared to strict per-texture alignment. Note: strict 1 MiB per-texture alignment is available via `DSPkg::set_bin_packing_enabled(false)` for backwards compatibility or specialized workloads where total chunk isolation is required. |
| `dsgd_extension/include_dsgd_license_file` | `bool` | `true` | Writes `dsgd_LICENSE.txt` next to the exported binaries, containing the project license (`LICENSE`) and the third-party components of `dsgd_64.dll` (`THIRD_PARTY_NOTICES.txt`). Disable when you carry these attributions in an in-game credits screen instead; the source texts stay available in the addon folder. |
| `dsgd_extension/include_directstorage_license_file` | `bool` | `true` | Writes Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt` for the active runtime version. Disable when you carry those texts in-game or handle them in your own distribution files. |
| `dsgd_extension/merge_license_files` | `bool` | `true` | When both license files are included, appends the DirectStorage runtime texts to `dsgd_LICENSE.txt` and skips the separate `DIRECTSTORAGE_LICENSE.txt`. Merge has no effect when fewer than two license files are included. |

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
- [ ] The export-contract gate passes on this machine: `project/headless_tests/export_contract/run_export_contract.ps1`
      ends with `RESULT PASS`, `cases=24/24`, `not_covered=none`, `not_run=none` and only the two `known_limits=`.
      It is manual - no CI job runs it - so this step is only done if you do it. A run that did not execute every
      case now fails instead of reporting PASS; `-AllowPartial` is the deliberate opt-out for debugging one case.
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

---

## 8. Building from Source

### 8.1 Prerequisites

The extension is built with SCons against the vendored SDKs, so there is no package to install for DirectStorage itself.

| Requirement | Version / notes |
| :--- | :--- |
| Windows | Windows 10/11 x64 for the DirectStorage path. The same sources also build on Linux/macOS, where they compile to the software fallback backend only. |
| Python | 3.8 or newer on `PATH` (3.10 verified). SCons and the godot-cpp binding generator run on it. |
| SCons | **4.11 or newer**: `python -m pip install --upgrade "scons>=4.11"` (verified with 4.11.1). Older releases such as 4.6 do not know Visual Studio 2026 (VS 18 / MSVC 14.5), silently fall back to MinGW, and fail at the first compile with `x86_64-w64-mingw32-gcc` not found. Check with `scons --version`. |
| MSVC C++ toolset | Visual Studio 2022 or 2026 **Build Tools** with the “Desktop development with C++” workload (MSVC 14.50 verified). Only the C++ toolset and the Windows SDK are needed — the IDE is not. |
| Windows SDK | A 10.x SDK providing D3D12 and DXGI (`d3d12.h`, `dxgi.lib`; `dxgi.h` lives in the SDK’s `shared` include directory). 10.0.26100 verified. |
| Git | Required: submodules, plus the commit stamp written into `build_manifest.json`. |
| Godot (tests and export only) | 4.7.2-stable **console** executable, pointed at by `GODOT_CONSOLE_PATH` in `.env`. The `.gdextension` manifest declares `compatibility_minimum = "4.3"`. For custom engine builds or building from source, D3D12 must be enabled (`d3d12=yes`); see [`docs/custom-godot-builds.md`](docs/custom-godot-builds.md). |

Vendored and already in the repository — nothing to download or install:

- `thirdparty/directstorage/include/` — DirectStorage SDK headers (`dstorage.h`, `dstorageerr.h`), MIT-licensed (see `thirdparty/directstorage/LICENSE-CODE.txt`).
- `thirdparty/gdeflate/`, `thirdparty/libdeflate/`, `thirdparty/monocypher/` — compression and crypto sources compiled into the DLL.
- `godot-cpp/` — submodule. Its static library is compiled into `godot-cpp/bin/` by the same SCons run as the extension, so no prebuilt bindings are required.
- `extern/gdextest/` — submodule, needed only for the native test suite.

Fetched once per checkout — EULA-bound, not committed:

- `thirdparty/directstorage/lib/x64/dstorage.lib`, `thirdparty/directstorage/bin/x64/dstorage.dll` and `dstoragecore.dll` come from the pinned official NuGet package. The package's `distributable_files.txt` lists only the two runtime DLLs, and the import library is not in that list, so neither is redistributed from this repository.
- Run the fetch before the first Windows build:
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1
  ```
  It verifies Microsoft's published SHA-512, shows the exact package `LICENSE.txt` in a separate PowerShell window, and records accept/decline per version in `.cache/directstorage/licenses.json`. In VS Code, run the **Fetch DirectStorage Runtime** task and pick `1.3.0` (build baseline) or `1.4.0-preview2-2606.904`. A declined license installs nothing; `-ResetLicense` asks again, and `-Decision Accept` is the explicit CI seam.
- `SCons` and `.vscode/build_remote.ps1` only check that the import library exists and fail closed with that fetch command. They never download the package or accept the license on your behalf. The runtime DLLs are optional for linking: without them the build succeeds and the DirectStorage test suites skip.
- A new worktree seeds `thirdparty/directstorage/{lib,bin}` from the primary through its pre-start hook, so it does not fetch again.

### 8.2 First build

```powershell
git submodule update --init --recursive
python -m pip install --upgrade "scons>=4.11"
powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1

scons platform=windows target=template_debug
scons platform=windows target=template_release
```

Add `dev_build=yes` to a `template_debug` build for the development variant with extra checks. Outputs land in `project/addons/directstorage_accelerator/bin/`:

| Artifact | Purpose |
| :--- | :--- |
| `dsgd_64.debug.dll` | editor and `template_debug` libraries |
| `dsgd_64.dll` | `template_release` library |
| `dstorage.dll`, `dstoragecore.dll` | copied from the fetched SDK next to the DLL |
| `build_manifest.json` | build status, commit, size, `sha256`, source snapshot id and the `dspkg-key-id` tag of each binary |

`build_manifest.json` also records, per library, a `sha256` and a `source_id` (HEAD plus the working-tree diff of the paths that compile into a binary). At load time the extension compares the debug and release entries through those fields and prints one `[DirectStorage] Binary mismatch` warning when they came from different source states: the editor runs the debug binary while an export runs the release one, so a stale release binary is otherwise only visible after exporting. The check is silent when the manifest is missing, when an entry predates these fields, or when a file no longer matches its recorded hash — it never fails a boot and never guesses.

The default build carries no archive key in the DLL: every export generates a fresh key and delivers it inside the encrypted PCK. A key compiled into the DLL is the legacy build with a baked key (`DSPKG_ENCRYPTION_KEY_FILE`), intended for experiments only — the build audit rejects it in editor, `template_debug` and `template_release` binaries.

### 8.3 Tests (optional)

The native suite is built from `tests/` plus the sources under test, and needs the Godot console executable:

```powershell
powershell -ExecutionPolicy Bypass -File .vscode/build_remote.ps1 -Local -IncludeTests -Target template_debug
powershell -ExecutionPolicy Bypass -File .vscode/run_gdextest_local.ps1
```

`-IncludeTests` builds the gdextest library; the second script runs the doctor/fixture/warmup/headless-report pipeline without rebuilding. Windowed D3D12 suites (`run_d3d12_windowed.ps1`) additionally exercise the live RendererDevice.

### 8.4 Local versus remote builds

Pipeline behaviour is read from `.env` (gitignored). Copy [`.env.example`](.env.example) and adjust:

- `REMOTE_BUILD` — `0` compiles here; `1` offloads the build to `REMOTE_HOST` over SSH/SCP.
- `PREVENT_BUILD_ON_THIS_DEVICE` — when `1`, `SConstruct` refuses to compile on this machine so a workstation cannot silently produce a different binary. `build_remote.ps1 -Local` skips the SSH leg but still honours this guard; clear the flag, or set it to `0` in the environment for a single run, to build locally.
- `GODOT_CONSOLE_PATH` — source of the executable used by the test and export-verification runners.
- `SYNC_TEST_DIR`, `SYNC_ADDON_COPYABLE_DIR`, `SYNC_ADDON_DIR` — optional deployment targets; each is skipped unless its path is defined.

### 8.5 Deploying a build

Building and deploying are separate steps. `SCons` only produces artifacts: the flat `dsgd_64*.dll` binaries plus `build_manifest.json` in `project/addons/directstorage_accelerator/bin/`, next to the DirectStorage runtime DLLs it copies from the fetched `thirdparty/directstorage/bin/x64/`. Running `scons` by hand therefore never writes into another project.

Deployment is one implementation, [`tools/sync_artifacts.py`](tools/sync_artifacts.py), invoked by the pipeline:

- `.vscode/build_remote.ps1` runs it as step `[6/6]` after a successful build — locally, and for a remote build after the artifacts are pulled back — so the mirror is always built from the local `bin/`.
- `.vscode/sync_addon.ps1` runs the same code without building: `-Targets all` for every configured mirror, `-Targets copyable`, or `-DryRun` to preview. This is the "Sync Addon to Test Project (no build)" task.

Only loadable modules and the manifest are mirrored (`bin/` outside those stays local), and the build manifest is patched once with the sync result before being copied to each target. The former `scons sync*` aliases are gone: a build tool should not be copying files into other repositories.

Because the copy is flat, `SYNC_ADDON_DIR` must name the addon directory itself. A target that looks like a project root (`project.godot` present) or an addons folder (other plugins, no addon marker of its own) is refused with a warning and nothing is copied, rather than scattering the addon across a project. Files the addon no longer ships are reported as a count; `--verbose` lists them.

---

## 9. Trademarks and Attribution

### License

`dsgd` is licensed under the **Mozilla Public License, v. 2.0 (MPL-2.0)** (`LICENSE`), except for the four shipped runtime GDScript helpers (`ds_runtime.gd`, `ds_preloader.gd`, `ds_batch_manager.gd`, and `ds_dspkg_static.gd`), which are licensed under the **MIT License** (`LICENSE-MIT`) so games can freely localize, tune, or adapt them.

- **Closed-source and commercial games are permitted:** You can bundle `dsgd` with proprietary games without disclosing your game's own code or assets; only modifications made to `dsgd`'s own MPL-2.0 files must be shared under MPL-2.0.
- **Export compliance (MPL-2.0 Section 3.2(a)):** Keep `dsgd_extension/include_dsgd_license_file` enabled (default) so `dsgd_LICENSE.txt` is written beside exported binaries. If you disable it to display attributions in-game, include the MPL-2.0 license notice, the DSGD source URL (`https://github.com/medium-effort/godot-directstorage-extension`), and the attributions from `THIRD_PARTY_NOTICES.txt` (plus Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt` when shipping `dstorage.dll` / `dstoragecore.dll`).

### Trademarks and Notices

This project (DSGD) is an independent project and is not affiliated with, endorsed by, or sponsored by Microsoft. Microsoft and DirectStorage are trademarks of the Microsoft group of companies. The DirectStorage SDK and runtime are used under Microsoft's license and are not redistributed from this repository; see Section 8.1.

Third-party license texts and attributions are in `THIRD_PARTY_NOTICES.txt` at the repository root; the project's own licenses are in `LICENSE` (MPL-2.0) and `LICENSE-MIT` (MIT). The build copies them into the addon folder, and the exporter deploys them next to exported binaries as `dsgd_LICENSE.txt` (with the DirectStorage runtime texts appended when merging is enabled) or as a separate `DIRECTSTORAGE_LICENSE.txt`. These files are exporter-managed: every export removes them and writes the set selected by the preset, so disabling an option deletes the previous export's copy.
