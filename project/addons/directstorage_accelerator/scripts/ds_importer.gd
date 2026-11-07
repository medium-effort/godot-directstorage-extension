@tool
class_name ResourceImporterDirectStorageTexture
extends EditorImportPlugin

static var _staging_warnings: Dictionary = {}

const D3D12_TEXTURE_DATA_PITCH_ALIGNMENT := 256
const DSTEX_MAX_DIMENSION := 16384

const IMPORT_PROFILE_SETTING := "directstorage/import/default_profile"
const PROFILE_3D := 0
const PROFILE_2D := 1
const PRESET_PROJECT_DEFAULT := 0
const PRESET_3D := 1
const PRESET_2D := 2
const PRESET_NAMES := ["Project Default", "3D (Streaming)", "CPU / ImageTexture (2D, UI, Decals)"]

## DirectStorage Texture Import Plugin
##
## Author: MediumEffort
## Description: Converts .png, .jpg, .jpeg, and .webp sources into .dstex
## resources. The importer runs in the editor and exposes compression settings
## in the Import dock:
## - [b]compression/mode[/b]: VRAM block compression vs VRAM uncompressed.
## - [b]compression/high_quality[/b]: BPTC/BC7 instead of S3TC (BC1-BC5).
## - [b]compression/source[/b]: Source color space (Generic, sRGB, Normal Map).
## - [b]compression/gdeflate_level[/b]: GDeflate compression level (1-12).
## - [b]mipmaps/generate[/b]: Generate a full mip chain.
## - [b]process/size_limit[/b]: Resize the longer edge before mip generation.
## - [b]process/fix_alpha_border[/b], [b]process/premult_alpha[/b]: 2D alpha fixes.
## - [b]compatibility/as_image_texture[/b]: Force the CPU ImageTexture path.
##
## Presets: "Project Default" follows directstorage/import/default_profile;
## "3D (Streaming)" is the product default; "CPU / ImageTexture (2D, UI, Decals)"
## keeps UI and sprite assets on the CPU path.

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
	return 5

## Gives this importer priority over Godot's default texture importer.
func _get_priority() -> float:
	return 2.0

## Returns the import order used by the editor import pipeline.
func _get_import_order() -> int:
	return 0

## Returns the number of named import presets.
func _get_preset_count() -> int:
	return 3

## Resolves the numeric profile from the project setting.
func _project_profile() -> int:
	var configured := int(ProjectSettings.get_setting(IMPORT_PROFILE_SETTING, PROFILE_3D))
	if configured < PROFILE_3D or configured > PROFILE_2D:
		return PROFILE_3D
	return configured

## Clamps an out-of-range preset index to the project-default preset.
func _clamp_preset(preset_index: int) -> int:
	if preset_index < PRESET_PROJECT_DEFAULT or preset_index > PRESET_2D:
		return PRESET_PROJECT_DEFAULT
	return preset_index

## Resolves a preset index to the concrete 3D or 2D preset.
func _resolved_preset(preset_index: int) -> int:
	var preset := _clamp_preset(preset_index)
	if preset == PRESET_PROJECT_DEFAULT:
		return PRESET_3D if _project_profile() == PROFILE_3D else PRESET_2D
	return preset

## Returns the display name of an import preset.
func _get_preset_name(preset_index: int) -> String:
	var preset := _clamp_preset(preset_index)
	if preset == PRESET_PROJECT_DEFAULT:
		return "Project Default (3D)" if _project_profile() == PROFILE_3D else "Project Default (2D)"
	return PRESET_NAMES[preset]

## Declares the compression, mipmap, and processing options for a preset.
func _get_import_options(path: String, preset_index: int) -> Array[Dictionary]:
	var preset := _resolved_preset(preset_index)
	var is_2d := preset == PRESET_2D
	return [
		{
			"name": "compression/mode",
			"default_value": 1 if is_2d else 0,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "VRAM Compressed (S3TC),VRAM Uncompressed",
			"usage": PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_UPDATE_ALL_IF_MODIFIED,
		},
		{
			"name": "compression/high_quality",
			"default_value": false,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Uses BPTC/BC7 instead of S3TC (BC1-BC5). Slower to import; ignored when mode is VRAM Uncompressed.",
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
			"name": "mipmaps/generate",
			"default_value": not is_2d,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Generate a full mip chain. Disable for 2D/UI textures that are always sampled at 1:1.",
		},
		{
			"name": "process/size_limit",
			"default_value": 0,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "0,16384,1",
		},
		{
			"name": "process/fix_alpha_border",
			"default_value": is_2d,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Puts surrounding color into transparent borders to reduce bilinear-filter halos.",
		},
		{
			"name": "process/premult_alpha",
			"default_value": false,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Premultiply RGB by alpha. Requires premultiplied-alpha materials.",
		},
		{
			"name": "compatibility/as_image_texture",
			"default_value": is_2d,
			"property_hint": PROPERTY_HINT_NONE,
			"hint_string": "Forces ImageTexture CPU loading even during GPU prewarm. Required for Light3D projectors, Decals, and get_image() callers.",
		}
	]

## Hides options that do not apply to the current compression mode.
func _get_option_visibility(path: String, option_name: StringName, options: Dictionary) -> bool:
	if option_name == &"compression/high_quality":
		return int(options.get("compression/mode", 0)) == 0
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

	var compression_mode: int = int(options.get("compression/mode", 0))
	var high_quality: bool = bool(options.get("compression/high_quality", false))
	var compression_source: int = int(options.get("compression/source", Image.COMPRESS_SOURCE_SRGB))
	var generate_mipmaps: bool = bool(options.get("mipmaps/generate", true))
	var size_limit: int = clampi(int(options.get("process/size_limit", 0)), 0, DSTEX_MAX_DIMENSION)
	var fix_alpha_border: bool = bool(options.get("process/fix_alpha_border", false))
	var premult_alpha: bool = bool(options.get("process/premult_alpha", false))

	if not ClassDB.class_exists("DSTex"):
		push_error("[DirectStorage Importer] ClassDB entry 'DSTex' missing. Is GDExtension built?")
		return ERR_UNCONFIGURED
	var dstex = ClassDB.instantiate("DSTex")
	if dstex == null:
		push_error("[DirectStorage Importer] Failed to instantiate DSTex.")
		return ERR_CANT_CREATE

	# L8/LA8 have no RD sRGB/luminance swizzle in .dstex; RGB8 needs an alpha
	# channel for the alpha options. R8/RG8 are exact and stay as-is.
	match img.get_format():
		Image.FORMAT_L8, Image.FORMAT_LA8, Image.FORMAT_RGB8:
			img.convert(Image.FORMAT_RGBA8)

	# Bound the source before mip generation and compression.
	var limit := size_limit if size_limit > 0 else DSTEX_MAX_DIMENSION
	var implicit_limit := size_limit == 0
	var source_width := img.get_width()
	var source_height := img.get_height()
	if source_width > limit or source_height > limit:
		var new_width: int
		var new_height: int
		if source_width >= source_height:
			new_width = limit
			new_height = maxi(1, int(round(float(source_height) * float(limit) / float(source_width))))
		else:
			new_height = limit
			new_width = maxi(1, int(round(float(source_width) * float(limit) / float(source_height))))
		img.resize(new_width, new_height, Image.INTERPOLATE_CUBIC)
		if implicit_limit:
			push_warning("[DirectStorage Importer] Asset '%s' exceeded the %d px container limit and was resized from %dx%d to %dx%d." % [source_file, DSTEX_MAX_DIMENSION, source_width, source_height, new_width, new_height])

	# Block codecs pad to 4x4 blocks internally; align first so the mip chain and
	# the written dimensions agree.
	if compression_mode == 0:
		var aligned_width := img.get_width()
		var aligned_height := img.get_height()
		var target_width := aligned_width if aligned_width <= 2 else (aligned_width + 3) & ~3
		var target_height := aligned_height if aligned_height <= 2 else (aligned_height + 3) & ~3
		if target_width != aligned_width or target_height != aligned_height:
			img.resize(target_width, target_height, Image.INTERPOLATE_CUBIC)
			push_warning("[DirectStorage Importer] Asset '%s' dimensions %dx%d are not a multiple of 4; block compression stores them as %dx%d." % [source_file, aligned_width, aligned_height, target_width, target_height])

	# Godot normalizes the base level only after a size-limit resize; do it for
	# every normal map so the source normals are unit length before mips.
	if compression_source == Image.COMPRESS_SOURCE_NORMAL:
		ClassDB.class_call_static("DSTex", "normalize_image", img)

	if fix_alpha_border:
		img.fix_alpha_edges()
	if premult_alpha:
		img.premultiply_alpha()

	if generate_mipmaps:
		img.generate_mipmaps(compression_source == Image.COMPRESS_SOURCE_NORMAL)
	else:
		img.clear_mipmaps()

	var effective_codec := "Uncompressed"
	if compression_mode == 0:
		var requested_codec := "BPTC" if high_quality else "S3TC"
		var compressed := false
		if high_quality:
			var bptc_error := img.compress(Image.COMPRESS_BPTC, compression_source)
			compressed = bptc_error == OK and img.is_compressed() and img.get_format() == Image.FORMAT_BPTC_RGBA
			if compressed:
				effective_codec = "BPTC"
		if not compressed:
			var s3tc_error := img.compress(Image.COMPRESS_S3TC, compression_source)
			var s3tc_formats := [Image.FORMAT_DXT1, Image.FORMAT_DXT3, Image.FORMAT_DXT5, Image.FORMAT_RGTC_R, Image.FORMAT_RGTC_RG]
			compressed = s3tc_error == OK and img.is_compressed() and s3tc_formats.has(img.get_format())
			if compressed:
				effective_codec = "S3TC"
		if not compressed:
			if img.get_format() != Image.FORMAT_RGBA8:
				img.convert(Image.FORMAT_RGBA8)
			effective_codec = "Uncompressed"
			push_warning("[DirectStorage Importer] Asset '%s': requested %s compression failed (codec unavailable or unsupported); wrote uncompressed RGBA8." % [source_file, requested_codec])
		elif effective_codec != requested_codec:
			push_warning("[DirectStorage Importer] Asset '%s': requested %s compression unavailable; wrote %s." % [source_file, requested_codec, effective_codec])

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

	var is_srgb: bool = (compression_source == Image.COMPRESS_SOURCE_SRGB)
	var as_image_texture: bool = bool(options.get("compatibility/as_image_texture", false))
	var out_file_path := save_path + "." + _get_save_extension()
	var write_err: Error = dstex.write_to_file(out_file_path, raw_data, width, height, format, mip_count, gdeflate_level, is_srgb, as_image_texture)
	if write_err != OK:
		push_error("[DirectStorage Importer] Failed to write .dstex file to: " + out_file_path)
		return write_err

	gen_files.append(out_file_path)
	print("[DirectStorage Importer] Imported ", source_file, " -> ", out_file_path, " (Mode: ", effective_codec, ", GDeflate Level: ", gdeflate_level, ", As ImageTexture: ", as_image_texture, ")")
	return OK
