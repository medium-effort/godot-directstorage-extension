extends SceneTree

## Compares the key-file magic derived by three independent places:
##   * the scanner's label constant (project/addons/directstorage_accelerator/tools/dspkg_pack_scan_core.gd),
##   * the exporter's label constant (ds_export_plugin.gd),
##   * the C++ reader's label constant (src/formats/dspkg_format.cpp).
##
## The C++ side is executed by the native reader, not here: the parity proven at
## run time is that a protected archive written with the exporter's magic opens
## in the native reader, and that the scanner finds the exporter's magic in the
## unpinned control pack. This script adds the static leg: the three labels and
## the 16-byte SHA-256 slice are identical, so one derivation cannot drift.
##
## Usage:
##   DSPKG_CONTRACT_WORKSPACE=<absolute workspace root>
##   godot --headless --path <project> --script <absolute path to this file>

const WORKSPACE_ENV := "DSPKG_CONTRACT_WORKSPACE"


func _init() -> void:
	var workspace := OS.get_environment(WORKSPACE_ENV).strip_edges()
	if workspace.is_empty():
		print("MAGIC_PARITY error=missing_workspace")
		quit(2)
		return

	var cpp_text := _read_text(workspace.path_join("src/formats/dspkg_format.cpp"))
	var exporter_text := _read_text(workspace.path_join("project/addons/directstorage_accelerator/scripts/ds_export_plugin.gd"))
	var scanner_text := _read_text(workspace.path_join("project/addons/directstorage_accelerator/tools/dspkg_pack_scan_core.gd"))
	if cpp_text.is_empty() or exporter_text.is_empty() or scanner_text.is_empty():
		print("MAGIC_PARITY error=source_unreadable cpp=%s exporter=%s scanner=%s" % [
			str(not cpp_text.is_empty()), str(not exporter_text.is_empty()), str(not scanner_text.is_empty()),
		])
		quit(2)
		return

	var cpp_label := _capture(cpp_text, "DSPKG_TOC_KEY_MAGIC_LABEL\\s*=\\s*\"([^\"]+)\"")
	var cpp_size := _capture(cpp_text, "DSPKG_TOC_KEY_MAGIC_SIZE\\s*=\\s*(\\d+)")
	var exporter_label := _capture(exporter_text, "KEY_MAGIC_LABEL\\s*:\\s*String\\s*=\\s*\"([^\"]+)\"")
	var scanner_label := _capture(scanner_text, "KEY_MAGIC_LABEL\\s*:\\s*=\\s*\"([^\"]+)\"")
	var scanner_size := _capture(scanner_text, "KEY_MAGIC_SIZE\\s*:\\s*=\\s*(\\d+)")

	var cpp_magic := _derive(cpp_label)
	var exporter_magic := _derive(exporter_label)
	var scanner_magic := _derive(scanner_label)

	var cpp_uses_sha256 := cpp_text.contains("sha256_buffer()")
	var cpp_slice := cpp_text.contains("memcpy(derived.data(), digest.ptr(), DSPKG_TOC_KEY_MAGIC_SIZE)")
	var labels_match := not cpp_label.is_empty() and cpp_label == exporter_label and cpp_label == scanner_label
	var sizes_match := cpp_size == "16" and scanner_size == "16"
	var digests_match := cpp_magic == exporter_magic and exporter_magic == scanner_magic and exporter_magic.size() == 16
	var matched := labels_match and sizes_match and digests_match and cpp_uses_sha256 and cpp_slice

	print("MAGIC_PARITY label=%s cpp_size=%s scanner_size=%s cpp_sha256=%s cpp_slice16=%s" % [
		cpp_label, cpp_size, scanner_size, str(cpp_uses_sha256), str(cpp_slice),
	])
	print("MAGIC_PARITY gd=%s exporter=%s scanner=%s cpp=%s" % [
		exporter_magic.hex_encode(), exporter_magic.hex_encode(), scanner_magic.hex_encode(), cpp_magic.hex_encode(),
	])
	print("MAGIC_PARITY labels_match=%s sizes_match=%s digests_match=%s match=%s" % [
		str(labels_match), str(sizes_match), str(digests_match), str(matched),
	])
	quit(0 if matched else 1)


func _derive(label: String) -> PackedByteArray:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(label.to_utf8_buffer())
	return context.finish().slice(0, 16)


func _capture(text: String, pattern: String) -> String:
	var regex := RegEx.new()
	if regex.compile(pattern) != OK:
		return ""
	var found := regex.search(text)
	if found == null:
		return ""
	return found.get_string(1)


func _read_text(path: String) -> String:
	if not FileAccess.file_exists(path):
		return ""
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return ""
	var text := file.get_as_text()
	file.close()
	return text
