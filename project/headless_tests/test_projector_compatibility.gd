@tool
extends SceneTree

## Projector & Decal Compatibility Test
##
## Author: MediumEffort
## Description: Verifies the 'compatibility/as_image_texture' import option in ds_importer.gd
## and tests projector compatibility with ImageTexture.

const IMPORTER_PATH = "res://addons/directstorage_accelerator/scripts/ds_importer.gd"
const PLUGIN_PATH = "res://addons/directstorage_accelerator/scripts/ds_plugin.gd"

var failures := 0

func _init() -> void:
	print("[Test Projector Compat] Starting verification...")

	# 1. Verify Importer exposes compatibility/as_image_texture in source
	var importer_script: GDScript = load(IMPORTER_PATH)
	if importer_script == null:
		_fail("Unable to load ds_importer.gd")
		_finish()
		return

	var importer_source: String = importer_script.source_code
	_check("compatibility/as_image_texture" in importer_source, "ds_importer.gd declares 'compatibility/as_image_texture' option")
	_check("as_image_texture" in importer_source and "dstex.write_to_file" in importer_source, "ds_importer.gd passes as_image_texture to dstex.write_to_file")

	# 2. Verify Plugin logic in ds_plugin.gd source
	var plugin_script: GDScript = load(PLUGIN_PATH)
	if plugin_script == null:
		_fail("Unable to load ds_plugin.gd")
		_finish()
		return

	var plugin_source: String = plugin_script.source_code
	_check("_check_scene_compatibility" in plugin_source, "ds_plugin.gd implements _check_scene_compatibility")
	_check("_on_scene_changed" in plugin_source, "ds_plugin.gd implements _on_scene_changed")
	_check("_on_scene_closed" in plugin_source, "ds_plugin.gd implements _on_scene_closed")
	_check("_checked_scenes" in plugin_source, "ds_plugin.gd implements _checked_scenes tab-switch caching")

	# 3. Test that SpotLight3D and Decal cleanly accept ImageTexture without warnings/errors
	var root := Node3D.new()
	root.scene_file_path = "res://test_scenes/mock_scene.tscn"
	var light := SpotLight3D.new()
	var img := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	var img_tex := ImageTexture.create_from_image(img)
	light.light_projector = img_tex
	root.add_child(light)

	var decal := Decal.new()
	decal.set_texture(Decal.TEXTURE_ALBEDO, img_tex)
	root.add_child(decal)

	_check(light.light_projector == img_tex, "SpotLight3D successfully assigned ImageTexture projector")
	_check(decal.get_texture(Decal.TEXTURE_ALBEDO) == img_tex, "Decal successfully assigned ImageTexture")

	# Clean up
	root.free()
	_finish()

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		_fail(message)

func _fail(message: String) -> void:
	failures += 1
	printerr("[FAIL] ", message)

func _finish() -> void:
	print("[Test Projector Compat] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
