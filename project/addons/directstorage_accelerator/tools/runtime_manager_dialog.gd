@tool
extends Window

## Minimal runtime manager: pick a published DirectStorage version, install it
## (verified against Microsoft's SHA-512), then restart the editor so the
## staged DLLs are applied before the first DirectStorage call.
##
## Full version management (pre-switch archive scan, EULA display, rollback
## list) belongs to the later installer GUI; this dialog only exposes the
## primitives the installer service already guarantees.

const INSTALLER_SCRIPT = preload("res://addons/directstorage_accelerator/scripts/ds_runtime_installer.gd")

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
@onready var _close_button: Button = %CloseButton

var _installer: Node = null


func _ready() -> void:
	_installer = INSTALLER_SCRIPT.new()
	add_child(_installer)
	_installer.install_finished.connect(_on_install_finished)
	_install_button.pressed.connect(_on_install_pressed)
	_restart_button.pressed.connect(_on_restart_pressed)
	_close_button.pressed.connect(hide)
	_refresh()


func open() -> void:
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
	_restart_button.disabled = pending.is_empty()
	_install_button.disabled = false


func _on_install_pressed() -> void:
	var index := _version_option.selected
	if index < 0 or index >= UI_VERSIONS.size():
		return
	var version: String = UI_VERSIONS[index]
	_install_button.disabled = true
	_status_label.text = "Downloading Microsoft.Direct3D.DirectStorage %s..." % version
	_installer.install(version)


func _on_install_finished(_version: String, error: int, message: String) -> void:
	_install_button.disabled = false
	_refresh()
	if error != OK:
		_status_label.text = "Install failed: " + message


func _on_restart_pressed() -> void:
	EditorInterface.restart_editor()
