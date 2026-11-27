# Binary Format Version History (`.dstex` & `.dspkg`)

This reference documents the container versions, header layouts, capability bits, and current writer defaults for `.dstex` texture files and `.dspkg` archives.

Source definitions:
- [`src/formats/ds_format_common.h`](../../src/formats/ds_format_common.h)
- [`src/formats/dstex_format.h`](../../src/formats/dstex_format.h) / [`src/formats/dstex_format.cpp`](../../src/formats/dstex_format.cpp)
- [`src/formats/dspkg_format.h`](../../src/formats/dspkg_format.h) / [`src/formats/dspkg_format.cpp`](../../src/formats/dspkg_format.cpp)

---

## 1. `.dstex` Texture Container (`DTEX`, `0x58455444`)

### Current Default

`DSTex::write_to_file()` writes **v2** by default and automatically promotes to **v3** when any of these apply:
- Logical dimensions differ from storage dimensions (`p_logical_width != p_width` or `p_logical_height != p_height`, e.g., non-multiple-of-4 sources) or `p_depth_or_layers > 0`.
- Uncompressed storage (`DSCompressionFormat::NONE`), either when `compression/disk_compression` is `None` (`p_compression_level <= 0`) or automatically when GDeflate fails to shrink the footprint (`total_compressed_size >= footprint_size`), because v2 has no codec field.
- Any mip is split into multiple chunks (`any_chunked`, which happens when a mip exceeds `compression/chunk_size_kb`, default `8192` KiB).

Because `ds_importer.gd` defaults to `compression/chunk_size_kb = 8192`, textures whose largest mip is `<= 8 MiB` and whose dimensions are multiples of 4 with GDeflate compression emit **v2**; larger, uncompressed, or non-multiple-of-4 textures emit **v3**.

### Version History

| Version | Header Size | Mip Table Entry | Key Changes |
| :--- | :--- | :--- | :--- |
| **v1** | 48 B (`DSTexHeader`) | 12 B (`DSTexMipEntry`: `offset`, `size`) | Legacy single-stream compressed blob. No D3D12 row-pitch or 512-byte placement alignment; falls back to CPU `ImageTexture` at load time. |
| **v2** | 48 B (`DSTexHeader`) | 24 B (`DSTexMipEntryV2`) | Added D3D12-aligned per-mip footprints (`compressed_size`, `uncompressed_size`, 256-byte `row_pitch`, 512-byte mip placement alignment) for direct-to-VRAM GPU decompression. `header.reserved` and `mip_entry.reserved` must be `0`. |
| **v3** | 64 B (`DSTexHeaderV3` = `DSTexHeader` + 16 B `DSTexHeaderV3Ext`) | 24 B (`DSTexMipEntryV2` with `chunk_count`) | Added `DSTexHeaderV3Ext` (`logical_width`, `logical_height`, `depth_or_layers`), capability word in `header.reserved` (bits 0–7: `DSCompressionFormat`, bits 8–15: `DSGaclTransform`, bit 16: `DSTEX_CAP_CHUNKED`), `NONE` uncompressed slices, and optional 16-byte `DSChunkEntry` chunk tables when `chunk_count > 1`. |

### Defensive Limits (`dstex_format.h`)

- `DSTEX_MAX_MIP_COUNT`: `16`
- `DSTEX_MAX_DIMENSION`: `16384`
- `DSTEX_MAX_TOTAL_BYTES`: `512 MiB`
- `DSTEX_MAX_CHUNKS_PER_MIP`: `4096`

---

## 2. `.dspkg` Archive Container (`DPKG`, `0x474B5044`)

### Current Default

`DSPkg::finalize()` writes **v4** by default and automatically promotes to **v5** when any entry in the archive requires a v5 feature (`write_v5`):
- Shared alias payloads (`has_shared_payload`, e.g., dual-key `res://` + `.godot/imported/*.dstex` entries sharing one payload).
- Any entry with chunked mips (`out_chunked`).
- Any entry with a non-zero NPOT logical-size TLV (`DS_TLV_TAG_TEXTURE_LOGICAL`).
- Any entry whose output compression format is not `DSCompressionFormat::GDEFLATE` (e.g., `NONE` or `ZSTD`).

Because `ds_export_plugin.gd` registers dual-key aliases (`res://...` and `res://.godot/imported/...`) sharing payload offsets, standard Godot exports emit **v5** archives; single-key archives without chunking, `NONE` codecs, or NPOT TLVs stay **v4** byte-identical.

### Version History

| Version | Constant | Header Size | Fixed TOC Entry Size | Key Changes |
| :--- | :--- | :--- | :--- | :--- |
| **v1** | `DSPKG_VERSION_LEGACY` | 28 B | 50 B | Initial archive format. `flags` must be `0`. |
| **v2** | `DSPKG_VERSION_GPU_METADATA` | 28 B | 50 B | Added `DSPKG_FLAG_GPU_MIP_METADATA` (`1 << 0`) and per-entry GPU mip tables (`DSPkgMipEntry`). |
| **v3** | `DSPKG_VERSION_ENTRY_FLAGS` | 28 B | 54 B | Added per-entry 32-bit `flags` field (`DSPKG_ENTRY_FIELDS_SIZE_V3 = 44` + 10 B prefix). |
| **v4** | `DSPKG_VERSION_V4` | 64 B (`DSPkgHeader`) | 54 B | Added `archive_fingerprint` (64-bit pairing id), `compression_codec`, `archive_salt` (16 B), `min_reader_version`, `key_id`, and AEAD TOC encryption (`DSPKG_FLAG_ENCRYPTED_TOC`, XChaCha20-Poly1305 + BLAKE2b). |
| **v5** | `DSPKG_VERSION_V5` | 96 B (`DSPkgHeaderV5`) | 66 B | Added `header_size`, `toc_entry_size`, `capabilities` bitmask, `extension_offset`/`extension_size`, `DSPKG_FLAG_EXTENSION_TABLE`, per-entry `entry_kind` / `compression_format` / `gacl_transform` / `descriptor_flags`, TLV metadata (`DS_TLV_TAG_TEXTURE_LOGICAL`, etc.), 16-byte `DSChunkEntry` chunk tables (`chunk_count > 1`), and `64 MiB` TOC limit (`DSPKG_MAX_TOC_SIZE_V5`, up from `16 MiB`). |

### v5 Capability Bits (`ds_format_common.h`)

Every v5 capability bit is a required feature (an older reader that does not recognize a set bit fails closed):
- `DSPKG_CAP_ZSTD` (`1 << 0`)
- `DSPKG_CAP_GACL` (`1 << 1`)
- `DSPKG_CAP_CHUNKED` (`1 << 2`)
- `DSPKG_CAP_CONTENT_HASH` (`1 << 3`)
- `DSPKG_CAP_MESH` (`1 << 4`)
- `DSPKG_CAP_CRYPTO_V2` (`1 << 5`)
- `DSPKG_CAP_MULTI_ARCHIVE` (`1 << 6`)
- `DSPKG_CAP_SHARED_PAYLOAD` (`1 << 7`)
