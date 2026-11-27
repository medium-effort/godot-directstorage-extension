# Migration guide: converting project textures to DirectStorage

This guide covers batch-converting existing Godot project textures to the **DirectStorage Texture** (`.dstex`) importer using the **Migrate textures to DirectStorage...** editor dialog, and rolling back with the built-in ZIP backup if needed.

---

## Before you start

Make sure the following are in place before migrating:

1. **Addon and runtime ready**: The `dsgd` plugin is enabled and (on Windows) the DirectStorage runtime DLLs are installed - see [Installation & Quick Start](../README.md#installation--quick-start) if you haven't set those up yet.
2. **Editor filesystem idle**: Wait until Godot's `EditorFileSystem` is not actively scanning or importing before starting a migration or restore.
3. **Project backed up**: The migration tool creates a ZIP backup of `.import` sidecars automatically, but committing or backing up your project in version control is still recommended.

> [!NOTE]
> The backup and restore features use Godot's `ZIPPacker` and `ZIPReader` classes. If you use a custom Godot build compiled with `module_zip_enabled=no`, ZIP backup creation and restore will be unavailable.

---

## Opening the migration tool

In the Godot editor menu, open:

**Project > Tools > dsgd Tools... (DirectStorage) > Migrate textures to DirectStorage...**

When the dialog opens, it automatically scans `res://` for supported source images (`.png`, `.jpg`, `.jpeg`, `.webp`, `.tga`, `.dds`).

For `.dds` files, the scanner checks the DDS header and pixel format up front:
- **Compatible 2D `.dds` files** (uncompressed RGBA/RGB/RG/R or S3TC/BPTC block-compressed) are marked **Adopt** and migrate cleanly (using DDS passthrough when compressed).
- **Incompatible `.dds` files** (cubemaps, volume/3D textures, DX10 texture arrays, or unsupported formats) are marked **Skipped** so they stay on Godot's built-in `DDS` importer instead of failing import.

The scanner skips:
- Hidden or internal directories (`.godot`, `.git`, `.migration_backup`, and any folder starting with `.`)
- `addons/directstorage_accelerator`
- Any directory containing a `.gdignore` file

---

## Understanding scan statuses and filters

Use the search bar (`Filter textures by path or name...`) and the status dropdown (`All Assets`, `Eligible Only`, `Adopt Only`, `Upgrade Only`, `Up to Date Only`, `Skipped Only`) to narrow down large projects.

| Status | Meaning | Eligible? |
| :--- | :--- | :---: |
| **Adopt** | Uses Godot's default texture importer with a compatible `compress/mode` (`2` VRAM Compressed S3TC, `3` VRAM Uncompressed, or `5` BPTC/BC7). | Yes |
| **Upgrade** | Already uses `directstorage_texture`, but at an older `importer_version`. Rewrites changed/legacy keys in place while preserving existing `[params]` (`gdeflate_level`, `as_image_texture`, `mipmaps/*`, `process/*`). | Yes |
| **Repair** | Uses the current `directstorage_texture` version, but one or more destination `.dstex` artifacts are missing or unreadable. | Yes |
| **Up to date** | Already on the current `directstorage_texture` version with a valid `.dstex` artifact. | No |
| **Skipped** | Missing or unreadable `.import` sidecar, or uses an incompatible `compress/mode` (`0` Lossless, `1` Lossy, `4` Basis Universal). | No (unless overridden in the Action column for incompatible modes) |

---

## Parameter mapping and per-texture actions

When scanning non-DirectStorage `.import` files, the tool maps Godot's default import parameters automatically:

| Godot `compress/mode` | Default mapped action | Resulting DirectStorage settings |
| :--- | :--- | :--- |
| `2` (VRAM Compressed / S3TC) | **Migrate - VRAM Compressed** | `compression/mode=0` (preserves `high_quality`) |
| `3` (VRAM Uncompressed) | **Migrate - VRAM Uncompressed** | `compression/mode=1`, `compression/high_quality=false` |
| `5` (VRAM Compressed / BPTC) | **Migrate - VRAM Compressed** | `compression/mode=0`, `compression/high_quality=true` (BC7) |
| `0` (Lossless), `1` (Lossy), `4` (Basis Universal) | **Skip (Keep Texture2D)** | Left on Godot's default importer |

You can override any row's action before migrating by clicking its **Action** column cell:

- **Skip (Keep Texture2D)** - leaves the texture on Godot's default importer.
- **Migrate - VRAM Compressed** - S3TC/BPTC block-compressed `.dstex`.
- **Migrate - VRAM Uncompressed** - uncompressed pixel data with lossless GDeflate on disk.
- **Migrate - Normal Map** - linear color space (`compression/color_space=0`), `compress/normal_map=1`, `compress/channel_pack=1`.

> [!IMPORTANT]
> Godot's built-in `Texture2D` importer defaults `compress/normal_map` to `0` (**Detect**), whereas `directstorage_texture` only supports `0` (**Disabled**) and `1` (**Enabled**). Only textures explicitly set to **Enable** (`1`) in their original `.import` file are auto-detected as **Migrate - Normal Map**. If a normal map was left on **Detect**, switch its **Action** cell to **Migrate - Normal Map** before migrating.

---

## Running the migration

1. Select the textures or folders you want to convert (or use **Select All Eligible** / **Deselect All**).
2. Keep **Automatically delete replaced `.ctex` cache files** enabled (default) if you want the tool to remove superseded `.ctex` and `.md5` files from `res://.godot/imported/` after verification. Live `.dstex` files are never deleted by cleanup.
3. Click **Migrate Selected (\<N\>)**.

The tool executes five steps:

1. **ZIP Backup**: Packs the original `.import` files and a `manifest.json` (recording UIDs and SHA-256 hashes) into `res://addons/directstorage_accelerator/.migration_backup/backup_YYYYMMDD_HHMMSS.zip` (with a `.gdignore` file so Godot does not import the folder).
2. **Rewrite `.import` sidecars**: Atomically updates each selected `.import` file while preserving its exact `uid` so scenes, materials, and scripts stay intact.
3. **Batch Reimport**: Uncaches live `CompressedTexture2D` instances and reimports textures in batches of 50 through `EditorFileSystem.reimport_files()`.
4. **Verify**: Confirms each `.import` points to `directstorage_texture` at the current `importer_version`, its `uid` is unchanged, and every destination `.dstex` file exists with a valid `DTEX` header.
5. **Cleanup (if enabled)**: Deletes superseded `.ctex` and `.md5` cache files for verified entries only.

When finished, the tree rescans automatically so you can confirm every migrated asset shows **Up to date**.

---

## Rolling back (Restore from Backup)

To revert a migration:

1. Click **Restore from Backup...** in the migration dialog.
   - If backups exist in `res://addons/directstorage_accelerator/.migration_backup/`, the tool selects the newest `backup_*.zip` automatically.
   - If none are found there, a file dialog opens so you can pick a backup ZIP manually.
2. Confirm the restore prompt. For each entry in `manifest.json`:
   - The tool checks that the source image still exists and matches its recorded SHA-256 hash (modified or missing sources are skipped rather than overwritten blindly).
   - Original `.import` files are restored atomically, and the migrated `.dstex` (and matching `.md5`) files are removed from `res://.godot/imported/`.
3. **Restart the editor** when prompted. Godot's startup scan cleanly reimports the restored `.import` sidecars. (If you choose **Reimport without restarting**, the tool triggers an in-session reimport instead.)

---

## Related documents

- [Repository README](../README.md) - Overview, quick start, and documentation index
- [Addon Reference](../project/addons/directstorage_accelerator/README.md) - Full list of `.dstex` import options, `dsgd_extension/*` Project Settings, and export preset options
- [Verifying an Exported Build](verifying-an-exported-build.md) - Checking exported `.dspkg` archives and runtime DLLs
