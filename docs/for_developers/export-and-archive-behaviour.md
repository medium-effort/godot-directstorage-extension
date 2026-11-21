# Export and archive loading behaviour

Implementation reference for how the exporter decides what to ship, how the runtime decides what to
load, and what the boot sentinel reports. This is not a user guide; the user-facing story lives in
[../encryption-guide.md](../encryption-guide.md).

Read this before changing `ds_export_plugin.gd`, `ds_export_policy.gd`, `ds_runtime.gd`, or the
archive header checks. The rules below are the result of a rebuild plus the test suites in §8; most of
them exist because the alternative silently ships a broken or unprotected build.

Verified against Godot 4.7.2, `template_release` (keyed) and `template_debug`, 2026-10-27.

---

## 1. The constraint everything follows

Godot runs the export hooks in this order (`editor/export/editor_export_platform.cpp`):

```text
_export_begin()            one call, before any file is processed
  _export_file()           once per exported file
  project.binary written   <- ProjectSettings is snapshotted here
  pack written
_export_end()              after the pack is sealed
```

Two consequences drive the whole design:

1. **Everything the shipped game learns about the export is a prediction.** It is written into
   `project.binary` before the reward (does the archive exist?) is known.
2. **`_export_end()` cannot change the pack.** It can only add, rename, or delete files on disk.

So the exporter's job is to make the prediction *safe when it is wrong*, and the runtime's job is to
detect a wrong prediction without hanging.

`skip()` is the third constraint: it is irreversible and happens per file, mid-walk, before the
archive is finalized. A texture removed from the PCK exists only in the archive from that moment on.

---

## 2. The published contract

Four internal settings are written into `project.binary` by `_publish_contract()`. They are hidden
from the editor (`set_as_internal`) and must stay in sync with the runtime.

| Setting | Meaning | Producer |
|---|---|---|
| `dsgd_extension/bundle_dspkg` | "an archive is expected to exist beside the executable" | `_publish_contract()` |
| `dsgd_extension/archive_id` | the random 64-bit archive id of this export, or `0` when pairing is off | same |
| `dsgd_extension/protected` | true for the `protected` and `failed` outcomes; additive only - the non-overridable carrier is the `res://.dspkg/toc.key` entry inside the encrypted PCK, which `DSPkg::open()` also checks | same |
| `dsgd_extension/archive_name` | archive filename (default `textures.dspkg`) | same |

`bundle_dspkg` is true only when **all** of these hold:

- the add-on option `dsgd_extension/bundle_textures_dspkg` is on;
- at least one payload was removed from the PCK (`_skipped_payloads > 0`). The trigger is the
  **skip**, not the add: a protected export that fails before its first add still skips payloads, and
  those textures then exist only in an archive that will never be written, so the game must expect
  one and fail loudly. Zero skips means the PCK is self-sufficient, even when a filter removed every
  texture.

`_expect_archive` is monotonic: it is false at export start, becomes true on the first skip, and is
never demoted, so a later failure cannot erase the expectation it already published.

`_publish_contract()` is called from `_export_begin()`, from the first successful skip, and from every
failure path that changes the answer. That is the only reason the pack can tell the truth.

`dsgd_extension/archive_id` is the pairing value the runtime passes to `DSPkg.open()`; it is a
random per-export id, not a project or content hash (§5).

---

## 3. Export decision table

`_resolve_outcome()` asks the active preset one question — does it request PCK encryption? — and then
whether the key file can be delivered inside the encrypted region. It returns one of the four
`Policy.OUTCOME_*` values, and every later decision reads that value. The add-on keeps no cached copy
because one plugin instance serves every preset of an "Export All" run.

| Outcome | When | PCK copies | Archive | `bundle_dspkg` | Runtime |
|---|---|---|---|---|---|
| `disabled` | `dsgd_extension/bundle_textures_dspkg` off | kept (intended) | none | false | CPU path |
| `plaintext` | bundling on, `preset.get_encrypt_pck()` false | skipped per texture | deployed, plaintext, no key | true | streams; sentinel `key_id=none` |
| `protected` | bundling on, PCK key present, key path encrypted | skipped per texture | deployed, encrypted TOC | true | streams; sentinel `mode=protected` |
| `failed` | bundling on, PCK encryption requested, but no key or the key path is not encrypted | **all skipped** | none (staging removed, stale target deleted) | true | boot fails with `missing_archive` |

Within the two archive outcomes, `resolve_file_action()` covers the mid-walk failures:

| Situation | PCK copies | Archive | `bundle_dspkg` | Runtime |
|---|---|---|---|---|
| `all_resources`, but the export filter excludes every texture | kept | none | false | CPU path |
| `add_texture` fails, entries already added (`plaintext`) | failed + remaining kept | **partial** deployed | true | archive + PCK hybrid; every texture is somewhere |
| `add_texture` fails, entries already added (`protected`) | all skipped | deleted | true | boot fails loudly |
| `add_texture` fails, no entries yet | kept | none | false | CPU path |
| failure before any add (`protected`) | all skipped | none | true | boot fails loudly |
| `finalize`/rename fails (`plaintext`) | skipped ones lost, rest kept | deleted | true (cannot change) | boot fails loudly |
| `finalize`/rename fails (`protected`) | all skipped | deleted | true | boot fails loudly |

The rule behind every row: **a texture is never left in neither container**, except in `protected`
mode, where a failed build deliberately leaves it nowhere rather than shipping it in the clear. That
is also why a partial archive is deployed for a `plaintext` failure: those bytes were already removed
from the PCK and exist nowhere else.

---

## 4. Rules in `_export_file()` and `_export_end()`

`_export_file()` distinguishes three states:

1. bundling disabled → return, let Godot pack the `.dstex` (intended CPU fallback);
2. bundling requested but not possible (`_dspkg == null` or `_archive_failed`) → `resolve_file_action()`
   decides: `skip()` the payload for `protected`/`failed`, leave it for `plaintext`;
3. adding → on success record `_archive_entries`, then `skip()` **as soon as the primary entry
   exists**, before the remapped alias is attempted. A later alias failure can therefore never leave
   the same payload in both containers.

Two paths are handled before that decision: any `.dspkg` in the project tree is always skipped (an
archive must never be embedded in the PCK), and `.import` files are always preserved so the remap
table that resolves `load("res://texture.png")` to the archive entry reaches the PCK.

Failure handling goes through `_abort_archive()`: it sets `_archive_failed`, and when no entry exists
yet it also clears the staging file. When entries do exist it deliberately keeps the staging archive
alive for salvage.

The three decisions themselves are pure functions in
[ds_export_policy.gd](../../project/addons/directstorage_accelerator/scripts/ds_export_policy.gd)
(`resolve_bundle_effective`, `resolve_file_action`, `resolve_failure_action`). The plugin only applies
their result, which lets `project/headless_tests/test_export_policy.gd` assert the whole table without
an extension or an export run. Change the rules there, not inline.

`_export_end()` then applies one rule:

- `_archive_failed` and `resolve_failure_action()` says delete → delete the target archive and the
  staging file;
- `_archive_failed` and salvage → finalize and deploy the partial archive, log a warning that it is
  partial;
- otherwise → finalize, delete any previous target, rename staging to target. For `protected`, the
  writer round-trip then reopens the deployed archive with the generated key and the exported id and
  prints `Writer round-trip verified`.

Deleting the target on failure is what stops a previous build's archive from being loaded by the new
build. Do not remove that.

---

## 5. Archive id pairing

The archive header carries one 64-bit pairing field. The exporter generates it per export
(`_new_archive_id()`: 8 random bytes, top bit cleared), publishes it as
`dsgd_extension/archive_id`, and passes it to `DSPkg.create()`.

The reader compares it in full width (`src/formats/dspkg_format.cpp`, `DSPkg::open()`):

```text
if (expected_id != 0 && expected_id != header.archive_fingerprint)
    return ERR_FILE_UNRECOGNIZED
```

- A non-zero `expected_id` must match exactly. An archive that carries id `0` is **rejected**, so
  zeroing the header field cannot satisfy a build that expects a real id.
- Passing `0` as the expected id skips the check. `dsgd_extension/enforce_archive_pairing=false`
  publishes `0`, so such a build only accepts archives it is paired with nothing; that is a
  local-tooling shape and not a shipping configuration.
- A mismatch is fatal at boot: `DSPKG_BOOT status=fail reason=archive_pairing_mismatch`, exit 1, no ok
  line. Measured with export B's pack next to export A's archive.
- A difference of one bit (`…472` vs `…473`) is rejected, which is the point of comparing the full
  width rather than a truncated value.
- `DSPkg.create()` defaults to id `0`, which produces an archive a pairing-enabled build rejects. Any
  custom build step must pass the id it publishes as `dsgd_extension/archive_id`.

What the check proves: the pack and the archive come from the same export. What it does not prove:
that the archive contains every texture the build expects, or anything about the payload bytes.

Because both the key and the id rotate per export, **the PCK and the archive ship as a pair**. Any
patch that rebuilds the pack must redistribute the archive with it.

---

## 6. Runtime boot contract

`ds_runtime.gd` reads the contract in `_ready()`:

- `bundle_dspkg == false` → print the CPU-mode notice and continue. Textures are in the PCK.
- `bundle_dspkg == true` → search for the archive (project settings, `res://`, executable directory),
  then `DSPkg.open(candidate, archive_id)`.

The native reader acquires its key itself from `res://.dspkg/toc.key`; the runtime never supplies one.

### 6.1 The sentinel grammar

One machine-readable line per verdict:

```text
DSPKG_BOOT status=ok   mode=<protected|unprotected> entries=<n> key_id=<16 hex|none>
DSPKG_BOOT status=fail reason=<code> mode=<protected|unprotected>
```

Failure reasons:

| Reason | Cause |
|---|---|
| `missing_extension` | `DSContext` not registered (extension DLL absent) |
| `missing_archive` | `bundle_dspkg=true` but no archive found |
| `archive_unauthorized` | `ERR_UNAUTHORIZED` (key mismatch / absent) |
| `archive_pairing_mismatch` | `ERR_FILE_UNRECOGNIZED` (different export or stale archive) |
| `archive_unreadable` | any other open error, including `ERR_FILE_CORRUPT` (truncated archive) |

`status=fail` prints on every boot. `status=ok` prints only when the boot was asked to validate or to
be verbose, so a normal boot stays silent. The success line is guarded by a static flag and is printed
at most once per process; a duplicate is a CI failure.

`mode` is the validate mode when one was requested, otherwise the exported
`dsgd_extension/protected` value. **`key_id` is the evidence**: `none` when the export was not
protected (or no key was committed), 16 hex characters when `open()` committed a key after matching
the header id and decrypting the TOC.

### 6.2 Flags and environment

| Input | Effect |
|---|---|
| `--dspkg-validate=protected\|unprotected` (after `--`) | turns on the ok line and pins the expected mode |
| `DSPKG_VALIDATE=protected\|unprotected` | same, for callers that cannot pass user args |
| `DSPKG_BOOT_VERBOSE=1` | turns on the ok line without pinning a mode |
| `DSPKG_VALIDATE_OUT=<file>` | appends the line to that file as well as stdout |
| `DSPKG_NO_ALERT=1` | never open a dialog; take the non-interactive path |

Only user arguments are scanned: `OS.get_cmdline_args()` is empty in an exported player, which is why
the switch follows `--`.

### 6.3 The CI gate

The gate is the **presence of exactly one success line**, not the exit code:

```powershell
$env:DSPKG_BOOT_VERBOSE = "1"
$env:DSPKG_NO_ALERT = "1"
<export-dir>\game.exe --headless -- --dspkg-validate=protected
```

Require exactly one `DSPKG_BOOT status=ok mode=protected entries=<n> key_id=<16 hex>`. Absence is a
failure; the exit code is supporting only, because `_fail_boot()` calls `quit(1)` without stopping the
tree and a later `quit(0)` can override it. Then scan the pack (§6.5). A build counts as protected only
after both checks pass.

Why the exit code cannot be the verdict: `_fail_boot()` is diagnostic. It prints, optionally alerts,
and quits with the code; it never pauses the tree or frees siblings, so later autoloads and the main
scene still run.

### 6.4 The ordering limitation (measured)

Godot loads the main scene **before** autoloads run. If the main scene depends on an archived payload
and the archive is missing, the engine aborts the scene load first, so `_fail_boot()` never executes:

```text
ERROR: Failed loading resource: res://assets/used.dstex.
ERROR: Can't load dependency: 'res://assets/used.dstex'.
ERROR: Failed loading scene: res://dependent.tscn.
   at: Main::start (main\main.cpp:4767)
```

Measured on both a windowed and a headless run of the dependent-scene fixture: exit 1, **zero**
`DSPKG_BOOT` lines. Consequences:

- Fail-closed still holds: the process exits 1 and no game runs.
- The gate still catches it, because it requires a success line and there is none.
- The friendly "Missing Game Files" dialog is **unreachable for this class of game**. A main scene
  that does not depend on archived payloads prints the sentinel and reaches the dialog path normally
  (measured: `reason=missing_archive` plus a blocking `OS.alert()`).
- The cause is the engine's ordering (`Main::start`), not this add-on.

If a project needs the dialog, keep the entry scene free of archived dependencies (a payload-free
bootstrap scene that loads the real scene after the autoload verdict).

### 6.5 The pack scan

```text
godot --headless --path project --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack "<export-dir>/game.pck"
```

The same scan runs from **Project > Tools > Verify exported pack (DSPkg)** in the editor; both faces use
one engine, so the verdict line is identical.

Verdict grammar:

```text
DSPKG_SCAN status=ok entries=<n> file=<path>
DSPKG_SCAN status=fail reason=<bad_header|zero_entries|directory_past_eof|key_magic_found|unreadable> file=<path>
```

The scanner validates the pack header (a truncated pack must not pass vacuously) and searches the raw
bytes for the 16-byte derived key magic. A hit outranks a malformed header. Exit codes: 0 ok, 1 fail,
2 usage.

The grammar has no encryption field on purpose: a plaintext pack produces the same `status=ok`, so the
verdict cannot be read as "this build is protected". The editor dialog shows the directory bit
separately as `Directory encrypted: yes/no` (taken from the pack header's `PACK_DIR_ENCRYPTED` flag);
the command line emits the verdict line only, keeping the gate's regular expressions and any external
automation stable.

### 6.6 Non-interactive behaviour

In a headless run (`DisplayServer.get_name() == "headless"` or `DSPKG_NO_ALERT=1`) `_fail_boot()`
prints the fail line and exits without calling `OS.alert()`. `OS.alert()` blocks a headless player
until an external timeout kills it, so this branch is what makes CI possible. `DSPKG_NO_ALERT=1`
forces the same path on a machine with a display.

`OS.has_feature("editor")` is true in an **editor binary even when it runs a project**, so these fatal
branches never execute when you launch the project from the editor. Use an exported player to
exercise them.

---

## 7. Known gaps

These are open, not design intent.

- **Disk-full during `finalize()` is silent (issue #0157).** `DSPkg::finalize()` ignores every
  `store_buffer` return value, so a short write produces a truncated archive and returns `OK`. The
  plugin then renames and reports success. The reader does reject it (`ERR_FILE_CORRUPT`, "Entry
  payload extends past archive length"), and the runtime treats that as fatal, but the export itself
  still reports success.
- **Salvage depends on file order.** If a corrupt texture is processed before any good one, nothing
  is archived and the build falls back entirely to the PCK (`bundle=false`). If a good one comes
  first, a partial archive is deployed (`bundle=true`). Both are safe; the artefact differs.
- **The missing-archive dialog is unreachable for a dependent main scene** (§6.4). Fail-closed holds;
  only the user-facing diagnostic is missing.
- **`DSPkg::create()` performs no I/O**, so the exporter's `create()`-failure injection point is
  unreachable from outside the builder. Reported as `not_covered` by the gate; the rename-failure
  path covers the consequence.

---

## 8. Tests

Run these after touching the exporter, the runtime, or the archive reader.

| Harness | Command | Proves |
|---|---|---|
| Policy unit test | `godot --headless --path project --script headless_tests/test_export_policy.gd` | the whole decision table above, in seconds, with no extension or export (17 checks) |
| Export-contract gate | `powershell -ExecutionPolicy Bypass -File project/headless_tests/export_contract/run_export_contract.ps1 -RequireFullCoverage` (2.5 min, measured peak ~230 MB of scratch, nothing left after a pass) | protected/unprotected/bundling-off exports, the three fail-closed presets, subset modes, stale-archive deletion, partial salvage, corrupted TOC, the boot grammar, the pack scan and the key-scan positive control |
| Pack scan | `godot --headless --path project --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack <pck>` | pack validity and key-magic absence for one file |

The gate is run manually before exporting a release; nothing runs it automatically, so a change to the
exporter, the runtime or the archive reader is only checked if someone runs it. It disposes of each
case's project tree and stage after the case has recorded its checks - only the two stages a later case
reads survive - and a passing run removes its whole work root (`-KeepArtifacts` / `-KeepWorkRoot` retain
them for diagnosis; a failing run always keeps them). It ends with `covers=`,
`not_covered=` and `known_limits=`:

- `covers=` and `checks=` state what actually ran (a full run is 162 checks over 20 cases).
- `not_covered=` is a gap - a case that did not run for want of a prerequisite. `-RequireFullCoverage`
  turns any entry there into a failing exit code, which is why the documented command passes it.
- `known_limits=` lists the two sub-checks that cannot pass on any machine:
  `protected_empty_key.pack_truncation` (this engine writes a complete PCK instead of the truncated one the
  case asks for) and `skip_on_create_failure` (the builder never opens a file in `create()`, so that failure
  branch is unreachable; the contract consequence is asserted in the same case).

The keyed template and its key file can be supplied without editing the script, through
`DSPKG_KEYED_TEMPLATE` / `DSPKG_KEYED_TEMPLATE_KEY_FILE` in the environment or `.env` (arguments still
win). A value supplied that way must exist, or the gate stops with `RESULT FAIL input` instead of skipping
the protected cases; a missing argument keeps the `not_covered` behaviour below.

Without a keyed template the protected cases land in `not_covered`, and with `-RequireFullCoverage` the run
fails rather than reporting a pass it did not earn.

Gotchas learned while building these tests, useful for the next harness:

- GDScript `String.hex_to_int()` clamps values above 2^63 to `INT64_MAX`. Pass a signed decimal for a
  uint64 id, or the comparison silently uses the wrong value.
- `FileAccess.file_exists()` on `res://` source paths is unreliable for packed resources: `main.tscn`
  reports `false` while `ResourceLoader.exists()` reports `true`. Assert on the exporter log and on
  settings read from the mounted pack instead.
- `--main-pack` does not load a GDExtension whose DLL is not inside the pack, so `DSPkg.open()` must
  run with `--path <project>`, not from the mounted pack.
- PowerShell registers a function only when its definition statement executes; define helpers above
  their first call.
- The probe project holds a **copy** of the addon. Any harness that copies that project must mirror
  `project/addons/directstorage_accelerator` into the copy, or it silently tests an old exporter.
- Do not run `--import` on a throwaway project that loads the extension with the `DSRuntime` autoload:
  it can exit with an access violation at teardown on this machine. The end-to-end harness writes
  `extension_list.cfg` directly instead.

---

## 9. Invariants to preserve

1. `bundle_dspkg` follows the skips, not the adds: true exactly when a payload was removed from the
   PCK. Zero skips means the PCK is self-sufficient, so publish false and let the game run from it;
   any skip makes the archive mandatory, so publish true even if it then fails to deploy — the game
   must fail loudly rather than boot without assets.
2. A failed export never leaves a previous build's archive loadable.
3. In `plaintext` mode, never leave a texture in neither container: skipped bytes belong to the
   archive, unsaved bytes stay in the PCK. In `protected`/`failed` mode a failed build deliberately
   leaves them nowhere — plaintext must not ship, so boot fails instead.
4. `skip()` happens as soon as the primary archive entry exists, never after the alias attempt.
5. Pairing is one 64-bit archive id, compared in full. `0` disables it and is a tooling value only.
6. Fatal boot paths never call `OS.alert()` in a headless run.
7. The success sentinel prints at most once per process, and only when validate or verbose was asked
   for.
