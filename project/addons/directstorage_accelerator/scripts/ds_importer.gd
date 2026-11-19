@tool
class_name ResourceImporterDirectStorageTexture
extends EditorImportPlugin

static var _staging_warnings: Dictionary = {}

const DSTEX_MAX_DIMENSION := 16384

const IMPORT_PROFILE_SETTING := "dsgd_extension/default_import_profile"
const PROFILE_3D := 0
const PROFILE_2D := 1
const PRESET_PROJECT_DEFAULT := 0
const PRESET_3D := 1
const PRESET_2D := 2
const PRESET_NAMES := ["Project Default", "3D (Streaming)", "CPU / ImageTexture (2D, UI, Decals)"]

## Format version stamped into every .import sidecar this importer writes. Migration tooling
## derives its target from this constant, so bumping it cannot leave the tool behind.
const FORMAT_VERSION := 5

# Replaces the single v5 compression/source enum. The sRGB flag is the only
# part .dstex stores explicitly; channel interpretation is a compression-time
# decision, so the two are separate options now.
const COLOR_SPACE_LINEAR := 0
const COLOR_SPACE_SRGB := 1
const NORMAL_MAP_DISABLED := 0
const NORMAL_MAP_ENABLED := 1
const CHANNEL_PACK_SRGB_FRIENDLY := 0
const CHANNEL_PACK_OPTIMIZED := 1

# Mirror ResourceImporterTexture::ChannelRemap (REMAP_R..REMAP_1) for the
# process/channel_remap/* options.
const CHANNEL_REMAP_RED := 0
const CHANNEL_REMAP_GREEN := 1
const CHANNEL_REMAP_BLUE := 2
const CHANNEL_REMAP_ALPHA := 3

const DDS_SUPPORTED_COMPRESSED_FORMATS := [
	Image.FORMAT_DXT1,
	Image.FORMAT_DXT3,
	Image.FORMAT_DXT5,
	Image.FORMAT_RGTC_R,
	Image.FORMAT_RGTC_RG,
	Image.FORMAT_BPTC_RGBA,
	Image.FORMAT_BPTC_RGBF,
	Image.FORMAT_BPTC_RGBFU,
]
const DDS_MAGIC := 0x20534444
const DDS_FOURCC_DX10 := 0x30315844
const DDS_CAPS2_CUBEMAP := 0x200
const DDS_CAPS2_VOLUME := 0x200000
const DDS_DIMENSION_TEXTURE2D := 3

# Keys introduced by the color-option split. Their presence in an asset's .import file
# means the user (or a previous import with the new options) already owns the channel settings,
# so the legacy compression/source shim must not override them.
const COLOR_OPTION_KEYS := ["compression/color_space", "compress/normal_map", "compress/channel_pack"]

## DirectStorage Texture Import Plugin
##
## Author: MediumEffort
## Description: Converts .png, .jpg, .jpeg, .webp, .tga, and .dds sources into .dstex
## resources. The importer runs in the editor and exposes compression settings
## in the Import dock:
## - [b]compression/mode[/b]: VRAM block compression vs VRAM uncompressed.
## - [b]compression/high_quality[/b]: BPTC/BC7 instead of S3TC (BC1-BC5).
## - [b]compression/color_space[/b]: Linear or sRGB storage flag.
## - [b]compress/normal_map[/b]: Normalize and renormalize as a normal map.
## - [b]compress/channel_pack[/b]: sRGB-friendly or optimized channel packing.
## - [b]compression/gdeflate_level[/b]: GDeflate compression level (1-12), import-time
##   only. Defaults to 3 for every preset: on block-compressed payloads that buys
##   ~1-2% for tens of milliseconds per stream, and on uncompressed payloads (the
##   CPU / ImageTexture preset) 6-27% for a similar cost. Levels 1-6 are the
##   practical range; 9-12 cost 5-13x compression time. The level does not change
##   runtime decompression cost; see the addon README for the measured tables.
## - [b]compression/chunk_size_kb[/b]: split each mip into independent GDeflate
##   chunks of at most this many uncompressed KiB. Chunks decouple the
##   DirectStorage staging requirement from the texture size. 0 restores the
##   legacy one-stream-per-mip layout: the Windows backend must then split the
##   stream at load time and the staging buffer has to cover a whole mip, so keep
##   it non-zero for new assets.
## - [b]mipmaps/generate[/b]: Generate a full mip chain.
## - [b]process/size_limit[/b]: Resize the longer edge before mip generation.
## - [b]process/channel_remap/red|green|blue|alpha[/b]: Swizzle, invert, or replace channels (e.g. pack PBR masks, flip normal-map Y).
## - [b]process/fix_alpha_border[/b], [b]process/premult_alpha[/b]: 2D alpha fixes.
## - [b]compatibility/as_image_texture[/b]: Force the CPU ImageTexture path.
##
## Presets: "Project Default" follows dsgd_extension/default_import_profile;
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
	return PackedStringArray(["png", "jpg", "jpeg", "webp", "tga", "dds"])

## Returns the generated resource extension.
func _get_save_extension() -> String:
	return "dstex"

## Returns the Godot resource type produced by the importer.
func _get_resource_type() -> String:
	return "Texture2D"

## Returns the importer format version used to invalidate old imports.
func _get_format_version() -> int:
	return FORMAT_VERSION

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
			"name": "compression/color_space",
			"default_value": COLOR_SPACE_SRGB,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Linear,sRGB",
		},
		{
			"name": "compress/normal_map",
			"default_value": NORMAL_MAP_DISABLED,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Disabled,Enabled",
		},
		{
			"name": "compress/channel_pack",
			"default_value": CHANNEL_PACK_SRGB_FRIENDLY,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "sRGB Friendly,Optimized",
		},
		{
			"name": "compression/disk_compression",
			"default_value": 1,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "None,GDeflate",
			"usage": PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_UPDATE_ALL_IF_MODIFIED,
		},
		{
			"name": "compression/gdeflate_level",
			"default_value": 3,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "1,12,1,prefer_slider",
		},
		{
			"name": "compression/chunk_size_kb",
			"default_value": 8192,
			"property_hint": PROPERTY_HINT_RANGE,
			"hint_string": "0,65536,1024,or_greater",
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
			"name": "process/channel_remap/red",
			"default_value": CHANNEL_REMAP_RED,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Red,Green,Blue,Alpha,Inverted Red,Inverted Green,Inverted Blue,Inverted Alpha,Unused,Zero,One",
		},
		{
			"name": "process/channel_remap/green",
			"default_value": CHANNEL_REMAP_GREEN,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Red,Green,Blue,Alpha,Inverted Red,Inverted Green,Inverted Blue,Inverted Alpha,Unused,Zero,One",
		},
		{
			"name": "process/channel_remap/blue",
			"default_value": CHANNEL_REMAP_BLUE,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Red,Green,Blue,Alpha,Inverted Red,Inverted Green,Inverted Blue,Inverted Alpha,Unused,Zero,One",
		},
		{
			"name": "process/channel_remap/alpha",
			"default_value": CHANNEL_REMAP_ALPHA,
			"property_hint": PROPERTY_HINT_ENUM,
			"hint_string": "Red,Green,Blue,Alpha,Inverted Red,Inverted Green,Inverted Blue,Inverted Alpha,Unused,Zero,One",
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
			"hint_string": "Forces ImageTexture CPU loading even during GPU prewarm. Required for Light3D projectors, Decals, and get_image() callers. Also the supported way to get authoring (logical) dimensions for non-multiple-of-4 sources, which GPU Texture2DRD wraps report at the padded storage size.",
		}
	]

## Hides options that do not apply to the current compression mode or disk compression codec.
func _get_option_visibility(path: String, option_name: StringName, options: Dictionary) -> bool:
	if option_name == &"compression/high_quality" or option_name == &"compress/channel_pack":
		return int(options.get("compression/mode", 0)) == 0
	if option_name == &"compression/gdeflate_level":
		return int(options.get("compression/disk_compression", 1)) == 1
	return true

## Returns true for an asset whose .import still stores the
## combined compression/source key and has not been migrated to the split options.
## Godot backfills missing option defaults in memory before calling _import, so
## the on-disk .import file is the only reliable record of what the user stored.
func _uses_legacy_compression_source(source_file: String, options: Dictionary) -> bool:
	if not options.has("compression/source"):
		return false
	var config := ConfigFile.new()
	if config.load(source_file + ".import") != OK:
		return false
	if not config.has_section("params"):
		return false
	for key in COLOR_OPTION_KEYS:
		if config.has_section_key("params", key):
			return false
	return config.has_section_key("params", "compression/source")


## Resolves the split color options, falling back to the legacy compression/source
## enum for assets that have not migrated yet. The legacy mapping is exactly the
## old behavior: Generic -> linear/optimized, sRGB -> sRGB/friendly, Normal Map
## -> linear/optimized with normal-map processing.
func _resolve_color_options(source_file: String, options: Dictionary) -> Dictionary:
	var color_space: int = clampi(int(options.get("compression/color_space", COLOR_SPACE_SRGB)), COLOR_SPACE_LINEAR, COLOR_SPACE_SRGB)
	var normal_map: int = clampi(int(options.get("compress/normal_map", NORMAL_MAP_DISABLED)), NORMAL_MAP_DISABLED, NORMAL_MAP_ENABLED)
	var channel_pack: int = clampi(int(options.get("compress/channel_pack", CHANNEL_PACK_SRGB_FRIENDLY)), CHANNEL_PACK_SRGB_FRIENDLY, CHANNEL_PACK_OPTIMIZED)

	if _uses_legacy_compression_source(source_file, options):
		var legacy: int = clampi(int(options.get("compression/source", Image.COMPRESS_SOURCE_SRGB)), Image.COMPRESS_SOURCE_GENERIC, Image.COMPRESS_SOURCE_NORMAL)
		match legacy:
			Image.COMPRESS_SOURCE_GENERIC:
				color_space = COLOR_SPACE_LINEAR
				normal_map = NORMAL_MAP_DISABLED
				channel_pack = CHANNEL_PACK_OPTIMIZED
			Image.COMPRESS_SOURCE_NORMAL:
				color_space = COLOR_SPACE_LINEAR
				normal_map = NORMAL_MAP_ENABLED
				channel_pack = CHANNEL_PACK_OPTIMIZED
			_:
				color_space = COLOR_SPACE_SRGB
				normal_map = NORMAL_MAP_DISABLED
				channel_pack = CHANNEL_PACK_SRGB_FRIENDLY

	var compress_source := Image.COMPRESS_SOURCE_GENERIC
	if normal_map == NORMAL_MAP_ENABLED:
		compress_source = Image.COMPRESS_SOURCE_NORMAL
	elif channel_pack == CHANNEL_PACK_SRGB_FRIENDLY:
		compress_source = Image.COMPRESS_SOURCE_SRGB

	return {
		"color_space": color_space,
		"normal_map": normal_map == NORMAL_MAP_ENABLED,
		"compress_source": compress_source,
	}

## Rejects DDS cubemaps, volume textures, and texture arrays before load_dds_from_buffer(),
## because Image.load_dds_from_buffer() silently returns only the first 2D slice.
static func _validate_dds_2d_header(bytes: PackedByteArray, source_file: String) -> Error:
	if bytes.size() < 128 or bytes.decode_u32(0) != DDS_MAGIC:
		push_error("[DirectStorage Importer] Invalid DDS header: " + source_file)
		return ERR_FILE_CORRUPT
	var caps2: int = bytes.decode_u32(112)
	if (caps2 & (DDS_CAPS2_CUBEMAP | DDS_CAPS2_VOLUME)) != 0:
		push_error("[DirectStorage Importer] Unsupported DDS cubemap or volume texture: " + source_file)
		return ERR_UNAVAILABLE
	var fourcc: int = bytes.decode_u32(84)
	if fourcc == DDS_FOURCC_DX10:
		if bytes.size() < 148:
			push_error("[DirectStorage Importer] Truncated DX10 DDS header: " + source_file)
			return ERR_FILE_CORRUPT
		var resource_dimension: int = bytes.decode_u32(132)
		var array_size: int = bytes.decode_u32(140)
		if resource_dimension != DDS_DIMENSION_TEXTURE2D or array_size > 1:
			push_error("[DirectStorage Importer] Unsupported DDS DX10 non-2D or texture array: " + source_file)
			return ERR_UNAVAILABLE
	return OK

## Loads a source image, routing .dds files through Image.load_dds_from_buffer().
static func _load_source_image(source_file: String, global_path: String) -> Dictionary:
	if source_file.get_extension().to_lower() == "dds":
		var dds_bytes := FileAccess.get_file_as_bytes(source_file)
		if dds_bytes.is_empty():
			dds_bytes = FileAccess.get_file_as_bytes(global_path)
		if dds_bytes.is_empty():
			var open_err := FileAccess.get_open_error()
			return {"image": null, "error": open_err if open_err != OK else ERR_CANT_OPEN}
		var header_err := _validate_dds_2d_header(dds_bytes, source_file)
		if header_err != OK:
			return {"image": null, "error": header_err}
		var dds_img := Image.new()
		var dds_err := dds_img.load_dds_from_buffer(dds_bytes)
		if dds_err != OK or dds_img.is_empty():
			return {"image": null, "error": dds_err if dds_err != OK else ERR_CANT_CREATE}
		return {"image": dds_img, "error": OK}

	var img := Image.load_from_file(global_path)
	if img == null or img.is_empty():
		img = Image.new()
		var err := img.load(source_file)
		if err != OK:
			return {"image": null, "error": err}
	return {"image": img, "error": OK}

## Normalizes uncompressed DDS/source formats that DSTex does not map directly,
## or rejects integer / shared-exponent formats with ERR_UNAVAILABLE.
static func _ensure_dstex_uncompressed_format(img: Image, source_file: String) -> Error:
	match img.get_format():
		Image.FORMAT_RGB565, Image.FORMAT_RGBA4444:
			img.convert(Image.FORMAT_RGBA8)
		Image.FORMAT_R16I, Image.FORMAT_RG16I, Image.FORMAT_RGBA16I, Image.FORMAT_RGBE9995:
			push_error("[DirectStorage Importer] Unsupported pixel format for .dstex: " + source_file)
			return ERR_UNAVAILABLE
	return OK

## Returns a descriptive codec label for compressed DDS passthrough imports.
static func _passthrough_codec_label(img_format: int) -> String:
	match img_format:
		Image.FORMAT_BPTC_RGBA, Image.FORMAT_BPTC_RGBF, Image.FORMAT_BPTC_RGBFU:
			return "DDS Passthrough (BPTC)"
		_:
			return "DDS Passthrough (S3TC)"

## Converts one source image into a compressed .dstex file and reports it to
## Godot's import pipeline through gen_files.
func _import(source_file: String, save_path: String, options: Dictionary, platform_variants: Array[String], gen_files: Array[String]) -> Error:
	var global_path := ProjectSettings.globalize_path(source_file)
	var load_result := _load_source_image(source_file, global_path)
	if load_result["error"] != OK or load_result["image"] == null:
		push_error("[DirectStorage Importer] Failed to load source image: " + source_file)
		return load_result["error"]
	var img: Image = load_result["image"]

	var compression_mode: int = int(options.get("compression/mode", 0))
	var high_quality: bool = bool(options.get("compression/high_quality", false))
	var color_options := _resolve_color_options(source_file, options)
	var compression_source: int = color_options["compress_source"]
	var is_normal_map: bool = color_options["normal_map"]
	var generate_mipmaps: bool = bool(options.get("mipmaps/generate", true))
	var size_limit: int = clampi(int(options.get("process/size_limit", 0)), 0, DSTEX_MAX_DIMENSION)
	var fix_alpha_border: bool = bool(options.get("process/fix_alpha_border", false))
	var premult_alpha: bool = bool(options.get("process/premult_alpha", false))
	var remap_r: int = int(options.get("process/channel_remap/red", CHANNEL_REMAP_RED))
	var remap_g: int = int(options.get("process/channel_remap/green", CHANNEL_REMAP_GREEN))
	var remap_b: int = int(options.get("process/channel_remap/blue", CHANNEL_REMAP_BLUE))
	var remap_a: int = int(options.get("process/channel_remap/alpha", CHANNEL_REMAP_ALPHA))

	if not ClassDB.class_exists("DSTex"):
		push_error("[DirectStorage Importer] ClassDB entry 'DSTex' missing. Is GDExtension built?")
		return ERR_UNCONFIGURED
	var dstex = ClassDB.instantiate("DSTex")
	if dstex == null:
		push_error("[DirectStorage Importer] Failed to instantiate DSTex.")
		return ERR_CANT_CREATE

	var limit := size_limit if size_limit > 0 else DSTEX_MAX_DIMENSION
	var identity_remap := (
		remap_r == CHANNEL_REMAP_RED and
		remap_g == CHANNEL_REMAP_GREEN and
		remap_b == CHANNEL_REMAP_BLUE and
		remap_a == CHANNEL_REMAP_ALPHA
	)
	var can_passthrough := (
		img.is_compressed() and
		DDS_SUPPORTED_COMPRESSED_FORMATS.has(img.get_format()) and
		compression_mode == 0 and
		not is_normal_map and
		not fix_alpha_border and
		not premult_alpha and
		identity_remap and
		img.get_width() <= limit and
		img.get_height() <= limit and
		(not generate_mipmaps or img.has_mipmaps())
	)

	var logical_width := img.get_width()
	var logical_height := img.get_height()
	var effective_codec := "Uncompressed"

	if can_passthrough:
		if not generate_mipmaps:
			img.clear_mipmaps()
		effective_codec = _passthrough_codec_label(img.get_format())
	else:
		if img.is_compressed():
			if not DDS_SUPPORTED_COMPRESSED_FORMATS.has(img.get_format()):
				push_error("[DirectStorage Importer] Unsupported compressed source format: " + source_file)
				return ERR_UNAVAILABLE
			var decomp_err := img.decompress()
			if decomp_err != OK:
				push_error("[DirectStorage Importer] Failed to decompress source image: " + source_file)
				return decomp_err

		var fmt_err := _ensure_dstex_uncompressed_format(img, source_file)
		if fmt_err != OK:
			return fmt_err

		# L8/LA8 have no RD sRGB/luminance swizzle in .dstex; RGB8 needs an alpha
		# channel for the alpha options. R8/RG8 are exact and stay as-is.
		match img.get_format():
			Image.FORMAT_L8, Image.FORMAT_LA8, Image.FORMAT_RGB8:
				img.convert(Image.FORMAT_RGBA8)

		# Bound the source before mip generation and compression.
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
		# the written dimensions agree. The pre-align size is the logical
		# (authoring) size; the post-compression size is the storage size. When
		# they differ the writer emits a v3 file so the loader can size-override
		# back to the logical dimensions.
		logical_width = img.get_width()
		logical_height = img.get_height()
		if compression_mode == 0:
			var aligned_width := img.get_width()
			var aligned_height := img.get_height()
			var target_width := aligned_width if aligned_width <= 2 else (aligned_width + 3) & ~3
			var target_height := aligned_height if aligned_height <= 2 else (aligned_height + 3) & ~3
			if target_width != aligned_width or target_height != aligned_height:
				img.resize(target_width, target_height, Image.INTERPOLATE_CUBIC)
				push_warning("[DirectStorage Importer] Asset '%s' dimensions %dx%d are not a multiple of 4; block compression stores them as %dx%d. GPU-decoded textures report the padded %dx%d size (Texture2DRD has no size setter); enable compatibility/as_image_texture to load it as a %dx%d ImageTexture." % [source_file, aligned_width, aligned_height, target_width, target_height, target_width, target_height, aligned_width, aligned_height])

		# Godot remaps channels right after the size-limit resize; do the same so
		# packed PBR masks and normal-map swizzles apply before normalization.
		ClassDB.class_call_static("DSTex", "remap_channels", img, remap_r, remap_g, remap_b, remap_a)

		# Godot normalizes the base level only after a size-limit resize; do it for
		# every normal map so the source normals are unit length before mips.
		if is_normal_map:
			ClassDB.class_call_static("DSTex", "normalize_image", img)

		if fix_alpha_border:
			img.fix_alpha_edges()
		if premult_alpha:
			img.premultiply_alpha()

		if generate_mipmaps:
			img.generate_mipmaps(is_normal_map)
		else:
			img.clear_mipmaps()

		if compression_mode == 0:
			var requested_codec := "BPTC" if high_quality else "S3TC"
			var compressed := false
			if high_quality:
				var bptc_error := img.compress(Image.COMPRESS_BPTC, compression_source)
				var bptc_formats := [Image.FORMAT_BPTC_RGBA, Image.FORMAT_BPTC_RGBF, Image.FORMAT_BPTC_RGBFU]
				compressed = bptc_error == OK and img.is_compressed() and bptc_formats.has(img.get_format())
				if compressed:
					effective_codec = "BPTC"
			if not compressed:
				var s3tc_error := img.compress(Image.COMPRESS_S3TC, compression_source)
				var s3tc_formats := [Image.FORMAT_DXT1, Image.FORMAT_DXT3, Image.FORMAT_DXT5, Image.FORMAT_RGTC_R, Image.FORMAT_RGTC_RG]
				compressed = s3tc_error == OK and img.is_compressed() and s3tc_formats.has(img.get_format())
				if compressed:
					effective_codec = "S3TC"
			if not compressed:
				if img.get_format() != Image.FORMAT_RGBA8 and img.get_format() != Image.FORMAT_RGBH:
					img.convert(Image.FORMAT_RGBA8)
				effective_codec = "Uncompressed"
				push_warning("[DirectStorage Importer] Asset '%s': requested %s compression failed (codec unavailable or unsupported); wrote uncompressed %s." % [source_file, requested_codec, "RGBH" if img.get_format() == Image.FORMAT_RGBH else "RGBA8"])
			elif effective_codec != requested_codec:
				push_warning("[DirectStorage Importer] Asset '%s': requested %s compression unavailable; wrote %s." % [source_file, requested_codec, effective_codec])

	# Hand write_to_file() the natural mip chain: it builds the 256-byte row-pitch
	# footprint itself (one memcpy per row into a zero-filled buffer), so padding
	# here would only duplicate that work in GDScript.
	var raw_data := img.get_data()
	var width := img.get_width()
	var height := img.get_height()
	var format := img.get_format()
	var mip_count: int = (img.get_mipmap_count() + 1) if img.has_mipmaps() else 1
	var disk_compression: int = int(options.get("compression/disk_compression", 1))
	var gdeflate_level: int = int(options.get("compression/gdeflate_level", options.get("compression/level", 1)))
	var effective_compression_level: int = gdeflate_level if disk_compression == 1 else 0
	var chunk_size_kb: int = int(options.get("compression/chunk_size_kb", 8192))

	var is_srgb: bool = int(color_options["color_space"]) == COLOR_SPACE_SRGB
	var as_image_texture: bool = bool(options.get("compatibility/as_image_texture", false))
	var out_file_path := save_path + "." + _get_save_extension()
	# The codec may round dimensions itself; logical dims never exceed storage.
	var v3_logical_width := mini(logical_width, width)
	var v3_logical_height := mini(logical_height, height)
	var write_err: Error = dstex.write_to_file(out_file_path, raw_data, width, height, format, mip_count, effective_compression_level, is_srgb, as_image_texture, v3_logical_width, v3_logical_height, 0, chunk_size_kb)
	if write_err != OK:
		push_error("[DirectStorage Importer] Failed to write .dstex file to: " + out_file_path)
		return write_err

	# Staging is per *request*, not per texture. The Windows backend splits a
	# GDeflate chain into per-mip requests (or per-chunk for chunked payloads),
	# and any request whose compressed or uncompressed data exceeds the staging
	# buffer fails with E_DSTORAGE_REQUEST_TOO_LARGE. read_header() reports the
	# exact floor for this file, so the warning can name the value to configure
	# instead of the old whole-chain estimate (which over-warned on every
	# multi-mip texture that the runtime splits anyway).
	var configured_staging_mb: float = float(ProjectSettings.get_setting("dsgd_extension/staging_buffer_size_mb", 32.0))
	var staging_mb: float = configured_staging_mb if configured_staging_mb > 0.0 else 128.0
	var header: Dictionary = dstex.read_header(out_file_path)
	if not header.is_empty():
		var required_bytes: int = int(header.get("min_staging_bytes", 0))
		if required_bytes > 0 and float(required_bytes) > staging_mb * 1048576.0 and not _staging_warnings.has(source_file):
			_staging_warnings[source_file] = true
			var required_mb := ceili(float(required_bytes) / 1048576.0)
			push_warning("[DirectStorage Importer] Asset '" + source_file + "' needs at least " + str(required_mb) + " MiB of DirectStorage staging memory (its largest single request) but dsgd_extension/staging_buffer_size_mb is " + str(staging_mb) + " MiB. DirectStorage/D3D12 loads of this asset will fail until that setting is at least " + str(required_mb) + " MiB or the texture is made smaller; non-Windows and CPU-fallback loads are unaffected.")

	gen_files.append(out_file_path)
	print("[DirectStorage Importer] Imported ", source_file, " -> ", out_file_path, " (Mode: ", effective_codec, ", GDeflate Level: ", gdeflate_level, ", As ImageTexture: ", as_image_texture, ")")
	return OK
