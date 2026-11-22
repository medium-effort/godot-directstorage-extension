#ifndef DS_DRIVE_SUPPORT_H
#define DS_DRIVE_SUPPORT_H

#include <godot_cpp/variant/string.hpp>

namespace godot {

// Neutral media classification so the drive-support decision can be unit-tested
// on every platform. The Win32 backend maps its StorageMediaClass onto these.
enum class DSDriveMedia {
    Flash,
    Rotational,
    Unknown,
};

struct DSDriveSupportInput {
    String bus_type;
    String file_system;
    bool is_unc = false;
    bool is_nvme = false;
    // NVMe, SATA or ATA: the bus set the legacy (strict) gate accepted.
    bool is_legacy_bus = false;
    bool is_ntfs = false;
    DSDriveMedia media = DSDriveMedia::Unknown;
    // dsgd_extension/allow_directstorage_without_bypassio. False restores the
    // old strict gate for users who need to roll back the relaxed behavior.
    bool allow_relaxed_gate = true;
};

struct DSDriveSupport {
    bool directstorage_supported = false;
    bool bypassio_supported = false;
    bool force_file_buffering = false;
    String reason;
};

// Pure decision: no engine calls, no I/O, no global state. DirectStorage works
// on any local volume; BypassIO requires NVMe + NTFS; ForceFileBuffering is only
// for rotational media (dstorage.h warns against buffering on fast drives).
inline DSDriveSupport resolve_drive_support(const DSDriveSupportInput &p_input) {
    DSDriveSupport out;

    if (p_input.is_unc) {
        out.reason = "DirectStorage and BypassIO are not supported on UNC network shares.";
        return out;
    }

    if (!p_input.allow_relaxed_gate) {
        if (!p_input.is_legacy_bus) {
            out.reason = "Drive bus is " + p_input.bus_type + ". DirectStorage support requires NVMe, SATA, or ATA.";
            return out;
        }
        if (!p_input.is_ntfs) {
            out.reason = "File system is " + p_input.file_system + ". DirectStorage requires NTFS.";
            return out;
        }
    }

    out.directstorage_supported = true;
    out.bypassio_supported = p_input.is_nvme && p_input.is_ntfs;
    out.force_file_buffering = p_input.media == DSDriveMedia::Rotational;

    if (out.force_file_buffering) {
        out.reason = out.bypassio_supported
                ? "Rotational NTFS media: DirectStorage uses buffered file I/O (ForceFileBuffering)."
                : "DirectStorage available on " + p_input.bus_type + " / " + p_input.file_system +
                        " with buffered I/O; BypassIO unavailable; GPU decompression retained.";
    } else {
        out.reason = out.bypassio_supported
                ? "NTFS flash media: DirectStorage uses the default unbuffered path (BypassIO when available)."
                : "DirectStorage available on " + p_input.bus_type + " / " + p_input.file_system +
                        " with standard unbuffered I/O; BypassIO unavailable; GPU decompression retained.";
    }
    return out;
}

} // namespace godot

#endif // DS_DRIVE_SUPPORT_H
