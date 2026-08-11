extends Node

## DirectStorage Per-Frame Batch Manager Autoload
##
## Author: MediumEffort
## Description: Flushes and submits all enqueued DirectStorage read requests at the end of each frame (_process).

## Initializes [DSContext] and reports the selected backend when the autoload
## enters the scene tree.
func _ready() -> void:
	if ClassDB.class_exists("DSContext"):
		var ctx = Engine.get_singleton("DSContext")
		if ctx:
			ctx.initialize_ds()
			print("[DirectStorage] Autoload initialized. Active Backend: ", ctx.get_active_backend_name())
	else:
		push_warning("[DirectStorage] GDExtension ClassDB entry 'DSContext' not found. Ensure GDExtension library built.")

## Submits all requests queued during the frame through [DSContext.flush_batch].
func _process(_delta: float) -> void:
	if ClassDB.class_exists("DSContext"):
		var ctx = Engine.get_singleton("DSContext")
		if ctx and ctx.is_initialized():
			ctx.flush_batch()

## Shuts down [DSContext] before the autoload is deleted.
func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		if ClassDB.class_exists("DSContext"):
			var ctx = Engine.get_singleton("DSContext")
			if ctx and ctx.is_initialized():
				ctx.shutdown_ds()
