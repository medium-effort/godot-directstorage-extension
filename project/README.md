# dsgd Demo Project

This directory (`project/`) is the **dsgd Demo** Godot 4.7 project (`project.godot` already configures `rendering_device/driver.windows = "d3d12"`, enables the `directstorage_accelerator` plugin, and registers the `DSRuntime` autoload).

## Prerequisites

1. **Compiled GDExtension binary** in `addons/directstorage_accelerator/bin/` (see [Building & Testing from Source](../docs/for_developers/building-and-testing.md)).
2. **DirectStorage runtime DLLs (Windows)**: Open **Project > Tools > dsgd Tools... (DirectStorage) > Manage DirectStorage Runtime...** to download and install `dstorage.dll` and `dstoragecore.dll` (the dialog opens automatically on Windows when enabling the plugin if `dstoragecore.dll` is missing).

---

## 1. Quick A/B Benchmark (`res://main.tscn`)

Press **F5** (Play Project) to run the default scene (`main.tscn`).

- Requires **no manual setup**: on first run it generates and caches 5 passes x 3 procedural textures (2048x2048 RGBA8) under `user://ds_ab_bench/` (outside `res://`) and compares DirectStorage `.dstex` loading against standard Godot `.res` CPU loading.
- Includes a button to switch to the **5-Mode Loading Screen Demo** (`res://demo/loading_demo_menu.tscn`).

---

## 2. 5-Mode Loading Screen Demo (`res://demo/loading_demo_menu.tscn`)

Compares five loading strategies on a procedural 3D arena level (48 x 2048x2048 textures + 1 dedicated 2048x2048 normal map):

- **Vanilla Load (Sync)** - [`demo/loaders/vanilla_sync_loader.gd`](demo/loaders/vanilla_sync_loader.gd)
- **Vanilla Optimized (Threaded)** - [`demo/loaders/vanilla_threaded_loader.gd`](demo/loaders/vanilla_threaded_loader.gd)
- **DS (CPU Prewarm)** - [`demo/loaders/ds_prewarm_loader.gd`](demo/loaders/ds_prewarm_loader.gd)
- **DS (GPU Prewarm)** - [`demo/loaders/ds_prewarm_loader.gd`](demo/loaders/ds_prewarm_loader.gd)
- **DS (GPU Prewarm + Threaded Scene)** - [`demo/loaders/ds_gpu_paced_loader.gd`](demo/loaders/ds_gpu_paced_loader.gd)

Demo assets are generated procedurally into gitignored `res://demo_generated/` (`vanilla/` and `ds/`) so large binary textures are not committed to Git.

### Setup Option A: Godot Editor (Recommended)

1. Stop any running game instance first (avoids Windows file locks during texture reimport).
2. Open [`demo/editor_scripts/generate_demo_assets_tool.gd`](demo/editor_scripts/generate_demo_assets_tool.gd) (`res://demo/editor_scripts/generate_demo_assets_tool.gd`) in the Godot Script Editor.
3. Press **Ctrl+Shift+X** (**File > Run**).

A cancellable progress dialog generates all 48 textures plus the normal map into `res://demo_generated/vanilla/` and `res://demo_generated/ds/`, temporarily releases `DSTextureCache` / `DSContext` handles, triggers an editor filesystem scan to import both `.ctex` and `.dstex` variants, re-initializes `DSContext`, and builds `demo_level_vanilla.scn` and `demo_level_ds.scn`.

Once the Output panel prints `[demo_tool] Demo assets ready in res://demo_generated.`, run `res://demo/loading_demo_menu.tscn` (or click the Loading Demo button in `res://main.tscn`).

### Setup Option B: Headless CLI

Run from the repository root (generate PNGs and `.import` sidecars, import into `.godot/imported/`, then build the `.scn` levels):

```powershell
godot --headless --path project --script res://demo/generate_assets_cli.gd -- --textures
godot --headless --path project --editor --import
godot --headless --path project --script res://demo/generate_assets_cli.gd -- --scenes
```

### Cleaning Up Generated Assets

Open [`demo/editor_scripts/clear_demo_assets_tool.gd`](demo/editor_scripts/clear_demo_assets_tool.gd) (`res://demo/editor_scripts/clear_demo_assets_tool.gd`) in the Script Editor and press **Ctrl+Shift+X** (**File > Run**) to delete `res://demo_generated/` and rescan the filesystem.

### Editor vs. Exported Runs

Running from the editor reads loose `.dstex`/`.ctex` files rather than `textures.dspkg`, and repeat Vanilla runs hit Windows' warm OS page cache. Export with the Windows Desktop preset (D3D12) to exercise the full `textures.dspkg` streaming path.

---

## 3. Other Directories in `project/`

- [`addons/directstorage_accelerator/`](addons/directstorage_accelerator/README.md) - the addon itself ([Addon Reference](addons/directstorage_accelerator/README.md))
- `benchmark/` - standalone GPU contention, chunked upload, CPU prewarm, and backend-mode benchmark scripts
- `tests/` - GUT unit tests and export-contract release gates (see [Building & Testing from Source](../docs/for_developers/building-and-testing.md))
