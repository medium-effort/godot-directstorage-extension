#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_drive_support.h"

// The drive gate decides whether a Windows volume gets Win32Backend (and keeps
// D3D12 GPU GDeflate) or is forced onto the software fallback. The decision is
// pure so the whole bus/filesystem/media matrix is testable on a machine whose
// only asset volume is NVMe/NTFS.

namespace {

godot::DSDriveSupportInput volume(const char *p_bus, const char *p_fs) {
    godot::DSDriveSupportInput in;
    in.bus_type = p_bus;
    in.file_system = p_fs;
    return in;
}

} // namespace

GDEX_TEST(drive_support, unc_is_never_supported) {
    godot::DSDriveSupportInput in = volume("Network", "SMB");
    in.is_unc = true;
    in.is_nvme = false;
    in.is_legacy_bus = false;
    in.is_ntfs = false;
    in.media = godot::DSDriveMedia::Unknown;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_FALSE(out.directstorage_supported);
    GDEX_EXPECT_FALSE(out.bypassio_supported);
    GDEX_EXPECT_FALSE(out.force_file_buffering);
    GDEX_EXPECT_FALSE(out.reason.is_empty());
}

GDEX_TEST(drive_support, nvme_ntfs_flash_keeps_bypassio) {
    godot::DSDriveSupportInput in = volume("NVMe", "NTFS");
    in.is_nvme = true;
    in.is_legacy_bus = true;
    in.is_ntfs = true;
    in.media = godot::DSDriveMedia::Flash;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    GDEX_EXPECT_TRUE(out.bypassio_supported);
    GDEX_EXPECT_FALSE(out.force_file_buffering);
}

GDEX_TEST(drive_support, nvme_ntfs_rotational_uses_buffering) {
    godot::DSDriveSupportInput in = volume("NVMe", "NTFS");
    in.is_nvme = true;
    in.is_legacy_bus = true;
    in.is_ntfs = true;
    in.media = godot::DSDriveMedia::Rotational;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    // Capability stays true: ForceFileBuffering is what actually turns BypassIO
    // off in the resolved DSTORAGE_CONFIGURATION1.
    GDEX_EXPECT_TRUE(out.bypassio_supported);
    GDEX_EXPECT_TRUE(out.force_file_buffering);
}

GDEX_TEST(drive_support, usb_ntfs_flash_uses_standard_io) {
    // Shape of the local F: validation volume: USB-attached NVMe SSD, NTFS,
    // flash, BypassIO blocked by UASPStor.sys.
    godot::DSDriveSupportInput in = volume("USB", "NTFS");
    in.is_nvme = false;
    in.is_legacy_bus = false;
    in.is_ntfs = true;
    in.media = godot::DSDriveMedia::Flash;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    GDEX_EXPECT_FALSE(out.bypassio_supported);
    GDEX_EXPECT_FALSE(out.force_file_buffering);
    GDEX_EXPECT_TRUE(out.reason.contains("GPU decompression retained"));
}

GDEX_TEST(drive_support, sd_exfat_rotational_uses_buffered_io) {
    godot::DSDriveSupportInput in = volume("SD", "exFAT");
    in.is_nvme = false;
    in.is_legacy_bus = false;
    in.is_ntfs = false;
    in.media = godot::DSDriveMedia::Rotational;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    GDEX_EXPECT_FALSE(out.bypassio_supported);
    GDEX_EXPECT_TRUE(out.force_file_buffering);
}

GDEX_TEST(drive_support, virtual_exfat_flash_uses_standard_io) {
    godot::DSDriveSupportInput in = volume("Virtual", "exFAT");
    in.is_nvme = false;
    in.is_legacy_bus = false;
    in.is_ntfs = false;
    in.media = godot::DSDriveMedia::Flash;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    GDEX_EXPECT_FALSE(out.bypassio_supported);
    GDEX_EXPECT_FALSE(out.force_file_buffering);
}

GDEX_TEST(drive_support, sata_ntfs_is_supported_without_bypassio) {
    godot::DSDriveSupportInput in = volume("SATA", "NTFS");
    in.is_nvme = false;
    in.is_legacy_bus = true;
    in.is_ntfs = true;
    in.media = godot::DSDriveMedia::Flash;

    const godot::DSDriveSupport out = godot::resolve_drive_support(in);
    GDEX_EXPECT_TRUE(out.directstorage_supported);
    GDEX_EXPECT_FALSE(out.bypassio_supported);
    GDEX_EXPECT_FALSE(out.force_file_buffering);
}

GDEX_TEST(drive_support, strict_gate_rejects_new_buses_and_filesystems) {
    godot::DSDriveSupportInput usb = volume("USB", "NTFS");
    usb.is_legacy_bus = false;
    usb.is_ntfs = true;
    usb.allow_relaxed_gate = false;
    usb.media = godot::DSDriveMedia::Flash;
    const godot::DSDriveSupport usb_out = godot::resolve_drive_support(usb);
    GDEX_EXPECT_FALSE(usb_out.directstorage_supported);
    GDEX_EXPECT_TRUE(usb_out.reason.contains("NVMe, SATA, or ATA"));

    godot::DSDriveSupportInput exfat = volume("NVMe", "exFAT");
    exfat.is_nvme = true;
    exfat.is_legacy_bus = true;
    exfat.is_ntfs = false;
    exfat.allow_relaxed_gate = false;
    exfat.media = godot::DSDriveMedia::Flash;
    const godot::DSDriveSupport exfat_out = godot::resolve_drive_support(exfat);
    GDEX_EXPECT_FALSE(exfat_out.directstorage_supported);
    GDEX_EXPECT_TRUE(exfat_out.reason.contains("NTFS"));
}
