# SPDX-FileCopyrightText: 2026 MediumEffort
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

@tool
extends Window

## Editor front end for the DSPkg pack scan, opened from
## Project > Tools > Verify exported pack (DSPkg).
##
## The scan engine is dspkg_pack_scan_core.gd - the same one the command-line
## scanner uses - so the verdict line shown here is byte-for-byte the line the
## terminal prints. The pack is read a slice per frame instead of in one
## blocking call, which keeps the editor responsive and lets the user abandon a
## multi-gigabyte scan.
##
## This dialog only reads the chosen file. It never exports, deletes, repairs or
## mounts anything, and a `status=ok` here is only half of the release proof:
## the other half is one `DSPKG_BOOT status=ok mode=protected` line from the
## exported player.

const ScanCore := preload("uid://cgmf75ap4elct")

## Bytes read per frame: small enough that a frame stays cheap, large enough
## that a normal pack finishes in seconds.
const SCAN_STEP_BYTES := 262144

var _pack_path: String = ""
var _verdict_line: String = ""
## Untyped on purpose: the core is a RefCounted script instance and its methods
## are resolved at call time, not through a class_name in the global cache.
var _core = null

@onready var _background: Panel = $Background
@onready var _layout: VBoxContainer = $Layout
@onready var _file_dialog: FileDialog = $PackFileDialog
@onready var _path_label: Label = %PathLabel
@onready var _pick_button: Button = %PickButton
@onready var _scan_button: Button = %ScanButton
@onready var _copy_button: Button = %CopyButton
@onready var _scan_progress: ProgressBar = %ScanProgress
@onready var _verdict_label: Label = %VerdictLabel
@onready var _encryption_label: Label = %EncryptionLabel


func _ready() -> void:
	_apply_editor_theme()

	close_requested.connect(_on_close_requested)
	_pick_button.pressed.connect(_on_pick_pressed)
	_scan_button.pressed.connect(_on_scan_pressed)
	_copy_button.pressed.connect(_on_copy_pressed)
	_file_dialog.file_selected.connect(_on_file_selected)

	_scan_button.disabled = true
	_copy_button.disabled = true
	_scan_progress.visible = false
	_verdict_label.text = ""
	_encryption_label.text = ""
	set_process(false)


## Follows the editor theme and UI scale so the window looks like a native
## editor dialog instead of a default-themed popup. The scene carries no
## hard-coded colours or separations for exactly this reason: these assignments
## cover dark, light and custom editor themes. Outside the editor the lookup is
## unavailable, and the window falls back to the project theme.
##
## The theme goes on the window and on the root Control. A theme set on the
## window does reach descendant Controls (verified), but assigning it twice costs
## nothing and keeps the guarantee local to the node that owns the layout.
func _apply_editor_theme() -> void:
	if not Engine.is_editor_hint():
		return

	_apply_editor_theme_look()

	var scale := EditorInterface.get_editor_scale()
	if is_equal_approx(scale, 1.0):
		return
	# content_scale_factor enlarges the content without resizing the window, so
	# the window has to grow by the same factor or the layout gets clipped.
	content_scale_factor = scale
	min_size = Vector2i(Vector2(min_size) * scale)
	size = Vector2i(Vector2(size) * scale)


## The theme half of _apply_editor_theme(), kept separate from the UI-scale
## resize so that re-applying it after a save cannot multiply size/min_size
## twice. The window/Control split is described next to _apply_editor_theme().
func _apply_editor_theme_look() -> void:
	var editor_theme: Theme = EditorInterface.get_editor_theme()
	theme = editor_theme
	_layout.theme = editor_theme
	_file_dialog.theme = editor_theme

	# A bare Window has no "panel" theme item (Window binds only its embedded
	# border), so its body would fall back to the default theme while the controls
	# inside are already editor-themed. AcceptDialog solves this with an internal
	# Panel that overrides "panel" with the editor's dialog_style; do the same
	# here so the body matches the editor's own dialogs.
	var dialog_style: StyleBox = editor_theme.get_stylebox("panel", "AcceptDialog")
	if dialog_style != null:
		_background.add_theme_stylebox_override("panel", dialog_style)
		_layout.offset_left = dialog_style.get_margin(SIDE_LEFT)
		_layout.offset_top = dialog_style.get_margin(SIDE_TOP)
		_layout.offset_right = -dialog_style.get_margin(SIDE_RIGHT)
		_layout.offset_bottom = -dialog_style.get_margin(SIDE_BOTTOM)


## The editor theme is generated at runtime and has no resource path, so while it
## is assigned to a node the editor has to inline it as a [sub_resource] when it
## packs the scene: ~1,045 SVG icons plus 29 embedded font binaries and their
## MSDF atlases, i.e. ~6.4 MB of theme data on a ~100-line dialog. Clearing the
## themed properties around saves keeps that out of the .tscn permanently, so
## opening this scene for an unrelated edit cannot re-embed it.
func _notification(what: int) -> void:
	if not Engine.is_editor_hint() or not is_node_ready():
		return

	if what == NOTIFICATION_EDITOR_PRE_SAVE:
		_strip_editor_theme()
	elif what == NOTIFICATION_EDITOR_POST_SAVE:
		_apply_editor_theme_look()


## Inverse of _apply_editor_theme_look(). The layout offsets set there are left
## alone on purpose: they are plain floats matching the panel margins, so they
## cost nothing in the scene file and keep the body inset before _ready() runs.
func _strip_editor_theme() -> void:
	theme = null
	_layout.theme = null
	_file_dialog.theme = null
	_background.remove_theme_stylebox_override("panel")


func _process(_delta: float) -> void:
	if _core == null:
		set_process(false)
		return

	var progress: Dictionary = _core.step(SCAN_STEP_BYTES)
	var total: int = maxi(1, int(progress["size"]))
	_scan_progress.value = float(int(progress["scanned"])) / float(total) * 100.0
	if progress["done"]:
		_finish_scan()


func _on_pick_pressed() -> void:
	_file_dialog.popup_centered()


func _on_file_selected(path: String) -> void:
	_pack_path = path
	_path_label.text = path
	_verdict_line = ""
	_verdict_label.text = ""
	_encryption_label.text = ""
	_copy_button.disabled = true
	_scan_button.disabled = false


func _on_scan_pressed() -> void:
	if _pack_path.is_empty():
		return

	_verdict_line = ""
	_verdict_label.text = ""
	_encryption_label.text = ""
	_copy_button.disabled = true
	_scan_button.disabled = true
	_pick_button.disabled = true
	_scan_progress.value = 0.0
	_scan_progress.visible = true

	_core = ScanCore.new()
	var opened: Dictionary = _core.start(_pack_path)
	if not opened["ok"]:
		_finish_scan()
		return
	set_process(true)


func _finish_scan() -> void:
	set_process(false)
	_scan_progress.visible = false
	_scan_progress.value = 0.0
	_pick_button.disabled = false
	_scan_button.disabled = _pack_path.is_empty()

	var verdict: Dictionary = _core.get_verdict()
	_verdict_line = _core.format_verdict(_pack_path)
	_encryption_label.text = _format_encryption(_core.get_directory_encryption())
	_core.close()
	_core = null

	var display := _verdict_line
	if not verdict["ok"]:
		display += "\n" + String(verdict["detail"])
	_verdict_label.text = display
	_copy_button.disabled = false


func _on_copy_pressed() -> void:
	if not _verdict_line.is_empty():
		DisplayServer.clipboard_set(_verdict_line)


## The machine line deliberately carries no encryption field: the gate's regular
## expressions and the documented grammar expect `entries=... file=...`. The
## directory bit is reported here instead, where a human reads it.
func _format_encryption(encryption: Dictionary) -> String:
	if not encryption["known"]:
		return "Directory encrypted: unknown (the pack header could not be read)"
	if encryption["encrypted"]:
		return "Directory encrypted: yes"
	return "Directory encrypted: no"


func _on_close_requested() -> void:
	set_process(false)
	queue_free()
