@tool
extends EditorPlugin

## DirectStorage Accelerator Editor Plugin
##
## Author: MediumEffort
## Description: Registers the DSBatchManager autoload, DSTex importer, and
## DSPkg export plugin in the Godot Editor. Enable this plugin to get the
## automatic texture import and archive bundling workflow.

const LOG_PREFIX: String = "[dsgd Extension] "
const AUTOLOAD_NAME: String = "DSRuntime"
const AUTOLOAD_PATH: String = "res://addons/directstorage_accelerator/scripts/ds_runtime.gd"
const LEGACY_AUTOLOAD_NAME: String = "DSBatchManager"
const VERIFY_MENU_ITEM: String = "Verify exported pack (DSPkg)…"
const VERIFY_DIALOG_SCENE: String = "uid://bdepjlb375cg5"
const VERIFY_DIALOG_SCENE_PATH: String = "res://addons/directstorage_accelerator/tools/dspkg_pack_scan_dialog.tscn"
const RUNTIME_MENU_ITEM: String = "Manage DirectStorage Runtime..."
const RUNTIME_DIALOG_SCENE_PATH: String = "res://addons/directstorage_accelerator/tools/runtime_manager_dialog.tscn"
const MIGRATION_MENU_ITEM: String = "Migrate textures to DirectStorage..."
const MIGRATION_DIALOG_SCENE_PATH: String = "res://addons/directstorage_accelerator/tools/ds_migration_dialog.tscn"
const TOOLS_SUBMENU_NAME: String = "dsgd Tools... (DirectStorage)"
const TOOLS_SUBMENU_VERIFY_ID: int = 0
const TOOLS_SUBMENU_RUNTIME_ID: int = 1
const TOOLS_SUBMENU_MIGRATION_ID: int = 2
const RUNTIME_INSTALLER_SCRIPT = preload("res://addons/directstorage_accelerator/scripts/ds_runtime_installer.gd")
const IMPORT_PROFILE_SETTING: String = "dsgd_extension/default_import_profile"
const LEGACY_IMPORT_PROFILE_SETTING: String = "directstorage/import/default_profile"

## Export plugin options are stored in export_presets.cfg under their option
## name, so renaming the constants hides the old values from get_option().
## This map carries them across once. It also retires options whose meaning
## changed: the old third-party toggle became the DSGD license file.
const LEGACY_EXPORT_OPTIONS := {
	"directstorage/bundle_textures_dspkg": "dsgd_extension/bundle_textures_dspkg",
	"directstorage/enforce_archive_pairing": "dsgd_extension/enforce_archive_pairing",
	"directstorage/steampipe_1mb_alignment": "dsgd_extension/steampipe_1mb_alignment",
	"dsgd_extension/include_third_party_notices": "dsgd_extension/include_dsgd_license_file",
}
## Must match the entry symbol in `src/register_types.cpp`; it is how a loaded
## library is identified as this addon when it has been copied or renamed.
const EXTENSION_ENTRY_SYMBOL: String = "directstorage_accelerator_init"

var _importer: EditorImportPlugin = null
var _export_plugin: EditorExportPlugin = null
var _checked_scenes: Dictionary = {}
var _verify_dialog: Window = null
var _runtime_dialog: Window = null
var _migration_dialog: Window = null
var _tools_submenu: PopupMenu = null
## One deferred re-initialization at a time: `extension_loaded` and
## `extensions_reloaded` can both fire for the same reload.
var _native_reinit_queued: bool = false
var _is_disabling: bool = false


## Registers the autoload, import plugin, export plugin, and DirectStorage
## runtime when the addon is enabled.
##
## The autoload is installed here rather than in `_enable_plugin()`:
## `EditorNode::init_plugins()` enables addons with `p_config_changed = false`,
## so the engine only calls `_enable_plugin()` on a runtime toggle, never when
## the editor starts with the addon already enabled. `_enter_tree()` runs on
## both paths. Unregistration lives in `_disable_plugin()`, the one callback
## the engine fires for a user-initiated disable alone (see `_exit_tree()`).
func _enter_tree() -> void:
	# A staged runtime swap has to land before the first DirectStorage call:
	# only then are the flat DLLs unloaded and replaceable on Windows. This is
	# a no-op when nothing is pending.
	if Engine.is_editor_hint():
		_apply_pending_runtime()

	# Purge legacy autoload if present from older addon versions
	_sync_autoload(LEGACY_AUTOLOAD_NAME, "")
	_sync_autoload(AUTOLOAD_NAME, AUTOLOAD_PATH)

	if not ProjectSettings.has_setting("autoload/" + AUTOLOAD_NAME):
		push_error(
			LOG_PREFIX + "Failed to register the %s autoload in project.godot. Async texture prewarm needs that node to drive the stream queue and will stay unavailable until 'autoload/%s' exists in Project Settings." % [AUTOLOAD_NAME, AUTOLOAD_NAME]
		)

	print(
		LOG_PREFIX + "by MediumEffort. Addon enabled. Autoload, Importer, and Exporter registered."
	)

	for internal_setting in [
		"dsgd_extension/bundle_dspkg",
		"dsgd_extension/archive_id",
		"dsgd_extension/protected",
		"dsgd_extension/archive_name",
	]:
		if ProjectSettings.has_setting(internal_setting):
			ProjectSettings.set_as_internal(internal_setting, true)

	_register_import_profile()
	_migrate_export_option_keys()

	if _importer == null:
		var importer_script = load("uid://kqg57i3wmiw")
		if importer_script:
			_importer = importer_script.new()
			add_import_plugin(_importer)

	if _export_plugin == null:
		var exporter_script = load("uid://37jisblsb2jo")
		if exporter_script:
			_export_plugin = exporter_script.new()
			add_export_plugin(_export_plugin)

	if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
		_add_tools_submenu()

	if not scene_changed.is_connected(_on_scene_changed):
		scene_changed.connect(_on_scene_changed)
	if not scene_closed.is_connected(_on_scene_closed):
		scene_closed.connect(_on_scene_closed)

	_report_extension_state()
	_sync_native_reload_signals(true)

	var ds_context = Engine.get_singleton("DSContext")
	if ds_context:
		ds_context.initialize_ds()
		_warn_if_runtime_missing(ds_context)


## Unregisters the import plugin, export plugin, and tool menu item when the
## editor stops this plugin: a normal editor shutdown and a user-initiated
## disable both land here.
##
## The autoload is deliberately NOT touched. It is project data, not editor
## session state: erasing it on shutdown also erased it from project.godot, so
## the next standalone or exported run had no `DSRuntime` node - nothing
## submitted or harvested the async stream queue, and prewarm stalled at
## 0/<total> with no diagnostic because the backend still initialised lazily.
## Only `_disable_plugin()`, which the engine calls for a user-initiated
## disable alone, unregisters the autoload.
func _exit_tree() -> void:
	if _importer != null:
		remove_import_plugin(_importer)
		_importer = null

	if _export_plugin != null:
		remove_export_plugin(_export_plugin)
		_export_plugin = null

	if Engine.is_editor_hint() and EditorInterface.get_editor_main_screen() != null:
		remove_tool_menu_item(TOOLS_SUBMENU_NAME)
	# remove_tool_menu_item() memdelete()s a registered submenu, so the plugin
	# only drops its reference here; freeing it again would be a double free.
	_tools_submenu = null

	if is_instance_valid(_verify_dialog):
		_verify_dialog.queue_free()
		_verify_dialog = null
	if is_instance_valid(_runtime_dialog):
		_runtime_dialog.queue_free()
		_runtime_dialog = null
	if is_instance_valid(_migration_dialog):
		_migration_dialog.queue_free()
		_migration_dialog = null

	if scene_changed.is_connected(_on_scene_changed):
		scene_changed.disconnect(_on_scene_changed)
	if scene_closed.is_connected(_on_scene_closed):
		scene_closed.disconnect(_on_scene_closed)
	_sync_native_reload_signals(false)
	_checked_scenes.clear()

	if not _is_disabling:
		print(
			LOG_PREFIX + "Editor hooks unloaded (%s autoload kept in project.godot)."
			% AUTOLOAD_NAME
		)
	_is_disabling = false


## Called by the engine only when the user enables the addon in the Plugin tab
## of Project Settings (never on normal editor startup). When the DirectStorage
## runtime DLLs are not installed yet on Windows, open the runtime manager
## automatically so the user can accept the license and install them right away.
func _enable_plugin() -> void:
	if _should_prompt_runtime_install_on_enable():
		_on_runtime_menu.call_deferred()


## Called by the engine only when the user disables the addon in the Plugin tab
## of the project settings (the editor never calls it while quitting). This is
## the single place that unregisters the autoload, so a plain editor shutdown
## cannot strip it from project.godot again.
func _disable_plugin() -> void:
	_is_disabling = true
	_sync_autoload(AUTOLOAD_NAME, "")

	print(
		LOG_PREFIX + "Addon disabled in Project Settings. %s autoload and editor hooks unregistered."
		% AUTOLOAD_NAME
	)


## Carries export-preset options across the settings rename.
##
## Godot keys export plugin options by the option name declared in
## _get_export_options(), so once the constants change, an existing preset's
## "directstorage/..." keys are invisible and every option would silently
## revert to its default. Copy them to the new names once, then drop the old
## keys so export_presets.cfg does not accumulate dead entries.
func _migrate_export_option_keys() -> void:
	const PRESET_PATH := "res://export_presets.cfg"
	if not FileAccess.file_exists(PRESET_PATH):
		return
	var config := ConfigFile.new()
	if config.load(PRESET_PATH) != OK:
		return
	var changed := false
	for section in config.get_sections():
		if not section.begins_with("preset."):
			continue
		for old_key in LEGACY_EXPORT_OPTIONS:
			if not config.has_section_key(section, old_key):
				continue
			var new_key: String = LEGACY_EXPORT_OPTIONS[old_key]
			if not config.has_section_key(section, new_key):
				config.set_value(section, new_key, config.get_value(section, old_key))
			config.erase_section_key(section, old_key)
			changed = true
	if changed:
		config.save(PRESET_PATH)


## Connects or disconnects the reload notifications of the native half.
##
## A GDExtension reload - Project Settings > GDExtension > Reload (Godot 4.7+),
## or the editor's automatic reload when focus returns after a rebuild - unloads
## the library and loads it again. The engine runs the SCENE terminator and then
## the initializer, so the addon ends up with fresh DSContext/DSErrorHandler/
## DSProfiler/DSTextureCache singletons in their default (uninitialized) state.
## Nothing re-enters this plugin on the manual path: that tab calls
## GDExtensionManager.reload_extension() directly, which emits neither
## `extensions_reloaded` nor the deferred script reload the automatic path
## performs. Without this wiring the editor would keep an uninitialized backend
## until the first DirectStorage call lazily initializes it.
##
## `extension_loaded` (Godot 4.4+) is emitted per extension on both paths;
## `extensions_reloaded` (Godot 4.3+) only on the automatic path, and is kept so
## the older editors this addon supports still re-initialize after a reload.
func _sync_native_reload_signals(p_connect: bool) -> void:
	var manager: Object = Engine.get_singleton("GDExtensionManager")
	if manager == null:
		return

	if manager.has_signal("extension_loaded"):
		if p_connect and not manager.is_connected("extension_loaded", _on_native_extension_loaded):
			manager.connect("extension_loaded", _on_native_extension_loaded)
		elif not p_connect and manager.is_connected("extension_loaded", _on_native_extension_loaded):
			manager.disconnect("extension_loaded", _on_native_extension_loaded)

	if manager.has_signal("extensions_reloaded"):
		if p_connect and not manager.is_connected("extensions_reloaded", _on_native_extensions_reloaded):
			manager.connect("extensions_reloaded", _on_native_extensions_reloaded)
		elif not p_connect and manager.is_connected("extensions_reloaded", _on_native_extensions_reloaded):
			manager.disconnect("extensions_reloaded", _on_native_extensions_reloaded)


## Reports a missing, duplicated, or relocated native half once per plugin start.
##
## All three states look the same from GDScript - the editor simply has a ClassDB
## without the DirectStorage classes, or two libraries fighting over them ("class
## already registered" in the Output panel) - and all three are common install
## mistakes. Naming the manifest and the loaded paths turns a silent failure into
## something fixable. It lives in the editor plugin rather than ds_runtime.gd
## because that script is shared with exported games, where the extension is
## loaded from the PCK and there is no addon folder to compare against.
func _report_extension_state() -> void:
	var manager = Engine.get_singleton("GDExtensionManager")
	if manager == null:
		return

	var expected_dir := AUTOLOAD_PATH.get_base_dir().get_base_dir()
	var expected_manifest := _find_addon_manifest()
	var matching := PackedStringArray()
	for extension_path in manager.get_loaded_extensions():
		var config := ConfigFile.new()
		if config.load(extension_path) != OK:
			continue
		if String(config.get_value("configuration", "entry_symbol", "")) == EXTENSION_ENTRY_SYMBOL:
			matching.append(extension_path)

	if matching.size() > 1:
		push_warning(
			LOG_PREFIX + "%d copies of the native library are loaded and only one can own the DirectStorage classes. Disable or remove all but one: %s"
			% [matching.size(), ", ".join(matching)]
		)
		return

	if matching.size() == 1:
		if matching[0].get_base_dir().simplify_path() != expected_dir:
			push_warning(
				LOG_PREFIX + "The native library is loaded from %s but this addon expects %s. Disable the stale copy, then reload the extension."
				% [matching[0], expected_manifest if not expected_manifest.is_empty() else expected_dir]
			)
		return

	push_warning(
		LOG_PREFIX + "The native library is not loaded, so DSContext and the importer's native classes are unavailable. Build and deploy the addon, then use Project Settings > GDExtension > Reload (Godot 4.7+) or restart the editor. Expected manifest: %s"
		% (expected_manifest if not expected_manifest.is_empty() else expected_dir)
	)


## Returns this addon's .gdextension manifest, or "" when it is not on disk.
## The directory is fixed (the addon's preloads and autoload already assume it),
## but the file name is looked up so a renamed manifest keeps working. With more
## than one manifest present the choice has to be deterministic: prefer the one
## named after the addon folder, then the first in sorted order, because
## DirAccess lists in filesystem order and that order differs between hosts.
func _find_addon_manifest() -> String:
	var addon_dir := AUTOLOAD_PATH.get_base_dir().get_base_dir()
	var dir := DirAccess.open(addon_dir)
	if dir == null:
		return ""

	var candidates := PackedStringArray()
	dir.list_dir_begin()
	var entry := dir.get_next()
	while not entry.is_empty():
		if not dir.current_is_dir() and entry.get_extension() == "gdextension":
			candidates.append(entry)
		entry = dir.get_next()
	dir.list_dir_end()

	if candidates.is_empty():
		return ""
	candidates.sort()
	var preferred := addon_dir.get_file() + ".gdextension"
	if candidates.has(preferred):
		return addon_dir.path_join(preferred)
	return addon_dir.path_join(candidates[0])


## Re-initializes the backend when a single extension has been (re)loaded.
##
## Deferred on purpose: `GDExtensionManager::reload_extension()` emits this
## signal synchronously, from inside the editor's GDExtension tab handler, while
## the manager is still finishing the reload. Bringing a D3D12 device and the
## DirectStorage worker threads up inside that call would run arbitrary work in
## the middle of the manager's reload; the deferred call runs on idle instead,
## and it also collapses the two signals into one attempt.
func _on_native_extension_loaded(_p_extension: Variant) -> void:
	_queue_native_reinit()


## Re-initializes the backend after the editor reloaded one or more extensions.
## This is the automatic focus-in path, kept for Godot 4.3 where the per
## extension `extension_loaded` signal does not exist yet.
func _on_native_extensions_reloaded() -> void:
	_queue_native_reinit()


## Queues exactly one deferred re-initialization.
func _queue_native_reinit() -> void:
	if _native_reinit_queued:
		return
	_native_reinit_queued = true
	_reinitialize_native_backend_after_reload.call_deferred()


## Re-runs initialize_ds() when a reload left this editor session with an
## uninitialized singleton. Idempotent: an initialized context is left alone.
##
## The ClassDB check is the filter that makes an unrelated extension's load a
## no-op, so `extension_loaded` needs no identity matching on its argument:
## GDExtension exposes only get_minimum_library_initialization_level() and
## is_library_open() to GDScript, and its `resource_path` is still empty while
## the signal is emitted for a first load, because GDExtensionManager calls
## set_path() only after emitting it. DSContext exists only while this library
## is loaded, which is exactly the question that matters here.
func _reinitialize_native_backend_after_reload() -> void:
	_native_reinit_queued = false
	if not ClassDB.class_exists("DSContext"):
		return

	var ds_context = Engine.get_singleton("DSContext")
	if ds_context == null or ds_context.is_initialized():
		return

	if ds_context.initialize_ds():
		print(
			LOG_PREFIX + "Native extension reloaded; backend re-initialized (%s)."
			% ds_context.get_active_backend_name()
		)
	else:
		push_warning(
			LOG_PREFIX + "Native extension reloaded but the DirectStorage backend could not be re-initialized."
		)


## Warns once per editor start when the fallback is caused by a missing runtime.
##
## The CPU fallback is a supported mode, so the engine only prints the reason and
## a deliberately CPU-only project stays quiet. The backend is identified with
## `is_supported()` rather than by its name: the fallback backend reports
## "SoftwareFallback" through get_name(), which is not the string the engine's
## console message prints. `get_runtime_info()["version"]` is used instead of
## ["loaded"] on purpose: "loaded" means "these modules are mapped in this
## process", which is also false in a headless or non-D3D12 editor that has the
## DLLs on disk, so it would false-positive there. The version probe reads the
## file from the search directories without loading it and returns "Unavailable"
## only when dstoragecore.dll is not found; the non-Windows stub returns an empty
## dictionary, so this never fires on Linux or macOS.
func _warn_if_runtime_missing(p_ds_context: Object) -> void:
	if not p_ds_context.is_initialized() or p_ds_context.is_supported():
		return
	if String(p_ds_context.get_runtime_info().get("version", "")) != "Unavailable":
		return

	push_warning(
		LOG_PREFIX + "DirectStorage runtime DLLs are not installed, so this project runs on the CPU fallback (no NVMe acceleration, no GPU decompression). Open Tools > DSGD Tools... > Manage DirectStorage Runtime... to install one, then restart the editor."
	)


func _should_prompt_runtime_install_on_enable() -> bool:
	if not Engine.is_editor_hint() or EditorInterface.get_editor_main_screen() == null:
		return false
	if not ClassDB.class_exists("DSContext"):
		return false
	var ds_context = Engine.get_singleton("DSContext")
	var pending := String(RUNTIME_INSTALLER_SCRIPT.get_pending_version())
	return _is_runtime_missing_for_prompt(ds_context, pending, OS.get_name(), DisplayServer.get_name())


static func _is_runtime_missing_for_prompt(
	p_ds_context: Object,
	p_pending_version: String,
	p_os_name: String,
	p_display_name: String
) -> bool:
	if p_os_name != "Windows" or p_display_name == "headless":
		return false
	if not p_pending_version.is_empty():
		return false
	if p_ds_context == null or not p_ds_context.is_initialized() or p_ds_context.is_supported():
		return false
	return String(p_ds_context.get_runtime_info().get("version", "")) == "Unavailable"


## Registers the project-wide import profile used by the DirectStorage importer.
##
## The value is the default for new imports only; stored .import parameters
## always win on reimport. set_setting() is required before set_initial_value()
## (the latter errors on a missing property), and ProjectSettings.save() is
## deliberately not called: the default equals its initial value, so
## save_custom() skips it until the user actually changes it.
func _register_import_profile() -> void:
	# Migrate the pre-rename key before the tree becomes visible. The value is
	# carried over once; the legacy key is then removed so a two-segment
	# "directstorage/..." entry cannot recreate the old settings category.
	if ProjectSettings.has_setting(LEGACY_IMPORT_PROFILE_SETTING):
		if not ProjectSettings.has_setting(IMPORT_PROFILE_SETTING):
			ProjectSettings.set_setting(IMPORT_PROFILE_SETTING, ProjectSettings.get_setting(LEGACY_IMPORT_PROFILE_SETTING))
		ProjectSettings.clear(LEGACY_IMPORT_PROFILE_SETTING)

	if not ProjectSettings.has_setting(IMPORT_PROFILE_SETTING):
		ProjectSettings.set_setting(IMPORT_PROFILE_SETTING, 0)
	ProjectSettings.set_initial_value(IMPORT_PROFILE_SETTING, 0)
	ProjectSettings.add_property_info({
		"name": IMPORT_PROFILE_SETTING,
		"type": TYPE_INT,
		"hint": PROPERTY_HINT_ENUM,
		# Enum labels are comma-separated: a comma inside a label is parsed as
		# another item and produced four truncated choices instead of two.
		"hint_string": "3D (Streaming),CPU / ImageTexture (2D / UI / Decals)",
	})
	ProjectSettings.set_as_basic(IMPORT_PROFILE_SETTING, true)

	if ProjectSettings.has_setting("importer_defaults/directstorage_texture"):
		push_warning(
			LOG_PREFIX + "'importer_defaults/directstorage_texture' is set and overrides the project import profile for new imports."
		)


## Writes (or erases) an `autoload/` entry directly, then persists project.godot.
##
## `add_autoload_singleton()`/`remove_autoload_singleton()` are deliberately NOT
## used: they go through EditorAutoloadSettings, which wraps the change in a
## GLOBAL_HISTORY undo action ("Add Autoload"/"Remove Autoload"). Committing that
## sets the editor's global unsaved flag, which is what puts "(*)" in the window
## title and makes quitting ask to save modified resources - and unlike a scene
## edit it cannot be cleared by undo. Writing the setting has no undo entry, and
## saving immediately means the entry on disk matches the plugin state instead of
## waiting for the editor to save project.godot.
##
## An empty `p_path` removes the entry.
func _sync_autoload(p_name: String, p_path: String) -> void:
	var key := "autoload/" + p_name

	if p_path.is_empty():
		if not ProjectSettings.has_setting(key):
			return
		ProjectSettings.clear(key)
	elif ProjectSettings.has_setting(key):
		# Already registered (possibly as *uid://...) - leave the form alone.
		return
	else:
		ProjectSettings.set_setting(key, "*" + p_path)
		ProjectSettings.set_as_basic(key, true)

	var err := ProjectSettings.save()
	if err != OK:
		push_warning(
			LOG_PREFIX + "Could not persist %s to project.godot (error %d)." % [key, err]
		)


func _on_scene_closed(filepath: String) -> void:
	_checked_scenes.erase(filepath)


func _on_scene_changed(root: Node) -> void:
	if root == null:
		return
	var scene_path := root.scene_file_path
	if scene_path.is_empty() or _checked_scenes.has(scene_path):
		return

	_checked_scenes[scene_path] = true
	_check_scene_compatibility.call_deferred(root, scene_path)


func _check_scene_compatibility(root: Node, scene_path: String) -> void:
	if not is_instance_valid(root):
		return

	var lights: Array[Node] = []
	if root is Light3D:
		lights.append(root)
	lights.append_array(root.find_children("*", "Light3D", true, false))

	var decals: Array[Node] = []
	if root is Decal:
		decals.append(root)
	decals.append_array(root.find_children("*", "Decal", true, false))

	if lights.is_empty() and decals.is_empty():
		return

	var invalid_assets: Dictionary = {}
	var light_count := 0
	var decal_count := 0

	for node in lights:
		var light := node as Light3D
		if light and light.light_projector and light.light_projector is Texture2DRD:
			var path := light.light_projector.resource_path
			invalid_assets[path if not path.is_empty() else "(unnamed Texture2DRD)"] = true
			light_count += 1

	for node in decals:
		var decal := node as Decal
		if decal:
			for i in 4:
				var tex := decal.get_texture(i)
				if tex and tex is Texture2DRD:
					var path := tex.resource_path
					invalid_assets[path if not path.is_empty() else "(unnamed Texture2DRD)"] = true
					decal_count += 1

	if invalid_assets.is_empty():
		return

	var msg := "[color=yellow]" + LOG_PREFIX.strip_edges() + "[/color] Scene [b]%s[/b] has " % scene_path.get_file()
	var parts: Array[String] = []
	if light_count > 0:
		parts.append("%d Light3D projector(s)" % light_count)
	if decal_count > 0:
		parts.append("%d Decal texture(s)" % decal_count)
	msg += " and ".join(parts) + " using [b]Texture2DRD[/b].\n"
	msg += "To eliminate atlas engine warnings, re-import the following texture(s) with [b]compatibility/as_image_texture[/b] enabled in the Import dock:\n"
	for asset_path in invalid_assets:
		msg += "  • %s\n" % asset_path

	print_rich(msg)


## Opens the pack scan dialog. A fresh window is instantiated per invocation and
## the dialog frees itself on close, so no half-finished scan survives between
## runs or between plugin reloads.
##
## The window is parented to the editor's base Control rather than shown through
## EditorInterface.popup_dialog_centered(). That helper parents it to EditorNode,
## which is a plain Node, and theme inheritance chains are broken by nodes that
## are neither Control nor Window - the dialog then renders with the default
## theme no matter what is assigned to its own `theme` property. gui_base is
## themed, and is exactly where the editor attaches its own dialogs.
func _on_verify_exported_pack() -> void:
	var dialog_scene := load(VERIFY_DIALOG_SCENE) as PackedScene
	if dialog_scene == null:
		push_error(LOG_PREFIX + "Pack scan dialog missing at " + VERIFY_DIALOG_SCENE_PATH)
		return
	var dialog := dialog_scene.instantiate() as Window
	_verify_dialog = dialog
	var base := EditorInterface.get_base_control()
	if base == null:
		# Extremely unlikely; keep the API path working rather than dropping the
		# request on the floor.
		EditorInterface.popup_dialog_centered(dialog)
		return
	base.add_child(dialog)
	dialog.popup_centered()


## Applies a staged runtime before the first DirectStorage call. Windows keeps
## loaded modules locked, so an install only stages the files; this is where the
## swap actually happens on the next editor start.
func _apply_pending_runtime() -> void:
	var error: int = RUNTIME_INSTALLER_SCRIPT.apply_pending()
	if error != OK:
		push_warning(
			LOG_PREFIX + "A staged runtime update could not be applied (%s); it will be retried on the next editor start." % error_string(error)
		)


## Opens the runtime manager dialog, reusing the window between invocations and
## parenting it to the themed editor base Control (same reason as the pack scan
## dialog).
func _on_runtime_menu() -> void:
	# Rebuild the dialog on every open. The editor caches loaded scenes, so a
	# dialog instance created before an addon update would keep its old node
	# tree while the hot-reloaded script expects the new one.
	if is_instance_valid(_runtime_dialog):
		_runtime_dialog.queue_free()
		_runtime_dialog = null
	var dialog_scene := ResourceLoader.load(RUNTIME_DIALOG_SCENE_PATH, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	if dialog_scene == null:
		push_error(LOG_PREFIX + "Runtime manager dialog missing at " + RUNTIME_DIALOG_SCENE_PATH)
		return
	_runtime_dialog = dialog_scene.instantiate() as Window
	var base := EditorInterface.get_base_control()
	if base == null:
		EditorInterface.popup_dialog_centered(_runtime_dialog)
		return
	base.add_child(_runtime_dialog)
	if _runtime_dialog.has_method("open"):
		_runtime_dialog.call("open")


## Opens the migration tool dialog, rebuilt on every open so a hot-reloaded script
## never meets a stale node tree (same as runtime menu).
func _on_migration_menu() -> void:
	if is_instance_valid(_migration_dialog):
		_migration_dialog.queue_free()
		_migration_dialog = null
	var dialog_scene := ResourceLoader.load(MIGRATION_DIALOG_SCENE_PATH, "PackedScene", ResourceLoader.CACHE_MODE_IGNORE) as PackedScene
	if dialog_scene == null:
		push_error(LOG_PREFIX + "Migration dialog missing at " + MIGRATION_DIALOG_SCENE_PATH)
		return
	_migration_dialog = dialog_scene.instantiate() as Window
	var base := EditorInterface.get_base_control()
	if base == null:
		EditorInterface.popup_dialog_centered(_migration_dialog)
		return
	base.add_child(_migration_dialog)
	if _migration_dialog.has_method("open"):
		_migration_dialog.call("open")


## Registers the DirectStorage entries as one Tools submenu instead of two flat
## items. The editor owns a registered submenu (remove_tool_menu_item() deletes
## it), so the plugin keeps only a reference and drops it on exit.
func _add_tools_submenu() -> void:
	if _tools_submenu != null:
		return
	_tools_submenu = PopupMenu.new()
	_tools_submenu.add_item(VERIFY_MENU_ITEM, TOOLS_SUBMENU_VERIFY_ID)
	_tools_submenu.add_item(RUNTIME_MENU_ITEM, TOOLS_SUBMENU_RUNTIME_ID)
	_tools_submenu.add_item(MIGRATION_MENU_ITEM, TOOLS_SUBMENU_MIGRATION_ID)
	_tools_submenu.id_pressed.connect(_on_tools_submenu_pressed)
	add_tool_submenu_item(TOOLS_SUBMENU_NAME, _tools_submenu)


func _on_tools_submenu_pressed(id: int) -> void:
	match id:
		TOOLS_SUBMENU_VERIFY_ID:
			_on_verify_exported_pack()
		TOOLS_SUBMENU_RUNTIME_ID:
			_on_runtime_menu()
		TOOLS_SUBMENU_MIGRATION_ID:
			_on_migration_menu()
