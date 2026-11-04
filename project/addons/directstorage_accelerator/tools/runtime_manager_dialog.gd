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

## Display order of the two published packages the addon currently supports.
const UI_VERSIONS := [
	"1.3.0",
	"1.4.0-preview2-2606.904",
]

@onready var _version_option: OptionButton = %VersionOption
@onready var _active_label: Label = %ActiveLabel
@onready var _status_label: Label = %StatusLabel
@onready var _install_button: Button = %InstallButton
@onready var _restart_button: Button = %RestartButton
@onready var _d3d12_button: Button = %D3D12Button
@onready var _close_button: Button = %CloseButton

var _installer: Node = null


func _ready() -> void:
	_installer = INSTALLER_SCRIPT.new()
	add_child(_installer)
	_installer.install_finished.connect(_on_install_finished)
	_version_option.item_selected.connect(_on_version_selected)
	_install_button.pressed.connect(_on_install_pressed)
	_restart_button.pressed.connect(_on_restart_pressed)
	if is_instance_valid(_d3d12_button):
		_d3d12_button.pressed.connect(_on_d3d12_pressed)
	_close_button.pressed.connect(hide)
	_refresh()


func open() -> void:
	# A Window root becomes ready on the next frame after add_child(), so an
	# immediate open() would refresh before @onready bindings exist.
	if not is_node_ready():
		await ready
	_refresh()
	popup_centered()


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
		_status_label.text = "DirectStorage %s is staged. Restart the editor to apply it." % pending

	var driver := RenderingServer.get_current_rendering_driver_name()
	if is_instance_valid(_d3d12_button):
		_d3d12_button.visible = driver != "d3d12"
		if _d3d12_button.visible:
			_status_label.text += "\nCurrent renderer: %s. DirectStorage requires Direct3D 12." % driver

	_update_actions(state)


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

	if not pending.is_empty():
		_restart_button.text = "Restart editor to apply"
		_restart_button.disabled = false
	elif downloaded and version != active:
		# Already on disk: switch to it without downloading again.
		_restart_button.text = "Switch to %s and restart" % version
		_restart_button.disabled = false
	else:
		_restart_button.disabled = true
		if version == active:
			_restart_button.text = "Already the currently active runtime"
		else:
			_restart_button.text = "Install first to apply"


func _selected_version() -> String:
	var index := _version_option.selected
	if index < 0 or index >= UI_VERSIONS.size():
		return ""
	return UI_VERSIONS[index]


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
	_install_button.disabled = false
	_refresh()
	if error != OK:
		_status_label.text = "Install failed: " + message


func _on_restart_pressed() -> void:
	# A cached version can be switched to without downloading it again; that is
	# the "restart before installing" path for an already-downloaded package.
	var state: Dictionary = INSTALLER_SCRIPT.get_state()
	var version := _selected_version()
	var active := String(state.get("active", ""))
	var cached: PackedStringArray = state.get("cached", PackedStringArray())
	if String(state.get("pending", "")).is_empty() and cached.has(version) and version != active:
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
