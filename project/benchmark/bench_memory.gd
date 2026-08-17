extends SceneTree

const Common = preload("uid://bu4lmsaq3vqq")

func _initialize() -> void:
	var ctx = Engine.get_singleton("DSContext")
	if ctx == null or not ClassDB.class_exists("GDeflateDecompressor"):
		print("[SKIP] Memory benchmark requires DSContext and GDeflateDecompressor")
		Common.finish(self, 0)
		return
	var fixture := Common.make_fixture("user://bench_memory.gdeflate")
	if not bool(fixture.get("ok", false)):
		printerr("[FAIL] ", fixture.get("reason", "fixture creation failed"))
		Common.finish(self, 1)
		return
	var before := Performance.get_monitor(Performance.MEMORY_STATIC)
	var decompressor: Variant = ClassDB.instantiate("GDeflateDecompressor")
	for _index in int(fixture["count"]):
		decompressor.decompress(fixture["compressed"], fixture["payload"].size())
	var after := Performance.get_monitor(Performance.MEMORY_STATIC)
	print("[Memory] static_memory_before=", before, " static_memory_after=", after, " delta=", after - before)
	print("[Memory] OS Private Bytes/Working Set are not exposed by portable GDScript; use the Windows process counter for the authoritative zero-copy result")
	if not ctx.is_d3d12_backend() or not ctx.is_gpu_decompression_available():
		print("[SKIP] GPU zero-copy comparison unavailable")
	else:
		var gpu_before := Performance.get_monitor(Performance.MEMORY_STATIC)
		var gpu_result := Common.run_gpu_texture_batch(ctx, RenderingServer.get_rendering_device(), fixture)
		var gpu_after := Performance.get_monitor(Performance.MEMORY_STATIC)
		if not bool(gpu_result.get("ok", false)):
			printerr("[FAIL] GPU batch: ", gpu_result.get("reason", "request failure"))
			Common.finish(self, 1)
			return
		print("[Memory] gpu_static_memory_before=", gpu_before, " gpu_static_memory_after=", gpu_after, " gpu_delta=", gpu_after - gpu_before, " failed_requests=", gpu_result["failed"])
	Common.finish(self, 0)
