extends Control

## DirectStorage Verification Test Scene Script
##
## Author: MediumEffort
## Description: Verifies DirectStorage GDExtension initialization, backend detection, request creation, batch submission, and profiling metrics.

@onready var status_label: Label = $VBoxContainer/StatusLabel
@onready var backend_label: Label = $VBoxContainer/BackendLabel
@onready var info_label: Label = $VBoxContainer/InfoLabel
@onready var test_button: Button = $VBoxContainer/TestButton

func _ready() -> void:
	test_button.pressed.connect(_on_test_button_pressed)
	update_status()

func update_status() -> void:
	if not ClassDB.class_exists("DSContext"):
		status_label.text = "Status: GDExtension Not Loaded (Check build)"
		backend_label.text = "Backend: None"
		info_label.text = "Please build the GDExtension library with SCons."
		return

	var ctx = Engine.get_singleton("DSContext")
	if ctx:
		if not ctx.is_initialized():
			ctx.initialize_ds()

		var is_supported = ctx.is_supported()
		var is_d3d12 = ctx.is_d3d12_backend()
		var backend_name = ctx.get_active_backend_name()

		status_label.text = "Status: GDExtension Active (Initialized)"
		backend_label.text = "Active Backend: " + backend_name
		info_label.text = "Platform Supported: " + str(is_supported) + " | D3D12 Active: " + str(is_d3d12)
	else:
		status_label.text = "Status: DSContext Singleton Missing"

func _on_test_button_pressed() -> void:
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSRequest"):
		info_label.text = "Error: DSContext / DSRequest classes not available in ClassDB."
		return

	var ctx = Engine.get_singleton("DSContext")
	if not ctx or not ctx.is_initialized():
		info_label.text = "Error: DSContext is not initialized."
		return

	info_label.text = "Running DirectStorage Read Test..."

	# Create a test request
	var req = DSRequest.new()
	req.file_path = "res://project.godot"
	req.offset = 0
	req.size = 128
	req.cancellation_tag = 1
	req.compression_format = DSRequest.COMPRESSION_NONE

	var enqueued = ctx.enqueue_request(req)
	ctx.flush_batch()

	var profiler = Engine.get_singleton("DSProfiler")
	var throughput = profiler.get_last_throughput_mbps() if profiler else 0.0

	info_label.text = "Test Request Enqueued: " + str(enqueued) + " | Status: " + str(req.status) + " | Throughput: " + String.num(throughput, 2) + " MB/s"
	print("[DirectStorage Test] Enqueued: ", enqueued, " Status: ", req.status, " Backend: ", ctx.get_active_backend_name())
