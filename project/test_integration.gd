@tool
extends SceneTree

func _init() -> void:
	print("--- Running DirectStorage Phase 2 Step 3 Integration Test ---")

	# Verify recognized extensions in ResourceLoader
	var tex_extensions = ResourceLoader.get_recognized_extensions_for_type("Texture2D")
	print("ResourceLoader Texture2D extensions: ", tex_extensions)
	if not ("dstex" in tex_extensions):
		print("FAIL: 'dstex' extension not found in ResourceLoader recognized extensions!")
		quit(1)
		return
	if not ("dspkg" in tex_extensions):
		print("FAIL: 'dspkg' extension not found in ResourceLoader recognized extensions!")
		quit(1)
		return
	print("SUCCESS: Registered ResourceFormatLoader recognizes .dstex and .dspkg extensions.")

	# Initialize DirectStorage context backend
	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()
		print("DSContext active backend: ", ds_context.get_active_backend_name())

	# 1. Create synthetic raw texture data and save as .dstex file
	var raw_data = PackedByteArray()
	for i in range(128 * 128 * 4):
		raw_data.append((i * 7) % 256)

	var dstex = DSTex.new()
	var test_dstex_path = "user://test_integration.dstex"
	var err = dstex.write_to_file(test_dstex_path, raw_data, 128, 128, Image.FORMAT_RGBA8, 1, 1)
	if err != OK:
		print("FAIL: Failed to write test .dstex file: ", err)
		quit(1)
		return
	print("SUCCESS: Wrote test .dstex file to ", test_dstex_path)

	# 2. Test ResourceLoader.load() for .dstex file
	var loader = ResourceLoader
	var loaded_tex = loader.load(test_dstex_path)
	if loaded_tex == null:
		print("FAIL: ResourceLoader returned null for .dstex file!")
		quit(1)
		return

	if not (loaded_tex is Texture2D):
		print("FAIL: Loaded resource is not a Texture2D instance! Type: ", loaded_tex.get_class())
		quit(1)
		return

	print("SUCCESS: Loaded .dstex as Texture2D: ", loaded_tex, " [", loaded_tex.get_width(), "x", loaded_tex.get_height(), "]")
	if loaded_tex.get_width() != 128 or loaded_tex.get_height() != 128:
		print("FAIL: Loaded texture dimensions mismatch: expected 128x128, got ", loaded_tex.get_width(), "x", loaded_tex.get_height())
		quit(1)
		return

	# 3. Test DSTextureCache hit
	var cache = Engine.get_singleton("DSTextureCache")
	if cache == null:
		print("FAIL: DSTextureCache singleton not registered!")
		quit(1)
		return

	var cached_count = cache.get_cached_count()
	print("DSTextureCache cached count: ", cached_count)
	if cached_count != 1:
		print("FAIL: Expected 1 cached texture in DSTextureCache, found ", cached_count)
		quit(1)
		return

	# Second load should return cached reference
	var loaded_tex2 = loader.load(test_dstex_path)
	if loaded_tex2 != loaded_tex:
		print("FAIL: Second load did not return identical cached reference!")
		quit(1)
		return
	print("SUCCESS: DSTextureCache hit confirmed on second load.")

	# 4. Test cache clear
	cache.clear()
	if cache.get_cached_count() != 0:
		print("FAIL: DSTextureCache clear() failed to empty cache.")
		quit(1)
		return
	print("SUCCESS: DSTextureCache clear() verified.")

	# 5. Test .dspkg loading via ResourceFormatLoader
	var dspkg = DSPkg.new()
	var test_dspkg_path = "user://test_archive.dspkg"
	err = dspkg.create(test_dspkg_path)
	if err == OK:
		dspkg.add_texture("res://test_tex.png", test_dstex_path)
		dspkg.finalize()

		var loaded_dspkg = loader.load(test_dspkg_path)
		if loaded_dspkg == null or not (loaded_dspkg is DSPkg):
			print("FAIL: Failed to load .dspkg via ResourceLoader! Type: ", loaded_dspkg)
			quit(1)
			return
		print("SUCCESS: Loaded .dspkg resource via ResourceLoader: ", loaded_dspkg)

	print("SUCCESS: All Phase 2 Step 3 integration tests passed successfully!")
	quit(0)
