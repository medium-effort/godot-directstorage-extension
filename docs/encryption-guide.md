# Encryption guide: shipping an encrypted archive with DSGD

This is the single guide for shipping a game with DirectStorage Accelerator (DSGD) and an encrypted
archive. It covers the strongest configuration the add-on supports today, the exact steps in the
order that works, the two checks that prove the result, what to say to players, and where the limits
are.

Developer-facing references (implementation contract, boot mechanics) are listed under
[Related documents](#related-documents) at the end.

---

## The short version

You get the strongest configuration by doing five things in this order:

1. **Generate one PCK key and keep it out of the repository.**
2. **Build Godot yourself with that key.** Stock Godot cannot open a PCK encrypted with your key.
   This is Godot's rule; the add-on does not remove the step.
3. **Set the key and the include filter on the preset's Encryption tab.** The filter must cover
   `res://.dspkg/toc.key`. Without it, turning on PCK encryption encrypts nothing.
4. **Export.** The add-on generates a fresh archive key, builds the archive with it, and injects it
   into the encrypted PCK.
5. **Boot the exported player and run the pack scan.** A build counts as protected only after both
   checks pass on the exact files you ship.

---

## What this does not protect

Read this before writing anything on a store page.

- **The texture payloads inside `textures.dspkg` are not encrypted.** They are stored in compressed
  form, and reading them back out of the archive does not involve a key. What is encrypted is the
  archive's table of contents: entry names, dimensions and offsets.
- **Anything the running game can read, a modified game can read.** Same boundary as Godot's PCK
  encryption.
- **An attacker who recovers your PCK key can read the archive key** out of the pack, and therefore
  the table of contents. That is the PCK baseline, no weaker and no stronger.
- **This is not a sandbox.** The boot sentinel and the scanner are diagnostics; they report, they do
  not enforce.
- **Unprotected mode exists and provides no protection.** If you export with PCK encryption off, the
  archive is plaintext and no key is involved. That mode is for streaming, not for protection.

What the key changes: the archive's entry table is unreadable outside the build it was exported for,
and an archive that does not belong to that build is refused at boot. A protected build enforces this
rather than trusting the archive: an archive that is not encrypted is refused at open with
`archive_unauthorized`, and the exporter's round-trip check applies the same rule to the archive it
just wrote.

---

## How the pieces fit

```text
your PCK key (64 hex characters)
   |
   +--> compiled into the Godot templates      (game.exe can mount game.pck)
   +--> set in the export preset               (Godot encrypts the pack with it)
   |
   +--> encrypted PCK
          |
          +--> res://.dspkg/toc.key            48 bytes: 16-byte magic + 32-byte random archive key
          |
          +--> textures.dspkg                  TOC encrypted with that archive key; payloads plaintext

shipping layout:
   game.exe            mounts game.pck with the compiled-in key
   game.pck            encrypted; contains the 48-byte key entry
   textures.dspkg      encrypted TOC, plaintext payloads, random archive id
   dsgd_64.dll         no key inside
```

At runtime the engine mounts the PCK, the native reader finds `res://.dspkg/toc.key`, checks the
16-byte magic, and uses the trailing 32 bytes. It installs that key only after it reproduces the
archive header's key id and decrypts the table of contents. With only the DLL and the archive, there
is no key anywhere.

The archive key is **random for every export**. It is not derived from the PCK key. The PCK and the
archive also carry a matching random 64-bit archive id, so a stale or foreign archive is rejected at
boot instead of loading old textures.

---

## Which extension build to use

The key path depends on one thing only: whether the DLL carries a baked key. Everything else about
the extension is the same in every build.

| Build | Key in the DLL | Use it when |
|---|---|---|
| **Prebuilt release** | no (`dspkg-key-id=none`) | The normal choice. You still get PCK-delivered keys; your game needs no API call. |
| **Build from source, no key** | no | You want to build the extension yourself from the same tree. |
| **Build from source with a baked key** | yes | Legacy (experiments only). The key can be recovered from the DLL, so this is weaker than the PCK you already use. Do not choose it for a new project. |

The shipped game never calls `DSPkg.set_global_encryption_key()`. That method exists for tooling and
for the editor (see **Previewing packaged textures**).

---

## Step by step

### 1. Generate the PCK key, and keep it out of the repository

```powershell
$b = New-Object byte[] 32
[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($b)
($b | ForEach-Object { $_.ToString('x2') }) -join '' | Set-Content godot.gdkey -NoNewline
```

That file holds 64 hexadecimal characters: a 256-bit key.

- Do not commit it.
- Do not paste it into a script.
- Back it up. **If you lose it, existing packs and their archives cannot be opened.**

### 2. Build the Godot templates with that key

In the Godot source tree, with the key in the environment:

```powershell
$env:SCRIPT_AES256_ENCRYPTION_KEY = (Get-Content <path>\godot.gdkey -Raw).Trim()
scons platform=windows target=template_release
scons platform=windows target=template_debug
```

Build the editor too if you want to open your own encrypted packs with it. Godot's own reference is
[Compiling with PCK encryption key](https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_with_script_encryption_key.html);
the add-on does not change this step.

One trap worth knowing before you spend an afternoon on it: under `ninja=yes`, changing
`SCRIPT_AES256_ENCRYPTION_KEY` alone does **not** regenerate the key file, and the build silently
reuses the old template that has no key. Delete `core/script_encryption_key.gen.cpp` and the previous
template binary before rebuilding, or build without Ninja.

Why this is a prerequisite: the PCK key is compiled into the engine. A stock template cannot mount
your pack; the game exits with a pack error and nothing in DSGD can fix that.

### 3. Build or take the extension

For most projects: use the prebuilt add-on as shipped. It carries no key in the DLL, so nothing about
the archive key lives there.

If you build from source, build it without a key input. The default shape has no key in the DLL: a
plain `build_remote.ps1` run bakes no key, because `.env.example` leaves
`DSPKG_ENCRYPTION_KEY_FILE` empty. Setting that variable, or passing `-EncryptionKeyFile` /
`-EncryptionKey`, opts into a **legacy build with a baked key** - experiments only, and the build
prints a deprecation warning. `-ResetKeyHeader` forces the no-key shape again and refuses to combine
with key arguments.

Copy the built add-on into your project:

```text
your_project/addons/directstorage_accelerator/
    directstorage_accelerator.gdextension
    plugin.cfg
    scripts/
    doc_classes/
    bin/dsgd_64.dll
    bin/dsgd_64.debug.dll
    bin/dstorage.dll
    bin/dstoragecore.dll
```

### 4. Configure the export preset

In **Project > Export**, select your preset. The settings below are split across two tabs (labels as
of Godot 4.7):

- **Resources** tab — **Export Mode**: `Export all resources in the project` (`all_resources`). This is
  the only mode tested end to end; see the note below.
- **Encryption** tab — everything else:
  - **Encrypt Exported PCK**: on.
  - **Encryption Key (256-bits as hexadecimal):**: the 64 hex characters from `godot.gdkey`.
  - **Filters to include files/folders**: `.dspkg/*` (or any glob that matches `res://.dspkg/toc.key`).
  - **Filters to exclude files/folders**: empty, or at least not matching the key path.
  - **Encrypt Index (File Names and Info)**: optional. It hides the pack's file names as well as its
    contents; the pack scan still works on the encrypted index, because it reads the header and scans
    raw bytes rather than listing entries.

Three similarly named filters, easy to confuse:

- `export_filter` — which project files are exported at all (**Resources** tab).
- `include_filter` — extra non-resource files to export (**Resources** tab).
- `encryption_include_filters` — the preset property behind **Filters to include files/folders** on
  the **Encryption** tab: which of the exported files are encrypted. **This is the one that must cover
  `res://.dspkg/toc.key`.**

Why the filter is mandatory: **Encrypt Exported PCK** on its own **encrypts nothing**. Godot decides
per file from the include and exclude globs. With an empty include list the injected key entry would
be written in the clear and anyone holding the pack could open the archive. The exporter checks this
before it injects anything and fails closed when the filter misses; it cannot stop Godot from writing
a pack, so the pack scan is the check you run on the result.

#### Export modes

The exporter builds the archive from the files Godot actually exports. Only `all_resources` is tested
end to end. The subset modes (`scenes`, `resources`, `exclude`, dedicated server) produce an archive
holding only the textures that were exported; that works if a partial archive is what you want, but
verify the result yourself.

### 5. Export

Export normally. During the export the add-on:

1. decides the outcome from the preset (bundling off / plaintext / protected / fail-closed),
2. generates a fresh archive key and archive id,
3. builds `textures.dspkg` with the key and the id,
4. injects `res://.dspkg/toc.key` (48 bytes) into the PCK,
5. skips each bundled texture payload from the PCK,
6. reopens the written archive with the generated key and id as a writer round-trip, and reports
   `Writer round-trip verified`.

The log states which mode was used: `protected`, `plaintext`, or an error explaining why protection
cannot be delivered. If it says plaintext while you expected protection, stop and read the log.

### 6. Verify: boot sentinel and pack scan

Two checks. Both must pass.

```powershell
<export-dir>\game.exe --headless -- --dspkg-validate=protected
```

Require exactly one line of the form:

```text
DSPKG_BOOT status=ok mode=protected entries=<n> key_id=<16 hex>
```

`mode` is the mode you asked to validate; when the boot was not asked to validate, it is the mode
the export recorded. `key_id` is the evidence: `key_id=none` means no key was committed, so the
archive was not opened with one. A missing line is a failure — the exit code is supporting only.

The validate switch turns the line on by itself; `DSPKG_BOOT_VERBOSE=1` is only for launchers that
cannot append user arguments. The full field and switch reference, the failure reasons, and the
ordering case that produces no line at all are in
[verifying-an-exported-build.md](verifying-an-exported-build.md).

Then scan the pack. The scanner ships with the add-on, so there is nothing to copy:

```text
godot --headless --path <your-project> --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack "<export-dir>/game.pck"
```

In the editor, the same scan is **Project > Tools > Verify exported pack (DSPkg)**. It needs neither the
add-on binary nor a key to run: it only reads the pack you point it at.

Require:

```text
DSPKG_SCAN status=ok entries=<n> file=<path>
```

The scanner validates the pack header and searches the raw bytes for the 16-byte key magic. A hit
reports `reason=key_magic_found`, which means the key entry landed outside the encrypted region.

`status=ok` alone does not say whether that pack is encrypted: a plaintext pack passes the same scan.
The editor dialog reports it separately as `Directory encrypted: yes/no`, and that is the line to check
when you meant to ship a protected build. The command line keeps its grammar unchanged, so automation
can rely on one verdict shape.

In CI, set `DSPKG_NO_ALERT=1` (no dialog can block a runner) and `DSPKG_VALIDATE_OUT=<file>` to write
the verdict to a file as well as stdout. Treat the *presence* of the success line as the verdict.

### 7. Deploy

Ship the whole export directory, from a single export. In particular these must stay together:

```text
game.exe
game.pck
textures.dspkg
dsgd_64.dll  (+ dstorage.dll, dstoragecore.dll)
```

**The PCK and the archive are a pair.** Both the archive key and the archive id rotate on every
export, so any patch that rebuilds the pack must redistribute the archive with it. There is no
stable-key or stable-id patch mode.

Then run the built game once on a clean machine: it starts, textures load, no key errors.

---

## What you may say

Allowed, for this configuration:

> The archive key is delivered inside the game's encrypted PCK and is not present in the published
> extension DLL. Without the game's PCK, the archive's table of contents cannot be read, and copying
> the DLL and the archive into another project does not let the engine load those textures. The
> shipped game fails at boot if the archive does not match the build it came with.

Not allowed:

- "The textures are protected." The payload bytes inside the archive are not encrypted.
- "Assets cannot be extracted", "asset confidentiality". The payloads can be recovered without a key.
- "Encrypted payloads." Only the table of contents is encrypted.
- "Integrity protected" or "tamper-proof". The TOC is authenticated; payload bytes are not.
- "Zero-copy" for any encrypted path.
- "Protected on every platform" unless you actually ship the CPU decode path for that platform.
- Any claim that covers loose `.dstex` files streamed through `DSContext`.

If in doubt: describe what the key does, not what an attacker cannot do.

---

## Troubleshooting

Field meanings and switch reference are in
[verifying-an-exported-build.md](verifying-an-exported-build.md); the rows below are what to do
about each symptom in this configuration.

| What you see | What it means |
|---|---|
| Game exits at startup with a pack error | The template was built with a different key than the preset. Rebuild the templates (and mind the Ninja key-file trap). |
| No `DSPKG_BOOT` line at all, stderr shows `Failed loading resource: …dstex` / `Can't load dependency` / `Failed loading scene` | The main scene depends on an archived payload and the archive is missing. The engine loads the main scene **before** autoloads run, so the sentinel never prints and the friendly missing-archive dialog is unreachable for this shape of game. The process still exits 1 with no game. Keep the main scene free of archived dependencies if you want the dialog. |
| `DSPKG_BOOT status=fail reason=missing_archive` | The build expects `textures.dspkg` beside the executable and did not find it. Ship it with the pack. |
| `DSPKG_BOOT status=fail reason=archive_pairing_mismatch` | The archive and the build come from different exports. Redistribute both from one export. |
| `DSPKG_BOOT status=fail reason=archive_unauthorized` | No key entry was readable: the PCK is not mounted, the include filter missed the key path, or the PCK key is wrong. |
| `DSPKG_BOOT status=fail reason=missing_extension` | The extension DLL is missing or failed to load. |
| `DSPKG_BOOT status=fail reason=archive_unreadable` | The archive is truncated or corrupt (`ERR_FILE_CORRUPT` and similar). |
| `status=ok mode=unprotected key_id=none` | The export was not protected: PCK encryption off, or bundling off (`entries=0`). |
| `DSPKG_SCAN status=fail reason=key_magic_found` | The key entry was written in the clear. Fix the encryption include filter and re-export. |
| `DSPKG_SCAN status=fail reason=bad_header` / `directory_past_eof` | The pack is truncated or malformed. A failed export can leave such a file. |
| Export log says protection cannot be delivered | The preset has no PCK key, or the key path is not covered by the encryption filter. The export failed closed: no archive and no texture payload in the PCK. Fix the preset and re-export. |
| Export log says `PLAINTEXT` | Bundling is on but PCK encryption is off. That build has no protection. |
| Editor shows missing packaged textures | You forced archive resolution in the editor without installing a key. Expected; see below. |

---

## Previewing packaged textures in the editor

During normal editing the project uses loose source textures, and `textures.dspkg` is an export
artifact, so nothing needs a key.

If you deliberately want to exercise the packaged path inside the editor, install the same archive key
your export used. It can be read back from the key entry inside your own encrypted pack, which the
editor can mount once it is built with that PCK key, and then handed to
`DSPkg.set_global_encryption_key(...)` as hex. Treat it as a convenience for local testing: it is not
part of the shipped game, and it is not a security control.

---

## Credentials, CI, and version control

- The PCK key lives in `.godot/export_credentials.cfg`. Godot keeps it out of `export_presets.cfg` on
  purpose.
- `.godot/` is gitignored, so **the key does not travel with your repository.** A fresh clone or a CI
  runner has no key until you supply one.
- Supply it at export time from your secret store, not from a committed file.
- Keep the key out of build logs, CI output, and issue reports.
- If you automate exports, read the key from your CI secret store into the environment for that step
  only, and never write it into a file inside the project.

---

## FAQ

**Someone can still rip my textures, right?**
Assume a determined attacker can. The key is not DRM, and it does not make the textures secret: it
raises the cost of the obvious paths and binds the archive to the build it was exported with — see
[What this does not protect](#what-this-does-not-protect).

**Why do I have to compile Godot?**
Because Godot compiles the PCK key into the engine binary. A stock template cannot mount a pack
encrypted with your key, and a stock editor cannot inspect one.

**Can I use the prebuilt extension instead?**
Yes, and it is the normal choice. The prebuilt binaries carry no key, so the DLL holds nothing that
opens the archive.

**Do I have to call `set_global_encryption_key` in my game?**
No. The runtime reads the key from `res://.dspkg/toc.key` itself. The method is for tooling and for
editor previews.

**What if I lose the key?**
You cannot open existing packs or their archives. There is no recovery path, by design.

**Can I reuse one PCK key across several games?**
Yes. Each game still gets its own random archive key and archive id, so the archives are unlinkable
from each other. A leaked PCK key covers every pack built with it.

**Does this break Steam updates?**
No, but turn on `dsgd_extension/steampipe_1mb_alignment` in the export preset before you ship on Steam;
it is off by default. It lays the payloads out in 1 MiB chunks, padding only when a texture would cross
a chunk boundary, so a modified texture rewrites the chunks it occupies instead of shifting every
payload behind it. Without it the archive is packed tightly and any change in a texture's size shifts
every payload behind it, turning a small edit into a large download. The archive header and the
encrypted TOC are still rewritten on every export (new random key and archive id), but they are
metadata-sized; the payload area is what the alignment keeps stable. Either way the PCK and the
archive must ship from the same export.

**Does this work on Linux or macOS?**
The archive and PCK scheme does; the DirectStorage GPU path is Windows. Other platforms decode on the
CPU, and the same archive loads.

**Is the exported game weaker than regular Godot's PCK encryption? (without the addon)**
For the table of contents and the loader, no: it is the same PCK boundary. For the texture payloads,
yes: they are not encrypted in the archive, where the same textures inside an encrypted PCK sit behind
the engine key. That is the documented trade for the GPU decompression path, and it is why the wording
above matters.

---

## Reference

### Files

| File | Purpose |
|---|---|
| `godot.gdkey` | your 64-hex PCK key; never commit |
| `game.exe` | built from a template compiled with that key |
| `game.pck` | encrypted; contains the 48-byte `res://.dspkg/toc.key` entry |
| `textures.dspkg` | the archive; encrypted TOC, plaintext payloads, random archive id |
| `dsgd_64.dll` | the extension; carries no key |

### Preset fields that matter

| Field | Where | Effect |
|---|---|---|
| `encrypt_pck` | export preset | turns PCK encryption on |
| Encryption Key | export preset, stored in `.godot/export_credentials.cfg` | the key compiled into your templates |
| `encryption_include_filters` | export preset | must cover `res://.dspkg/toc.key`, or nothing is encrypted |
| `dsgd_extension/bundle_textures_dspkg` | add-on export option | whether an archive is built at all |
| `dsgd_extension/enforce_archive_pairing` | add-on export option | publishes a random archive id; off publishes `0` and disables the check |
| `dsgd_extension/steampipe_1mb_alignment` | add-on export option | 1 MiB payload chunking so a changed texture only rewrites the chunks it occupies (smaller SteamPipe deltas); off by default |
| `dsgd_extension/include_third_party_notices` | add-on export option | copies `THIRD_PARTY_NOTICES.txt` next to the exported binaries as `dsgd_THIRD_PARTY_NOTICES.txt`; on by default |

### Build inputs

| Input | Where | Effect |
|---|---|---|
| `SCRIPT_AES256_ENCRYPTION_KEY` | Godot build, environment variable | compiles the PCK key into the engine |
| `DSPKG_ENCRYPTION_KEY_FILE=<path>` | DSGD build, `.env` or environment | opts into a legacy build with a baked key: the key is compiled into the DLL (weaker). Empty or unset builds the default shape, with no key in the DLL |
| `-ResetKeyHeader` | `.vscode/build_remote.ps1` | forces the no-key shape (strips a stale baked-key header); refuses to combine with key arguments |

### Related documents

- Godot's own PCK-key guide:
  [Compiling with PCK encryption key](https://docs.godotengine.org/en/stable/engine_details/development/compiling/compiling_with_script_encryption_key.html).
- Verification reference, the two checks in full (sentinel fields and switches, failure reasons, pack
  scan, CI):
  [verifying-an-exported-build.md](verifying-an-exported-build.md).
- Export and archive loading behaviour, the implementation reference for the exporter, the runtime
  contract and the boot sentinel:
  [for_developers/export-and-archive-behaviour.md](for_developers/export-and-archive-behaviour.md).
