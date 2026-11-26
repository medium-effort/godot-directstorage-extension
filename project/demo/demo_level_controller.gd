extends Node3D

## Lightweight runtime controller attached to generated demo_level_*.scn scenes.
## Slowly orbits the gallery cubes and exposes a Return to Main Menu HUD button.

signal return_requested

var _hud_layer: CanvasLayer
var _info_label: Label
var _return_button: Button
var _mode_name: String = ""
var _elapsed_ms: float = 0.0


func configure_hud(mode_name: String, elapsed_ms: float) -> void:
	_mode_name = mode_name
	_elapsed_ms = elapsed_ms
	_refresh_hud_text()


func _ready() -> void:
	_build_hud()
	_refresh_hud_text()


func _process(delta: float) -> void:
	for child in get_children():
		if child is MeshInstance3D and child.name.begins_with("ArenaProp_"):
			(child as MeshInstance3D).rotate_y(delta * 0.45)


func _build_hud() -> void:
	_hud_layer = CanvasLayer.new()
	_hud_layer.name = "DemoHUD"
	add_child(_hud_layer)

	var panel := PanelContainer.new()
	panel.position = Vector2(16, 16)
	_hud_layer.add_child(panel)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 8)
	panel.add_child(vbox)

	_info_label = Label.new()
	vbox.add_child(_info_label)

	_return_button = Button.new()
	_return_button.text = "Return to Main Menu (Clear Caches)"
	_return_button.pressed.connect(func() -> void: return_requested.emit())
	vbox.add_child(_return_button)


func _refresh_hud_text() -> void:
	if _info_label == null:
		return
	var variant := String(get_meta("variant_label", "Demo Level"))
	_info_label.text = "%s | Mode: %s | Load: %.2f ms" % [variant, _mode_name, _elapsed_ms]
