@tool
extends SceneTree

var failures: int = 0
var cleanup_paths: Array[String] = []

func _init() -> void:
	call_deferred("_run")

func _run() -> void:
	var raw_data := PackedByteArray()
	for i in range(64 * 64 * 4):
		raw_data.append((i * 17 + 3) % 256)

	var png_path := "res://test_assets/test.png"
	var jpg_path := "res://test_assets/loader_contract.jpg"
	var dstex_path := "user://loader_contract.dstex"
	var archive_path := "res://textures.dspkg"
	var virtual_path := "res://packaged_test_texture.png"
	cleanup_paths = [jpg_path, dstex_path, archive_path]

	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	image.set_data(64, 64, false, Image.FORMAT_RGBA8, raw_data)
	_check(image.save_jpg(jpg_path) == OK, "created loose JPG fixture")

	var dstex = ClassDB.instantiate("DSTex")
	_check(dstex != null, "DSTex class is available")
	if dstex == null:
		_finish()
		return
	_check(dstex.write_to_file(dstex_path, raw_data, 64, 64, Image.FORMAT_RGBA8, 1, 1) == OK, "created DSTex fixture")

	var png_texture = ResourceLoader.load(png_path)
	_check(png_texture is Texture2D, "loose PNG uses the built-in texture loader")
	var jpg_image = Image.load_from_file(jpg_path)
	_check(jpg_image != null and not jpg_image.is_empty(), "loose JPG is readable by Godot's built-in image codec")

	var cache = Engine.get_singleton("DSTextureCache")
	_check(cache != null, "DSTextureCache singleton is available")
	if cache == null:
		_finish()
		return

	cache.clear()
	var ignored_texture = ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_IGNORE)
	_check(ignored_texture is Texture2D, "CACHE_MODE_IGNORE loads a DSTex texture")
	_check(cache.get_cached_count() == 0, "CACHE_MODE_IGNORE does not populate DSTextureCache")

	var reused_texture = ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	_check(reused_texture is Texture2D, "CACHE_MODE_REUSE loads a DSTex texture")
	_check(cache.get_cached_count() == 1, "CACHE_MODE_REUSE populates DSTextureCache")
	var reused_texture_again = ResourceLoader.load(dstex_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	_check(reused_texture_again == reused_texture, "repeated CACHE_MODE_REUSE load returns the cached texture")

	var package = ClassDB.instantiate("DSPkg")
	_check(package != null, "DSPkg class is available")
	if package == null:
		_finish()
		return
	_check(package.create(archive_path) == OK, "created DSPkg fixture")
	_check(package.add_texture(virtual_path, dstex_path) == OK, "added virtual texture entry to DSPkg")
	_check(package.finalize() == OK, "finalized DSPkg fixture")

	_check(ResourceLoader.exists(virtual_path), "virtual texture path resolves through DSPkg")
	_check(ResourceLoader.exists(virtual_path), "repeated virtual texture exists query succeeds")
	var virtual_texture = ResourceLoader.load(virtual_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	_check(virtual_texture is Texture2D, "virtual DSPkg texture loads as Texture2D")
	var virtual_texture_again = ResourceLoader.load(virtual_path, "Texture2D", ResourceLoader.CACHE_MODE_REUSE)
	_check(virtual_texture_again == virtual_texture, "repeated virtual texture load reuses the cached texture")

	_finish()

func _check(condition: bool, message: String) -> void:
	if condition:
		print("[PASS] ", message)
	else:
		failures += 1
		printerr("[FAIL] ", message)

func _finish() -> void:
	for path in cleanup_paths:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))
	print("[Loader Contract] result=", "FAIL" if failures > 0 else "PASS", " failures=", failures)
	quit(1 if failures > 0 else 0)
