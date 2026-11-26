# Contributing to godot-directstorage-extension (`dsgd`)

Thank you for contributing to `dsgd`!

## Project Licensing

- **Core & Editor Tooling (`MPL-2.0`):** The repository is licensed under the **Mozilla Public License, v. 2.0** ([LICENSE](LICENSE)).
- **Shipped Runtime Helpers (`MIT`):** Four runtime GDScript helpers are licensed under the **MIT License** ([LICENSE-MIT](LICENSE-MIT)) so game developers can freely adapt them in their projects:
  - `project/addons/directstorage_accelerator/scripts/ds_runtime.gd`
  - `project/addons/directstorage_accelerator/scripts/ds_preloader.gd`
  - `project/addons/directstorage_accelerator/scripts/ds_batch_manager.gd`
  - `project/addons/directstorage_accelerator/scripts/ds_dspkg_static.gd`
- **Third-Party Components:** See [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).
- **License Mirrors:** `LICENSE`, `LICENSE-MIT`, and `THIRD_PARTY_NOTICES.txt` at the repository root must remain byte-identical to their mirrors inside `project/addons/directstorage_accelerator/`.

### SPDX Headers for New Files

Shipped first-party code (`src/` excluding `src/gen/`, and `project/addons/directstorage_accelerator/**/*.gd`) must carry SPDX headers. Non-shipped repository files (`tests/`, `project/tests/`, `project/demo/`, `tools/`, `SConstruct`) are governed by the root [LICENSE](LICENSE) (`MPL-2.0`) by default.

- **New C/C++ files under `src/` (`MPL-2.0`):**
  ```cpp
  // SPDX-FileCopyrightText: 2026 MediumEffort
  // SPDX-License-Identifier: MPL-2.0
  ```
- **New addon editor/tooling `.gd` scripts (`MPL-2.0`):**
  ```gdscript
  # SPDX-FileCopyrightText: 2026 MediumEffort
  # SPDX-License-Identifier: MPL-2.0
  ```
- **The four MIT runtime `.gd` scripts (`MIT`):**
  ```gdscript
  # SPDX-FileCopyrightText: 2026 MediumEffort
  # SPDX-License-Identifier: MIT
  ```

## Contributor License Agreement (CLA)

Before your pull request can be merged, you must sign our [Contributor License Agreement](CLA.md).

### Why a CLA is required
Under MPL-2.0 alone, each contributor holds copyright and licenses their changes to the project under MPL-2.0 only, which would prevent the maintainer from ever relicensing `dsgd` (or extracted components) under an OSI-approved permissive license like MIT or Apache-2.0 without tracking down every past contributor for written permission.

Our [CLA](CLA.md) is a **license-grant CLA**:
- You **keep copyright** to your own code.
- You grant **MediumEffort** permission to distribute your contribution under `MPL-2.0` (or `MIT` for MIT-designated files) and to relicense future releases under the following OSI-approved permissive open-source licenses: `MIT`, `Apache-2.0`, `BSD-2-Clause`, `BSD-3-Clause`, `ISC`, or `Zlib`.
- It does **not** allow taking your contributions closed-source/proprietary.

### How the automated CLA check works
1. Open your pull request against `main`.
2. If you have not signed before, the CLA workflow (`github-actions[bot]`) will post a comment linking to [CLA.md](CLA.md). **Every commit author** in a pull request must sign.
3. Reply on the pull request with a standalone comment containing **only** the exact text below (no backticks, quotes, or extra text):
   ```text
   I have read the CLA Document and I hereby sign the CLA
   ```
   If you need to re-trigger the check after signing or updating commits, comment `recheck`.
4. The workflow records your GitHub username and timestamp on the `cla-signatures` branch (`signatures/version1/cla.json`) and marks the CLA status check as passing. You only need to sign once.

## Important Rules for Contributors

- **Never commit Microsoft DirectStorage binaries:** Do not commit `dstorage.dll`, `dstoragecore.dll`, `dstorage.lib`, or preview runtime DLLs. They are EULA-bound and fetched locally per checkout via `tools/fetch_directstorage.ps1` (see [README.md](README.md), Section 8.1).
- **Commit & PR conventions:** Target `main` and use Conventional Commits prefixes for commit messages (e.g., `feat(...)`, `fix(...)`, `test(...)`, `docs(...)`).

## Verification Before Opening a PR

- **License & SPDX check (local):** Run `python tools/check_license.py` to verify license files, addon mirrors, and SPDX headers (this check is run locally and is not wired into CI).
- **Native build & `gdextest` C++ tests:**
  - Cross-platform: build with `scons target=editor tests=true` and run `python extern/gdextest/tools/gdextest.py test`.
  - Windows helpers: see [README.md](README.md) (Section 8.3) for the `.vscode/build_remote.ps1` and `.vscode/run_gdextest_local.ps1` workflow.
- **GDScript unit tests (`GUT`):** Run `python tools/run_gut.py`. Requires a built extension library and the Godot 4.7.2 console executable (`GODOT_CONSOLE_PATH`, `GODOT_BIN`, `GODOT`, `--godot`, or on `PATH`); the runner fetches GUT automatically unless `--no-fetch` is passed.
- **Class documentation check:** Run `python tools/check_doc_classes.py` when modifying C++ class bindings or `doc_classes/` XML documentation.
- **Build & setup reference:** Follow the full prerequisites and build instructions in [README.md](README.md).
