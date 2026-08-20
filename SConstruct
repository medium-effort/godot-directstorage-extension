#!/usr/bin/env python
import os
import sys

EnsureSConsVersion(4, 0)
EnsurePythonVersion(3, 8)

# Delegate environment initialization to godot-cpp/SConstruct
env = SConscript("godot-cpp/SConstruct")

# Add cross-platform include paths. The DirectStorage SDK is Windows-only and
# must not be visible to non-Windows toolchains.
include_paths = [
    "src",
    "thirdparty/gdeflate",
    "thirdparty/libdeflate",
    "thirdparty/libdeflate/lib",
]
if env["platform"] == "windows":
    include_paths.append("thirdparty/directstorage/include")
env.Append(CPPPATH=include_paths)

# Windows platform setup for DirectStorage SDK & MSVC compiler flags
if env["platform"] == "windows":
    env.Append(LIBPATH=["thirdparty/directstorage/lib/x64"])
    env.Append(LIBS=["dstorage.lib", "d3d12.lib", "dxgi.lib"])
    if "msvc" in env["TOOLS"]:
        env.Append(LINKFLAGS=["/DELAYLOAD:dstorage.dll", "delayimp.lib", "/ignore:4099"])
        # Replace /std:c++17 with /std:c++20 to eliminate MSVC warning D9025
        env["CXXFLAGS"] = [flag if flag != "/std:c++17" else "/std:c++20" for flag in env.get("CXXFLAGS", [])]
        if "/std:c++20" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/std:c++20"])
        if "/EHsc" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/EHsc"])

# Source files (GDExtension C++ code + GDeflate & libdeflate sources).
# Keep the Windows backend out of non-Windows builds; its implementation uses
# Win32 and DirectStorage APIs that are not available on those platforms.
project_sources = (
    Glob("src/*.cpp") +
    Glob("src/core/*.cpp") +
    Glob("src/compression/*.cpp") +
    Glob("src/formats/*.cpp") +
    Glob("src/integration/*.cpp") +
    Glob("src/platform/fallback/*.cpp")
)
thirdparty_sources = (
    Glob("thirdparty/gdeflate/*.cpp") +
    Glob("thirdparty/libdeflate/lib/*.c") +
    Glob("thirdparty/libdeflate/lib/x86/*.c")
)
if env["platform"] == "windows":
    project_sources += Glob("src/platform/win32/*.cpp")

# Keep non-critical MSVC warning suppressions scoped to third-party sources so
# legitimate diagnostics in the extension's src/ code remain visible.
thirdparty_objects = thirdparty_sources
if env["platform"] == "windows" and "msvc" in env["TOOLS"]:
    thirdparty_env = env.Clone()
    thirdparty_env.Append(CCFLAGS=["/wd4244", "/wd4127", "/wd4267", "/wd4100", "/wd4245", "/wd4456", "/wd4018", "/wd4146", "/wd4310"])
    thirdparty_objects = thirdparty_env.SharedObject(thirdparty_sources)

sources = project_sources + thirdparty_objects

# Embed GDExtension class reference data in editor/debug builds so Godot can
# show the extension API in the Help window and on Ctrl-click.
if env["target"] in ["editor", "template_debug"]:
    doc_data = env.GodotCPPDocData(
        "src/gen/doc_data.gen.cpp",
        source=Glob("project/addons/directstorage_accelerator/doc_classes/*.xml"),
    )
    sources.append(doc_data)


output_dir = "project/addons/directstorage_accelerator/bin/"
target_name = "directstorage_accelerator" + env["suffix"] + env["SHLIBSUFFIX"]

# Build GDExtension Shared Library (.dll)
library = env.SharedLibrary(
    target=output_dir + target_name,
    source=sources,
)

targets = [library]

# Copy DirectStorage runtime redistributable DLLs into output directory on Windows
if env["platform"] == "windows":
    ds_dlls = [
        "thirdparty/directstorage/bin/x64/dstorage.dll",
        "thirdparty/directstorage/bin/x64/dstoragecore.dll",
    ]
    installed_dlls = env.Install(output_dir, ds_dlls)
    targets.append(installed_dlls)

# Post-build action: generate build status manifest artifact and incremental project sync
import json
import datetime
import subprocess
import atexit
import shutil
import SCons.Script

if sys.platform == "win32":
    import winreg

BUILD_START_TIME = datetime.datetime.now().astimezone()

# Automatic external synchronization to D:\directstorage-test is opt-in via
# ENABLE_EXTERNAL_SYNC and additionally requires this development machine GUID.
TARGET_MACHINE_GUID = "4e2b44a0-d5af-4b59-aa22-7a24d8187a63"
# Keep verification builds workspace-local unless an explicit sync is requested.
ENABLE_EXTERNAL_SYNC = os.environ.get("DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC") == "1"

def get_machine_guid():
    if sys.platform == "win32":
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography") as key:
                guid, _ = winreg.QueryValueEx(key, "MachineGuid")
                return guid
        except Exception:
            pass
    return None

# Automatic post-build synchronization uses this helper only after
# ENABLE_EXTERNAL_SYNC is enabled and the host machine GUID matches
# TARGET_MACHINE_GUID. Explicit sync aliases remain machine-GUID gated; the
# destination may be D:\directstorage-test or another approved sync target.
def sync_changed_files(src_dir, dst_dir, file_filter=None):
    copied_count = 0
    for root, dirs, files in os.walk(src_dir):
        # Exclude .godot cache folder from post-build sync
        dirs[:] = [d for d in dirs if d != ".godot"]

        rel_path = os.path.relpath(root, src_dir)
        target_root = os.path.join(dst_dir, rel_path) if rel_path != "." else dst_dir
        os.makedirs(target_root, exist_ok=True)

        for f in files:
            # Exclude temp files globally
            if f.startswith("~") or f.lower().endswith(".tmp"):
                continue

            rel_file_path = os.path.normpath(os.path.join(rel_path, f)) if rel_path != "." else f
            if file_filter and not file_filter(rel_file_path, f):
                continue

            src_file = os.path.join(root, f)
            dst_file = os.path.join(target_root, f)

            should_copy = False
            if not os.path.exists(dst_file):
                should_copy = True
            else:
                try:
                    src_stat = os.stat(src_file)
                    dst_stat = os.stat(dst_file)
                    if src_stat.st_mtime > dst_stat.st_mtime + 0.1 or src_stat.st_size != dst_stat.st_size:
                        should_copy = True
                except Exception:
                    should_copy = True

            if should_copy:
                try:
                    shutil.copy2(src_file, dst_file)
                    copied_count += 1
                except Exception:
                    pass
    return copied_count

def addon_windows_filter(rel_path, filename):
    fname_lower = filename.lower()
    if filename.startswith("~") or fname_lower.endswith(".tmp"):
        return False

    parts = os.path.normpath(rel_path).split(os.sep)
    if "bin" in parts:
        # Exclude development builds (.dev.)
        if ".dev." in fname_lower:
            return False
        # Only allow Windows DLLs and build manifest inside bin
        if fname_lower.endswith(".dll") or filename == "build_manifest.json":
            return True
        return False

    return True

def src_filter(rel_path, filename):
    fname_lower = filename.lower()
    if filename.startswith("~") or fname_lower.endswith(".tmp"):
        return False
    if fname_lower.endswith((".obj", ".o", ".os")):
        return False
    return True

def clean_excluded_files(dst_dir, file_filter):
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
            elif file_filter and not file_filter(rel_file_path, f):
                should_remove = True

            if should_remove:
                try:
                    os.remove(os.path.join(root, f))
                except Exception:
                    pass

# def clean_stale_parent_addon_files(base_dir):
#     if not os.path.exists(base_dir):
#         return
#     stale_names = [
#         "plugin.cfg",
#         "README.md",
#         "SUPPORTED_PLATFORMS.md",
#         "directstorage_accelerator.gdextension",
#         "directstorage_accelerator.gdextension.uid"
#     ]
#     for name in stale_names:
#         p = os.path.join(base_dir, name)
#         if os.path.isfile(p):
#             try:
#                 os.remove(p)
#             except Exception:
#                 pass

def write_build_manifest():
    # Skip manifest generation and post-build sync on dry-run, clean, help, question, or sync-only invocations
    sync_only_targets = {"sync", "sync-all", "sync-addon", "sync-test"}
    if (
        SCons.Script.GetOption("no_exec")
        or SCons.Script.GetOption("clean")
        or SCons.Script.GetOption("help")
        or SCons.Script.GetOption("question")
        or any(t in sync_only_targets for t in SCons.Script.COMMAND_LINE_TARGETS)
    ):
        return

    build_finish_time = datetime.datetime.now().astimezone()
    duration_seconds = round((build_finish_time - BUILD_START_TIME).total_seconds(), 3)
    manifest_path = os.path.join(output_dir, "build_manifest.json")
    failures = SCons.Script.GetBuildFailures()

    commit_hash = "unknown"
    commit_short = "unknown"
    commit_msg = ""
    commit_date = ""
    is_dirty = False

    try:
        commit_hash = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip()
        commit_short = commit_hash[:7]
        commit_msg = subprocess.check_output(["git", "log", "-1", "--pretty=%s"], text=True).strip()
        commit_date = subprocess.check_output(["git", "log", "-1", "--pretty=%cI"], text=True).strip()
        status_out = subprocess.check_output(["git", "status", "--porcelain"], text=True).strip()
        is_dirty = len(status_out) > 0
    except Exception:
        pass

    status = "FAILED" if failures else "SUCCESS"

    target_sync_dir = r"D:\directstorage-test"
    addon_base_dir = r"D:\directstorage-addon-copyable-windows"
    addon_sync_dir = os.path.join(addon_base_dir, "directstorage_accelerator")
    addon_src_sync_dir = os.path.join(addon_base_dir, "src")
    sync_info = {
        "target_path": target_sync_dir,
        "synced": False,
        "copied_files_count": 0,
        "addon_copyable": {
            "target_path": addon_sync_dir,
            "synced": False,
            "copied_files_count": 0,
            "src_target_path": addon_src_sync_dir,
            "src_copied_files_count": 0
        }
    }

    if status == "SUCCESS" and ENABLE_EXTERNAL_SYNC and os.path.exists("D:\\") and get_machine_guid() == TARGET_MACHINE_GUID:
        try:
            os.makedirs(target_sync_dir, exist_ok=True)
            copied = sync_changed_files(os.path.abspath("project"), target_sync_dir)
            sync_info["synced"] = True
            sync_info["copied_files_count"] = copied
            if copied > 0:
                print(f"[SCons] Post-build sync: Copied/Updated {copied} file(s) -> {target_sync_dir}")
            else:
                print(f"[SCons] Post-build sync: Target {target_sync_dir} is up to date.")
        except Exception as e:
            sync_info["error"] = str(e)
            print(f"[SCons] Post-build sync error: {e}")

        try:
            os.makedirs(addon_sync_dir, exist_ok=True)
            # clean_stale_parent_addon_files(addon_base_dir)
            clean_excluded_files(addon_sync_dir, addon_windows_filter)
            addon_src_dir = os.path.abspath("project/addons/directstorage_accelerator")
            addon_copied = sync_changed_files(addon_src_dir, addon_sync_dir, addon_windows_filter)
            sync_info["addon_copyable"]["synced"] = True
            sync_info["addon_copyable"]["copied_files_count"] = addon_copied
            if addon_copied > 0:
                print(f"[SCons] Post-build sync: Copied/Updated {addon_copied} file(s) -> {addon_sync_dir}")
            else:
                print(f"[SCons] Post-build sync: Target {addon_sync_dir} is up to date.")

            os.makedirs(addon_src_sync_dir, exist_ok=True)
            clean_excluded_files(addon_src_sync_dir, src_filter)
            src_project_dir = os.path.abspath("src")
            src_copied = sync_changed_files(src_project_dir, addon_src_sync_dir, src_filter)
            sync_info["addon_copyable"]["src_copied_files_count"] = src_copied
            if src_copied > 0:
                print(f"[SCons] Post-build sync: Copied/Updated {src_copied} file(s) -> {addon_src_sync_dir}")
            else:
                print(f"[SCons] Post-build sync: Target {addon_src_sync_dir} is up to date.")
        except Exception as e:
            sync_info["addon_copyable"]["error"] = str(e)
            print(f"[SCons] Post-build sync error ({addon_base_dir}): {e}")

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

    # Standard known binary targets for the current platform
    known_targets = {
        "directstorage_accelerator.windows.editor.x86_64.dll": "editor",
        "directstorage_accelerator.windows.template_debug.x86_64.dll": "template_debug",
        "directstorage_accelerator.windows.template_debug.dev.x86_64.dll": "template_debug.dev",
        "directstorage_accelerator.windows.template_release.x86_64.dll": "template_release",
        "directstorage_accelerator.windows.template_release.dev.x86_64.dll": "template_release.dev",
    }
    for kt_name, kt_type in known_targets.items():
        if kt_name not in targets_dict:
            targets_dict[kt_name] = {"target_type": kt_type}

    # Discover any additional compiled shared library binaries in output_dir
    if os.path.exists(output_dir):
        for fname in os.listdir(output_dir):
            if (fname.startswith("directstorage_accelerator") or fname.startswith("libdirectstorage_accelerator")) and (
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
    curr_target_type = env.get("target", "unknown")
    if env.get("dev_build", False):
        curr_target_type += ".dev"

    curr_entry = targets_dict.setdefault(target_name, {})
    curr_entry["target_type"] = curr_target_type
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

    manifest_data = {
        "status": status,
        "last_built_target": target_name,
        "timestamps": {
            "started_at": BUILD_START_TIME.isoformat(),
            "finished_at": build_finish_time.isoformat(),
            "duration_seconds": duration_seconds
        },
        "target": target_name,
        "platform": env.get("platform", "unknown"),
        "git": {
            "commit": commit_hash,
            "short_commit": commit_short,
            "message": commit_msg,
            "date": commit_date,
            "dirty": is_dirty
        },
        "targets": targets_dict,
        "post_build_sync": sync_info
    }
    if failures:
        manifest_data["failures_count"] = len(failures)

    try:
        os.makedirs(os.path.dirname(manifest_path), exist_ok=True)
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest_data, f, indent=2)
        print(f"[SCons] Updated build manifest ({status}): {manifest_path}")

        # Copy the final updated manifest JSON directly to sync target directories
        if sync_info["synced"]:
            dst_manifest = os.path.join(target_sync_dir, os.path.relpath(manifest_path, os.path.abspath("project")))
            os.makedirs(os.path.dirname(dst_manifest), exist_ok=True)
            shutil.copy2(manifest_path, dst_manifest)

        if sync_info.get("addon_copyable", {}).get("synced", False):
            dst_addon_manifest = os.path.join(
                addon_sync_dir,
                os.path.relpath(manifest_path, os.path.abspath("project/addons/directstorage_accelerator"))
            )
            os.makedirs(os.path.dirname(dst_addon_manifest), exist_ok=True)
            shutil.copy2(manifest_path, dst_addon_manifest)
    except Exception as e:
        print(f"[SCons] Failed to write build manifest: {e}")

atexit.register(write_build_manifest)


Default(targets)

# Standalone sync targets (no compilation needed):
#   scons sync        -> Syncs both project to directstorage-test and addon to directstorage-addon-copyable-windows
#   scons sync-addon  -> Syncs directstorage_accelerator addon to directstorage-addon-copyable-windows
#   scons sync-test   -> Syncs project to directstorage-test
#   scons sync-all    -> Same as scons sync
def run_sync_test(target, source, env):
    if not os.path.exists("D:\\") or get_machine_guid() != TARGET_MACHINE_GUID:
        print("[SCons] Sync skipped: Machine GUID or D:\\ drive does not match target environment.")
        return 0
    target_sync_dir = r"D:\directstorage-test"
    try:
        os.makedirs(target_sync_dir, exist_ok=True)
        copied = sync_changed_files(os.path.abspath("project"), target_sync_dir)
        if copied > 0:
            print(f"[SCons] Sync: Copied/Updated {copied} file(s) -> {target_sync_dir}")
        else:
            print(f"[SCons] Sync: Target {target_sync_dir} is up to date.")
        manifest_src = os.path.join(output_dir, "build_manifest.json")
        if os.path.exists(manifest_src):
            dst_manifest = os.path.join(target_sync_dir, os.path.relpath(manifest_src, os.path.abspath("project")))
            os.makedirs(os.path.dirname(dst_manifest), exist_ok=True)
            shutil.copy2(manifest_src, dst_manifest)
    except Exception as e:
        print(f"[SCons] Sync error ({target_sync_dir}): {e}")
        return 1
    return 0

def run_sync_addon(target, source, env):
    if not os.path.exists("D:\\") or get_machine_guid() != TARGET_MACHINE_GUID:
        print("[SCons] Sync skipped: Machine GUID or D:\\ drive does not match target environment.")
        return 0
    addon_base_dir = r"D:\directstorage-addon-copyable-windows"
    addon_sync_dir = os.path.join(addon_base_dir, "directstorage_accelerator")
    addon_src_sync_dir = os.path.join(addon_base_dir, "src")
    try:
        os.makedirs(addon_sync_dir, exist_ok=True)
        # clean_stale_parent_addon_files(addon_base_dir)
        clean_excluded_files(addon_sync_dir, addon_windows_filter)
        addon_src_dir = os.path.abspath("project/addons/directstorage_accelerator")
        addon_copied = sync_changed_files(addon_src_dir, addon_sync_dir, addon_windows_filter)
        if addon_copied > 0:
            print(f"[SCons] Sync: Copied/Updated {addon_copied} file(s) -> {addon_sync_dir}")
        else:
            print(f"[SCons] Sync: Target {addon_sync_dir} is up to date.")
        manifest_src = os.path.join(output_dir, "build_manifest.json")
        if os.path.exists(manifest_src):
            dst_addon_manifest = os.path.join(
                addon_sync_dir,
                os.path.relpath(manifest_src, os.path.abspath("project/addons/directstorage_accelerator"))
            )
            os.makedirs(os.path.dirname(dst_addon_manifest), exist_ok=True)
            shutil.copy2(manifest_src, dst_addon_manifest)

        os.makedirs(addon_src_sync_dir, exist_ok=True)
        clean_excluded_files(addon_src_sync_dir, src_filter)
        src_project_dir = os.path.abspath("src")
        src_copied = sync_changed_files(src_project_dir, addon_src_sync_dir, src_filter)
        if src_copied > 0:
            print(f"[SCons] Sync: Copied/Updated {src_copied} file(s) -> {addon_src_sync_dir}")
        else:
            print(f"[SCons] Sync: Target {addon_src_sync_dir} is up to date.")
    except Exception as e:
        print(f"[SCons] Sync error ({addon_base_dir}): {e}")
        return 1
    return 0

def run_sync_all(target, source, env):
    r1 = run_sync_test(target, source, env)
    r2 = run_sync_addon(target, source, env)
    return max(r1, r2)

alias_sync = env.Alias("sync", [], env.Action(run_sync_all, "[SCons] Syncing all targets..."))
env.AlwaysBuild(alias_sync)

alias_sync_addon = env.Alias("sync-addon", [], env.Action(run_sync_addon, "[SCons] Syncing directstorage-addon-copyable-windows..."))
env.AlwaysBuild(alias_sync_addon)

alias_sync_test = env.Alias("sync-test", [], env.Action(run_sync_test, "[SCons] Syncing directstorage-test..."))
env.AlwaysBuild(alias_sync_test)

alias_sync_all = env.Alias("sync-all", [], env.Action(run_sync_all, "[SCons] Syncing all targets..."))
env.AlwaysBuild(alias_sync_all)
