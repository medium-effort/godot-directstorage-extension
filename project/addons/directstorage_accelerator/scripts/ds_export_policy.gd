@tool
extends RefCounted

## Pure decision helpers for the DirectStorage exporter.
##
## Extracted from ds_export_plugin.gd so the shipping rules can be asserted in a
## plain headless test (project/headless_tests/test_export_policy.gd) without a
## Godot export run, the native extension, or an EditorExportPlugin context.
## No side effects: callers apply the result.

const MODE_PCK: String = "pck"
const MODE_NONE: String = "none"

## Leave the texture in the PCK (intended CPU fallback, or nothing was skipped).
const ACTION_PACK: String = "pack"
## Remove the texture from the PCK: a protected build must not ship plaintext.
const ACTION_SKIP: String = "skip"
## Add the texture to the archive.
const ACTION_ADD: String = "add"
## Delete the archive and the staging file: nothing is salvageable.
const ACTION_DELETE: String = "delete"
## Deploy the partial archive: skipped textures exist nowhere else.
const ACTION_SALVAGE: String = "salvage"


## Whether the shipped game must expect an archive beside the executable.
##
## A whole-project scan cannot prove what the export ships, and `project.binary`
## is written after the file walk, so the claim is made only once at least one
## texture was actually added. An export filter, a `.gdignore` directory or a
## subset mode can all remove every texture.
static func resolve_bundle_effective(p_bundle_enabled: bool, p_project_has_textures: bool, p_bundle_possible: bool, p_archive_entry_count: int) -> bool:
	return p_bundle_enabled and p_project_has_textures and p_bundle_possible and p_archive_entry_count > 0


## What to do with one exported file.
##
## `p_archive_available` is false when the DSPkg object is gone (creation failed,
## or a previous add aborted). A protected build must then keep the payload out
## of the PCK; an unprotected build keeps it so CPU mode still works.
static func resolve_file_action(p_bundle_enabled: bool, p_archive_failed: bool, p_archive_available: bool, p_protection_mode: String) -> String:
	if not p_bundle_enabled:
		return ACTION_PACK
	if p_archive_failed or not p_archive_available:
		return ACTION_SKIP if p_protection_mode == MODE_PCK else ACTION_PACK
	return ACTION_ADD


## What to do with the archive after a failed export.
##
## Delete when a protected build must fail loudly, or when no texture was
## skipped yet (the PCK still holds every texture). Salvage only when entries
## were already removed from the PCK and exist nowhere else.
static func resolve_failure_action(p_protection_mode: String, p_archive_entry_count: int) -> String:
	if p_protection_mode == MODE_PCK or p_archive_entry_count == 0:
		return ACTION_DELETE
	return ACTION_SALVAGE
