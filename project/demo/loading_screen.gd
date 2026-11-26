extends Control

## Loading-screen overlay displaying progress bar, phase status,
## live DSPreloader counters, and a Cancel button during async prewarm.

signal cancel_requested

@onready var _phase_label: Label = %PhaseLabel
@onready var _progress_bar: ProgressBar = %ProgressBar
@onready var _stats_label: Label = %StatsLabel
@onready var _cancel_button: Button = %CancelButton


func _ready() -> void:
	_cancel_button.pressed.connect(func() -> void: cancel_requested.emit())


func reset_view(mode_name: String) -> void:
	_phase_label.text = "Loading (%s)..." % mode_name
	_progress_bar.value = 0.0
	_stats_label.text = "Preparing..."
	_cancel_button.disabled = false


func set_phase(phase_text: String, progress_01: float = -1.0, stats_text: String = "") -> void:
	_phase_label.text = phase_text
	if progress_01 >= 0.0:
		_progress_bar.value = clampf(progress_01, 0.0, 1.0) * 100.0
	if not stats_text.is_empty():
		_stats_label.text = stats_text


func set_cancel_enabled(enabled: bool) -> void:
	_cancel_button.disabled = not enabled
