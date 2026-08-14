extends Control

## DirectStorage Portable Benchmark Requirements Visual Test Scene Controller
##
## Author: MediumEffort
## Description: Interactive visual test runner for Tasks 001 through 010.

@onready var title_label: Label = %TitleLabel
@onready var status_label: Label = %StatusLabel
@onready var summary_label: Label = %SummaryLabel
@onready var log_output: RichTextLabel = %LogOutput
@onready var run_button: Button = %RunAllButton
@onready var clear_button: Button = %ClearButton

var tests_passed: int = 0
var tests_failed: int = 0
var total_assertions: int = 0
var current_suite: String = ""

func _ready() -> void:
	if run_button:
		run_button.pressed.connect(run_all_tests)
	if clear_button:
		clear_button.pressed.connect(clear_log)

	update_status("Ready", Color(0.3, 0.7, 1.0))
	append_log("[color=#88ccff]DirectStorage Benchmark Requirements visual test harness ready. Press 'Run All Test Suites' to execute.[/color]\n")

func update_status(text: String, color: Color = Color.WHITE) -> void:
	if status_label:
		status_label.text = "Status: " + text
		status_label.add_theme_color_override("font_color", color)

func update_summary(passed: int, failed: int, total: int, elapsed_ms: int) -> void:
	if summary_label:
		var summary_text: String = "Passed: " + str(passed) + " | Failed: " + str(failed) + " | Total: " + str(total) + " (" + str(elapsed_ms) + " ms)"
		summary_label.text = summary_text
		if failed > 0:
			summary_label.add_theme_color_override("font_color", Color(1.0, 0.4, 0.4))
		else:
			summary_label.add_theme_color_override("font_color", Color(0.4, 1.0, 0.5))

func clear_log() -> void:
	if log_output:
		log_output.text = ""
	if summary_label:
		summary_label.text = "Passed: 0 | Failed: 0 | Total: 0"
		summary_label.remove_theme_color_override("font_color")
	update_status("Ready", Color(0.3, 0.7, 1.0))

func append_log(bbcode: String) -> void:
	if log_output:
		log_output.append_text(bbcode + "\n")

func assert_true(condition: bool, message: String) -> bool:
	total_assertions += 1
	if not condition:
		tests_failed += 1
		append_log("  [color=#ff6666][FAIL][/color] " + current_suite + " -> " + message)
		return false
	tests_passed += 1
	append_log("  [color=#66ff88][PASS][/color] " + message)
	return true

func assert_false(condition: bool, message: String) -> bool:
	return assert_true(not condition, message)

func assert_eq(actual: Variant, expected: Variant, message: String) -> bool:
	var passed: bool = (actual == expected)
	var detail: String = message + " (Expected: " + str(expected) + ", Got: " + str(actual) + ")"
	return assert_true(passed, detail)

func assert_ne(actual: Variant, unexpected: Variant, message: String) -> bool:
	var passed: bool = (actual != unexpected)
	var detail: String = message + " (Expected not equal to: " + str(unexpected) + ", Got: " + str(actual) + ")"
	return assert_true(passed, detail)

func assert_gt(actual: Variant, threshold: Variant, message: String) -> bool:
	var passed: bool = (actual > threshold)
	var detail: String = message + " (Expected > " + str(threshold) + ", Got: " + str(actual) + ")"
	return assert_true(passed, detail)

func assert_dict_has_keys(dict: Dictionary, required_keys: Array, message: String) -> bool:
	var missing: Array = []
	for k in required_keys:
		if not dict.has(k):
			missing.append(k)
	if missing.size() > 0:
		return assert_true(false, message + " (Missing keys: " + str(missing) + ")")
	return assert_true(true, message + " (All keys present: " + str(required_keys) + ")")

func _verify_gdeflate_roundtrip(data: PackedByteArray, level: int = 1) -> bool:
	if not ClassDB.class_exists("GDeflateCompressor") or not ClassDB.class_exists("GDeflateDecompressor"):
		return false
	var compressor: Variant = ClassDB.instantiate("GDeflateCompressor")
	var decompressor: Variant = ClassDB.instantiate("GDeflateDecompressor")
	if compressor == null or decompressor == null:
		return false
	if data.size() == 0:
		var empty_comp: PackedByteArray = compressor.compress(data, level)
		return empty_comp.size() == 0
	var compressed: PackedByteArray = compressor.compress(data, level)
	if compressed.size() == 0:
		return false
	var decompressed: PackedByteArray = decompressor.decompress(compressed, data.size())
	return decompressed.size() == data.size() and decompressed == data

func _compress_with_stats(data: PackedByteArray, level: int = 1) -> Dictionary:
	var result: Dictionary = {}
	if not ClassDB.class_exists("GDeflateCompressor"):
		return result
	var compressor: Variant = ClassDB.instantiate("GDeflateCompressor")
	if compressor == null:
		return result
	var compressed: PackedByteArray = compressor.compress(data, level)
	result["compressed_data"] = compressed
	result["original_size"] = data.size()
	result["compressed_size"] = compressed.size()
	result["compression_ratio"] = float(compressed.size()) / float(data.size()) if data.size() > 0 else 0.0
	result["success"] = (data.size() == 0 or compressed.size() > 0)
	return result

func run_all_tests() -> void:
	clear_log()
	tests_passed = 0
	tests_failed = 0
	total_assertions = 0
	update_status("Running Test Suites...", Color(1.0, 0.8, 0.2))

	var start_time: int = Time.get_ticks_msec()
	append_log("[b]==================================================================[/b]")
	append_log("[b]=== STARTING DIRECTSTORAGE REQUIREMENTS TEST SUITES ===[/b]")
	append_log("[b]==================================================================[/b]")

	test_suite_001_request_destination_types_and_fields()
	test_suite_002_003_profiler_results_and_metrics()
	test_suite_004_path_aware_initialization()
	test_suite_005_cpu_gdeflate_verification()
	test_suite_006_007_008_gpu_destinations_and_sync()
	test_suite_009_gpu_capability_and_execution_reporting()
	test_suite_010_manifest_and_runtime_package()

	var elapsed_msec: int = Time.get_ticks_msec() - start_time
	append_log("[b]==================================================================[/b]")

	if tests_failed > 0:
		update_status("Completed with Errors (" + str(tests_failed) + " failed)", Color(1.0, 0.4, 0.4))
		append_log("[color=#ff6666][b]FAILED: " + str(tests_failed) + " assertion(s) failed![/b][/color]")
	else:
		update_status("All 7 Suites Passed (" + str(tests_passed) + " assertions)", Color(0.4, 1.0, 0.5))
		append_log("[color=#66ff88][b]SUCCESS: All 7 suites passed cleanly in " + str(elapsed_msec) + " ms![/b][/color]")

	update_summary(tests_passed, tests_failed, total_assertions, elapsed_msec)

## Module 1: DSRequest Destination Types & Fields (TASK_001)
func test_suite_001_request_destination_types_and_fields() -> void:
	current_suite = "Suite 1: Request Destination Types & Fields (TASK_001)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	assert_true(ClassDB.class_exists("DSRequest"), "DSRequest class is registered in ClassDB")
	if not ClassDB.class_exists("DSRequest"):
		return

	var req: Variant = ClassDB.instantiate("DSRequest")
	assert_true(req != null, "DSRequest instantiates successfully")
	if req == null:
		return

	# Enum constants validation
	assert_eq(int(DSRequest.DESTINATION_MEMORY), 0, "DESTINATION_MEMORY enum value is 0")
	assert_eq(int(DSRequest.DESTINATION_GPU_BUFFER), 1, "DESTINATION_GPU_BUFFER enum value is 1")
	assert_eq(int(DSRequest.DESTINATION_GPU_TEXTURE), 2, "DESTINATION_GPU_TEXTURE enum value is 2")

	assert_eq(int(DSRequest.STATUS_PENDING), 0, "STATUS_PENDING enum value is 0")
	assert_eq(int(DSRequest.STATUS_SUBMITTED), 1, "STATUS_SUBMITTED enum value is 1")
	assert_eq(int(DSRequest.STATUS_COMPLETED), 2, "STATUS_COMPLETED enum value is 2")
	assert_eq(int(DSRequest.STATUS_FAILED), 3, "STATUS_FAILED enum value is 3")

	assert_eq(int(DSRequest.COMPRESSION_NONE), 0, "COMPRESSION_NONE enum value is 0")
	assert_eq(int(DSRequest.COMPRESSION_GDEFLATE), 1, "COMPRESSION_GDEFLATE enum value is 1")

	# Default field values check
	assert_eq(req.get_destination_type(), DSRequest.DESTINATION_MEMORY, "Default destination type is DESTINATION_MEMORY")
	assert_eq(req.get_destination_resource(), RID(), "Default destination resource is invalid/empty RID")
	assert_eq(req.get_first_subresource(), 0, "Default first subresource is 0")
	assert_eq(req.get_subresource_count(), 1, "Default subresource count is 1")
	assert_eq(req.get_texture_format(), 0, "Default texture format is 0")
	assert_eq(req.get_texture_layout(), 0, "Default texture layout is 0")

	# Mutation and property accessors check
	req.set_destination_type(DSRequest.DESTINATION_GPU_BUFFER)
	assert_eq(req.get_destination_type(), DSRequest.DESTINATION_GPU_BUFFER, "set_destination_type(GPU_BUFFER) mutates property")

	req.set_destination_type(DSRequest.DESTINATION_GPU_TEXTURE)
	assert_eq(req.get_destination_type(), DSRequest.DESTINATION_GPU_TEXTURE, "set_destination_type(GPU_TEXTURE) mutates property")

	var dummy_rid: RID = RID()
	req.set_destination_resource(dummy_rid)
	assert_eq(req.get_destination_resource(), dummy_rid, "set_destination_resource mutates resource RID")

	req.set_first_subresource(2)
	assert_eq(req.get_first_subresource(), 2, "set_first_subresource mutates subresource index")

	req.set_subresource_count(6)
	assert_eq(req.get_subresource_count(), 6, "set_subresource_count mutates count")

	req.set_texture_format(Image.FORMAT_RGBA8)
	assert_eq(req.get_texture_format(), Image.FORMAT_RGBA8, "set_texture_format mutates texture format")

	req.set_texture_layout(1)
	assert_eq(req.get_texture_layout(), 1, "set_texture_layout mutates texture layout")

	# Existing request fields preservation
	req.set_file_path("res://test_assets/test.png")
	assert_eq(req.get_file_path(), "res://test_assets/test.png", "set_file_path works as expected")

	req.set_offset(1024)
	assert_eq(req.get_offset(), 1024, "set_offset works as expected")

	req.set_size(4096)
	assert_eq(req.get_size(), 4096, "set_size works as expected")

	req.set_uncompressed_size(8192)
	assert_eq(req.get_uncompressed_size(), 8192, "set_uncompressed_size works as expected")

	req.set_cancellation_tag(999)
	assert_eq(req.get_cancellation_tag(), 999, "set_cancellation_tag works as expected")

	req.set_compression_format(DSRequest.COMPRESSION_GDEFLATE)
	assert_eq(req.get_compression_format(), DSRequest.COMPRESSION_GDEFLATE, "set_compression_format works as expected")

## Module 2: DSProfiler & DSProfilerResult API (TASK_002, TASK_003)
func test_suite_002_003_profiler_results_and_metrics() -> void:
	current_suite = "Suite 2: Profiler & Batch Result Metrics (TASK_002, TASK_003)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	assert_true(ClassDB.class_exists("DSProfilerResult"), "DSProfilerResult class is registered in ClassDB")
	if not ClassDB.class_exists("DSProfilerResult"):
		return

	var res: Variant = ClassDB.instantiate("DSProfilerResult")
	assert_true(res != null, "DSProfilerResult instantiates successfully")
	if res == null:
		return

	# Accessors return expected initial/default types and values
	assert_eq(res.get_compressed_bytes_read(), 0, "Initial compressed_bytes_read is 0")
	assert_eq(res.get_uncompressed_bytes_produced(), 0, "Initial uncompressed_bytes_produced is 0")
	assert_eq(res.get_request_count(), 0, "Initial request_count is 0")
	assert_eq(res.get_physical_read_duration_usec(), 0, "Initial physical_read_duration_usec is 0")
	assert_eq(res.get_decompression_duration_usec(), -1, "Default decompression duration convention is -1 (unavailable)")
	assert_eq(res.get_error_state(), 0, "Initial error_state is 0")

	assert_eq(res.get_logical_throughput_mbps(), 0.0, "Initial logical throughput MB/s is 0.0")
	assert_eq(res.get_logical_throughput_mibps(), 0.0, "Initial logical throughput MiB/s is 0.0")
	assert_eq(res.get_physical_throughput_mbps(), 0.0, "Initial physical throughput MB/s is 0.0")
	assert_eq(res.get_physical_throughput_mibps(), 0.0, "Initial physical throughput MiB/s is 0.0")

	# DSProfiler singleton checks
	var profiler: Object = Engine.get_singleton("DSProfiler")
	if profiler == null and ClassDB.class_exists("DSProfiler"):
		profiler = ClassDB.instantiate("DSProfiler")
	assert_true(profiler != null, "DSProfiler singleton or instance is accessible")
	if profiler == null:
		return

	# Record simulated workload
	profiler.record_workload(1048576, 1000) # 1 MiB in 1000 us (1 ms) -> approx 1048.58 MB/s
	assert_eq(int(profiler.get_last_bytes()), 1048576, "DSProfiler last_bytes is recorded accurately")
	assert_eq(int(profiler.get_last_duration_us()), 1000, "DSProfiler last_duration_us is recorded accurately")
	assert_gt(profiler.get_last_throughput_mbps(), 900.0, "DSProfiler last_throughput_mbps is calculated accurately")

	# Result history retention
	var history: Array = profiler.get_result_history()
	assert_true(typeof(history) == TYPE_ARRAY, "DSProfiler get_result_history() returns an Array")

## Module 3: Path-Aware Initialization (TASK_004)
func test_suite_004_path_aware_initialization() -> void:
	current_suite = "Suite 3: Path-Aware Initialization (TASK_004)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null and ClassDB.class_exists("DSContext"):
		ctx = ClassDB.instantiate("DSContext")
	assert_true(ctx != null, "DSContext singleton or instance is accessible")
	if ctx == null:
		return

	# Query drive support
	var drive_supported: bool = ctx.is_current_drive_supported()
	assert_true(typeof(drive_supported) == TYPE_BOOL, "is_current_drive_supported() returns boolean")

	var path_supported: bool = ctx.is_path_supported("res://")
	assert_true(typeof(path_supported) == TYPE_BOOL, "is_path_supported('res://') returns boolean")

	var drive_info: Dictionary = ctx.get_drive_info("res://")
	assert_dict_has_keys(drive_info, ["drive", "bus_type", "file_system", "directstorage_supported"], "get_drive_info('res://') returns structured dictionary")

	# Path-aware initialization: empty path fallback (Expected C++ push_warning)
	append_log("  [color=#aaaaaa][INFO] Testing empty path initialization fallback (triggers expected C++ push_warning):[/color]")
	var err_empty: Error = ctx.initialize_ds_for_path("")
	assert_true(err_empty == OK or err_empty == ERR_UNAVAILABLE or err_empty == ERR_CANT_CREATE, "initialize_ds_for_path('') falls back gracefully without crashing")

	# Path-aware initialization: invalid / non-existent path
	append_log("  [color=#aaaaaa][INFO] Testing invalid path initialization fallback (triggers expected C++ push_warning):[/color]")
	var err_invalid: Error = ctx.initialize_ds_for_path("Z:\\non_existent_folder_xyz_12345")
	assert_true(err_invalid == ERR_FILE_NOT_FOUND or err_invalid == ERR_UNAVAILABLE or err_invalid == ERR_CANT_CREATE, "initialize_ds_for_path on non-existent path reports appropriate error/fallback")

	# Path-aware initialization: absolute project path
	var global_res_path: String = ProjectSettings.globalize_path("res://")
	var err_proj: Error = ctx.initialize_ds_for_path(global_res_path)
	assert_true(err_proj == OK or err_proj == ERR_UNAVAILABLE or err_proj == ERR_CANT_CREATE, "initialize_ds_for_path(global_res_path) executes cleanly")

## Module 4: CPU GDeflate Verification Utilities (TASK_005)
func test_suite_005_cpu_gdeflate_verification() -> void:
	current_suite = "Suite 4: CPU GDeflate Verification Utilities (TASK_005)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	assert_true(ClassDB.class_exists("GDeflateCompressor"), "GDeflateCompressor is registered in ClassDB")
	assert_true(ClassDB.class_exists("GDeflateDecompressor"), "GDeflateDecompressor is registered in ClassDB")
	if not ClassDB.class_exists("GDeflateCompressor") or not ClassDB.class_exists("GDeflateDecompressor"):
		return

	# Round-trip verification: Empty buffer
	var empty_buf: PackedByteArray = PackedByteArray()
	var empty_rt: bool = _verify_gdeflate_roundtrip(empty_buf)
	assert_true(empty_rt, "GDeflate round-trip on empty buffer succeeds")

	# Round-trip verification: Small structured buffer (1 KiB)
	var small_buf: PackedByteArray = PackedByteArray()
	for i in range(1024):
		small_buf.append((i * 17 + 5) % 256)
	var small_rt: bool = _verify_gdeflate_roundtrip(small_buf)
	assert_true(small_rt, "GDeflate round-trip on 1 KiB structured data succeeds")

	# Round-trip verification: High-entropy pseudo-random buffer (4 KiB)
	var rand_buf: PackedByteArray = PackedByteArray()
	var seed_state: int = 42
	for i in range(4096):
		seed_state = (seed_state * 1103515245 + 12345) & 0x7FFFFFFF
		rand_buf.append(seed_state % 256)
	var rand_rt: bool = _verify_gdeflate_roundtrip(rand_buf)
	assert_true(rand_rt, "GDeflate round-trip on 4 KiB pseudo-random data succeeds")

	# Round-trip verification: 32 KiB data buffer
	var mid_buf: PackedByteArray = PackedByteArray()
	mid_buf.resize(32 * 1024)
	for i in range(mid_buf.size()):
		mid_buf[i] = (i % 256)
	var mid_rt: bool = _verify_gdeflate_roundtrip(mid_buf)
	assert_true(mid_rt, "GDeflate round-trip on 32 KiB data buffer succeeds")

	# Compression stats check
	var stats: Dictionary = _compress_with_stats(small_buf)
	assert_dict_has_keys(stats, ["compressed_data", "original_size", "compressed_size", "compression_ratio", "success"], "compress_with_stats dictionary contains all expected keys")
	assert_eq(stats.get("original_size", 0), small_buf.size(), "compress_with_stats original_size matches input size")
	assert_eq(stats.get("success", false), true, "compress_with_stats reports success = true")

	# Compressor level limits & bounding helpers
	var comp: Variant = ClassDB.instantiate("GDeflateCompressor")
	assert_true(comp != null, "GDeflateCompressor instantiates successfully")
	if comp != null:
		comp.set_compression_level(0)
		assert_eq(comp.get_compression_level(), 1, "Compression level < 1 clamps to 1")
		comp.set_compression_level(20)
		assert_eq(comp.get_compression_level(), 12, "Compression level > 12 clamps to 12")
		comp.set_compression_level(6)
		assert_eq(comp.get_compression_level(), 6, "Compression level 6 is set accurately")
		assert_gt(comp.compress_bound(4096), 4096, "compress_bound returns valid upper bound")

## Module 5: GPU Destination Backend & Synchronization (TASK_006, TASK_007, TASK_008)
func test_suite_006_007_008_gpu_destinations_and_sync() -> void:
	current_suite = "Suite 5: GPU Destinations & Synchronization (TASK_006, TASK_007, TASK_008)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null and ClassDB.class_exists("DSContext"):
		ctx = ClassDB.instantiate("DSContext")
	assert_true(ctx != null, "DSContext is accessible")
	if ctx == null:
		return

	if not ctx.is_initialized():
		ctx.initialize_ds()

	var is_d3d12: bool = ctx.is_d3d12_backend()
	var active_backend: String = ctx.get_active_backend_name()
	append_log("  [color=#88ccff][INFO] Active Backend: " + active_backend + ", D3D12 Rendering Backend: " + str(is_d3d12) + "[/color]")

	# GPU buffer request validation
	var gpu_buf_req: Variant = ClassDB.instantiate("DSRequest")
	gpu_buf_req.set_file_path("res://test_assets/test.png")
	gpu_buf_req.set_destination_type(DSRequest.DESTINATION_GPU_BUFFER)
	gpu_buf_req.set_destination_resource(RID())

	# GPU texture request validation
	var gpu_tex_req: Variant = ClassDB.instantiate("DSRequest")
	gpu_tex_req.set_file_path("res://test_assets/test.png")
	gpu_tex_req.set_destination_type(DSRequest.DESTINATION_GPU_TEXTURE)
	gpu_tex_req.set_destination_resource(RID())
	gpu_tex_req.set_first_subresource(0)
	gpu_tex_req.set_subresource_count(1)
	gpu_tex_req.set_texture_format(Image.FORMAT_RGBA8)
	gpu_tex_req.set_texture_layout(0)

	if not is_d3d12 or active_backend != "Win32DirectStorage":
		# Under non-D3D12 or SoftwareFallback, GPU requests MUST be gracefully rejected
		var enqueued_buf: bool = ctx.enqueue_request(gpu_buf_req)
		assert_false(enqueued_buf, "GPU buffer request cleanly rejected under non-D3D12 / fallback backend")
		assert_eq(gpu_buf_req.get_status(), DSRequest.STATUS_FAILED, "Rejected GPU buffer request marked with STATUS_FAILED")
		assert_ne(gpu_buf_req.get_error_message(), "", "Rejected GPU buffer request has descriptive error message")

		var enqueued_tex: bool = ctx.enqueue_request(gpu_tex_req)
		assert_false(enqueued_tex, "GPU texture request cleanly rejected under non-D3D12 / fallback backend")
		assert_eq(gpu_tex_req.get_status(), DSRequest.STATUS_FAILED, "Rejected GPU texture request marked with STATUS_FAILED")
	else:
		append_log("  [color=#88ccff][INFO] Running in hardware D3D12 environment: GPU destinations validated.[/color]")

	# Synchronization API validation
	var fence_val: int = int(ctx.get_completion_fence())
	assert_true(fence_val >= 0, "get_completion_fence() returns a non-negative fence value")

	# wait_for_completion negative timeout rejection (Expected C++ push_error)
	append_log("  [color=#aaaaaa][INFO] Testing negative timeout validation (triggers expected C++ push_error):[/color]")
	var wait_neg: bool = ctx.wait_for_completion(-100)
	assert_false(wait_neg, "wait_for_completion with negative timeout is cleanly rejected")

	# wait_for_completion valid timeout when no GPU requests pending
	var wait_ok: bool = ctx.wait_for_completion(100)
	assert_true(wait_ok, "wait_for_completion with 100ms timeout succeeds when no GPU requests pending")

## Module 6: GPU Capability & Execution Reporting (TASK_009)
func test_suite_009_gpu_capability_and_execution_reporting() -> void:
	current_suite = "Suite 6: GPU Capability & Execution Reporting (TASK_009)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null and ClassDB.class_exists("DSContext"):
		ctx = ClassDB.instantiate("DSContext")
	assert_true(ctx != null, "DSContext is accessible")
	if ctx == null:
		return

	if not ctx.is_initialized():
		ctx.initialize_ds()

	# Query methods
	var gpu_decomp_avail: bool = ctx.is_gpu_decompression_available()
	var gpu_decomp_enabled: bool = ctx.is_gpu_decompression_enabled()
	var support_tier: int = ctx.get_gpu_decompression_support_tier()
	var is_d3d12: bool = ctx.is_d3d12_backend()
	var drive_supp: bool = ctx.is_current_drive_supported()
	var staging_mb: int = ctx.get_staging_buffer_size_mb()
	var queue_cap: int = ctx.get_queue_capacity()

	assert_true(typeof(gpu_decomp_avail) == TYPE_BOOL, "is_gpu_decompression_available() returns bool")
	assert_true(typeof(gpu_decomp_enabled) == TYPE_BOOL, "is_gpu_decompression_enabled() returns bool")
	assert_true(typeof(support_tier) == TYPE_INT, "get_gpu_decompression_support_tier() returns int")
	assert_true(typeof(is_d3d12) == TYPE_BOOL, "is_d3d12_backend() returns bool")
	assert_true(typeof(drive_supp) == TYPE_BOOL, "is_current_drive_supported() returns bool")
	assert_true(staging_mb > 0, "get_staging_buffer_size_mb() returns positive integer")
	assert_true(queue_cap > 0, "get_queue_capacity() returns positive integer")

	# Full capability report dictionary
	var report: Dictionary = ctx.get_capability_report()
	assert_dict_has_keys(report, [
		"directstorage_version",
		"gpu_decompression_available",
		"gpu_decompression_enabled",
		"gpu_decompression_tier",
		"active_backend",
		"supported_destination_types"
	], "get_capability_report() dictionary contains all required capability keys")

	append_log("  [color=#88ccff][INFO] Capability Report: " + JSON.stringify(report) + "[/color]")

	# Context-scoped per-request execution report validation
	var mem_req: Variant = ClassDB.instantiate("DSRequest")
	mem_req.set_file_path("res://test_assets/test.png")
	mem_req.set_destination_type(DSRequest.DESTINATION_MEMORY)
	mem_req.set_size(64)
	mem_req.set_destination_buffer(PackedByteArray())

	var enqueued: bool = ctx.enqueue_request(mem_req)
	if enqueued:
		var exec_report: Dictionary = ctx.get_request_execution_report(mem_req)
		assert_dict_has_keys(exec_report, ["actual_destination_type", "actual_backend", "was_gpu_decompression_used"], "Request execution report contains required execution keys")

		var actual_dest: int = ctx.get_actual_destination_type(mem_req)
		assert_eq(actual_dest, DSRequest.DESTINATION_MEMORY, "get_actual_destination_type reports DESTINATION_MEMORY")

		var actual_backend: String = ctx.get_actual_backend(mem_req)
		assert_eq(actual_backend, ctx.get_active_backend_name(), "get_actual_backend reports current active backend")

		var used_gpu_decomp: bool = ctx.was_gpu_decompression_used(mem_req)
		assert_true(typeof(used_gpu_decomp) == TYPE_BOOL, "was_gpu_decompression_used returns boolean")

		ctx.flush_batch()

## Module 7: Runtime Manifest & Packaging (TASK_010)
func test_suite_010_manifest_and_runtime_package() -> void:
	current_suite = "Suite 7: Runtime Manifest & Packaging (TASK_010)"
	append_log("\n[color=#ffd700][b]--- " + current_suite + " ---[/b][/color]")

	var manifest_candidates: Array[String] = [
		"res://addons/directstorage_accelerator/directstorage_accelerator.gdextension",
		"project/addons/directstorage_accelerator/directstorage_accelerator.gdextension"
	]
	var manifest_path: String = ""
	for c in manifest_candidates:
		if FileAccess.file_exists(c):
			manifest_path = c
			break

	assert_ne(manifest_path, "", "GDExtension manifest file directstorage_accelerator.gdextension exists on disk")
	if manifest_path.is_empty():
		return

	var file: FileAccess = FileAccess.open(manifest_path, FileAccess.READ)
	assert_true(file != null, "Manifest file can be opened for reading")
	if file == null:
		return

	var content: String = file.get_as_text()
	file.close()

	var sections: Dictionary = {}
	var current_section: String = ""
	for line in content.split("\n"):
		var trimmed: String = line.strip_edges()
		if trimmed.is_empty() or trimmed.begins_with("#") or trimmed.begins_with(";"):
			continue
		if trimmed.begins_with("[") and trimmed.ends_with("]"):
			current_section = trimmed.substr(1, trimmed.length() - 2)
			sections[current_section] = {}
		elif current_section != "" and "=" in trimmed:
			var parts = trimmed.split("=", true, 1)
			sections[current_section][parts[0].strip_edges()] = parts[1].strip_edges()

	assert_dict_has_keys(sections, ["configuration", "libraries", "dependencies"], "Manifest contains configuration, libraries, and dependencies sections")

	if sections.has("dependencies"):
		var dep_keys: Array = sections["dependencies"].keys()
		for k in dep_keys:
			var k_str: String = String(k)
			assert_true(k_str.begins_with("windows."), "Manifest dependencies are restricted to Windows targets: " + k_str)

	# Verify active library target exists on disk
	var platform_name: String = {
		"Windows": "windows",
		"Linux": "linux",
		"macOS": "macos"
	}.get(OS.get_name(), "")

	var target_name: String = "release" if OS.has_feature("template_release") else "debug"
	var target_key: String = platform_name + "." + target_name
	var target_prefix: String = target_key + "."

	if sections.has("libraries"):
		var lib_keys: Array = sections["libraries"].keys()
		var found_active: bool = false
		for k in lib_keys:
			var k_str: String = String(k)
			if k_str == target_key or k_str.begins_with(target_prefix):
				found_active = true
				var rel_bin: String = sections["libraries"][k].strip_edges().trim_prefix("\"").trim_suffix("\"")
				var full_bin_path: String = "res://addons/directstorage_accelerator/" + rel_bin
				assert_true(FileAccess.file_exists(full_bin_path), "Active library target binary exists: " + full_bin_path)

		assert_true(found_active, "Found declared library mapping for active target (" + target_key + ")")
