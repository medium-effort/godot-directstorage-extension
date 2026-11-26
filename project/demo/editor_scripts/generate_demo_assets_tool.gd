@tool
extends EditorScript

## Run inside the Godot Editor via File > Run (Ctrl+Shift+X) to generate
## and import all procedural demo assets into res://demo_generated/ with a
## non-blocking, cancellable progress dialog.

func _run() -> void:
	var err := DemoAssetGenerator.prepare_texture_dirs()
	if err != OK:
		push_error("[demo_tool] Failed creating directories: %s" % error_string(err))
		return

	var runner := _AssetGenRunner.new()
	EditorInterface.get_base_control().add_child(runner)


class _AssetGenRunner extends AcceptDialog:
	var _status_label: Label
	var _progress_bar: ProgressBar
	var _cancel_btn: Button
	var _cancelled: bool = false

	func _ready() -> void:
		title = "Generating Demo Assets"
		dialog_hide_on_ok = false
		get_ok_button().visible = false
		_cancel_btn = add_cancel_button("Cancel")
		min_size = Vector2i(420, 130)

		var vbox := VBoxContainer.new()
		vbox.add_theme_constant_override("separation", 8)
		_status_label = Label.new()
		_status_label.text = "Starting texture generation..."
		_progress_bar = ProgressBar.new()
		_progress_bar.max_value = DemoAssetGenerator.TEX_COUNT
		_progress_bar.value = 0.0
		_progress_bar.custom_minimum_size = Vector2(380, 22)
		vbox.add_child(_status_label)
		vbox.add_child(_progress_bar)
		add_child(vbox)

		_cancel_btn.pressed.connect(_on_cancel)
		canceled.connect(_on_cancel)
		popup_centered()
		call_deferred("_run_async")

	func _on_cancel() -> void:
		_cancelled = true

	func _run_async() -> void:
		for i in DemoAssetGenerator.TEX_COUNT:
			if _cancelled:
				break
			_status_label.text = "Generating texture %d / %d (%dx%d)..." % [
				i + 1, DemoAssetGenerator.TEX_COUNT, DemoAssetGenerator.TEX_SIZE, DemoAssetGenerator.TEX_SIZE
			]
			var task_res := {"err": OK}
			var task_id := WorkerThreadPool.add_task(func() -> void:
				task_res["err"] = DemoAssetGenerator.generate_single_texture(i)
			)
			while not WorkerThreadPool.is_task_completed(task_id):
				await get_tree().process_frame
			WorkerThreadPool.wait_for_task_completion(task_id)

			if int(task_res["err"]) != OK:
				push_error("[demo_tool] Texture %d failed: %s" % [i, error_string(int(task_res["err"]))])
				queue_free()
				return
			_progress_bar.value = float(i + 1)

		if _cancelled:
			print("[demo_tool] Generation cancelled by user.")
			queue_free()
			return

		_status_label.text = "Generating dedicated normal map (%dx%d)..." % [
			DemoAssetGenerator.TEX_SIZE, DemoAssetGenerator.TEX_SIZE
		]
		var norm_res := {"err": OK}
		var norm_task := WorkerThreadPool.add_task(func() -> void:
			norm_res["err"] = DemoAssetGenerator.generate_normal_texture()
		)
		while not WorkerThreadPool.is_task_completed(norm_task):
			await get_tree().process_frame
		WorkerThreadPool.wait_for_task_completion(norm_task)
		if int(norm_res["err"]) != OK:
			push_error("[demo_tool] Normal texture failed: %s" % error_string(int(norm_res["err"])))
			queue_free()
			return

		_cancel_btn.disabled = true
		hide()
		await get_tree().process_frame

		# Release any editor-side DSTextureCache / DirectStorage file handles on .dstex before reimporting.
		var reinit_ds := false
		if Engine.has_singleton("DSTextureCache"):
			Engine.get_singleton("DSTextureCache").call("clear")
		if Engine.has_singleton("DSContext"):
			var ctx := Engine.get_singleton("DSContext")
			if bool(ctx.call("is_initialized")):
				ctx.call("shutdown_ds")
				reinit_ds = true

		var fs := EditorInterface.get_resource_filesystem()
		var reimport_state := {"done": false}
		var on_reimported := func(_resources: PackedStringArray) -> void:
			reimport_state["done"] = true
		fs.connect("resources_reimported", on_reimported)
		fs.scan()
		await get_tree().process_frame
		await get_tree().process_frame

		var deadline_msec := Time.get_ticks_msec() + 180000
		var next_probe_msec := 0
		var imported_ok := false
		while Time.get_ticks_msec() < deadline_msec:
			var now_msec := Time.get_ticks_msec()
			if bool(reimport_state["done"]) and not fs.is_scanning() and now_msec >= next_probe_msec:
				next_probe_msec = now_msec + 250
				if DemoAssetGenerator.textures_imported():
					imported_ok = true
					break
			await get_tree().process_frame

		if fs.is_connected("resources_reimported", on_reimported):
			fs.disconnect("resources_reimported", on_reimported)

		# Let EditorFileSystem::reimport_files() unwind completely off the C++ call stack.
		await get_tree().process_frame
		await get_tree().process_frame

		if reinit_ds and Engine.has_singleton("DSContext"):
			Engine.get_singleton("DSContext").call("initialize_ds")

		if not imported_ok:
			push_error("[demo_tool] Timed out waiting for Editor to finish importing generated textures.")
			queue_free()
			return

		var scene_err := DemoAssetGenerator.build_level_scenes()
		if Engine.has_singleton("DSTextureCache"):
			Engine.get_singleton("DSTextureCache").call("clear")
		if scene_err != OK:
			push_error("[demo_tool] Scene build failed: %s" % error_string(scene_err))
			queue_free()
			return

		await get_tree().process_frame
		fs.scan()
		print("[demo_tool] Demo assets ready in %s." % DemoAssetGenerator.GENERATED_ROOT)
		queue_free()
