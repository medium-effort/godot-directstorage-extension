# Export-contract gate

One repeatable job that proves the archive/bundle contract on real exports. It
is a release gate, not a unit test: every boot runs the exported player from a
staged copy of the release directory, sets `DSPKG_BOOT_VERBOSE=1` and requires
exactly one `DSPKG_BOOT status=ok` line in stdout and in the
`DSPKG_VALIDATE_OUT` sink. Absence, a timeout or a duplicate fails the case.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File project/headless_tests/export_contract/run_export_contract.ps1
```

Options:

| Option | Meaning |
|---|---|
| `-Only name1,name2` | run a subset (dependency cases such as `corrupted_toc` need `protected_success` in the same run) |
| `-KeyedTemplate <path>` | 4.7.2 keyed release template (default: `.agents/scratch/test_keyed_template_release/godot.windows.template_release.x86_64.exe`) |
| `-KeyFile <path>` | key file next to the template (default: `key.txt` in the same folder) |
| `-WorkRoot <path>` | artifact directory (default: `out/run-<timestamp>`) |
| `-ExportTimeoutSec`, `-BootTimeoutSec` | process timeouts |
| `-NegativeControl` | self-test: leaves the archive in place during `alert_bypass`; the gate must then fail |

The key is read from the template directory at run time. This script and the
fixture never store a key.

Requirements: `GODOT_CONSOLE_PATH` in `.env` must point at a **console** build of
Godot 4.7.2, and `project/addons/directstorage_accelerator/bin/` must hold the
built extension DLLs. Without a keyed template the protected cases are reported
under `not_covered` and the run is not a full pass.

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
| Boot invocation | `protected_success` (user args reach the player, engine args empty, `DISPLAY=headless`) |
| Alert bypass | `alert_bypass` (missing archive exits without a dialog; a timeout fails) |
| Pack scan | every case; `key_scan_control` proves a plaintext key is found |
| Magic parity | `magic_parity` (C++, exporter and scanner labels and digests agree) |

The runner prints `covers=... not_covered=...` at the end. A case listed under
`not_covered` is not a pass; the footer states the reason and the measurement.

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

`.agents/scratch/key_scan_control/` is a throwaway exporter that injects
`res://.dspkg/toc.key` as `magic16 || random32` with an encryption include filter
that misses the key path. The product exporter refuses that preset before
`add_file()`, so the control cannot come from the addon. The gate requires the
scan to report `status=fail reason=key_magic_found` for its pack.
