extends GutTest

const MANIFEST_PATH := "res://addons/directstorage_accelerator/directstorage_accelerator.gdextension"


func test_gdextension_manifest_structure() -> void:
	assert_true(FileAccess.file_exists(MANIFEST_PATH), "manifest exists")
	var content := FileAccess.get_file_as_string(MANIFEST_PATH)
	var current_section := ""
	var sections := {}
	for line in content.split("\n"):
		var trimmed := line.strip_edges()
		if trimmed.is_empty() or trimmed.begins_with("#") or trimmed.begins_with(";"):
			continue
		if trimmed.begins_with("[") and trimmed.ends_with("]"):
			current_section = trimmed.substr(1, trimmed.length() - 2)
			sections[current_section] = {}
		elif current_section != "" and "=" in trimmed:
			var parts := trimmed.split("=", true, 1)
			sections[current_section][parts[0].strip_edges()] = parts[1].strip_edges()

	for sec in ["configuration", "libraries", "dependencies"]:
		assert_true(sections.has(sec), "manifest has [%s]" % sec)

	var lib_keys: Array = sections["libraries"].keys()
	assert_false(lib_keys.is_empty(), "manifest declares libraries")

	var platform_name: String = {
		"Windows": "windows",
		"Linux": "linux",
		"macOS": "macos",
	}.get(OS.get_name(), "")
	assert_false(platform_name.is_empty(), "supported test platform")

	var target_name := "release" if OS.has_feature("template_release") else "debug"
	var target_prefix := platform_name + "." + target_name + "."
	var target_key := platform_name + "." + target_name
	var active_count := 0
	for k in lib_keys:
		var ks := String(k)
		if ks != target_key and not ks.begins_with(target_prefix):
			continue
		active_count += 1
		var rel := String(sections["libraries"][k]).strip_edges().trim_prefix("\"").trim_suffix("\"")
		assert_true(FileAccess.file_exists("res://addons/directstorage_accelerator/" + rel), "active library exists: " + rel)
	assert_gt(active_count, 0, "at least one active library entry for %s" % target_prefix)

	for k in sections["dependencies"].keys():
		assert_true(String(k).begins_with("windows."), "dependencies only on Windows: " + String(k))
	for k in lib_keys:
		var ks := String(k)
		if ks.begins_with("linux.") or ks.begins_with("macos."):
			assert_false(sections["dependencies"].has(k), "no dependencies for non-Windows target: " + ks)


func test_project_settings_defaults_all_13_keys() -> void:
	var expected := {
		"dsgd_extension/staging_buffer_size_mb": 32,
		"dsgd_extension/disable_gpu_decompression": false,
		"dsgd_extension/disable_gpu_decompression_metacommand": false,
		"dsgd_extension/force_disable_bypass_io": false,
		"dsgd_extension/force_software_fallback": false,
		"dsgd_extension/allow_directstorage_without_bypassio": true,
		"dsgd_extension/max_cpu_threads": 0,
		"dsgd_extension/debug_break_on_error": false,
		"dsgd_extension/queue_capacity": 512,
		"dsgd_extension/auto_tune_for_integrated_gpu": true,
		"dsgd_extension/auto_tune_for_low_vram_gpu": true,
		"dsgd_extension/texture_cache_enabled": true,
		"dsgd_extension/texture_cache_max_entries": 512,
	}
	for setting_name in expected:
		assert_true(ProjectSettings.has_setting(setting_name), "registered: " + setting_name)
		if ProjectSettings.has_setting(setting_name):
			assert_eq(ProjectSettings.get_setting(setting_name), expected[setting_name], "default for " + setting_name)


func test_classdb_and_singleton_smoke() -> void:
	for cls in ["DSContext", "DSRequest", "DSErrorHandler", "DSCrypto", "DSPackScanner", "DSTex", "DSPkg", "DSTextureCache"]:
		assert_true(ClassDB.class_exists(cls), "ClassDB has " + cls)

	var ctx := Engine.get_singleton("DSContext")
	assert_not_null(ctx, "DSContext singleton available")
	assert_true(typeof(ctx.is_supported()) == TYPE_BOOL)
	assert_true(ctx.initialize_ds(), "initialize_ds() succeeds")
	assert_eq(ctx.initialize_ds_for_path(""), OK)
	assert_true(ctx.is_initialized())
	ctx.shutdown_ds()
