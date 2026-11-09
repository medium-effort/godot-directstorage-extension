@tool
extends Node

## Installs and activates Microsoft DirectStorage runtime packages for the
## editor addon.
##
## The DirectStorage SDK is distributed as the official NuGet package
## Microsoft.Direct3D.DirectStorage. Downloads come from the NuGet flat
## container and are verified against the packageHash Microsoft publishes
## (SHA-512, base64) before anything is extracted.
##
## Layout (editor only, all inside the addon):
##   bin/dstorage.dll, bin/dstoragecore.dll   active runtime, flat so the
##                                            .gdextension [dependencies] and
##                                            exports keep working unchanged
##   bin/.versions/<version>/                 cached package payloads
##   bin/.pending.json                        version staged for next start
##   bin/.runtime.json                        version currently active
##
## Windows locks loaded modules, so installs never overwrite the flat DLLs
## directly. They stage a version and write .pending.json; apply_pending()
## runs before the first DSContext initialization on the next start and copies
## the staged files while nothing holds them.

signal install_finished(version: String, error: int, message: String)
## Emitted after a package is downloaded and verified but before its payload is
## installed. The integration must show the exact license from that package and
## call accept_license() or decline_license().
signal license_required(version: String, license_text: String)

const ADDON_BIN: String = "res://addons/directstorage_accelerator/bin"
const VERSIONS_PATH: String = ADDON_BIN + "/.versions"
const PENDING_PATH: String = ADDON_BIN + "/.pending.json"
const ACTIVE_PATH: String = ADDON_BIN + "/.runtime.json"
const DOWNLOAD_DIR: String = "user://directstorage_runtime"

## Package payload extracted into bin/.versions/<version>/. get_file() is used
## for the destination name, so every basename here must be unique.
## Only redistributable runtime files plus the package texts are extracted: the
## import library is not in distributable_files.txt and the headers are unused
## at runtime, so neither belongs in a project that may later be packaged.
const PAYLOAD_FILES := [
	"native/bin/x64/dstorage.dll",
	"native/bin/x64/dstoragecore.dll",
	"LICENSE.txt",
	"NOTICES.txt",
]

## The only files copied over the active flat layout.
const RUNTIME_FILES := [
	"dstorage.dll",
	"dstoragecore.dll",
]

## Package table. runtime_versions.json is the maintained list: one entry per
## published Microsoft.Direct3D.DirectStorage package, each carrying the NuGet
## URL, the package size and Microsoft's published packageHash (base64 SHA-512).
## Keeping it in a data file means a new runtime version does not need a
## GDScript edit.
##
## FALLBACK_VERSIONS keeps the baseline installable when that file is missing,
## unreadable or partially synced: a stripped copy of the addon must still be
## able to fetch 1.3.0. These are the same values as the manifest's baseline
## entry, and tools/check_runtime_versions.ps1 fails the build when the two
## drift apart.
const VERSIONS_MANIFEST_PATH: String = "res://addons/directstorage_accelerator/runtime_versions.json"

const FALLBACK_VERSIONS: Dictionary = {
	"1.3.0": {
		"url": "https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.directstorage/1.3.0/microsoft.direct3d.directstorage.1.3.0.nupkg",
		"sha512": "WJqDGU6eBWVFI7nKEEAZgtI126qZHIU1gH1bn4Udh3KBQX15oCEPd7RsD2URT4BfShLwS3vbPeigD+IMeN73kQ==",
		"size": 2816824,
		"preview": false,
	},
}

static var _versions: Dictionary = {}


## Merged package table: the built-in fallback plus every valid manifest entry.
## A version already pinned in FALLBACK_VERSIONS keeps the code copy, so a stale
## or hand-edited manifest can never downgrade the baseline hash.
static func get_versions() -> Dictionary:
	if not _versions.is_empty():
		return _versions
	var merged: Dictionary = FALLBACK_VERSIONS.duplicate(true)
	var manifest := _read_manifest_versions()
	for version in manifest:
		if not merged.has(version):
			merged[version] = manifest[version]
	_versions = merged
	return _versions


static func _read_manifest_versions() -> Dictionary:
	var versions: Dictionary = {}
	if not FileAccess.file_exists(VERSIONS_MANIFEST_PATH):
		push_warning("[DirectStorage] Runtime version manifest missing (%s); using the built-in package table." % VERSIONS_MANIFEST_PATH)
		return versions
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(VERSIONS_MANIFEST_PATH))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("[DirectStorage] Runtime version manifest is not a JSON object; using the built-in package table.")
		return versions
	for version in (parsed as Dictionary):
		var meta: Variant = (parsed as Dictionary)[version]
		if typeof(meta) != TYPE_DICTIONARY or not _is_valid_entry(meta as Dictionary):
			push_warning("[DirectStorage] Ignoring incomplete manifest entry for %s (an https url, sha512 and a positive size are required)." % String(version))
			continue
		versions[String(version)] = meta
	return versions


## An entry is usable only when every field the install path trusts is present.
## None of these may be optional: the URL is fetched and the SHA-512 is the only
## integrity check, so an incomplete entry is dropped rather than half-used.
static func _is_valid_entry(meta: Dictionary) -> bool:
	var url := String(meta.get("url", ""))
	var sha512 := String(meta.get("sha512", ""))
	var size := int(meta.get("size", 0))
	return url.begins_with("https://") and not sha512.is_empty() and size > 0


var _http: HTTPRequest = null
var _license_version: String = ""
var _license_nupkg: String = ""
var _license_meta: Dictionary = {}


## Starts an asynchronous download and install. Always ends with
## install_finished, including for validation and extraction failures.
func install(version: String) -> void:
	if not OS.has_feature("editor"):
		_finish(version, ERR_UNAUTHORIZED, "DirectStorage runtime installation is editor-only.")
		return
	var versions := get_versions()
	if not versions.has(version):
		_finish(version, ERR_INVALID_PARAMETER, "Unknown DirectStorage runtime version: %s" % version)
		return
	if _http != null:
		_finish(version, ERR_BUSY, "An install is already in progress.")
		return
	if not _license_version.is_empty():
		_finish(version, ERR_BUSY, "A license decision is pending.")
		return

	var meta: Dictionary = versions[version]
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(DOWNLOAD_DIR))
	var nupkg := DOWNLOAD_DIR.path_join(version + ".nupkg")
	if FileAccess.file_exists(nupkg):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(nupkg))

	_http = HTTPRequest.new()
	_http.download_file = nupkg
	_http.max_redirects = 8
	_http.use_threads = true
	_http.request_completed.connect(_on_download_completed.bind(version, nupkg, meta), CONNECT_ONE_SHOT)
	add_child(_http)

	var err := _http.request(String(meta["url"]))
	if err != OK:
		_http.queue_free()
		_http = null
		_finish(version, err, "Could not start the download: %s" % error_string(err))


## Applies a staged runtime before the first DirectStorage call. Safe to call
## on every start: returns OK immediately when nothing is pending. On a locked
## DLL it leaves the stage in place and returns ERR_FILE_CANT_WRITE so the next
## start retries instead of reporting a false success.
static func apply_pending() -> int:
	if not OS.has_feature("editor"):
		return OK
	if not FileAccess.file_exists(PENDING_PATH):
		return OK

	var pending: Variant = JSON.parse_string(FileAccess.get_file_as_string(PENDING_PATH))
	if typeof(pending) != TYPE_DICTIONARY:
		push_warning("[DirectStorage] Runtime stage is not valid JSON; ignoring it. Delete %s to reset." % PENDING_PATH)
		return ERR_FILE_CORRUPT

	var version := String((pending as Dictionary).get("version", ""))
	if version.is_empty():
		push_warning("[DirectStorage] Runtime stage has no version; ignoring it.")
		return ERR_FILE_CORRUPT

	var source_dir := VERSIONS_PATH.path_join(version)
	# Snapshot the active pair first. If the second copy fails, the first one has
	# already been replaced; restoring both from the backup keeps the shim and
	# core from the same package instead of leaving a mixed pair that the next
	# initialize_ds() would load.
	var backup_dir := VERSIONS_PATH.path_join(".backup")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(backup_dir))
	for file_name in RUNTIME_FILES:
		var destination := ADDON_BIN.path_join(file_name)
		var backup := backup_dir.path_join(file_name)
		if FileAccess.file_exists(destination) and not _copy_verified(destination, backup):
			push_warning("[DirectStorage] Could not snapshot %s before the runtime swap; no files were changed." % file_name)
			return ERR_FILE_CANT_WRITE

	for file_name in RUNTIME_FILES:
		var source := source_dir.path_join(file_name)
		var destination := ADDON_BIN.path_join(file_name)
		if not FileAccess.file_exists(source):
			push_warning("[DirectStorage] Runtime stage for %s is missing %s; reinstalling is required." % [version, file_name])
			_restore_pair(backup_dir)
			return ERR_FILE_NOT_FOUND
		if not _copy_verified(source, destination):
			_restore_pair(backup_dir)
			push_warning("[DirectStorage] Runtime update to %s is staged but %s is in use; the previous pair was restored and the update will be retried on the next editor start." % [version, file_name])
			return ERR_FILE_CANT_WRITE

	if not _write_json(ACTIVE_PATH, {"active": version}):
		_restore_pair(backup_dir)
		return ERR_FILE_CANT_WRITE
	DirAccess.remove_absolute(ProjectSettings.globalize_path(PENDING_PATH))
	return OK


static func _restore_pair(backup_dir: String) -> void:
	for file_name in RUNTIME_FILES:
		var backup := backup_dir.path_join(file_name)
		var destination := ADDON_BIN.path_join(file_name)
		if FileAccess.file_exists(backup) and not _copy_verified(backup, destination):
			push_error("[DirectStorage] Could not restore %s after a failed runtime swap; reinstall the runtime before the next start." % file_name)


## Stages an already-downloaded version for the next start (rollback path).
static func activate_cached(version: String) -> int:
	if not OS.has_feature("editor"):
		return ERR_UNAUTHORIZED
	if not FileAccess.file_exists(VERSIONS_PATH.path_join(version).path_join("dstorage.dll")):
		return ERR_FILE_NOT_FOUND
	return OK if _write_json(PENDING_PATH, {"version": version}) else ERR_FILE_CANT_WRITE


static func get_active_version() -> String:
	# Prefer what the DLLs actually are: .runtime.json only records installs
	# made through this service, so a pre-existing flat runtime would otherwise
	# read as "not installed".
	var installed := get_installed_version()
	if not installed.is_empty():
		return installed
	return String(_read_json(ACTIVE_PATH).get("active", ""))


## Reads the installed dstoragecore.dll version through DSContext and maps it
## to a known package key. Returns "" when no runtime is present.
static func get_installed_version() -> String:
	if not ClassDB.class_exists("DSContext"):
		return ""
	var context = Engine.get_singleton("DSContext")
	if context == null or not context.has_method("get_runtime_info"):
		return ""
	var info: Dictionary = context.get_runtime_info()
	var raw := String(info.get("version", ""))
	if raw.is_empty() or raw == "Unavailable":
		return ""
	return _match_known_version(raw)


## Maps a DLL file version to a known package key. Microsoft's packages carry
## the release in major.minor (1.3.x.x is the 1.3.0 package, 1.4.x.x is the
## 1.4 preview); build/revision are unrelated dates. Multiple packages sharing
## a major.minor would need a build-prefix table here.
static func _match_known_version(raw: String) -> String:
	var parts := raw.split(".")
	if parts.size() >= 2:
		var major_minor := "%s.%s" % [parts[0], parts[1]]
		for key in get_versions():
			var known := String(key).split(".")
			if known.size() >= 2 and "%s.%s" % [known[0], known[1]] == major_minor:
				return String(key)
	return raw


static func get_pending_version() -> String:
	return String(_read_json(PENDING_PATH).get("version", ""))


static func get_cached_versions() -> PackedStringArray:
	var versions := PackedStringArray()
	var dir := DirAccess.open(VERSIONS_PATH)
	if dir == null:
		return versions
	dir.list_dir_begin()
	var entry := dir.get_next()
	while not entry.is_empty():
		if dir.current_is_dir() and not entry.begins_with("."):
			versions.append(entry)
		entry = dir.get_next()
	dir.list_dir_end()
	versions.sort()
	return versions


static func get_available_versions() -> Dictionary:
	return get_versions().duplicate(true)


static func get_state() -> Dictionary:
	return {
		"active": get_active_version(),
		"pending": get_pending_version(),
		"cached": get_cached_versions(),
	}


func _on_download_completed(result: int, response_code: int, _headers: PackedStringArray, _body: PackedByteArray, version: String, nupkg: String, meta: Dictionary) -> void:
	if _http != null:
		_http.queue_free()
		_http = null

	if result != HTTPRequest.RESULT_SUCCESS:
		_remove_download(nupkg)
		_finish(version, ERR_CANT_CONNECT, "Download failed (HTTPRequest result %d)." % result)
		return
	if response_code != 200:
		_remove_download(nupkg)
		_finish(version, ERR_CANT_CONNECT, "Download failed (HTTP %d)." % response_code)
		return

	var file := FileAccess.open(nupkg, FileAccess.READ)
	if file == null:
		_finish(version, FileAccess.get_open_error(), "Downloaded package could not be opened.")
		return
	var size := file.get_length()
	file.close()
	if size != int(meta["size"]):
		_remove_download(nupkg)
		_finish(version, ERR_FILE_CORRUPT, "Package size mismatch: got %d bytes, expected %d." % [size, int(meta["size"])])
		return

	if not ClassDB.class_exists("DSCrypto"):
		_remove_download(nupkg)
		_finish(version, ERR_UNAVAILABLE, "This extension build has no DSCrypto class; rebuild the addon to verify packages.")
		return
	var digest: PackedByteArray = DSCrypto.sha512_file(nupkg)
	if digest.is_empty():
		_remove_download(nupkg)
		_finish(version, ERR_CANT_OPEN, "Could not compute the package SHA-512.")
		return
	var actual := Marshalls.raw_to_base64(digest)
	if actual != String(meta["sha512"]):
		_remove_download(nupkg)
		_finish(version, ERR_FILE_CORRUPT, "Package SHA-512 does not match Microsoft's published hash; download discarded.")
		return

	# The license is shown before anything is installed, and it is the exact
	# LICENSE.txt from the verified package, not a copy bundled in the addon.
	var zip := ZIPReader.new()
	if zip.open(nupkg) != OK:
		_remove_download(nupkg)
		_finish(version, ERR_FILE_CORRUPT, "Could not reopen the verified package to read its license.")
		return
	if not zip.file_exists("LICENSE.txt"):
		zip.close()
		_remove_download(nupkg)
		_finish(version, ERR_FILE_CORRUPT, "Package has no LICENSE.txt; refusing to install it.")
		return
	var license_text := zip.read_file("LICENSE.txt").get_string_from_utf8()
	zip.close()
	if license_text.strip_edges().is_empty():
		_remove_download(nupkg)
		_finish(version, ERR_FILE_CORRUPT, "Package license is empty; refusing to install it.")
		return

	_license_version = version
	_license_nupkg = nupkg
	_license_meta = meta
	license_required.emit(version, license_text)


## Continues an install after the user accepted the shown license.
func accept_license() -> void:
	if _license_version.is_empty():
		_finish("", ERR_UNAUTHORIZED, "No license decision is pending.")
		return
	var version := _license_version
	var nupkg := _license_nupkg
	_clear_license()
	var extract_error := _extract_package(nupkg, version)
	if extract_error != OK:
		_remove_download(nupkg)
		_finish(version, extract_error, "Package extraction failed: %s" % error_string(extract_error))
		return
	_remove_download(nupkg)
	if not _write_json(PENDING_PATH, {"version": version}):
		_finish(version, ERR_FILE_CANT_WRITE, "Package is verified but the stage marker could not be written.")
		return
	_finish(version, OK, "DirectStorage %s is staged. Restart the editor to apply it." % version)


## Discards a downloaded package after the user declined its license.
func decline_license() -> void:
	if _license_version.is_empty():
		return
	var version := _license_version
	var nupkg := _license_nupkg
	_clear_license()
	_remove_download(nupkg)
	_finish(version, ERR_UNAUTHORIZED, "DirectStorage license declined; nothing was installed.")


func _clear_license() -> void:
	_license_version = ""
	_license_nupkg = ""
	_license_meta = {}


static func _extract_package(nupkg: String, version: String) -> int:
	var zip := ZIPReader.new()
	if zip.open(nupkg) != OK:
		return ERR_FILE_CORRUPT

	var target_dir := VERSIONS_PATH.path_join(version)
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(target_dir))

	for entry in PAYLOAD_FILES:
		if not zip.file_exists(entry):
			zip.close()
			return ERR_FILE_CORRUPT
		var data: PackedByteArray = zip.read_file(entry)
		var destination := target_dir.path_join(entry.get_file())
		var out := FileAccess.open(destination, FileAccess.WRITE)
		if out == null:
			zip.close()
			return FileAccess.get_open_error()
		out.store_buffer(data)
		var write_error := out.get_error()
		out.close()
		if write_error != OK:
			zip.close()
			return write_error

	zip.close()
	return OK


static func _copy_verified(source: String, destination: String) -> bool:
	var data: PackedByteArray = FileAccess.get_file_as_bytes(source)
	if data.is_empty():
		return false
	# Windows can keep a loaded module mapped briefly after shutdown and
	# Defender can hold a freshly written file, so retry instead of failing on
	# the first sharing violation.
	for attempt in 3:
		var out := FileAccess.open(destination, FileAccess.WRITE)
		if out != null:
			out.store_buffer(data)
			var write_error := out.get_error()
			out.close()
			if write_error == OK and FileAccess.get_file_as_bytes(destination).size() == data.size():
				return true
		OS.delay_msec(200)
	return false


static func _remove_download(nupkg: String) -> void:
	if FileAccess.file_exists(nupkg):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(nupkg))


static func _read_json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


static func _write_json(path: String, value: Dictionary) -> bool:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_warning("[DirectStorage] Could not write %s: %s" % [path, error_string(FileAccess.get_open_error())])
		return false
	file.store_string(JSON.stringify(value))
	var write_error := file.get_error()
	file.close()
	if write_error != OK:
		push_warning("[DirectStorage] Could not write %s: %s" % [path, error_string(write_error)])
		return false
	return true


func _finish(version: String, error: int, message: String) -> void:
	if error != OK:
		push_warning("[DirectStorage] " + message)
	install_finished.emit(version, error, message)
