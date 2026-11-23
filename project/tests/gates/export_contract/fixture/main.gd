extends Node

## Fixture entry scene for the export-contract gate.
##
## It reports the facts the gate cannot read from outside the shipped player:
## how the command line arrived, which display server the run used, whether the
## PCK key entry is readable (and at what size), and whether the texture payloads
## are still resolvable from the PCK. The autoload boot verdict runs before this
## scene, so a failure verdict has already been printed when this line appears.
##
## One line, one boot. The process quits after a single frame so a successful
## case never waits for a window.


func _ready() -> void:
	var key_path := "res://.dspkg/toc.key"
	var key_size := -1
	if FileAccess.file_exists(key_path):
		key_size = FileAccess.get_file_as_bytes(key_path).size()
	var user_args := ", ".join(OS.get_cmdline_user_args())
	print("DSPKG_PROBE display=%s args=%d user_args=[%s] toc_key=%d payload_used=%s payload_unused=%s" % [
		DisplayServer.get_name(),
		OS.get_cmdline_args().size(),
		user_args,
		key_size,
		str(FileAccess.file_exists("res://assets/used.dstex")),
		str(FileAccess.file_exists("res://assets/unused.dstex")),
	])
	await get_tree().process_frame
	get_tree().quit(0)
