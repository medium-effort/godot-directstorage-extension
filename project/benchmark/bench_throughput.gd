extends SceneTree

const Common = preload("uid://bu4lmsaq3vqq")

func _initialize() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ClassDB.class_exists("GDeflateDecompressor"):
		print("[SKIP] Throughput benchmark requires DSContext and GDeflateDecompressor")
		Common.finish(self, 0)
		return
	var fixture := Common.make_fixture("user://bench_throughput.gdeflate")
	if not bool(fixture.get("ok", false)):
		printerr("[FAIL] ", fixture.get("reason", "fixture creation failed"))
		Common.finish(self, 1)
		return
	var decompressor: Variant = ClassDB.instantiate("GDeflateDecompressor")
	var start_usec := Time.get_ticks_usec()
	for _index in int(fixture["count"]):
		decompressor.decompress(fixture["compressed"], fixture["payload"].size())
	var cpu_seconds := float(Time.get_ticks_usec() - start_usec) / 1000000.0
	var logical_gb := float(fixture["payload"].size() * int(fixture["count"])) / 1000000000.0
	print("[Throughput] cpu_path_gb_s=", logical_gb / maxf(cpu_seconds, 0.000001), " compressed_bytes=", fixture["compressed"].size() * int(fixture["count"]), " logical_bytes=", fixture["payload"].size() * int(fixture["count"]))
	if not ctx.is_d3d12_backend() or not ctx.is_gpu_decompression_available():
		print("[SKIP] GPU path unavailable; NVMe-to-VRAM comparison requires an initialized D3D12 backend")
	else:
		var gpu_result := Common.run_gpu_texture_batch(ctx, RenderingServer.get_rendering_device(), fixture)
		if not bool(gpu_result.get("ok", false)):
			printerr("[FAIL] GPU batch: ", gpu_result.get("reason", "request failure"))
			Common.finish(self, 1)
			return
		var gpu_seconds := float(gpu_result["elapsed_ms"]) / 1000.0
		print("[Throughput] gpu_path_gb_s=", logical_gb / maxf(gpu_seconds, 0.000001), " gpu_elapsed_ms=", gpu_result["elapsed_ms"], " failed_requests=", gpu_result["failed"])
	Common.finish(self, 0)
