extends GutTest

const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const DIALOG_SCRIPT_PATH := "res://addons/directstorage_accelerator/tools/runtime_manager_dialog.gd"
const DIALOG_SCENE_PATH := "res://addons/directstorage_accelerator/tools/runtime_manager_dialog.tscn"
const SETTING_KEY := "dsgd_extension/show_preview_runtimes"
const STABLE := "1.3.0"
const PREVIEW := "1.4.0-preview2-2606.904"
const FUTURE := "1.10.0"

var _script: GDScript = null
var _had_setting := false
var _prev_setting: Variant = null


func before_all() -> void:
	_script = load(DIALOG_SCRIPT_PATH) as GDScript


func before_each() -> void:
	_had_setting = ProjectSettings.has_setting(SETTING_KEY)
	_prev_setting = ProjectSettings.get_setting(SETTING_KEY) if _had_setting else null


func after_each() -> void:
	Helpers.restore_setting(SETTING_KEY, _had_setting, _prev_setting)


func test_baseline_preview_gating() -> void:
	assert_not_null(_script, "runtime_manager_dialog.gd loads")
	assert_true(bool(_script.callv("_should_show_version", [STABLE, false, PackedStringArray()])), "stable is listed without opt-in")
	assert_false(bool(_script.callv("_should_show_version", [PREVIEW, false, PackedStringArray()])), "preview is hidden without opt-in")
	assert_true(bool(_script.callv("_should_show_version", [PREVIEW, true, PackedStringArray()])), "preview is listed with opt-in")


func test_pinned_versions_survive_off_opt_in() -> void:
	for key in ["active", "pending"]:
		var state := {"active": "", "pending": "", "cached": PackedStringArray()}
		state[key] = PREVIEW
		var pinned: PackedStringArray = _script.callv("_pinned_versions", [state])
		assert_true(pinned.has(PREVIEW), "%s preview is pinned" % key)
		assert_true(bool(_script.callv("_should_show_version", [PREVIEW, false, pinned])), "%s preview stays listed" % key)

	var cached_state := {"active": "", "pending": "", "cached": PackedStringArray([PREVIEW])}
	var cached_pinned: PackedStringArray = _script.callv("_pinned_versions", [cached_state])
	assert_true(bool(_script.callv("_should_show_version", [PREVIEW, false, cached_pinned])), "cached preview stays listed")

	var unrelated: PackedStringArray = _script.callv("_pinned_versions", [{"active": PREVIEW, "pending": "", "cached": PackedStringArray()}])
	assert_false(bool(_script.callv("_should_show_version", [FUTURE, false, unrelated])), "pinning one preview does not reveal others")


func test_numeric_version_sorting() -> void:
	var future_key := String(_script.callv("_version_sort_key", [FUTURE]))
	var stable_key := String(_script.callv("_version_sort_key", [STABLE]))
	assert_true(future_key > stable_key, "1.10.0 sorts above 1.3.0")


func test_dialog_scene_and_wiring() -> void:
	assert_not_null(load(DIALOG_SCENE_PATH), "dialog scene parses")
	var scene_text := FileAccess.get_file_as_string(DIALOG_SCENE_PATH)
	var check_block := _node_block(scene_text, "ShowPreviewCheck")
	assert_false(check_block.is_empty(), "dialog scene has ShowPreviewCheck node")
	assert_true("type=\"CheckBox\"" in check_block, "ShowPreviewCheck is a CheckBox")
	assert_true("unique_name_in_owner = true" in check_block, "ShowPreviewCheck is scene-unique")

	var source := _script.source_code
	assert_true(SETTING_KEY in source, "dialog reads persisted setting")
	assert_false("OS.get_environment" in source, "dialog no longer reads environment variable")
	assert_true("close_requested.connect(hide)" in source, "title-bar close button hides dialog")


func test_setting_persistence_roundtrip() -> void:
	ProjectSettings.set_setting(SETTING_KEY, true)
	assert_true(bool(ProjectSettings.get_setting(SETTING_KEY, false)))
	ProjectSettings.set_setting(SETTING_KEY, false)
	assert_false(bool(ProjectSettings.get_setting(SETTING_KEY, true)))


static func _node_block(scene_text: String, node_name: String) -> String:
	var header := "[node name=\"%s\"" % node_name
	var start := scene_text.find(header)
	if start < 0:
		return ""
	var next := scene_text.find("\n[node ", start + 1)
	if next < 0:
		return scene_text.substr(start)
	return scene_text.substr(start, next - start)
