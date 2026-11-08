@tool
extends SceneTree

var passed: int = 0
var failed: int = 0
var completed_events: Array[Dictionary] = []

func _init() -> void:
	call_deferred("_run")

func _check(condition: bool, message: String) -> void:
	if condition:
		passed += 1
		print("[PASS] ", message)
	else:
		failed += 1
		printerr("[FAIL] ", message)

func _on_batch_completed(fence: int, completed_count: int) -> void:
	completed_events.append({"fence": fence, "count": completed_count})

func _make_fixture(path: String, size: int) -> PackedByteArray:
	var data := PackedByteArray()
	data.resize(size)
	for index in range(size):
		data[index] = (index * 29 + 7) % 256
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_buffer(data)
		file.close()
	return data

func _make_request(path: String, size: int) -> DSRequest:
	var request := DSRequest.new()
	request.set_file_path(path)
	request.set_size(size)
	request.set_uncompressed_size(size)
	request.set_destination_type(DSRequest.DESTINATION_MEMORY)
	return request

func _wait_for_requests(ctx: Object, requests: Array[DSRequest]) -> void:
	for _iteration in range(240):
		var batch_complete: bool = ctx.is_batch_complete()
		var pending_results: bool = ctx.has_pending_results()
		if pending_results:
			ctx.collect_completed_results()
		var all_done := true
		for request in requests:
			if request.get_status() != DSRequest.STATUS_COMPLETED and request.get_status() != DSRequest.STATUS_FAILED:
				all_done = false
		if all_done and batch_complete:
			return
		await process_frame

func _run() -> void:
	print("=== TASK_008 API & Engine Integration Test Suite ===")

	var setting_defaults := {
		"dsgd_extension/staging_buffer_size_mb": 128,
		"dsgd_extension/disable_gpu_decompression": false,
		"dsgd_extension/queue_capacity": 512,
		"dsgd_extension/texture_cache_enabled": true,
		"dsgd_extension/texture_cache_max_entries": 128,
	}
	for setting_name in setting_defaults:
		_check(ProjectSettings.has_setting(setting_name), "ProjectSettings registers " + setting_name)
		if ProjectSettings.has_setting(setting_name):
			_check(ProjectSettings.get_setting(setting_name) == setting_defaults[setting_name], "ProjectSettings default is correct for " + setting_name)

	_check(ClassDB.class_exists("DSContext"), "DSContext is registered")
	_check(ClassDB.class_exists("DSRequest"), "DSRequest is registered")
	var ctx: Object = Engine.get_singleton("DSContext")
	_check(ctx != null, "DSContext singleton is available")
	if ctx == null:
		_print_summary()
		return

	var support_result: bool = ctx.is_supported()
	var support_consistent := true
	for _probe in range(32):
		if typeof(ctx.is_supported()) != TYPE_BOOL or ctx.is_supported() != support_result:
			support_consistent = false
	_check(typeof(support_result) == TYPE_BOOL, "DSContext.is_supported() returns bool")
	_check(support_consistent, "Repeated DSContext.is_supported() probes are consistent")

	var init_status: Variant = ctx.initialize_ds()
	_check(typeof(init_status) == TYPE_BOOL, "initialize_ds() returns a boolean status")
	_check(init_status == true, "initialize_ds() succeeds")
	var path_status: Variant = ctx.initialize_ds_for_path("")
	_check(typeof(path_status) == TYPE_INT, "initialize_ds_for_path() returns an Error status code")
	_check(path_status == OK, "initialize_ds_for_path(\"\") returns OK")
	_check(ctx.is_initialized(), "DSContext remains initialized after initialization probes")

	ctx.batch_completed.connect(_on_batch_completed)
	var fixture_path := "user://task_008_api_engine_integration.bin"
	var fixture := _make_fixture(fixture_path, 4096)
	var manager: Node = get_root().get_node_or_null("DSRuntime")
	if manager == null:
		manager = get_root().get_node_or_null("DSBatchManager")
	_check(manager != null, "DSRuntime autoload is available")
	if manager != null:
		_check(manager.has_method("_process"), "DSRuntime exposes its per-frame process method")

	while ctx.has_pending_results():
		ctx.collect_completed_results()
	_check(ctx.is_batch_complete(), "Batch is complete before a new submission")
	_check(not ctx.has_pending_results(), "No pending results exist before a new submission")

	var sync_request := _make_request(fixture_path, fixture.size())
	_check(ctx.enqueue_request(sync_request), "Synchronous request enqueues successfully")
	_check(ctx.get_pending_request_count() == 1, "Pending request count is one before synchronous flush")
	ctx.flush_batch()
	_check(sync_request.get_status() == DSRequest.STATUS_COMPLETED, "Synchronous request completes")
	_check(sync_request.get_destination_buffer() == fixture, "Synchronous request data is exact")
	_check(ctx.get_pending_request_count() == 0, "Pending request count is zero after synchronous flush")
	_check(ctx.is_batch_complete(), "Batch is complete after synchronous flush")
	_check(not ctx.has_pending_results(), "Synchronous flush leaves no pending results")

	var async_requests: Array[DSRequest] = []
	for _request_index in range(3):
		var request := _make_request(fixture_path, fixture.size())
		async_requests.append(request)
		_check(ctx.enqueue_request(request), "Asynchronous batch request enqueues successfully")
	_check(ctx.get_pending_request_count() == 3, "Pending request count matches asynchronous batch size")
	ctx.flush_batch_async()
	_check(ctx.get_pending_request_count() == 0, "Pending request count is zero after asynchronous submission")
	var async_batch_state_is_valid: bool = typeof(ctx.is_batch_complete()) == TYPE_BOOL and typeof(ctx.has_pending_results()) == TYPE_BOOL
	_check(async_batch_state_is_valid, "Asynchronous batch completion and pending-result states are booleans during submission")
	await _wait_for_requests(ctx, async_requests)
	for request in async_requests:
		_check(request.get_status() == DSRequest.STATUS_COMPLETED, "Asynchronous request completes")
		_check(request.get_destination_buffer() == fixture, "Asynchronous request data is exact")
	_check(ctx.is_batch_complete(), "Batch is complete after asynchronous results are harvested")
	while ctx.has_pending_results():
		ctx.collect_completed_results()
	_check(not ctx.has_pending_results(), "Pending results are cleared after collection")
	await process_frame
	var found_three_request_event := false
	for event in completed_events:
		if int(event["count"]) == 3:
			found_three_request_event = true
			_check(int(event["fence"]) > 0, "batch_completed reports a positive fence")
		_check(found_three_request_event, "batch_completed completed_count accurately reports three requests")

	# DSRuntime is stream-only by design: _process() drives the async prewarm
	# queue (flush_stream_async) and never touches the sync loader queue, so
	# this contract is exercised with a stream enqueue. A sync-queue request
	# here would stay pending forever - not a runtime defect (ds_runtime.gd
	# documents the split: sync loads flush themselves).
	if manager != null:
		var manager_request := _make_request(fixture_path, fixture.size())
		_check(ctx.enqueue_request(manager_request, true), "DSRuntime stream request enqueues successfully")
		_check(ctx.get_stream_pending_count() == 1, "DSRuntime sees one pending stream request")
		manager.call("_process", 0.016)
		_check(ctx.get_stream_pending_count() == 0, "DSRuntime _process submits the pending stream batch")
		await _wait_for_requests(ctx, [manager_request])
		_check(manager_request.get_status() == DSRequest.STATUS_COMPLETED, "DSRuntime-submitted request completes")
		while ctx.has_pending_results():
			ctx.collect_completed_results()

	DirAccess.remove_absolute(ProjectSettings.globalize_path(fixture_path))
	_print_summary()

func _print_summary() -> void:
	print("=== TASK_008 RESULT: ", passed, " passed, ", failed, " failed ===")
	quit(1 if failed > 0 else 0)
