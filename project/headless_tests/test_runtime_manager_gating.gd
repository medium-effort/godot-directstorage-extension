@tool
extends SceneTree

## Runtime Manager version-gating test.
##
## Author: MediumEffort
## Description: Verifies the Manage DirectStorage Runtime dropdown policy: the
## baseline is always listed, a newer preview is listed only with the persisted
## opt-in, and a version the project already has (active, staged or downloaded)
## stays listed either way. The policy lives in static helpers, so this runs
## without an editor instance, a DirectStorage runtime or a RenderingDevice.
## The wiring checks cover the checkbox node, the ProjectSettings key and the
## absence of the old environment-variable gate.

const DIALOG_SCRIPT_PATH = "res://addons/directstorage_accelerator/tools/runtime_manager_dialog.gd"
const DIALOG_SCENE_PATH = "res://addons/directstorage_accelerator/tools/runtime_manager_dialog.tscn"
const SETTING_KEY = "dsgd_extension/show_preview_runtimes"
const STABLE = "1.3.0"
const PREVIEW = "1.4.0-preview2-2606.904"
const FUTURE = "1.10.0"

var _failures: int = 0


func _init() -> void:
	print("[Test Runtime Gating] Starting...")

	var script: GDScript = load(DIALOG_SCRIPT_PATH)
	if script == null:
		_fail("could not load " + DIALOG_SCRIPT_PATH)
		_finish()
		return

	# Baseline policy: stable always listed, preview only on opt-in.
	_check(bool(script.callv("_should_show_version", [STABLE, false, PackedStringArray()])), "stable is listed without the opt-in")
	_check(not bool(script.callv("_should_show_version", [PREVIEW, false, PackedStringArray()])), "preview is hidden without the opt-in")
	_check(bool(script.callv("_should_show_version", [PREVIEW, true, PackedStringArray()])), "preview is listed with the opt-in")

	# Pinned versions survive an off opt-in: active, pending and cached.
	for key in ["active", "pending"]:
		var state := {"active": "", "pending": "", "cached": PackedStringArray()}
		state[key] = PREVIEW
		var pinned: PackedStringArray = script.callv("_pinned_versions", [state])
		_check(pinned.has(PREVIEW), "%s preview is pinned" % key)
		_check(bool(script.callv("_should_show_version", [PREVIEW, false, pinned])), "%s preview stays listed with the opt-in off" % key)
	var cached_state := {"active": "", "pending": "", "cached": PackedStringArray([PREVIEW])}
	var cached_pinned: PackedStringArray = script.callv("_pinned_versions", [cached_state])
	_check(bool(script.callv("_should_show_version", [PREVIEW, false, cached_pinned])), "cached preview stays listed with the opt-in off")

	# Pinning one version must not reveal a different hidden one.
	var unrelated: PackedStringArray = script.callv("_pinned_versions", [{"active": PREVIEW, "pending": "", "cached": PackedStringArray()}])
	_check(not bool(script.callv("_should_show_version", [FUTURE, false, unrelated])), "pinning one preview does not reveal others")

	# Numeric ordering: 1.10 sorts after 1.3 (plain string order inverts it).
	var future_key := String(script.callv("_version_sort_key", [FUTURE]))
	var stable_key := String(script.callv("_version_sort_key", [STABLE]))
	_check(future_key > stable_key, "1.10.0 sorts above 1.3.0")

	# Wiring: the checkbox exists in the scene and the setting is the only gate.
	_check(load(DIALOG_SCENE_PATH) != null, "dialog scene parses")
	var scene_text := FileAccess.get_file_as_string(DIALOG_SCENE_PATH)
	var check_block := _node_block(scene_text, "ShowPreviewCheck")
	_check(not check_block.is_empty(), "dialog scene has the ShowPreviewCheck node")
	_check("type=\"CheckBox\"" in check_block, "ShowPreviewCheck is a CheckBox")
	_check("unique_name_in_owner = true" in check_block, "ShowPreviewCheck is a scene-unique node")
	var source := script.source_code
	_check(SETTING_KEY in source, "dialog reads the persisted setting")
	_check(not "OS.get_environment" in source, "dialog no longer reads an environment variable")
	# A Window only emits close_requested; without a handler the title-bar X is
	# inert while the bottom Close button works.
	_check("close_requested.connect(hide)" in source, "title-bar close button hides the dialog")

	# Persistence round-trip: the key survives set/get, then the prior value is
	# restored so a later suite sees the project file as it found it.
	var had := ProjectSettings.has_setting(SETTING_KEY)
	var previous = ProjectSettings.get_setting(SETTING_KEY) if had else null
	ProjectSettings.set_setting(SETTING_KEY, true)
	_check(bool(ProjectSettings.get_setting(SETTING_KEY, false)), "setting reads back true")
	ProjectSettings.set_setting(SETTING_KEY, false)
	_check(not bool(ProjectSettings.get_setting(SETTING_KEY, true)), "setting reads back false")
	if had:
		ProjectSettings.set_setting(SETTING_KEY, previous)
	else:
		ProjectSettings.set_setting(SETTING_KEY, null)

	_finish()


## Returns the authored block of [param node_name] in a .tscn, up to the next
## node declaration, or an empty string when the node is absent.
static func _node_block(scene_text: String, node_name: String) -> String:
	var header := "[node name=\"%s\"" % node_name
	var start := scene_text.find(header)
	if start < 0:
		return ""
	var next := scene_text.find("\n[node ", start + 1)
	if next < 0:
		return scene_text.substr(start)
	return scene_text.substr(start, next - start)


func _check(condition: bool, what: String) -> void:
	if condition:
		print("[Test Runtime Gating] PASS: " + what)
	else:
		_fail(what)


func _fail(what: String) -> void:
	_failures += 1
	push_error("[Test Runtime Gating] FAIL: " + what)


func _finish() -> void:
	if _failures > 0:
		print("[Test Runtime Gating] FAILED (%d)" % _failures)
		quit(1)
		return
	print("[Test Runtime Gating] SUCCESS")
	quit(0)
