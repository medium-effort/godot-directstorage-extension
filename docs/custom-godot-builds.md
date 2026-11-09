# Building Godot from Source for DirectStorage

This guide covers SCons build flags and requirements when compiling custom Godot Engine binaries (editor and export templates) for use with the DirectStorage Accelerator GDExtension.

In vanilla Godot builds, almost all core modules are enabled by default. However, custom engine builds—especially those compiled from source or stripped for binary size—require specific flags to ensure DirectStorage hardware acceleration functions correctly.

---

## 1. Direct3D 12 Driver Requirement (Mandatory)

DirectStorage on Windows binds directly to Godot's active `RenderingDevice` when running under the Direct3D 12 driver (`--rendering-driver d3d12`). This allows DirectStorage to issue direct GPU DMA requests and decompress GDeflate mip streams directly into VRAM.

### The Source Build Gotcha
In Godot's source code (`SConstruct`), the `d3d12` build flag is `False` by default. Additionally, the Windows platform build checks for the Direct3D 12 Agility SDK dependencies: if they are missing, SCons warns to either install them or pass `d3d12=no`. **Never pass `d3d12=no`** if you intend to use DirectStorage hardware acceleration.

### How to Build
1. In your Godot source directory, install the D3D12 SDK dependencies:
   ```shell
   python misc/scripts/install_d3d12_sdk_windows.py
   ```
2. Compile Godot with `d3d12=yes`:
   ```shell
   scons platform=windows target=template_release d3d12=yes
   ```
   *(For an editor build, use `target=editor d3d12=yes`.)*

### Outcome if Excluded
If Godot is compiled without `d3d12=yes` (or with `d3d12=no`), `DSContext::is_d3d12_backend()` returns `false`. DirectStorage GPU decompression to VRAM cannot run, and the extension automatically falls back to CPU-side software decompression (`DSFallbackBackend`).

---

## 2. Minimal / Size-Optimized Engine Builds (`modules_enabled_by_default=no`)

When aggressively optimizing Godot for binary size, developers often pass `modules_enabled_by_default=no`, which turns off every optional module in Godot.

If you use this flag, you must explicitly re-enable the modules this extension relies on:

```shell
scons platform=windows target=template_release \
    d3d12=yes \
    modules_enabled_by_default=no \
    module_zip_enabled=yes \
    module_mbedtls_enabled=yes \
    module_betsy_enabled=yes \
    module_etcpak_enabled=yes \
    module_webp_enabled=yes
```

### Module Breakdown
* **`module_zip_enabled=yes` (`minizip=yes`)**: Required by the in-editor DirectStorage Runtime Manager to inspect and extract official Microsoft NuGet `.nupkg` packages (`ZIPReader`).
* **`module_mbedtls_enabled=yes`**: Backs Godot's `Crypto` class (used by the export plugin to generate random archive encryption keys) and powers TLS/HTTPS in `HTTPRequest` for package downloads.
* **`module_betsy_enabled=yes` & `module_etcpak_enabled=yes`**: Implements S3TC/BC texture compression in `Image.compress()` during `.dstex` import. If disabled, the importer cannot compress textures into VRAM block formats and falls back to uncompressed VRAM.
* **`module_webp_enabled=yes` (and `module_jpg_enabled=yes`)**: Required if importing `.webp` or `.jpg` source images into `.dstex`.

---

## 3. Blacklist Guardrails for `custom.py`

If you optimize Godot by maintaining a list of disabled modules in `custom.py` or passing individual `module_<name>_enabled=no` flags, ensure you **do not disable**:

| Option | Consequence if disabled |
| :--- | :--- |
| `d3d12` | Direct GPU decompression disabled; falls back to CPU software decompression. |
| `threads` | Multithreading disabled; asynchronous I/O and parallel GDeflate decompression fail. |
| `module_mbedtls_enabled` | `Crypto.new()` fails, breaking package encryption key generation on export; HTTPS package downloads fail. |
| `module_zip_enabled` | NuGet runtime package extraction disabled in the editor. |
| `module_betsy_enabled` / `module_etcpak_enabled` | VRAM block compression disabled during texture import. |
