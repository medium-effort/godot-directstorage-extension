#!/usr/bin/env python3
"""Package the DirectStorage GDExtension addon into a distributable zip archive."""

from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ADDON_SRC = ROOT / "project" / "addons" / "directstorage_accelerator"
ARCHIVE_PREFIX = "addons/directstorage_accelerator"

CANONICAL_BINARIES = (
    "dsgd_64.debug.dll",
    "dsgd_64.dll",
    "libdsgd_64.debug.so",
    "libdsgd_64.so",
    "libdsgd_64.debug.dylib",
    "libdsgd_64.dylib",
)

REQUIRED_ADDON_FILES = (
    "directstorage_accelerator.gdextension",
    "directstorage_accelerator.gdextension.uid",
    "plugin.cfg",
    "README.md",
    "LICENSE",
    "LICENSE-MIT",
    "THIRD_PARTY_NOTICES.txt",
    "runtime_versions.json",
    ".gitignore",
)

FORBIDDEN_DIR_NAMES = frozenset({
    "bin",
    ".migration_backup",
    ".versions",
    "__pycache__",
})

FORBIDDEN_FILE_NAMES = frozenset({
    "dstorage.dll",
    "dstoragecore.dll",
    "build_manifest.json",
    ".runtime.json",
    ".pending.json",
})

FORBIDDEN_EXTENSIONS = frozenset({
    ".pdb",
    ".ilk",
    ".exp",
    ".lib",
    ".old",
})

KEYED_TAG_RE = re.compile(rb"dspkg-key-id=(?!none\b)[0-9a-fA-F]+")
KEYLESS_TAG = b"dspkg-key-id=none"


def parse_plugin_cfg(cfg_path: Path) -> dict[str, str]:
    """Parse key=value pairs from plugin.cfg."""
    if not cfg_path.is_file():
        raise RuntimeError(f"Missing plugin.cfg: {cfg_path}")
    values: dict[str, str] = {}
    for raw_line in cfg_path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("["):
            continue
        if "=" not in line:
            continue
        key, val = line.split("=", 1)
        key = key.strip()
        val = val.strip()
        if len(val) >= 2 and val[0] == '"' and val[-1] == '"':
            val = val[1:-1]
        values[key] = val
    return values


def verify_keyless_binary(bin_path: Path) -> None:
    """Fail closed if a binary is empty, missing the keyless tag, or carries a baked key."""
    data = bin_path.read_bytes()
    if not data:
        raise RuntimeError(f"Binary is empty: {bin_path}")
    if KEYED_TAG_RE.search(data):
        raise RuntimeError(f"Binary contains a baked encryption key tag (refusing to pack): {bin_path}")
    if KEYLESS_TAG not in data:
        raise RuntimeError(f"Binary is missing required keyless tag 'dspkg-key-id=none': {bin_path}")


def is_forbidden_addon_path(rel_parts: tuple[str, ...]) -> bool:
    """Return True if an addon-relative path must be excluded from the zip."""
    if any(part in FORBIDDEN_DIR_NAMES for part in rel_parts[:-1]):
        return True
    name = rel_parts[-1]
    lower_name = name.lower()
    if lower_name in FORBIDDEN_FILE_NAMES:
        return True
    if name.startswith("~"):
        return True
    if Path(lower_name).suffix in FORBIDDEN_EXTENSIONS:
        return True
    return False


def _git_tracked_addon_files(addon_dir: Path) -> list[Path] | None:
    """Return sorted git-tracked file paths under addon_dir, or None outside a git work tree."""
    probe = subprocess.run(
        ["git", "-C", str(ROOT), "rev-parse", "--is-inside-work-tree"],
        capture_output=True,
        text=True,
        check=False,
    )
    if probe.returncode != 0 or probe.stdout.strip().lower() != "true":
        return None

    rel_addon = addon_dir.relative_to(ROOT).as_posix()
    res = subprocess.run(
        ["git", "-C", str(ROOT), "ls-files", "-z", "--", rel_addon],
        capture_output=True,
        check=True,
    )
    paths: list[Path] = []
    for raw_rel in res.stdout.split(b"\x00"):
        if not raw_rel:
            continue
        rel_str = raw_rel.decode("utf-8")
        paths.append(ROOT / rel_str)
    return sorted(paths)


def collect_addon_entries(addon_dir: Path) -> list[tuple[Path, str]]:
    """Collect tracked non-binary addon files and their archive paths."""
    if not addon_dir.is_dir():
        raise RuntimeError(f"Addon directory not found: {addon_dir}")

    tracked = _git_tracked_addon_files(addon_dir)
    candidates = tracked if tracked is not None else sorted(addon_dir.rglob("*"))

    entries: list[tuple[Path, str]] = []
    for path in candidates:
        if not path.is_file():
            continue
        rel = path.relative_to(addon_dir)
        if is_forbidden_addon_path(rel.parts):
            continue
        arcname = f"{ARCHIVE_PREFIX}/{rel.as_posix()}"
        entries.append((path, arcname))

    present_rels = {
        path.relative_to(addon_dir).as_posix()
        for path, _ in entries
    }
    missing_required = [req for req in REQUIRED_ADDON_FILES if req not in present_rels]
    if missing_required:
        raise RuntimeError(f"Missing required addon file(s): {', '.join(missing_required)}")
    return entries


def collect_binary_entries(binaries_dir: Path, allow_partial: bool) -> list[tuple[Path, str]]:
    """Collect and validate canonical binaries from binaries_dir."""
    if not binaries_dir.is_dir():
        raise RuntimeError(f"Binaries directory not found: {binaries_dir}")

    entries: list[tuple[Path, str]] = []
    missing: list[str] = []
    for name in CANONICAL_BINARIES:
        candidate = binaries_dir / name
        if candidate.is_file():
            verify_keyless_binary(candidate)
            entries.append((candidate, f"{ARCHIVE_PREFIX}/bin/{name}"))
        else:
            missing.append(name)

    if missing and not allow_partial:
        raise RuntimeError(
            "Missing canonical binary/binaries for release pack: "
            + ", ".join(missing)
        )
    if not entries:
        raise RuntimeError(f"No canonical binaries found in {binaries_dir}")
    return entries


def audit_zip(zip_path: Path, expected_binaries: list[str]) -> None:
    """Re-open the generated zip and audit its layout, required files, and exclusions."""
    with zipfile.ZipFile(zip_path, "r") as zf:
        names = zf.namelist()
        if not names:
            raise RuntimeError(f"Zip audit failed: archive is empty ({zip_path})")

        name_set = set(names)
        for name in names:
            if not name.startswith(f"{ARCHIVE_PREFIX}/"):
                raise RuntimeError(f"Zip audit failed: entry outside {ARCHIVE_PREFIX}/: {name}")
            rel = name[len(ARCHIVE_PREFIX) + 1:]
            parts = tuple(rel.split("/"))
            if parts[0] == "bin" and (len(parts) != 2 or parts[1] not in CANONICAL_BINARIES):
                raise RuntimeError(f"Zip audit failed: non-canonical entry in bin/: {name}")
            if any(part in (".migration_backup", ".versions", "__pycache__") for part in parts[:-1]):
                raise RuntimeError(f"Zip audit failed: forbidden directory in entry: {name}")
            base = parts[-1]
            lower_base = base.lower()
            if lower_base in FORBIDDEN_FILE_NAMES:
                raise RuntimeError(f"Zip audit failed: forbidden file in archive: {name}")
            if base.startswith("~") or Path(lower_base).suffix in FORBIDDEN_EXTENSIONS:
                raise RuntimeError(f"Zip audit failed: forbidden artifact in archive: {name}")

        for req in REQUIRED_ADDON_FILES:
            expected = f"{ARCHIVE_PREFIX}/{req}"
            if expected not in name_set:
                raise RuntimeError(f"Zip audit failed: missing required entry {expected}")

        for bin_arcname in expected_binaries:
            if bin_arcname not in name_set:
                raise RuntimeError(f"Zip audit failed: missing expected binary {bin_arcname}")
            info = zf.getinfo(bin_arcname)
            if info.file_size <= 0:
                raise RuntimeError(f"Zip audit failed: zero-byte binary {bin_arcname}")


def write_sha256_file(zip_path: Path) -> Path:
    """Compute SHA-256 of zip_path and write <zip_path>.sha256."""
    digest = hashlib.sha256()
    with zip_path.open("rb") as fh:
        while chunk := fh.read(1024 * 1024):
            digest.update(chunk)
    sha_path = Path(f"{zip_path}.sha256")
    sha_path.write_text(f"{digest.hexdigest()}  {zip_path.name}\n", encoding="utf-8")
    return sha_path


def main() -> int:
    parser = argparse.ArgumentParser(description="Package the DirectStorage GDExtension addon.")
    parser.add_argument(
        "--binaries-dir",
        type=Path,
        default=ADDON_SRC / "bin",
        help="Directory containing collected platform binaries (default: project/addons/directstorage_accelerator/bin).",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output .zip path (default: dist/dsgd-<version>.zip).",
    )
    parser.add_argument(
        "--version",
        type=str,
        default=None,
        help="Optional version string; if provided, must match plugin.cfg version (leading 'v' is stripped).",
    )
    parser.add_argument(
        "--allow-partial",
        action="store_true",
        help="Allow packing a subset of the 6 canonical binaries (for local single-OS smoke tests).",
    )
    parser.add_argument(
        "--write-sha256",
        action="store_true",
        help="Write <output>.sha256 alongside the zip archive.",
    )
    args = parser.parse_args()

    cfg = parse_plugin_cfg(ADDON_SRC / "plugin.cfg")
    plugin_version = cfg.get("version", "").strip()
    if not plugin_version:
        raise RuntimeError("plugin.cfg is missing a 'version' value")

    icon_rel = cfg.get("icon", "").strip()
    if icon_rel and not (ADDON_SRC / icon_rel).is_file():
        raise RuntimeError(f"plugin.cfg icon path does not exist: {ADDON_SRC / icon_rel}")

    if args.version is not None:
        normalized = args.version[1:] if args.version.startswith("v") else args.version
        if normalized != plugin_version:
            raise RuntimeError(
                f"Version mismatch: requested '{normalized}' != plugin.cfg version '{plugin_version}'"
            )

    output_path: Path = args.output if args.output is not None else ROOT / "dist" / f"dsgd-{plugin_version}.zip"
    output_path.parent.mkdir(parents=True, exist_ok=True)

    addon_entries = collect_addon_entries(ADDON_SRC)
    binary_entries = collect_binary_entries(args.binaries_dir, allow_partial=args.allow_partial)
    all_entries = sorted(addon_entries + binary_entries, key=lambda item: item[1])

    with zipfile.ZipFile(output_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for src_path, arcname in all_entries:
            zf.write(src_path, arcname)

    audit_zip(output_path, [arc for _, arc in binary_entries])

    sha_msg = ""
    if args.write_sha256:
        sha_path = write_sha256_file(output_path)
        sha_msg = f" + {sha_path}"

    print(
        f"pack_addon.py PASSED: {output_path}{sha_msg} "
        f"({len(addon_entries)} addon files, {len(binary_entries)}/{len(CANONICAL_BINARIES)} binaries)"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except RuntimeError as exc:
        print(f"pack_addon.py FAILED: {exc}", file=sys.stderr)
        sys.exit(1)
