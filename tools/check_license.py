#!/usr/bin/env python3
"""Verify DSGD license files, mirrors, and SPDX headers."""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ADDON_DIR = ROOT / "project" / "addons" / "directstorage_accelerator"

CPP_EXTS = {".h", ".hpp", ".c", ".cpp"}
MIT_GD_NAMES = {
    "ds_runtime.gd",
    "ds_preloader.gd",
    "ds_batch_manager.gd",
    "ds_dspkg_static.gd",
}


def check_identical_pair(rel_a: str, rel_b: str, errors: list[str]) -> bytes | None:
    path_a = ROOT / rel_a
    path_b = ROOT / rel_b
    if not path_a.is_file():
        errors.append(f"Missing file: {rel_a}")
        return None
    if not path_b.is_file():
        errors.append(f"Missing mirror file: {rel_b}")
        return None
    data_a = path_a.read_bytes()
    data_b = path_b.read_bytes()
    if data_a != data_b:
        errors.append(f"Mirror mismatch: {rel_a} != {rel_b}")
    return data_a


def main() -> int:
    errors: list[str] = []

    # 1. MPL-2.0 LICENSE and addon mirror
    mpl_bytes = check_identical_pair(
        "LICENSE",
        "project/addons/directstorage_accelerator/LICENSE",
        errors,
    )
    if mpl_bytes is not None:
        mpl_text = mpl_bytes.decode("utf-8", errors="replace")
        if "Mozilla Public License Version 2.0" not in mpl_text:
            errors.append("LICENSE does not contain 'Mozilla Public License Version 2.0'")
        if "PLACEHOLDER LICENSE" in mpl_text:
            errors.append("LICENSE still contains 'PLACEHOLDER LICENSE'")
        if "Exhibit B -" in mpl_text:
            errors.append("LICENSE must not contain Exhibit B")

    # 2. MIT LICENSE-MIT and addon mirror
    mit_bytes = check_identical_pair(
        "LICENSE-MIT",
        "project/addons/directstorage_accelerator/LICENSE-MIT",
        errors,
    )
    if mit_bytes is not None:
        mit_text = mit_bytes.decode("utf-8", errors="replace")
        if "MIT License" not in mit_text or "Permission is hereby granted, free of charge" not in mit_text:
            errors.append("LICENSE-MIT does not appear to be a valid MIT License file")

    # 3. THIRD_PARTY_NOTICES.txt and addon mirror
    check_identical_pair(
        "THIRD_PARTY_NOTICES.txt",
        "project/addons/directstorage_accelerator/THIRD_PARTY_NOTICES.txt",
        errors,
    )

    # 4. First-party C/C++ sources in src/ (excluding src/gen/)
    src_dir = ROOT / "src"
    cpp_checked = 0
    for path in sorted(src_dir.rglob("*")):
        if not path.is_file() or path.suffix not in CPP_EXTS:
            continue
        if "gen" in path.relative_to(src_dir).parts:
            continue
        cpp_checked += 1
        head = path.read_text(encoding="utf-8", errors="replace")[:512]
        if "SPDX-License-Identifier: MPL-2.0" not in head:
            errors.append(f"Missing MPL-2.0 SPDX header: {path.relative_to(ROOT)}")

    # 5. Addon GDScripts (13 MPL-2.0, 4 MIT)
    gd_mpl_checked = 0
    gd_mit_checked = 0
    for path in sorted(ADDON_DIR.rglob("*.gd")):
        head = path.read_text(encoding="utf-8", errors="replace")[:512]
        if path.name in MIT_GD_NAMES:
            gd_mit_checked += 1
            if "SPDX-License-Identifier: MIT" not in head:
                errors.append(f"Missing MIT SPDX header: {path.relative_to(ROOT)}")
        else:
            gd_mpl_checked += 1
            if "SPDX-License-Identifier: MPL-2.0" not in head:
                errors.append(f"Missing MPL-2.0 SPDX header: {path.relative_to(ROOT)}")

    # 6. Vendored thirdparty/ must never carry MPL-2.0
    thirdparty_dir = ROOT / "thirdparty"
    if thirdparty_dir.is_dir():
        for path in sorted(thirdparty_dir.rglob("*")):
            if not path.is_file():
                continue
            try:
                content = path.read_bytes()
            except OSError as exc:
                errors.append(f"Could not read {path.relative_to(ROOT)}: {exc}")
                continue
            if b"MPL-2.0" in content:
                errors.append(f"Unexpected MPL-2.0 marker in thirdparty/: {path.relative_to(ROOT)}")

    if errors:
        print(f"check_license.py FAILED ({len(errors)} issue(s)):")
        for err in errors:
            print(f"  - {err}")
        return 1

    print(
        "check_license.py PASSED "
        f"(cpp={cpp_checked}, gd_mpl={gd_mpl_checked}, gd_mit={gd_mit_checked})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
