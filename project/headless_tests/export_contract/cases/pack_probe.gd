extends SceneTree

## Inspects an exported PCK that the engine mounted as `res://`.
##
## Usage:
##   godot --headless --main-pack <absolute .pck> --script <absolute path to this file>
##
## It reports what `project.binary` published (the export contract), which
## texture payloads are still resolvable from the PCK, and whether the derived
## 16-byte key-file magic occurs in any packed script, as bytes or as hex text.
## The magic is derived here exactly as the scanner derives it; no constant form
## of it is stored on either side.

const KEY_MAGIC_LABEL := "dspkg-toc-key-magic-v1"
const KEY_MAGIC_SIZE := 16

var _magic := PackedByteArray()
var _magic_hex := PackedByteArray()


func _init() -> void:
	_magic = _derive_key_magic()
	_magic_hex = _magic.hex_encode().to_utf8_buffer()

	print("PACK_PROBE published bundle=%s protected=%s archive_id=%d archive_name=%s" % [
		str(bool(ProjectSettings.get_setting("directstorage/export/bundle_dspkg", false))),
		str(bool(ProjectSettings.get_setting("directstorage/export/protected", false))),
		int(ProjectSettings.get_setting("directstorage/export/archive_id", 0)),
		String(ProjectSettings.get_setting("directstorage/archive_name", "textures.dspkg")),
	])
	print("PACK_PROBE payload used=%s unused=%s trigger=%s" % [
		str(FileAccess.file_exists("res://assets/used.dstex")),
		str(FileAccess.file_exists("res://assets/unused.dstex")),
		str(FileAccess.file_exists("res://assets/zzz_trigger.dstex")),
	])

	var scripts := _collect_scripts("res://")
	var gd_count := 0
	var gdc_count := 0
	var unreadable := 0
	var magic_hits := 0
	for path in scripts:
		if path.ends_with(".gd"):
			gd_count += 1
		elif path.ends_with(".gdc"):
			gdc_count += 1
		var file := FileAccess.open(path, FileAccess.READ)
		if file == null:
			unreadable += 1
			continue
		var bytes := file.get_buffer(file.get_length())
		file.close()
		if _contains_bytes(bytes, _magic) or _contains_bytes(bytes, _magic_hex):
			magic_hits += 1
			print("PACK_PROBE magic_hit=%s" % path)

	print("PACK_PROBE scripts gd=%d gdc=%d unreadable=%d magic_hits=%d" % [
		gd_count, gdc_count, unreadable, magic_hits,
	])
	print("PACK_PROBE done=True")
	quit(0)


func _derive_key_magic() -> PackedByteArray:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(KEY_MAGIC_LABEL.to_utf8_buffer())
	return context.finish().slice(0, KEY_MAGIC_SIZE)


func _collect_scripts(directory: String) -> PackedStringArray:
	var found := PackedStringArray()
	var dir := DirAccess.open(directory)
	if dir == null:
		return found
	dir.list_dir_begin()
	var name := dir.get_next()
	while not name.is_empty():
		if name != "." and name != "..":
			var path := directory.path_join(name)
			if dir.current_is_dir():
				found.append_array(_collect_scripts(path))
			elif name.ends_with(".gd") or name.ends_with(".gdc"):
				found.append(path)
		name = dir.get_next()
	dir.list_dir_end()
	return found


func _contains_bytes(haystack: PackedByteArray, needle: PackedByteArray) -> bool:
	if needle.is_empty() or haystack.size() < needle.size():
		return false
	var last := haystack.size() - needle.size()
	var start := 0
	while start <= last:
		var hit := haystack.find(needle[0], start)
		if hit < 0 or hit > last:
			return false
		if haystack.slice(hit, hit + needle.size()) == needle:
			return true
		start = hit + 1
	return false
