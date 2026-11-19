#!/usr/bin/env python
"""Post-build deployment: mirror a finished build into external projects.

SCons produces artifacts (the flat DLLs plus build_manifest.json inside
project/addons/directstorage_accelerator/bin) and knows nothing about where
they are deployed. This module owns that step: it copies the addon, the src
tree or the whole project into the directories named by the SYNC_* variables,
and records the outcome in build_manifest.json.

Invoked by:
  .vscode/build_remote.ps1   after a successful build (local or remote)
  .vscode/sync_addon.ps1     standalone, without building

The files come from this checkout unless --source-root names another one (a
worktree of the same repository). With an external source the copy test is
content-based instead of mtime-based: a worktree's artifacts are usually older
than what is already deployed, so mtime would copy nothing.

Targets (each runs only when its variable is configured):
  addon     SYNC_ADDON_DIR            addon dir  -> external project's addon
  copyable  SYNC_ADDON_COPYABLE_DIR   addon dir  -> <base>/directstorage_accelerator
                                      src/       -> <base>/src
  test      SYNC_TEST_DIR             project/   -> full project mirror

Exit codes: 0 = done (skips and per-file warnings included), 1 = a target in
--fatal-targets hit a directory-level failure, 2 = usage error.
"""
import argparse
import datetime
import json
import os
import re
import shutil
import sys
import tempfile

ADDON_REL = os.path.join("project", "addons", "directstorage_accelerator")
MANIFEST_REL = os.path.join(ADDON_REL, "bin", "build_manifest.json")

# Manifest nodes are kept identical to the shape SConstruct used to write, so
# any external reader of build_manifest.json keeps working. Node names:
#   root level       -> SYNC_TEST_DIR
#   addon_copyable   -> SYNC_ADDON_COPYABLE_DIR
#   tps_demo_addon   -> SYNC_ADDON_DIR
DEFAULT_FATAL = "addon"


# --------------------------------------------------------------------------- #
# Logging
# --------------------------------------------------------------------------- #

class Log:
    def __init__(self, verbose=False):
        self.verbose = verbose

    def info(self, message):
        print(f"[sync] {message}", flush=True)

    def detail(self, message):
        if self.verbose:
            print(f"[sync]   {message}", flush=True)

    def warn(self, message):
        print(f"[sync] WARNING: {message}", flush=True)


# --------------------------------------------------------------------------- #
# Environment
# --------------------------------------------------------------------------- #

def load_env_file(repo_root, env_path=None):
    """Fill missing environment variables from .env (env vars keep precedence).

    Mirrors SConstruct's load_env_file(): comments, quotes and utf-8-sig BOM
    tolerated, existing environment values never overwritten. Supports quoted
    multiline values ("..." / '...') and lines ending with ';'.
    """
    path = env_path or os.path.join(repo_root, ".env")
    if not os.path.isfile(path):
        return
    key_assign_re = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*\s*=")
    try:
        pending_key = None
        pending_quote = None
        pending_lines = []

        def flush_pending(strip_outer_quotes=False):
            if pending_key and pending_key not in os.environ:
                joined = "\n".join(pending_lines).strip()
                if strip_outer_quotes:
                    joined = joined.strip('"').strip("'")
                os.environ[pending_key] = joined

        def reset_pending():
            nonlocal pending_key, pending_quote, pending_lines
            pending_key = None
            pending_quote = None
            pending_lines = []

        with open(path, "r", encoding="utf-8-sig") as f:
            for raw_line in f:
                line = raw_line.strip()
                if pending_quote is not None:
                    if line.endswith(pending_quote):
                        pending_lines.append(line[:-1])
                        flush_pending(strip_outer_quotes=False)
                        reset_pending()
                    else:
                        pending_lines.append(line)
                    continue

                if not line or line.startswith("#"):
                    continue

                if pending_key is not None and not key_assign_re.match(line):
                    pending_lines.append(line)
                    if not line.endswith(";"):
                        flush_pending(strip_outer_quotes=True)
                        reset_pending()
                    continue

                if pending_key is not None:
                    flush_pending(strip_outer_quotes=True)
                    reset_pending()

                if "=" in line:
                    key, value = line.split("=", 1)
                    key = key.strip()
                    value = value.strip()
                    if value[:1] in ('"', "'") and not (len(value) >= 2 and value.endswith(value[0])):
                        pending_key = key
                        pending_quote = value[0]
                        pending_lines = [value[1:]]
                    elif value.endswith(";"):
                        pending_key = key
                        pending_lines = [value]
                    elif key and key not in os.environ:
                        os.environ[key] = value.strip('"').strip("'")
        if pending_key is not None:
            flush_pending(strip_outer_quotes=(pending_quote is None))
    except Exception:
        pass


def parse_path_list(raw):
    """Split a semicolon- or newline-delimited path list, trimming and deduplicating."""
    if not raw:
        return []
    paths = []
    seen = set()
    for entry in re.split(r"[;\r\n]+", raw):
        item = entry.strip().strip('"').strip("'").strip()
        if not item:
            continue
        key = os.path.normcase(os.path.abspath(item))
        if key in seen:
            continue
        seen.add(key)
        paths.append(item)
    return paths


# --------------------------------------------------------------------------- #
# Copy primitives (moved from SConstruct, semantics unchanged)
# --------------------------------------------------------------------------- #

def safe_copy_file(src_file, dst_file, dry_run=False):
    """Copy src -> dst, surviving a locked destination DLL.

    When a running Godot process holds the target DLL, rotate it to .old and
    retry once. Mirrors the previous SConstruct implementation.
    """
    if dry_run:
        return
    if dst_file.lower().endswith(".dll"):
        old_file = dst_file + ".old"
        if os.path.exists(old_file):
            try:
                os.remove(old_file)
            except Exception:
                pass

    try:
        shutil.copy2(src_file, dst_file)
    except (PermissionError, OSError):
        if os.path.exists(dst_file) and dst_file.lower().endswith(".dll"):
            old_file = dst_file + ".old"
            if os.path.exists(old_file):
                try:
                    os.remove(old_file)
                except Exception:
                    old_file = f"{dst_file}.{int(datetime.datetime.now().timestamp())}.old"
            try:
                os.replace(dst_file, old_file)
            except Exception:
                pass
            shutil.copy2(src_file, dst_file)
        else:
            raise


def file_contents_equal(left_path, right_path, chunk_size=1 << 20):
    """Byte-for-byte comparison, for when mtimes cannot be trusted."""
    with open(left_path, "rb") as left, open(right_path, "rb") as right:
        while True:
            left_chunk = left.read(chunk_size)
            right_chunk = right.read(chunk_size)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def should_copy_file(src_file, dst_file, content_compare=False):
    """Whether dst_file must be replaced from src_file.

    mtime is the cheap default: inside one working tree a rebuild always makes
    the source newer. Across checkouts it means nothing, so content_compare
    decides by bytes instead.
    """
    if not os.path.exists(dst_file):
        return True
    try:
        src_stat = os.stat(src_file)
        dst_stat = os.stat(dst_file)
        if src_stat.st_size != dst_stat.st_size:
            return True
        if content_compare:
            return not file_contents_equal(src_file, dst_file)
        return src_stat.st_mtime > dst_stat.st_mtime + 0.1
    except Exception:
        return True


def sync_changed_files(src_dir, dst_dir, file_filter=None, dry_run=False, log=None, content_compare=False):
    """Copy files that are new, different in size, or newer; return the count."""
    log = log or Log()
    copied_count = 0
    for root, dirs, files in os.walk(src_dir):
        # The Godot import cache is never deployed.
        dirs[:] = [d for d in dirs if d != ".godot"]

        rel_path = os.path.relpath(root, src_dir)
        target_root = os.path.join(dst_dir, rel_path) if rel_path != "." else dst_dir
        if not dry_run:
            os.makedirs(target_root, exist_ok=True)

        for f in files:
            if f.startswith("~") or f.lower().endswith((".tmp", ".old")):
                continue

            rel_file_path = os.path.normpath(os.path.join(rel_path, f)) if rel_path != "." else f
            if file_filter and not file_filter(rel_file_path, f):
                continue

            src_file = os.path.join(root, f)
            dst_file = os.path.join(target_root, f)

            if should_copy_file(src_file, dst_file, content_compare):
                try:
                    safe_copy_file(src_file, dst_file, dry_run)
                    copied_count += 1
                    log.detail(f"copy {rel_file_path}")
                except Exception as exc:
                    log.warn(f"failed to copy {src_file} -> {dst_file}: {exc}")
    return copied_count


# --------------------------------------------------------------------------- #
# Filters
# --------------------------------------------------------------------------- #

def addon_files_filter(rel_path, filename):
    """What may enter an addon mirror.

    Outside bin/ everything is legitimate addon content. Inside bin/ only the
    loadable native modules and the build manifest belong in a target project:
    linker scratch (.pdb/.ilk/.exp/import .lib) and .dev. builds stay local.
    Cross-platform on purpose: .dll, .so (including versioned .so.1), .dylib.
    """
    fname_lower = filename.lower()
    if filename.startswith("~") or fname_lower.endswith((".tmp", ".old")):
        return False

    parts = os.path.normpath(rel_path).split(os.sep)
    if "bin" in parts:
        if ".dev." in fname_lower:
            return False
        if (
            fname_lower.endswith((".dll", ".so", ".dylib"))
            or re.search(r"\.so\.\d+$", fname_lower)
            or fname_lower == "build_manifest.json"
        ):
            return True
        return False

    return True


# Previous name, kept so any external caller/test keeps working.
addon_windows_filter = addon_files_filter


def src_files_filter(rel_path, filename):
    """What may enter the copyable src/ mirror: sources, never build output."""
    fname_lower = filename.lower()
    if filename.startswith("~") or fname_lower.endswith(".tmp"):
        return False
    if fname_lower.endswith((".obj", ".o", ".os")):
        return False
    if fname_lower == "dspkg_encryption_key.h":
        return False
    return True


def without_manifest(file_filter):
    """Drop bin/build_manifest.json from a copy pass.

    patch_manifest() writes the authoritative (post-sync) copy after the copy
    pass, so comparing the deployed manifest against the source one would
    re-copy it on every run and report a phantom change.
    """
    def filtered(rel_path, filename):
        if filename.lower() == "build_manifest.json":
            return False
        return file_filter(rel_path, filename)

    return filtered


def clean_excluded_files(dst_dir, file_filter, dry_run=False, log=None):
    """Delete stale/temp files and anything the filter rejects from a mirror."""
    log = log or Log()
    if not os.path.exists(dst_dir):
        return
    for root, dirs, files in os.walk(dst_dir, topdown=False):
        rel_path = os.path.relpath(root, dst_dir)
        for f in files:
            rel_file_path = os.path.normpath(os.path.join(rel_path, f)) if rel_path != "." else f
            fname_lower = f.lower()
            should_remove = False
            if f.startswith("~") or fname_lower.endswith(".tmp"):
                should_remove = True
            elif fname_lower.endswith(".old"):
                should_remove = True
            elif file_filter and not file_filter(rel_file_path, f):
                should_remove = True

            if should_remove:
                if dry_run:
                    log.detail(f"would remove {rel_file_path}")
                    continue
                try:
                    os.remove(os.path.join(root, f))
                    log.detail(f"removed {rel_file_path}")
                except Exception:
                    pass


# --------------------------------------------------------------------------- #
# Targets
# --------------------------------------------------------------------------- #

class TargetResult:
    def __init__(self, name, target_path):
        self.name = name
        self.target_path = target_path
        self.synced = False
        self.copied = 0
        self.src_target_path = ""
        self.src_copied = 0
        self.error = ""
        self.attempted = True


def find_orphans(src_dir, dst_dir):
    """Files present in the destination (outside bin/) with no source counterpart."""
    orphans = []
    if not os.path.isdir(dst_dir):
        return orphans
    for root, dirs, files in os.walk(dst_dir):
        for f in files:
            full = os.path.join(root, f)
            rel = os.path.relpath(full, dst_dir)
            parts = os.path.normpath(rel).split(os.sep)
            if "bin" in parts:
                continue
            if f.endswith((".old", ".tmp")):
                continue
            if not os.path.exists(os.path.join(src_dir, rel)):
                orphans.append(full)
    return orphans


# A mirror target receives the addon's own contents, so the target must be the
# addon directory itself. These markers separate a real addon folder from the two
# ways that value is usually mistyped - a Godot project root, or an addons folder
# holding several plugins. Both copy "successfully" and then report every
# unrelated file in the tree as an orphan, which buries the real result.
PROJECT_ROOT_MARKER = "project.godot"
ADDON_MARKERS = ("directstorage_accelerator.gdextension", "plugin.cfg")


def describe_wrong_addon_target(target):
    """Why `target` cannot be an addon mirror, or "" when it plausibly is one."""
    if not os.path.isdir(target):
        return ""
    if os.path.isfile(os.path.join(target, PROJECT_ROOT_MARKER)):
        return f"it contains {PROJECT_ROOT_MARKER}, so it is a Godot project root"
    if any(os.path.isfile(os.path.join(target, marker)) for marker in ADDON_MARKERS):
        return ""
    foreign = []
    try:
        for entry in sorted(os.listdir(target)):
            if os.path.isfile(os.path.join(target, entry, "plugin.cfg")):
                foreign.append(entry)
    except OSError:
        return ""
    if foreign:
        shown = ", ".join(foreign[:3]) + ("..." if len(foreign) > 3 else "")
        return f"it holds other addons ({shown}) and has no addon marker of its own"
    return ""


def _sync_single_addon_target(addon_dir, target, log, dry_run, content_compare=False):
    """Mirror addon_dir into a single validated addon target directory."""
    result = TargetResult("addon", target)

    problem = describe_wrong_addon_target(target)
    if problem:
        # Refuse the write instead of scattering the addon into a project root:
        # the copy is flat, so the damage is real and the orphan list that would
        # explain it is exactly the unreadable dump this guard exists to prevent.
        result.error = f"SYNC_ADDON_DIR is not an addon directory: {problem}"
        log.warn(
            f"addon target rejected ({target}): {problem}. "
            "Point SYNC_ADDON_DIR at the addon folder itself, for example "
            "<project>/addons/directstorage_accelerator. Nothing was copied."
        )
        return result

    copy_filter = without_manifest(addon_files_filter) if content_compare else addon_files_filter
    try:
        if not dry_run:
            os.makedirs(target, exist_ok=True)
        clean_excluded_files(target, addon_files_filter, dry_run, log)
        result.copied = sync_changed_files(addon_dir, target, copy_filter, dry_run, log, content_compare)
        result.synced = True
        state = "would copy" if dry_run else "copied"
        log.info(f"addon    : {state} {result.copied} file(s) -> {target}")
    except Exception as exc:
        result.error = str(exc)
        log.warn(f"addon target failed ({target}): {exc}")
    return result


def sync_addon_targets(addon_dir, env, log, dry_run, content_compare=False, target_override=None):
    """addon dir -> one or more semicolon-separated SYNC_ADDON_DIR targets."""
    raw = target_override if target_override is not None else env.get("SYNC_ADDON_DIR", "")
    targets = parse_path_list(raw)
    if not targets:
        result = TargetResult("addon", "")
        result.attempted = False
        log.info("addon    : skipped (SYNC_ADDON_DIR is not configured)")
        return [result]

    return [
        _sync_single_addon_target(addon_dir, target, log, dry_run, content_compare)
        for target in targets
    ]


def sync_addon_target(addon_dir, env, log, dry_run, content_compare=False, target_override=None):
    """addon dir -> single SYNC_ADDON_DIR (kept for single-target callers)."""
    return sync_addon_targets(addon_dir, env, log, dry_run, content_compare, target_override)[0]


def sync_copyable_target(addon_dir, src_dir, env, log, dry_run, content_compare=False):
    """addon dir + src/ -> SYNC_ADDON_COPYABLE_DIR."""
    base = env.get("SYNC_ADDON_COPYABLE_DIR", "").strip()
    addon_target = os.path.join(base, "directstorage_accelerator") if base else ""
    src_target = os.path.join(base, "src") if base else ""
    result = TargetResult("copyable", addon_target)
    result.src_target_path = src_target
    if not base:
        result.attempted = False
        log.info("copyable : skipped (SYNC_ADDON_COPYABLE_DIR is not configured)")
        return result

    copy_filter = without_manifest(addon_files_filter) if content_compare else addon_files_filter
    try:
        if not dry_run:
            os.makedirs(addon_target, exist_ok=True)
        clean_excluded_files(addon_target, addon_files_filter, dry_run, log)
        result.copied = sync_changed_files(addon_dir, addon_target, copy_filter, dry_run, log, content_compare)

        if not dry_run:
            os.makedirs(src_target, exist_ok=True)
        clean_excluded_files(src_target, src_files_filter, dry_run, log)
        result.src_copied = sync_changed_files(src_dir, src_target, src_files_filter, dry_run, log, content_compare)

        result.synced = True
        state = "would copy" if dry_run else "copied"
        log.info(f"copyable : {state} {result.copied} addon file(s) -> {addon_target}")
        log.info(f"copyable : {state} {result.src_copied} src file(s) -> {src_target}")
    except Exception as exc:
        result.error = str(exc)
        log.warn(f"copyable target failed ({base}): {exc}")
    return result


def sync_test_target(project_dir, env, log, dry_run, content_compare=False):
    """project/ -> SYNC_TEST_DIR (full project mirror)."""
    target = env.get("SYNC_TEST_DIR", "").strip()
    result = TargetResult("test", target)
    if not target:
        result.attempted = False
        log.info("test     : skipped (SYNC_TEST_DIR is not configured)")
        return result

    try:
        if not dry_run:
            os.makedirs(target, exist_ok=True)
        copy_filter = without_manifest(lambda rel_path, filename: True) if content_compare else None
        result.copied = sync_changed_files(project_dir, target, copy_filter, dry_run, log, content_compare)
        result.synced = True
        state = "would copy" if dry_run else "copied"
        log.info(f"test     : {state} {result.copied} file(s) -> {target}")
    except Exception as exc:
        result.error = str(exc)
        log.warn(f"test target failed ({target}): {exc}")
    return result


# --------------------------------------------------------------------------- #
# Manifest
# --------------------------------------------------------------------------- #

def patch_manifest(results, source_root, repo_root, env, log, dry_run):
    """Record the sync outcome in build_manifest.json and deploy the patched copy.

    Single writer: SCons only produces the build half of this file now. The file
    patched is the one that describes the copied artifacts (source_root), so
    publishing another worktree never rewrites this checkout's record of its own
    build.
    """
    manifest_path = os.path.join(source_root, MANIFEST_REL)
    attempted = [r for r in results if r.attempted]
    if not attempted:
        return
    if not os.path.isfile(manifest_path):
        log.warn(f"build manifest missing ({manifest_path}); sync result not recorded")
        return

    try:
        with open(manifest_path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
    except Exception as exc:
        log.warn(f"could not read build manifest ({exc}); sync result not recorded")
        return

    pbs = data.get("post_build_sync")
    if not isinstance(pbs, dict):
        pbs = {}
        data["post_build_sync"] = pbs

    # Canonical shape, all three nodes present (the SConstruct writer did this).
    # Unconfigured targets keep an empty path, as before.
    copyable_base = env.get("SYNC_ADDON_COPYABLE_DIR", "").strip()
    pbs.setdefault("target_path", env.get("SYNC_TEST_DIR", ""))
    pbs.setdefault("synced", False)
    pbs.setdefault("copied_files_count", 0)
    copyable = pbs.setdefault("addon_copyable", {})
    copyable.setdefault("target_path", os.path.join(copyable_base, "directstorage_accelerator") if copyable_base else "")
    copyable.setdefault("synced", False)
    copyable.setdefault("copied_files_count", 0)
    copyable.setdefault("src_target_path", os.path.join(copyable_base, "src") if copyable_base else "")
    copyable.setdefault("src_copied_files_count", 0)
    addon = pbs.setdefault("tps_demo_addon", {})
    addon.setdefault("target_path", env.get("SYNC_ADDON_DIR", ""))
    addon.setdefault("synced", False)
    addon.setdefault("copied_files_count", 0)

    addon_results = [r for r in attempted if r.name == "addon"]
    for result in attempted:
        if result.name == "test":
            node = pbs
            node["target_path"] = result.target_path
            node["synced"] = bool(result.synced)
            node["copied_files_count"] = result.copied
            if result.error:
                node["error"] = result.error
        elif result.name == "copyable":
            node = copyable
            node["target_path"] = result.target_path
            node["src_target_path"] = result.src_target_path
            node["synced"] = bool(result.synced)
            node["copied_files_count"] = result.copied
            node["src_copied_files_count"] = result.src_copied
            if result.error:
                node["error"] = result.error
        elif result.name == "addon":
            continue
        else:  # pragma: no cover - defensive
            continue
        if not result.error:
            node.pop("error", None)

    if len(addon_results) == 1:
        result = addon_results[0]
        addon["target_path"] = result.target_path
        addon["synced"] = bool(result.synced)
        addon["copied_files_count"] = result.copied
        addon.pop("targets", None)
        if result.error:
            addon["error"] = result.error
        else:
            addon.pop("error", None)
    elif addon_results:
        addon["target_path"] = ";".join(r.target_path for r in addon_results)
        addon["synced"] = all(r.synced and not r.error for r in addon_results)
        addon["copied_files_count"] = sum(r.copied for r in addon_results)
        addon["targets"] = [
            {
                "target_path": r.target_path,
                "synced": bool(r.synced),
                "copied_files_count": r.copied,
                **({"error": r.error} if r.error else {}),
            }
            for r in addon_results
        ]
        errors = [f"{r.target_path}: {r.error}" for r in addon_results if r.error]
        if errors:
            addon["error"] = "; ".join(errors)
        else:
            addon.pop("error", None)

    if dry_run:
        log.info("manifest : would record sync result (dry run)")
        return

    # The manifest describes the build it sits next to. Publishing another
    # checkout must not rewrite that checkout's file, so an external source
    # writes a temporary copy that is only deployed, never kept.
    in_place = os.path.normcase(os.path.normpath(source_root)) == os.path.normcase(os.path.normpath(repo_root))
    written_path = manifest_path
    if not in_place:
        handle, written_path = tempfile.mkstemp(prefix="dsgd_manifest_", suffix=".json")
        os.close(handle)

    try:
        with open(written_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)
    except Exception as exc:
        log.warn(f"could not write build manifest ({exc})")
        if not in_place:
            try:
                os.remove(written_path)
            except Exception:
                pass
        return

    # Deploy the patched manifest next to each mirrored copy (as SCons used to).
    addon_dir = os.path.join(source_root, ADDON_REL)
    project_dir = os.path.join(source_root, "project")
    destinations = []
    for result in attempted:
        if not result.synced:
            continue
        if result.name == "test":
            destinations.append(os.path.join(result.target_path, os.path.relpath(manifest_path, project_dir)))
        elif result.name == "copyable":
            destinations.append(os.path.join(result.target_path, os.path.relpath(manifest_path, addon_dir)))
        elif result.name == "addon":
            destinations.append(os.path.join(result.target_path, os.path.relpath(manifest_path, addon_dir)))

    for dst in destinations:
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            safe_copy_file(written_path, dst)
            log.detail(f"manifest -> {dst}")
        except Exception as exc:
            log.warn(f"failed to deploy manifest to {dst}: {exc}")

    if not in_place:
        try:
            os.remove(written_path)
        except Exception:
            pass


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #

ALL_TARGETS = ("test", "copyable", "addon")


def parse_targets(raw):
    if not raw or raw.strip().lower() == "all":
        return list(ALL_TARGETS)
    names = [name.strip().lower() for name in raw.split(",") if name.strip()]
    unknown = [name for name in names if name not in ALL_TARGETS]
    if unknown:
        raise SystemExit(f"unknown target(s): {', '.join(unknown)} (expected: {', '.join(ALL_TARGETS)} or all)")
    return names


def main(argv=None):
    parser = argparse.ArgumentParser(
        prog="sync_artifacts.py",
        description="Mirror build output into the projects named by the SYNC_* variables.",
    )
    parser.add_argument("--targets", default="all",
                        help="all, or a comma-separated subset of: test,copyable,addon (default: all)")
    parser.add_argument("--dry-run", action="store_true", dest="dry_run",
                        help="report what would change; copy and delete nothing")
    parser.add_argument("--verbose", action="store_true", help="per-file output")
    parser.add_argument("--fatal-targets", default=DEFAULT_FATAL, dest="fatal_targets",
                        help="directory-level failures in these targets exit non-zero "
                             f"(default: {DEFAULT_FATAL}; use 'none' for warn-only)")
    parser.add_argument("--repo-root", default=None, help="repository root (default: parent of tools/)")
    parser.add_argument("--env-file", default=None, help="configuration file (default: <repo>/.env)")
    parser.add_argument("--source-root", default=None, dest="source_root",
                        help="checkout to copy from (default: this one), typically a worktree path")
    parser.add_argument("--source-label", default="", dest="source_label",
                        help="name of --source-root for the log (default: derived from the path)")
    parser.add_argument("--addon-dir", default=None, dest="addon_dir",
                        help="override SYNC_ADDON_DIR for the addon target (semicolon-separated for multiple)")
    args = parser.parse_args(argv)

    repo_root = os.path.abspath(args.repo_root or os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    load_env_file(repo_root, args.env_file)

    source_root = os.path.abspath(args.source_root) if args.source_root else repo_root
    same_checkout = os.path.normcase(os.path.normpath(source_root)) == os.path.normcase(os.path.normpath(repo_root))
    # mtimes are only comparable inside one working tree; a worktree's artifact
    # is usually older than the deployed copy, so mtime would never replace it.
    content_compare = not same_checkout
    source_label = args.source_label.strip() or ("current checkout" if same_checkout else source_root)

    targets = parse_targets(args.targets)
    fatal = {name.strip().lower() for name in (args.fatal_targets or "").split(",") if name.strip()}
    if "none" in fatal:
        fatal = set()

    log = Log(args.verbose)
    addon_dir = os.path.join(source_root, ADDON_REL)
    src_dir = os.path.join(source_root, "src")
    project_dir = os.path.join(source_root, "project")

    if content_compare:
        log.info(f"source   : {source_label} ({source_root}); comparing file contents, not mtimes")

    if not os.path.isdir(addon_dir):
        log.warn(f"addon directory not found in sync source {source_label}: {addon_dir}")
        return 1

    if args.dry_run:
        log.info("dry run: nothing will be copied or deleted")

    results = []
    if "test" in targets:
        results.append(sync_test_target(project_dir, os.environ, log, args.dry_run, content_compare))
    if "copyable" in targets:
        results.append(sync_copyable_target(addon_dir, src_dir, os.environ, log, args.dry_run, content_compare))
    if "addon" in targets:
        addon_override = (
            ";".join(os.path.abspath(p) for p in parse_path_list(args.addon_dir))
            if args.addon_dir is not None
            else None
        )
        results.extend(sync_addon_targets(addon_dir, os.environ, log, args.dry_run, content_compare, addon_override))

    patch_manifest(results, source_root, repo_root, os.environ, log, args.dry_run)

    # Orphan report for the addon mirror (files the addon no longer contains).
    if "addon" in targets:
        addon_results = [r for r in results if r.name == "addon" and r.synced and os.path.isdir(r.target_path)]
        for addon_result in addon_results:
            orphans = find_orphans(addon_dir, addon_result.target_path)
            if orphans:
                scope = f" in {addon_result.target_path}" if len(addon_results) > 1 else ""
                log.info(f"orphaned file(s) in mirror{scope} ({len(orphans)}) - no longer in the addon:")
                if log.verbose:
                    for orphan in orphans:
                        log.detail(f"- {orphan}")
                else:
                    log.info("rerun with --verbose to list them")
                log.info("remove them manually if they are no longer needed")

    attempted = [r for r in results if r.attempted]
    target_names = list(dict.fromkeys(r.name for r in attempted))
    failed = list(dict.fromkeys(r.name for r in attempted if r.error))
    copied_total = sum(r.copied for r in attempted)
    src_total = sum(r.src_copied for r in attempted)

    if not attempted:
        log.info("no sync target configured (SYNC_TEST_DIR / SYNC_ADDON_COPYABLE_DIR / SYNC_ADDON_DIR)")

    print(
        f"[sync] SUMMARY targets={','.join(target_names)} "
        f"copied={copied_total} src_copied={src_total} failed={','.join(failed) or 'none'}"
        + (f" source={source_label}" if content_compare else ""),
        flush=True,
    )

    fatal_failures = list(dict.fromkeys(r.name for r in attempted if r.error and r.name in fatal))
    if fatal_failures:
        print(f"[sync] ERROR: fatal sync failure in: {', '.join(fatal_failures)}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
