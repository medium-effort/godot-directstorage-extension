#!/usr/bin/env python3
"""Run the GDScript GUT unit test suite headless (fetching GUT if missing).

Cross-platform runner for local development and CI. Resolves the Godot console
binary from --godot, environment variables, or the repo .env file, ensures
GUT is fetched into project/addons/gut/, and invokes GUT with
res://tests/.gutconfig.json.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from fetch_gut import ensure_gut


def read_dotenv(dotenv_path: Path) -> dict[str, str]:
    """Parse simple KEY=VALUE entries from a .env file."""
    values: dict[str, str] = {}
    if not dotenv_path.is_file():
        return values
    for raw_line in dotenv_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        values[key.strip()] = val.strip().strip('"').strip("'")
    return values


def resolve_godot_bin(repo_root: Path, explicit: str | None) -> str:
    """Resolve the Godot console executable path."""
    if explicit:
        return explicit
    for env_key in ("GODOT_CONSOLE_PATH", "GODOT_BIN", "GODOT"):
        val = os.environ.get(env_key, "").strip()
        if val:
            return val
    dotenv = read_dotenv(repo_root / ".env")
    for env_key in ("GODOT_CONSOLE_PATH", "GODOT_BIN", "GODOT"):
        val = dotenv.get(env_key, "").strip()
        if val:
            return val
    for candidate in ("godot", "godot4"):
        found = shutil.which(candidate)
        if found:
            return found
    raise FileNotFoundError(
        "Could not locate Godot binary. Pass --godot or set GODOT_CONSOLE_PATH / GODOT_BIN / GODOT."
    )


def ensure_imported(godot_bin: str, project_dir: Path) -> None:
    """Run --headless --import if .godot/extension_list.cfg is not yet present."""
    ext_cfg = project_dir / ".godot" / "extension_list.cfg"
    if ext_cfg.is_file():
        return
    print("[gut] Importing project to register GDExtension classes...")
    subprocess.run(
        [godot_bin, "--headless", "--path", str(project_dir), "--import"],
        check=False,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Fetch GUT if needed and run GDScript unit tests.")
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="Repository root directory.",
    )
    parser.add_argument("--godot", default=None, help="Path to Godot console executable.")
    parser.add_argument("--select", default=None, help="Optional GUT -gselect script filter.")
    parser.add_argument("--no-fetch", action="store_true", help="Do not download GUT if missing.")
    args, extra_gut_args = parser.parse_known_args(argv)

    repo_root = args.repo_root.resolve()
    project_dir = repo_root / "project"

    if not args.no_fetch:
        ensure_gut(repo_root)

    godot_bin = resolve_godot_bin(repo_root, args.godot)
    ensure_imported(godot_bin, project_dir)

    cmd = [
        godot_bin,
        "--headless",
        "--path",
        str(project_dir),
        "-s",
        "addons/gut/gut_cmdln.gd",
        "-gconfig=res://tests/.gutconfig.json",
    ]
    if args.select:
        cmd.append(f"-gselect={args.select}")
    cmd.extend(extra_gut_args)

    print(f"[gut] Running: {' '.join(cmd)}")
    completed = subprocess.run(cmd)
    return completed.returncode


if __name__ == "__main__":
    raise SystemExit(main())
