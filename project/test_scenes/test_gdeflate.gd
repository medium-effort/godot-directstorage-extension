extends MainLoop

func _process(_delta: float) -> bool:
	print("--- GDeflate Compression/Decompression Verification Test ---")

	if not ClassDB.class_exists("GDeflateCompressor") or not ClassDB.class_exists("GDeflateDecompressor"):
		printerr("FAIL: GDeflateCompressor or GDeflateDecompressor class not registered in ClassDB!")
		return true

	var compressor = ClassDB.instantiate("GDeflateCompressor")
	var decompressor = ClassDB.instantiate("GDeflateDecompressor")

	print("Compressor default level: ", compressor.get_compression_level())
	print("Decompressor worker count: ", decompressor.get_worker_count())

	# Create test pattern data (1000 bytes)
	var original_data := PackedByteArray()
	for i in range(1000):
		original_data.append((i * 17 + 5) % 256)

	# Compress
	var compressed_data: PackedByteArray = compressor.compress(original_data, 1)
	print("Original size: ", original_data.size(), " bytes")
	print("Compressed size: ", compressed_data.size(), " bytes")

	if compressed_data.size() == 0:
		printerr("FAIL: Compression produced 0-byte output!")
		return true

	# Decompress
	var decompressed_data: PackedByteArray = decompressor.decompress(compressed_data, original_data.size())
	print("Decompressed size: ", decompressed_data.size(), " bytes")

	if decompressed_data == original_data:
		print("SUCCESS: Decompressed data EXACTLY matches original data!")
	else:
		printerr("FAIL: Decompressed data does NOT match original data!")

	return true
