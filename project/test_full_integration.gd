@tool
extends SceneTree

func _init() -> void:
	print("==========================================================")
	print("=== Starting Full DirectStorage Integration Test Pipeline ===")
	print("==========================================================")

	# 1. Verify GDExtension classes exist
	if not ClassDB.class_exists("DSContext") or not ClassDB.class_exists("DSTex") or not ClassDB.class_exists("DSPkg"):
		print("FAIL: Required DirectStorage GDExtension classes missing!")
		quit(1)
		return

	# 2. Check DSContext initialization
	var ctx = Engine.get_singleton("DSContext")
	if ctx:
		if not ctx.is_initialized():
			ctx.initialize_ds()
		print("SUCCESS: DSContext Initialized. Active Backend: ", ctx.get_active_backend_name())
	else:
		print("FAIL: DSContext singleton not found!")
		quit(1)
		return

	# 3. Create test PNG asset in res://test_assets/test.png
	var dir = DirAccess.open("res://")
	if not dir.dir_exists("test_assets"):
		dir.make_dir("test_assets")

	var img = Image.create(64, 64, false, Image.FORMAT_RGBA8)
	img.fill(Color.CORNFLOWER_BLUE)
	var img_path = "res://test_assets/test.png"
	var err = img.save_png(img_path)
	if err != OK:
		print("FAIL: Failed to save test PNG asset: ", err)
		quit(1)
		return
	print("SUCCESS: Created test image asset at ", img_path)

	# 4. Generate .dstex file using DSTex (underlying engine class for DSImporter)
	var dstex = DSTex.new()
	var dstex_output_path = "user://test_imported.dstex"
	err = dstex.write_to_file(dstex_output_path, img.get_data(), img.get_width(), img.get_height(), img.get_format(), 1, 1)
	if err != OK:
		print("FAIL: DSTex write_to_file failed with error: ", err)
		quit(1)
		return
	print("SUCCESS: DSTex generated binary texture at ", dstex_output_path)

	# 5. Load the generated .dstex via ResourceLoader
	var tex = ResourceLoader.load(dstex_output_path)
	if tex == null or not (tex is Texture2D):
		print("FAIL: ResourceLoader failed to load .dstex as Texture2D!")
		quit(1)
		return
	print("SUCCESS: Loaded .dstex as Texture2D: ", tex, " [", tex.get_width(), "x", tex.get_height(), "]")

	# 6. Verify DSTextureCache hit
	var cache = Engine.get_singleton("DSTextureCache")
	if cache == null:
		print("FAIL: DSTextureCache singleton missing!")
		quit(1)
		return

	var cached_count = cache.get_cached_count()
	print("DSTextureCache cached texture count: ", cached_count)
	if cached_count < 1:
		print("FAIL: DSTextureCache did not cache loaded texture!")
		quit(1)
		return

	var tex_cached = ResourceLoader.load(dstex_output_path)
	if tex_cached != tex:
		print("FAIL: DSTextureCache cache hit test failed! Reference mismatch.")
		quit(1)
		return
	print("SUCCESS: DSTextureCache hit confirmed on repeated ResourceLoader call.")

	# 7. Test DSPkg container creation (underlying engine class for DSExportPlugin)
	var dspkg_output_path = "user://test_exported_pack.dspkg"
	var dspkg = DSPkg.new()
	err = dspkg.create(dspkg_output_path)
	if err != OK:
		print("FAIL: Failed to create export .dspkg package: ", err)
		quit(1)
		return

	dspkg.add_texture(img_path, dstex_output_path)
	err = dspkg.finalize()
	if err != OK:
		print("FAIL: Failed to finalize export .dspkg package: ", err)
		quit(1)
		return
	print("SUCCESS: Exported .dspkg package created successfully at ", dspkg_output_path)

	# 8. Load package via ResourceLoader
	var loaded_pkg = ResourceLoader.load(dspkg_output_path)
	if loaded_pkg == null or not (loaded_pkg is DSPkg):
		print("FAIL: Failed to load exported .dspkg package via ResourceLoader!")
		quit(1)
		return
	print("SUCCESS: ResourceLoader successfully loaded exported .dspkg package: ", loaded_pkg)

	print("==========================================================")
	print("=== ALL FULL INTEGRATION TEST STEPS PASSED SUCCESSFULLY! ===")
	print("==========================================================")
	quit(0)

