#!/usr/bin/env python3
"""Fetch and verify the pinned GUT (Godot Unit Test) addon into project/addons/gut/.

Cross-platform contributor and CI helper. Downloads the pinned GitHub tag
archive into .cache/gut/, verifies its SHA-256 checksum, and extracts only
addons/gut/ into project/addons/gut/ (which is gitignored).
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import urllib.request
import zipfile
from pathlib import Path

GUT_VERSION = "9.6.1"
GUT_TAG = f"v{GUT_VERSION}"
GUT_URL = f"https://github.com/bitwes/Gut/archive/refs/tags/{GUT_TAG}.zip"
GUT_SHA256 = "3901c72603b7573d7772cad2462f50a0f50ba8748e38f8727599344ce3aadfef"
ARCHIVE_PREFIX = f"Gut-{GUT_VERSION}/addons/gut/"
STAMP_FILENAME = ".gut_version"


def sha256_file(path: Path) -> str:
    """Compute the lowercase hex SHA-256 digest of a file."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def expected_stamp() -> str:
    return f"{GUT_TAG}:{GUT_SHA256}\n"


def is_installed(dest_dir: Path) -> bool:
    """Return True if the pinned GUT version is already extracted in dest_dir."""
    stamp_path = dest_dir / STAMP_FILENAME
    cmdln_path = dest_dir / "gut_cmdln.gd"
    if not cmdln_path.is_file() or not stamp_path.is_file():
        return False
    return stamp_path.read_text(encoding="utf-8") == expected_stamp()


def ensure_gut(
    repo_root: Path,
    cache_dir: Path | None = None,
    force: bool = False,
) -> Path:
    """Ensure the pinned GUT addon is present in project/addons/gut/."""
    dest_dir = repo_root / "project" / "addons" / "gut"
    if not force and is_installed(dest_dir):
        print(f"[gut] {GUT_TAG} already installed at {dest_dir}")
        return dest_dir

    resolved_cache = cache_dir if cache_dir is not None else (repo_root / ".cache" / "gut")
    resolved_cache.mkdir(parents=True, exist_ok=True)
    archive_path = resolved_cache / f"gut-{GUT_TAG}.zip"

    if archive_path.is_file():
        actual = sha256_file(archive_path)
        if actual != GUT_SHA256:
            print(f"[gut] Cached archive hash mismatch ({actual}); re-downloading...")
            archive_path.unlink()

    if not archive_path.is_file():
        print(f"[gut] Downloading {GUT_URL}...")
        tmp_path = archive_path.with_suffix(".zip.tmp")
        try:
            urllib.request.urlretrieve(GUT_URL, tmp_path)
            actual = sha256_file(tmp_path)
            if actual != GUT_SHA256:
                raise RuntimeError(
                    f"SHA-256 mismatch for {GUT_URL}: expected {GUT_SHA256}, got {actual}"
                )
            tmp_path.replace(archive_path)
        finally:
            if tmp_path.exists():
                tmp_path.unlink()

    if dest_dir.exists():
        shutil.rmtree(dest_dir)
    dest_dir.mkdir(parents=True, exist_ok=True)

    extracted = 0
    with zipfile.ZipFile(archive_path, "r") as zf:
        for member in zf.infolist():
            if not member.filename.startswith(ARCHIVE_PREFIX) or member.is_dir():
                continue
            rel = member.filename[len(ARCHIVE_PREFIX):]
            if not rel:
                continue
            target = (dest_dir / rel).resolve()
            if not target.is_relative_to(dest_dir.resolve()):
                raise RuntimeError(f"Unsafe zip member path: {member.filename}")
            target.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(member, "r") as src, target.open("wb") as dst:
                shutil.copyfileobj(src, dst)
            extracted += 1

    if not (dest_dir / "gut_cmdln.gd").is_file():
        raise RuntimeError(f"Extraction failed: gut_cmdln.gd missing from {dest_dir}")

    (dest_dir / STAMP_FILENAME).write_text(expected_stamp(), encoding="utf-8")
    print(f"[gut] Extracted {GUT_TAG} ({extracted} files) into {dest_dir}")
    return dest_dir


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch pinned GUT addon into project/addons/gut/")
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root directory (defaults to parent of tools/).",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=None,
        help="Download cache directory (defaults to <repo-root>/.cache/gut).",
    )
    parser.add_argument("--force", action="store_true", help="Re-extract even if already installed.")
    parser.add_argument("--check", action="store_true", help="Check if pinned GUT is installed without downloading.")
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    dest_dir = repo_root / "project" / "addons" / "gut"
    if args.check:
        if is_installed(dest_dir):
            print(f"[gut] {GUT_TAG} installed at {dest_dir}")
            return 0
        print(f"[gut] {GUT_TAG} is NOT installed at {dest_dir}", file=sys.stderr)
        return 1

    ensure_gut(repo_root, cache_dir=args.cache_dir, force=args.force)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
