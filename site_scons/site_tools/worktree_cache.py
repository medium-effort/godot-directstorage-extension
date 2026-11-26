"""SCons site tool: hardlinked shared CacheDir and per-worktree cache ledger.

Replaces SCons' default file-copy CacheDir transfers with NTFS hardlinks (with
automatic copy fallback across volumes or for mutable-in-place files such as
PDBs) and records a lightweight per-worktree JSON ledger under
<SCONS_CACHE>/.worktrees/<worktree_id>.json so tools/scons_cache.ps1 can
selectively trim cache entries when a worktree is removed.
"""
import atexit
import datetime
import hashlib
import json
import os
import re
import subprocess
import threading

import SCons.Action
import SCons.CacheDir
import SCons.Script

_LOCK = threading.Lock()
_CACHE_DIR = ""
_RETRIEVED = {}
_PUSHED = {}
_PUSHED_KEYS = set()
_ATEXIT_REGISTERED = False

_MUTABLE_SUFFIXES = (".pdb", ".ilk")


def exists(env):
    return True


def generate(env):
    env.AddMethod(enable_worktree_cache, "EnableWorktreeCache")


def _hardlink_enabled():
    val = os.environ.get("SCONS_CACHE_HARDLINK", "1").strip().lower()
    return val not in ("0", "false", "no", "off")


def _is_mutable_in_place(path):
    return str(path).lower().endswith(_MUTABLE_SUFFIXES)


class WorktreeCacheDir(SCons.CacheDir.CacheDir):
    """CacheDir subclass that hardlinks derived objects and tracks worktree keys."""

    @classmethod
    def _hardlink_or_fallback(cls, env, src, dst, fallback_fn):
        if _hardlink_enabled() and not _is_mutable_in_place(dst):
            parent = os.path.dirname(dst)
            if parent:
                os.makedirs(parent, exist_ok=True)
            if os.path.lexists(dst):
                try:
                    os.unlink(dst)
                except OSError:
                    pass
            try:
                os.link(src, dst)
                return dst
            except OSError:
                pass
        return fallback_fn(env, src, dst)

    @classmethod
    def copy_from_cache(cls, env, src, dst):
        return cls._hardlink_or_fallback(env, src, dst, SCons.CacheDir.CacheDir.copy_from_cache)

    @classmethod
    def copy_to_cache(cls, env, src, dst):
        return cls._hardlink_or_fallback(env, src, dst, SCons.CacheDir.CacheDir.copy_to_cache)

    def retrieve(self, node):
        hit = super().retrieve(node)
        if hit and SCons.Action.execute_actions:
            _, cachefile = self.cachepath(node)
            if cachefile:
                sig = os.path.basename(cachefile)
                rel = node.get_internal_path().replace("\\", "/")
                with _LOCK:
                    _RETRIEVED[rel] = sig
        return hit

    def push(self, node):
        result = super().push(node)
        if self.is_enabled() and not self.is_readonly() and not node.nocache:
            _, cachefile = self.cachepath(node)
            if cachefile and os.path.isfile(cachefile):
                sig = os.path.basename(cachefile)
                rel = node.get_internal_path().replace("\\", "/")
                with _LOCK:
                    _PUSHED[rel] = sig
                    _PUSHED_KEYS.add(sig)
        return result


def enable_worktree_cache(env, cache_dir, skip=False):
    """Configure WorktreeCacheDir on SCons' default and active environments."""
    global _CACHE_DIR, _ATEXIT_REGISTERED

    if skip or not cache_dir:
        return

    resolved = os.path.abspath(str(cache_dir).strip())
    with _LOCK:
        _CACHE_DIR = resolved
        if not _ATEXIT_REGISTERED:
            atexit.register(_flush_worktree_ledger)
            _ATEXIT_REGISTERED = True

    SCons.Script.CacheDir(resolved, custom_class=WorktreeCacheDir)
    env.CacheDir(resolved, custom_class=WorktreeCacheDir)


def _atomic_write_json(path, payload):
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp_path = f"{path}.tmp.{os.getpid()}"
    try:
        with open(tmp_path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(payload, f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp_path, path)
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def _read_json(path):
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8-sig") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else None
    except Exception:
        return None


def _git_state(root):
    head_commit = ""
    branch = ""
    is_dirty = False
    try:
        head_commit = subprocess.check_output(
            ["git", "-C", root, "rev-parse", "HEAD"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        branch = subprocess.check_output(
            ["git", "-C", root, "branch", "--show-current"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        status_out = subprocess.check_output(
            ["git", "-C", root, "status", "--porcelain"],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
        is_dirty = len(status_out) > 0
    except Exception:
        pass
    return head_commit, branch, is_dirty


def _cleanup_primary_handover(ledger_dir, active_sigs):
    # Only drop pins that primary has already materialized into active_targets.
    # Leave expired pins in _handover.json so `tools/scons_cache.ps1 -Action
    # TrimOrphans` can delete their payload files when no build is active;
    # removing them here without deleting the cache files turns them into
    # untracked orphans.
    handover_path = os.path.join(ledger_dir, "_handover.json")
    data = _read_json(handover_path)
    if not data:
        return
    pins = data.get("pins")
    if not isinstance(pins, dict):
        return

    kept = {}
    changed = False
    for sig, meta in pins.items():
        if sig in active_sigs:
            changed = True
            continue
        kept[sig] = meta

    if not changed:
        return
    if not kept:
        try:
            os.remove(handover_path)
        except OSError:
            pass
    else:
        data["pins"] = kept
        _atomic_write_json(handover_path, data)


def _flush_worktree_ledger():
    try:
        with _LOCK:
            cache_dir = _CACHE_DIR
            retrieved = dict(_RETRIEVED)
            pushed = dict(_PUSHED)
            pushed_keys = set(_PUSHED_KEYS)

        if not cache_dir:
            return

        root = os.path.abspath(".")
        is_primary = os.path.isdir(os.path.join(root, ".git"))
        norm_root = os.path.normcase(root)
        if is_primary:
            wt_id = "primary"
        else:
            leaf = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(root)) or "worktree"
            suffix = hashlib.sha1(norm_root.encode("utf-8")).hexdigest()[:8]
            wt_id = f"{leaf}-{suffix}"

        ledger_dir = os.path.join(cache_dir, ".worktrees")
        ledger_path = os.path.join(ledger_dir, f"{wt_id}.json")
        existing = _read_json(ledger_path)

        if not retrieved and not pushed and existing is None:
            return

        active_targets = {}
        all_pushed = set(pushed_keys)
        if existing:
            prev_active = existing.get("active_targets")
            if isinstance(prev_active, dict):
                for rel, sig in prev_active.items():
                    if isinstance(rel, str) and isinstance(sig, str) and sig:
                        active_targets[rel] = sig
            prev_pushed = existing.get("pushed_keys")
            if isinstance(prev_pushed, list):
                for sig in prev_pushed:
                    if isinstance(sig, str) and sig:
                        all_pushed.add(sig)

        # Prune targets that no longer exist in this worktree (cleaned/deleted).
        for rel in list(active_targets.keys()):
            disk_path = os.path.join(root, rel.replace("/", os.sep))
            if not os.path.exists(disk_path):
                old_sig = active_targets.pop(rel)
                all_pushed.add(old_sig)

        # Merge hits and pushes from this invocation, demoting superseded keys.
        for updates in (retrieved, pushed):
            for rel, sig in updates.items():
                prev_sig = active_targets.get(rel)
                if prev_sig and prev_sig != sig:
                    all_pushed.add(prev_sig)
                active_targets[rel] = sig

        head_commit, branch, is_dirty = _git_state(root)
        payload = {
            "schema": 1,
            "worktree_id": wt_id,
            "worktree_path": root,
            "is_primary": is_primary,
            "branch": branch,
            "head_commit": head_commit,
            "is_dirty": is_dirty,
            "updated_at": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "active_targets": dict(sorted(active_targets.items())),
            "pushed_keys": sorted(all_pushed),
        }
        _atomic_write_json(ledger_path, payload)

        if is_primary:
            _cleanup_primary_handover(ledger_dir, set(active_targets.values()))
    except Exception as exc:
        print(f"[SCons] WARNING: could not update worktree cache ledger ({exc}).")
