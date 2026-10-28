@tool
extends EditorPlugin

## Test fixture for the export-contract gate. Not shipped, never enabled by the
## addon, and inert unless both environment variables below are set.
##
## It injects the one failure the gate cannot reach from outside the exporter:
## a texture that is already staged in the archive is mutated during the same
## export, after its `add_texture()` call and before `finalize()`. The builder
## re-stats every source at finalize time, so the export must abort instead of
## streaming bytes that were never staged.
##
## DSPKG_CONTRACT_MUTATE_TARGET resource path to append one byte to. The change
## happens in `_export_end`, which runs after the whole export walk. The add-on
## plugin is registered after this one, so its `_export_end` (the `finalize()`
## call) runs after the mutation.

const MUTATE_TARGET_ENV := "DSPKG_CONTRACT_MUTATE_TARGET"

var _exporter: ContractMutationExporter


func _enter_tree() -> void:
	_exporter = ContractMutationExporter.new()
	add_export_plugin(_exporter)
	print("[contract-injector] registered")


func _exit_tree() -> void:
	if _exporter != null:
		remove_export_plugin(_exporter)
		_exporter = null


class ContractMutationExporter extends EditorExportPlugin:
	var _mutated: bool = false

	func _get_name() -> String:
		return "ExportContractInjector"

	func _export_begin(_features: PackedStringArray, _is_debug: bool, _path: String, _flags: int) -> void:
		_mutated = false
		var target := OS.get_environment(MUTATE_TARGET_ENV).strip_edges()
		if not target.is_empty():
			print("[contract-injector] armed target=%s" % target)

	func _export_end() -> void:
		if _mutated:
			return
		var target := OS.get_environment(MUTATE_TARGET_ENV).strip_edges()
		if target.is_empty():
			return
		_mutated = true
		var file := FileAccess.open(target, FileAccess.READ_WRITE)
		if file == null:
			print("[contract-injector] mutate_failed path=%s error=%d" % [target, FileAccess.get_open_error()])
			return
		file.seek_end()
		file.store_8(0)
		file.close()
		print("[contract-injector] mutated=%s" % target)
