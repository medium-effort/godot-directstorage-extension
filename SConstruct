#!/usr/bin/env python
import hashlib
import json
import os
import re
import shutil
import sys
import time

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


def _dotenv_or_env_value(key):
    """Read an environment variable, falling back to .env.

    load_env_file() does not run until much later in this file, and the
    construction environment snapshots ENV before that, so this fallback is the
    only way these settings reach the compiler subprocess or godot-cpp.
    """
    val = os.environ.get(key, "").strip()
    if val:
        return val
    try:
        if os.path.isfile(".env"):
            with open(".env", "r", encoding="utf-8") as env_file:
                for line in env_file:
                    line = line.strip()
                    if line and not line.startswith("#") and "=" in line:
                        name, value = line.split("=", 1)
                        if name.strip() == key:
                            return value.strip().strip('"').strip("'")
    except Exception:
        pass
    return ""


# godot-cpp enables its shared object cache (CacheDir + MD5 decider) when
# SCONS_CACHE is in the environment, but it reads that inside its own SConscript
# - before this file's load_env_file() runs. Resolving it from .env here means a
# bare `scons` run in a worktree gets the same shared cache as a
# build_remote.ps1 run. The cache is what makes a new worktree's first build
# cheap: SCons signatures are content/command based and path independent, so an
# object built in one worktree is retrieved by another.
if not os.environ.get("SCONS_CACHE"):
    _scons_cache_setting = _dotenv_or_env_value("SCONS_CACHE")
    if _scons_cache_setting:
        os.environ["SCONS_CACHE"] = _scons_cache_setting


# --- Optional prebuilt godot-cpp ---------------------------------------------
# GODOT_CPP_PREBUILT=1 builds godot-cpp once (in the primary worktree) and reuses
# its static library from every worktree, instead of recompiling ~1,000 generated
# class translation units + a ~436 MB dev library per worktree. That is the only
# per-worktree cost SCons' cache cannot remove: a cache hit still materialises a
# real .obj/.lib inside the tree (the linker needs a file there, and SCons never
# hardlinks), so a built worktree costs ~1 GB regardless of hits.
#
# Requirements, all checked below rather than assumed:
#   * godot-cpp/bin/<libgodot-cpp...>.lib present - the worktree bootstrap
#     hardlinks it from the shared store, costing no extra bytes;
#   * godot-cpp/gen/include present - generated once, junctioned by the bootstrap;
#   * the same target/dev_build and godot-cpp submodule commit as the provider.
# build_library=no must be forced before godot-cpp reads its options, hence here.
_godot_cpp_prebuilt = _dotenv_or_env_value("GODOT_CPP_PREBUILT").lower() in ("1", "true", "yes", "y", "on")
if _godot_cpp_prebuilt:
    SCons.Script.ARGUMENTS["build_library"] = "no"


_sync_only = {"compiledb"}
_cmd_targets = set(SCons.Script.COMMAND_LINE_TARGETS)

# 'scons sync*' targets were removed: deployment is .vscode/sync_addon.ps1 ->
# tools/sync_artifacts.py. Fail before any build target is registered, so a
# leftover habit cannot overwrite the provenance of the last good build in
# build_manifest.json (the manifest writer is registered much later).
_removed_sync_targets = {"sync", "sync-all", "sync-addon", "sync-test", "sync-copyable", "sync-tps"}
_removed_hits = sorted(_cmd_targets & _removed_sync_targets)
if _removed_hits:
    print(f"[SCons] ERROR: target(s) {', '.join(_removed_hits)} were removed.")
    print("[SCons] Deployment is no longer a build target. Run instead:")
    print("[SCons]   powershell -ExecutionPolicy Bypass -File .vscode/sync_addon.ps1")
    print("[SCons]   (add -Targets all for every configured mirror, -DryRun to preview)")
    Exit(2)

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

# Guard against a stale ambient DSPKG_ENCRYPTION_KEY: nothing reads that variable any more, so a build
# that accepted it would misrepresent how its key reached the game (R7-14, R8-01, R8-02, R9-02).
# Evaluated strictly AFTER scons_key_file resolution (R8-01), strictly for compiling invocations
# (R8-02), and pre-.env by construction: load_env_file() does not run until line 320, so a key
# declared only in .env (which the Godot editor never reads) can never trip this gate (R9-02).
if not _non_build and "DSPKG_ENCRYPTION_KEY" in os.environ and not scons_key_file and not keep_key_header and not reset_header:
    print("[SCons] ERROR: Ambient DSPKG_ENCRYPTION_KEY is set in environment, but SCons requires DSPKG_ENCRYPTION_KEY_FILE=<path>.")
    print("[SCons] Nothing reads DSPKG_ENCRYPTION_KEY any more: the export plugin generates a random archive key per export and delivers it inside the encrypted PCK.")
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

if _godot_cpp_prebuilt:
    # godot-cpp appends its library to LIBS even with build_library=no, so the
    # hardlinked file is what the extension links against. Verify it here: an
    # absent lib would otherwise surface as an unrelated LNK1104 much later.
    #
    # Introspection invocations (compiledb, -n, --clean, --help, --question) are
    # exempt from the library check: they never compile or link, so a checkout can
    # legitimately probe a variant the primary has not built. That is not
    # hypothetical - the hardening gate (.vscode/check_build_flags.ps1) runs
    # `scons compiledb` with SCons' default target=template_debug dev_build=no,
    # so requiring the library would make the gate fail on a tree whose local
    # builds all use dev_build=yes. The generated headers are still required,
    # because every compile command in the database resolves through them.
    if not os.path.isdir(os.path.join("godot-cpp", "gen", "include")):
        print("[SCons] ERROR: GODOT_CPP_PREBUILT=1 but godot-cpp/gen/include is missing.")
        print("[SCons] Re-run tools/wt_share_godot_cpp.ps1 <primary> <worktree> to junction the shared generated tree.")
        Exit(1)
    if _non_build:
        print("[SCons] prebuilt godot-cpp: library presence not checked for an introspection/non-build invocation.")
    else:
        _prebuilt_libs = [lib for lib in env.get("LIBS", []) if "libgodot-cpp" in str(lib)]
        _prebuilt_paths = []
        for _lib in _prebuilt_libs:
            _lib_path = _lib.get_abspath() if hasattr(_lib, "get_abspath") else str(_lib)
            if not os.path.isfile(_lib_path):
                print(f"[SCons] ERROR: GODOT_CPP_PREBUILT=1 but the prebuilt library is missing:\n[SCons]   {_lib_path}")
                print("[SCons] Build that target/dev_build variant in the primary worktree first, then re-run")
                print("[SCons] tools/wt_share_godot_cpp.ps1 <primary> <worktree> to hardlink it here.")
                print("[SCons] (tools/wt_bootstrap_env.ps1 only writes this worktree's .env - it cannot link a library.)")
                Exit(1)
            _prebuilt_paths.append(_lib_path)
        if not _prebuilt_paths:
            print("[SCons] ERROR: GODOT_CPP_PREBUILT=1 but godot-cpp appended no library to LIBS.")
            Exit(1)
        for _lib_path in _prebuilt_paths:
            print(f"[SCons] reusing prebuilt godot-cpp library: {_lib_path}")
else:
    # Inverse safety check. A worktree provisioned for prebuilt mode has
    # godot-cpp/gen junctioned to the shared store and its libraries hardlinked
    # from it. Building godot-cpp there would write objects through the junction
    # and relink the hardlinked .lib IN PLACE, corrupting the copy every other
    # worktree links against.
    #
    # Only linked worktrees are guarded. The primary checkout is the *owner* of
    # those artifacts: its godot-cpp/bin libraries are the hardlink targets the
    # store points at, so nlink > 1 there is normal and rebuilding them in place
    # is exactly what the primary is for (and is how a new godot-cpp reaches every
    # worktree). Guarding it would break ad-hoc `scons` and `scons compiledb` in
    # the main checkout. `.git` is a directory in the primary and a file in a
    # linked worktree, which is what distinguishes them here.
    # Non-build invocations (compiledb, dry run, --clean) never compile, so they
    # cannot corrupt anything and stay allowed in a worktree too.
    _is_linked_worktree = os.path.isfile(".git")
    _shared_markers = []
    if os.path.isfile(os.path.join("godot-cpp", "gen", ".stamp")):
        _shared_markers.append("godot-cpp/gen is the shared generated tree (junction)")
    _bin_dir = os.path.join("godot-cpp", "bin")
    if os.path.isdir(_bin_dir):
        for _entry in os.listdir(_bin_dir):
            if not _entry.endswith(".lib"):
                continue
            try:
                if os.stat(os.path.join(_bin_dir, _entry)).st_nlink > 1:
                    _shared_markers.append(f"godot-cpp/bin/{_entry} is hardlinked to the shared store")
            except OSError:
                pass
    if _shared_markers and _is_linked_worktree and not _non_build:
        print("[SCons] ERROR: this worktree shares godot-cpp with the primary worktree, but GODOT_CPP_PREBUILT is not set:")
        for _marker in _shared_markers:
            print(f"[SCons]   - {_marker}")
        print("[SCons] Building godot-cpp here would corrupt the shared copy. Either set GODOT_CPP_PREBUILT=1 in .env,")
        print("[SCons] or remove godot-cpp/gen and godot-cpp/bin/*.lib from this worktree to build it locally.")
        Exit(1)


# --- Optional compiler-cache wrapper (sccache / buildcache) -------------------
# Opt in per worktree with USE_CC_WRAPPER=sccache|buildcache (process env or
# .env). USE_SCCACHE=1 is kept as a backwards-compatible alias for sccache.
#
# NOTE (measured): neither wrapper is the shared cache for parallel worktrees -
# use SCONS_CACHE for that. sccache hashes MSVC's preprocessed output, which
# carries `#line "D:\\...\\<worktree>\\src\\..."` entries that SCCACHE_BASEDIRS
# cannot strip (it strips plain paths, not backslash-escaped ones) and /pathmap
# does not rewrite, so two worktrees miss on identical sources. buildcache is the
# only one of the two that can share across worktrees, because its
# BUILDCACHE_ACCURACY=DEFAULT stops counting debug-info paths in the key.
#
# Both wrappers need /Z7 instead of /Zi: godot-cpp's common_compiler_flags.py
# adds /Zi with no per-object /Fd, i.e. every object writes the same vc140.pdb,
# which is not cacheable (sccache reports it as "Non-cacheable reasons: shared
# pdb"; buildcache's MSVC docs require Embedded/C7 debug info too).
#
# Applied *after* the godot-cpp SConscript deliberately: every node created
# there keeps a reference to this same construction environment, so mutating it
# here also changes their compile commands (verified with `scons -n`). Doing it
# earlier is impossible - godot-cpp runs `env.Tool("msvc")`, which resets CC to
# `cl` after any wrapper placed before it.
_WRAPPER_TOOLS = {
    # name: (env var holding an explicit exe path, env vars passed through)
    "sccache": (
        "SCCACHE_EXE",
        ("SCCACHE_DIR", "SCCACHE_CACHE_SIZE", "SCCACHE_LOG", "SCCACHE_ERROR_LOG"),
    ),
    "buildcache": (
        "BUILDCACHE_EXE",
        ("BUILDCACHE_DIR", "BUILDCACHE_ACCURACY", "BUILDCACHE_COMPRESS",
         "BUILDCACHE_COMPRESS_FORMAT", "BUILDCACHE_HARD_LINKS",
         "BUILDCACHE_MAX_CACHE_SIZE", "BUILDCACHE_MAX_LOCAL_ENTRY_SIZE",
         "BUILDCACHE_DIRECT_MODE", "BUILDCACHE_DEBUG", "BUILDCACHE_LOG_FILE",
         "BUILDCACHE_STAT_ID"),
    ),
}


def _cc_wrapper_kind():
    """Return 'sccache', 'buildcache', or '' when no wrapper is requested."""
    kind = _dotenv_or_env_value("USE_CC_WRAPPER").strip().lower()
    if kind in _WRAPPER_TOOLS:
        return kind
    if kind not in ("", "none", "0", "off", "false"):
        print(f"[SCons] WARNING: unknown USE_CC_WRAPPER={kind!r}; expected 'sccache' or 'buildcache'. Compiling without a wrapper.")
        return ""
    if _dotenv_or_env_value("USE_SCCACHE").lower() in ("1", "true", "yes", "y", "on"):
        return "sccache"
    return ""


def _cc_wrapper_exe(kind):
    """Explicit path from .env first (the tools may live in a gitignored dir), then PATH."""
    explicit = _dotenv_or_env_value(_WRAPPER_TOOLS[kind][0])
    if explicit:
        return explicit if os.path.isfile(explicit) else ""
    return shutil.which(kind)


_cc_wrapper = _cc_wrapper_kind()
if _cc_wrapper:
    _cc_wrapper_exe_path = _cc_wrapper_exe(_cc_wrapper)
    if not _cc_wrapper_exe_path:
        print(f"[SCons] WARNING: USE_CC_WRAPPER={_cc_wrapper} but the executable was not found (set {_WRAPPER_TOOLS[_cc_wrapper][0]} or put it on PATH); compiling without it.")
    else:
        def _wrap_compiler_var(_name):
            """Prepend the wrapper to one compiler variable, once.

            Skips variables that are aliases of another one: the MSVC tool sets
            CXX = "$CC" (and SHCC = "$CC", SHCXX = "$CXX"), so prepending here
            would expand to "<wrapper> <wrapper> cl" and the wrapper would treat
            itself as the compiler (sccache: "Compiler not supported: unexpected
            argument '-E'"). The alias picks the wrapper up from the variable it
            points at, so wrapping CC is enough for CXX/SHCC/SHCXX.
            """
            _current = env[_name]
            if isinstance(_current, str):
                _current = [_current]
            _current = list(_current)
            if len(_current) == 1 and str(_current[0]).startswith(("$", "${")):
                return False
            if _current and os.path.normcase(str(_current[0])) == os.path.normcase(_cc_wrapper_exe_path):
                return False
            env[_name] = [_cc_wrapper_exe_path] + _current
            return True

        _wrapped = [_n for _n in ("CC", "CXX") if _wrap_compiler_var(_n)]
        if not _wrapped:
            print(f"[SCons] WARNING: USE_CC_WRAPPER={_cc_wrapper} but neither CC nor CXX could be wrapped; compiling without it.")
        else:
            if "ENV" not in env:
                env["ENV"] = dict(os.environ)
            for _passthrough in _WRAPPER_TOOLS[_cc_wrapper][1]:
                _value = _dotenv_or_env_value(_passthrough)
                if _value:
                    env["ENV"][_passthrough] = _value
            if _cc_wrapper == "sccache":
                env["ENV"]["SCCACHE_BASEDIRS"] = os.path.abspath(".")
                # A dead cache daemon must degrade to a plain compile, never fail a build.
                env["ENV"]["SCCACHE_IGNORE_SERVER_IO_ERROR"] = "1"
            for _flag_var in ("CCFLAGS", "CXXFLAGS", "SHCCFLAGS", "SHCXXFLAGS"):
                if _flag_var not in env:
                    continue
                _flags = list(env[_flag_var])
                _swapped = False
                for _i, _flag in enumerate(_flags):
                    if str(_flag).strip().lower() == "/zi":
                        _flags[_i] = "/Z7"
                        _swapped = True
                if _swapped:
                    env[_flag_var] = _flags
            print(f"[SCons] compiler cache wrapper enabled: {_cc_wrapper} -> {_cc_wrapper_exe_path} via {'+'.join(_wrapped)} (/Zi swapped to /Z7)")


gen_header_dir = os.path.join("src", "gen")
gen_header_file = os.path.join(gen_header_dir, "dspkg_encryption_key.h")
is_keyed_build = bool(scons_key_file or keep_key_header or os.environ.get("DSPKG_KEY_HEADER_ACTIVE") == "1")

# Deprecated shape: the supported model generates a random archive key per export
# and delivers it inside the encrypted PCK, so no key belongs in the binary. Warn
# here too, because a direct `scons` run never goes through .vscode/build_remote.ps1
# (which warns and sets DSPKG_BAKE_KEY_WARNED to avoid a duplicate line). Phrased for
# anyone building from source, not only for whoever packages the addon.
if not _non_build and is_keyed_build and os.environ.get("DSPKG_BAKE_KEY_WARNED") != "1":
    print("[SCons] WARNING: DEPRECATED BUILD SHAPE - a fixed archive key is baked into this DLL. The supported path delivers a random archive key inside the encrypted PCK. Whatever you ship from this build carries the key, so rebuild without a key input before releasing.")

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
            # Single source of truth for "keyed header": Stage B and the build_manifest site tool.
            # The binary audit scanner reuses the same 16-hex id shape but must additionally accept `none` (R9-06).
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
    # The DirectStorage import library is EULA-bound: the package's
    # distributable_files.txt lists only dstorage.dll and dstoragecore.dll, and
    # native/lib/ is not covered by the MIT header license. It is therefore
    # fetched (tools/fetch_directstorage.ps1) and never committed. Check it before
    # the library is registered so a clean checkout fails with the fetch command
    # instead of a later LNK1104. Introspection invocations (compiledb, -n,
    # --clean, --help, --question) never link and are exempt, which also keeps the
    # hardening gate's `scons compiledb` runnable without the SDK.
    _directstorage_lib = "thirdparty/directstorage/lib/x64/dstorage.lib"
    if not _non_build and not os.path.isfile(_directstorage_lib):
        print("[SCons] ERROR: DirectStorage import library is missing:")
        print(f"[SCons]   {_directstorage_lib}")
        print("[SCons] It is EULA-bound and is not committed to the repository. Fetch the pinned SDK:")
        print("[SCons]   powershell -NoProfile -ExecutionPolicy Bypass -File tools/fetch_directstorage.ps1")
        print("[SCons] or run the VS Code task 'Fetch DirectStorage Runtime'.")
        Exit(1)
    env.Append(LIBPATH=["thirdparty/directstorage/lib/x64"])
    env.Append(LIBS=["dstorage.lib", "d3d12.lib", "dxgi.lib", "version.lib", "bcrypt.lib"])
    if "msvc" in env["TOOLS"]:
        env.Append(LINKFLAGS=["/DELAYLOAD:dstorage.dll", "delayimp.lib", "/ignore:4099"])
        # Replace /std:c++17 with /std:c++20 to eliminate MSVC warning D9025
        env["CXXFLAGS"] = [flag if flag != "/std:c++17" else "/std:c++20" for flag in env.get("CXXFLAGS", [])]
        if "/std:c++20" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/std:c++20"])
        if "/EHsc" not in env["CXXFLAGS"]:
            env.Append(CXXFLAGS=["/EHsc"])
        # /guard:cf and /Qspectre are code-generation flags, and this extension compiles
        # attacker-reachable C (libdeflate's GDeflate decoder, Monocypher) as well as C++.
        # SCons expands $CFLAGS/$CCFLAGS on the C compile line and $CXXFLAGS/$CCFLAGS on
        # the C++ line, so a flag that lives only in CXXFLAGS never reaches a .c file:
        # keep both here, where every translation unit picks them up exactly once.
        # .vscode/check_build_flags.ps1 reads the emitted command lines and fails the build
        # if that stops being true.
        if "/guard:cf" not in env["CCFLAGS"]:
            env.Append(CCFLAGS=["/guard:cf"])
        if "/guard:cf" not in env["LINKFLAGS"]:
            env.Append(LINKFLAGS=["/guard:cf"])
        if "/Qspectre" not in env["CCFLAGS"]:
            env.Append(CCFLAGS=["/Qspectre"])
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


# --- Build signature (SCons cache diagnostics) --------------------------------
# SCons discards a cached object whenever the command that would rebuild it no
# longer matches what was recorded. A changed compiler, flag or define therefore
# invalidates EVERY object at once, and a warm ~3 minute CI job becomes a ~25
# minute cold one (godot-cpp's ~1,100 generated translation units dominate).
# SCons reports that per object, never globally, and --debug=explain needs one
# line per rebuilt target to say it. Recording the inputs that decide the compile
# command next to the shared object cache lets the next run diff them and print
# one delta line per changed field, which is what the CI cache summary reports.
#
# The cache is restored before SCons starts, so the previous signature is already
# on disk. Only real build invocations write it: a `compiledb` or -n run must not
# claim to have produced objects it never built.
def _build_signature_value(name):
    try:
        return str(env.subst("$" + name)).strip()
    except Exception:
        return ""


def _build_signature_list(name):
    value = env.get(name, [])
    if isinstance(value, str):
        value = [value]
    try:
        return sorted({str(item) for item in value})
    except Exception:
        return []


def _build_signature_paths(name):
    # Absolute paths differ per worktree and machine while the compile command is
    # identical, so normalise to the project root and keep the interesting tail.
    root = os.path.abspath(".")
    entries = []
    value = env.get(name, [])
    if isinstance(value, str):
        value = [value]
    try:
        for entry in value:
            path = str(entry)
            absolute = os.path.abspath(path)
            if absolute == root or absolute.startswith(root + os.sep):
                path = os.path.relpath(absolute, root)
            entries.append(path.replace(os.sep, "/"))
    except Exception:
        return []
    return sorted(set(entries))


def _build_signature_fields():
    return {
        "platform": _build_signature_value("platform"),
        "target": _build_signature_value("target"),
        "arch": _build_signature_value("arch"),
        "dev_build": _build_signature_value("dev_build"),
        "tools": _build_signature_list("TOOLS"),
        "cc": _build_signature_value("CC"),
        "cxx": _build_signature_value("CXX"),
        "ccflags": _build_signature_list("CCFLAGS"),
        "cflags": _build_signature_list("CFLAGS"),
        "cxxflags": _build_signature_list("CXXFLAGS"),
        "defines": sorted(set(_build_signature_list("CPPDEFINES") + _build_signature_list("DEFINES"))),
        "linkflags": _build_signature_list("LINKFLAGS"),
        "cpppath": _build_signature_paths("CPPPATH"),
        # GitHub-hosted runner identity, so a rejected cache entry can name the
        # cause it actually has. Neither field reaches a compile line, so neither
        # rotates an object access key by itself; but a new runner image ships a
        # new toolchain and new system headers, and those content signatures are
        # part of every object's key - the whole key space moves at once while
        # cc/cxx (which record names such as "clang++", never versions) look
        # unchanged. Without these two fields a cross-image miss is reported as
        # "Build signature changed: TOOLS -javah", i.e. a machine fingerprint
        # (Tool/javah.py exists() = env.Detect("javah")) presented as the cause.
        # Both are unset off CI (local builds, Device B), which is recorded as ""
        # rather than guessed, and they must stay out of OBJECT_KEY_FIELDS in
        # .github/scripts/summarize_scons_cache.py for the same reason.
        "image_os": os.environ.get("ImageOS", "").strip(),
        "image_version": os.environ.get("ImageVersion", "").strip(),
    }


def _report_build_signature():
    cache_dir = os.environ.get("SCONS_CACHE", "").strip()
    fields = _build_signature_fields()
    signature = hashlib.sha1(json.dumps(fields, sort_keys=True).encode("utf-8")).hexdigest()
    signature_path = os.path.join(cache_dir, "build-signature.json") if cache_dir else ""

    previous = None
    if signature_path and os.path.isfile(signature_path):
        try:
            # utf-8-sig: tolerate an editor/tool that rewrote the file with a BOM.
            with open(signature_path, "r", encoding="utf-8-sig") as signature_file:
                previous = json.load(signature_file)
        except Exception:
            previous = None

    if not isinstance(previous, dict) or not isinstance(previous.get("fields"), dict):
        print(f"[SCons] build-signature: {signature[:12]} (no previous signature in the cache)")
    else:
        previous_fields = previous["fields"]
        changed = [
            key
            for key in sorted(set(fields) | set(previous_fields))
            if fields.get(key) != previous_fields.get(key)
        ]
        deltas = []
        for key in changed:
            before = previous_fields.get(key)
            after = fields.get(key)
            if isinstance(before, list) and isinstance(after, list):
                added = [value for value in after if value not in before]
                removed = [value for value in before if value not in after]
                change = " ".join([f"+{value}" for value in added] + [f"-{value}" for value in removed])
                deltas.append(f"{key} {change}".strip() if change else f"{key} reordered")
            else:
                deltas.append(f"{key} {before!r} -> {after!r}")
        stamp = previous.get("written_at", "unknown time")
        if deltas:
            print(f"[SCons] previous build-signature: {str(previous.get('signature', '?'))[:12]} ({stamp})")
            print(f"[SCons] build-signature: {signature[:12]} (changed: {len(deltas)} field(s))")
            if set(changed) <= {"linkflags"}:
                # Object signatures do not include LINKFLAGS, so a link-only change
                # relinks without recompiling; do not claim a cold build.
                print("[SCons] Only link inputs changed: cached objects stay valid, only the link is redone.")
            else:
                print("[SCons] Objects cached under the old signature cannot be reused; expect recompilation.")
            for delta in deltas:
                print(f"[SCons] signature-delta: {delta}")
        else:
            print(f"[SCons] build-signature: {signature[:12]} (unchanged since {stamp})")

    if not signature_path or _non_build:
        return
    try:
        os.makedirs(cache_dir, exist_ok=True)
        payload = {
            "signature": signature,
            "written_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "fields": fields,
        }
        with open(signature_path, "w", encoding="utf-8") as signature_file:
            json.dump(payload, signature_file, indent=1, sort_keys=True)
    except Exception as error:
        # A read-only or unwritable cache never fails the build: the signature is
        # diagnostics, and the build itself does not depend on it.
        print(f"[SCons] WARNING: could not record the build signature ({error}).")


_report_build_signature()


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
# question, compiledb).
def _assert_dll_naming_consistent():
    skip_targets = {"compiledb"}
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

# Copy DirectStorage runtime redistributable DLLs into output directory on Windows.
# The DLLs are fetched with the SDK (they are not committed), and they are only
# needed at runtime, never to link. When they are absent the build still succeeds
# and the directstorage test suites skip until the fetch task is run.
if env["platform"] == "windows":
    ds_dlls = [
        path for path in (
            "thirdparty/directstorage/bin/x64/dstorage.dll",
            "thirdparty/directstorage/bin/x64/dstoragecore.dll",
        ) if os.path.isfile(path)
    ]
    if ds_dlls:
        installed_dlls = env.Install(output_dir, ds_dlls)
        targets.append(installed_dlls)
    else:
        print("[SCons] NOTE: DirectStorage runtime DLLs are not fetched; skipping the bin/ copy.")
        print("[SCons]       Run tools/fetch_directstorage.ps1 (or the 'Fetch DirectStorage Runtime' task) to enable the DirectStorage suites.")

# The addon folder is the distributable unit and must carry the project license
# and the third-party notice. Keep both byte-identical to the repository-root
# canonical copies.
install_notice = env.InstallAs(
    "project/addons/directstorage_accelerator/THIRD_PARTY_NOTICES.txt",
    "THIRD_PARTY_NOTICES.txt",
)
targets.append(install_notice)
install_license = env.InstallAs(
    "project/addons/directstorage_accelerator/LICENSE",
    "LICENSE",
)
targets.append(install_license)

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

# Build provenance is written by the site tool site_scons/site_tools/build_manifest.py.
# Deployment (copying artifacts into other projects) is not a build concern and
# lives in tools/sync_artifacts.py, driven by .vscode/build_remote.ps1.
import datetime
import atexit

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
    print("[SCons] Notice: DSPKG_ENCRYPTION_KEY declared in environment or .env is ignored because nothing reads that variable any more (the export plugin generates a random archive key per export). A source build that bakes a key uses DSPKG_ENCRYPTION_KEY_FILE=<path>.")

BUILD_START_TIME = datetime.datetime.now().astimezone()

# Build provenance is a site tool now: SCons builds artifacts, the tool records
# which commit and key state produced them. Nothing outside the addon bin is
# written here - deployment is tools/sync_artifacts.py, driven by the pipeline.
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


Default(targets)

