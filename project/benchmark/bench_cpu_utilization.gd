extends SceneTree

const Common = preload("uid://bu4lmsaq3vqq")

func _initialize() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ClassDB.class_exists("GDeflateDecompressor"):
		print("[SKIP] CPU utilization benchmark requires DSContext and GDeflateDecompressor")
		Common.finish(self, 0)
		return
	var fixture := Common.make_fixture("user://bench_cpu_utilization.gdeflate")
	if not bool(fixture.get("ok", false)):
		printerr("[FAIL] ", fixture.get("reason", "fixture creation failed"))
		Common.finish(self, 1)
		return
	Common.print_environment(ctx)
	var decompressor: Variant = ClassDB.instantiate("GDeflateDecompressor")
	var start_usec := Time.get_ticks_usec()
	for _index in int(fixture["count"]):
		var output: PackedByteArray = decompressor.decompress(fixture["compressed"], fixture["payload"].size())
		if output.size() != fixture["payload"].size():
			printerr("[FAIL] CPU decompression returned an unexpected size")
			Common.finish(self, 1)
			return
	var cpu_ms := float(Time.get_ticks_usec() - start_usec) / 1000.0
	print("[CPU Utilization] samples=", fixture["count"], " cpu_decompression_ms=", cpu_ms, " cpu_utilization_proxy=100%")
	if not ctx.is_d3d12_backend() or not ctx.is_gpu_decompression_available():
		print("[SKIP] GPU comparison unavailable; expected GPU CPU-decompression utilization is near 0%")
	else:
		var gpu_result := Common.run_gpu_texture_batch(ctx, RenderingServer.get_rendering_device(), fixture)
		if not bool(gpu_result.get("ok", false)):
			printerr("[FAIL] GPU batch: ", gpu_result.get("reason", "request failure"))
			Common.finish(self, 1)
			return
		print("[CPU Utilization] gpu_texture_batch_ms=", gpu_result["elapsed_ms"], " cpu_decompression_expected=near 0%")
	Common.finish(self, 0)
