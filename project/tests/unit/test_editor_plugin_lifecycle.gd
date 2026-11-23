extends GutTest

const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const AUTOLOAD_NAME := "DSRuntime"
const PLUGIN_SCRIPT_PATH := "res://addons/directstorage_accelerator/scripts/ds_plugin.gd"
const EXPORT_PLUGIN_PATH := "res://addons/directstorage_accelerator/scripts/ds_export_plugin.gd"

var _had_autoload := false
var _prev_autoload: Variant = null


func before_each() -> void:
	var key := "autoload/" + AUTOLOAD_NAME
	_had_autoload = ProjectSettings.has_setting(key)
	_prev_autoload = ProjectSettings.get_setting(key) if _had_autoload else null


func after_each() -> void:
	Helpers.restore_setting("autoload/" + AUTOLOAD_NAME, _had_autoload, _prev_autoload)


func test_export_plugin_parse_isolation() -> void:
	var export_script := load(EXPORT_PLUGIN_PATH) as GDScript
	assert_not_null(export_script, "ds_export_plugin.gd loads")
	assert_false("DSPkg." in export_script.source_code, "no direct DSPkg call at parse time")


func test_plugin_source_lifecycle_and_reload_wiring() -> void:
	var script_resource := load(PLUGIN_SCRIPT_PATH) as GDScript
	assert_not_null(script_resource, "ds_plugin.gd loads")
	var source_code := script_resource.source_code

	assert_true("func _disable_plugin" in source_code)
	assert_false("_sync_autoload(AUTOLOAD_NAME, \"\")" in _function_body(source_code, "_exit_tree"))
	assert_true("add_tool_submenu_item" in source_code)
	assert_true("remove_tool_menu_item" in source_code)
	assert_true(FileAccess.file_exists("res://addons/directstorage_accelerator/tools/dspkg_pack_scan_dialog.tscn"))

	assert_true("func _sync_native_reload_signals" in source_code)
	assert_true("_sync_native_reload_signals(true)" in _function_body(source_code, "_enter_tree"))
	assert_true("_sync_native_reload_signals(false)" in _function_body(source_code, "_exit_tree"))
	assert_true("\"extension_loaded\"" in source_code)
	assert_true("\"extensions_reloaded\"" in source_code)
	assert_true("ClassDB.class_exists(\"DSContext\")" in _function_body(source_code, "_reinitialize_native_backend_after_reload"))
	assert_true("call_deferred" in _function_body(source_code, "_queue_native_reinit"))


func test_autoload_project_settings_contract() -> void:
	var key := "autoload/" + AUTOLOAD_NAME
	var registered_path := "*res://addons/directstorage_accelerator/scripts/ds_runtime.gd"
	ProjectSettings.set_setting(key, registered_path)
	assert_true(ProjectSettings.has_setting(key))
	ProjectSettings.set_setting(key, null)
	assert_false(ProjectSettings.has_setting(key))
	ProjectSettings.set_setting(key, registered_path)
	assert_true(ProjectSettings.has_setting(key))


static func _function_body(source: String, func_name: String) -> String:
	var marker := "\nfunc " + func_name
	var start := source.find(marker)
	if start < 0:
		return ""
	var rest := source.substr(start + 1)
	var next_func := rest.find("\nfunc ", 1)
	if next_func < 0:
		return rest
	return rest.substr(0, next_func)
