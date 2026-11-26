class_name DemoLoadContext
extends RefCounted

## Shared state & frame-hitch tracker passed to each demo loader strategy.

var tree: SceneTree
var scene_path: String
var loading_screen: Control
var cancelled: bool = false
var active_tag: int = 0
var active_scene_tag: int = 0

var prewarm_ms: float = 0.0
var instantiate_ms: float = 0.0
var max_hitch_ms: float = 0.0
var error_msg: String = ""
var loaded_scene: PackedScene = null

var _last_frame_usec: int = 0


func _init(p_tree: SceneTree, p_scene_path: String, p_loading_screen: Control) -> void:
	tree = p_tree
	scene_path = p_scene_path
	loading_screen = p_loading_screen
	_last_frame_usec = Time.get_ticks_usec()


func mark_frame_start() -> void:
	_last_frame_usec = Time.get_ticks_usec()


func next_frame() -> void:
	await tree.process_frame
	var now_usec := Time.get_ticks_usec()
	max_hitch_ms = maxf(max_hitch_ms, float(now_usec - _last_frame_usec) / 1000.0)
	_last_frame_usec = now_usec


func await_frame_post_draw() -> void:
	await RenderingServer.frame_post_draw
	var now_usec := Time.get_ticks_usec()
	max_hitch_ms = maxf(max_hitch_ms, float(now_usec - _last_frame_usec) / 1000.0)
	_last_frame_usec = now_usec


func record_sync_span(t0_usec: int) -> float:
	var t1_usec := Time.get_ticks_usec()
	max_hitch_ms = maxf(max_hitch_ms, float(t1_usec - _last_frame_usec) / 1000.0)
	_last_frame_usec = t1_usec
	return float(t1_usec - t0_usec) / 1000.0
