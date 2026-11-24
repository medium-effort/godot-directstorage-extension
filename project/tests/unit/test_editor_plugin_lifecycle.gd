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
	assert_true("func _enable_plugin" in source_code)
	assert_true("_should_prompt_runtime_install_on_enable" in _function_body(source_code, "_enable_plugin"))
	assert_true("_on_runtime_menu.call_deferred()" in _function_body(source_code, "_enable_plugin"))


class _FakeDSContext:
	var initialized := true
	var supported := false
	var runtime_version := "Unavailable"

	func is_initialized() -> bool:
		return initialized

	func is_supported() -> bool:
		return supported

	func get_runtime_info() -> Dictionary:
		return {"version": runtime_version}


func test_enable_plugin_runtime_prompt_guard() -> void:
	var plugin_script := load(PLUGIN_SCRIPT_PATH) as GDScript
	assert_not_null(plugin_script, "ds_plugin.gd loads")

	var ctx := _FakeDSContext.new()
	assert_true(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Windows", "windows"),
		"prompts on Windows when runtime is Unavailable and no pending swap exists"
	)
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Linux", "x11"),
		"never prompts on non-Windows"
	)
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Windows", "headless"),
		"never prompts in headless mode"
	)
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "1.3.0", "Windows", "windows"),
		"skips prompt when a runtime install is already pending"
	)
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(null, "", "Windows", "windows"),
		"skips prompt when DSContext is missing"
	)

	ctx.initialized = false
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Windows", "windows"),
		"skips prompt when DSContext is uninitialized"
	)

	ctx.initialized = true
	ctx.supported = true
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Windows", "windows"),
		"skips prompt when DirectStorage is already supported"
	)

	ctx.supported = false
	ctx.runtime_version = "1.3.2506.2501"
	assert_false(
		plugin_script._is_runtime_missing_for_prompt(ctx, "", "Windows", "windows"),
		"skips prompt when runtime DLLs are already present on disk"
	)


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
