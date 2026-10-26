#!/usr/bin/env python
import os
import re
import sys

import SCons.Script

# Guard: refuse local builds when PREVENT_BUILD_ON_THIS_DEVICE=1 is set.
# This must run before the godot-cpp SConscript (expensive) so the failure
# is immediate. Sync-only / introspection invocations are allowed through.
def _prevent_build_flag_set():
    val = os.environ.get("PREVENT_BUILD_ON_THIS_DEVICE", "").strip().lower()
    if val:
        return val in ("1", "true", "yes", "y", "on")
    # Fall back to .env so the flag also applies when set there but not
    # exported into the environment (mirrors load_env_file() below, which
    # runs later in this file).
    try:
        if os.path.isfile(".env"):
            with open(".env", "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        k, v = line.split("=", 1)
                        if k.strip() == "PREVENT_BUILD_ON_THIS_DEVICE":
                            return v.strip().strip('"').strip("'").lower() in ("1", "true", "yes", "y", "on")
    except Exception:
        pass
    return False


_sync_only = {"sync", "sync-all", "sync-addon", "sync-test", "sync-copyable", "sync-tps", "compiledb"}
_cmd_targets = set(SCons.Script.COMMAND_LINE_TARGETS)
_non_build = bool(
    SCons.Script.GetOption("no_exec")
    or SCons.Script.GetOption("clean")
    or SCons.Script.GetOption("help")
    or SCons.Script.GetOption("question")
    or (_cmd_targets and _cmd_targets <= _sync_only)
)

if _prevent_build_flag_set() and not _non_build:
    print("[SCons] ERROR: Local builds are disabled on this device (PREVENT_BUILD_ON_THIS_DEVICE=1).")
    print("[SCons] Use the remote build script instead: .vscode/build_remote.ps1")
    Exit(1)

if "DSPKG_ENCRYPTION_KEY" in SCons.Script.ARGUMENTS:
    print("[SCons] ERROR: Passing DSPKG_ENCRYPTION_KEY on the command line is deprecated for security (avoids process argument logging).")
    print("[SCons] Use DSPKG_ENCRYPTION_KEY_FILE=<path> or build_remote.ps1 -EncryptionKeyFile instead.")
    Exit(1)

# Pop variables before godot-cpp parses options to avoid UnknownVariables warnings
scons_key_file = SCons.Script.ARGUMENTS.pop("DSPKG_ENCRYPTION_KEY_FILE", None)
keep_key_header = SCons.Script.ARGUMENTS.pop("KEEP_KEY_HEADER", None) in ("1", "true", "yes", True)
reset_header = SCons.Script.ARGUMENTS.pop("DSPKG_KEY_HEADER_RESET", None) in ("1", "true", "yes", True)

# Disallow the mutually exclusive command-line combination (R8-03, R9-10). The environment/
# .env form of DSPKG_ENCRYPTION_KEY_FILE is intentionally ignored in this mode: the on-disk header wins.
if keep_key_header and scons_key_file:
    print("[SCons] ERROR: KEEP_KEY_HEADER=1 and DSPKG_ENCRYPTION_KEY_FILE=<path> cannot be combined on the command line.")
    print("[SCons] Pass KEEP_KEY_HEADER=1 to retain/trust an existing header, or DSPKG_ENCRYPTION_KEY_FILE to generate a new one.")
    Exit(1)

# Precedence: ARGUMENTS > os.environ > .env
# When keep_key_header or reset_header is active, DO NOT scan ambient environment or .env (R7-01, R7-13).
# The caller's header or explicit reset is authoritative.
if not keep_key_header and not reset_header:
    if not scons_key_file:
        scons_key_file = os.environ.get("DSPKG_ENCRYPTION_KEY_FILE")
    if not scons_key_file and os.path.isfile(".env"):
        try:
            # Read .env with utf-8-sig to tolerate Windows UTF-8/UTF-16 BOM (R7-07)
            with open(".env", "r", encoding="utf-8-sig") as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("DSPKG_ENCRYPTION_KEY_FILE="):
                        candidate_file = line.split("=", 1)[1].strip().strip('"').strip("'")
                        if candidate_file:
                            scons_key_file = candidate_file
                        break
        except Exception: pass

# Guard against a genuinely exported DSPKG_ENCRYPTION_KEY leaking into export plugin without key file
# (R7-14, R8-01, R8-02, R9-02). Evaluated strictly AFTER scons_key_file resolution (R8-01), strictly for
# compiling invocations (R8-02), and pre-.env by construction: load_env_file() does not run until line 320,
# so a key declared only in .env (which the Godot editor never reads) can never trip this gate (R9-02).
if not _non_build and "DSPKG_ENCRYPTION_KEY" in os.environ and not scons_key_file and not keep_key_header and not reset_header:
    print("[SCons] ERROR: Ambient DSPKG_ENCRYPTION_KEY is set in environment, but SCons requires DSPKG_ENCRYPTION_KEY_FILE=<path>.")
    print("[SCons] DirectStorage export plugin reads DSPKG_ENCRYPTION_KEY; building a keyless DLL now would cause runtime decryption failure.")
    print("[SCons] Unset DSPKG_ENCRYPTION_KEY or specify DSPKG_ENCRYPTION_KEY_FILE=<path>.")
    Exit(1)

import hashlib
import atexit

def generate_dspkg_key_header_from_file(key_file_path, output_header_path):
    try:
        with open(key_file_path, "r", encoding="utf-8-sig") as f:
            key_hex = f.read().strip()
    except Exception as e:
        print(f"[SCons] ERROR: Failed to read encryption key file ({key_file_path}): {e}")
        Exit(1)

    if len(key_hex) != 64 or not all(c in "0123456789abcdefABCDEF" for c in key_hex):
        print(f"[SCons] ERROR: Encryption key file ({key_file_path}) must contain exactly 64 hexadecimal characters.")
        Exit(1)

    raw = bytes.fromhex(key_hex)
    mask = hashlib.sha256(raw).digest()
    masked = bytes(r ^ m for r, m in zip(raw, mask))
    key_id = hashlib.blake2b(raw, digest_size=8).hexdigest().lower()

    mask_str = ", ".join(f"0x{b:02x}" for b in mask)
    masked_str = ", ".join(f"0x{b:02x}" for b in masked)

    content = (
        "// Auto-generated by build pipeline. DO NOT COMMIT OR EDIT MANUALLY.\n"
        "#pragma once\n\n"
        "#define DSPKG_ENCRYPTION_ENABLED 1\n"
        f'#define DSPKG_KEY_ID_TAG "dspkg-key-id={key_id}"\n\n'
        "namespace dspkg_key {\n"
        f"    constexpr unsigned char MASK[32] = {{\n        {mask_str}\n    }};\n\n"
        f"    constexpr unsigned char MASKED_KEY[32] = {{\n        {masked_str}\n    }};\n\n"
        '    static_assert(sizeof(MASK) == 32 && sizeof(MASKED_KEY) == 32, "Mask and key arrays must be exactly 32 bytes");\n'
        "} // namespace dspkg_key\n"
    )

    os.makedirs(os.path.dirname(output_header_path), exist_ok=True)
    if os.path.exists(output_header_path):
        try:
            with open(output_header_path, "r", encoding="utf-8") as existing_f:
                if existing_f.read().replace("\r\n", "\n") == content:
                    return
        except Exception: pass

    tmp_path = output_header_path + ".tmp"
    try:
        with open(tmp_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        os.replace(tmp_path, output_header_path)
    finally:
        if os.path.exists(tmp_path):
            try: os.remove(tmp_path)
            except Exception: pass

def restore_branch_b_header(output_header_path):
    os.makedirs(os.path.dirname(output_header_path), exist_ok=True)
    branch_b_content = (
        "// Auto-generated bootstrap fallback. Overwritten when key is provided.\n"
        "#pragma once\n\n"
        "#define DSPKG_ENCRYPTION_ENABLED 0\n"
        '#define DSPKG_KEY_ID_TAG "dspkg-key-id=none"\n'
    )
    tmp_path = output_header_path + ".tmp"
    try:
        with open(tmp_path, "w", encoding="utf-8", newline="\n") as f:
            f.write(branch_b_content)
        os.replace(tmp_path, output_header_path)
    finally:
        if os.path.exists(tmp_path):
            try: os.remove(tmp_path)
            except Exception: pass

if scons_key_file and not keep_key_header and not reset_header:
    if not _non_build:
        generate_dspkg_key_header_from_file(scons_key_file, "src/gen/dspkg_encryption_key.h")
        atexit.register(restore_branch_b_header, os.path.abspath("src/gen/dspkg_encryption_key.h"))


EnsureSConsVersion(4, 0)
EnsurePythonVersion(3, 8)

# Delegate environment initialization to godot-cpp/SConstruct
env = SConscript("godot-cpp/SConstruct")

gen_header_dir = os.path.join("src", "gen")
gen_header_file = os.path.join(gen_header_dir, "dspkg_encryption_key.h")
is_keyed_build = bool(scons_key_file or keep_key_header or os.environ.get("DSPKG_KEY_HEADER_ACTIVE") == "1")

# Stage A: Create-if-missing only (runs unconditionally, including for _non_build like --help, compiledb, -n) (R7-03, R8-04)
# This guarantees fresh clones immediately possess Branch B for IDE/clangd without mutating existing state,
# even when .env configures a key file for subsequent compiling builds (R8-04).
if not os.path.exists(gen_header_file) and (_non_build or not is_keyed_build):
    os.makedirs(gen_header_dir, exist_ok=True)
    restore_branch_b_header(gen_header_file)

# Stage B: Full validation, corruption check, and stale reset (runs strictly when compiling)
if not _non_build:
    # Fail-closed guard: keyed build requested but header missing (R7-02)
    if is_keyed_build and not reset_header and not os.path.exists(gen_header_file):
        print("[SCons] ERROR: Keyed build requested (via KEEP_KEY_HEADER / DSPKG_KEY_HEADER_ACTIVE), but src/gen/dspkg_encryption_key.h is missing.")
        print("[SCons] Check that the header was generated on Device A and properly transported.")
        Exit(1)

    needs_bootstrap = True
    if os.path.exists(gen_header_file):
        try:
            with open(gen_header_file, "r", encoding="utf-8") as f:
                hdr_text = f.read()
            import re
            key_id_match = re.search(r'DSPKG_KEY_ID_TAG\s+"dspkg-key-id=([0-9a-fA-F]{16})"', hdr_text)
            # Single source of truth for "keyed header": Stage B and write_build_manifest. The binary audit
            # scanner reuses the same 16-hex id shape but must additionally accept `none` (R9-06).
            if "DSPKG_ENCRYPTION_ENABLED 1" in hdr_text:
                if "MASK[32]" in hdr_text and "MASKED_KEY[32]" in hdr_text and key_id_match is not None:
                    # Only honor Branch A if key is actively configured for this process
                    if is_keyed_build:
                        needs_bootstrap = False
                    else:
                        # Stale Branch A from killed process without key argument: reset to Branch B
                        needs_bootstrap = True
                else:
                    print("[SCons] ERROR: Existing src/gen/dspkg_encryption_key.h is corrupted (missing MASK[32]/MASKED_KEY[32] or a valid 16-hex DSPKG_KEY_ID_TAG).")
                    print("[SCons] Rerun with DSPKG_ENCRYPTION_KEY_FILE=<path> or delete the file to build keyless.")
                    Exit(1)
            elif "DSPKG_ENCRYPTION_ENABLED 0" in hdr_text and re.search(r'DSPKG_KEY_ID_TAG\s+"dspkg-key-id=none"', hdr_text):
                # Branch B must carry the exact keyless tag, not just the macro name: a truncated
                # header ending at `#define DSPKG_KEY_ID_TAG` must fall through to the "no
                # recognizable markers" error instead of passing as a valid keyless header (R10-08).
                if is_keyed_build and not reset_header:
                    # Fail-closed guard: keyed build requested, but header is Branch B (R8-03).
                    # Refuse to silently compile keyless when keyed build was intended.
                    print("[SCons] ERROR: Keyed build requested, but src/gen/dspkg_encryption_key.h contains DSPKG_ENCRYPTION_ENABLED 0 (Branch B).")
                    print("[SCons] Transport or generate the keyed header on Device A, or run without KEEP_KEY_HEADER.")
                    Exit(1)
                needs_bootstrap = False
            else:
                # No recognizable marker (0-byte file, truncated write, foreign content).
                # Keyed builds must fail closed rather than silently downgrade to keyless (R8-03).
                if is_keyed_build and not reset_header:
                    print("[SCons] ERROR: Existing src/gen/dspkg_encryption_key.h has no consistent key-header markers (expected DSPKG_ENCRYPTION_ENABLED 1 with a 16-hex DSPKG_KEY_ID_TAG, or DSPKG_ENCRYPTION_ENABLED 0 with DSPKG_KEY_ID_TAG \"dspkg-key-id=none\").")
                    print("[SCons] Delete the file to build keyless, or regenerate it from the key file / with build_remote.ps1.")
                    Exit(1)
                needs_bootstrap = True
        except Exception as e:
            # Keyed builds fail closed on an unreadable header (R8-03); keyless builds may repair it.
            if is_keyed_build and not reset_header:
                print(f"[SCons] ERROR: Failed to read src/gen/dspkg_encryption_key.h during validation: {e}")
                Exit(1)
            needs_bootstrap = True

    if needs_bootstrap:
        os.makedirs(gen_header_dir, exist_ok=True)
        restore_branch_b_header(gen_header_file)

env.Depends("src/formats/dspkg_format.cpp", gen_header_file)

# Add cross-platform include paths. The DirectStorage SDK is Windows-only and
# must not be visible to non-Windows toolchains.
include_paths = [
    "src",
    "thirdparty/monocypher",
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
        if "/guard:cf" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/guard:cf"])
        if "/guard:cf" not in env["LINKFLAGS"]:
            env.Append(LINKFLAGS=["/guard:cf"])
        if "/Qspectre" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/Qspectre"])
        if "/FS" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/FS"])

# Linux link-time hardening: GNU ld, unlike ld64 on macOS, happily emits a
# shared object whose symbols are still undefined. A missing translation unit
# (e.g. a vendored .c dropped from the source list: issue #0114) therefore linked
# "successfully" and only failed much later, at runtime, when Godot loaded the
# extension with dlopen(RTLD_NOW). Reject undefined symbols at link time instead.
# GDExtension libraries do not link against engine symbols - they receive the
# GDExtensionInterface function table at init - so this only rejects genuinely
# missing objects or libraries (libstdc++/libc are linked normally).
if env["platform"] == "linux":
    env.Append(LINKFLAGS=["-Wl,--no-undefined"])

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

monocypher_sources = Glob("thirdparty/monocypher/monocypher.c")
if env["platform"] == "windows":
    project_sources += Glob("src/platform/win32/*.cpp")

# Keep non-critical MSVC warning suppressions scoped to third-party sources so
# legitimate diagnostics in the extension's src/ code remain visible.
# Monocypher backs DSPkg's TOC encryption (src/formats/dspkg_format.cpp), so it
# belongs to the source set on EVERY platform; only the warning suppressions and
# the C11 std flag are MSVC-specific. Do not reset thirdparty_objects to
# thirdparty_sources here: that silently dropped Monocypher from macOS/Linux
# builds and broke the link with undefined crypto_aead_lock/crypto_aead_unlock/
# crypto_blake2b/crypto_wipe.
thirdparty_objects = thirdparty_sources + monocypher_sources
if env["platform"] == "windows" and "msvc" in env["TOOLS"]:
    thirdparty_env = env.Clone()
    thirdparty_env.Append(CCFLAGS=["/wd4244", "/wd4127", "/wd4267", "/wd4100", "/wd4245", "/wd4456", "/wd4018", "/wd4146", "/wd4310", "/FS"])
    monocypher_env = thirdparty_env.Clone()
    monocypher_env.Append(CFLAGS=["/std:c11"])
    monocypher_objects = monocypher_env.SharedObject(monocypher_sources)
    thirdparty_objects = thirdparty_env.SharedObject(thirdparty_sources) + monocypher_objects

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
# Unified flat-DLL naming scheme (single source of truth for the Windows
# binary names; project/addons/directstorage_accelerator/
# directstorage_accelerator.gdextension must mirror these literally).
# Value-neutral names: the current values ("dsgd", "_64", ".debug") live
# only on the right-hand side so a future rename touches values, not identifiers.
#   ADDON_BINARY_STEM = product stem ("dsgd")
#   ADDON_BINARY_ARCH_SUFFIX = target arch identifier ("_64" for x86_64)
#   ADDON_BINARY_DEBUG_SUFFIX = debug/editor tag (".debug"; release has none)
#   ADDON_BINARY_NAME = full binary stem: product + arch ("dsgd_64")
# Flat root DLL deployment. Every platform uses the same clean 2-binary model
# regardless of dev_build flag (SCons supplies the POSIX "lib" prefix and the
# platform extension via SHLIBPREFIX/SHLIBSUFFIX):
#   template_release -> dsgd_64.dll   / libdsgd_64.so   / libdsgd_64.dylib
#   template_debug / editor -> dsgd_64.debug.dll / libdsgd_64.debug.so /
#                              libdsgd_64.debug.dylib
ADDON_BINARY_STEM = "dsgd"
ADDON_BINARY_ARCH_SUFFIX = "_64"
ADDON_BINARY_DEBUG_SUFFIX = ".debug"
ADDON_BINARY_NAME = ADDON_BINARY_STEM + ADDON_BINARY_ARCH_SUFFIX
if env["target"] == "template_release":
    target_name = ADDON_BINARY_NAME + env["SHLIBSUFFIX"]
elif env["target"] in ["template_debug", "editor"]:
    target_name = ADDON_BINARY_NAME + ADDON_BINARY_DEBUG_SUFFIX + env["SHLIBSUFFIX"]
else:
    # Unknown/custom target: keep the godot-cpp suffix so the variant cannot
    # silently overwrite the two canonical artifacts above.
    target_name = ADDON_BINARY_NAME + env["suffix"] + env["SHLIBSUFFIX"]


# Fail fast when the .gdextension manifest drifts from the ADDON_BINARY_*
# constants above (a mismatch silently breaks editor loading and exports).
# Uses the live constant values, so a rename only needs the manifest updated.
# Skipped for introspection / non-build invocations (--help, dry-run, clean,
# question, compiledb) and the sync-only aliases.
def _assert_dll_naming_consistent():
    skip_targets = {"sync", "sync-all", "sync-addon", "sync-test", "sync-copyable", "sync-tps", "compiledb"}
    if (
        SCons.Script.GetOption("no_exec")
        or SCons.Script.GetOption("clean")
        or SCons.Script.GetOption("help")
        or SCons.Script.GetOption("question")
        or any(t in skip_targets for t in SCons.Script.COMMAND_LINE_TARGETS)
    ):
        return
    # Every platform in the manifest is validated, not just Windows: the stem
    # and debug tag are shared, only the prefix/extension differ.
    _posix = "lib" + ADDON_BINARY_NAME
    expected = {
        "windows.editor.x86_64": ADDON_BINARY_NAME + ADDON_BINARY_DEBUG_SUFFIX + ".dll",
        "windows.debug.x86_64": ADDON_BINARY_NAME + ADDON_BINARY_DEBUG_SUFFIX + ".dll",
        "windows.release.x86_64": ADDON_BINARY_NAME + ".dll",
        "linux.editor.x86_64": _posix + ADDON_BINARY_DEBUG_SUFFIX + ".so",
        "linux.debug.x86_64": _posix + ADDON_BINARY_DEBUG_SUFFIX + ".so",
        "linux.release.x86_64": _posix + ".so",
        "macos.editor": _posix + ADDON_BINARY_DEBUG_SUFFIX + ".dylib",
        "macos.debug": _posix + ADDON_BINARY_DEBUG_SUFFIX + ".dylib",
        "macos.release": _posix + ".dylib",
    }
    manifest_path = os.path.join("project", "addons", "directstorage_accelerator", "directstorage_accelerator.gdextension")
    try:
        with open(manifest_path, "r", encoding="utf-8") as f:
            manifest_text = f.read()
    except OSError as e:
        print(f"[SCons] ERROR: Cannot read {manifest_path}: {e}")
        Exit(1)
    problems = []
    for key, expected_name in expected.items():
        m = re.search(rf'^{re.escape(key)}\s*=\s*"([^"]+)"', manifest_text, re.MULTILINE)
        actual = m.group(1) if m else None
        if actual != f"bin/{expected_name}":
            problems.append(f'{key}: expected "bin/{expected_name}", found "{actual}"')
    if "directstorage_accelerator.windows." in manifest_text:
        problems.append("legacy directstorage_accelerator.windows.* keys still present")
    if problems:
        print("[SCons] ERROR: .gdextension DLL mappings out of sync with ADDON_BINARY_* constants:")
        for problem in problems:
            print(f"[SCons]   - {problem}")
        Exit(1)


_assert_dll_naming_consistent()



# Build GDExtension Shared Library (.dll)
library = env.SharedLibrary(
    target=output_dir + target_name,
    source=sources,
)
env.Depends(library, gen_header_file)

targets = [library]

# Copy DirectStorage runtime redistributable DLLs into output directory on Windows
if env["platform"] == "windows":
    ds_dlls = [
        "thirdparty/directstorage/bin/x64/dstorage.dll",
        "thirdparty/directstorage/bin/x64/dstoragecore.dll",
    ]
    installed_dlls = env.Install(output_dir, ds_dlls)
    targets.append(installed_dlls)

# --- gdextest integration (test-only build target) ---
# Build with: scons tests=true
# Run with:   ./extern/gdextest/gdextest test
# Gated on tests flag so normal builds never require extern/gdextest.
# NOTE: `scons tests=true` still prints godot-cpp's "Unknown SCons variables"
# warning. That check (godot-cpp/SConstruct: UnknownVariables) only knows
# godot-cpp's own variables and runs before this declaration, so the warning
# is expected noise: the flag IS honored below (verified: the test library
# links in the same run that prints it). BoolVariable also normalizes values,
# so `tests=false` is False (raw ARGUMENTS strings are truthy for ANY
# non-empty value, including "false" and "0").
_tests_options = SCons.Script.Variables()
_tests_options.Add(SCons.Script.BoolVariable("tests", "Build the gdextest unit-test library alongside the extension", False))
_tests_options.Update(env)
if env["tests"]:
    if os.path.isfile("extern/gdextest/SConscript"):
        # The test library now compiles in the Win32 backend, so it DOES
        # import dstorage.dll. Keep the main env's /DELAYLOAD:dstorage.dll +
        # delayimp.lib: without them the fixture fails to load the test DLL
        # ("Error 126: module could not be found") because dstorage.dll is not
        # shipped beside the fixture. Delay-load makes it resolve lazily, so
        # non-D3D12 suites run without the DirectStorage runtime present.
        # /ignore:4199 keeps the no-op delay-load warning non-fatal if a
        # future source set stops importing dstorage.dll (spike defect #3).
        _gdex_env = env.Clone()
        _gdex_env.Depends("src/formats/dspkg_format.cpp", gen_header_file)
        if "msvc" in _gdex_env.get("TOOLS", []):
            _gdex_env.Append(LINKFLAGS=["/ignore:4199"])
            _gdex_env.Append(CFLAGS=["/std:c11"])
        _gdex_exports = {"env": _gdex_env, "gdextest": {}}
        if _gdex_env.get("platform", "") != "windows":
            # gdextest has no per-platform sources: .gdextest.toml lists both
            # backends, so a macOS/Linux test lib would compile Win32Backend
            # stubs that the main lib (see project_sources above) excludes.
            # Filter them here so the test build's source closure mirrors the
            # main build. Windows keeps win32 for the D3D12 suites.
            _raw_sources = os.environ.get("gdextest_SOURCES", "")
            if _raw_sources:
                _all = _raw_sources.split(os.pathsep)
                _filtered = [p for p in _all if "src/platform/win32/" not in p.replace("\\", "/")]
                if len(_filtered) != len(_all):
                    _gdex_exports["gdextest"] = {"suites": _filtered}
        gdextest_lib = _gdex_env.SConscript(
            "extern/gdextest/SConscript",
            variant_dir="build/gdextest", duplicate=0,
            # Do NOT pass "suites" here: the SConscript treats a non-empty
            # "suites" list as the complete source set and skips .gdextest.toml
            # discovery. When "suites" is omitted the SConscript falls back to
            # gdextest_SOURCES (set by `gdextest.py test` from the TOML), which
            # includes the extension implementation files (src/core, src/formats,
            # src/integration, src/platform/fallback, etc.) alongside the test
            # files. Without those sources the test dylib links but crashes at
            # runtime on macOS because dyld cannot resolve the extension symbols.
            # (Exception: non-Windows filtering above passes the TOML list minus
            # src/platform/win32, which is equivalent to full discovery.)
            exports=_gdex_exports,
        )
        if gdextest_lib:
            Default(targets, gdextest_lib)
    else:
        print("[SCons] Warning: tests=true requested but extern/gdextest/SConscript is missing; skipping test target.")

# Post-build action: generate build status manifest artifact and incremental project sync
import json
import datetime
import subprocess
import atexit
import shutil
import SCons.Script

# Load configuration from .env file if present
def load_env_file(env_path=".env"):
    if os.path.isfile(env_path):
        try:
            with open(env_path, "r", encoding="utf-8-sig") as f:
                for line in f:
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        k, v = line.split("=", 1)
                        k = k.strip()
                        v = v.strip().strip('"').strip("'")
                        if k and k not in os.environ:
                            os.environ[k] = v
        except Exception:
            pass

load_env_file()

if "DSPKG_ENCRYPTION_KEY" in os.environ and not _non_build:
    print("[SCons] Notice: DSPKG_ENCRYPTION_KEY declared in environment or .env is ignored by SCons. DirectStorage export plugin reads this variable, but SCons builds require DSPKG_ENCRYPTION_KEY_FILE=<path>.")

BUILD_START_TIME = datetime.datetime.now().astimezone()

# Post-build sync targets are environment-driven (see .env.example):
# SYNC_TEST_DIR, SYNC_ADDON_COPYABLE_DIR, SYNC_ADDON_DIR.
# Each target only syncs when its variable is explicitly defined.

# Post-build sync is enabled by default; can be disabled via ENABLE_POST_BUILD_ADDON_SYNC=0
sync_toggle_str = os.environ.get(
    "ENABLE_POST_BUILD_ADDON_SYNC",
    os.environ.get("ENABLE_ADDON_SYNC", os.environ.get("DIRECTSTORAGE_ENABLE_EXTERNAL_SYNC", "1"))
).strip().lower()
ENABLE_EXTERNAL_SYNC = sync_toggle_str not in ("0", "false", "no", "off")

def safe_copy_file(src_file, dst_file):
    """
    Safely copy src_file to dst_file. If dst_file is an in-use Windows DLL locked by a running
    Godot process, catch the PermissionError, rename dst_file to dst_file.old, and retry copy.
    Cleans up unlocked .old files whenever possible.
    """
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

# Automatic post-build synchronization uses this helper only after
# ENABLE_EXTERNAL_SYNC is enabled and the corresponding SYNC_* path is set.
# Explicit sync aliases evaluate the configured environment paths directly.
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
            if f.startswith("~") or f.lower().endswith((".tmp", ".old")):
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
                    safe_copy_file(src_file, dst_file)
                    copied_count += 1
                except Exception as e:
                    print(f"[SCons] Warning: Failed to copy {src_file} -> {dst_file}: {e}")
    return copied_count

def addon_windows_filter(rel_path, filename):
    fname_lower = filename.lower()
    if filename.startswith("~") or fname_lower.endswith((".tmp", ".old")):
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
    if fname_lower == "dspkg_encryption_key.h":
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
            elif fname_lower.endswith(".old"):
                # Clean stale renamed DLL if not in use
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
    if _non_build:
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
        commit_hash = subprocess.check_output(["git", "rev-parse", "HEAD"], text=True, stderr=subprocess.DEVNULL).strip()
        commit_short = commit_hash[:7]
        commit_msg = subprocess.check_output(["git", "log", "-1", "--pretty=%s"], text=True, stderr=subprocess.DEVNULL).strip()
        commit_date = subprocess.check_output(["git", "log", "-1", "--pretty=%cI"], text=True, stderr=subprocess.DEVNULL).strip()
        status_out = subprocess.check_output(["git", "status", "--porcelain"], text=True, stderr=subprocess.DEVNULL).strip()
        is_dirty = len(status_out) > 0
    except Exception:
        pass

    status = "FAILED" if failures else "SUCCESS"

    # Environment-driven sync targets (all optional; skipped when unset).
    target_sync_dir = os.environ.get("SYNC_TEST_DIR", "")
    addon_base_dir = os.environ.get("SYNC_ADDON_COPYABLE_DIR", "")
    addon_sync_dir = os.path.join(addon_base_dir, "directstorage_accelerator") if addon_base_dir else ""
    addon_src_sync_dir = os.path.join(addon_base_dir, "src") if addon_base_dir else ""

    # External addon sync target (configured via SYNC_ADDON_DIR in .env).
    sync_addon_dir = os.environ.get("SYNC_ADDON_DIR", "")

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
        },
        "tps_demo_addon": {
            "target_path": sync_addon_dir,
            "synced": False,
            "copied_files_count": 0
        }
    }

    # NOTE: flat dsgd_64 naming is dev_build-agnostic (dev_build=yes still
    # outputs dsgd_64.dll / dsgd_64.debug.dll), so no canonical alias copy
    # is needed here.

    if status == "SUCCESS" and ENABLE_EXTERNAL_SYNC:
        # 1. Full project mirror (SYNC_TEST_DIR).
        if target_sync_dir:
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

        # 2. Copyable addon + src mirror (SYNC_ADDON_COPYABLE_DIR).
        if addon_base_dir and addon_sync_dir and addon_src_sync_dir:
            try:
                os.makedirs(addon_sync_dir, exist_ok=True)
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

        # 3. External addon target (SYNC_ADDON_DIR).
        if not sync_addon_dir:
            print("[SCons] Post-build sync skipped: SYNC_ADDON_DIR is not configured in .env.")
        else:
            try:
                os.makedirs(sync_addon_dir, exist_ok=True)
                clean_excluded_files(sync_addon_dir, addon_windows_filter)
                addon_src_dir = os.path.abspath("project/addons/directstorage_accelerator")
                tps_copied = sync_changed_files(addon_src_dir, sync_addon_dir, addon_windows_filter)
                sync_info["tps_demo_addon"]["synced"] = True
                sync_info["tps_demo_addon"]["copied_files_count"] = tps_copied
                if tps_copied > 0:
                    print(f"[SCons] Post-build sync: Copied/Updated {tps_copied} file(s) -> {sync_addon_dir}")
                else:
                    print(f"[SCons] Post-build sync: Target {sync_addon_dir} is up to date.")
            except Exception as e:
                sync_info["tps_demo_addon"]["error"] = str(e)
                print(f"[SCons] Post-build sync error ({sync_addon_dir}): {e}")

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
    # derived from the ADDON_BINARY_* naming constants above.
    known_targets = {
        ADDON_BINARY_NAME + ".dll": "template_release",
        ADDON_BINARY_NAME + ADDON_BINARY_DEBUG_SUFFIX + ".dll": "template_debug",
    }
    for kt_name, kt_type in known_targets.items():
        if kt_name not in targets_dict:
            targets_dict[kt_name] = {"target_type": kt_type}

    # Prune legacy long-form binary entries from the pre-dsgd naming scheme.
    for legacy_name in list(targets_dict.keys()):
        if (
            legacy_name.startswith("directstorage_accelerator.windows.")
            or legacy_name.startswith("libdirectstorage_accelerator.")
        ):
            targets_dict.pop(legacy_name, None)

    # Discover any additional compiled shared library binaries in output_dir
    if os.path.exists(output_dir):
        for fname in os.listdir(output_dir):
            if (fname.startswith(ADDON_BINARY_STEM) or fname.startswith("lib" + ADDON_BINARY_STEM)) and (
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
    # NOTE: target_type intentionally omits the .dev suffix — flat dsgd_64
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

    # Per-target "built in this run" test instead of the run-global `status` (R9-07): a
    # multi-target run in which another target fails must still record the key_id of the
    # target that DID compile, otherwise its freshly written DLL is audited against a
    # stale key and the run fails on a false mismatch.
    _built_this_run = (
        os.path.isfile(curr_fpath)
        and os.path.getmtime(curr_fpath) >= (BUILD_START_TIME.timestamp() - 2.0)
    )
    if _built_this_run:
        is_enc = False
        key_id_hex = None
        if os.path.isfile(gen_header_file):
            try:
                with open(gen_header_file, "r", encoding="utf-8") as f:
                    txt = f.read()
                    if "DSPKG_ENCRYPTION_ENABLED 1" in txt:
                        import re
                        m = re.search(r'DSPKG_KEY_ID_TAG\s+"dspkg-key-id=([0-9a-fA-F]{16})"', txt)
                        if m:
                            is_enc = True
                            key_id_hex = m.group(1).lower()
            except Exception: pass
        curr_entry["encryption"] = {
            "enabled": is_enc,
            "key_id": key_id_hex
        }
    elif os.path.isfile(curr_fpath):
        # Target was up-to-date (not rebuilt this run): reconcile encryption metadata
        # directly from the binary's embedded tag so stale manifest entries cannot persist.
        try:
            with open(curr_fpath, "rb") as f:
                bin_data = f.read()
            import re
            m = re.search(b'dspkg-key-id=([0-9a-fA-F]{16}|none)', bin_data)
            if m:
                tag = m.group(1).decode("ascii").lower()
                curr_entry["encryption"] = {
                    "enabled": tag != "none",
                    "key_id": tag if tag != "none" else None,
                }
        except Exception:
            pass
    else:
        curr_entry.pop("encryption", None)

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
        if sync_info["synced"] and target_sync_dir:
            dst_manifest = os.path.join(target_sync_dir, os.path.relpath(manifest_path, os.path.abspath("project")))
            os.makedirs(os.path.dirname(dst_manifest), exist_ok=True)
            safe_copy_file(manifest_path, dst_manifest)

        if sync_info.get("addon_copyable", {}).get("synced", False) and addon_sync_dir:
            dst_addon_manifest = os.path.join(
                addon_sync_dir,
                os.path.relpath(manifest_path, os.path.abspath("project/addons/directstorage_accelerator"))
            )
            os.makedirs(os.path.dirname(dst_addon_manifest), exist_ok=True)
            safe_copy_file(manifest_path, dst_addon_manifest)

        if sync_info.get("tps_demo_addon", {}).get("synced", False) and sync_addon_dir:
            dst_tps_manifest = os.path.join(
                sync_addon_dir,
                os.path.relpath(manifest_path, os.path.abspath("project/addons/directstorage_accelerator"))
            )
            os.makedirs(os.path.dirname(dst_tps_manifest), exist_ok=True)
            safe_copy_file(manifest_path, dst_tps_manifest)
    except Exception as e:
        print(f"[SCons] Failed to write build manifest: {e}")

atexit.register(write_build_manifest)


Default(targets)

# Standalone sync targets (no compilation needed):
#   scons sync          -> Syncs all configured SYNC_* targets
#   scons sync-addon    -> Syncs addon to SYNC_ADDON_DIR and/or SYNC_ADDON_COPYABLE_DIR
#   scons sync-test     -> Syncs project to SYNC_TEST_DIR
#   scons sync-copyable -> Syncs SYNC_ADDON_COPYABLE_DIR (addon + src)
#   scons sync-tps      -> Syncs addon to SYNC_ADDON_DIR
#   scons sync-all      -> Same as scons sync
def run_sync_test(target, source, env):
    target_sync_dir = os.environ.get("SYNC_TEST_DIR", "")
    if not target_sync_dir:
        print("[SCons] Sync skipped: SYNC_TEST_DIR is not configured in .env or environment.")
        return 0
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
            safe_copy_file(manifest_src, dst_manifest)
    except Exception as e:
        print(f"[SCons] Sync error ({target_sync_dir}): {e}")
        return 1
    return 0

def run_sync_copyable(target, source, env):
    addon_base_dir = os.environ.get("SYNC_ADDON_COPYABLE_DIR", "")
    if not addon_base_dir:
        print("[SCons] Sync skipped: SYNC_ADDON_COPYABLE_DIR is not configured in .env or environment.")
        return 0
    addon_sync_dir = os.path.join(addon_base_dir, "directstorage_accelerator")
    addon_src_sync_dir = os.path.join(addon_base_dir, "src")
    try:
        os.makedirs(addon_sync_dir, exist_ok=True)
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
            safe_copy_file(manifest_src, dst_addon_manifest)

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

def run_sync_tps(target, source, env):
    sync_addon_dir = os.environ.get("SYNC_ADDON_DIR", "")
    if not sync_addon_dir:
        print("[SCons] Sync skipped: SYNC_ADDON_DIR is not configured in .env or environment.")
        return 0
    try:
        os.makedirs(sync_addon_dir, exist_ok=True)
        clean_excluded_files(sync_addon_dir, addon_windows_filter)
        addon_src_dir = os.path.abspath("project/addons/directstorage_accelerator")
        copied = sync_changed_files(addon_src_dir, sync_addon_dir, addon_windows_filter)
        if copied > 0:
            print(f"[SCons] Sync: Copied/Updated {copied} file(s) -> {sync_addon_dir}")
        else:
            print(f"[SCons] Sync: Target {sync_addon_dir} is up to date.")
        manifest_src = os.path.join(output_dir, "build_manifest.json")
        if os.path.exists(manifest_src):
            dst_manifest = os.path.join(
                sync_addon_dir,
                os.path.relpath(manifest_src, os.path.abspath("project/addons/directstorage_accelerator"))
            )
            os.makedirs(os.path.dirname(dst_manifest), exist_ok=True)
            safe_copy_file(manifest_src, dst_manifest)
    except Exception as e:
        print(f"[SCons] Sync error ({sync_addon_dir}): {e}")
        return 1
    return 0

def run_sync_addon(target, source, env):
    # Sync every configured addon target; skip gracefully when unset.
    r1 = run_sync_copyable(target, source, env)
    r2 = run_sync_tps(target, source, env)
    # run_sync_* return 0 when skipped, so max() preserves real failures.
    return max(r1, r2)

def run_sync_all(target, source, env):
    r1 = run_sync_test(target, source, env)
    r2 = run_sync_copyable(target, source, env)
    r3 = run_sync_tps(target, source, env)
    return max(r1, r2, r3)

alias_sync = env.Alias("sync", [], env.Action(run_sync_all, "[SCons] Syncing configured targets..."))
env.AlwaysBuild(alias_sync)

alias_sync_addon = env.Alias("sync-addon", [], env.Action(run_sync_addon, "[SCons] Syncing configured addon targets..."))
env.AlwaysBuild(alias_sync_addon)

alias_sync_test = env.Alias("sync-test", [], env.Action(run_sync_test, "[SCons] Syncing test project..."))
env.AlwaysBuild(alias_sync_test)

alias_sync_copyable = env.Alias("sync-copyable", [], env.Action(run_sync_copyable, "[SCons] Syncing copyable addon..."))
env.AlwaysBuild(alias_sync_copyable)

alias_sync_tps = env.Alias("sync-tps", [], env.Action(run_sync_tps, "[SCons] Syncing external addon..."))
env.AlwaysBuild(alias_sync_tps)

alias_sync_all = env.Alias("sync-all", [], env.Action(run_sync_all, "[SCons] Syncing all targets..."))
env.AlwaysBuild(alias_sync_all)
