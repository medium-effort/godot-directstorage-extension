extends SceneTree

## Opens one exported .dspkg with the archive id published in its PCK and lists
## the entry paths, so a pack-only export can prove pairing without a manifest.
##
## Usage (inside a project that loads the extension, so DSPkg exists):
##   DSPKG_CONTRACT_ARCHIVE=<absolute .dspkg>
##   DSPKG_CONTRACT_EXPECTED_ID=<decimal int64, signed>
##   godot --headless --path <project> --script <absolute path to this file>

const ARCHIVE_ENV := "DSPKG_CONTRACT_ARCHIVE"
const ID_ENV := "DSPKG_CONTRACT_EXPECTED_ID"


func _init() -> void:
	var path := OS.get_environment(ARCHIVE_ENV).strip_edges()
	var id_text := OS.get_environment(ID_ENV).strip_edges()
	var expected_id := 0
	if not id_text.is_empty():
		expected_id = int(id_text)

	if path.is_empty():
		print("ARCHIVE_PROBE error=missing_archive_path")
		quit(2)
		return
	if not ClassDB.class_exists("DSPkg"):
		print("ARCHIVE_PROBE error=dspkg_class_missing")
		quit(2)
		return

	var package: RefCounted = ClassDB.instantiate("DSPkg")
	if package == null:
		print("ARCHIVE_PROBE error=dspkg_instantiate_failed")
		quit(2)
		return

	var open_err: Error = package.open(path, expected_id)
	print("ARCHIVE_PROBE open_err=%d expected_id=%d entries=%d" % [
		open_err, expected_id, package.get_entry_count(),
	])
	for entry in package.get_entries():
		print("ARCHIVE_PROBE entry=%s" % String(entry))
	quit(0)
