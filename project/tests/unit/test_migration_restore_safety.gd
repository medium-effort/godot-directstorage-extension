extends GutTest

const DIALOG_SCRIPT_PATH := "res://addons/directstorage_accelerator/tools/ds_migration_dialog.gd"
const DIALOG_SCENE_PATH := "res://addons/directstorage_accelerator/tools/ds_migration_dialog.tscn"


func test_restore_safety_guarantees() -> void:
	var script := load(DIALOG_SCRIPT_PATH) as GDScript
	assert_not_null(script, "ds_migration_dialog.gd loads")
	var source := script.source_code

	var confirm := _strip_comments(_function_body(source, "_on_confirm_dialog_confirmed"))
	assert_false(confirm.is_empty(), "confirmation handler exists")
	assert_true("CONFIRM_RESTART" in confirm)
	assert_true("hide()" in confirm)
	assert_true("_request_editor_restart.call_deferred" in confirm)
	assert_false("restart_editor" in confirm)

	var restart := _strip_comments(_function_body(source, "_request_editor_restart"))
	assert_true("EditorInterface.restart_editor(true)" in restart)
	assert_true("is_inside_tree()" in restart)
	assert_true("is_scanning()" in restart)
	assert_true("is_importing()" in restart)
	assert_true("RESTART_SETTLE_TIMEOUT_MS" in restart)

	var canceled := _strip_comments(_function_body(source, "_on_confirm_dialog_canceled"))
	assert_true("CONFIRM_RESTART" in canceled)
	assert_true("_reimport_after_skipped_restart" in canceled)

	var cancel_import := _strip_comments(_function_body(source, "_reimport_after_skipped_restart"))
	assert_true("reimport_files" in cancel_import)
	assert_true("is_importing()" in cancel_import)
	assert_true("running the game is safe now" in cancel_import)
	assert_true("do not run the game" in cancel_import)

	var ready_body := _strip_comments(_function_body(source, "_ready"))
	assert_true("_confirm_dialog.canceled.connect" in ready_body)
	assert_true("_restored_sources" in _strip_comments(_function_body(source, "_begin_restore")))

	var restore := _strip_comments(_function_body(source, "_begin_restore"))
	assert_false(restore.is_empty(), "restore handler exists")
	assert_true("is_scanning()" in restore)
	assert_true("is_importing()" in restore)
	assert_true("_is_busy" in restore)
	assert_true("CORE.restore_from_zip" in restore)
	assert_false("reimport_files" in restore)
	assert_false("fs.scan" in restore)
	assert_false("take_over_path" in restore)
	assert_true("_prompt_restart" in restore)

	var prompt := _strip_comments(_function_body(source, "_prompt_restart"))
	assert_true("_pending_action = CONFIRM_RESTART" in prompt)
	assert_true("_confirm_dialog.popup_centered" in prompt)
	assert_true("Restart the editor now?" in prompt)
	assert_true("If the editor does not come back" in prompt)
	assert_true("cancel_button_text = RESTART_CANCEL_TEXT" in prompt)

	assert_false("RESTORE_SCAN_TIMEOUT_MS" in source)
	assert_false("_finish_restore_after_scan" in source)
	assert_false("_wait_for_filesystem_idle" in source)

	var migrate := _strip_comments(_function_body(source, "_on_migrate_pressed"))
	assert_true("reimport_files" in migrate)
	assert_true("BATCH_SIZE" in migrate)

	assert_not_null(load(DIALOG_SCENE_PATH), "dialog scene parses")


static func _function_body(source: String, func_name: String) -> String:
	var start := source.find("func %s(" % func_name)
	if start < 0:
		return ""
	var next := source.find("\nfunc ", start + 1)
	if next < 0:
		return source.substr(start)
	return source.substr(start, next - start)


static func _strip_comments(text: String) -> String:
	var out := PackedStringArray()
	for line in text.split("\n"):
		var hash_idx := line.find("#")
		if hash_idx >= 0:
			line = line.substr(0, hash_idx)
		out.append(line)
	return "\n".join(out)
