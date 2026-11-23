extends GutTest

const Helpers = preload("res://tests/unit/_gut_helpers.gd")

const TMP_DIR := "user://gut_tmp/loader_contract"
const ARCHIVE_PATH := "res://textures.dspkg"
const FORCE_ARCHIVE_SETTING := "dsgd_extension/force_archive_resolution"

var _had_force_archive := false
var _prev_force_archive: Variant = null


func before_each() -> void:
	Helpers.ensure_clean_dir(TMP_DIR)
	_had_force_archive = ProjectSettings.has_setting(FORCE_ARCHIVE_SETTING)
	_prev_force_archive = ProjectSettings.get_setting(FORCE_ARCHIVE_SETTING) if _had_force_archive else null
	ProjectSettings.set_setting(FORCE_ARCHIVE_SETTING, true)


func after_each() -> void:
	Helpers.restore_setting(FORCE_ARCHIVE_SETTING, _had_force_archive, _prev_force_archive)
	if Engine.has_singleton("DSTextureCache"):
		Engine.get_singleton("DSTextureCache").clear()
	if Engine.has_singleton("DSContext"):
		var ctx := Engine.get_singleton("DSContext")
		if ctx != null and ctx.is_initialized():
			ctx.shutdown_ds()
	var abs_archive := ProjectSettings.globalize_path(ARCHIVE_PATH)
	if FileAccess.file_exists(abs_archive):
		DirAccess.remove_absolute(abs_archive)
	Helpers.rm_dir_recursive(TMP_DIR)


func test_loader_cache_modes_and_dspkg_resolution() -> void:
	assert_true(ClassDB.class_exists("DSTex"), "DSTex class is available")
	assert_true(ClassDB.class_exists("DSPkg"), "DSPkg class is available")
	var cache := Engine.get_singleton("DSTextureCache")
	assert_not_null(cache, "DSTextureCache singleton is available")

	var raw_data := PackedByteArray()
	for i in range(64 * 64 * 4):
		raw_data.append((i * 17 + 3) % 256)

	var png_path := TMP_DIR.path_join("loader_contract.png")
	var jpg_path := TMP_DIR.path_join("loader_contract.jpg")
	var dstex_path := TMP_DIR.path_join("loader_contract.dstex")
	var virtual_path := "res://packaged_test_texture.png"

	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	image.set_data(64, 64, false, Image.FORMAT_RGBA8, raw_data)
	assert_eq(image.save_png(png_path), OK, "created loose PNG fixture")
	assert_eq(image.save_jpg(jpg_path), OK, "created loose JPG fixture")
	var png_image := Image.load_from_file(png_path)
	assert_true(png_image != null and not png_image.is_empty(), "loose PNG readable")
	var jpg_image := Image.load_from_file(jpg_path)
	assert_true(jpg_image != null and not jpg_image.is_empty(), "loose JPG readable")

	var dstex: Variant = ClassDB.instantiate("DSTex")
	assert_not_null(dstex)
	assert_eq(dstex.write_to_file(dstex_path, raw_data, 64, 64, Image.FORMAT_RGBA8, 1, 1), OK, "created DSTex fixture")

	cache.clear()
	var ignored_texture := ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_IGNORE)
	assert_true(ignored_texture is Texture2D, "CACHE_MODE_IGNORE loads DSTex")
	assert_eq(cache.get_cached_count(), 0, "CACHE_MODE_IGNORE does not populate DSTextureCache")

	var reused_texture := ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	assert_true(reused_texture is Texture2D, "CACHE_MODE_REUSE loads DSTex")
	assert_eq(cache.get_cached_count(), 1, "CACHE_MODE_REUSE populates DSTextureCache")
	var reused_again := ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	assert_eq(reused_again, reused_texture, "repeated CACHE_MODE_REUSE returns cached texture")

	var package: Variant = ClassDB.instantiate("DSPkg")
	assert_not_null(package)
	assert_eq(package.create(ProjectSettings.globalize_path(ARCHIVE_PATH)), OK)
	assert_eq(package.add_texture(virtual_path, dstex_path), OK)
	assert_eq(package.finalize(), OK)

	assert_true(ResourceLoader.exists(virtual_path), "virtual texture path resolves through DSPkg")
	assert_true(ResourceLoader.exists(virtual_path), "repeated virtual texture exists succeeds")
	var virtual_texture := ResourceLoader.load(virtual_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	assert_true(virtual_texture is Texture2D, "virtual DSPkg texture loads as Texture2D")
	var virtual_again := ResourceLoader.load(virtual_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	assert_eq(virtual_again, virtual_texture, "repeated virtual texture load reuses cached instance")
