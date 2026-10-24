@tool
extends SceneTree

func _write_dspkg_header(file: FileAccess, version: int, flags: int, entry_count: int, data_offset: int, min_reader_version: int = 4, compression_codec: int = 0, archive_salt: PackedByteArray = PackedByteArray(), key_id: int = 0, reserved: PackedByteArray = PackedByteArray(), archive_fingerprint: int = 0, legacy_reserved: int = 0) -> void:
	file.store_32(0x474B5044)
	file.store_16(version)
	file.store_16(flags)
	file.store_32(entry_count)
	file.store_64(data_offset)
	if version < 4:
		file.store_64(legacy_reserved)
	else:
		file.store_64(archive_fingerprint)
		file.store_32(compression_codec)
		var salt = archive_salt.duplicate()
		salt.resize(16)
		file.store_buffer(salt)
		file.store_32(min_reader_version)
		file.store_64(key_id)
		var res = reserved.duplicate()
		res.resize(4)
		file.store_buffer(res)

func _write_dspkg_entry(file: FileAccess, path: String, data_offset: int, compressed_size: int, original_size: int, width: int = 256, height: int = 256, mip_count: int = 1, flags: int = 0, version: int = 1) -> void:
	var path_bytes = path.to_utf8_buffer()
	file.store_64(path.hash())
	file.store_16(path_bytes.size())
	file.store_buffer(path_bytes)
	file.store_64(data_offset)
	file.store_64(compressed_size)
	file.store_64(original_size)
	file.store_32(width)
	file.store_32(height)
	file.store_32(Image.FORMAT_RGBA8)
	file.store_32(mip_count)
	if version >= 3:
		file.store_32(flags)

func _init() -> void:
	print("--- Running DirectStorage Phase 2 Step 2 Formats Test ---")
	var error_handler = Engine.get_singleton("DSErrorHandler")
	if error_handler != null and error_handler.has_method("describe_hresult"):
		var mapped_error: String = error_handler.call("describe_hresult", 0x89240008)
		if mapped_error.is_empty() or not mapped_error.to_lower().contains("staging"):
			print("FAIL: E_DSTORAGE_REQUEST_TOO_LARGE was not mapped to an actionable message.")
			quit(1)
			return
		print("SUCCESS: DirectStorage HRESULT mapping verified.")

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

	# 1. Truncated Header (< 28 bytes legacy, < 64 bytes v4)
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

	# 2. Bad Version (version = 5)
	var bad_ver_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	bad_ver_file.store_32(0x474B5044)
	bad_ver_file.store_16(5)
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
	for b in range(50):
		bad_offset_file.store_8(0)
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

	# 8. Invalid zero mip count
	var bad_mips_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_mips_file, 1, 0, 1, 28 + 50 + path_len)
	_write_dspkg_entry(bad_mips_file, dummy_path, 0, 1, 1, 256, 256, 0)
	bad_mips_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Zero mip count accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Zero mip count correctly rejected.")

	# 9. Entry path length extends past the entry table boundary
	var bad_path_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_path_file, 1, 0, 1, 28 + 50)
	bad_path_file.store_64(dummy_path.hash())
	bad_path_file.store_16(100)
	for b in range(40):
		bad_path_file.store_8(0)
	bad_path_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Entry path length overflow accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Entry path length overflow correctly rejected.")

	# 10. Entry payload offset arithmetic overflow
	var bad_arithmetic_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_arithmetic_file, 1, 0, 1, 28 + 50 + path_len)
	_write_dspkg_entry(bad_arithmetic_file, dummy_path, -1, 1, 1)
	bad_arithmetic_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Entry payload offset overflow accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Entry payload offset overflow correctly rejected.")

	# 11. Compressed size exceeds the backend/allocation limit
	var bad_compressed_size_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_compressed_size_file, 1, 0, 1, 28 + 50 + path_len)
	_write_dspkg_entry(bad_compressed_size_file, dummy_path, 0, 2147483648, 1)
	bad_compressed_size_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Oversized compressed size accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Oversized compressed size correctly rejected.")

	# 12. Original size exceeds the backend/allocation limit
	var bad_original_size_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_original_size_file, 1, 0, 1, 28 + 50 + path_len)
	_write_dspkg_entry(bad_original_size_file, dummy_path, 0, 1, 2147483648)
	bad_original_size_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Oversized original size accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Oversized original size correctly rejected.")

	var valid_v4_path = "user://test_valid_v4.dspkg"
	var valid_v4_file = FileAccess.open(valid_v4_path, FileAccess.WRITE)
	var v4_path_bytes = dummy_path.to_utf8_buffer()
	var v4_payload_size = 4096
	var v4_data_offset = 64 + 54 + v4_path_bytes.size()
	_write_dspkg_header(valid_v4_file, 4, 0, 1, v4_data_offset)
	_write_dspkg_entry(valid_v4_file, dummy_path, 0, v4_payload_size, v4_payload_size, 256, 256, 1, 0, 4)
	for b in range(v4_payload_size):
		valid_v4_file.store_8(0)
	valid_v4_file.close()
	var v4_reader = DSPkg.new()
	if v4_reader.open(valid_v4_path) != OK:
		print("FAIL: Valid unencrypted v4 archive failed to open cleanly.")
		quit(1)
		return
	DirAccess.remove_absolute(ProjectSettings.globalize_path(valid_v4_path))
	print("SUCCESS: Valid unencrypted v4 archive opened cleanly.")

	# Truncated 40-byte v4 header (< 64 bytes)
	var trunc_v4_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(trunc_v4_file, 4, 0, 1, 100)
	trunc_v4_file.close()
	var raw_bytes = FileAccess.get_file_as_bytes(test_bad_path).slice(0, 40)
	var trunc_write = FileAccess.open(test_bad_path, FileAccess.WRITE)
	trunc_write.store_buffer(raw_bytes)
	trunc_write.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Truncated 40-byte v4 header accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Truncated 40-byte v4 header correctly rejected.")

	# Malformed v4 with min_reader_version = 5
	var bad_min_ver_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_min_ver_file, 4, 0, 1, 64 + 54 + path_len, 5)
	bad_min_ver_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Malformed v4 with min_reader_version=5 accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Malformed v4 with min_reader_version=5 correctly rejected.")

	# Malformed v4 with compression_codec = 1
	var bad_codec_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_codec_file, 4, 0, 1, 64 + 54 + path_len, 4, 1)
	bad_codec_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Malformed v4 with compression_codec=1 accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Malformed v4 with compression_codec=1 correctly rejected.")

	# Malformed v4 with reserved[0] != 0
	var bad_reserved_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_reserved_file, 4, 0, 1, 64 + 54 + path_len, 4, 0, PackedByteArray(), 0, PackedByteArray([1, 0, 0, 0]))
	bad_reserved_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Malformed v4 with non-zero reserved bytes accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Malformed v4 with non-zero reserved bytes correctly rejected.")

	# Malformed v3 with flags = DSPKG_FLAG_ENCRYPTED_TOC (2)
	var bad_v3_enc_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(bad_v3_enc_file, 3, 2, 1, 28 + 54 + path_len)
	bad_v3_enc_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Malformed v3 with encrypted flag accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Malformed v3 with encrypted flag correctly rejected.")

	# Malformed encrypted v4 with empty TOC envelope (data_offset == toc_start == 80)
	var empty_env_file = FileAccess.open(test_bad_path, FileAccess.WRITE)
	_write_dspkg_header(empty_env_file, 4, 2, 0, 80, 4, 0, PackedByteArray(), 0x123456789ABCDEF0)
	for b in range(16):
		empty_env_file.store_8(0)
	empty_env_file.close()
	reader = DSPkg.new()
	if reader.open(test_bad_path) == OK:
		print("FAIL: Empty encrypted TOC envelope accepted by DSPkg.open()")
		quit(1)
		return
	print("SUCCESS: Empty encrypted TOC envelope correctly rejected.")

	# A failed reopen must not leave the previous archive exposed.
	var state_reader = DSPkg.new()
	if state_reader.open(test_dspkg_path) != OK:
		print("FAIL: Could not prepare DSPkg reader state reset test.")
		quit(1)
		return
	if state_reader.open(test_bad_path) == OK or state_reader.get_entry_count() != 0 or state_reader.has_texture(dummy_path):
		print("FAIL: Failed DSPkg.open() left stale archive state exposed.")
		quit(1)
		return
	print("SUCCESS: Failed DSPkg.open() resets reader state.")

	print("SUCCESS: All Phase 2 Step 2 & Boundary Validation tests passed successfully!")
	quit(0)
