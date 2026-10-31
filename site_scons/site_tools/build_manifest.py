"""SCons site tool: record build provenance in build_manifest.json.

SCons builds artifacts; this tool records what was built, from which commit and
which key state, so a shipped DLL can be related back to the source that
produced it. It writes only inside the addon bin directory (the shared library
output directory) and never touches another project - deployment lives in
tools/sync_artifacts.py and is driven by the pipeline.

Usage (from SConstruct, after the naming constants and BUILD_START_TIME exist):

    env.Tool("build_manifest")
    env.RegisterBuildManifest(
        output_dir=output_dir,
        target_name=target_name,
        addon_binary_stem=ADDON_BINARY_STEM,
        addon_binary_name=ADDON_BINARY_NAME,
        addon_debug_suffix=ADDON_BINARY_DEBUG_SUFFIX,
        gen_header_file=gen_header_file,
        build_start_time=BUILD_START_TIME,
        skip=_non_build,
    )
"""
import atexit
import datetime
import json
import os
import re
import subprocess

import SCons.Script

# Legacy long-form binary names from the pre-dsgd naming scheme.
_LEGACY_PREFIXES = ("directstorage_accelerator.windows.", "libdirectstorage_accelerator.")

_KEY_ID_RE = re.compile(r'DSPKG_KEY_ID_TAG\s+"dspkg-key-id=([0-9a-fA-F]{16})"')
_KEY_TAG_RE = re.compile(rb"dspkg-key-id=([0-9a-fA-F]{16}|none)")

_STATE = {}


def exists(env):
    return True


def generate(env):
    env.AddMethod(register, "RegisterBuildManifest")


def register(
    env,
    output_dir,
    target_name,
    addon_binary_stem,
    addon_binary_name,
    addon_debug_suffix,
    gen_header_file,
    build_start_time,
    skip,
):
    """Capture the build context and install the atexit writer."""
    if skip:
        return
    _STATE.update(
        env=env,
        output_dir=output_dir,
        target_name=target_name,
        addon_binary_stem=addon_binary_stem,
        addon_binary_name=addon_binary_name,
        addon_debug_suffix=addon_debug_suffix,
        gen_header_file=gen_header_file,
        build_start_time=build_start_time,
    )
    if not _STATE.get("registered"):
        atexit.register(_write)
        _STATE["registered"] = True


def _write():
    state = _STATE
    if not state:
        return

    env = state["env"]
    output_dir = state["output_dir"]
    target_name = state["target_name"]
    addon_binary_stem = state["addon_binary_stem"]
    addon_binary_name = state["addon_binary_name"]
    addon_debug_suffix = state["addon_debug_suffix"]
    gen_header_file = state["gen_header_file"]
    build_start_time = state["build_start_time"]

    build_finish_time = datetime.datetime.now().astimezone()
    duration_seconds = round((build_finish_time - build_start_time).total_seconds(), 3)
    manifest_path = os.path.join(output_dir, "build_manifest.json")
    failures = SCons.Script.GetBuildFailures()

    commit_hash = "unknown"
    commit_short = "unknown"
    commit_msg = ""
    commit_date = ""
    is_dirty = False

    try:
        commit_hash = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL).strip()
        commit_short = commit_hash[:7]
        commit_msg = subprocess.check_output(["git", "log", "-1", "--pretty=%s"], text=True, stderr=subprocess.DEVNULL).strip()
        commit_date = subprocess.check_output(["git", "log", "-1", "--pretty=%cI"], text=True, stderr=subprocess.DEVNULL).strip()
        status_out = subprocess.check_output(["git", "status", "--porcelain"], text=True, stderr=subprocess.DEVNULL).strip()
        is_dirty = len(status_out) > 0
    except Exception:
        pass

    status = "FAILED" if failures else "SUCCESS"

    # Load existing manifest to preserve historical metadata for other targets
    targets_dict = {}
    if os.path.exists(manifest_path):
        try:
            with open(manifest_path, "r", encoding="utf-8") as f:
                old_manifest = json.load(f)
                if isinstance(old_manifest.get("targets"), dict):
                    targets_dict = old_manifest["targets"]
        except Exception:
            pass

    # Standard known binary targets: flat 2-binary model on Windows,
    # derived from the ADDON_BINARY_* naming constants.
    known_targets = {
        addon_binary_name + ".dll": "template_release",
        addon_binary_name + addon_debug_suffix + ".dll": "template_debug",
    }
    for kt_name, kt_type in known_targets.items():
        if kt_name not in targets_dict:
            targets_dict[kt_name] = {"target_type": kt_type}

    # Prune legacy long-form binary entries from the pre-dsgd naming scheme.
    for legacy_name in list(targets_dict.keys()):
        if legacy_name.startswith(_LEGACY_PREFIXES):
            targets_dict.pop(legacy_name, None)

    # Discover any additional compiled shared library binaries in output_dir
    if os.path.exists(output_dir):
        for fname in os.listdir(output_dir):
            if (fname.startswith(addon_binary_stem) or fname.startswith("lib" + addon_binary_stem)) and (
                fname.endswith(".dll") or fname.endswith(".so") or fname.endswith(".dylib")
            ):
                if fname not in targets_dict:
                    targets_dict[fname] = {}

    # Inspect all tracked targets on disk for existence, size, and last modification timestamp
    for fname, entry in list(targets_dict.items()):
        fpath = os.path.join(output_dir, fname)
        if os.path.isfile(fpath):
            try:
                st = os.stat(fpath)
                mtime_dt = datetime.datetime.fromtimestamp(st.st_mtime, tz=datetime.timezone.utc).astimezone()
                entry["exists"] = True
                entry["size_bytes"] = st.st_size
                entry["last_modified"] = mtime_dt.isoformat()
            except Exception:
                entry["exists"] = True
        else:
            entry["exists"] = False
            entry.pop("size_bytes", None)

    # Update metadata specifically for the target compiled in the current SCons run
    # NOTE: target_type intentionally omits the .dev suffix - flat dsgd_64
    # naming is identical for dev and non-dev builds.
    curr_target_type = env.get("target", "unknown")

    curr_entry = targets_dict.setdefault(target_name, {})
    curr_entry["target_type"] = curr_target_type
    curr_entry["is_dev_build"] = bool(env.get("dev_build", False))
    curr_entry.pop("aliased_from", None)
    curr_fpath = os.path.join(output_dir, target_name)
    if os.path.isfile(curr_fpath):
        try:
            st = os.stat(curr_fpath)
            mtime_dt = datetime.datetime.fromtimestamp(st.st_mtime, tz=datetime.timezone.utc).astimezone()
            curr_entry["exists"] = True
            curr_entry["size_bytes"] = st.st_size
            curr_entry["last_modified"] = mtime_dt.isoformat()
        except Exception:
            curr_entry["exists"] = True
    else:
        curr_entry["exists"] = False

    curr_entry["last_build_status"] = status
    curr_entry["last_commit"] = commit_short
    curr_entry["last_built_at"] = build_finish_time.isoformat()
    curr_entry["last_build_duration_seconds"] = duration_seconds

    # Per-target "built in this run" test instead of the run-global status: a
    # multi-target run in which another target fails must still record the key_id
    # of the target that DID compile, otherwise its freshly written DLL is audited
    # against a stale key and the run fails on a false mismatch.
    built_this_run = (
        os.path.isfile(curr_fpath)
        and os.path.getmtime(curr_fpath) >= (build_start_time.timestamp() - 2.0)
    )
    if built_this_run:
        is_enc = False
        key_id_hex = None
        if os.path.isfile(gen_header_file):
            try:
                with open(gen_header_file, "r", encoding="utf-8") as f:
                    txt = f.read()
                    if "DSPKG_ENCRYPTION_ENABLED 1" in txt:
                        match = _KEY_ID_RE.search(txt)
                        if match:
                            is_enc = True
                            key_id_hex = match.group(1).lower()
            except Exception:
                pass
        curr_entry["encryption"] = {
            "enabled": is_enc,
            "key_id": key_id_hex,
            # Provenance: where this binary's key came from. "baked" means the key
            # was compiled in by a source build; "none" means no key is in the DLL.
            "key_source": "baked" if is_enc else "none",
        }
    elif os.path.isfile(curr_fpath):
        # Target was up-to-date (not rebuilt this run): reconcile encryption metadata
        # directly from the binary's embedded tag so stale manifest entries cannot persist.
        try:
            with open(curr_fpath, "rb") as f:
                bin_data = f.read()
            match = _KEY_TAG_RE.search(bin_data)
            if match:
                tag = match.group(1).decode("ascii").lower()
                curr_entry["encryption"] = {
                    "enabled": tag != "none",
                    "key_id": tag if tag != "none" else None,
                    "key_source": "none" if tag == "none" else "baked",
                }
        except Exception:
            pass
    else:
        curr_entry.pop("encryption", None)

    manifest_data = {
        "status": status,
        "last_built_target": target_name,
        "timestamps": {
            "started_at": build_start_time.isoformat(),
            "finished_at": build_finish_time.isoformat(),
            "duration_seconds": duration_seconds,
        },
        "target": target_name,
        "platform": env.get("platform", "unknown"),
        "git": {
            "commit": commit_hash,
            "short_commit": commit_short,
            "message": commit_msg,
            "date": commit_date,
            "dirty": is_dirty,
        },
        "targets": targets_dict,
    }
    if failures:
        manifest_data["failures_count"] = len(failures)

    try:
        os.makedirs(os.path.dirname(manifest_path), exist_ok=True)
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest_data, f, indent=2)
        print(f"[SCons] Updated build manifest ({status}): {manifest_path}")
    except Exception as exc:
        print(f"[SCons] Failed to write build manifest: {exc}")
