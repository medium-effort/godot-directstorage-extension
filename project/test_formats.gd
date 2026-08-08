@tool
extends SceneTree

func _init() -> void:
	print("--- Running DirectStorage Phase 2 Step 2 Formats Test ---")

	# Test 1: DSTex write & read
	var raw_data = PackedByteArray()
	for i in range(256 * 256 * 4):
		raw_data.append(i % 256)

	var dstex = DSTex.new()
	var test_dstex_path = "user://test_texture.dstex"
	var err = dstex.write_to_file(test_dstex_path, raw_data, 256, 256, Image.FORMAT_RGBA8, 1, 1)
	if err != OK:
		print("FAIL: DSTex write_to_file returned error: ", err)
		quit(1)
		return
	print("SUCCESS: DSTex write_to_file completed.")

	var read_dict = dstex.read_from_file(test_dstex_path)
	if not read_dict.has("header") or not read_dict.has("compressed_data"):
		print("FAIL: DSTex read_from_file returned invalid data.")
		quit(1)
		return
	var header = read_dict["header"]
	print("DSTex Header: ", header)
	if header["width"] != 256 or header["height"] != 256:
		print("FAIL: DSTex header dimensions mismatch.")
		quit(1)
		return

	var decompressor = GDeflateDecompressor.new()
	var decompressed = decompressor.decompress(read_dict["compressed_data"], header["original_size"])
	if decompressed.size() != raw_data.size():
		print("FAIL: Decompressed size mismatch: ", decompressed.size(), " vs ", raw_data.size())
		quit(1)
		return
	if decompressed != raw_data:
		print("FAIL: Decompressed bytes do not match original raw bytes!")
		quit(1)
		return
	print("SUCCESS: DSTex compression/decompression byte equality verified.")

	# Test 2: DSPkg create, add_texture, finalize & open/read
	var dspkg = DSPkg.new()
	var test_dspkg_path = "user://test_archive.dspkg"
	err = dspkg.create(test_dspkg_path)
	if err != OK:
		print("FAIL: DSPkg create failed.")
		quit(1)
		return

	err = dspkg.add_texture("res://textures/test_texture.png", test_dstex_path)
	if err != OK:
		print("FAIL: DSPkg add_texture failed.")
		quit(1)
		return

	err = dspkg.finalize()
	if err != OK:
		print("FAIL: DSPkg finalize failed.")
		quit(1)
		return
	print("SUCCESS: DSPkg package created.")

	var dspkg_reader = DSPkg.new()
	err = dspkg_reader.open(test_dspkg_path)
	if err != OK:
		print("FAIL: DSPkg open failed.")
		quit(1)
		return

	if not dspkg_reader.has_texture("res://textures/test_texture.png"):
		print("FAIL: DSPkg missing expected entry.")
		quit(1)
		return

	var entry_dict = dspkg_reader.read_texture("res://textures/test_texture.png")
	if not entry_dict.has("compressed_data"):
		print("FAIL: DSPkg read_texture returned no data.")
		quit(1)
		return

	print("DSPkg Entry metadata: ", entry_dict)
	if entry_dict["compressed_data"] != read_dict["compressed_data"]:
		print("FAIL: DSPkg compressed data does not match original .dstex data!")
		quit(1)
		return

	print("SUCCESS: All Phase 2 Step 2 tests passed successfully!")
	quit(0)
