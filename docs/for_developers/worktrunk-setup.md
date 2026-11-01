# Worktrunk setup: parallel agent worktrees on a new device

This is the install-and-setup guide for running several agents on this repository at once, each in its
own git worktree, sharing one build cache and one prebuilt godot-cpp. By the end of it a new device can
create a worktree, build it, run the native suite, land exactly one commit on the target branch with no
merge commit, and clean up — which is the same sequence the `do-it-in-a-worktree` skill tells agents to
follow.

Verified on Windows against worktrunk v0.81.0 and this repository at `b19ee59`, 2026-11-02. Everything
labelled "measured" was run on that device; the `wt step push` behaviour comes from worktrunk's docs and
has not been exercised here yet.

---

## 1. What this gives you

Worktrees give each agent its own checkout, index, object files, addon binaries and gdextest fixture, so
two agents can edit, build and test at the same time without touching each other's state. The expensive
parts are shared instead of duplicated:

| Resource | Shared | Per worktree |
|---|---|---|
| SCons object cache | `.cache/scons` (about 700 MB once tests are built) | — |
| godot-cpp build | `.cache/godot-cpp-shared` + hardlinks (about 16 MB of real data) | — |
| Checkout, objects, DLLs, fixture | — | about 90 MB |
| Submodule git data | — | about 48 MB in `.git/worktrees/<branch>/modules` |

Measured cost of a new worktree: **about 140 MB** and **about 40 s** for a `template_debug` build
(about 1.2 min with `-IncludeTests`). The same build costs about 1 GB and 6 minutes when the shared
pieces are missing.

---

## 2. Prerequisites

Install these before touching worktrunk; the hooks assume all of them.

| Requirement | Why | Check |
|---|---|---|
| Windows 10/11 | hooks and scripts are PowerShell + Win32 | — |
| Git for Windows, 2.43 or newer | worktrunk requires it, and **hooks run through Git Bash** even when your shell is PowerShell | `git --version` |
| Visual Studio Build Tools with MSVC, C++20 | the SConstruct builds with MSVC on Windows | `where cl` inside a Developer prompt, or just build once |
| Python 3.8+ and SCons | SCons drives the build; the scripts call `python`/`py` | `python --version`, `scons --version` |
| Godot 4.7.2 **console** executable | headless test runs need the console build | `GODOT_CONSOLE_PATH` in `.env` |
| Network access on first worktree | the first `git submodule update` clones submodule repositories (including a nested one) | — |
| Optional: sccache | only for the opt-in intra-worktree compiler cache; not needed for any of the above | `sccache --version` |

The submodule clone is a one-time cost per worktree, not per build: later builds reuse the working
trees checked out there.

---

## 3. Install worktrunk

```powershell
winget install --id max-sixty.worktrunk
```

`cargo install worktrunk` works too. Then verify what `wt` actually resolves to:

```powershell
wt --version        # must print something like: wt v0.81.0
```

`wt.exe` is also the name of Windows Terminal's executable, so on a machine that has Terminal, `wt`
can resolve to the wrong program. Two ways out:

- use the `git-wt` alias worktrunk also installs (`git wt --version`), or
- make sure the worktrunk shim comes first on `PATH` (`%LOCALAPPDATA%\Microsoft\WinGet\Links`).

If `wt --version` prints nothing resembling `wt v0.xx`, fix the `PATH` order before continuing — every
command in this guide is written as `wt`, but `git wt` is a drop-in replacement.

---

## 4. Point worktrunk at this repository's layout

Worktrees must live **inside** the repository (`.worktrees/<branch>`), not beside it:

- agents are sandboxed to the project root, so a sibling worktree would be unreadable and unwritable
  for them;
- the repository already ignores `.worktrees/` and the codegraph index excludes it.

Create the user config and set the template:

```powershell
wt config create
```

Then make sure this line is present in `%APPDATA%\worktrunk\config.toml`:

```toml
worktree-path = "{{ repo_path }}/.worktrees/{{ branch | sanitize }}"
```

Verify it resolves as expected with a throwaway branch name:

```powershell
wt step eval "{{ expected_worktree_path_of_branch('example-branch') }}"
# -> /d/.../godot-directstorage-extension/.worktrees/example-branch   (MSYS form of the same path)
```

Use single quotes *inside* a double-quoted PowerShell string, as above: PowerShell strips double quotes
from arguments passed to native commands, and the template then fails to parse and is echoed back
unexpanded.

`wt switch --path <template>` can override this per command, but keeping it in the config means every
`wt switch`, `wt list`, `wt step relocate` and `wt step prune` agrees on one layout without flags.

Shell integration (`wt config shell install`) is optional. It gives interactive shells an automatic
`cd` after `wt switch`; agents and scripts should `cd .worktrees/<branch>` explicitly instead, because a
non-interactive process never receives that `cd`.

---

## 5. Prepare the clone

```powershell
git submodule update --init --recursive
```

`.env` is gitignored, so a new device has none and `build_remote.ps1` fails with a missing-config
error. Copy the example and set the values that matter:

```powershell
Copy-Item .env.example .env
```

| Key | Set it to |
|---|---|
| `REMOTE_BUILD` | **The switch that decides where `build_remote.ps1` builds:** `0` builds on this device, `1` offloads the build to the remote host (`-Remote`/`-Local` override it per run). Worktrees require `0`. |
| `PREVENT_BUILD_ON_THIS_DEVICE` | A hard lock on local `scons`, not a redirect: `1` makes any local SCons invocation exit with "Local builds are disabled on this device". It does not send builds to the remote host — `REMOTE_BUILD` does. Keep `0` for worktrees. |
| `GODOT_CONSOLE_PATH` | absolute path to the Godot console executable |
| `ENABLE_POST_BUILD_ADDON_SYNC` | `1` in the primary if you deploy to an integration project; the worktree hook forces `0` per worktree |
| `SYNC_ADDON_DIR`, `SYNC_TEST_DIR` | only if you actually deploy there |
| `DSPKG_ENCRYPTION_KEY_FILE` | leave empty unless you deliberately bake a fixed key |

Do not put a key assignment on a comment line: this workspace's IDE exports `.env` into every terminal
and ignores `#` markers, so `# KEY=value` still becomes a live environment variable.

If this device will offload builds to another machine (`REMOTE_BUILD=1`), stop here — remote mode is not
parallel-safe (see section 11).

The two settings are independent, which is easy to get wrong:

- `REMOTE_BUILD=1` + `PREVENT_BUILD_ON_THIS_DEVICE=1` is the offloading setup. `build_remote.ps1` runs
  SCons on the remote host, and the lock stops anyone on this device — a person or an agent — from
  quietly building locally against a half-populated tree.
- `REMOTE_BUILD=0` + `PREVENT_BUILD_ON_THIS_DEVICE=1` simply fails every build. That combination is the
  usual mistake: the lock refuses local SCons, and nothing was configured to build remotely.
- Worktrees need `REMOTE_BUILD=0` + `PREVENT_BUILD_ON_THIS_DEVICE=0`, i.e. local builds that are
  actually allowed to happen.

---

## 6. Prime the shared caches once

The worktree hooks share godot-cpp, but something has to build it first. In the primary checkout:

```powershell
powershell -ExecutionPolicy Bypass -File .vscode/build_remote.ps1 -Local -Target template_debug
```

About 6 minutes cold. This produces the two things worktrees reuse:

- `godot-cpp/gen` — the generated bindings;
- `godot-cpp/bin/libgodot-cpp.windows.*.lib` — one static library per target/dev_build variant.

Without them a worktree still works, but the bootstrap prints "primary godot-cpp/gen is not generated
yet — skipping prebuilt sharing", the worktree's `.env` gets `GODOT_CPP_PREBUILT=0`, and that worktree
compiles godot-cpp itself (about 1 GB, 6 minutes). Re-run the `pre-start` hooks inside that worktree
once the primary has built (the hooks must be committed on its branch), or simply recreate it.

Optional: make the primary use the shared object cache too by adding `SCONS_CACHE=<repo>\.cache\scons`
to `.env`. The first build after that rebuilds everything once (SCons switches to content signatures),
then it behaves like a worktree. Worktrees always get this line from the hook, so leaving the primary
alone changes nothing for them.

---

## 7. Approve the project hooks

`.config/wt.toml` defines three `pre-start` commands. Worktrunk asks for approval the first time it
would run them; approve once and it is remembered in `%APPDATA%\worktrunk\approvals.toml`.

For unattended runs add `--yes` to `wt switch`, or pre-approve:

```powershell
wt config approvals add
```

The hooks must be **committed on the branch you branch from**. Worktrunk reads the project config from
the worktree the command runs in, so `wt switch --create` works from the primary even with an
uncommitted `.config/wt.toml`, but `wt hook …` run from inside a worktree prints "No pre-start hooks
configured" unless the file is committed there.

---

## 8. Smoke test the whole workflow

Create a throwaway worktree from the current branch. `--base=@` is required: `--create` otherwise bases
the branch on `main`, not on your working branch.

```powershell
wt switch --create wt-smoke --base=@ --yes
cd .worktrees/wt-smoke
```

Expected: "Created branch wt-smoke from <your branch>", then three hook lines — submodules, the shared
godot-cpp link, and the `.env` write. Confirm the result:

```powershell
(Get-Item godot-cpp/gen).LinkType                  # Junction
(Get-ChildItem godot-cpp/bin -Filter *.lib).Count  # one per built variant
Select-String .env -Pattern 'SCONS_CACHE|GODOT_CPP_PREBUILT'
```

Build and test:

```powershell
powershell -ExecutionPolicy Bypass -File .vscode/build_remote.ps1 -Local -Target template_debug -IncludeTests
powershell -ExecutionPolicy Bypass -File .vscode/run_gdextest_local.ps1
```

A brand-new worktree has no `project/.godot`, and the build's GDScript verification needs
`project/.godot/extension_list.cfg` to resolve GDExtension classes. The verification step now imports
the project once by itself, so the build repairs an unimported checkout instead of aborting. Run the
same helper directly when you build with `-SkipVerify`, or before anything else that needs the classes:

```powershell
powershell -ExecutionPolicy Bypass -File tools/ensure_project_imported.ps1
```

About 6 seconds, and it writes the cfg. Without it a `-SkipVerify` build still succeeds while a later
verified step fails with `Parse Error: Identifier "DSPkg" not declared` in `ds_dspkg_static.gd` — the
branch is fine, the checkout has simply never been imported. `--path project --quit` (without
`--editor`) does not import, and `-SkipVerify` skips the check altogether, so a green `-SkipVerify`
build says nothing about GDScript validity.

Measured expectations: about 40 s for the plain build, about 1.2 min with tests, and the headless suite
reporting **168 passed / 0 failed / 30 skipped**. The skips are the D3D12/GPU gates, not failures.

Land a commit and clean up (see section 9 for the commit step in a real task):

```powershell
wt remove wt-smoke
```

---

## 9. The workflow, in short

```powershell
# 1. create, from the branch you want to build on
wt switch --create <branch> --base=@ --yes
cd .worktrees/<branch>

# 2. edit, build, test inside the worktree
#    first time only, and unnecessary with a verified build: tools/ensure_project_imported.ps1
powershell -ExecutionPolicy Bypass -File .vscode/build_remote.ps1 -Local -Target template_debug
powershell -ExecutionPolicy Bypass -File .vscode/run_gdextest_local.ps1        # after -IncludeTests

# 3. one commit on the worktree branch (only if the user asked for a commit)
git add -A
git commit -m "feat(scope): ..."

# 4. land it as a single commit, no merge commit (only if the user asked to land)
git log --oneline <target>..HEAD      # expect exactly one line
wt step push <target>                 # fast-forward; updates the worktree that holds <target>
wt remove <branch>
```

`wt step push` is the worktrunk way; `git merge --ff-only <branch>` from the primary is the plain-git
equivalent. If the target moved since you branched, rebase first (`wt step rebase <target>`) and push
again — still linear, still no merge commit. Do **not** use `wt merge` when the commit must land as-is:
it squashes and generates the message.

After `wt remove`, expect a line like "Branch unmerged; to delete, run wt remove -D <branch>". That is
not the removal failing: the worktree is already gone. Worktrunk only deletes a branch automatically
when its content is in the **default branch** (`main`), and work branched from `dspkg-encryption` is not.
Finish with `wt remove -D <branch>` or `git branch -D <branch>` once the commits are on your target.

Steps 3 and 4 are opt-in. A change that only needs to exist somewhere does not need a commit, and a
request to "change X" is not a request to move the user's branch: landing rewrites the files in the
primary checkout, which may be exactly what the user is editing.

### Files a worktree cannot see

A worktree is a checkout of a commit, so it contains none of the primary's uncommitted work and none of
the ignored files. This repository ignores `.agents/skills/audit-pipeline`, `.agents/skills/xmlq`,
`.agents/skills/godot-specifics` and `.env`, so those never appear in a worktree at all. Editing them is
a primary-checkout job; a copy placed inside a worktree is an untracked duplicate that no commit can
carry back to the user's tree.

For tracked files with uncommitted edits, pick one of these and say which you chose:

- commit or stash them first (simplest, and the user owns that state);
- snapshot without disturbing the working tree — `git stash create` prints a commit of the dirty tracked
  state, then `git worktree add -b <branch> <path> <sha>`, after `git add -N <file>` for files git does
  not know yet;
- move the work with `git stash push -u`, then `git stash pop` inside the worktree, which empties the
  primary's working tree and needs explicit agreement.

Agents should follow `.agents/skills/do-it-in-a-worktree/SKILL.md`, which carries the same steps plus
the guardrails and failure signatures.

---

## 10. What lives where

| Path | Contents | Safe to delete? |
|---|---|---|
| `.worktrees/<branch>` | that worktree's checkout and build output | yes, via `wt remove` |
| `.git/worktrees/<branch>` | per-worktree git state and cloned submodule repos | yes, removed by `wt remove` |
| `.cache/scons` | shared object cache for every worktree | yes — it is a cache; the next build refills it, cold |
| `.cache/godot-cpp-shared` | generated bindings plus the libs exposed as hardlinks | **not freely** — worktrees hold junctions into it; remove the worktrees first, or re-run the hook afterwards |
| `.cache/buildcache-bin` | optional compiler-cache tool | yes, if unused |

The SCons cache has no size cap and no eviction, so it grows with every target/variant combination;
prune it when you change target sets. Removing a worktree deletes its links, never the shared store.

---

## 11. Resource planning

- **CPU.** SCons uses `cores - 1` jobs per invocation (7 on an 8-core machine). Three simultaneous
  builds oversubscribe; stagger them, or build sequentially when timing matters.
- **Disk.** One built worktree with tests is about 330 MB real (plus hardlinks that cost nothing). Plan
  roughly 150 MB per editing worktree and 350 MB per built-and-tested one.
- **Remote builds stay serial.** With `REMOTE_BUILD=1` every run shares one `REMOTE_DIR`, one
  "SCons succeeded" marker, and `verify_remote.ps1 -CleanOrphans` task-kills by build directory, so
  parallel remote builds corrupt each other. Parallel worktrees require local builds.

---

## 12. Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `wt --version` shows Windows Terminal | `wt.exe` from Terminal won on `PATH` | use `git wt`, or put the worktrunk shim first |
| New branch starts at `main` | `--base=@` omitted | recreate the worktree with `--base=@` |
| Worktree has no `.env`, no junction, `GODOT_CPP_PREBUILT=0` | the worktree's `SConstruct` predates the shared-artifact support, or the hook was interrupted | commit the setup on the target branch and recreate the worktree; re-running `wt hook pre-start --yes` *inside* that worktree also works once its config is committed |
| `No pre-start hooks configured` | ran `wt hook` inside a worktree whose branch has no committed `.config/wt.toml` | commit the config, or create worktrees from the primary |
| `[SCons] ERROR: this worktree shares godot-cpp … but GODOT_CPP_PREBUILT is not set` | shared artifacts present, prebuilt mode disabled | set `GODOT_CPP_PREBUILT=1` in the worktree `.env`, or delete `godot-cpp/gen` and `godot-cpp/bin/*.lib` to build godot-cpp there |
| `Another build is already running for this workspace` | two builds in the *same* worktree | wait for the first to finish |
| `0 tests ran` with a valid filter | the test binary was not rebuilt | build with `-IncludeTests` before running the suite |
| Suite never returns | native deadlock, not a test failure | bisect with `-Only`/`-Count`, keep the `--log-file` output |
| Cold `--import` aborts on first run | pre-existing engine issue (#0169) | rerun; the warm pass exits 0 |
| First worktree creation is slow and prints "Cloning into …" | submodule repositories are cloned per worktree | expected once per worktree; needs network |
| `wt switch`, `git` or the hooks print red `NativeCommandError` text in PowerShell | PowerShell reports a native command's stderr that way; the commands still succeed | check `$LASTEXITCODE` (0) rather than the colour |
| `wt remove` prints "Branch unmerged; to delete, run wt remove -D …" | branch deletion is judged against the default branch (`main`), and the work sits on `dspkg-encryption` | the worktree is already removed; finish with `wt remove -D <branch>` or `git branch -D <branch>` once the commits are on your branch |
| `[BUILD FAILED] GDScript Verification`, `Identifier "DSPkg" not declared` | the checkout has no `.godot/extension_list.cfg`, so `--check-only` resolves no GDExtension classes | a verified build now imports the project itself; if it still fails, run `tools/ensure_project_imported.ps1`, confirm the cfg appears, then rebuild |
| `godot --headless --path project --quit` never returns | `--quit` only ends the main loop after one iteration; it does not import | use `--headless --editor --quit-after 4 --path project` for the one-shot warm-up |
| Deployment audit reports `dsgd_64.dll` missing | only `template_debug` was built, and the extension manifest declares both the debug and release libraries | expected for a single-target build; build `template_release` as well if the audit must be clean |

---

## 13. Opting out or uninstalling

- **Per worktree, keep godot-cpp local:** comment out the `godotcpp` block in `.config/wt.toml` (or pass
  no `-GodotCppPrebuilt`), and delete `godot-cpp/gen` plus `godot-cpp/bin/*.lib` in that worktree. It
  then builds godot-cpp itself, at the old cost.
- **Stop using worktrees:** work directly in the primary checkout; nothing in the build requires
  worktrunk.
- **Remove shell integration:** `wt config shell uninstall`.
- **Remove worktrunk:** uninstall the package and delete `%APPDATA%\worktrunk\` (config, approvals,
  caches) and each `.worktrees/<branch>` entry via `wt remove` first.

---

## 14. Notes for agents (and their operators)

1. Worktrees must stay inside the repository. If you place them elsewhere, add the parent directory to
   `.agents/PATH_ALLOWLIST.md`; the sandbox otherwise refuses to read or write them.
2. Add `wt` (and `sccache`, if used) to `.agents/PATH_ALLOWLIST.md`, or agents cannot execute them.
3. Commit the worktree tooling — `SConstruct`, `.config/wt.toml`, `tools/wt_bootstrap_env.ps1`,
   `tools/wt_share_godot_cpp.ps1`, `.worktreeinclude`, `.gitignore`, `codegraph.json` — before asking an
   agent to create a worktree. Otherwise the bootstrap correctly degrades to a local godot-cpp build and
   the agent gets a 1 GB, 6-minute build instead of a 140 MB, 40-second one.
4. Never `git clean -xdf` in a worktree (kills `.env`) or in the primary (deletes `.cache` and leaves
   every shared worktree with a dangling `godot-cpp/gen` junction).
