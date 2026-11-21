# Export-contract gate

One repeatable job that proves the archive/bundle contract on real exports. It
is a release gate, not a unit test: every boot runs the exported player from a
staged copy of the release directory, sets `DSPKG_BOOT_VERBOSE=1` and requires
exactly one `DSPKG_BOOT status=ok` line in stdout and in the
`DSPKG_VALIDATE_OUT` sink. Absence, a timeout or a duplicate fails the case.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File project/headless_tests/export_contract/run_export_contract.ps1 -RequireFullCoverage
```

Run it before exporting a release, not on every build: it takes about 2.5 minutes.
Scratch is disposed of as the run proceeds: a measured full 24-case sweep peaks at
about 230 MB (down from about 1.05 GB before case disposal existed) and leaves
nothing behind after a pass. Nothing runs it automatically - there is no CI job for
it - so a release is only checked if a person checks it.

Options:

| Option | Meaning |
|---|---|
| `-Only name1,name2` | run a subset (dependency cases such as `corrupted_toc` need `protected_success` in the same run) |
| `-KeyedTemplate <path>` | 4.7.2 keyed release template (default: `.agents/scratch/test_keyed_template_release/godot.windows.template_release.x86_64.exe`); the environment variables below override the default, and this argument overrides both |
| `-KeyFile <path>` | key file next to the template (default: `key.txt` in the same folder) |
| `-EnvFilePath <path>` | `.env` to read `DSPKG_KEYED_TEMPLATE` / `DSPKG_KEYED_TEMPLATE_KEY_FILE` from (default: `<workspace>/.env`) |
| `-WorkRoot <path>` | artifact directory (default: `out/run-<timestamp>`) |
| `-ExportTimeoutSec`, `-BootTimeoutSec` | process timeouts |
| `-NegativeControl` | self-test: leaves the archive in place during `alert_bypass`; the gate must then fail |
| `-RequireFullCoverage` | fail the run when any case was skipped for a real reason (missing template, fixture or prerequisite stage) |
| `-KeepArtifacts` | keep every case directory and the work root; disables per-case disposal and the work-root removal |
| `-KeepWorkRoot` | keep the work root after a **passing** run (default: removed) |
| `-Cleanup` | accepted for compatibility; removing the work root after a passing run is now the default |

The key is read from the template directory at run time. This script and the
fixture never store a key.

Requirements: `GODOT_CONSOLE_PATH` must point at a **console** build of Godot
4.7.2, in `.env` or in the environment, and
`project/addons/directstorage_accelerator/bin/` must hold the built extension
DLLs. The keyed template is needed for the protected cases; without it they are
reported under `not_covered` and `-RequireFullCoverage` fails the run rather than
passing silently.

The template and key file resolve in this order: `-KeyedTemplate`/`-KeyFile`, then
`DSPKG_KEYED_TEMPLATE` / `DSPKG_KEYED_TEMPLATE_KEY_FILE` in the process environment, then
the same names in `.env`, then the built-in default above. A value supplied by the
environment or `.env` must exist, or the run stops before any case with
`RESULT FAIL input <VARIABLE>=<value>`; a missing argument keeps the older behaviour (a
`not_covered` gap). Both are paths, never key bytes: the key stays in a file beside the
template, so it is never inherited by the Godot processes and exports this script spawns.

The work root is disposable, so the gate disposes of it as it goes. A case's project
tree and its disposable release stage are removed as soon as the case has recorded its
checks; only the two stages a later case reads (`stage-protected_success`,
`stage-unprotected_success`) survive; and a **passing** run removes the whole work root.
A failing run keeps everything and names the pinned cases, so there is always something
to inspect. `-KeepArtifacts` and `-KeepWorkRoot` opt out, and `-Cleanup` is accepted for
compatibility.

Removal is permanent on purpose: sending the run's scratch to the Recycle Bin would hold
the bytes this policy exists to free, and the whole work root is generated output. A
locked file (an antivirus scan or a Godot process that has not released a handle) can
leave one stage behind mid-run; the `[gate] artifact cleanup skipped: ...` line reports
it and never affects the verdict, and the end-of-run removal clears the whole root. Old
`out/run-*` directories therefore only accumulate from failing runs and from
`-KeepWorkRoot` runs; the prune command is:

```powershell
Get-ChildItem out | Sort-Object LastWriteTime -Descending | Select-Object -Skip 5 | Remove-Item -Recurse -Force
```

## What is covered

| Area | Case |
|---|---|
| Protected success | `protected_success` (48-byte key entry, `status=ok mode=protected`, scan `ok`, magic absent from packed scripts) |
| `encrypt_directory` on | `protected_encrypt_directory_on` (same grammar as off) |
| Unprotected success | `unprotected_success` (no key entry, `mode=unprotected`) |
| Bundling off | `bundling_off` (`entries=0`, payloads stay in the PCK) |
| Fail closed | `protected_empty_key`, `include_filter_misses_key`, `exclude_filter_removes_key` |
| Stale archive | deleted by the failed export (`protected_empty_key`) |
| Subset exports | `subset_scenes`, `subset_resources`, `subset_exclude`, `subset_dedicated_server` |
| Partial salvage | `partial_salvage` (deployed, opened, failed payload stays in the PCK) |
| Skip implies expectation | `skip_on_first_add_failure`, `skip_on_finalize_failure`, `partial_salvage` (mid-walk); `skip_on_create_failure` is limited by the builder (see footer) |
| Corrupted TOC | `corrupted_toc` (`status=fail reason=archive_unauthorized`) |
| Plaintext archive in a protected build | `protected_plaintext_archive` (an ordinary unprotected archive with the protected archive id copied into its cleartext header is refused: `status=fail reason=archive_unauthorized`) |
| Boot invocation | `protected_success` (user args reach the player, engine args empty, `DISPLAY=headless`) |
| Alert bypass | `alert_bypass` (missing archive exits without a dialog; a timeout fails) |
| Pack scan | every case; `key_scan_control` proves a plaintext key is found |
| Magic parity | `magic_parity` (C++, exporter and scanner labels and digests agree) |

The runner ends with three lines that are read together:

```text
covers=<case names that ran>
not_covered=<cases skipped for a real reason, with the reason>
known_limits=<sub-checks that cannot pass on any machine, with the reason>
```

`covers=` is the surface actually exercised and `checks=` (printed just above)
is its size - a full run is 162 checks over 20 cases. `not_covered=` is a gap: a
case did not run because something was missing, and `-RequireFullCoverage` turns
any entry there into a failing exit code. `known_limits=` is not a gap: two
sub-checks are limited by the engine or by the builder and cannot pass anywhere -

- `protected_empty_key.pack_truncation`, because this Godot writes a complete PCK
  instead of the truncated one the case asks for (the fail-closed payload removal
  and the `missing_archive` boot are still asserted);
- `skip_on_create_failure`, because `DSPkg.create()` only initialises builder
  state and never opens a file, so its failure branch is unreachable from outside
  (the contract consequence is asserted in the same case).

Read the footer as: `covers=`/`checks=` say what ran, `not_covered=none` says
nothing was skipped for want of a prerequisite, `known_limits=` says the two
permanent exceptions are still the only ones.

## Fixture

`fixture/` is a small Godot project with three imported-looking `.dstex`
payloads (decoded from `fixture/data/dstex_fixtures.json` because `*.dstex` is
gitignored), a boot scene that depends on no archived payload (the engine loads
the main scene before autoloads, so a scene that needs the archive would abort
before the sentinel), and a separate `subset.tscn` used only by the `scenes`
subset preset.

`fixture/addons/contract_injector/` is a test fixture, not shipped: it mutates a
staged texture between `add_texture()` and `finalize()` to exercise the builder
mutation guard.

## Positive control

`control_fixture/key_scan_control/` is a throwaway exporter that injects
`res://.dspkg/toc.key` as `magic16 || random32` with an encryption include filter
that misses the key path. The product exporter refuses that preset before
`add_file()`, so the control cannot come from the addon. The gate requires the
scan to report `status=fail reason=key_magic_found` for its pack.

It lives in the repository so a fresh clone can run it; a workstation that still
has the older gitignored `.agents/scratch/key_scan_control/` copy uses that
instead (both are accepted, the repository copy wins when present).
