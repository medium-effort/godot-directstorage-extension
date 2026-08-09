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

	var info_dict = dspkg_reader.get_entry_info("res://textures/test_texture.png")
	if not info_dict.has("archive_path") or not info_dict.has("data_offset"):
		print("FAIL: DSPkg get_entry_info returned invalid metadata dictionary.")
		quit(1)
		return
	if info_dict["compressed_size"] != read_dict["compressed_data"].size():
		print("FAIL: DSPkg get_entry_info compressed_size mismatch!")
		quit(1)
		return
	print("DSPkg Entry info metadata: ", info_dict)
	print("SUCCESS: DSPkg get_entry_info metadata lookup verified.")

	# Test 3: Malformed DSPkg Boundary Validation Test Suite
	print("--- Running DSPkg Boundary Validation Tests ---")

	var test_bad_path = "user://test_malformed.dspkg"

	# 1. Truncated Header (< 28 bytes)
	var truncated_bytes = PackedByteArray([0x44, 0x50, 0x4B, 0x47, 0x01, 0x00])
	var f_trunc = FileAccess.open(test_bad_path, FileAccess.WRITE)
	f_trunc.store_buffer(truncated_bytes)
	f_trunc.close()
	var reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Truncated header accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Truncated header correctly rejected.")

	# 2. Bad Version (version = 2)
	var bad_ver_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_ver_file.store_32(0x474B5044)
	bad_ver_file.store_16(2)
	bad_ver_file.store_16(0)
	bad_ver_file.store_32(0)
	bad_ver_file.store_64(28)
	bad_ver_file.store_64(0)
	bad_ver_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Bad version accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Bad version correctly rejected.")

	# 3. Invalid Flags (flags = 1)
	var bad_flags_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_flags_file.store_32(0x474B5044)
	bad_flags_file.store_16(1)
	bad_flags_file.store_16(1)
	bad_flags_file.store_32(0)
	bad_flags_file.store_64(28)
	bad_flags_file.store_64(0)
	bad_flags_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Non-zero flags accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Non-zero flags correctly rejected.")

	# 4. Oversized entry_count (entry_count = 100, but file ends after header)
	var bad_cnt_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_cnt_file.store_32(0x474B5044)
	bad_cnt_file.store_16(1)
	bad_cnt_file.store_16(0)
	bad_cnt_file.store_32(100)
	bad_cnt_file.store_64(5228)
	bad_cnt_file.store_64(0)
	bad_cnt_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Truncated entry count accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Truncated entry count correctly rejected.")

	# 5. data_offset before table end
	var bad_offset_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_offset_file.store_32(0x474B5044)
	bad_offset_file.store_16(1)
	bad_offset_file.store_16(0)
	bad_offset_file.store_32(1)
	bad_offset_file.store_64(20)
	bad_offset_file.store_64(0)
	bad_offset_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Invalid data_offset accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Invalid data_offset correctly rejected.")

	# 6. Payload range extends past EOF
	var bad_payload_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_payload_file.store_32(0x474B5044)
	bad_payload_file.store_16(1)
	bad_payload_file.store_16(0)
	bad_payload_file.store_32(1)
	var dummy_path = "res://test.png"
	var dummy_utf8 = dummy_path.to_utf8_buffer()
	var path_len = dummy_utf8.size()
	var data_off = 28 + 8 + 2 + path_len + 8 + 8 + 8 + 4 + 4 + 4 + 4
	bad_payload_file.store_64(data_off)
	bad_payload_file.store_64(0)
	bad_payload_file.store_64(dummy_path.hash())
	bad_payload_file.store_16(path_len)
	bad_payload_file.store_buffer(dummy_utf8)
	bad_payload_file.store_64(0)
	bad_payload_file.store_64(500)
	bad_payload_file.store_64(1000)
	bad_payload_file.store_32(256)
	bad_payload_file.store_32(256)
	bad_payload_file.store_32(Image.FORMAT_RGBA8)
	bad_payload_file.store_32(1)
	bad_payload_file.close()

	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Payload range past EOF accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Payload range past EOF correctly rejected.")

	# 7. Invalid zero dimensions (width = 0)
	var bad_dim_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_dim_file.store_32(0x474B5044)
	bad_dim_file.store_16(1)
	bad_dim_file.store_16(0)
	bad_dim_file.store_32(1)
	bad_dim_file.store_64(data_off)
	bad_dim_file.store_64(0)
	bad_dim_file.store_64(dummy_path.hash())
	bad_dim_file.store_16(path_len)
	bad_dim_file.store_buffer(dummy_utf8)
	bad_dim_file.store_64(0)
	bad_dim_file.store_64(10)
	bad_dim_file.store_64(10)
	bad_dim_file.store_32(0)
	bad_dim_file.store_32(256)
	bad_dim_file.store_32(Image.FORMAT_RGBA8)
	bad_dim_file.store_32(1)
	for b in range(10):
		bad_dim_file.store_8(0xAA)
	bad_dim_file.close()

	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Zero dimensions accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Zero dimensions correctly rejected.")

	print("SUCCESS: All Phase 2 Step 2 & Boundary Validation tests passed successfully!")
	quit(0)
