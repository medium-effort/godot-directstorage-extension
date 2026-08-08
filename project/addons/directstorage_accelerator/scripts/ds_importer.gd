@tool
extends EditorImportPlugin

## DirectStorage Texture Import Plugin
##
## Author: MediumEffort
## Description: Custom EditorImportPlugin to convert textures (.png, .jpg, .jpeg, .webp) into .dstex format.

func _get_importer_name() -> String:
	return "directstorage_texture"

func _get_visible_name() -> String:
	return "DirectStorage Texture"

func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["png", "jpg", "jpeg", "webp"])

func _get_save_extension() -> String:
	return "dstex"

func _get_resource_type() -> String:
	return "Texture2D"

func _get_priority() -> float:
	return 2.0

func _get_import_order() -> int:
	return 0

func _get_preset_count() -> int:
	return 1

func _get_preset_name(preset_index: int) -> String:
	return "Default"

func _get_import_options(path: String, preset_index: int) -> Array[Dictionary]:
	return [
		{
			"name": "compression/level",
			"default_value": 1,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "1,12,1",
		}
	]

func _get_option_visibility(path: String, option_name: StringName, options: Dictionary) -> bool:
	return true

func _import(source_file: String, save_path: String, options: Dictionary, platform_variants: Array[String], gen_files: Array[String]) -> Error:
	var img := Image.new()
	var err := img.load(source_file)
	if err != OK:
		push_error("[DirectStorage Importer] Failed to load source image: " + source_file)
		return err

	if not img.has_mipmaps():
		img.generate_mipmaps()

	var raw_data := img.get_data()
	var width := img.get_width()
	var height := img.get_height()
	var format := img.get_format()
	var mip_count := img.get_mipmap_count()
	var compression_level: int = options.get("compression/level", 1)

	if not ClassDB.class_exists("DSTex"):
		push_error("[DirectStorage Importer] ClassDB entry 'DSTex' missing. Is GDExtension built?")
		return ERR_UNCONFIGURED

	var dstex = ClassDB.instantiate("DSTex")
	if dstex == null:
		push_error("[DirectStorage Importer] Failed to instantiate DSTex.")
		return ERR_CANT_CREATE

	var out_file_path := save_path + "." + _get_save_extension()
	var write_err: Error = dstex.write_to_file(out_file_path, raw_data, width, height, format, mip_count, compression_level)
	if write_err != OK:
		push_error("[DirectStorage Importer] Failed to write .dstex file to: " + out_file_path)
		return write_err

	print("[DirectStorage Importer] Imported ", source_file, " -> ", out_file_path, " (Level: ", compression_level, ")")
	return OK
