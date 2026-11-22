#!/usr/bin/env python3
"""
Extract compiler errors and diagnostics from build logs for GitHub Actions Step Summary.
Supports GCC, Clang, and MSVC diagnostic outputs with surrounding context.
"""

import argparse
import os
import re
import sys
from typing import List, Tuple

# ANSI color sequences: gdextest colorizes its result lines, and the log is
# captured through tee, so the escapes would otherwise prefix every marker.
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# Failing-test marker emitted by gdextest; the indented lines below it carry
# the file:line assertions that failed. Self-contained: never backtrack into
# the preceding suite output for compiler-style context.
TEST_FAILURE_PATTERN = re.compile(r"^\s*(?:❌|\[\s*FAILED\s*\])")
TEST_FAILURE_SUMMARY_HEADER = re.compile(r"^\s*\[\s*FAILED\s*\]\s+\d+\s+tests?,\s+listed below:")
TEST_RESULT_LINE = re.compile(r"^\s*\[(?:\s*(?:RUN|OK|FAILED|PASSED|SKIPPED)\s*|[-=]{4,})\]")

# Patterns that indicate an actual error line
ERROR_PATTERNS = [
    re.compile(r":\s*(fatal error|error)\s*:", re.IGNORECASE),          # GCC / Clang
    re.compile(r"\b(error C\d+|error LNK\d+):", re.IGNORECASE),          # MSVC
    re.compile(r"^\s*clang(\+\+)?: error:", re.IGNORECASE),              # Clang driver
    re.compile(r"^\s*scons:\s*\*\*\*\s*\[.*\]\s*Error\s+\d+", re.IGNORECASE), # SCons target error
    re.compile(r"^\s*SCRIPT ERROR:", re.IGNORECASE),                     # Godot GDScript
    re.compile(r"^\s*ERROR:\s"),                                         # Godot engine error
    TEST_FAILURE_PATTERN,                                                # gdextest failing test
]

# Per-failure-kind wording for the step summary. The failing step decides the
# kind, so a gdextest assertion is never reported as a build failure.
FAILURE_KINDS = {
    "build": {
        "heading": "Build Failure",
        "diagnostics": "Compiler Diagnostics",
        "unit": "error(s)",
        "empty": "No compiler error patterns detected in log. Check the build log tail below.",
        "tail": "Build Log",
    },
    "validation": {
        "heading": "Validation Failure",
        "diagnostics": "Diagnostics",
        "unit": "error(s)",
        "empty": "No error patterns detected in the validation log. Check the log tail below.",
        "tail": "Validation Log",
    },
    "test": {
        "heading": "Test Failure",
        "diagnostics": "Test Failures",
        "unit": "failing test block(s)",
        "empty": "No test failure markers detected in the log. Check the test log tail below.",
        "tail": "Test Log",
    },
}

# Patterns that indicate warning (to be avoided or used as boundary)
WARNING_PATTERN = re.compile(r":\s*warning\s*:", re.IGNORECASE)

# Patterns that indicate context/continuation
CONTEXT_START_PATTERNS = [
    re.compile(r"^\s*In file included from\s+"),
    re.compile(r":\s*In (?:member )?function\s+"),
    re.compile(r":\s*In (?:copy )?constructor\s+"),
    re.compile(r":\s*In destructor\s+"),
    re.compile(r":\s*At global scope:\s*"),
]


def is_error_line(line: str) -> bool:
    return any(p.search(line) for p in ERROR_PATTERNS)


def is_continuation_line(line: str) -> bool:
    """Lines that are part of a compiler diagnostic block (snippets, carets, notes)."""
    # Notes, macro expansions
    if re.search(r":\s*note:\s*", line, re.IGNORECASE):
        return True
    # Line snippet or caret pipe: '  299 | ...' or '      | ^~~~~'
    if re.match(r"^\s*\d*\s*\|", line):
        return True
    # Indented caret line with no pipe (older GCC / MSVC)
    if re.match(r"^\s*[\^~]+", line):
        return True
    # MSVC continuation: '  see reference to ...' or '  while compiling ...'
    if line.startswith("        ") or line.startswith("\t"):
        return True
    return False


def extract_error_blocks(log_content: str, max_blocks: int = 20, kind: str = "build") -> Tuple[List[str], int]:
    """
    Parses compiler error blocks preserving surrounding context (function header, code snippet, caret, notes).
    Returns (list of formatted blocks, total error count).
    """
    lines = log_content.splitlines(keepends=True)

    # For test runs, prefer explicit test-failure markers over Godot engine ERROR:
    # lines in the trailing raw-log dump (tests intentionally trigger push_error).
    # Deduplicate summary repeats ("... listed below:") by keeping only the
    # run-site marker when indented details follow, or the first occurrence.
    if kind == "test":
        raw_test_indices = [
            i for i, line in enumerate(lines)
            if TEST_FAILURE_PATTERN.search(line) and not TEST_FAILURE_SUMMARY_HEADER.search(line)
        ]
        if raw_test_indices:
            with_details = [
                i for i in raw_test_indices
                if i + 1 < len(lines) and lines[i + 1].startswith(("    ", "\t"))
            ]
            error_indices = with_details if with_details else raw_test_indices
        else:
            error_indices = [
                i for i, line in enumerate(lines)
                if is_error_line(line) and not line.strip().startswith("scons:")
            ]
    else:
        # Exclude the generic SCons exit line from individual error spans
        error_indices = [
            i for i, line in enumerate(lines)
            if is_error_line(line) and not line.strip().startswith("scons:")
        ]

    total_errors = len(error_indices)
    if total_errors == 0:
        scons_errors = [i for i, line in enumerate(lines) if is_error_line(line)]
        if scons_errors:
            return ([lines[scons_errors[-1]].strip()], len(scons_errors))
        return ([], 0)

    spans: List[Tuple[int, int]] = []

    for idx in error_indices[:max_blocks]:
        # Test-failure markers are self-contained: their detail lines follow
        # (4-space or tab indented), so never pull in preceding suite output.
        if TEST_FAILURE_PATTERN.search(lines[idx]):
            end = idx + 1
            while end < len(lines) and (end - idx) < 30:
                nxt = lines[end]
                if TEST_RESULT_LINE.search(nxt) or not nxt.startswith(("    ", "\t")):
                    break
                end += 1
            spans.append((idx, end))
            continue

        # 1. Backtrack up to 10 lines to find context headers ('In function...', 'In file included...')
        start = idx
        while start > 0 and (idx - start) < 10:
            prev = lines[start - 1]
            if any(p.search(prev) for p in CONTEXT_START_PATTERNS):
                start -= 1
                while start > 0 and (idx - start) < 12 and any(p.search(lines[start - 1]) for p in CONTEXT_START_PATTERNS):
                    start -= 1
                break
            if prev.strip().startswith(("Compiling", "Building", "scons:")) or WARNING_PATTERN.search(prev):
                break
            start -= 1

        # 2. Advance forward to capture code snippets, carets, notes, and multi-line explanations
        end = idx + 1
        while end < len(lines) and (end - idx) < 15:
            nxt = lines[end]
            if is_error_line(nxt):
                break
            if is_continuation_line(nxt):
                end += 1
                continue
            if nxt.strip().startswith(("Compiling", "Building", "scons:")) or WARNING_PATTERN.search(nxt):
                break
            if any(p.search(nxt) for p in CONTEXT_START_PATTERNS):
                break
            if not nxt.startswith(" ") and not nxt.startswith("\t"):
                break
            end += 1

        spans.append((start, end))

    # Merge overlapping or touching spans
    merged_spans: List[Tuple[int, int]] = []
    for s, e in spans:
        if not merged_spans:
            merged_spans.append((s, e))
        else:
            prev_s, prev_e = merged_spans[-1]
            if s <= prev_e:
                merged_spans[-1] = (prev_s, max(prev_e, e))
            else:
                merged_spans.append((s, e))

    blocks = ["".join(lines[s:e]).rstrip() for s, e in merged_spans]
    return (blocks, total_errors)


def format_markdown(log_content: str, os_name: str, kind: str = "build") -> str:
    labels = FAILURE_KINDS.get(kind, FAILURE_KINDS["build"])
    blocks, total_errors = extract_error_blocks(log_content, kind=kind)
    lines = log_content.splitlines()
    tail_lines = lines[-40:] if len(lines) > 40 else lines
    tail_text = "\n".join(tail_lines)

    md = []
    md.append(f"### ❌ {labels['heading']} on {os_name}")
    md.append("")
    if total_errors > 0:
        md.append(f"**{labels['diagnostics']}:** Found {total_errors} {labels['unit']}.")
        md.append("")
        md.append("<details open><summary><b>Extracted Error Context</b></summary>")
        md.append("")
        md.append("```text")
        md.append("\n\n---\n\n".join(blocks))
        md.append("```")
        md.append("</details>")
        md.append("")
    else:
        md.append(f"**Notice:** {labels['empty']}")
        md.append("")

    md.append(f"<details><summary><b>{labels['tail']} (Tail - Last 40 lines)</b></summary>")
    md.append("")
    md.append("```text")
    md.append(tail_text)
    md.append("```")
    md.append("</details>")
    md.append("")

    return "\n".join(md)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log_file", nargs="?", default="scons-build.log")
    parser.add_argument("os_name", nargs="?", default=None)
    parser.add_argument("--kind", choices=sorted(FAILURE_KINDS), default="build",
                        help="Which step failed; selects the summary heading and wording.")
    args = parser.parse_args()

    log_file = args.log_file
    os_name = os.environ.get("RUNNER_OS") or args.os_name or "Unknown OS"
    labels = FAILURE_KINDS[args.kind]

    if not os.path.exists(log_file):
        print(f"Warning: log file '{log_file}' not found.", file=sys.stderr)
        summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
        if summary_path:
            with open(summary_path, "a", encoding="utf-8") as f:
                f.write(f"### ❌ {labels['heading']} on {os_name}\n\nLog file `{log_file}` was not found.\n")
        sys.exit(0)

    with open(log_file, "r", encoding="utf-8", errors="replace") as f:
        log_content = ANSI_ESCAPE.sub("", f.read())

    markdown_output = format_markdown(log_content, os_name, args.kind)

    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary_path:
        with open(summary_path, "a", encoding="utf-8") as f:
            f.write(markdown_output)
    else:
        # Default to printing to stdout if not in GitHub Actions
        print(markdown_output)


if __name__ == "__main__":
    main()
