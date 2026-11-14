@tool
extends Window

## Front-end UI for the DirectStorage texture migration tool.
##
## Displays a folder-hierarchy Tree with checkboxes (matching Godot's export dialog
## UX), allowing users to filter by directory and select which textures to migrate
## or keep. Backs up original .import files into a ZIP archive, rewrites .import
## files, coordinates batch reimports, verifies artifacts, auto-deletes replaced
## .ctex cache, and provides one-click rollback.

const CORE = preload("res://addons/directstorage_accelerator/tools/ds_migration_core.gd")

const BATCH_SIZE := 50
## ConfirmationDialog modes: the dialog is shared by the restore confirmation and
## the "restart the editor" prompt that follows a committed restore.
const CONFIRM_RESTORE := "restore"
const CONFIRM_RESTART := "restart"
## Upper bound for letting the editor's own scan/import settle before restarting.
const RESTART_SETTLE_TIMEOUT_MS := 30000
## Cancel button of the restart prompt: declining the restart reimports in-session.
const RESTART_CANCEL_TEXT := "Reimport without restarting"
const ACTION_LABELS := [
	"Skip (Keep Texture2D)",
	"Migrate - VRAM Compressed",
	"Migrate - VRAM Uncompressed",
	"Migrate - Normal Map",
]

@onready var _background: Panel = $Background
@onready var _layout: VBoxContainer = $Layout

# Controls
@onready var _filter_edit: LineEdit = %FilterEdit
@onready var _status_filter_option: OptionButton = %StatusFilterOption
@onready var _select_all_button: Button = %SelectAllButton
@onready var _deselect_all_button: Button = %DeselectAllButton
@onready var _expand_all_button: Button = %ExpandAllButton
@onready var _collapse_all_button: Button = %CollapseAllButton

@onready var _filter_banner: PanelContainer = %FilterBanner
@onready var _filter_info_label: Label = %FilterInfoLabel
@onready var _show_all_button: Button = %ShowAllButton

@onready var _content_split: VSplitContainer = %ContentSplit
@onready var _asset_tree: Tree = %AssetTree
@onready var _summary_label: Label = %SummaryLabel
@onready var _auto_delete_check: CheckBox = %AutoDeleteCheck

@onready var _progress_bar: ProgressBar = %ProgressBar
@onready var _log_label: RichTextLabel = %LogLabel

@onready var _scan_button: Button = %ScanButton
@onready var _restore_button: Button = %RestoreButton
@onready var _migrate_button: Button = %MigrateButton
@onready var _close_button: Button = %CloseButton

@onready var _restore_file_dialog: FileDialog = %RestoreFileDialog
@onready var _confirm_dialog: ConfirmationDialog = %ConfirmDialog

var _plan: Array[Dictionary] = []
var _file_items: Array[TreeItem] = []
var _is_busy := false
var _pending_restore_zip := ""
## Which action the shared confirmation dialog is confirming (CONFIRM_*).
var _pending_action := CONFIRM_RESTORE
## Sources the last restore put back on disk; reimported if the restart is skipped.
var _restored_sources: Array[String] = []
var _action_popup: PopupMenu
var _current_editing_item: TreeItem = null
var _last_closed_item: TreeItem = null
var _closed_by_toggle := false
var _popup_action_applied := false
var _is_scrolling := false
var _popup_just_closed_time: int = 0
var _popup_just_opened_time: int = 0


func _ready() -> void:
	_apply_editor_theme()

	close_requested.connect(_on_close_requested)
	_close_button.pressed.connect(hide)
	_scan_button.pressed.connect(_on_scan_pressed)
	_migrate_button.pressed.connect(_on_migrate_pressed)
	_restore_button.pressed.connect(_on_restore_pressed)

	_select_all_button.pressed.connect(_on_select_all_pressed)
	_deselect_all_button.pressed.connect(_on_deselect_all_pressed)
	_expand_all_button.pressed.connect(_on_expand_all_pressed)
	_collapse_all_button.pressed.connect(_on_collapse_all_pressed)
	_show_all_button.pressed.connect(_on_show_all_pressed)

	_filter_edit.text_changed.connect(_on_filter_changed)
	_status_filter_option.item_selected.connect(_on_status_filter_selected)

	_asset_tree.item_edited.connect(_on_tree_item_edited)
	_asset_tree.custom_popup_edited.connect(_on_tree_custom_popup_edited)
	_asset_tree.gui_input.connect(_on_tree_gui_input)
	for child in _asset_tree.get_children(true):
		if child is ScrollBar:
			child.value_changed.connect(_on_tree_scrolled)

	_restore_file_dialog.file_selected.connect(_on_restore_file_selected)
	_confirm_dialog.confirmed.connect(_on_confirm_dialog_confirmed)
	_confirm_dialog.canceled.connect(_on_confirm_dialog_canceled)

	_setup_tree_columns()
	_setup_status_options()
	_setup_action_popup()


func open() -> void:
	if not is_node_ready():
		await ready
	_on_scan_pressed()
	popup_centered()


func _on_close_requested() -> void:
	if not _is_busy:
		hide()


func _setup_tree_columns() -> void:
	_asset_tree.columns = 3
	_asset_tree.column_titles_visible = true
	_asset_tree.set_column_title(0, "Asset / Directory")
	_asset_tree.set_column_title(1, "Action")
	_asset_tree.set_column_title(2, "Details")
	_asset_tree.set_column_expand(0, true)
	_asset_tree.set_column_expand(1, false)
	_asset_tree.set_column_custom_minimum_width(1, 260)
	_asset_tree.set_column_expand(2, true)


func _setup_status_options() -> void:
	_status_filter_option.clear()
	_status_filter_option.add_item("All Assets", 0)
	_status_filter_option.add_item("Eligible Only", 1)
	_status_filter_option.add_item("Adopt Only", 2)
	_status_filter_option.add_item("Upgrade Only", 3)
	_status_filter_option.add_item("Up to Date Only", 4)
	_status_filter_option.add_item("Skipped Only", 5)


# ==============================================================================
# Editor Theme Integration
# ==============================================================================

func _apply_editor_theme() -> void:
	if not Engine.is_editor_hint():
		return
	_apply_editor_theme_look()
	var scale := EditorInterface.get_editor_scale()
	if is_equal_approx(scale, 1.0):
		return
	content_scale_factor = scale
	min_size = Vector2i(Vector2(min_size) * scale)
	size = Vector2i(Vector2(size) * scale)


func _apply_editor_theme_look() -> void:
	var editor_theme: Theme = EditorInterface.get_editor_theme()
	theme = editor_theme
	_layout.theme = editor_theme
	_restore_file_dialog.theme = editor_theme
	_confirm_dialog.theme = editor_theme
	if _action_popup != null:
		_action_popup.theme = editor_theme

	var dialog_style: StyleBox = editor_theme.get_stylebox("panel", "AcceptDialog")
	if dialog_style != null:
		_background.add_theme_stylebox_override("panel", dialog_style)
		_layout.offset_left = dialog_style.get_margin(SIDE_LEFT)
		_layout.offset_top = dialog_style.get_margin(SIDE_TOP)
		_layout.offset_right = -dialog_style.get_margin(SIDE_RIGHT)
		_layout.offset_bottom = -dialog_style.get_margin(SIDE_BOTTOM)


func _strip_editor_theme() -> void:
	theme = null
	_layout.theme = null
	_restore_file_dialog.theme = null
	_confirm_dialog.theme = null
	if _action_popup != null:
		_action_popup.theme = null
	_background.remove_theme_stylebox_override("panel")


func _notification(what: int) -> void:
	if not Engine.is_editor_hint() or not is_node_ready():
		return
	if what == NOTIFICATION_EDITOR_PRE_SAVE:
		_strip_editor_theme()
	elif what == NOTIFICATION_EDITOR_POST_SAVE:
		_apply_editor_theme_look()


# ==============================================================================
# Scanning & Tree Building
# ==============================================================================

func _on_scan_pressed() -> void:
	if _is_busy:
		return
	_log("Scanning project for textures...")
	_plan = CORE.scan()
	_rebuild_tree()
	_log("Scan finished. Discovered %d textures." % _plan.size())


func _get_folder_icon() -> Texture2D:
	if Engine.is_editor_hint() and EditorInterface.get_base_control() != null:
		var theme := EditorInterface.get_editor_theme()
		if theme.has_icon("Folder", "EditorIcons"):
			return theme.get_icon("Folder", "EditorIcons")
	return null


func _get_file_icon(ext: String) -> Texture2D:
	if Engine.is_editor_hint() and EditorInterface.get_base_control() != null:
		var theme := EditorInterface.get_editor_theme()
		if theme.has_icon("ImageTexture", "EditorIcons"):
			return theme.get_icon("ImageTexture", "EditorIcons")
		elif theme.has_icon("File", "EditorIcons"):
			return theme.get_icon("File", "EditorIcons")
	return null


func _rebuild_tree() -> void:
	_asset_tree.clear()
	_file_items.clear()

	var root_item := _asset_tree.create_item()
	root_item.set_cell_mode(0, TreeItem.CELL_MODE_CHECK)
	root_item.set_text(0, "res://")
	root_item.set_icon(0, _get_folder_icon())
	root_item.set_editable(0, true)
	root_item.set_metadata(0, {"is_dir": true, "path": "res://"})

	var dir_items: Dictionary = {"res://": root_item}
	var folder_icon := _get_folder_icon()

	for entry in _plan:
		var src_res: String = entry.get("source", "")
		var clean_path := src_res.trim_prefix("res://").trim_prefix("/")
		var parts := clean_path.split("/")
		var current_path := "res://"
		var parent_item := root_item

		# Create folder hierarchy
		for i in range(parts.size() - 1):
			var dir_name := parts[i]
			current_path = current_path.path_join(dir_name)
			if not dir_items.has(current_path):
				var dir_item := _asset_tree.create_item(parent_item)
				dir_item.set_cell_mode(0, TreeItem.CELL_MODE_CHECK)
				dir_item.set_text(0, dir_name + "/")
				dir_item.set_icon(0, folder_icon)
				dir_item.set_editable(0, true)
				dir_item.set_metadata(0, {"is_dir": true, "path": current_path})
				dir_items[current_path] = dir_item
			parent_item = dir_items[current_path]

		# Create file item
		var file_name := parts[parts.size() - 1]
		var file_item := _asset_tree.create_item(parent_item)
		file_item.set_cell_mode(0, TreeItem.CELL_MODE_CHECK)
		file_item.set_text(0, file_name)
		file_item.set_icon(0, _get_file_icon(file_name.get_extension()))
		file_item.set_tooltip_text(0, src_res)
		file_item.set_metadata(0, {"is_dir": false, "entry": entry})
		_file_items.append(file_item)

		var mode: String = entry.get("mode", "skip")
		var eligible: bool = entry.get("eligible", false)
		var reason: String = entry.get("reason", "")
		var iver: int = entry.get("importer_version", 0)

		if mode == "up_to_date":
			file_item.set_cell_mode(1, TreeItem.CELL_MODE_STRING)
			file_item.set_text(1, "Up to date (DirectStorage Texture)")
			file_item.set_editable(1, false)
			file_item.set_text(2, "Valid .dstex artifact verified")
			file_item.set_checked(0, false)
			file_item.set_editable(0, false)
			file_item.set_selectable(0, false)
			var disabled_color := Color(1, 1, 1, 0.4)
			file_item.set_custom_color(0, disabled_color)
			file_item.set_custom_color(1, disabled_color)
			file_item.set_custom_color(2, disabled_color)
		else:
			# Interactive Custom Dropdown Button (CELL_MODE_CUSTOM)
			file_item.set_cell_mode(1, TreeItem.CELL_MODE_CUSTOM)
			file_item.set_custom_as_button(1, true)
			file_item.set_editable(1, true)
			file_item.set_tooltip_text(1, "Click to choose migration action")

			var action_idx := 0
			if eligible:
				var mapped_params: Dictionary = entry.get("mapped_params", {})
				action_idx = CORE.action_for_params(mapped_params)

				file_item.set_text(1, ACTION_LABELS[action_idx])
				file_item.set_checked(0, true)
				file_item.set_editable(0, true)
				file_item.set_selectable(0, true)
				file_item.set_text(2, reason)
			else:
				action_idx = 0 # Skip
				file_item.set_text(1, ACTION_LABELS[action_idx])
				file_item.set_checked(0, false)
				file_item.set_editable(0, false)
				file_item.set_selectable(0, true)
				file_item.set_text(2, reason)
				var disabled_color := Color(1, 1, 1, 0.4)
				file_item.set_custom_color(0, disabled_color)
				file_item.set_custom_color(2, disabled_color)

	_update_folder_check_states(root_item)
	_apply_filters()
	_update_summary_and_actions()


# ==============================================================================
# Filtering & Search
# ==============================================================================

func _on_filter_changed(_new_text: String) -> void:
	_apply_filters()


func _on_status_filter_selected(_idx: int) -> void:
	_apply_filters()


func _on_show_all_pressed() -> void:
	_filter_edit.text = ""
	_status_filter_option.selected = 0
	_apply_filters()


func _apply_filters() -> void:
	var query := _filter_edit.text.strip_edges().to_lower()
	var status_filter_id := _status_filter_option.get_selected_id()

	var visible_count := 0
	var total := _file_items.size()

	for file_item in _file_items:
		var meta: Dictionary = file_item.get_metadata(0)
		var entry: Dictionary = meta.get("entry", {})
		var src_path := str(entry.get("source", "")).to_lower()
		var file_name := file_item.get_text(0).to_lower()
		var mode: String = entry.get("mode", "skip")
		var eligible: bool = entry.get("eligible", false)

		var text_match := query.is_empty() or src_path.contains(query) or file_name.contains(query)
		var status_match := true

		match status_filter_id:
			1: # Eligible Only
				status_match = eligible
			2: # Adopt Only
				status_match = (mode == "adopt")
			3: # Upgrade Only
				status_match = (mode == "upgrade")
			4: # Up to date Only
				status_match = (mode == "up_to_date")
			5: # Skipped Only
				status_match = (mode == "skip")

		var visible := text_match and status_match
		file_item.visible = visible
		if visible:
			visible_count += 1

	# Recursively propagate visibility to directories
	_propagate_dir_visibility(_asset_tree.get_root())

	# Update subtle filter banner
	var hidden_count := total - visible_count
	if hidden_count > 0:
		_filter_banner.visible = true
		_filter_info_label.text = "Showing %d of %d textures (%d hidden by current filter)." % [visible_count, total, hidden_count]
	else:
		_filter_banner.visible = false


func _propagate_dir_visibility(item: TreeItem) -> bool:
	if item == null:
		return false
	var meta: Dictionary = item.get_metadata(0)
	if not meta.get("is_dir", false):
		return item.visible

	var any_child_visible := false
	var child := item.get_first_child()
	while child != null:
		if _propagate_dir_visibility(child):
			any_child_visible = true
		child = child.get_next()

	item.visible = any_child_visible
	return any_child_visible


# ==============================================================================
# Selection & Tree Interactions
# ==============================================================================

func _setup_action_popup() -> void:
	_action_popup = PopupMenu.new()
	_action_popup.name = "ActionPopupMenu"
	for i in range(ACTION_LABELS.size()):
		_action_popup.add_item(ACTION_LABELS[i], i)
	_action_popup.id_pressed.connect(_on_action_popup_id_pressed)
	_action_popup.popup_hide.connect(_on_action_popup_hide)
	add_child(_action_popup)


func _on_tree_gui_input(event: InputEvent) -> void:
	if not (event is InputEventMouseButton):
		return
	var mb := event as InputEventMouseButton

	# Mouse wheel -> Hide popup when scrolling
	if mb.button_index in [MOUSE_BUTTON_WHEEL_UP, MOUSE_BUTTON_WHEEL_DOWN, MOUSE_BUTTON_WHEEL_LEFT, MOUSE_BUTTON_WHEEL_RIGHT] and mb.is_pressed():
		if _action_popup != null and _action_popup.visible:
			_is_scrolling = true
			_action_popup.hide()
		return

	# Left mouse button click directly on action cell
	if mb.button_index == MOUSE_BUTTON_LEFT and mb.is_pressed():
		var m_pos := _asset_tree.get_local_mouse_position()
		var clicked_item := _asset_tree.get_item_at_position(m_pos)
		var clicked_col := _asset_tree.get_column_at_position(m_pos)

		if clicked_item != null and clicked_col == 1:
			var meta: Dictionary = clicked_item.get_metadata(0)
			if not meta.get("is_dir", false):
				_asset_tree.accept_event()
				_toggle_or_open_popup_for_item(clicked_item)


func _on_tree_custom_popup_edited(_arrow_clicked: bool) -> void:
	var item := _asset_tree.get_edited()
	if item != null and _asset_tree.get_edited_column() == 1:
		var meta: Dictionary = item.get_metadata(0)
		if not meta.get("is_dir", false):
			_toggle_or_open_popup_for_item(item)


func _on_tree_scrolled(_val: float) -> void:
	if _action_popup != null and _action_popup.visible:
		_is_scrolling = true
		_action_popup.hide()


func _toggle_or_open_popup_for_item(item: TreeItem) -> void:
	if item == null:
		return
	var meta: Dictionary = item.get_metadata(0)
	if meta.get("is_dir", false):
		return

	# 1. If popup is currently visible:
	if _action_popup != null and _action_popup.visible:
		if _current_editing_item == item:
			# Clicked SAME button that is already open -> TOGGLE CLOSE!
			_closed_by_toggle = true
			_last_closed_item = item
			_current_editing_item = null
			_popup_just_closed_time = Time.get_ticks_msec()
			_action_popup.hide()
			return
		else:
			# Clicked DIFFERENT button -> QUICK SWITCH!
			_action_popup.hide()
			_open_action_popup_for_item(item)
			return

	# 2. If popup is NOT currently visible:
	var now := Time.get_ticks_msec()
	# Debounce if it was just closed by clicking this same item (prevent duplicate event on same click)
	if (_current_editing_item == item or _last_closed_item == item) and (now - _popup_just_closed_time < 80):
		return

	# Open fresh popup for this item
	_open_action_popup_for_item(item)


func _open_action_popup_for_item(item: TreeItem) -> void:
	if item == null:
		return
	var meta: Dictionary = item.get_metadata(0)
	if meta.get("is_dir", false):
		return

	# Visually select the item row in the tree
	item.select(0)

	_current_editing_item = item
	_last_closed_item = null
	_closed_by_toggle = false
	_popup_action_applied = false
	_popup_just_opened_time = Time.get_ticks_msec()

	# Position popup right below the cell relative to Window parent
	var cell_rect := _asset_tree.get_item_area_rect(item, 1)
	var local_pos := Vector2i(_asset_tree.global_position) + Vector2i(int(cell_rect.position.x), int(cell_rect.position.y + cell_rect.size.y))
	var popup_size := Vector2i(int(cell_rect.size.x), 0)

	_action_popup.popup_on_parent(Rect2i(local_pos, popup_size))


func _on_action_popup_hide() -> void:
	_popup_just_closed_time = Time.get_ticks_msec()
	if _current_editing_item != null:
		_last_closed_item = _current_editing_item
	_deferred_check_quick_switch.call_deferred()


func _deferred_check_quick_switch() -> void:
	if _closed_by_toggle:
		_closed_by_toggle = false
		_current_editing_item = null
		return

	if _popup_action_applied:
		_popup_action_applied = false
		_current_editing_item = null
		return

	if _is_scrolling:
		_is_scrolling = false
		_current_editing_item = null
		return

	var closing_item := _current_editing_item if _current_editing_item != null else _last_closed_item
	_current_editing_item = null

	# If mouse is inside the popup menu area, this was a click on the popup, not an underlying tree item
	if _action_popup != null:
		var popup_rect := Rect2(_action_popup.position, _action_popup.size)
		var win_mouse_pos := get_viewport().get_mouse_position()
		if popup_rect.has_point(win_mouse_pos):
			return

	var m_pos := _asset_tree.get_local_mouse_position()
	var clicked_item := _asset_tree.get_item_at_position(m_pos)
	var clicked_col := _asset_tree.get_column_at_position(m_pos)

	if clicked_item != null and clicked_col == 1 and clicked_item != closing_item:
		var meta: Dictionary = clicked_item.get_metadata(0)
		if not meta.get("is_dir", false):
			_open_action_popup_for_item(clicked_item)


func _on_action_popup_id_pressed(action_idx: int) -> void:
	if _current_editing_item == null and _last_closed_item == null:
		return
	_popup_action_applied = true
	var target_item := _current_editing_item if _current_editing_item != null else _last_closed_item
	_current_editing_item = null
	_last_closed_item = null

	var meta: Dictionary = target_item.get_metadata(0)
	var entry: Dictionary = meta.get("entry", {})
	_handle_action_selected(target_item, entry, action_idx)
	_update_folder_check_states(_asset_tree.get_root())
	_update_summary_and_actions()


func _on_tree_item_edited() -> void:
	var item := _asset_tree.get_edited()
	if item == null:
		return
	var col := _asset_tree.get_edited_column()
	var meta: Dictionary = item.get_metadata(0)

	if col == 0:
		# Checkbox toggled
		if meta.get("is_dir", false):
			var want_checked := item.is_checked(0)
			_set_dir_checked_recursive(item, want_checked)
		else:
			var entry: Dictionary = meta.get("entry", {})
			if not bool(entry.get("eligible", false)) and item.is_checked(0):
				item.set_checked(0, false)
		_update_folder_check_states(_asset_tree.get_root())
		_update_summary_and_actions()

	elif col == 1:
		# Action cell clicked!
		if not meta.get("is_dir", false):
			_toggle_or_open_popup_for_item(item)


func _handle_action_selected(item: TreeItem, entry: Dictionary, action_idx: int) -> void:
	item.set_text(1, ACTION_LABELS[action_idx])
	match action_idx:
		CORE.ACTION_SKIP: # Skip (Keep Texture2D)
			entry["eligible"] = false
			entry["mode"] = "skip"
			item.set_checked(0, false)
			item.set_editable(0, false)
			item.set_text(2, "Skipped by user (kept on default texture importer)")
			var disabled_color := Color(1, 1, 1, 0.4)
			item.set_custom_color(0, disabled_color)
			item.set_custom_color(2, disabled_color)

		CORE.ACTION_VRAM_COMPRESSED: # Migrate: VRAM Compressed
			entry["eligible"] = true
			entry["mode"] = CORE.action_mode_for_entry(entry, CORE.ACTION_VRAM_COMPRESSED)
			entry["mapped_params"] = CORE.action_mapped_params(CORE.ACTION_VRAM_COMPRESSED)
			item.set_checked(0, true)
			item.set_editable(0, true)
			item.set_text(2, "VRAM Compressed (BC/S3TC) -> compression/mode=0")
			item.clear_custom_color(0)
			item.clear_custom_color(2)

		CORE.ACTION_VRAM_UNCOMPRESSED: # Migrate: VRAM Uncompressed
			entry["eligible"] = true
			entry["mode"] = CORE.action_mode_for_entry(entry, CORE.ACTION_VRAM_UNCOMPRESSED)
			entry["mapped_params"] = CORE.action_mapped_params(CORE.ACTION_VRAM_UNCOMPRESSED)
			item.set_checked(0, true)
			item.set_editable(0, true)
			item.set_text(2, "VRAM Uncompressed -> compression/mode=1")
			item.clear_custom_color(0)
			item.clear_custom_color(2)

		CORE.ACTION_NORMAL_MAP: # Migrate: Normal Map
			entry["eligible"] = true
			entry["mode"] = CORE.action_mode_for_entry(entry, CORE.ACTION_NORMAL_MAP)
			entry["mapped_params"] = CORE.action_mapped_params(CORE.ACTION_NORMAL_MAP)
			item.set_checked(0, true)
			item.set_editable(0, true)
			item.set_text(2, "Normal Map -> compress/normal_map=1, compression/color_space=0")
			item.clear_custom_color(0)
			item.clear_custom_color(2)


func _set_dir_checked_recursive(item: TreeItem, checked: bool) -> void:
	if item == null:
		return
	var meta: Dictionary = item.get_metadata(0)
	if not meta.get("is_dir", false):
		var entry: Dictionary = meta.get("entry", {})
		if bool(entry.get("eligible", false)):
			item.set_checked(0, checked)
			item.set_indeterminate(0, false)
		else:
			item.set_checked(0, false)
			item.set_indeterminate(0, false)
	else:
		item.set_indeterminate(0, false)
		item.set_checked(0, checked)
		var child := item.get_first_child()
		while child != null:
			_set_dir_checked_recursive(child, checked)
			child = child.get_next()


func _update_folder_check_states(item: TreeItem) -> Dictionary:
	if item == null:
		return {"total": 0, "checked": 0}

	var meta: Dictionary = item.get_metadata(0)
	if not meta.get("is_dir", false):
		var entry: Dictionary = meta.get("entry", {})
		if not bool(entry.get("eligible", false)):
			return {"total": 0, "checked": 0}
		return {"total": 1, "checked": 1 if item.is_checked(0) else 0}

	var total_eligible := 0
	var checked_eligible := 0
	var child := item.get_first_child()
	while child != null:
		var res := _update_folder_check_states(child)
		total_eligible += res["total"]
		checked_eligible += res["checked"]
		child = child.get_next()

	if total_eligible == 0:
		item.set_checked(0, false)
		item.set_indeterminate(0, false)
	elif checked_eligible == 0:
		item.set_checked(0, false)
		item.set_indeterminate(0, false)
	elif checked_eligible == total_eligible:
		item.set_checked(0, true)
		item.set_indeterminate(0, false)
	else:
		item.set_checked(0, false)
		item.set_indeterminate(0, true)

	return {"total": total_eligible, "checked": checked_eligible}


func _on_select_all_pressed() -> void:
	for item in _file_items:
		var meta: Dictionary = item.get_metadata(0)
		var entry: Dictionary = meta.get("entry", {})
		if bool(entry.get("eligible", false)):
			item.set_checked(0, true)
			item.set_indeterminate(0, false)
		else:
			item.set_checked(0, false)
			item.set_indeterminate(0, false)
	_update_folder_check_states(_asset_tree.get_root())
	_update_summary_and_actions()


func _on_deselect_all_pressed() -> void:
	for item in _file_items:
		item.set_checked(0, false)
		item.set_indeterminate(0, false)
	_update_folder_check_states(_asset_tree.get_root())
	_update_summary_and_actions()


func _on_expand_all_pressed() -> void:
	_set_collapsed_recursive(_asset_tree.get_root(), false)


func _on_collapse_all_pressed() -> void:
	_set_collapsed_recursive(_asset_tree.get_root(), true)
	if _asset_tree.get_root():
		_asset_tree.get_root().collapsed = false


func _set_collapsed_recursive(item: TreeItem, collapsed: bool) -> void:
	if item == null:
		return
	item.collapsed = collapsed
	var child := item.get_first_child()
	while child != null:
		_set_collapsed_recursive(child, collapsed)
		child = child.get_next()


func _get_checked_entries() -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for item in _file_items:
		if item.is_checked(0):
			var meta: Dictionary = item.get_metadata(0)
			var entry: Dictionary = meta.get("entry", {})
			if bool(entry.get("eligible", false)):
				out.append(entry)
	return out


func _update_summary_and_actions() -> void:
	var total := _plan.size()
	var eligible_count := 0
	var selected_count := 0
	var up_to_date_count := 0
	var skip_count := 0

	for entry in _plan:
		var mode: String = entry.get("mode", "skip")
		if bool(entry.get("eligible", false)):
			eligible_count += 1
		elif mode == "up_to_date":
			up_to_date_count += 1
		else:
			skip_count += 1

	for item in _file_items:
		if item.is_checked(0):
			var meta: Dictionary = item.get_metadata(0)
			var entry: Dictionary = meta.get("entry", {})
			if bool(entry.get("eligible", false)):
				selected_count += 1

	_summary_label.text = "Discovered: %d | Eligible: %d | Selected: %d | Up to date: %d | Skipped: %d" % [
		total, eligible_count, selected_count, up_to_date_count, skip_count
	]
	_migrate_button.text = "Migrate Selected (%d)" % selected_count
	_migrate_button.disabled = (_is_busy or selected_count == 0)


# ==============================================================================
# Migration Execution & Reimport
# ==============================================================================

func _log(msg: String) -> void:
	_log_label.visible = true
	_log_label.append_text(msg + "\n")
	if msg.begins_with("[ERROR]"):
		push_error("[DirectStorage Migration] " + msg)
	elif msg.begins_with("[WARN]"):
		push_warning("[DirectStorage Migration] " + msg)


func _set_busy(busy: bool) -> void:
	_is_busy = busy
	_scan_button.disabled = busy
	_restore_button.disabled = busy
	_migrate_button.disabled = busy or (_get_checked_entries().is_empty())
	_progress_bar.visible = busy


func _on_migrate_pressed() -> void:
	var selected := _get_checked_entries()
	if selected.is_empty():
		return

	var fs := EditorInterface.get_resource_filesystem()
	if fs != null and fs.is_scanning():
		_log("[WARN] Cannot start migration: EditorFileSystem is currently scanning. Please wait and retry.")
		return

	_set_busy(true)
	_progress_bar.value = 5.0

	_log("--- Starting Migration of %d Textures ---" % selected.size())

	# 1. Create ZIP backup
	_log("Step 1/5: Packing original .import files into ZIP archive...")
	var backup_res: Dictionary = CORE.create_zip_backup(selected)
	if not bool(backup_res.get("ok", false)):
		_log("[ERROR] Backup failed: %s" % backup_res.get("error", "Unknown error"))
		_set_busy(false)
		return

	_log("ZIP backup created: %s" % backup_res.get("zip_res_path", ""))
	_progress_bar.value = 25.0
	await get_tree().process_frame

	# 2. Mutate .import files
	_log("Step 2/5: Rewriting .import sidecars (preserving exact UIDs)...")
	var apply_res: Dictionary = CORE.apply_migration(selected)
	var to_reimport: Array[String] = apply_res.get("reimport_sources", [])
	if not bool(apply_res.get("ok", false)):
		for f in apply_res.get("failed", []):
			_log("[WARN] %s: %s" % [f.get("source", "").get_file(), f.get("error", "")])

	_progress_bar.value = 40.0
	await get_tree().process_frame

	# 3. Uncache live CompressedTexture2D instances before reimport to prevent "Bad header" errors
	for src in to_reimport:
		if ResourceLoader.has_cached(src):
			var cached := ResourceLoader.load(src, "", ResourceLoader.CACHE_MODE_REUSE)
			if cached != null and cached is CompressedTexture2D:
				cached.take_over_path("")

	# 4. Batch reimport
	_log("Step 3/5: Reimporting %d textures via EditorFileSystem..." % to_reimport.size())
	if fs != null and not to_reimport.is_empty():
		var total := to_reimport.size()
		var processed := 0
		var current_batch := PackedStringArray()

		for src in to_reimport:
			current_batch.append(src)
			if current_batch.size() >= BATCH_SIZE:
				fs.reimport_files(current_batch)
				current_batch.clear()
				processed += BATCH_SIZE
				_progress_bar.value = 40.0 + (float(processed) / float(total)) * 30.0
				await get_tree().process_frame

		if not current_batch.is_empty():
			fs.reimport_files(current_batch)
			current_batch.clear()
			_progress_bar.value = 70.0
			await get_tree().process_frame

	# 5. Verify post-reimport state
	_log("Step 4/5: Verifying DirectStorage artifacts and headers...")
	var verify_results := CORE.verify_migration(selected)
	var verified_entries: Array = []
	for v in verify_results:
		if bool(v.get("verified", false)):
			verified_entries.append(v)
		else:
			_log("[WARN] Verification failure on %s: %s" % [v.get("source", "").get_file(), v.get("detail", "")])

	_log("Verification complete: %d/%d successfully verified." % [verified_entries.size(), selected.size()])
	_progress_bar.value = 85.0
	await get_tree().process_frame

	# 6. Cleanup replaced .ctex artifacts (verified entries only; live .dstex are never touched)
	if _auto_delete_check.button_pressed and not verified_entries.is_empty():
		var to_clean := CORE.filter_verified_entries(selected, verify_results)
		_log("Step 5/5: Deleting replaced .ctex cache from .godot/imported/...")
		var del_results := CORE.cleanup_old_artifacts(to_clean)
		var total_deleted := 0
		for d in del_results:
			total_deleted += d.get("deleted", []).size()
		_log("Cleaned up %d obsolete .ctex cache files." % total_deleted)

	var failed_count := selected.size() - verified_entries.size()
	_progress_bar.value = 100.0
	if failed_count == 0:
		_log("--- Migration Completed Successfully! ---")
	else:
		_log("[WARN] Migration finished with %d verification failure(s) (%d/%d verified)." % [
			failed_count, verified_entries.size(), selected.size()
		])
	_set_busy(false)

	# Rescan to reflect updated state in the tree
	_on_scan_pressed()


# ==============================================================================
# Rollback / Restore
# ==============================================================================

func _on_restore_pressed() -> void:
	var backups := CORE.get_available_backups()
	if backups.is_empty():
		_log("No previous migration ZIP backups found in %s" % CORE.DEFAULT_BACKUP_DIR)
		_restore_file_dialog.current_dir = ProjectSettings.globalize_path(CORE.DEFAULT_BACKUP_DIR)
		_restore_file_dialog.popup_centered()
		return

	# Default to latest backup
	_pending_restore_zip = backups[0]
	_pending_action = CONFIRM_RESTORE
	_confirm_dialog.cancel_button_text = "Cancel"
	_confirm_dialog.dialog_text = "Restore original .import files and remove generated .dstex textures from latest backup?\n\nBackup file:\n%s" % _pending_restore_zip
	_confirm_dialog.popup_centered()


func _on_restore_file_selected(path: String) -> void:
	_pending_restore_zip = path
	_pending_action = CONFIRM_RESTORE
	_confirm_dialog.cancel_button_text = "Cancel"
	_confirm_dialog.dialog_text = "Restore original .import files and remove generated .dstex textures from:\n\n%s" % _pending_restore_zip
	_confirm_dialog.popup_centered()


func _on_confirm_dialog_confirmed() -> void:
	if _pending_action == CONFIRM_RESTART:
		_pending_action = ""
		_log("Asking the editor to reload the project (confirm its own prompt if it appears)...")
		# This tool is an exclusive window: an editor confirmation popped while it
		# is still visible can end up unreachable behind it (that is why the editor
		# appeared not to restart). Hide first, then ask on the next idle frame.
		hide()
		_request_editor_restart.call_deferred()
		return
	_begin_restore()


## Runs after this window is hidden. EditorInterface.restart_editor() can ask its
## own questions (unsaved resources, a playing project, an untitled scene), and all
## of those need a reachable editor dialog rather than a modal tool window.
##
## It also waits for the editor's own filesystem work to settle first: a scan or
## import still running while the editor tears down is what crashed the previous
## attempt inside ProgressDialog::add_task (the editor then closed without
## relaunching, because a crashed process never reaches the restart-on-exit
## hand-off).
func _request_editor_restart() -> void:
	if not is_inside_tree():
		return

	var fs := EditorInterface.get_resource_filesystem()
	if fs != null:
		var deadline_msec := Time.get_ticks_msec() + RESTART_SETTLE_TIMEOUT_MS
		while (fs.is_scanning() or fs.is_importing()) and Time.get_ticks_msec() < deadline_msec:
			if not is_inside_tree():
				return
			await get_tree().process_frame

	if not is_inside_tree():
		return
	EditorInterface.restart_editor(true)


## Commits the restore on disk and then asks for the editor restart that makes
## Godot reimport the restored sidecars.
##
## The tool deliberately does not call EditorFileSystem.reimport_files()/scan()
## any more. EditorFileSystem::_update_scan_actions() -> reimport_files() ->
## EditorProgress -> EditorNode::progress_add_task() -> ProgressDialog::add_task()
## runs against the editor's progress dialog, and the engine frees that dialog in
## EditorNode::NOTIFICATION_EXIT_TREE while EditorNode::singleton (and the member
## pointer) is still set, so a scan/import that is still settling when the editor
## is closed dereferences a freed ProgressDialog (measured: 0xc0000005 inside
## ProgressDialog::add_task, both before and after the earlier deferred-scan
## change). A restart is the editor's own, well-tested path for "import files
## changed on disk": the startup scan reimports them before anything else runs.
func _begin_restore() -> void:
	if _pending_restore_zip.is_empty():
		return
	if _is_busy:
		# A second confirmation while the restore is still running would start a
		# concurrent restore over the same sidecars.
		return

	var fs := EditorInterface.get_resource_filesystem()
	if fs != null and (fs.is_scanning() or fs.is_importing()):
		_log("[WARN] Cannot start restore: the editor is still scanning or importing. Please wait and retry.")
		return

	_set_busy(true)
	_log("--- Restoring from ZIP: %s ---" % _pending_restore_zip)

	var res := CORE.restore_from_zip(_pending_restore_zip)
	if not bool(res.get("ok", false)):
		_log("[ERROR] Restore failed: %s" % res.get("error", "Unknown error"))
		_set_busy(false)
		return

	var restored: Array[String] = res.get("reimport_sources", [])
	_pending_restore_zip = ""
	_restored_sources = restored
	_progress_bar.value = 100.0
	_log("Restored %d .import file(s) and removed the migrated artifacts." % restored.size())
	_set_busy(false)
	_on_scan_pressed()
	_prompt_restart(restored.size())


## Offers the restart. Declining leaves the sidecars restored on disk; the next
## editor start still reimports them, so the restore is already committed.
func _prompt_restart(restored_count: int) -> void:
	_pending_action = CONFIRM_RESTART
	_confirm_dialog.dialog_text = (
		"Restored %d .import file(s).\n\n"
		+ "The editor must restart so Godot reimports the restored textures; the startup scan does this automatically. "
		+ "Unsaved scene changes are saved first.\n\nRestart the editor now?\n\n"
		+ "(The restore is already saved to disk. If the editor does not come back, open the project again; "
		+ "you can also use Project > Reload Current Project. Choosing '" + RESTART_CANCEL_TEXT
		+ "' reimports them here instead.)"
	) % restored_count
	_confirm_dialog.cancel_button_text = RESTART_CANCEL_TEXT
	_confirm_dialog.popup_centered()


## Cancel of the restart prompt (AcceptDialog also emits this for the title-bar X).
## The sidecars are restored but the .ctex artifacts were removed by the migration,
## so the project is unusable (e.g. running the game fails on every missing texture)
## until a reimport. Cancelling therefore reimports in this session.
func _on_confirm_dialog_canceled() -> void:
	if _pending_action != CONFIRM_RESTART:
		_pending_action = CONFIRM_RESTORE
		_confirm_dialog.cancel_button_text = "Cancel"
		return
	_pending_action = ""
	_confirm_dialog.cancel_button_text = "Cancel"
	_reimport_after_skipped_restart.call_deferred()


## Imports exactly the restored sources, the way the migration tool already does.
## Deferred so the editor is not re-entered from inside the dialog's own signal.
## Running the game before this finishes loads .ctex files that do not exist yet.
func _reimport_after_skipped_restart() -> void:
	if not is_inside_tree():
		return

	var fs := EditorInterface.get_resource_filesystem()
	if fs == null or _restored_sources.is_empty():
		_log("[WARN] Restart skipped. The restored textures are not reimported; restart the editor before running the game.")
		return

	var paths := PackedStringArray(_restored_sources)
	_restored_sources = []
	_set_busy(true)
	_progress_bar.value = 10.0
	_log("Restart skipped. Reimporting %d restored texture(s) now; do not run the game until this finishes..." % paths.size())

	fs.reimport_files(paths)

	var deadline_msec := Time.get_ticks_msec() + RESTART_SETTLE_TIMEOUT_MS
	while fs.is_scanning() or fs.is_importing():
		if not is_inside_tree():
			return
		if Time.get_ticks_msec() > deadline_msec:
			_log("[WARN] The editor is still importing. Wait for its progress dialog to finish before running the game.")
			_set_busy(false)
			return
		_progress_bar.value = minf(95.0, _progress_bar.value + 1.5)
		await get_tree().process_frame

	if not is_inside_tree():
		return
	_progress_bar.value = 100.0
	_set_busy(false)
	_log("Reimport finished. The restored textures are available; running the game is safe now.")
	_on_scan_pressed()
