@tool
extends EditorPlugin

## Test fixture for the export-contract gate. NOT SHIPPED and never referenced
## by the addon.
##
## Why it exists: the product exporter refuses a preset whose encryption filters
## leave the key entry unencrypted, so the product path can never produce the
## leak the scanner is supposed to catch. This fixture reproduces that leak the
## way a broken exporter would: it injects `res://.dspkg/toc.key` as
## `magic16 || random32` while the preset's encryption include filter misses the
## key path, so the entry is written in the clear.
##
## The gate runs the scanner over the resulting pack and requires
## `status=fail reason=key_magic_found`. A scanner that reported `ok` here would
## be blind to a plaintext key.
##
## The magic is derived at run time from the same label the exporter and the
## scanner use; no hex or byte form of it is stored in this file.

const KEY_PATH := "res://.dspkg/toc.key"
const KEY_MAGIC_LABEL := "dspkg-toc-key-magic-v1"
const KEY_MAGIC_SIZE := 16
const KEY_SIZE := 32

var _exporter: KeyScanControlExporter


func _enter_tree() -> void:
	_exporter = KeyScanControlExporter.new()
	add_export_plugin(_exporter)
	print("[key-scan-control] registered")


func _exit_tree() -> void:
	if _exporter != null:
		remove_export_plugin(_exporter)
		_exporter = null


class KeyScanControlExporter extends EditorExportPlugin:
	func _get_name() -> String:
		return "KeyScanControl"

	func _export_begin(_features: PackedStringArray, _is_debug: bool, path: String, _flags: int) -> void:
		var preset := get_export_preset()
		if preset != null:
			print("[key-scan-control] encrypt_pck=%s include=[%s] target=%s" % [
				str(preset.get_encrypt_pck()),
				preset.get_encryption_in_filter(),
				path,
			])
		var payload := _derive_key_magic()
		payload.append_array(Crypto.new().generate_random_bytes(KEY_SIZE))
		add_file(KEY_PATH, payload, false)
		print("[key-scan-control] injected=%s len=%d" % [KEY_PATH, payload.size()])

	func _derive_key_magic() -> PackedByteArray:
		var context := HashingContext.new()
		context.start(HashingContext.HASH_SHA256)
		context.update(KEY_MAGIC_LABEL.to_utf8_buffer())
		return context.finish().slice(0, KEY_MAGIC_SIZE)
