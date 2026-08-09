@tool
extends SceneTree

func _init() -> void:
	print("--- Running DirectStorage Phase 2 Step 3 Integration Test ---")

	# Verify recognized extensions in ResourceLoader
	var tex_extensions = ResourceLoader.get_recognized_extensions_for_type("Texture2D")
	print("ResourceLoader Texture2D extensions: ", tex_extensions)
	if not ("dstex" in tex_extensions):
		print("FAIL: 'dstex' extension not found in ResourceLoader recognized extensions for Texture2D!")
		quit(1)
		return

	var res_extensions = ResourceLoader.get_recognized_extensions_for_type("Resource")
	print("ResourceLoader Resource extensions: ", res_extensions)
	if not ("dspkg" in res_extensions):
		print("FAIL: 'dspkg' extension not found in ResourceLoader recognized extensions for Resource!")
		quit(1)
		return

	print("SUCCESS: Registered ResourceFormatLoader recognizes .dstex for Texture2D and .dspkg for Resource.")

	# Initialize DirectStorage context backend
	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()
		var backend_name = ds_context.get_active_backend_name()
		print("DSContext active backend: ", backend_name)
		if backend_name != "Win32DirectStorage" and backend_name != "SoftwareFallback":
			print("FAIL: Unknown active backend name: ", backend_name)
			quit(1)
			return

		var is_supp = ds_context.is_supported()
		if is_supp != (backend_name == "Win32DirectStorage"):
			print("FAIL: is_supported() does not match active backend name! is_supported=", is_supp, ", backend=", backend_name)
			quit(1)
			return

		var is_d3d12 = ds_context.is_d3d12_backend()
		if backend_name == "SoftwareFallback" and is_d3d12:
			print("FAIL: is_d3d12_backend() returned true while using SoftwareFallback!")
			quit(1)
			return
		print("SUCCESS: DSContext backend selection contract verified (backend=", backend_name, ", is_supported=", is_supp, ", is_d3d12=", is_d3d12, ").")

	# 0. Test DSRequest uncompressed_size contract
	if ClassDB.class_exists("DSRequest"):
		var req_test = DSRequest.new()
		req_test.set_size(1024)
		req_test.set_uncompressed_size(4096)
		if req_test.get_size() != 1024 or req_test.get_uncompressed_size() != 4096:
			print("FAIL: DSRequest size and uncompressed_size property test failed!")
			quit(1)
			return
		print("SUCCESS: DSRequest size (1024) and uncompressed_size (4096) contract verified.")

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

	# Verify header compressed_size vs original_size separation
	var header = dstex.read_header(test_dstex_path)
	if header.is_empty():
		print("FAIL: Failed to read .dstex header.")
		quit(1)
		return
	print("DSTex Header: original_size=", header["original_size"], ", compressed_size=", header["compressed_size"])
	if header["original_size"] != 128 * 128 * 4:
		print("FAIL: Original size in header mismatch!")
		quit(1)
		return
	if header["compressed_size"] == 0 or header["compressed_size"] >= header["original_size"]:
		print("FAIL: DSTex fixture compressed_size (", header["compressed_size"], ") is not strictly smaller than original_size (", header["original_size"], ")!")
		quit(1)
		return
	print("SUCCESS: Verified DSTex compressed_size (", header["compressed_size"], ") != original_size (", header["original_size"], ")")

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

	# Verify Tier 1 Texture2DRD or ImageTexture fallback
	var rd = RenderingServer.get_rendering_device()
	if rd != null:
		if not (loaded_tex is Texture2DRD):
			print("FAIL: Loaded texture is not a Texture2DRD instance despite RenderingDevice active! Type: ", loaded_tex.get_class())
			quit(1)
			return
		print("SUCCESS: Confirmed Tier 1 Texture2DRD class instance on active RenderingDevice.")
	else:
		print("INFO: RenderingDevice inactive, confirmed ImageTexture fallback: ", loaded_tex.get_class())

	# Verify Sprite2D assignment
	var sprite = Sprite2D.new()
	sprite.texture = loaded_tex

	print("SUCCESS: Loaded .dstex as Texture2D: ", loaded_tex, " [", loaded_tex.get_width(), "x", loaded_tex.get_height(), "]")
	if loaded_tex.get_width() != 128 or loaded_tex.get_height() != 128:
		print("FAIL: Loaded texture dimensions mismatch: expected 128x128, got ", loaded_tex.get_width(), "x", loaded_tex.get_height())
		sprite.free()
		quit(1)
		return

	sprite.free()

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

	# 6. Test packaged texture load via DSContext / DSRequest pipeline
	var textures_pkg_path = "res://textures.dspkg"
	var pkg_builder = DSPkg.new()
	err = pkg_builder.create(textures_pkg_path)
	if err == OK:
		pkg_builder.add_texture("res://packaged_test_texture.png", test_dstex_path)
		pkg_builder.finalize()

		# Load contained texture by original resource path
		var loaded_packaged_tex = loader.load("res://packaged_test_texture.png")
		if loaded_packaged_tex == null or not (loaded_packaged_tex is Texture2D):
			print("FAIL: Failed to load packaged texture from res://textures.dspkg!")
			quit(1)
			return
		if rd != null:
			if not (loaded_packaged_tex is Texture2DRD):
				print("FAIL: Packaged texture is not Texture2DRD instance despite RenderingDevice active! Type: ", loaded_packaged_tex.get_class())
				quit(1)
				return
		if loaded_packaged_tex.get_width() != 128 or loaded_packaged_tex.get_height() != 128:
			print("FAIL: Packaged texture dimensions mismatch!")
			quit(1)
			return
		print("SUCCESS: Loaded packaged texture res://packaged_test_texture.png via DSRequest/DSContext pipeline: ", loaded_packaged_tex)

		# Clean up temporary archive file
		DirAccess.remove_absolute(textures_pkg_path)

	print("SUCCESS: All Phase 2 Step 3 integration tests passed successfully!")
	quit(0)
