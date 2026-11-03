#!/usr/bin/env python3
"""Report how much of an SCons build came from the shared object cache.

A successful cache restore step only proves that an archive was downloaded. SCons
can still reject every cached object - a changed compiler, flag or define
invalidates all of them at once - and a three-minute job becomes a twenty-five
minute one while every step stays green (measured: run 41358649928, Windows,
0 of 1174 objects reused after /guard:cf and /Qspectre moved from CXXFLAGS to
CCFLAGS). The restore step cannot see that; the build log can.

The job summary is a *result* page, so this reports only what is worth reading
there: one muted line when the cache cost the job real time, and the numbers
behind it folded into a collapsed block. Warm and partially warm builds say
nothing at all. Two different cold builds are kept apart on purpose:

  * no cache entry existed (first run of a branch/OS): expected, quiet note;
  * an entry was restored but its objects no longer match the build signature:
    that is the expensive surprise, so it also emits a warning annotation.

Never fails the job: a summary must not turn a green build red.
"""

import argparse
import os
import re
import sys
from typing import Dict, List, Tuple

# SCons prints "Retrieved `<path>' from cache" for every derived file it takes
# from CacheDir. Only object files matter here: the project's generated sources
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

CACHE_MISS = "miss"
CACHE_REUSED = "reused"
CACHE_STALE = "stale"


def _env(name: str) -> str:
    return (os.environ.get(name) or "").strip()


def _hash_of(cache_key: str) -> str:
    """The hashFiles(...) value a cache key was built from, or '' if absent."""
    match = HASH_PATTERN.search(cache_key)
    return match.group(1) if match else ""


def parse_log(text: str) -> Tuple[int, int, str, List[str]]:
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


def cache_state() -> Tuple[str, Dict[str, str]]:
    """Classify the restore: nothing matched, it matched, or it matched stale.

    actions/cache has no "did it work" signal beyond the key it restored, so the
    distinction that matters is whether the restored entry was built from the
    same inputs as this tree (hashFiles value) or from an older one.
    """
    primary = _env("CACHE_PRIMARY_KEY")
    matched = _env("CACHE_MATCHED_KEY")
    exact_hit = _env("CACHE_HIT").lower() == "true"

    primary_hash = _hash_of(primary)
    matched_hash = _hash_of(matched)

    if exact_hit or (matched_hash and primary_hash and matched_hash == primary_hash):
        return CACHE_REUSED, {"hash": matched_hash or primary_hash}

    if matched:
        info = {"hash": matched_hash}
        run_match = RUN_PATTERN.search(matched)
        if run_match:
            info["run"] = run_match.group(1)
        sha_match = SHA_PATTERN.search(matched)
        if sha_match:
            info["sha"] = sha_match.group(1)
        return CACHE_STALE, info

    return CACHE_MISS, {}


def describe_entry(state: str, info: Dict[str, str], primary_hash: str) -> str:
    """Table cell: which entry was used, in terms of the current inputs."""
    if state == CACHE_REUSED:
        return f"`{(info.get('hash') or primary_hash)[:12]}` (current inputs)"
    if state == CACHE_STALE:
        detail = f"`{(info.get('hash') or 'unknown')[:12]}`"
        if info.get("run"):
            detail += f" from run #{info['run']}"
        if info.get("sha"):
            detail += f" (commit `{info['sha'][:7]}`)"
        current = f"`{primary_hash[:12]}`" if primary_hash else "unknown"
        return f"{detail}, current inputs {current}"
    return "no entry matched"


def format_elapsed(seconds: int) -> str:
    if seconds <= 0:
        return "unknown"
    minutes, remainder = divmod(seconds, 60)
    if minutes:
        return f"{minutes}m {remainder}s"
    return f"{remainder}s"


def scope_label(args) -> str:
    os_name = _env("RUNNER_OS") or args.os_name or "this runner"
    ref = _env("CACHE_REF")
    return f"`{ref}` on {os_name}" if ref else os_name


def headline(state: str, info: Dict[str, str], retrieved: int, compiled: int, args) -> str:
    """The one muted sentence the reader sees when the cache cost them time."""
    elapsed = format_elapsed(args.elapsed_seconds)
    if state == CACHE_STALE:
        entry = "the restored cache entry"
        if info.get("run"):
            entry = f"the cache entry from run #{info['run']}"
        if info.get("sha"):
            entry += f" (commit `{info['sha'][:7]}`)"
        sentence = (
            f"_SCons cache: {entry} no longer matches this build - "
            f"{retrieved:,} of {retrieved + compiled:,} objects reusable, "
            f"{compiled:,} recompiled in {elapsed}._"
        )
    else:
        sentence = (
            f"_SCons cache: no entry for {scope_label(args)} - "
            f"{compiled:,} objects compiled in {elapsed}._"
        )

    if args.build_failed:
        sentence = sentence[:-1] + " The build failed, so these counts are partial._"
    return sentence


def format_details(
    args, state: str, info: Dict[str, str], retrieved: int, compiled: int, signature_line: str, deltas: List[str]
) -> str:
    total = retrieved + compiled
    ratio = (retrieved / total) if total else None
    primary_hash = _hash_of(_env("CACHE_PRIMARY_KEY"))

    objects = f"{retrieved:,} retrieved / {compiled:,} compiled"
    if ratio is not None:
        objects += f" ({ratio * 100:.0f}% reused)"

    rows = [
        ("cache", describe_entry(state, info, primary_hash)),
        ("objects", objects),
        ("build", format_elapsed(args.elapsed_seconds)),
    ]
    if signature_line:
        rows.append(("signature", f"`{signature_line}`"))

    lines = ["<details><summary>SCons cache details</summary>", "", "| | |", "|---|---|"]
    for label, value in rows:
        lines.append(f"| {label} | {value} |")
    lines.append("")

    if deltas:
        lines.append("Build signature changed (this is why cached objects were rejected):")
        lines.append("")
        for delta in deltas:
            # SCons reports the construction-variable name in lower case; spell it
            # the way the build system does so the line reads as a real setting.
            field, _, change = delta.partition(" ")
            lines.append(f"- `{(field.upper() + ' ' + change).strip()}`")
        lines.append("")

    lines.append("</details>")
    return "\n".join(lines)


def console_line(state: str, retrieved: int, compiled: int, args, notable: bool) -> str:
    """Always printed to the step log, so the numbers are never lost."""
    total = retrieved + compiled
    ratio = f"{retrieved * 100 // total}%" if total else "n/a"
    return (
        f"SCons cache [{state}]: {retrieved:,} retrieved / {compiled:,} compiled ({ratio} reused), "
        f"build {format_elapsed(args.elapsed_seconds)}"
        + ("" if notable else "; nothing to report in the job summary")
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", default="scons-build.log", help="SCons build log to read.")
    parser.add_argument("--os-name", default=None, help="Override the platform name (defaults to RUNNER_OS).")
    parser.add_argument("--elapsed-seconds", type=int, default=0, help="Wall time of the build step.")
    parser.add_argument(
        "--cold-below",
        type=float,
        default=0.5,
        help="Treat a build as cold when less than this fraction of objects came from the cache (default: 0.5).",
    )
    parser.add_argument(
        "--min-compiled",
        type=int,
        default=100,
        help="Ignore builds that compiled fewer than this many objects (default: 100).",
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
    total = retrieved + compiled
    ratio = (retrieved / total) if total else None
    state, info = cache_state()

    # A cold build is only notable when there was enough work to cost time: a
    # 40-object incremental build is never worth a line.
    cold = ratio is not None and compiled >= args.min_compiled and ratio < args.cold_below
    notable = bool((cold or args.build_failed) and total > 0)

    print(console_line(state, retrieved, compiled, args, notable))

    if notable:
        blocks = [headline(state, info, retrieved, compiled, args)]
        if total:
            blocks.append(format_details(args, state, info, retrieved, compiled, signature_line, deltas))
        markdown = "\n\n".join(blocks) + "\n"

        print(markdown)

        summary_path = _env("GITHUB_STEP_SUMMARY")
        if summary_path:
            try:
                with open(summary_path, "a", encoding="utf-8") as summary_file:
                    summary_file.write(markdown + "\n")
            except OSError as error:
                print(f"SCons cache summary: cannot write the step summary ({error}).", file=sys.stderr)

    # Only a rejected entry is a surprise worth interrupting the run list for.
    # A first run on a branch/OS legitimately has no cache and must stay quiet.
    if notable and state == CACHE_STALE:
        print(
            "::warning title=SCons cache rejected::"
            f"A restored cache entry no longer matched this build's signature; "
            f"{compiled:,} of {retrieved + compiled:,} objects were recompiled. "
            "The build-signature delta in the job summary names the field that changed."
        )

    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as error:  # A summary must never fail the job.
        print(f"SCons cache summary failed: {error}", file=sys.stderr)
        sys.exit(0)
