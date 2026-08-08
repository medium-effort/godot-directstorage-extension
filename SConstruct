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
    Glob("src/**/*.cpp") +
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

Default(library)
