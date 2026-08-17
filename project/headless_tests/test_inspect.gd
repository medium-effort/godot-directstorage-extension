@tool
extends SceneTree

func _init() -> void:
	var dstex = DSTex.new()
	var path = "res://.godot/imported/test.png-907c241c41bc767026da45239985ecb9.dstex"
	var header = dstex.read_header(path)
	print("HEADER: ", header)
	if header.has("pixel_format"):
		var rd_fmt = header["pixel_format"]
		var godot_fmt = DSTex.rd_format_to_godot_format(rd_fmt)
		print("rd_fmt=", rd_fmt, " godot_fmt=", godot_fmt)
	var res = ResourceLoader.load(path)
	print("Loaded res: ", res)
	quit(0)
