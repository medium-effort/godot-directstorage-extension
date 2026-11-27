# Exporting Your Project

This guide covers standard project exports with `dsgd`: how `.dstex` textures are packaged into `textures.dspkg`, what ships in the export directory across Windows, Linux, and macOS, when to adjust export defaults, and how to run a quick sanity check.

- For the full option-by-option table of `dsgd_extension/*` export toggles, see the [Addon Reference](../project/addons/directstorage_accelerator/README.md#export-preset-options).
- For encrypted archives (PCK + TOC encryption with custom keyed Godot templates), see the [Encryption Guide](encryption-guide.md).
- For automated/CI boot verification and pack scanning, see [Verifying an Exported Build](verifying-an-exported-build.md).

---

## Before You Export

Make sure the following are in place before exporting:

1. **Plugin enabled (`DSRuntime` autoload):** Keep the **dsgd - Unofficial DirectStorage binding** plugin enabled in **Project Settings > Plugins** so the `DSRuntime` autoload remains registered in `project.godot`. Exported builds depend on `DSRuntime` to locate and open `textures.dspkg` at startup and drive asynchronous prewarm.
2. **DirectStorage runtime installed (Windows):** On Windows, install `dstorage.dll` and `dstoragecore.dll` via **Project > Tools > dsgd Tools... (DirectStorage) > Manage DirectStorage Runtime...** before exporting. These DLLs and their license texts are not bundled in the repository; Godot's GDExtension exporter copies them from `addons/directstorage_accelerator/bin/` into the export directory automatically.
3. **Separate `.pck` export:** Use a standard separate `.pck` export (leave **Binary Format > Embed PCK** off). Full Embed PCK support is still on the roadmap, and protected embedded exports cannot be pack-verified automatically.

---

## Standard Export Walkthrough

The `dsgd Extension` export plugin attaches automatically to **Windows Desktop**, **Linux**, and **macOS** presets:

1. Open **Project > Export...** and create or select your desktop preset.
2. Under **Resources > Export Mode**, use **Export all resources in the project** (`all_resources`), which is the primary end-to-end tested mode. Subset modes (`scenes`, `resources`, `exclude`, dedicated server) work by archiving only the textures Godot exports, producing a partial archive that you should verify yourself.
3. Under **Options**, review the `dsgd_extension/*` toggles if you need non-default behavior. The defaults work out of the box for standard builds; see the [Addon Reference](../project/addons/directstorage_accelerator/README.md#export-preset-options) for every option.
4. Click **Export Project...**.

During export, the exporter automatically:

- Builds `textures.dspkg` via a staging file (`textures.dspkg.tmp`) directly in the target export directory (not `user://`), removes any previous `textures.dspkg` up front so a failed export never leaves a stale archive behind, and renames the staging file on completion.
- Removes bundled `.dstex` payloads from the `.pck` while preserving `.import` remap metadata so `ResourceLoader.load()` continues to resolve logical texture paths.
- Skips archive creation and marks the build self-contained from the `.pck` if no `.dstex` textures are exported.
- Copies the native library and (on Windows) the DirectStorage runtime DLLs beside the executable via `.gdextension`, and writes the selected license files.

---

## What Ships in the Export Directory

### Windows (Release)

```text
<export-dir>/
  game.exe
  game.pck
  textures.dspkg
  dsgd_64.dll              (dsgd_64.debug.dll for debug exports)
  dstorage.dll
  dstoragecore.dll
  dsgd_LICENSE.txt         (includes Microsoft DirectStorage notices when merge_license_files is on;
                            otherwise DIRECTSTORAGE_LICENSE.txt is written separately)
```

### Linux / macOS (Release)

```text
<export-dir>/
  game.x86_64              (or game.app on macOS)
  game.pck
  textures.dspkg
  libdsgd_64.so            (libdsgd_64.dylib on macOS; libdsgd_64.debug.* for debug exports)
  dsgd_LICENSE.txt
```

> [!IMPORTANT]
> **`game.pck` and `textures.dspkg` are a matched pair.** By default (`dsgd_extension/enforce_archive_pairing = true`), each export generates a fresh random 64-bit archive ID embedded in both `project.binary` and `textures.dspkg`. Always ship the executable, `.pck`, and `textures.dspkg` from the **same** export run; mixing files from different exports causes startup to fail with `archive_pairing_mismatch`.

---

## Common Export Choices

Defaults are tuned for typical desktop releases. Adjust the following in **Project > Export... > \<Preset\> > Options** only when your target requires it:

- **Cross-platform compression (`dsgd_extension/store_uncompressed_textures`):** Defaults to `false` on Windows (keeps GDeflate payloads for DirectStorage GPU decompression and smaller downloads) and `true` on Linux/macOS (repacks payloads uncompressed into a v5 `textures.dspkg`, or into the `.pck` when bundling is off, avoiding CPU GDeflate decompression overhead at load time). Turn it off on Linux/macOS if minimizing distribution size matters more than CPU load speed.
- **Steam & delta patching (`dsgd_extension/steampipe_1mb_alignment`):** Off by default. Enable it before shipping on Steam so payloads are bin-packed into 1 MiB chunks; editing one texture then only rewrites the chunks it occupies instead of shifting every payload behind it in `textures.dspkg`.
- **Disabling archive bundling (`dsgd_extension/bundle_textures_dspkg = false`):** Skips `textures.dspkg` completely and leaves `.dstex` files inside the `.pck`, loading them through CPU decompression at runtime. Useful for quick fallback/debugging builds or A/B comparisons.
- **License & attribution files (`include_dsgd_license_file`, `include_directstorage_license_file`, `merge_license_files`):** Enabled by default to satisfy MPL-2.0 Section 3.2(a) and Microsoft DirectStorage redistribution terms. The exporter removes and rewrites these files on every export so the directory always matches the active preset. If you disable them to display attributions in-game instead, include the MPL-2.0 license notice, the DSGD source URL (`https://github.com/medium-effort/godot-directstorage-extension`), the attributions from `THIRD_PARTY_NOTICES.txt`, and (on Windows) Microsoft's DirectStorage runtime `LICENSE.txt` and `NOTICES.txt`.

---

## Quick Sanity Check

Running the project from the Godot editor is **not** a substitute for testing an export: editor runs read loose `.dstex` files and skip fatal exported-archive checks.

1. Launch the exported executable normally and confirm textures load cleanly with no missing-archive dialog.
2. For a fast CLI check on a standard (unencrypted) build:

   ```powershell
   <export-dir>\game.exe --headless -- --dspkg-validate=unprotected
   ```

   Expect one line:

   ```text
   DSPKG_BOOT status=ok mode=unprotected entries=<n> key_id=none
   ```

For the complete sentinel grammar, `DSPKG_SCAN` pack scanner, failure codes, and CI setup, see [Verifying an Exported Build](verifying-an-exported-build.md). If you need encrypted PCKs and encrypted `.dspkg` tables of contents, see the [Encryption Guide](encryption-guide.md).

---

## Common Gotchas

- **Mixed `.pck` and `textures.dspkg` across exports:** Fails at boot with `archive_pairing_mismatch`. Redeploy both from one export. (Disabling `enforce_archive_pairing` sets archive ID `0` and is intended only for local tooling, not shipping.)
- **Missing `textures.dspkg` when the main scene directly references a `.dstex`:** Godot loads the main scene before autoloads (`DSRuntime`) execute. The build still fails closed (exits 1), but Godot aborts with a resource load error before `DSRuntime` can print `DSPKG_BOOT` or show its missing-files alert. Use a lightweight bootstrap/loading scene with no archived `.dstex` dependencies if you want that alert dialog to appear.
- **`.dspkg` files inside `res://`:** Never place a `.dspkg` inside your project folder expecting it to ship inside the `.pck`; the exporter always skips `.dspkg` paths so archives are never embedded inside the `.pck` (which breaks DirectStorage DMA).
- **Stale Windows runtime license warning in Export dialog:** If `include_directstorage_license_file` warns that runtime license texts are missing, reinstall the runtime through **Project > Tools > dsgd Tools... (DirectStorage) > Manage DirectStorage Runtime...**.

---

## Related Documents

- [Addon Reference (Export Preset Options)](../project/addons/directstorage_accelerator/README.md#export-preset-options) - Full table of `dsgd_extension/*` export options and Encryption tab outcomes
- [Encryption Guide](encryption-guide.md) - Shipping a protected build with encrypted `.pck` and `.dspkg` TOC encryption
- [Verifying an Exported Build](verifying-an-exported-build.md) - `DSPKG_BOOT` sentinel reference, `DSPKG_SCAN` pack scanner, and CI gates
- [Texture Import & Compression](import-and-compression.md) - `.dstex` import presets, GDeflate levels, and chunked payloads
- [Export & Archive Behaviour](for_developers/export-and-archive-behaviour.md) - Developer reference for exporter hooks, policy matrix, and runtime contract
