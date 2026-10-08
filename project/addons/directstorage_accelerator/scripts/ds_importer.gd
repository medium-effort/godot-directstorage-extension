@tool
class_name ResourceImporterDirectStorageTexture
extends EditorImportPlugin

static var _staging_warnings: Dictionary = {}

const D3D12_TEXTURE_DATA_PITCH_ALIGNMENT := 256

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
	return 4

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
		},
		{
			"name": "compatibility/as_image_texture",
			"default_value": false,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Forces ImageTexture CPU loading even during GPU prewarm. Required for Light3D projectors, Decals, and get_image() callers.",
		}
	]

## Keeps all importer options visible in the Import dock.
func _get_option_visibility(path: String, option_name: StringName, options: Dictionary) -> bool:
	return true

## Aligns an integer value up to the nearest multiple of alignment.
static func _align_up(value: int, alignment: int) -> int:
	return int((value + alignment - 1) / alignment) * alignment

## Computes the unaligned row byte size and total row count for one mip level.
static func _get_mip_row_layout(width: int, height: int, format: int) -> Array:
	var row_bytes := width * 4
	var row_count := height
	match format:
		Image.FORMAT_L8, Image.FORMAT_R8:
			row_bytes = width
		Image.FORMAT_LA8, Image.FORMAT_RG8:
			row_bytes = width * 2
		Image.FORMAT_RGB8:
			row_bytes = width * 3
		Image.FORMAT_RGBA8:
			row_bytes = width * 4
		Image.FORMAT_RF:
			row_bytes = width * 4
		Image.FORMAT_RGF:
			row_bytes = width * 8
		Image.FORMAT_RGBF:
			row_bytes = width * 12
		Image.FORMAT_RGBAF:
			row_bytes = width * 16
		Image.FORMAT_RH:
			row_bytes = width * 2
		Image.FORMAT_RGH:
			row_bytes = width * 4
		Image.FORMAT_RGBH:
			row_bytes = width * 6
		Image.FORMAT_RGBAH:
			row_bytes = width * 8
		Image.FORMAT_R16:
			row_bytes = width * 2
		Image.FORMAT_RG16:
			row_bytes = width * 4
		Image.FORMAT_RGB16:
			row_bytes = width * 6
		Image.FORMAT_RGBA16:
			row_bytes = width * 8
		Image.FORMAT_DXT1, Image.FORMAT_RGTC_R:
			row_bytes = maxi(1, ceili(float(width) / 4.0)) * 8
			row_count = maxi(1, ceili(float(height) / 4.0))
		Image.FORMAT_DXT3, Image.FORMAT_DXT5, Image.FORMAT_RGTC_RG, Image.FORMAT_BPTC_RGBA, Image.FORMAT_BPTC_RGBF, Image.FORMAT_BPTC_RGBFU:
			row_bytes = maxi(1, ceili(float(width) / 4.0)) * 16
			row_count = maxi(1, ceili(float(height) / 4.0))
	return [row_bytes, row_count]

## Pads raw texture rows to D3D12 256-byte pitch alignment for direct GPU decompression.
static func _pad_texture_data(raw_data: PackedByteArray, width: int, height: int, format: int, mip_count: int) -> PackedByteArray:
	var natural_size := 0
	var footprint_size := 0
	var mip_width := width
	var mip_height := height
	var layouts: Array[Array] = []
	for _mip in mip_count:
		var layout := _get_mip_row_layout(mip_width, mip_height, format)
		var row_bytes: int = layout[0]
		var row_count: int = layout[1]
		var row_pitch := _align_up(row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT)
		layouts.append([row_bytes, row_count, row_pitch])
		natural_size += row_bytes * row_count
		footprint_size += row_pitch * row_count
		mip_width = maxi(1, int(mip_width / 2))
		mip_height = maxi(1, int(mip_height / 2))

	if raw_data.size() != natural_size:
		return raw_data

	var padded := PackedByteArray()
	var source_offset := 0
	for layout in layouts:
		var row_bytes: int = layout[0]
		var row_count: int = layout[1]
		var row_pitch: int = layout[2]
		var row_padding := PackedByteArray()
		row_padding.resize(row_pitch - row_bytes)
		for row in row_count:
			padded.append_array(raw_data.slice(source_offset + row * row_bytes, source_offset + (row + 1) * row_bytes))
			padded.append_array(row_padding)
		source_offset += row_bytes * row_count
	return padded

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
	raw_data = _pad_texture_data(raw_data, width, height, format, mip_count)
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

	var is_srgb: bool = (compression_source == Image.COMPRESS_SOURCE_SRGB)
	var as_image_texture: bool = bool(options.get("compatibility/as_image_texture", false))
	var out_file_path := save_path + "." + _get_save_extension()
	var write_err: Error = dstex.write_to_file(out_file_path, raw_data, width, height, format, mip_count, gdeflate_level, is_srgb, as_image_texture)
	if write_err != OK:
		push_error("[DirectStorage Importer] Failed to write .dstex file to: " + out_file_path)
		return write_err

	gen_files.append(out_file_path)
	print("[DirectStorage Importer] Imported ", source_file, " -> ", out_file_path, " (Mode: ", "S3TC" if compression_mode == 0 else "GDeflate", ", GDeflate Level: ", gdeflate_level, ", As ImageTexture: ", as_image_texture, ")")
	return OK
