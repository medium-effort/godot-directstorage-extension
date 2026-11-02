#!/usr/bin/env python3
"""Report how much of an SCons build came from the shared object cache.

A successful cache restore step only proves that an archive was downloaded. SCons
can still reject every cached object - a changed compiler, flag or define
invalidates all of them at once - and a three-minute job becomes a twenty-five
minute one while every step stays green (measured: run 41358649928, Windows,
0 of 1174 objects reused after /guard:cf and /Qspectre moved from CXXFLAGS to
CCFLAGS). The restore step cannot see that; the build log can.

This writes the distinction into the job summary: objects retrieved from the
cache versus objects recompiled, which cache entry was used, and the
build-signature delta that SConstruct prints when the compile command changed.
Cold builds also emit a GitHub warning annotation, so the reason for the long
job is visible in the run list instead of only in the log.

Never fails the job: a summary must not turn a green build red.
"""

import argparse
import os
import re
import sys
from typing import List, Optional, Tuple

# SCons prints "Retrieved `<path>' from cache" for every derived file it takes
# from CacheDir. Only object files matter here: the projects' generated sources
# (godot-cpp/gen/**, gdextension_interface.h) are also cache entries, and they
# stay valid when the compiler flags change, so counting them would make a cold
# build look warm.
RETRIEVED_PATTERN = re.compile(r"^Retrieved `(?P<path>[^`]+)' from cache$", re.MULTILINE)
OBJECT_SUFFIXES = (".obj", ".o", ".os")
COMPILED_PATTERN = re.compile(r"^Compiling \S+", re.MULTILINE)
SIGNATURE_PATTERN = re.compile(r"^\[SCons\] (build-signature|previous build-signature): (.+)$", re.MULTILINE)
SIGNATURE_DELTA_PATTERN = re.compile(r"^\[SCons\] signature-delta: (.+)$", re.MULTILINE)

# Cache keys are
#   scons-<os>-<branch>-<hashFiles(...)>[-run-<run_id>-<attempt>][-sha-<sha>]
HASH_PATTERN = re.compile(r"([0-9a-f]{64})")
RUN_PATTERN = re.compile(r"-run-(\d+)")
SHA_PATTERN = re.compile(r"-sha-([0-9a-f]{7,40})")


def _env(name: str) -> str:
    return (os.environ.get(name) or "").strip()


def parse_log(text: str) -> Tuple[int, int, Optional[str], List[str]]:
    """Return (retrieved_objects, compiled_objects, signature_line, deltas)."""
    retrieved = sum(
        1 for match in RETRIEVED_PATTERN.finditer(text) if match.group("path").endswith(OBJECT_SUFFIXES)
    )
    compiled = len(COMPILED_PATTERN.findall(text))

    signature_line = ""
    for match in SIGNATURE_PATTERN.finditer(text):
        if match.group(1) == "build-signature":
            signature_line = match.group(2).strip()

    deltas = [match.group(1).strip() for match in SIGNATURE_DELTA_PATTERN.finditer(text)]
    return retrieved, compiled, signature_line, deltas


def describe_cache() -> str:
    """Explain which cache entry was used, in terms of the current inputs."""
    primary = _env("CACHE_PRIMARY_KEY")
    matched = _env("CACHE_MATCHED_KEY")
    hit = _env("CACHE_HIT")

    primary_hash_match = HASH_PATTERN.search(primary)
    primary_hash = primary_hash_match.group(1) if primary_hash_match else ""
    matched_hash_match = HASH_PATTERN.search(matched)
    matched_hash = matched_hash_match.group(1) if matched_hash_match else ""

    if not matched and not hit:
        return "no entry matched; nothing was restored"

    if matched_hash and primary_hash and matched_hash == primary_hash:
        return f"inputs match the current tree (`{matched_hash[:12]}`)"

    if matched:
        detail = f"`{matched_hash[:12] or 'unknown'}`"
        run_match = RUN_PATTERN.search(matched)
        if run_match:
            detail += f" from run #{run_match.group(1)}"
        sha_match = SHA_PATTERN.search(matched)
        if sha_match:
            detail += f" (commit `{sha_match.group(1)[:7]}`)"
        current = f"`{primary_hash[:12]}`" if primary_hash else "unknown"
        return f"stale inputs: restored {detail}, current inputs {current}"

    return f"restored `{matched}`"


def format_elapsed(seconds: int) -> str:
    if seconds <= 0:
        return "unknown"
    minutes, remainder = divmod(seconds, 60)
    if minutes:
        return f"{minutes}m {remainder}s"
    return f"{remainder}s"


def format_markdown(
    args, retrieved: int, compiled: int, signature_line: str, deltas: List[str], cold: bool
) -> str:
    total = retrieved + compiled
    ratio = (retrieved / total) if total else None
    os_name = _env("RUNNER_OS") or args.os_name or "Unknown OS"

    lines = [f"### SCons cache - {os_name}", ""]

    if cold:
        lines.append(
            "**Cold build:** the cache was restored, but no cached object matched, "
            "so every translation unit was recompiled."
        )
        lines.append("")

    if total == 0:
        lines.append("_No compile activity was recorded in the build log._")
        lines.append("")
        return "\n".join(lines)

    objects = f"**{retrieved:,}** retrieved / **{compiled:,}** compiled"
    if ratio is not None:
        objects += f" ({ratio * 100:.0f}% reused)"

    elapsed = args.elapsed_seconds
    rows = [
        ("cache", describe_cache()),
        ("objects", objects),
        ("build", format_elapsed(elapsed)),
    ]
    if signature_line:
        rows.append(("signature", f"`{signature_line}`"))

    lines.append("| | |")
    lines.append("|---|---|")
    for label, value in rows:
        lines.append(f"| {label} | {value} |")
    lines.append("")

    if deltas:
        lines.append("**Build signature changed** - cached objects cannot be reused, this explains the recompile:")
        lines.append("")
        for delta in deltas:
            lines.append(f"- `{delta}`")
        lines.append("")
    elif compiled and retrieved:
        lines.append("A partial recompile is expected when sources changed; only the counts above matter.")
        lines.append("")

    if args.build_failed:
        lines.append("**Note:** the build step failed, so these counts cover a partial build.")
        lines.append("")

    return "\n".join(lines)


def is_cold(retrieved: int, compiled: int, args) -> bool:
    total = retrieved + compiled
    if total == 0 or compiled < args.min_compiled:
        return False
    return (retrieved / total) < args.warn_below


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", default="scons-build.log", help="SCons build log to read.")
    parser.add_argument("--os-name", default=None, help="Override the platform name (defaults to RUNNER_OS).")
    parser.add_argument("--elapsed-seconds", type=int, default=0, help="Wall time of the build step.")
    parser.add_argument(
        "--warn-below",
        type=float,
        default=0.5,
        help="Warn when less than this fraction of objects came from the cache (default: 0.5).",
    )
    parser.add_argument(
        "--min-compiled",
        type=int,
        default=100,
        help="Do not warn for small builds below this many compiled objects (default: 100).",
    )
    parser.add_argument("--build-failed", action="store_true", help="Mark the counts as partial.")
    args = parser.parse_args()

    if not args.elapsed_seconds:
        try:
            args.elapsed_seconds = int(_env("BUILD_SECONDS") or 0)
        except ValueError:
            args.elapsed_seconds = 0

    try:
        # utf-8-sig: the log arrives through `tee` on a Windows runner, where a
        # BOM would otherwise hide the first "Retrieved ..." line.
        with open(args.log, "r", encoding="utf-8-sig", errors="replace") as log_file:
            log_text = log_file.read()
    except OSError as error:
        print(f"SCons cache summary: cannot read {args.log} ({error}); nothing to report.", file=sys.stderr)
        return 0

    retrieved, compiled, signature_line, deltas = parse_log(log_text)
    cold = is_cold(retrieved, compiled, args)
    markdown = format_markdown(args, retrieved, compiled, signature_line, deltas, cold)

    print(markdown)

    summary_path = _env("GITHUB_STEP_SUMMARY")
    if summary_path:
        try:
            with open(summary_path, "a", encoding="utf-8") as summary_file:
                summary_file.write(markdown + "\n")
        except OSError as error:
            print(f"SCons cache summary: cannot write the step summary ({error}).", file=sys.stderr)

    if cold:
        total = retrieved + compiled
        print(
            "::warning title=Cold SCons build::"
            f"{compiled:,} of {total:,} objects were recompiled (only {retrieved:,} came from the cache). "
            "A global compiler flag, define or toolchain change invalidates the whole cache; "
            "the build-signature delta in the job summary names the field that changed."
        )

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # A summary must never fail the job.
        print(f"SCons cache summary failed: {error}", file=sys.stderr)
        sys.exit(0)
