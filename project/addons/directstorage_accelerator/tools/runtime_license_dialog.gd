@tool
extends Window

## Shows the exact Microsoft DirectStorage SDK license extracted from the
## verified package. The install continues only on accepted; closing the window
## or pressing Decline emits declined.

signal accepted
signal declined

@onready var _version_label: Label = %VersionLabel
@onready var _license_text: TextEdit = %LicenseText
@onready var _accept_button: Button = %AcceptButton
@onready var _decline_button: Button = %DeclineButton

var _decided: bool = false


func _ready() -> void:
	_accept_button.pressed.connect(_accept)
	_decline_button.pressed.connect(_decline)
	close_requested.connect(_decline)


func show_license(version: String, license_text: String) -> void:
	# The Window root becomes ready a frame after add_child().
	if not is_node_ready():
		await ready
	_version_label.text = "Microsoft DirectStorage SDK %s" % version
	_license_text.text = license_text
	_license_text.scroll_vertical = 0
	_decided = false
	popup_centered()


func _accept() -> void:
	if _decided:
		return
	_decided = true
	accepted.emit()
	hide()


func _decline() -> void:
	if _decided:
		return
	_decided = true
	declined.emit()
	hide()
