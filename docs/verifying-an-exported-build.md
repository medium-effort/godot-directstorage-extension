# Verifying an exported build: boot sentinel and pack scan

This page covers the two checks that decide whether an exported build is actually the build you
think you shipped: the **boot sentinel** line and the **pack scan**. Both work on any export,
whether or not the archive is bundled and whether or not the PCK is encrypted.

Neither check enforces anything. They report; they do not prevent a build from running. The
verdict is read from the lines they print, not from the process exit code.

---

## The two checks

Boot the exported player and ask it for the sentinel line:

```powershell
<export-dir>\game.exe --headless -- --dspkg-validate=protected
```

Require exactly one line of the form:

```text
DSPKG_BOOT status=ok mode=protected entries=<n> key_id=<16 hex>
```

Then scan the pack that ships next to it:

```powershell
godot --headless --path <your-project> --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack "<export-dir>/game.pck"
```

Require:

```text
DSPKG_SCAN status=ok entries=<n> file=<path>
```

The same scan runs from the editor as **Project > Tools > Verify exported pack (DSPkg)**. It needs
neither the extension binary nor a key: it only reads the pack you point it at.

---

## The boot sentinel

`DSPKG_BOOT` is one line printed by the runtime autoload when an exported build finishes deciding
whether to open the archive. It is written for machines: one line per verdict, stable field order,
stable reason tokens.

### When it prints

| Situation | Output |
|---|---|
| a normal successful boot | nothing |
| a successful boot that was asked to validate or to be verbose | one `status=ok` line |
| a failed boot | one `status=fail` line, on every boot |

An empty log from a healthy build is therefore the expected result, not a symptom. A missing line
is only a failure when you asked for one.

The success line is printed at most once per process. A duplicate is treated as a failure by the
gate: two `status=ok` lines mean the verdict is ambiguous.

### The ok line

```text
DSPKG_BOOT status=ok mode=<protected|unprotected> entries=<n> key_id=<16 hex|none>
```

| Field | Meaning |
|---|---|
| `mode` | the mode you asked to validate; when the boot was not asked to validate, the mode the export recorded in `dsgd_extension/protected` |
| `entries` | number of entries in the opened archive; `0` when the archive was not bundled, or was bundled with nothing to archive |
| `key_id` | `none` when no key was committed; 16 hex characters when a key was committed after matching the archive header's key id and decrypting the table of contents |

`status=ok` on its own does **not** mean the build is protected. Read `key_id`: `none` means the
archive was opened without a key, so nothing about its table of contents was protected. A key that
was compiled into a source build is not reported here; it is not part of the shipped contract.

### The fail line

```text
DSPKG_BOOT status=fail reason=<code> mode=<protected|unprotected>
```

| Reason | What happened |
|---|---|
| `missing_extension` | the extension DLL is absent, so `DSContext` was never registered |
| `missing_archive` | the build expects `textures.dspkg` beside the executable and did not find it |
| `archive_unauthorized` | no usable key: the PCK is not mounted, the include filter missed `res://.dspkg/toc.key`, the PCK key is wrong, or the archive is plaintext in a build whose PCK delivered that key entry |
| `archive_pairing_mismatch` | the archive belongs to a different export than the build |
| `archive_unreadable` | the archive is truncated or corrupt |

`mode` on a fail line follows the same rule as on the ok line, so you can tell which shape the
build expected.

### Flags and environment

| Input | Effect |
|---|---|
| `--dspkg-validate=protected\|unprotected` (after `--`) | prints the ok line and states the mode you expect |
| `DSPKG_VALIDATE=protected\|unprotected` | same, for launchers that cannot append user arguments |
| `DSPKG_BOOT_VERBOSE=1` | prints the ok line without asking for a mode; the line then reports the mode the export recorded |
| `DSPKG_VALIDATE_OUT=<file>` | appends every sentinel line to that file as well as stdout |
| `DSPKG_NO_ALERT=1` | never open a dialog; take the non-interactive path |

The validate switch controls verbosity and the reported mode. It does not change the boot itself
and it does not compare the mode: the line simply echoes what you asked for, and your check is what
compares it. A headless run, or a validation run, never opens a dialog. Only user arguments are
scanned, which is why the switch follows `--`: in an exported player the engine's own argument
list is empty.

### The one case with no line at all

Godot loads the main scene **before** autoloads run. If the main scene itself depends on an
archived payload and the archive is missing, the engine aborts the scene load first, so the
sentinel never gets a chance to print:

```text
ERROR: Failed loading resource: res://assets/used.dstex.
ERROR: Can't load dependency: 'res://assets/used.dstex'.
ERROR: Failed loading scene: res://dependent.tscn.
```

The process still exits 1 and no game runs, so the build still fails closed. What is lost is the
diagnostic: no line, and no friendly "missing game files" dialog. If you want the dialog, keep the
entry scene free of archived payloads and let it load the real scene after the autoload verdict.

### Editor runs are not a substitute

The fatal paths are skipped in an editor binary even when it runs your project, so a missing archive
looks harmless there. Exercise the sentinel and the failure paths from an exported player.

---

## The pack scan

```text
DSPKG_SCAN status=ok   entries=<n> file=<path>
DSPKG_SCAN status=fail reason=<code> file=<path>
```

| Reason | Meaning |
|---|---|
| `bad_header` | not a PCK, or a truncated/unsupported header |
| `zero_entries` | the header declares no entries |
| `directory_past_eof` | the entry directory points past the end of the file |
| `key_magic_found` | the 16-byte derived key magic occurs in the pack's raw bytes, so the key entry shipped in the clear |
| `unreadable` | the file could not be read |

Exit codes are 0 for `ok`, 1 for `fail`, 2 for a usage error.

`status=ok` does not mean the pack is encrypted: a plaintext pack passes the same scan. The command
line keeps one grammar on purpose, so automation can rely on it; the editor dialog reports
encryption separately as `Directory encrypted: yes/no`, read from the pack header. A relative
`--pack` path is resolved against the project directory that `--path` selects, so pass an absolute
path when the pack lives elsewhere.

---

## Using this in CI

The verdict is the **presence of exactly one success line**, not the exit code. The exit code is
supporting only: the failure path quits with 1 without stopping the tree, so a later quit from the
game can override it.

```powershell
$env:DSPKG_VALIDATE_OUT = "boot-verdict.log"
<export-dir>\game.exe --headless -- --dspkg-validate=protected
```

Match the ok line's `mode` and its `key_id` field, then run the pack scan and require its ok line.
A build counts as protected only when `mode=protected`, `key_id` is 16 hex characters, and the scan
is ok.

Set `DSPKG_NO_ALERT=1` as well when the runner has a display: a blocking dialog would otherwise
wait for a human nobody is there to provide. `--headless` already takes that path on its own.

`DSPKG_VALIDATE_OUT` is *appended* to, never truncated, so give each run its own path when your check
counts lines. The gate that ships with the add-on requires exactly one ok line in stdout and exactly
one in the sink file, so a reused sink from a previous run turns a good build into a duplicate.

---

## FAQ

**Why is there no `DSPKG_BOOT` line in my log?**
A successful boot is silent unless you ask for the line. Pass `--dspkg-validate=protected` (or
`=unprotected`), set `DSPKG_VALIDATE` to the same value, or set `DSPKG_BOOT_VERBOSE=1`.

**The game exits 1 and prints no line at all.**
The main scene needs an archived payload and the archive is missing; the engine aborts before the
autoload runs. Ship the archive from the same export, and see the ordering note above if you want
the dialog.

**Can I just check the exit code?**
Not as the verdict. It is supporting evidence; the line names the reason and is the contract. Since
`_fail_boot()` re-applies its code at teardown, a non-zero exit now reliably accompanies a failure
line even when the game's own main loop quits with 0.

**Does `status=ok` mean the build is protected?**
No. `key_id=none` means no key was involved. The evidence for a protected build is `mode=protected`,
a 16-hex `key_id`, and an ok pack scan.

---

## Related documents

- [Encryption guide: shipping an encrypted archive with DSGD](encryption-guide.md) — the full
  encryption walkthrough.
- [Export and archive loading behaviour](for_developers/export-and-archive-behaviour.md) — the
  implementation contract behind both checks.
- [DirectStorage Accelerator add-on README](../project/addons/directstorage_accelerator/README.md) —
  the export outcomes that produce each build shape.
