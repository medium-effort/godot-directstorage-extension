extends GutTest

const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const PROFILE_SETTING := "dsgd_extension/default_import_profile"
const TMP_DIR := "user://gut_tmp/importer_parity"

var _had_profile := false
var _prev_profile: Variant = null


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP_DIR)
	_had_profile = ProjectSettings.has_setting(PROFILE_SETTING)
	_prev_profile = ProjectSettings.get_setting(PROFILE_SETTING) if _had_profile else null


func after_each() -> void:
	Helpers.restore_setting(PROFILE_SETTING, _had_profile, _prev_profile)
	Helpers.rm_dir_recursive(TMP_DIR)


func test_importer_presets_visibility_and_output() -> void:
	if not ClassDB.can_instantiate("EditorImportPlugin"):
		pending("EditorImportPlugin requires --editor context")
		return

	assert_true(ClassDB.class_exists("DSTex"), "DSTex class is registered")
	var importer := ResourceImporterDirectStorageTexture.new()
	var opts_3d: Array = importer._get_import_options("", 1)
	var opts_2d: Array = importer._get_import_options("", 2)
	var d3d := _defaults(opts_3d)
	var d2d := _defaults(opts_2d)

	assert_eq(importer._get_format_version(), 5)
	assert_eq(importer._get_preset_count(), 3)
	assert_eq(importer._get_preset_name(1), "3D (Streaming)")
	assert_eq(importer._get_preset_name(2), "CPU / ImageTexture (2D, UI, Decals)")
	assert_true(d3d.has("compression/color_space") and not d3d.has("compression/source"))
	assert_eq(int(d3d["compression/color_space"]), 1)
	assert_eq(int(d3d["compress/normal_map"]), 0)
	assert_eq(int(d3d["compress/channel_pack"]), 0)
	assert_eq(int(d3d["compression/mode"]), 0)
	assert_true(bool(d3d["mipmaps/generate"]))
	assert_false(bool(d3d["compatibility/as_image_texture"]))
	assert_eq(int(d2d["compression/mode"]), 1)
	assert_false(bool(d2d["mipmaps/generate"]))
	assert_true(bool(d2d["process/fix_alpha_border"]))
	assert_true(bool(d2d["compatibility/as_image_texture"]))

	assert_true(importer._get_option_visibility("", &"compression/high_quality", {"compression/mode": 0}))
	assert_false(importer._get_option_visibility("", &"compression/high_quality", {"compression/mode": 1}))
	assert_true(importer._get_option_visibility("", &"compress/channel_pack", {"compression/mode": 0}))
	assert_false(importer._get_option_visibility("", &"compress/channel_pack", {"compression/mode": 1}))
	assert_true(importer._get_option_visibility("", &"compress/normal_map", {"compression/mode": 1}))

	ProjectSettings.set_setting(PROFILE_SETTING, 1)
	assert_eq(importer._get_preset_name(0), "Project Default (2D)")
	assert_eq(int(_defaults(importer._get_import_options("", 0))["compression/mode"]), 1)
	ProjectSettings.set_setting(PROFILE_SETTING, 0)
	assert_eq(importer._get_preset_name(0), "Project Default (3D)")
	assert_eq(int(_defaults(importer._get_import_options("", 0))["compression/mode"]), 0)

	var rgba_fixture := TMP_DIR.path_join("rgba.png")
	_make_png(rgba_fixture, 128, 128, Image.FORMAT_RGBA8, false)
	var bc7 := _import_fixture(importer, rgba_fixture, "bc7", _with(d3d, {
		"compression/high_quality": true,
		"compression/color_space": 1,
		"mipmaps/generate": true,
	}))
	assert_eq(int(bc7["header"]["pixel_format"]), RenderingDevice.DATA_FORMAT_BC7_SRGB_BLOCK)
	assert_gt(int(bc7["header"]["mip_count"]), 1)
	assert_eq(int(bc7["header"]["version"]), 2)

	var npot_fixture := TMP_DIR.path_join("npot.png")
	_make_png(npot_fixture, 100, 50, Image.FORMAT_RGBA8, false)
	var npot := _import_fixture(importer, npot_fixture, "npot", _with(d3d, {
		"compression/mode": 0,
		"mipmaps/generate": false,
	}))
	assert_eq(int(npot["header"]["version"]), 3)
	assert_eq(int(npot["header"]["width"]), 100)
	assert_eq(int(npot["header"]["height"]), 52)
	assert_eq(int(npot["header"]["logical_width"]), 100)
	assert_eq(int(npot["header"]["logical_height"]), 50)
	var npot_tex: Texture2D = load(npot["path"])
	assert_not_null(npot_tex)
	assert_eq(npot_tex.get_width(), 100)
	assert_eq(npot_tex.get_height(), 50)


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


func _make_png(path: String, width: int, height: int, format: int, gradient_alpha: bool) -> void:
	var image := Image.create(width, height, false, format)
	for y in height:
		for x in width:
			var luminance := float(x) / float(maxi(1, width - 1))
			var color := Color(luminance, float(y) / float(maxi(1, height - 1)), 0.25, 1.0)
			if gradient_alpha:
				color.a = 0.0 if (x + y) % 3 == 0 else 1.0
			image.set_pixel(x, y, color)
	assert_eq(image.save_png(path), OK)


func _import_fixture(importer: ResourceImporterDirectStorageTexture, source: String, tag: String, options: Dictionary) -> Dictionary:
	var save_base := TMP_DIR.path_join(tag)
	var generated: Array[String] = []
	var error: Error = importer._import(source, save_base, options, [], generated)
	assert_eq(error, OK)
	assert_false(generated.is_empty())
	var dstex: Variant = ClassDB.instantiate("DSTex")
	var header: Dictionary = dstex.read_header(generated[0])
	assert_false(header.is_empty())
	return {"header": header, "path": generated[0]}
