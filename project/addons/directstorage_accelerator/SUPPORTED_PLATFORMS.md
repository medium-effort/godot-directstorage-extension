# Supported Extension Artifacts

The manifest declares these build targets:

- Windows x86_64: `template_debug` and `template_release`, with DirectStorage runtime DLLs. All local volumes are supported (NVMe, SATA/ATA, USB, SD, exFAT, RAID/Storage Spaces and virtual disks); UNC network shares are excluded. BypassIO is used only on NVMe + NTFS; other volumes use standard I/O with D3D12 GPU decompression intact. Rotational media automatically uses buffered DirectStorage I/O (`ForceFileBuffering`), flash media keeps the default unbuffered path. Set `dsgd_extension/allow_directstorage_without_bypassio=false` to restore the older NVMe/SATA/ATA + NTFS restriction.
- Linux x86_64: `template_debug` and `template_release`, using the software GDeflate fallback.
- macOS universal: `template_debug` and `template_release`, covering x86_64 and arm64 with the software GDeflate fallback.

Other architectures and platforms are not declared until matching artifacts are built and tested. They must not be inferred from the fallback backend alone. DirectStorage runtime DLLs are Windows-only dependencies.
