extends "res://addons/directstorage_accelerator/scripts/ds_runtime.gd"

## DirectStorage Legacy Compatibility Shim
##
## Author: MediumEffort
## Description: Backward-compatibility wrapper for older branches and commits
## that reference DSBatchManager (or its UID: uid://c3w3tb6oorh1f).
##
## When running in an older project where DSBatchManager is the primary autoload,
## this class inherits and runs the complete DirectStorage runtime coordinator loop.
##
## When running in a modern project where DSRuntime is the active autoload,
## this node acts as a transparent proxy to DSRuntime so state and slice metrics
## are never split between two coordinator instances.

var _target: Node = null


func _enter_tree() -> void:
	super._enter_tree()
	_target = _resolve_target()
	if _target:
		_target.prewarm_progress.connect(func(tag, done, total): prewarm_progress.emit(tag, done, total))
		_target.prewarm_finished.connect(func(tag): prewarm_finished.emit(tag))


func _resolve_target() -> Node:
	if name == "DSRuntime":
		return null
	var tree := get_tree()
	if tree and tree.root:
		var primary: Node = tree.root.get_node_or_null("DSRuntime")
		if primary != null and primary != self:
			return primary
	return null


func _process(delta: float) -> void:
	if not _target:
		super._process(delta)


func track_prewarm(tag: int) -> void:
	if _target:
		_target.track_prewarm(tag)
	else:
		super.track_prewarm(tag)


func untrack_prewarm(tag: int) -> void:
	if _target:
		_target.untrack_prewarm(tag)
	else:
		super.untrack_prewarm(tag)


func is_tracking_prewarm(tag: int) -> bool:
	return _target.is_tracking_prewarm(tag) if _target else super.is_tracking_prewarm(tag)


func set_slice_debug(enable: bool) -> void:
	if _target:
		_target.set_slice_debug(enable)
	else:
		super.set_slice_debug(enable)


func reset_slice_stats() -> void:
	if _target:
		_target.reset_slice_stats()
	else:
		super.reset_slice_stats()


func get_slice_stats() -> Dictionary:
	return _target.get_slice_stats() if _target else super.get_slice_stats()
