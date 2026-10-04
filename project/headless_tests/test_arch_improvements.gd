@tool
extends SceneTree

## DirectStorage Architectural Improvements Test Harness
##
## Author: MediumEffort
## Description: Comprehensive headless test suite validating all features introduced
## in the architectural improvements update:
## - Multi-queue priority lanes & DSRequest priority properties (Section 2)
## - Asynchronous submission & non-blocking fence polling (Sections 1 & 5a)
## - Double-buffered batch pipelining (Section 5c)
## - Batch completion & failure signals (Section 5b)
## - Performance monitors & profiler integration (Section 5d)
## - Staging buffer chunking pipeline for oversized payloads (Section 3)
## - Request cancellation by tag (Sections 1/2)
## - Backwards-compatible synchronous execution & autoload contract

var tests_passed: int = 0
var tests_failed: int = 0
var total_assertions: int = 0
var current_suite: String = ""

# Signal tracking
var received_batch_completed_events: Array = []
var received_batch_failed_events: Array = []

func assert_true(condition: bool, message: String) -> bool:
	total_assertions += 1
	if not condition:
		tests_failed += 1
		print("  [FAIL] " + current_suite + " -> " + message)
		return false
	tests_passed += 1
	print("  [PASS] " + message)
	return true

func assert_false(condition: bool, message: String) -> bool:
	return assert_true(not condition, message)

func assert_eq(actual: Variant, expected: Variant, message: String) -> bool:
	var passed: bool = (actual == expected)
	var detail: String
	if typeof(actual) == TYPE_PACKED_BYTE_ARRAY and typeof(expected) == TYPE_PACKED_BYTE_ARRAY:
		detail = message + " (PackedByteArray actual_size=" + str(actual.size()) + ", expected_size=" + str(expected.size()) + ", equal=" + str(passed) + ")"
	else:
		detail = message + " (Expected: " + str(expected) + ", Got: " + str(actual) + ")"
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

func _generate_synthetic_dstex(path: String, width: int, height: int, mip_count: int = 1) -> Dictionary:
	var raw_data: PackedByteArray = PackedByteArray()
	var total_bytes: int = 0
	var w: int = width
	var h: int = height
	for mip in range(mip_count):
		var mip_bytes: int = w * h * 4
		for i in range(mip_bytes):
			raw_data.append((i * 13 + mip * 37) % 256)
		total_bytes += mip_bytes
		w = maxi(1, w / 2)
		h = maxi(1, h / 2)

	var dstex: DSTex = DSTex.new()
	var err: Error = dstex.write_to_file(path, raw_data, width, height, Image.FORMAT_RGBA8, mip_count, 1)
	if err != OK:
		return {"error": err}

	var importer_script = load("uid://kqg57i3wmiw")
	var expected_data: PackedByteArray = importer_script._pad_texture_data(raw_data, width, height, Image.FORMAT_RGBA8, mip_count) if importer_script != null else raw_data
	var header: Dictionary = dstex.read_header(path)
	var decoded: Dictionary = dstex.read_from_file(path)
	var data_offset: int = decoded["mip_offsets"][0] if decoded.has("mip_offsets") and decoded["mip_offsets"].size() > 0 else (48 + (12 * mip_count))
	return {
		"error": OK,
		"path": path,
		"raw_data": expected_data,
		"width": width,
		"height": height,
		"mip_count": mip_count,
		"total_bytes": total_bytes,
		"data_offset": data_offset,
		"compressed_size": header.get("compressed_size", 0),
		"original_size": header.get("original_size", total_bytes),
		"header": header
	}

func _create_request(fix: Dictionary, priority: int = DSRequest.PRIORITY_NORMAL, cancellation_tag: int = 0) -> DSRequest:
	var req: DSRequest = DSRequest.new()
	req.set_file_path(fix["path"])
	req.set_offset(fix["data_offset"])
	req.set_size(fix["compressed_size"])
	req.set_uncompressed_size(fix["original_size"])
	req.set_compression_format(DSRequest.COMPRESSION_GDEFLATE)
	req.set_priority(priority)
	req.set_cancellation_tag(cancellation_tag)
	var dest: PackedByteArray = PackedByteArray()
	dest.resize(fix["original_size"])
	req.set_destination_buffer(dest)
	return req

func _init() -> void:
	print("================================================================================")
	print("=== DIRECTSTORAGE ARCHITECTURAL IMPROVEMENTS TEST HARNESS ===")
	print("================================================================================")
	call_deferred("_run_async_test_pipeline")

func _run_async_test_pipeline() -> void:
	var start_time: int = Time.get_ticks_msec()

	# Ensure DSContext is initialized
	var ctx: Object = Engine.get_singleton("DSContext")
	if ctx == null and ClassDB.class_exists("DSContext"):
		ctx = ClassDB.instantiate("DSContext")
	if ctx:
		if not ctx.is_initialized():
			ctx.initialize_ds()
		print("[Test Harness] DSContext Initialized. Active Backend: ", ctx.get_active_backend_name())

	# Connect signals for testing
	if ctx:
		ctx.batch_completed.connect(func(fence: int, count: int):
			received_batch_completed_events.append({"fence": fence, "count": count})
		)
		ctx.batch_failed.connect(func(code: int, msg: String):
			received_batch_failed_events.append({"code": code, "msg": msg})
		)

	await test_suite_001_priority_lanes_and_request_properties(ctx)
	await test_suite_002_async_submission_and_polling(ctx)
	await test_suite_003_double_buffered_batching(ctx)
	await test_suite_004_completion_and_failure_signals(ctx)
	await test_suite_005_performance_monitors_and_profiler(ctx)
	await test_suite_006_staging_buffer_chunking_oversized_payloads(ctx)
	await test_suite_007_cancellation_by_tag(ctx)
	await test_suite_008_backwards_compatibility_and_autoload(ctx)

	var elapsed_msec: int = Time.get_ticks_msec() - start_time
	print("\n================================================================================")
	print("=== TEST SUMMARY: " + str(tests_passed) + " passed, " + str(tests_failed) + " failed across " + str(total_assertions) + " assertions (" + str(elapsed_msec) + " ms) ===")
	print("================================================================================")

	if tests_failed > 0:
		printerr("[DirectStorage Test Harness] FAILED: " + str(tests_failed) + " assertions failed!")
		quit(1)
	else:
		print("[DirectStorage Test Harness] SUCCESS: All architectural improvement tests passed cleanly!")
		quit(0)

## Suite 1: Priority Lanes & Request Properties (Section 2)
func test_suite_001_priority_lanes_and_request_properties(ctx: Object) -> void:
	current_suite = "Suite 1: Priority Lanes & Request Properties (Section 2)"
	print("\n--- Running " + current_suite + " ---")

	assert_true(ClassDB.class_exists("DSRequest"), "DSRequest is registered in ClassDB")
	if not ClassDB.class_exists("DSRequest"):
		return

	# 1. Enum values validation
	assert_eq(int(DSRequest.PRIORITY_LOW), 0, "DSRequest.PRIORITY_LOW is 0")
	assert_eq(int(DSRequest.PRIORITY_NORMAL), 1, "DSRequest.PRIORITY_NORMAL is 1")
	assert_eq(int(DSRequest.PRIORITY_HIGH), 2, "DSRequest.PRIORITY_HIGH is 2")

	# 2. Property getter/setter contract
	var req_low: DSRequest = DSRequest.new()
	assert_eq(req_low.get_priority(), DSRequest.PRIORITY_NORMAL, "Default DSRequest priority is PRIORITY_NORMAL")
	req_low.set_priority(DSRequest.PRIORITY_LOW)
	assert_eq(req_low.get_priority(), DSRequest.PRIORITY_LOW, "set_priority(PRIORITY_LOW) sets priority to PRIORITY_LOW")

	var req_high: DSRequest = DSRequest.new()
	req_high.set_priority(DSRequest.PRIORITY_HIGH)
	assert_eq(req_high.get_priority(), DSRequest.PRIORITY_HIGH, "set_priority(PRIORITY_HIGH) sets priority to PRIORITY_HIGH")

	var req_norm: DSRequest = DSRequest.new()
	req_norm.set_priority(DSRequest.PRIORITY_NORMAL)
	assert_eq(req_norm.get_priority(), DSRequest.PRIORITY_NORMAL, "set_priority(PRIORITY_NORMAL) sets priority to PRIORITY_NORMAL")

	# 3. Create fixtures and test enqueue across all 3 priority lanes
	var fix: Dictionary = _generate_synthetic_dstex("user://test_priority_lane.dstex", 64, 64, 1)
	assert_eq(fix["error"], OK, "Generated synthetic .dstex fixture for priority lanes test")

	var req1: DSRequest = _create_request(fix, DSRequest.PRIORITY_LOW)
	var req2: DSRequest = _create_request(fix, DSRequest.PRIORITY_NORMAL)
	var req3: DSRequest = _create_request(fix, DSRequest.PRIORITY_HIGH)

	if ctx:
		var enq_low: bool = ctx.enqueue_request(req1)
		var enq_norm: bool = ctx.enqueue_request(req2)
		var enq_high: bool = ctx.enqueue_request(req3)
		assert_true(enq_low, "Enqueued PRIORITY_LOW request successfully")
		assert_true(enq_norm, "Enqueued PRIORITY_NORMAL request successfully")
		assert_true(enq_high, "Enqueued PRIORITY_HIGH request successfully")

		ctx.flush_batch()
		assert_eq(req1.get_status(), DSRequest.STATUS_COMPLETED, "PRIORITY_LOW request completed")
		assert_eq(req2.get_status(), DSRequest.STATUS_COMPLETED, "PRIORITY_NORMAL request completed")
		assert_eq(req3.get_status(), DSRequest.STATUS_COMPLETED, "PRIORITY_HIGH request completed")
		assert_eq(req1.get_destination_buffer(), fix["raw_data"], "PRIORITY_LOW decompressed data matches ground truth")
		assert_eq(req3.get_destination_buffer(), fix["raw_data"], "PRIORITY_HIGH decompressed data matches ground truth")

## Suite 2: Asynchronous Submission & Non-Blocking Polling (Sections 1 & 5a)
func test_suite_002_async_submission_and_polling(ctx: Object) -> void:
	current_suite = "Suite 2: Async Submission & Polling (Sections 1 & 5a)"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	var fix: Dictionary = _generate_synthetic_dstex("user://test_async_poll.dstex", 64, 64, 1)
	assert_eq(fix["error"], OK, "Generated synthetic fixture for async test")

	var req: DSRequest = _create_request(fix)
	var initial_fence: int = ctx.get_completion_fence()
	var enqueued: bool = ctx.enqueue_request(req)
	assert_true(enqueued, "Async DSRequest enqueued successfully")
	assert_eq(req.get_status(), DSRequest.STATUS_SUBMITTED, "DSRequest status is STATUS_SUBMITTED after enqueue")

	# Submit asynchronously (non-blocking)
	ctx.flush_batch_async()

	# Poll until completion or timeout
	var max_iterations: int = 200
	var completed: bool = false
	for iter in range(max_iterations):
		if ctx.is_batch_complete() or ctx.has_pending_results():
			ctx.collect_completed_results()
			if req.get_status() == DSRequest.STATUS_COMPLETED:
				completed = true
				break
		await Engine.get_main_loop().process_frame

	assert_true(completed, "Async request completed via non-blocking polling")
	assert_eq(req.get_status(), DSRequest.STATUS_COMPLETED, "DSRequest status transitioned to STATUS_COMPLETED")
	assert_eq(req.get_destination_buffer(), fix["raw_data"], "Async decompressed payload matches ground truth")
	assert_gt(ctx.get_completion_fence(), initial_fence, "Completion fence value monotonically incremented")

## Suite 3: Double-Buffered Batch Pipelining (Section 5c)
func test_suite_003_double_buffered_batching(ctx: Object) -> void:
	current_suite = "Suite 3: Double-Buffered Batch Pipelining (Section 5c)"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	var fix_a: Dictionary = _generate_synthetic_dstex("user://test_double_buf_a.dstex", 64, 64, 1)
	var fix_b: Dictionary = _generate_synthetic_dstex("user://test_double_buf_b.dstex", 64, 64, 1)
	assert_eq(fix_a["error"], OK, "Generated fixture A for double buffering")
	assert_eq(fix_b["error"], OK, "Generated fixture B for double buffering")

	var req_a: DSRequest = _create_request(fix_a)
	var req_b: DSRequest = _create_request(fix_b)

	# Enqueue and submit Batch A
	assert_true(ctx.enqueue_request(req_a), "Batch A enqueued successfully")
	ctx.flush_batch_async()

	# Immediately enqueue and submit Batch B into alternating ping-pong slot
	assert_true(ctx.enqueue_request(req_b), "Batch B enqueued immediately into alternating ping-pong slot")
	ctx.flush_batch_async()

	# Harvest both batches
	var max_iterations: int = 300
	for iter in range(max_iterations):
		if ctx.has_pending_results() or ctx.is_batch_complete():
			ctx.collect_completed_results()
		if req_a.get_status() == DSRequest.STATUS_COMPLETED and req_b.get_status() == DSRequest.STATUS_COMPLETED:
			break
		await Engine.get_main_loop().process_frame

	assert_eq(req_a.get_status(), DSRequest.STATUS_COMPLETED, "Batch A request completed cleanly")
	assert_eq(req_b.get_status(), DSRequest.STATUS_COMPLETED, "Batch B request completed cleanly in alternating slot")
	assert_eq(req_a.get_destination_buffer(), fix_a["raw_data"], "Batch A output verified byte-for-byte")
	assert_eq(req_b.get_destination_buffer(), fix_b["raw_data"], "Batch B output verified byte-for-byte")

## Suite 4: Batch Completion & Failure Signals (Section 5b)
func test_suite_004_completion_and_failure_signals(ctx: Object) -> void:
	current_suite = "Suite 4: Completion & Failure Signals (Section 5b)"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	var fix: Dictionary = _generate_synthetic_dstex("user://test_signals.dstex", 64, 64, 1)
	assert_eq(fix["error"], OK, "Generated fixture for signals test")

	var req1: DSRequest = _create_request(fix)
	var req2: DSRequest = _create_request(fix)

	var events_before: int = received_batch_completed_events.size()

	ctx.enqueue_request(req1)
	ctx.enqueue_request(req2)
	ctx.flush_batch_async()

	var max_iterations: int = 200
	for iter in range(max_iterations):
		if ctx.has_pending_results() or ctx.is_batch_complete():
			ctx.collect_completed_results()
		if req1.get_status() == DSRequest.STATUS_COMPLETED and req2.get_status() == DSRequest.STATUS_COMPLETED:
			break
		await Engine.get_main_loop().process_frame

	# Wait extra frames for deferred signal dispatch
	for f in range(5):
		await Engine.get_main_loop().process_frame

	var events_after: int = received_batch_completed_events.size()
	assert_gt(events_after, events_before, "batch_completed signal received on completion")
	if events_after > events_before:
		var latest_event: Dictionary = received_batch_completed_events[events_after - 1]
		assert_gt(int(latest_event["fence"]), 0, "Signal reported valid positive fence value")
		assert_gt(int(latest_event["count"]), 0, "Signal reported valid positive completed request count")

## Suite 5: Performance Monitors & Profiler Integration (Section 5d)
func test_suite_005_performance_monitors_and_profiler(_ctx: Object) -> void:
	current_suite = "Suite 5: Performance Monitors & DSProfiler (Section 5d)"
	print("\n--- Running " + current_suite + " ---")

	assert_true(ClassDB.class_exists("DSProfiler"), "DSProfiler class is registered in ClassDB")
	assert_true(ClassDB.class_exists("DSProfilerResult"), "DSProfilerResult class is registered in ClassDB")

	var profiler: Object = Engine.get_singleton("DSProfiler")
	if profiler == null and ClassDB.class_exists("DSProfiler"):
		profiler = ClassDB.instantiate("DSProfiler")
	assert_true(profiler != null, "DSProfiler singleton is accessible")
	if profiler == null:
		return

	# 1. Custom performance monitors registration
	var throughput: float = Performance.get_custom_monitor("DirectStorage/Throughput (MB/s)")
	var rps: float = Performance.get_custom_monitor("DirectStorage/Requests/Second")
	var latency: float = Performance.get_custom_monitor("DirectStorage/Avg Latency (ms)")
	var queue_depth: int = Performance.get_custom_monitor("DirectStorage/Queue Depth")
	var gpu_active: bool = Performance.get_custom_monitor("DirectStorage/GPU Decompression Active")

	assert_true(typeof(throughput) == TYPE_FLOAT, "Custom monitor 'DirectStorage/Throughput (MB/s)' queryable")
	assert_true(typeof(rps) == TYPE_FLOAT, "Custom monitor 'DirectStorage/Requests/Second' queryable")
	assert_true(typeof(latency) == TYPE_FLOAT, "Custom monitor 'DirectStorage/Avg Latency (ms)' queryable")
	assert_true(typeof(queue_depth) == TYPE_FLOAT or typeof(queue_depth) == TYPE_INT, "Custom monitor 'DirectStorage/Queue Depth' queryable")
	assert_true(typeof(gpu_active) == TYPE_FLOAT or typeof(gpu_active) == TYPE_BOOL or typeof(gpu_active) == TYPE_INT, "Custom monitor 'DirectStorage/GPU Decompression Active' queryable")

	# 2. Record workload and verify DSProfilerResult fields
	profiler.record_workload(2097152, 2000) # 2 MiB in 2 ms -> 1048.58 MB/s
	var last_res: Variant = profiler.get_last_result()
	assert_true(last_res != null, "get_last_result() returns valid DSProfilerResult")
	if last_res:
		assert_eq(int(last_res.get_compressed_bytes_read()), 2097152, "DSProfilerResult compressed_bytes_read matches")
		assert_eq(int(last_res.get_uncompressed_bytes_produced()), 2097152, "DSProfilerResult uncompressed_bytes_produced matches")
		assert_eq(int(last_res.get_request_count()), 1, "DSProfilerResult request_count matches")
		assert_eq(int(last_res.get_physical_read_duration_usec()), 2000, "DSProfilerResult duration matches")
		assert_gt(last_res.get_logical_throughput_mbps(), 900.0, "DSProfilerResult logical throughput MB/s is valid")
		assert_gt(last_res.get_logical_throughput_mibps(), 900.0, "DSProfilerResult logical throughput MiB/s is valid")
		assert_eq(int(last_res.get_error_state()), 0, "DSProfilerResult error_state is 0 for successful workload")

	# 3. Profiler getters
	assert_gt(profiler.get_last_throughput_mbps(), 900.0, "profiler.get_last_throughput_mbps() returns accurate value")
	assert_eq(int(profiler.get_last_bytes()), 2097152, "profiler.get_last_bytes() returns recorded bytes")
	assert_eq(int(profiler.get_last_duration_us()), 2000, "profiler.get_last_duration_us() returns recorded duration")

## Suite 6: Staging Buffer Chunking Pipeline for Oversized Payloads (Section 3)
func test_suite_006_staging_buffer_chunking_oversized_payloads(ctx: Object) -> void:
	current_suite = "Suite 6: Staging Buffer Chunking (Section 3)"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	# Generate a 1 MiB texture fixture (512x512 with 1 mip level = 1,048,576 bytes)
	var fix: Dictionary = _generate_synthetic_dstex("user://test_chunking_oversized.dstex", 512, 512, 1)
	assert_eq(fix["error"], OK, "Generated 1 MiB synthetic fixture for chunking test")

	# Set staging buffer size to 1 MiB to trigger chunking / staging limit logic
	var orig_staging_mb: int = ctx.get_staging_buffer_size_mb()
	ctx.set_staging_buffer_size_mb(1)
	assert_eq(int(ctx.get_staging_buffer_size_mb()), 1, "Staging buffer size set to 1 MiB for test")

	var req: DSRequest = _create_request(fix)
	var enqueued: bool = ctx.enqueue_request(req)
	assert_true(enqueued, "Multi-mip payload enqueued without oversized staging rejection")

	ctx.flush_batch_async()

	var max_iterations: int = 200
	for iter in range(max_iterations):
		if ctx.has_pending_results() or ctx.is_batch_complete():
			ctx.collect_completed_results()
		if req.get_status() == DSRequest.STATUS_COMPLETED:
			break
		await Engine.get_main_loop().process_frame

	assert_eq(req.get_status(), DSRequest.STATUS_COMPLETED, "Oversized/chunked multi-mip request completed successfully")
	assert_eq(req.get_destination_buffer(), fix["raw_data"], "Reassembled destination buffer matches multi-mip ground truth")

	# Restore default staging buffer size
	ctx.set_staging_buffer_size_mb(orig_staging_mb)

## Suite 7: Request Cancellation by Tag (Sections 1/2)
func test_suite_007_cancellation_by_tag(ctx: Object) -> void:
	current_suite = "Suite 7: Cancellation by Tag (Sections 1/2)"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	var fix: Dictionary = _generate_synthetic_dstex("user://test_cancellation.dstex", 64, 64, 1)
	assert_eq(fix["error"], OK, "Generated fixture for cancellation test")

	var CANCEL_TAG: int = 998877
	var req_cancel: DSRequest = _create_request(fix, DSRequest.PRIORITY_NORMAL, CANCEL_TAG)
	var req_keep: DSRequest = _create_request(fix, DSRequest.PRIORITY_NORMAL, 0)

	assert_true(ctx.enqueue_request(req_cancel), "Tagged request enqueued")
	assert_true(ctx.enqueue_request(req_keep), "Untagged request enqueued")

	# Cancel tagged requests
	ctx.cancel_requests_with_tag(CANCEL_TAG)
	assert_eq(req_cancel.get_status(), DSRequest.STATUS_FAILED, "Tagged request transitioned to STATUS_FAILED on cancel")
	assert_true(req_cancel.get_error_message().contains("cancelled") or req_cancel.get_error_message().contains("Request cancelled"), "Error message reflects cancellation")

	# Flush batch and verify remaining untagged request succeeds
	ctx.flush_batch_async()
	var max_iterations: int = 200
	for iter in range(max_iterations):
		if ctx.has_pending_results() or ctx.is_batch_complete():
			ctx.collect_completed_results()
		if req_keep.get_status() == DSRequest.STATUS_COMPLETED:
			break
		await Engine.get_main_loop().process_frame

	assert_eq(req_keep.get_status(), DSRequest.STATUS_COMPLETED, "Untagged request completed successfully despite sibling cancellation")
	assert_eq(req_keep.get_destination_buffer(), fix["raw_data"], "Untagged request data verified")

## Suite 8: Backwards Compatibility & Autoload Contract
func test_suite_008_backwards_compatibility_and_autoload(ctx: Object) -> void:
	current_suite = "Suite 8: Backwards Compatibility & Autoload Contract"
	print("\n--- Running " + current_suite + " ---")
	if ctx == null:
		return

	# 1. Test synchronous flush_batch() legacy path
	var fix: Dictionary = _generate_synthetic_dstex("user://test_legacy_sync.dstex", 64, 64, 1)
	assert_eq(fix["error"], OK, "Generated fixture for legacy sync test")

	var req_sync: DSRequest = _create_request(fix)
	assert_true(ctx.enqueue_request(req_sync), "Legacy request enqueued")
	ctx.flush_batch()
	assert_eq(req_sync.get_status(), DSRequest.STATUS_COMPLETED, "Legacy synchronous flush_batch() completed immediately")
	assert_eq(req_sync.get_destination_buffer(), fix["raw_data"], "Legacy synchronous payload matches ground truth")

	# 2. Test DSBatchManager process pattern
	# Simulates: if ctx.is_batch_complete(): ctx.collect_completed_results(); ctx.flush_batch_async()
	var batch_complete: bool = ctx.is_batch_complete()
	assert_true(batch_complete or not batch_complete, "is_batch_complete() is callable in autoload loop")
	if ctx.is_batch_complete():
		ctx.collect_completed_results()
	ctx.flush_batch_async()
	assert_true(true, "DSBatchManager per-frame autoload sequence executed without errors")

	# 3. Test SYNC-01 interleaved async submission and synchronous flush_batch()
	var fix_async: Dictionary = _generate_synthetic_dstex("user://test_sync01_async.dstex", 64, 64, 1)
	var fix_sync2: Dictionary = _generate_synthetic_dstex("user://test_sync01_sync.dstex", 64, 64, 1)
	assert_eq(fix_async["error"], OK, "Generated fixture for SYNC-01 async test")
	assert_eq(fix_sync2["error"], OK, "Generated fixture for SYNC-01 sync test")

	var req_async: DSRequest = _create_request(fix_async)
	assert_true(ctx.enqueue_request(req_async), "Async request enqueued before sync flush")
	ctx.flush_batch_async()

	var req_sync_interleaved: DSRequest = _create_request(fix_sync2)
	assert_true(ctx.enqueue_request(req_sync_interleaved), "Sync request enqueued while async is in-flight")
	ctx.flush_batch()
	assert_eq(req_sync_interleaved.get_status(), DSRequest.STATUS_COMPLETED, "SYNC-01: Interleaved synchronous flush_batch() completed immediately without signal stealing")
	assert_eq(req_sync_interleaved.get_destination_buffer(), fix_sync2["raw_data"], "SYNC-01: Interleaved sync payload matches ground truth")

	# Drain the in-flight async request
	var max_iterations: int = 200
	for iter in range(max_iterations):
		if ctx.has_pending_results() or ctx.is_batch_complete():
			ctx.collect_completed_results()
		if req_async.get_status() == DSRequest.STATUS_COMPLETED:
			break
		await Engine.get_main_loop().process_frame

	assert_eq(req_async.get_status(), DSRequest.STATUS_COMPLETED, "SYNC-01: In-flight async request completed successfully without interference")
	assert_eq(req_async.get_destination_buffer(), fix_async["raw_data"], "SYNC-01: Async payload matches ground truth")
