# SPDX-FileCopyrightText: 2026 MediumEffort
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

@tool
extends RefCounted

## Pure decision helpers for the DirectStorage exporter.
##
## Extracted from ds_export_plugin.gd so the shipping rules can be asserted in a
## plain headless test (project/headless_tests/test_export_policy.gd) without a
## Godot export run, the native extension, or an EditorExportPlugin context.
## No side effects: callers apply the result.

## Bundling is off: no archive is built and every payload stays in the PCK.
const OUTCOME_DISABLED: String = "disabled"
## Bundling is on but the preset does not request PCK encryption: the archive is
## written in plaintext and carries no key.
const OUTCOME_PLAINTEXT: String = "plaintext"
## PCK encryption was requested and the key file will land inside the encrypted
## region: the archive is written with a random per-export key.
const OUTCOME_PROTECTED: String = "protected"
## PCK encryption was requested but the key file cannot be delivered: the export
## fails closed, writing no archive and leaving no plaintext payload in the PCK.
const OUTCOME_FAILED: String = "failed"

## Leave the texture in the PCK (intended CPU fallback, or nothing was skipped).
const ACTION_PACK: String = "pack"
## Remove the texture from the PCK: the archive is its only copy.
const ACTION_SKIP: String = "skip"
## Add the texture to the archive.
const ACTION_ADD: String = "add"
## Delete the archive and the staging file: nothing is salvageable.
const ACTION_DELETE: String = "delete"
## Deploy the partial archive: skipped textures exist nowhere else.
const ACTION_SALVAGE: String = "salvage"


## Whether the shipped game must expect an archive beside the executable.
##
## `project.binary` is written before the archive is finalized, so the claim is
## based on what was actually removed from the PCK: if any payload was skipped,
## the game can only load it from the archive. Zero skips means the PCK is
## self-sufficient, even when a filter removed every texture.
static func resolve_bundle_effective(p_bundle_enabled: bool, p_skipped_payload_count: int) -> bool:
	return p_bundle_enabled and p_skipped_payload_count > 0


## What to do with one texture payload.
##
## `p_archive_available` is false when the DSPkg object is gone (creation failed,
## or a previous add aborted). A protected build must then keep the payload out
## of the PCK; a plaintext build keeps it so CPU mode still works. The failed
## outcome never adds anything: it exists to fail closed.
static func resolve_file_action(p_outcome: String, p_archive_failed: bool, p_archive_available: bool) -> String:
	if p_outcome == OUTCOME_DISABLED:
		return ACTION_PACK
	if p_outcome == OUTCOME_FAILED:
		return ACTION_SKIP
	if p_archive_failed or not p_archive_available:
		return ACTION_SKIP if p_outcome == OUTCOME_PROTECTED else ACTION_PACK
	return ACTION_ADD


## What to do with the archive after a failed export.
##
## Delete when a protected build must fail loudly, when the gate itself failed,
## or when no texture was skipped yet (the PCK still holds every texture).
## Salvage only when payloads already removed from the PCK exist nowhere else.
static func resolve_failure_action(p_outcome: String, p_archive_entry_count: int) -> String:
	if p_outcome == OUTCOME_PROTECTED or p_outcome == OUTCOME_FAILED or p_archive_entry_count == 0:
		return ACTION_DELETE
	return ACTION_SALVAGE
