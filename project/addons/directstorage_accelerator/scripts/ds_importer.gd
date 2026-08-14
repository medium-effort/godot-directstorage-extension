@tool
class_name ResourceImporterDirectStorageTexture
extends EditorImportPlugin

static var _staging_warnings: Dictionary = {}

## DirectStorage Texture Import Plugin
##
## Author: MediumEffort
## Description: Converts .png, .jpg, .jpeg, and .webp sources into .dstex
## resources. The importer runs in the editor and exposes compression settings
## in the Import dock:
## - [b]compression/mode[/b]: S3TC VRAM block compression vs VRAM uncompressed.
## - [b]compression/source[/b]: Source color space (Generic, sRGB, Normal Map).
## - [b]compression/gdeflate_level[/b]: GDeflate compression level (1-12).

## Returns the stable importer identifier stored in .import metadata.
func _get_importer_name() -> String:
	return "directstorage_texture"

## Returns the importer name shown in the Import dock.
func _get_visible_name() -> String:
	return "DirectStorage Texture"

## Returns the source image extensions handled by this importer.
func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["png", "jpg", "jpeg", "webp"])

## Returns the generated resource extension.
func _get_save_extension() -> String:
	return "dstex"

## Returns the Godot resource type produced by the importer.
func _get_resource_type() -> String:
	return "Texture2D"

## Returns the importer format version used to invalidate old imports.
func _get_format_version() -> int:
	return 3

## Gives this importer priority over Godot's default texture importer.
func _get_priority() -> float:
	return 2.0

## Returns the import order used by the editor import pipeline.
func _get_import_order() -> int:
	return 0

## Returns the number of named import presets.
func _get_preset_count() -> int:
	return 1

## Returns the display name of an import preset.
func _get_preset_name(preset_index: int) -> String:
	return "Default"

## Declares compression mode, source color space, and GDeflate level options.
func _get_import_options(path: String, preset_index: int) -> Array[Dictionary]:
	return [
		{
			"name": "compression/mode",
			"default_value": 0,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "VRAM Compressed (S3TC),VRAM Uncompressed",
		},
		{
			"name": "compression/source",
			"default_value": Image.COMPRESS_SOURCE_SRGB,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Generic,sRGB,Normal Map",
		},
		{
			"name": "compression/gdeflate_level",
			"default_value": 1,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "1,12,1,prefer_slider",
		}
	]

## Keeps all importer options visible in the Import dock.
func _get_option_visibility(path: String, option_name: StringName, options: Dictionary) -> bool:
	return true

## Converts one source image into a compressed .dstex file and reports it to
## Godot's import pipeline through gen_files.
func _import(source_file: String, save_path: String, options: Dictionary, platform_variants: Array[String], gen_files: Array[String]) -> Error:
	var global_path := ProjectSettings.globalize_path(source_file)
	var img := Image.load_from_file(global_path)
	if img == null or img.is_empty():
		img = Image.new()
		var err := img.load(source_file)
		if err != OK:
			push_error("[DirectStorage Importer] Failed to load source image: " + source_file)
			return err

	if img.get_format() == Image.FORMAT_RGB8:
		img.convert(Image.FORMAT_RGBA8)

	if not img.has_mipmaps():
		img.generate_mipmaps()

	var compression_mode: int = int(options.get("compression/mode", 0))
	var compression_source: int = int(options.get("compression/source", Image.COMPRESS_SOURCE_SRGB))
	if compression_mode == 0:
		var compress_err := img.compress(Image.COMPRESS_S3TC, compression_source)
		if compress_err != OK:
			push_warning("[DirectStorage Importer] S3TC compression unavailable for '" + source_file + "'; falling back to lossless GDeflate.")
			compression_mode = 1

	var raw_data := img.get_data()
	var width := img.get_width()
	var height := img.get_height()
	var format := img.get_format()
	var mip_count: int = (img.get_mipmap_count() + 1) if img.has_mipmaps() else 1
	var gdeflate_level: int = int(options.get("compression/gdeflate_level", options.get("compression/level", 1)))
	var configured_staging_mb: float = float(ProjectSettings.get_setting("directstorage/staging_buffer_size_mb", 128.0))
	var staging_mb: float = configured_staging_mb if configured_staging_mb > 0.0 else 128.0
	if raw_data.size() > int(staging_mb * 1024.0 * 1024.0) and not _staging_warnings.has(source_file):
		_staging_warnings[source_file] = true
		push_warning("[DirectStorage Importer] Asset '" + source_file + "' requires " + str(ceili(float(raw_data.size()) / 1048576.0)) + " MiB of staging memory, but DirectStorage is configured for " + str(staging_mb) + " MiB. Increase Project Settings > DirectStorage > Staging Buffer Size.")

	if not ClassDB.class_exists("DSTex"):
		push_error("[DirectStorage Importer] ClassDB entry 'DSTex' missing. Is GDExtension built?")
		return ERR_UNCONFIGURED

	var dstex = ClassDB.instantiate("DSTex")
	if dstex == null:
		push_error("[DirectStorage Importer] Failed to instantiate DSTex.")
		return ERR_CANT_CREATE

	var out_file_path := save_path + "." + _get_save_extension()
	var write_err: Error = dstex.write_to_file(out_file_path, raw_data, width, height, format, mip_count, gdeflate_level)
	if write_err != OK:
		push_error("[DirectStorage Importer] Failed to write .dstex file to: " + out_file_path)
		return write_err

	gen_files.append(out_file_path)
	print("[DirectStorage Importer] Imported ", source_file, " -> ", out_file_path, " (Mode: ", "S3TC" if compression_mode == 0 else "GDeflate", ", GDeflate Level: ", gdeflate_level, ")")
	return OK
