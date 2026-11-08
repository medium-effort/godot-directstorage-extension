@tool
extends Window

## Minimal runtime manager: pick a published DirectStorage version, install it
## (verified against Microsoft's SHA-512), then restart the editor so the
## staged DLLs are applied before the first DirectStorage call.
##
## The same dialog offers the D3D12 renderer switch: DirectStorage needs a
## D3D12 RenderingDevice, so a project running Vulkan or OpenGL cannot use it
## even with the runtime installed.
##
## Full version management (pre-switch archive scan, EULA display, rollback
## list) belongs to the later installer GUI; this dialog only exposes the
## primitives the installer service already guarantees.

const INSTALLER_SCRIPT = preload("res://addons/directstorage_accelerator/scripts/ds_runtime_installer.gd")

const DRIVER_SETTING: String = "rendering/rendering_device/driver.windows"
const METHOD_SETTING: String = "rendering/renderer/rendering_method"
const LICENSE_DIALOG_SCENE_PATH: String = "res://addons/directstorage_accelerator/tools/runtime_license_dialog.tscn"

## Runtimes newer than this baseline are hidden from the dropdown unless the
## developer opts in. This is a product policy, not a license check: the
## installer still accepts any version its package table knows about.
const BASELINE_MAJOR: int = 1
const BASELINE_MINOR: int = 3
const ALLOW_ABOVE_BASELINE_ENV: String = "DSGD_ALLOW_DOWNLOAD_RUNTIME_ABOVE_1_POINT_3"

@onready var _version_option: OptionButton = %VersionOption
@onready var _active_label: Label = %ActiveLabel
@onready var _status_label: Label = %StatusLabel
@onready var _install_button: Button = %InstallButton
@onready var _restart_button: Button = %RestartButton
@onready var _d3d12_button: Button = %D3D12Button
@onready var _close_button: Button = %CloseButton

var _installer: Node = null
var _license_dialog: Window = null
## Package entries filtered out of the dropdown because they are newer than the
## baseline and the developer has not opted in. Drives the status hint.
var _gated_hidden: int = 0


func _ready() -> void:
	_installer = INSTALLER_SCRIPT.new()
	add_child(_installer)
	_installer.install_finished.connect(_on_install_finished)
	_installer.license_required.connect(_on_license_required)
	_version_option.item_selected.connect(_on_version_selected)
	_install_button.pressed.connect(_on_install_pressed)
	_restart_button.pressed.connect(_on_restart_pressed)
	if is_instance_valid(_d3d12_button):
		_d3d12_button.pressed.connect(_on_d3d12_pressed)
	_close_button.pressed.connect(hide)
	_populate_versions()
	_refresh()


func open() -> void:
	# A Window root becomes ready on the next frame after add_child(), so an
	# immediate open() would refresh before @onready bindings exist.
	if not is_node_ready():
		await ready
	_refresh()
	popup_centered()


## Rebuilds the version list from the installer's package table. The scene's
## authored items are only the design-time default; the real list is conditional
## (entries newer than the baseline are opt-in), so it is regenerated here.
func _populate_versions() -> void:
	_version_option.clear()
	_gated_hidden = 0
	var allow_above := _allow_above_baseline()
	var versions: Dictionary = INSTALLER_SCRIPT.get_versions()
	var keys: Array = versions.keys()
	keys.sort_custom(func(a, b): return _version_sort_key(String(a)) < _version_sort_key(String(b)))
	for key in keys:
		var version := String(key)
		if _is_above_baseline(version) and not allow_above:
			_gated_hidden += 1
			continue
		var meta: Dictionary = versions[key]
		var label := version + (" (preview)" if bool(meta.get("preview", false)) else " (stable)")
		_version_option.add_item(label)
		_version_option.set_item_metadata(_version_option.item_count - 1, version)
	if _version_option.item_count == 0:
		# Only reachable when the manifest is empty and the fallback was stripped.
		_version_option.add_item("No runtime available")
		_version_option.set_item_metadata(0, "")
		_version_option.disabled = true


static func _allow_above_baseline() -> bool:
	var raw := OS.get_environment(ALLOW_ABOVE_BASELINE_ENV).strip_edges().to_lower()
	return raw in ["1", "true", "yes", "on"]


## Parses the numeric release prefix of a package key. Pre-release/build suffixes
## ("-preview2-2606.904") are ignored and a missing component reads as 0, so
## "1.4" and "1.4.0" compare equal.
static func _version_components(version: String) -> PackedInt32Array:
	var core := version
	var dash := core.find("-")
	if dash >= 0:
		core = core.substr(0, dash)
	var parts := core.split(".")
	var components := PackedInt32Array([0, 0, 0])
	for i in mini(parts.size(), 3):
		components[i] = int(parts[i])
	return components


## Numeric, zero-padded sort key: string comparison alone puts "1.10.0" below
## "1.3.0", which is the wrong order the day a 1.10 runtime ships.
static func _version_sort_key(version: String) -> String:
	var components := _version_components(version)
	return "%08d.%08d.%08d.%s" % [components[0], components[1], components[2], version]


static func _is_above_baseline(version: String) -> bool:
	var components := _version_components(version)
	return components[0] > BASELINE_MAJOR or (components[0] == BASELINE_MAJOR and components[1] > BASELINE_MINOR)


func _refresh() -> void:
	var state: Dictionary = INSTALLER_SCRIPT.get_state()
	var active := String(state.get("active", ""))
	_active_label.text = "Active runtime: %s" % (active if not active.is_empty() else "not installed")
	if _version_option.selected < 0 and _version_option.item_count > 0:
		_version_option.select(0)

	var pending := String(state.get("pending", ""))
	if pending.is_empty():
		_status_label.text = "Select a version, then install. Microsoft's SHA-512 is verified before extraction."
	else:
		_status_label.text = "DirectStorage %s is staged. Select it, then restart to apply it." % pending

	var driver := RenderingServer.get_current_rendering_driver_name()
	if is_instance_valid(_d3d12_button):
		_d3d12_button.visible = driver != "d3d12"
		if _d3d12_button.visible:
			_status_label.text += "\nCurrent renderer: %s. DirectStorage requires Direct3D 12." % driver

	_update_actions(state)

	if _gated_hidden > 0:
		_status_label.text += "\n%d runtime version(s) newer than %d.%d are hidden. Set %s=1 and restart the editor to show them." % [_gated_hidden, BASELINE_MAJOR, BASELINE_MINOR, ALLOW_ABOVE_BASELINE_ENV]


func _update_actions(state: Dictionary) -> void:
	var version := _selected_version()
	var active := String(state.get("active", ""))
	var pending := String(state.get("pending", ""))
	var cached: PackedStringArray = state.get("cached", PackedStringArray())
	var downloaded := cached.has(version)

	# A version that is already installed or cached has nothing to install, so
	# the same action becomes an explicit reinstall.
	_install_button.text = "Reinstall" if (version == active or downloaded) else "Install"
	_install_button.disabled = false

	# The restart action follows the selection, not just the global pending
	# state: selecting the active version must not offer to apply a different
	# staged version behind the user's back.
	if not pending.is_empty() and version == pending:
		_restart_button.text = "Restart editor to apply %s" % version
		_restart_button.disabled = false
	elif version == active:
		_restart_button.text = "Already the currently active runtime"
		_restart_button.disabled = true
	elif downloaded:
		# Already on disk: switch to it without downloading again. If another
		# version was staged, this replaces the stage with the selected one.
		_restart_button.text = "Switch to %s and restart" % version
		_restart_button.disabled = false
	else:
		_restart_button.text = "Install first to apply"
		_restart_button.disabled = true


func _selected_version() -> String:
	var index := _version_option.selected
	if index < 0 or index >= _version_option.item_count:
		return ""
	return String(_version_option.get_item_metadata(index))


func _on_version_selected(_index: int) -> void:
	_update_actions(INSTALLER_SCRIPT.get_state())


func _on_install_pressed() -> void:
	var version := _selected_version()
	if version.is_empty():
		return
	_install_button.disabled = true
	_status_label.text = "Downloading Microsoft.Direct3D.DirectStorage %s..." % version
	_installer.install(version)


func _on_install_finished(_version: String, error: int, message: String) -> void:
	_close_license_dialog()
	_install_button.disabled = false
	_refresh()
	call_deferred("_raise_manager")
	if error != OK:
		# A declined license is a user choice, not an install failure.
		_status_label.text = message if error == ERR_UNAUTHORIZED else "Install failed: " + message


func _on_license_required(version: String, license_text: String) -> void:
	_close_license_dialog()
	var dialog_scene := ResourceLoader.load(LICENSE_DIALOG_SCENE_PATH, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	if dialog_scene == null:
		_installer.decline_license()
		_status_label.text = "License dialog is missing; the install was cancelled."
		return
	_license_dialog = dialog_scene.instantiate() as Window
	_license_dialog.connect("accepted", _on_license_accepted)
	_license_dialog.connect("declined", _on_license_declined)
	var base := EditorInterface.get_base_control()
	if base != null:
		base.add_child(_license_dialog)
	else:
		add_child(_license_dialog)
	_license_dialog.call("show_license", version, license_text)


func _on_license_accepted() -> void:
	_close_license_dialog()
	_installer.accept_license()
	call_deferred("_raise_manager")


func _on_license_declined() -> void:
	_close_license_dialog()
	_installer.decline_license()
	call_deferred("_raise_manager")


func _close_license_dialog() -> void:
	if is_instance_valid(_license_dialog):
		# Hide immediately so the transient/exclusive state is cleared before the
		# deferred free; otherwise the manager window can stay behind or hidden.
		_license_dialog.hide()
		_license_dialog.queue_free()
		_license_dialog = null


## Bring the manager back after the license window closes.
func _raise_manager() -> void:
	if not is_inside_tree():
		return
	# Unconditional: if the editor hid the window (visible == false) this shows
	# it again, and if it is merely behind, popup_centered() raises it.
	popup_centered()
	grab_focus()


func _exit_tree() -> void:
	_close_license_dialog()


func _on_restart_pressed() -> void:
	var state: Dictionary = INSTALLER_SCRIPT.get_state()
	var version := _selected_version()
	var active := String(state.get("active", ""))
	var pending := String(state.get("pending", ""))
	var cached: PackedStringArray = state.get("cached", PackedStringArray())
	if version == pending:
		# The selected version is the staged one; restart applies it.
		EditorInterface.restart_editor()
		return
	if cached.has(version) and version != active:
		# Switching to a cached version replaces any other staged version.
		var error := INSTALLER_SCRIPT.activate_cached(version)
		if error != OK:
			_status_label.text = "Could not stage %s: %s" % [version, error_string(error)]
			return
		EditorInterface.restart_editor()


func _on_d3d12_pressed() -> void:
	# DirectStorage needs a D3D12 RenderingDevice. Persist the project settings
	# first; the restart then reopens the editor with the D3D12 driver.
	ProjectSettings.set_setting(DRIVER_SETTING, "d3d12")
	if String(ProjectSettings.get_setting(METHOD_SETTING, "forward_plus")) == "gl_compatibility":
		ProjectSettings.set_setting(METHOD_SETTING, "forward_plus")
	var error := ProjectSettings.save()
	if error != OK:
		_status_label.text = "Could not save the D3D12 renderer setting: %s" % error_string(error)
		return
	EditorInterface.restart_editor()
