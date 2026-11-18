extends RefCounted

## Verification policy over dspkg_pack_scan_core.gd for exported PCKs.
##
## Separate from ds_export_plugin.gd so the policy and machine-line formatting
## can be unit-tested in headless GDScript without an EditorExportPlugin
## context. All methods are static; no instance is required.

const SCAN_CORE_UID: String = "uid://cgmf75ap4elct"


## Scans `pack_path` with the shared pack scan core and returns one flat verdict
## dictionary: {ok, status, reason, dir_encrypted, entries, bytes, ms, detail}.
static func verify_pack(pack_path: String) -> Dictionary:
	var core_script: GDScript = ResourceLoader.load(SCAN_CORE_UID, "GDScript") as GDScript
	if core_script == null:
		return {
			"ok": false,
			"status": "fail",
			"reason": "verifier_unavailable",
			"dir_encrypted": "unknown",
			"entries": 0,
			"bytes": 0,
			"ms": 0,
			"detail": "pack scan core script not found (%s)" % SCAN_CORE_UID,
		}

	var started_ms: int = Time.get_ticks_msec()
	var core: RefCounted = core_script.new()
	var opened: Dictionary = core.call("start", pack_path)
	if bool(opened.get("ok", false)):
		core.call("scan_all")

	var verdict: Dictionary = core.call("get_verdict")
	var encryption: Dictionary = core.call("get_directory_encryption")
	var progress: Dictionary = core.call("get_progress")
	core.call("close")
	var elapsed_ms: int = maxi(0, Time.get_ticks_msec() - started_ms)

	var ok: bool = bool(verdict.get("ok", false))
	var dir_encrypted := "unknown"
	if bool(encryption.get("known", false)):
		dir_encrypted = "true" if bool(encryption.get("encrypted", false)) else "false"

	return {
		"ok": ok,
		"status": "ok" if ok else "fail",
		"reason": "none" if ok else String(verdict.get("reason", "unknown")),
		"dir_encrypted": dir_encrypted,
		"entries": int(verdict.get("entries", 0)),
		"bytes": int(progress.get("size", 0)),
		"ms": elapsed_ms,
		"detail": String(verdict.get("detail", "")),
	}


## Result dictionary for exports whose pack cannot be read as a standalone PCK
## (e.g. embedded inside an executable). `ok` is true so the export is not
## failed, while `status` stays `unverified` and is never counted as verified.
static func unverified(reason: String) -> Dictionary:
	return {
		"ok": true,
		"status": "unverified",
		"reason": reason,
		"dir_encrypted": "unknown",
		"entries": 0,
		"bytes": 0,
		"ms": 0,
		"detail": "",
	}


## Result dictionary for exports where pack verification does not apply (zip
## exports, or presets that explicitly disabled verification).
static func skipped(reason: String) -> Dictionary:
	return {
		"ok": true,
		"status": "skipped",
		"reason": reason,
		"dir_encrypted": "unknown",
		"entries": 0,
		"bytes": 0,
		"ms": 0,
		"detail": "",
	}


## Formats one machine-readable PACK_VERIFY line with the path last. Never
## prints the needle or any key byte.
static func format_line(result: Dictionary, pack_path: String) -> String:
	return "PACK_VERIFY status=%s reason=%s dir_encrypted=%s entries=%d bytes=%d ms=%d file=%s" % [
		String(result.get("status", "fail")),
		String(result.get("reason", "unknown")),
		String(result.get("dir_encrypted", "unknown")),
		int(result.get("entries", 0)),
		int(result.get("bytes", 0)),
		int(result.get("ms", 0)),
		pack_path,
	]
