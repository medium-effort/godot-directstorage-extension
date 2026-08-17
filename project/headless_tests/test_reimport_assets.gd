@tool
extends SceneTree

func _init() -> void:
	print("--- Testing Asset Loading and Import for test_assets ---")
	var dir = DirAccess.open("res://test_assets")
	if dir == null:
		print("FAIL: Could not open res://test_assets")
		quit(1)
		return

	dir.list_dir_begin()
	var file_name = dir.get_next()
	var loaded_count = 0
	while file_name != "":
		if not dir.current_is_dir() and file_name.ends_with(".png"):
			var asset_path = "res://test_assets/" + file_name
			print("Loading asset: ", asset_path)
			var tex = ResourceLoader.load(asset_path)
			if tex == null or not (tex is Texture2D):
				print("FAIL: Could not load texture: ", asset_path)
				quit(1)
				return
			print("  SUCCESS: Loaded ", asset_path, " [", tex.get_width(), "x", tex.get_height(), "] as ", tex.get_class())
			loaded_count += 1
		file_name = dir.get_next()
	dir.list_dir_end()

	print("SUCCESS: Successfully loaded ", loaded_count, " test assets with zero errors!")
	quit(0)
