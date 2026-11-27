# Building and Testing from Source

This guide covers contributor build prerequisites, fetching the EULA-bound DirectStorage SDK, running native `gdextest` suites, local vs. remote builds, and deploying compiled artifacts.

---

## 1. Prerequisites

The extension is built with SCons against the vendored SDKs, so there is no package to install for DirectStorage itself beyond fetching the pinned Microsoft runtime/import-lib NuGet package.

| Requirement | Version / notes |
| :--- | :--- |
| Windows | Windows 10/11 x64 for the DirectStorage path. The same sources also build on Linux/macOS, where they compile to the software fallback backend only. |
| Python | 3.8 or newer on `PATH` (3.10 verified). SCons and the `godot-cpp` binding generator run on it. |
| SCons | **4.11 or newer**: `python -m pip install --upgrade "scons>=4.11"` (verified with 4.11.1). Older releases such as 4.6 do not know Visual Studio 2026 (VS 18 / MSVC 14.5), silently fall back to MinGW, and fail at the first compile with `x86_64-w64-mingw32-gcc` not found. Check with `scons --version`. |
| MSVC C++ toolset | Visual Studio 2022 or 2026 **Build Tools** with the "Desktop development with C++" workload (MSVC 14.50 verified). Only the C++ toolset and the Windows SDK are needed — the IDE is not. |
| Windows SDK | A 10.x SDK providing D3D12 and DXGI (`d3d12.h`, `dxgi.lib`; `dxgi.h` lives in the SDK's `shared` include directory). 10.0.26100 verified. |
| Git | Required: submodules, plus the commit stamp written into `build_manifest.json`. |
| Godot (tests and export only) | 4.7.2-stable **console** executable, pointed at by `GODOT_CONSOLE_PATH` in `.env`. The `.gdextension` manifest declares `compatibility_minimum = "4.3"`. For custom engine builds or building from source, D3D12 must be enabled (`d3d12=yes`); see [Custom Godot Builds](../custom-godot-builds.md). |

### Vendored in the repository

- `thirdparty/directstorage/include/` — DirectStorage SDK headers (`dstorage.h`, `dstorageerr.h`), MIT-licensed (see `thirdparty/directstorage/LICENSE-CODE.txt`).
- `thirdparty/gdeflate/`, `thirdparty/monocypher/` - compression and crypto sources compiled into the DLL.

### Submodules

- `thirdparty/libdeflate/` - submodule (`medium-effort/libdeflate`), compiled into the DLL for CPU GDeflate decompression.
- `godot-cpp/` — submodule. Its static library is compiled into `godot-cpp/bin/` by the same SCons run as the extension, so no prebuilt bindings are required.
- `extern/gdextest/` — submodule, needed only for the native test suite.

### Fetched once per checkout (EULA-bound, not committed)

`thirdparty/directstorage/lib/x64/dstorage.lib`, `thirdparty/directstorage/bin/x64/dstorage.dll`, and `dstoragecore.dll` come from the pinned official NuGet package. The package's `distributable_files.txt` lists only the two runtime DLLs, and the import library is not in that list, so neither is redistributed from this repository.

Run the fetch before the first Windows build:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1
```

It verifies Microsoft's published SHA-512, shows the exact package `LICENSE.txt` in a separate PowerShell window, and records accept/decline per version in `.cache/directstorage/licenses.json`. In VS Code, run the **Fetch DirectStorage Runtime** task and pick `1.3.0` (build baseline) or `1.4.0-preview2-2606.904`. A declined license installs nothing; `-ResetLicense` asks again, and `-Decision Accept` is the explicit CI seam.

`SCons` and `.vscode/build_remote.ps1` only check that the import library exists and fail closed with that fetch command. They never download the package or accept the license on your behalf. The runtime DLLs are optional for linking: without them the build succeeds and the DirectStorage test suites skip. A new worktree seeds `thirdparty/directstorage/{lib,bin}` from the primary through its pre-start hook, so it does not fetch again.

---

## 2. First Build & `build_manifest.json`

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
| `build_manifest.json` | build status, commit, size, `sha256`, source snapshot id, and the `dspkg-key-id` tag of each binary |

`build_manifest.json` also records, per library, a `sha256` and a `source_id` (`HEAD` plus the working-tree diff of the paths that compile into a binary). At load time the extension compares the debug and release entries through those fields and prints one `[DirectStorage] Binary mismatch` warning when they came from different source states: the editor runs the debug binary while an export runs the release one, so a stale release binary is otherwise only visible after exporting. The check is silent when the manifest is missing, when an entry predates these fields, or when a file no longer matches its recorded hash — it never fails a boot and never guesses.

The default build carries no archive key in the DLL (`dspkg-key-id=none`): every export generates a fresh key and delivers it inside the encrypted PCK. A key compiled into the DLL (`DSPKG_ENCRYPTION_KEY_FILE`) is the legacy path for experiments only — the build audit rejects it in editor, `template_debug`, and `template_release` binaries.

---

## 3. Native Tests (`gdextest`)

The native suite is built from `tests/` plus the sources under test, and needs the Godot console executable:

```powershell
powershell -ExecutionPolicy Bypass -File .vscode/build_remote.ps1 -Local -IncludeTests -Target template_debug
powershell -ExecutionPolicy Bypass -File .vscode/run_gdextest_local.ps1
```

`-IncludeTests` builds the `gdextest` library; the second script runs the doctor/fixture/warmup/headless-report pipeline without rebuilding. Windowed D3D12 suites (`.vscode/run_d3d12_windowed.ps1`) additionally exercise the live `RenderingDevice`.

---

## 4. Local vs. Remote Builds (`.env`)

Pipeline behaviour is read from `.env` (gitignored). Copy [`.env.example`](../../.env.example) and adjust:

- `REMOTE_BUILD` — `0` compiles locally; `1` offloads the build to `REMOTE_HOST` over SSH/SCP.
- `PREVENT_BUILD_ON_THIS_DEVICE` — when `1`, `SConstruct` refuses to compile on this machine so a workstation cannot silently produce a different binary. `build_remote.ps1 -Local` skips the SSH leg but still honours this guard; clear the flag, or set it to `0` in the environment for a single run, to build locally.
- `GODOT_CONSOLE_PATH` — source of the executable used by the test and export-verification runners.
- `SYNC_TEST_DIR`, `SYNC_ADDON_COPYABLE_DIR`, `SYNC_ADDON_DIR` — optional deployment targets; each is skipped unless its path is defined.

---

## 5. Deploying a Build

Building and deploying are separate steps. `SCons` only produces artifacts: the flat `dsgd_64*.dll` binaries plus `build_manifest.json` in `project/addons/directstorage_accelerator/bin/`, next to the DirectStorage runtime DLLs it copies from `thirdparty/directstorage/bin/x64/`. Running `scons` by hand therefore never writes into another project.

Deployment is one implementation, [`tools/sync_artifacts.py`](../../tools/sync_artifacts.py), invoked by the pipeline:

- `.vscode/build_remote.ps1` runs it after a successful build — locally, and for a remote build after the artifacts are pulled back — so the mirror is always built from the local `bin/`.
- `.vscode/sync_addon.ps1` runs the same code without building: `-Targets all` for every configured mirror, `-Targets copyable`, or `-DryRun` to preview (the "Sync Addon to Test Project (no build)" VS Code task).

Only loadable modules and the manifest are mirrored (`bin/` outside those stays local), and `build_manifest.json` is patched once with the sync result before being copied to each target.

Because the copy is flat, `SYNC_ADDON_DIR` must name the addon directory itself. A target that looks like a project root (`project.godot` present) or an addons folder (other plugins, no addon marker of its own) is refused with a warning and nothing is copied, rather than scattering the addon across a project. Files the addon no longer ships are reported as a count; `--verbose` lists them.

---

## 6. Native Library Reload (GDExtension)

The native half (`bin/dsgd_64*.dll`) is a GDExtension library, so it can be reloaded without restarting the editor. In Godot 4.7+, open **Project Settings > GDExtension** and press the reload button on the `directstorage_accelerator.gdextension` row. The editor also reloads it automatically when you return to the window after rebuilding, and only then - saving the file is not enough.

A reload unloads and reopens the library, so the addon's native singletons (`DSContext`, `DSErrorHandler`, `DSProfiler`, `DSTextureCache`) are rebuilt from scratch; the editor plugin re-initializes the DirectStorage backend as soon as that happens.

- On Windows the editor runs the library from a hidden copy beside it (`bin/~dsgd_64.debug.dll`), which is why rebuilding over a running editor works. The DirectStorage runtime DLLs (`dstorage.dll`, `dstoragecore.dll`) are not copied, so switching runtime versions still needs a staged editor restart.
- A failed reload (half-written build, missing dependency) leaves the extension unloaded until the next successful reload or an editor restart. The button ignores the result, so check the Output panel. An extension that never loaded at editor start is not listed in that tab at all.
- A reload cannot re-initialize an extension whose minimum initialization level is below the scene level; this addon initializes at scene level, so it reloads cleanly.
- Two copies of the library in the same project cannot both own the DirectStorage classes. The plugin warns at startup with both paths when it detects that, and with the expected manifest when the native half is missing altogether.
