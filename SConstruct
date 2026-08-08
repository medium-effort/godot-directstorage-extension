#!/usr/bin/env python
import os
import sys

EnsureSConsVersion(4, 0)
EnsurePythonVersion(3, 8)

# Delegate environment initialization to godot-cpp/SConstruct
env = SConscript("godot-cpp/SConstruct")

# Add custom include paths for DirectStorage, GDeflate, libdeflate, and src
env.Append(CPPPATH=[
    "src",
    "thirdparty/directstorage/include",
    "thirdparty/gdeflate",
    "thirdparty/libdeflate",
    "thirdparty/libdeflate/lib",
])

# Windows platform setup for DirectStorage SDK & MSVC compiler flags
if env["platform"] == "windows":
    env.Append(LIBPATH=["thirdparty/directstorage/lib/x64"])
    env.Append(LIBS=["dstorage.lib"])
    if "msvc" in env["TOOLS"]:
        env.Append(LINKFLAGS=["/DELAYLOAD:dstorage.dll", "delayimp.lib"])
        # Replace /std:c++17 with /std:c++20 to eliminate MSVC warning D9025
        env["CXXFLAGS"] = [flag if flag != "/std:c++17" else "/std:c++20" for flag in env.get("CXXFLAGS", [])]
        if "/std:c++20" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/std:c++20"])
        if "/EHsc" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/EHsc"])
        # Suppress non-critical MSVC warnings in third-party libdeflate C code
        env.Append(CCFLAGS=["/wd4244", "/wd4127", "/wd4267", "/wd4100", "/wd4245", "/wd4456", "/wd4018", "/wd4146", "/wd4310"])

# Source files (GDExtension C++ code + GDeflate & libdeflate sources)
sources = (
    Glob("src/*.cpp") +
    Glob("src/*/*.cpp") +
    Glob("src/*/*/*.cpp") +
    Glob("thirdparty/gdeflate/*.cpp") +
    Glob("thirdparty/libdeflate/lib/*.c") +
    Glob("thirdparty/libdeflate/lib/x86/*.c")
)


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

TARGET_MACHINE_GUID = "4e2b44a0-d5af-4b59-aa22-7a24d8187a63"

def get_machine_guid():
    if sys.platform == "win32":
        try:
            with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Cryptography") as key:
                guid, _ = winreg.QueryValueEx(key, "MachineGuid")
                return guid
        except Exception:
            pass
    return None

def sync_changed_files(src_dir, dst_dir):
    copied_count = 0
    for root, dirs, files in os.walk(src_dir):
        # Exclude .godot cache folder from post-build sync
        dirs[:] = [d for d in dirs if d != ".godot"]

        rel_path = os.path.relpath(root, src_dir)
        target_root = os.path.join(dst_dir, rel_path) if rel_path != "." else dst_dir
        os.makedirs(target_root, exist_ok=True)

        for f in files:
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

def write_build_manifest():
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
    sync_info = {
        "target_path": target_sync_dir,
        "synced": False,
        "copied_files_count": 0
    }

    if status == "SUCCESS" and os.path.exists("D:\\") and get_machine_guid() == TARGET_MACHINE_GUID:
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

    manifest_data = {
        "status": status,
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
        "post_build_sync": sync_info
    }
    if failures:
        manifest_data["failures_count"] = len(failures)

    try:
        os.makedirs(os.path.dirname(manifest_path), exist_ok=True)
        with open(manifest_path, "w", encoding="utf-8") as f:
            json.dump(manifest_data, f, indent=2)
        print(f"[SCons] Updated build manifest ({status}): {manifest_path}")

        # Copy the final updated manifest JSON directly to sync target directory
        if sync_info["synced"]:
            dst_manifest = os.path.join(target_sync_dir, os.path.relpath(manifest_path, os.path.abspath("project")))
            os.makedirs(os.path.dirname(dst_manifest), exist_ok=True)
            shutil.copy2(manifest_path, dst_manifest)
    except Exception as e:
        print(f"[SCons] Failed to write build manifest: {e}")

atexit.register(write_build_manifest)


Default(targets)



