@tool
extends Node

## Editor-context verification for the DirectStorage importer parity options.
##
## Run with:
##   godot --headless --editor --path project res://test_scenes/test_importer_options.tscn --quit-after 400
##
## The scene quits itself with 0 (pass) or 1 (fail). It uses the importer's
## public EditorImportPlugin surface and writes fixtures under user://.

const PROFILE_SETTING := "directstorage/import/default_profile"
const TMP_DIR := "user://importer_parity_test"

var _failures := 0
var _checks := 0
var _importer: ResourceImporterDirectStorageTexture = null
var _cleanup: Array[String] = []


func _check(condition: bool, message: String) -> void:
	_checks += 1
	if condition:
		print("[ImporterParity] PASS ", message)
	else:
		_failures += 1
		printerr("[ImporterParity] FAIL ", message)


func _ready() -> void:
	var watchdog := get_tree().create_timer(60.0)
	watchdog.timeout.connect(func() -> void:
		printerr("[ImporterParity] TIMEOUT after 60 s")
		get_tree().quit(2)
	)

	_run()
	_cleanup_files()
	print("[ImporterParity] result=", "FAIL" if _failures > 0 else "PASS", " checks=", _checks, " failures=", _failures)
	get_tree().quit(1 if _failures > 0 else 0)


func _run() -> void:
	if not ClassDB.class_exists("DSTex"):
		_check(false, "DSTex class is registered")
		return
	_importer = ResourceImporterDirectStorageTexture.new()

	var opts_3d: Array = _importer._get_import_options("", 1)
	var opts_2d: Array = _importer._get_import_options("", 2)
	var d3d := _defaults(opts_3d)
	var d2d := _defaults(opts_2d)

	_check(_importer._get_format_version() == 5, "importer format version is 5")
	_check(_importer._get_preset_count() == 3, "preset count is 3")
	_check(_importer._get_preset_name(1) == "3D (Streaming)", "preset 1 is 3D (Streaming)")
	_check(_importer._get_preset_name(2) == "CPU / ImageTexture (2D, UI, Decals)", "preset 2 is CPU/ImageTexture")
	_check(d3d.has("compression/color_space") and not d3d.has("compression/source"), "split compression/source into color options")
	_check(int(d3d["compression/color_space"]) == 1, "color space defaults to sRGB")
	_check(int(d3d["compress/normal_map"]) == 0, "normal map processing defaults to disabled")
	_check(int(d3d["compress/channel_pack"]) == 0, "channel packing defaults to sRGB friendly")
	_check(int(d3d["process/channel_remap/red"]) == 0, "channel remap red defaults to Red")
	_check(int(d3d["process/channel_remap/green"]) == 1, "channel remap green defaults to Green")
	_check(int(d3d["process/channel_remap/blue"]) == 2, "channel remap blue defaults to Blue")
	_check(int(d3d["process/channel_remap/alpha"]) == 3, "channel remap alpha defaults to Alpha")
	_check(int(d3d["compression/mode"]) == 0, "3D preset compresses")
	_check(bool(d3d["mipmaps/generate"]) == true, "3D preset generates mipmaps")
	_check(bool(d3d["process/fix_alpha_border"]) == false, "3D preset leaves alpha borders alone")
	_check(bool(d3d["compatibility/as_image_texture"]) == false, "3D preset uses the GPU/DRD path")
	_check(int(d2d["compression/mode"]) == 1, "2D preset is uncompressed")
	_check(bool(d2d["mipmaps/generate"]) == false, "2D preset skips mipmaps")
	_check(bool(d2d["process/fix_alpha_border"]) == true, "2D preset fixes alpha borders")
	_check(bool(d2d["compatibility/as_image_texture"]) == true, "2D preset forces ImageTexture")

	_check(_importer._get_option_visibility("", &"compression/high_quality", {"compression/mode": 0}), "high_quality visible in compressed mode")
	_check(not _importer._get_option_visibility("", &"compression/high_quality", {"compression/mode": 1}), "high_quality hidden in uncompressed mode")
	_check(_importer._get_option_visibility("", &"compress/channel_pack", {"compression/mode": 0}), "channel_pack visible in compressed mode")
	_check(not _importer._get_option_visibility("", &"compress/channel_pack", {"compression/mode": 1}), "channel_pack hidden in uncompressed mode")
	_check(_importer._get_option_visibility("", &"compress/normal_map", {"compression/mode": 1}), "normal_map stays visible when uncompressed")

	var mode_usage := 0
	for option in opts_3d:
		if String(option["name"]) == "compression/mode":
			mode_usage = int(option.get("usage", 0))
	_check((mode_usage & PROPERTY_USAGE_DEFAULT) != 0, "compression/mode keeps PROPERTY_USAGE_DEFAULT")
	_check((mode_usage & PROPERTY_USAGE_UPDATE_ALL_IF_MODIFIED) != 0, "compression/mode refreshes the dock on change")

	# Project profile: index 0 follows the setting; 3D/2D presets stay explicit.
	var had_profile := ProjectSettings.has_setting(PROFILE_SETTING)
	var previous_profile = ProjectSettings.get_setting(PROFILE_SETTING) if had_profile else null
	ProjectSettings.set_setting(PROFILE_SETTING, 1)
	_check(_importer._get_preset_name(0) == "Project Default (2D)", "profile 1 names preset 0 as 2D")
	_check(int(_defaults(_importer._get_import_options("", 0))["compression/mode"]) == 1, "profile 1 resolves preset 0 to the 2D defaults")
	ProjectSettings.set_setting(PROFILE_SETTING, 0)
	_check(_importer._get_preset_name(0) == "Project Default (3D)", "profile 0 names preset 0 as 3D")
	_check(int(_defaults(_importer._get_import_options("", 0))["compression/mode"]) == 0, "profile 0 resolves preset 0 to the 3D defaults")
	if had_profile:
		ProjectSettings.set_setting(PROFILE_SETTING, previous_profile)
	else:
		ProjectSettings.clear(PROFILE_SETTING)

	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(TMP_DIR))

	var rgba_fixture := TMP_DIR + "/rgba.png"
	_make_png(rgba_fixture, 128, 128, Image.FORMAT_RGBA8, false)
	_cleanup.append(rgba_fixture)

	var bc7_options := _with(d3d, {
		"compression/high_quality": true,
		"compression/color_space": 1,
		"mipmaps/generate": true,
	})
	var bc7 := _import_fixture(rgba_fixture, "bc7", bc7_options)
	if not bc7.is_empty():
		var header: Dictionary = bc7["header"]
		_check(int(header["pixel_format"]) == RenderingDevice.DATA_FORMAT_BC7_SRGB_BLOCK, "high_quality writes BC7 sRGB")
		_check(int(header["mip_count"]) > 1, "mipmaps/generate writes a full chain")
		_check(int(header["version"]) == 2, "aligned dims keep the v2 container")
		_check(int(header["logical_width"]) == 128 and int(header["logical_height"]) == 128, "aligned dims report storage as logical")

	var no_mips := _import_fixture(rgba_fixture, "no_mips", _with(bc7_options, {"mipmaps/generate": false}))
	if not no_mips.is_empty():
		_check(int(no_mips["header"]["mip_count"]) == 1, "mipmaps/generate=false writes one mip")

	var limited := _import_fixture(rgba_fixture, "limited", _with(d3d, {
		"compression/mode": 1,
		"process/size_limit": 64,
	}))
	if not limited.is_empty():
		_check(int(limited["header"]["width"]) == 64, "size_limit downsizes the width")
		_check(int(limited["header"]["height"]) == 64, "size_limit downsizes the height")

	var l8_fixture := TMP_DIR + "/l8.png"
	_make_png(l8_fixture, 32, 32, Image.FORMAT_L8, false)
	_cleanup.append(l8_fixture)
	var l8 := _import_fixture(l8_fixture, "l8", _with(d2d, {"compression/color_space": 1}))
	if not l8.is_empty():
		var format := int(l8["header"]["pixel_format"])
		_check(format == RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM or format == RenderingDevice.DATA_FORMAT_R8G8B8A8_SRGB, "L8 becomes RGBA8 (not R8)")

	var la8_fixture := TMP_DIR + "/la8.png"
	_make_png(la8_fixture, 32, 32, Image.FORMAT_LA8, true)
	_cleanup.append(la8_fixture)
	var la8 := _import_fixture(la8_fixture, "la8", _with(d2d, {"compression/color_space": 1}))
	if not la8.is_empty():
		var format := int(la8["header"]["pixel_format"])
		_check(format == RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM or format == RenderingDevice.DATA_FORMAT_R8G8B8A8_SRGB, "LA8 becomes RGBA8 (alpha preserved)")

	# Channel remap: swap red and blue on an uncompressed import, then decode.
	# The gradient fixture has r=0.0, b=0.25 at (0,0); swapped it is r=0.25, b=0.0.
	var remap_fixture := TMP_DIR + "/remap.png"
	_make_png(remap_fixture, 4, 4, Image.FORMAT_RGBA8, false)
	_cleanup.append(remap_fixture)
	var remapped := _import_fixture(remap_fixture, "remap", _with(d2d, {
		"process/channel_remap/red": 2,
		"process/channel_remap/blue": 0,
		"mipmaps/generate": false,
	}))
	if not remapped.is_empty():
		var remap_image := _decode_uncompressed(remapped, 4, 4)
		_check(remap_image != null and not remap_image.is_empty(), "remapped payload decodes")
		if remap_image != null and not remap_image.is_empty():
			var pixel := remap_image.get_pixel(0, 0)
			_check(absf(pixel.r - 0.25) < 0.02 and absf(pixel.b - 0.0) < 0.02, "channel remap swaps red and blue")

	var normal_fixture := TMP_DIR + "/normal.png"
	_make_normal_png(normal_fixture, 8, 8)
	_cleanup.append(normal_fixture)
	var normal := _import_fixture(normal_fixture, "normal", _with(d3d, {
		"compression/high_quality": true,
		"compress/normal_map": 1,
		"compression/color_space": 0,
		"mipmaps/generate": false,
	}))
	if not normal.is_empty():
		_check(bool(normal["header"]["is_srgb"]) == false, "normal map writes a linear header")
		check_normal_base(normal)

	# NPOT parity: a non-multiple-of-4 source stores aligned dims but loads
	# at its logical (authoring) dims via texture_set_size_override.
	var npot_fixture := TMP_DIR + "/npot.png"
	_make_png(npot_fixture, 100, 50, Image.FORMAT_RGBA8, false)
	_cleanup.append(npot_fixture)
	var npot := _import_fixture(npot_fixture, "npot", _with(d3d, {
		"compression/mode": 0,
		"mipmaps/generate": false,
	}))
	if not npot.is_empty():
		var npot_header: Dictionary = npot["header"]
		_check(int(npot_header["version"]) == 3, "NPOT import writes v3")
		_check(int(npot_header["width"]) == 100 and int(npot_header["height"]) == 52, "NPOT stores aligned dims")
		_check(int(npot_header["logical_width"]) == 100 and int(npot_header["logical_height"]) == 50, "NPOT records logical dims")
		var npot_tex: Texture2D = load(npot["path"])
		_check(npot_tex != null, "NPOT texture loads")
		if npot_tex != null:
			_check(npot_tex.get_width() == 100 and npot_tex.get_height() == 50, "NPOT loads at logical dims")

	# Legacy shim: an .import file that only stores compression/source keeps
	# the old channel behavior instead of falling back to the split defaults.
	var legacy_fixture := TMP_DIR + "/legacy.png"
	_make_normal_png(legacy_fixture, 8, 8)
	_cleanup.append(legacy_fixture)
	_cleanup.append(legacy_fixture + ".import")

	var legacy_options := _with(d3d, {
		"compression/high_quality": true,
		"compression/source": Image.COMPRESS_SOURCE_NORMAL,
		"compression/color_space": 1,
		"compress/normal_map": 0,
		"compress/channel_pack": 0,
		"mipmaps/generate": false,
	})
	_write_import_params(legacy_fixture, {"compression/source": Image.COMPRESS_SOURCE_NORMAL})
	var legacy_resolved: Dictionary = _importer._resolve_color_options(legacy_fixture, legacy_options)
	_check(bool(legacy_resolved["normal_map"]) == true, "legacy Normal Map shim enables normal processing")
	_check(int(legacy_resolved["color_space"]) == 0, "legacy Normal Map shim stores linear")
	_check(int(legacy_resolved["compress_source"]) == Image.COMPRESS_SOURCE_NORMAL, "legacy Normal Map shim compresses as NORMAL")
	var legacy := _import_fixture(legacy_fixture, "legacy_normal", legacy_options)
	if not legacy.is_empty():
		_check(bool(legacy["header"]["is_srgb"]) == false, "legacy Normal Map shim writes a linear header")
		check_normal_base(legacy)

	# Once any split key exists on disk, the stored new options win over the shim.
	_write_import_params(legacy_fixture, {
		"compression/source": Image.COMPRESS_SOURCE_NORMAL,
		"compress/normal_map": 0,
	})
	var migrated_resolved: Dictionary = _importer._resolve_color_options(legacy_fixture, legacy_options)
	_check(bool(migrated_resolved["normal_map"]) == false, "new keys override the legacy shim")
	_check(int(migrated_resolved["color_space"]) == 1, "new keys keep the stored color space")
	_check(int(migrated_resolved["compress_source"]) == Image.COMPRESS_SOURCE_SRGB, "new channel pack selects the sRGB source")


func _defaults(options: Array) -> Dictionary:
	var result := {}
	for option in options:
		result[String(option["name"])] = option["default_value"]
	return result


func _with(base: Dictionary, overrides: Dictionary) -> Dictionary:
	var result := base.duplicate()
	for key in overrides:
		result[key] = overrides[key]
	return result


func _write_import_params(source: String, params: Dictionary) -> void:
	var config := ConfigFile.new()
	config.set_value("remap", "importer", "directstorage_texture")
	for key in params:
		config.set_value("params", key, params[key])
	var error := config.save(source + ".import")
	_check(error == OK, "import params written: " + source + ".import")


func _make_png(path: String, width: int, height: int, format: int, gradient_alpha: bool) -> void:
	var image := Image.create(width, height, false, format)
	for y in height:
		for x in width:
			var luminance := float(x) / float(maxi(1, width - 1))
			var color := Color(luminance, float(y) / float(maxi(1, height - 1)), 0.25, 1.0)
			if gradient_alpha:
				color.a = 0.0 if (x + y) % 3 == 0 else 1.0
			image.set_pixel(x, y, color)
	var error := image.save_png(path)
	_check(error == OK, "fixture written: " + path)


func _make_normal_png(path: String, width: int, height: int) -> void:
	var image := Image.create(width, height, false, Image.FORMAT_RGB8)
	for y in height:
		for x in width:
			# Deliberately non-unit vectors in tangent space; the importer must
			# normalize the base level before compression.
			image.set_pixel(x, y, Color(0.9, 0.2, 0.3))
	var error := image.save_png(path)
	_check(error == OK, "normal fixture written: " + path)


func _import_fixture(source: String, tag: String, options: Dictionary) -> Dictionary:
	var save_base := TMP_DIR + "/" + tag
	var generated: Array[String] = []
	var error: Error = _importer._import(source, save_base, options, [], generated)
	_check(error == OK, "import %s returns OK (%s)" % [tag, error_string(error)])
	if error != OK or generated.is_empty():
		return {}
	_cleanup.append(generated[0])
	var dstex = ClassDB.instantiate("DSTex")
	var header: Dictionary = dstex.read_header(generated[0])
	_check(not header.is_empty(), "import %s header readable" % tag)
	if header.is_empty():
		return {}
	header["_path"] = generated[0]
	return {"header": header, "path": generated[0]}


func _decode_uncompressed(result: Dictionary, width: int, height: int) -> Image:
	# Decodes a single-mip uncompressed RGBA8 payload: decompress the GDeflate
	# mip, unpad the 256-byte footprint rows, and rebuild the natural image.
	var header: Dictionary = result["header"]
	var path: String = result["path"]
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty() or not header.has("mip_offsets"):
		return null
	var offset := int(header["mip_offsets"][0])
	var size := int(header["mip_sizes"][0])
	var uncompressed := int(header["mip_uncompressed_sizes"][0])
	var row_pitch := int(header["mip_row_pitches"][0])
	var payload := bytes.slice(offset, offset + size)
	var decompressor = ClassDB.instantiate("GDeflateDecompressor")
	var footprint: PackedByteArray = decompressor.decompress(payload, uncompressed)
	var row_bytes := width * 4
	var row_count := height
	if footprint.size() != uncompressed or row_pitch < row_bytes:
		return null
	var natural := PackedByteArray()
	natural.resize(row_bytes * row_count)
	for row in row_count:
		for byte in row_bytes:
			natural[row * row_bytes + byte] = footprint[row * row_pitch + byte]
	return Image.create_from_data(width, height, false, Image.FORMAT_RGBA8, natural)


func check_normal_base(result: Dictionary) -> void:
	var header: Dictionary = result["header"]
	var path: String = result["path"]
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty() or not header.has("mip_offsets"):
		_check(false, "normal map payload readable")
		return
	var offset := int(header["mip_offsets"][0])
	var size := int(header["mip_sizes"][0])
	var uncompressed := int(header["mip_uncompressed_sizes"][0])
	var payload := bytes.slice(offset, offset + size)
	var decompressor = ClassDB.instantiate("GDeflateDecompressor")
	var footprint: PackedByteArray = decompressor.decompress(payload, uncompressed)
	# The payload is the 256-byte-pitch footprint; rebuild the natural block rows
	# for Image.create_from_data (unpad_footprint_mip is not a bound method).
	var row_bytes := maxi(1, ceili(8.0 / 4.0)) * 16
	var row_count := maxi(1, ceili(8.0 / 4.0))
	var row_pitch := int(float(uncompressed) / float(row_count))
	if footprint.size() != uncompressed or row_pitch < row_bytes:
		_check(false, "normal map footprint shape (%)" % footprint.size())
		return
	var natural := PackedByteArray()
	natural.resize(row_bytes * row_count)
	for row in row_count:
		for byte in row_bytes:
			natural[row * row_bytes + byte] = footprint[row * row_pitch + byte]
	var image := Image.create_from_data(8, 8, false, Image.FORMAT_BPTC_RGBA, natural)
	_check(image != null and not image.is_empty(), "normal map base decodes")
	if image == null or image.is_empty():
		return
	image.decompress()
	var color := image.get_pixel(4, 4)
	var normal := Vector3(color.r * 2.0 - 1.0, color.g * 2.0 - 1.0, color.b * 2.0 - 1.0)
	_check(absf(normal.length() - 1.0) < 0.05, "normal map base is unit length (%.3f)" % normal.length())


func _cleanup_files() -> void:
	for path in _cleanup:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	var directory := ProjectSettings.globalize_path(TMP_DIR)
	if DirAccess.dir_exists_absolute(directory):
		DirAccess.remove_absolute(directory)
