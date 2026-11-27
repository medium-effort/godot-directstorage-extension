# GDeflate Compression Level Benchmarks

This developer reference records the detailed `compression/gdeflate_level` (1–12) measurements behind the default level `3` in [`ds_importer.gd`](../../project/addons/directstorage_accelerator/scripts/ds_importer.gd).

`compression/gdeflate_level` is applied at import time only: it changes the stored `.dstex` / `.dspkg` bytes, not the runtime decompression cost, which is per-byte identical at every level.

For a user-friendly summary of which level to choose, see [Texture Import & Compression](../import-and-compression.md#2-gdeflate-compression-level).

---

## 1. Block-Compressed Payloads (`compression/mode = 0`, S3TC)

On the default 3D path the payload is already block-compressed (S3TC/BC1), so gains flatten quickly. Measured on 8 logical cores, mip 0 of a 4096x4096 albedo (8 MiB S3TC payload) with the addon's own compressor:

| Level | `.dstex` payload | vs level 1 | Compression time |
| :--- | --- | --- | --- |
| 1 | 3,240,912 B | - | 187 ms |
| 3 (3D default) | 3,187,260 B | -1.7% | 193 ms |
| 6 | 3,083,248 B | -4.9% | 238 ms |
| 9 | 2,975,292 B | -8.2% | 1126 ms |
| 12 | 2,959,824 B | -8.7% | 2526 ms |

End to end for the same 4096x4096 BC1 asset (decode, 13 mip levels, `.dstex` write), level 6 costs +11% import time for a 4.7% smaller file, and level 12 costs 6.1x for 8.4%.

The same sweep on two other map types from a production 3D project (8 MiB S3TC payloads, 4096x4096), showing how the gain depends on entropy (albedo gains most, low-entropy ORM masks least):

| Level | ORM payload | vs level 1 | Normal payload | vs level 1 |
| :--- | --- | --- | --- | --- |
| 1 | 5,912,908 B | - | 5,208,268 B | - |
| 3 | 5,869,528 B | -0.7% | 5,118,716 B | -1.7% |
| 6 | 5,679,348 B | -4.0% | 5,018,660 B | -3.6% |
| 9 | 5,535,556 B | -6.4% | 4,833,756 B | -7.2% |

---

## 2. Stylized / Hand-Painted S3TC Payloads

Stylized/hand-painted art gains considerably more at the same levels, because its flat color regions leave more redundancy than photographic albedo. Measured the same way (S3TC payloads, 8 logical cores) on a second 3D project whose assets are imported by Godot's own importer (modeling what this addon produces for the same art):

| Asset | S3TC payload | L1 | L3 | L6 | L9 |
| :--- | --- | --- | --- | --- | --- |
| moss albedo 2048x2048 | 2,097,152 B | 470,496 B | -14.5% | -24.7% | -31.3% |
| moss normal 2048x2048 | 2,097,152 B | 553,096 B | -7.5% | -13.6% | -19.6% |
| grass albedo 1024x1024 | 524,288 B | 222,828 B | -3.2% | -6.9% | -13.2% |
| leaves ORM 1024x1024 | 524,288 B | 347,636 B | -0.8% | -3.1% | -5.8% |
| menu background 1920x1080 | 1,036,800 B | 515,632 B | -2.2% | -4.7% | -9.7% |

Encode time at level 3 stays within a few percent (moss albedo 34.9 to 36.0 ms, moss normal 34.8 to 35.3 ms), while level 6 roughly doubles it (66.8 and 54.5 ms) and level 9 is 10–12x.

---

## 3. Uncompressed Payloads (`compression/mode = 1`)

Uncompressed payloads leave much more redundancy for GDeflate to find.

### Shipped small UI / `as_image_texture` assets

On tiny assets (totaling ~68 KiB raw), the level is worth bytes rather than kilobytes:

| Asset | Payload | L1 | L3 | L6 |
| :--- | --- | --- | --- | --- |
| icon 128x128 (`mode=1`) | 65,536 B | 44,016 B | 43,996 B (-0.05%) | 43,316 B (-1.6%) |
| crosshair 32x32 (`mode=1`) | 4,096 B | 392 B | 292 B (-25.5%) | 280 B (-28.6%) |
| spotlight 256x256 (`as_image_texture`, S3TC) | 32,768 B | 16,724 B | 16,568 B (-0.9%) | 16,256 B (-2.8%) |

### Large uncompressed RGBA8 models

Re-reading production 3D sources as raw RGBA8 to model what a film, lightmap, or photo-backed UI texture costs:

| Asset | Raw RGBA8 | L1 | L3 | L6 | L9 |
| :--- | --- | --- | --- | --- | --- |
| floor 512x512 (model) | 1,048,576 B | 43,872 B | -27.0% | -33.0% | -32.0% |
| particle 512x512 (model) | 1,048,576 B | 66,972 B | -15.9% | -23.4% | -24.7% |
| splash 1024x233 (model) | 954,368 B | 17,136 B | +0.4% | -7.4% | -7.9% |
| albedo 4096x4096 (model) | 67,108,864 B | 12,263,368 B | -6.1% | -8.8% | -18.7% |
| ORM 4096x4096 (model) | 67,108,864 B | 23,040,224 B | -6.2% | -11.8% | -16.7% |

Encode time on this path is within noise for small assets (7.5 / 7.0 / 6.7 ms for 512x512 at L1/L3/L6; 4.1 / 5.5 / 4.1 ms for the icon) and +15% / +43% for 64 MiB textures (1.04 / 1.20 / 1.49 s); level 9 is 10x (`10.2 s`).

---

## 4. Takeaway

Level `3` is the default for both paths: it is the last point that is close to free before encode time climbs two to four times faster than byte savings. Levels `9–12` are the libdeflate cliff and should be avoided for interactive or CI reimports.
