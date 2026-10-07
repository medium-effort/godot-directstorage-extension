class_name GDeflateTextureVerification
extends RefCounted

const ROW_PITCH_ALIGNMENT := 256

static func align_up(value: int, alignment: int = ROW_PITCH_ALIGNMENT) -> int:
	return int((value + alignment - 1) / alignment) * alignment

static func mip_extent(width: int, height: int, mip: int) -> Vector2i:
	return Vector2i(maxi(1, width >> mip), maxi(1, height >> mip))

static func is_block_compressed(format: int) -> bool:
	return format == RenderingDevice.DATA_FORMAT_BC1_RGB_UNORM_BLOCK or format == RenderingDevice.DATA_FORMAT_BC3_UNORM_BLOCK or format == RenderingDevice.DATA_FORMAT_BC5_UNORM_BLOCK or format == RenderingDevice.DATA_FORMAT_BC6H_SFLOAT_BLOCK or format == RenderingDevice.DATA_FORMAT_BC7_UNORM_BLOCK

static func bytes_per_pixel(format: int) -> int:
	match format:
		RenderingDevice.DATA_FORMAT_R8_UNORM, RenderingDevice.DATA_FORMAT_R8_UINT, RenderingDevice.DATA_FORMAT_R8_SINT:
			return 1
		RenderingDevice.DATA_FORMAT_R8G8_UNORM, RenderingDevice.DATA_FORMAT_R16_UNORM, RenderingDevice.DATA_FORMAT_R16_UINT, RenderingDevice.DATA_FORMAT_R16_SFLOAT:
			return 2
		RenderingDevice.DATA_FORMAT_R8G8B8A8_UNORM, RenderingDevice.DATA_FORMAT_R8G8B8A8_SRGB, RenderingDevice.DATA_FORMAT_R32_SFLOAT, RenderingDevice.DATA_FORMAT_R32_UINT:
			return 4
		RenderingDevice.DATA_FORMAT_R16G16B16A16_UNORM, RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT, RenderingDevice.DATA_FORMAT_R32G32_SFLOAT:
			return 8
		RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT:
			return 16
		_:
			return 4

static func row_layout(width: int, height: int, format: int) -> Array:
	if is_block_compressed(format):
		var blocks_x := maxi(1, (width + 3) / 4)
		var blocks_y := maxi(1, (height + 3) / 4)
		var block_bytes := 8 if format == RenderingDevice.DATA_FORMAT_BC1_RGB_UNORM_BLOCK else 16
		return [blocks_x * block_bytes, blocks_y]
	return [width * bytes_per_pixel(format), height]

static func mip_footprints(width: int, height: int, format: int, mip_count: int) -> Array:
	var result: Array = []
	for mip in mip_count:
		var extent := mip_extent(width, height, mip)
		var layout := row_layout(extent.x, extent.y, format)
		var row_bytes: int = layout[0]
		var row_count: int = layout[1]
		result.append({
			"mip": mip,
			"width": extent.x,
			"height": extent.y,
			"row_bytes": row_bytes,
			"row_count": row_count,
			"row_pitch": align_up(row_bytes),
			"size": align_up(row_bytes) * row_count
		})
	return result

static func make_pattern_payload(footprints: Array) -> PackedByteArray:
	var payload := PackedByteArray()
	for footprint in footprints:
		var start := payload.size()
		payload.resize(start + int(footprint["size"]))
		for index in range(int(footprint["size"])):
			payload[start + index] = (int(footprint["mip"]) * 53 + index) % 256
	return payload

static func write_gdeflate_payload(path: String, payload: PackedByteArray, level: int = 1) -> Dictionary:
	if not ClassDB.class_exists("GDeflateCompressor"):
		return {"ok": false, "reason": "GDeflateCompressor is not registered"}
	var compressor: Variant = ClassDB.instantiate("GDeflateCompressor")
	var compressed: PackedByteArray = compressor.compress(payload, level)
	if compressed.is_empty():
		return {"ok": false, "reason": "GDeflate compression returned an empty stream"}
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return {"ok": false, "reason": "Could not open " + path + " for writing"}
	file.store_buffer(compressed)
	file.close()
	return {"ok": true, "compressed": compressed}

static func command_line_has(flag: String) -> bool:
	return OS.get_cmdline_args().has(flag)
