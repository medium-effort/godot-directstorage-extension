@tool
extends SceneTree

## Migration tool restore-path safety test.
##
## Author: MediumEffort
## Description: Guards the restore flow against driving the editor's filesystem
## from inside the tool. The restore would otherwise reach
## EditorFileSystem::_update_scan_actions -> reimport_files -> EditorProgress ->
## EditorNode::progress_add_task -> ProgressDialog::add_task, and the engine frees
## that dialog in EditorNode's EXIT_TREE notification while EditorNode::singleton
## is still set. A scan/import still settling during editor shutdown then
## dereferences the freed dialog (measured 0xc0000005 inside ProgressDialog::add_task).
##
## The restore must therefore commit the sidecars on disk and hand the reimport to
## an editor restart, not to scan()/reimport_files(); the migration path keeps its
## in-session reimport and is asserted to stay unchanged.
##
## The restore path only runs inside the editor, so these are source-level
## guarantees plus a scene-parse check, matching test_runtime_manager_gating.gd.

const DIALOG_SCRIPT_PATH = "res://addons/directstorage_accelerator/tools/ds_migration_dialog.gd"
const DIALOG_SCENE_PATH = "res://addons/directstorage_accelerator/tools/ds_migration_dialog.tscn"

var _failures: int = 0


func _init() -> void:
	print("[Test Restore Safety] Starting...")

	var script: GDScript = load(DIALOG_SCRIPT_PATH)
	if script == null:
		_fail("could not load " + DIALOG_SCRIPT_PATH)
		_finish()
		return

	var source := script.source_code

	var confirm := _strip_comments(_function_body(source, "_on_confirm_dialog_confirmed"))
	_check(not confirm.is_empty(), "confirmation handler exists")
	_check("CONFIRM_RESTART" in confirm, "confirmation handler dispatches the restart prompt")
	_check("hide()" in confirm, "tool window is hidden before the restart request")
	_check("_request_editor_restart.call_deferred" in confirm, "restart is requested after the window is hidden")
	_check(not ("restart_editor" in confirm), "confirmation handler does not restart while its window is up")

	var restart := _strip_comments(_function_body(source, "_request_editor_restart"))
	_check("EditorInterface.restart_editor(true)" in restart, "restart is requested with a scene save")
	_check("is_inside_tree()" in restart, "restart request survives a freed tool window")
	_check("is_scanning()" in restart, "restart waits for a scan still in flight")
	_check("is_importing()" in restart, "restart waits for an import still in flight")
	_check("RESTART_SETTLE_TIMEOUT_MS" in restart, "restart settle wait is bounded")

	var canceled := _strip_comments(_function_body(source, "_on_confirm_dialog_canceled"))
	_check("CONFIRM_RESTART" in canceled, "cancel only acts on the restart prompt")
	_check("_reimport_after_skipped_restart" in canceled, "cancelling the restart reimports")

	var cancel_import := _strip_comments(_function_body(source, "_reimport_after_skipped_restart"))
	_check("reimport_files" in cancel_import, "skipped restart imports the restored sources")
	_check("is_importing()" in cancel_import, "skipped restart waits for the import")
	_check("running the game is safe now" in cancel_import, "skipped restart reports when the game is safe")
	_check("do not run the game" in cancel_import, "skipped restart warns before the import finishes")

	var ready_body := _strip_comments(_function_body(source, "_ready"))
	_check("_confirm_dialog.canceled.connect" in ready_body, "dialog cancel is connected")
	_check("_restored_sources" in _strip_comments(_function_body(source, "_begin_restore")), "restore keeps the restored sources")

	var restore := _strip_comments(_function_body(source, "_begin_restore"))
	_check(not restore.is_empty(), "restore handler exists")
	_check("is_scanning()" in restore, "restore refuses while the editor is scanning")
	_check("is_importing()" in restore, "restore refuses while the editor is importing")
	_check("_is_busy" in restore, "restore cannot run twice concurrently")
	_check("CORE.restore_from_zip" in restore, "restore commits the backup on disk")
	_check(not ("reimport_files" in restore), "restore does not drive EditorFileSystem.reimport_files")
	_check(not ("fs.scan" in restore), "restore does not start a filesystem scan")
	_check(not ("take_over_path" in restore), "restore does not orphan live textures")
	_check("_prompt_restart" in restore, "restore asks for the restart that reimports")

	var prompt := _strip_comments(_function_body(source, "_prompt_restart"))
	_check("_pending_action = CONFIRM_RESTART" in prompt, "restart prompt arms the restart confirmation")
	_check("_confirm_dialog.popup_centered" in prompt, "restart prompt is shown")
	_check("Restart the editor now?" in prompt, "restart prompt asks before restarting")
	_check("If the editor does not come back" in prompt, "restart prompt explains the manual fallback")
	_check("cancel_button_text = RESTART_CANCEL_TEXT" in prompt, "restart prompt labels the reimport-only cancel")

	# The scan-driven machinery is gone; none of its helpers may come back.
	_check(not ("RESTORE_SCAN_TIMEOUT_MS" in source), "no post-restore scan timeout constant")
	_check(not ("_finish_restore_after_scan" in source), "no deferred scan-driven restore helper")
	_check(not ("_wait_for_filesystem_idle" in source), "no filesystem-idle wait helper")

	# Scope guard: the migration path keeps its batched in-session reimport.
	var migrate := _strip_comments(_function_body(source, "_on_migrate_pressed"))
	_check("reimport_files" in migrate, "migration path still reimports in batches")
	_check("BATCH_SIZE" in migrate, "migration path still uses the batch size")

	_check(load(DIALOG_SCENE_PATH) != null, "dialog scene parses")

	_finish()


## Returns the authored body of [param func_name] up to the next top-level `func`.
static func _function_body(source: String, func_name: String) -> String:
	var start := source.find("func %s(" % func_name)
	if start < 0:
		return ""
	var next := source.find("\nfunc ", start + 1)
	if next < 0:
		return source.substr(start)
	return source.substr(start, next - start)


## Drops `#` comments so a "do not call X" note cannot pass for a call. No string
## literal in the dialog contains a `#`, so trimming at the first `#` is safe here.
static func _strip_comments(text: String) -> String:
	var out := PackedStringArray()
	for line in text.split("\n"):
		var hash := line.find("#")
		if hash >= 0:
			line = line.substr(0, hash)
		out.append(line)
	return "\n".join(out)


func _check(condition: bool, what: String) -> void:
	if condition:
		print("[Test Restore Safety] PASS: " + what)
	else:
		_fail(what)


func _fail(what: String) -> void:
	_failures += 1
	push_error("[Test Restore Safety] FAIL: " + what)


func _finish() -> void:
	if _failures > 0:
		print("[Test Restore Safety] FAILED (%d)" % _failures)
		quit(1)
		return
	print("[Test Restore Safety] SUCCESS")
	quit(0)
