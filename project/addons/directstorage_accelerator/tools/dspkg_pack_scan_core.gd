extends RefCounted

## Pack-validity and key-leak scan for one exported PCK.
##
## This file is the shared engine behind the command-line scanner
## (dspkg_pack_scan.gd) and the editor dialog (dspkg_pack_scan_dialog.gd). It
## owns the verdict vocabulary - the status words, the reason tokens, the exit
## codes and the `DSPKG_SCAN ...` line format - so the terminal and the editor
## button cannot disagree about what a result is called.
##
## The scanner answers two independent questions about the same file:
##  1. Does the pack header describe a non-empty directory that lies inside the
##     file? The header is validated before the verdict can be `ok` because a
##     failed export leaves a truncated pack that still starts with the pack
##     magic: a key scan over such a file finds nothing and would otherwise pass
##     vacuously.
##  2. Do the raw bytes contain the 16-byte key-file magic anywhere? The magic
##     is derived at run time as SHA-256("dspkg-toc-key-magic-v1")[0..16] and is
##     never stored here as bytes or hex. Callers read in slices with a 31-byte
##     carry, so a needle that straddles a slice boundary is still found and the
##     whole file is never held in memory.
##
## A key hit outranks a malformed header. Both are failures, but the leak is the
## stronger finding and must be reported even when the pack shape is broken. The
## verdict can only be `ok` when the header is valid and no needle occurs.
##
## Embedded packs (`embed_pck`) keep the magic at the end of an executable, so
## this scanner expects a standalone `.pck`; it does not scan executables.
##
## Positive control: the probe exporter under `.agents/scratch/key_scan_control/`
## produces the leak fixture by packing `magic16 || random32` in the clear. The
## product exporter refuses the preset that would be needed before `add_file()`,
## so it cannot build that fixture itself. This scanner never writes one.

## "GDPC" as stored little-endian by the engine (core/io/file_access_pack.h).
const PACK_HEADER_MAGIC := 0x43504447
const PACK_FORMAT_VERSION_V2 := 2
const PACK_FORMAT_VERSION_V4 := 4
## v2 keeps the directory right after 16 reserved 32-bit words: 32 + 64 bytes.
const PACK_V2_HEADER_SIZE := 96
## v3/v4 add the 64-bit directory offset: 40 + 64 bytes.
const PACK_V3_V4_HEADER_SIZE := 104
const PACK_DIR_ENCRYPTED := 1 << 0
## Shortest possible directory entry: length, offset, size, md5, flags.
const PACK_MIN_ENTRY_SIZE := 4 + 8 + 8 + 16 + 4

const KEY_MAGIC_LABEL := "dspkg-toc-key-magic-v1"
const KEY_MAGIC_SIZE := 16

## The carry is one more than the longest needle-1 so a boundary hit survives.
const SCAN_CHUNK_SIZE := 4 * 1024 * 1024
const SCAN_CARRY_SIZE := 31
## The needle prefix that anchors the search inside a slice.
const ANCHOR_SIZE := 8
## Per-window candidate cap before falling back to the 8-alignment word walk.
const SCAN_CANDIDATE_BUDGET := 65536

const EXIT_OK := 0
const EXIT_FAIL := 1
const EXIT_USAGE := 2

const STATUS_OK := "ok"
const STATUS_FAIL := "fail"

const REASON_BAD_HEADER := "bad_header"
const REASON_ZERO_ENTRIES := "zero_entries"
const REASON_DIRECTORY_PAST_EOF := "directory_past_eof"
const REASON_KEY_MAGIC_FOUND := "key_magic_found"
const REASON_UNREADABLE := "unreadable"

const USAGE := "usage: godot --headless --path project --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack <path>"

var _file: FileAccess = null
var _opened: bool = false
var _size: int = 0
var _needle: PackedByteArray = PackedByteArray()
var _anchor: int = 0
var _carry: PackedByteArray = PackedByteArray()
var _position: int = 0
var _header: Dictionary = {}
var _dir_encrypted: bool = false
var _dir_encrypted_known: bool = false
var _hit: bool = false
var _short_read: bool = false
var _finished: bool = false
var _open_detail: String = "the file could not be opened for reading"
var _pack_path: String = ""
var _native_scanner: Object = ClassDB.instantiate(&"DSPackScanner") if ClassDB.class_exists(&"DSPackScanner") else null


## Opens the pack and parses its header. The returned dictionary reports only
## whether the file could be opened; a malformed header is not an open failure
## and is reported by get_verdict() once the byte scan has run.
func start(pack_path: String) -> Dictionary:
	_close_file()
	_opened = false
	_hit = false
	_short_read = false
	_finished = false
	_position = 0
	_carry = PackedByteArray()
	_header = {}
	_dir_encrypted = false
	_dir_encrypted_known = false
	_pack_path = pack_path

	_file = FileAccess.open(pack_path, FileAccess.READ)
	if _file == null:
		_open_detail = "the file could not be opened for reading (error %d)" % FileAccess.get_open_error()
		_finished = true
		return {"ok": false, "reason": REASON_UNREADABLE, "detail": _open_detail}

	_opened = true
	_size = int(_file.get_length())
	_needle = _derive_key_magic()
	_anchor = _needle.slice(0, ANCHOR_SIZE).decode_s64(0)

	# Parse the header first; its verdict decides whether an absent needle may
	# be reported as `ok`. The scan still runs even when the header is broken.
	_header = _read_pack_header()
	return {"ok": true, "reason": "", "detail": ""}


## Scans the entire file in one call. Uses the native whole-file scanner when
## available so large packs avoid per-chunk GDScript buffer copies; otherwise
## steps through the file in SCAN_CHUNK_SIZE slices.
func scan_all() -> Dictionary:
	if _finished or _file == null:
		_finished = true
		return {"done": true, "scanned": _position, "size": _size}
	if _native_scanner != null and _position == 0:
		var res: Dictionary = _native_scanner.call(&"file_contains_bytes", _pack_path, _needle)
		if not bool(res.get("ok", false)):
			_short_read = true
			_finished = true
			return {"done": true, "scanned": _position, "size": _size}
		_position = _size
		if bool(res.get("found", false)):
			_hit = true
		_finished = true
		return {"done": true, "scanned": _size, "size": _size}
	while true:
		var s := step(SCAN_CHUNK_SIZE)
		if s.get("done", true):
			return s
	return {"done": true, "scanned": _position, "size": _size}


## Reads at most `max_bytes` more of the file and reports progress. Returns
## {"done": bool, "scanned": int, "size": int}. Call repeatedly until done.
func step(max_bytes: int) -> Dictionary:
	if _finished or _file == null:
		_finished = true
		return {"done": true, "scanned": _position, "size": _size}

	if _native_scanner != null and _position == 0 and max_bytes >= _size:
		return scan_all()

	var want: int = mini(maxi(1, max_bytes), _size - _position)
	if want > 0:
		_file.seek(_position)
		var chunk := _file.get_buffer(want)
		if chunk.size() != want:
			_short_read = true
			_finished = true
			return {"done": true, "scanned": _position, "size": _size}

		var window := PackedByteArray()
		window.append_array(_carry)
		window.append_array(chunk)
		if _window_contains_needle(window):
			_hit = true
			_finished = true
			return {"done": true, "scanned": _position, "size": _size}

		var keep: int = mini(SCAN_CARRY_SIZE, window.size())
		_carry = window.slice(window.size() - keep)
		_position += want

	if _position >= _size:
		_finished = true
	return {"done": _finished, "scanned": _position, "size": _size}


## Bytes examined so far, and the total, for a progress bar.
func get_progress() -> Dictionary:
	return {"scanned": _position, "size": _size}


## Whether the pack's file directory is encrypted, i.e. whether the export that
## produced it was protected. This is presentation-only: the machine line is
## deliberately unchanged, so the command-line contract and the gate's regular
## expressions stay valid. `known` is false when the header could not be read far
## enough to contain the flags word.
func get_directory_encryption() -> Dictionary:
	return {"known": _dir_encrypted_known, "encrypted": _dir_encrypted}


## The current verdict. `ok` is only possible after a completed scan whose
## header parsed and whose bytes carry no key magic. A short read and a key hit
## are both failures regardless of the header.
func get_verdict() -> Dictionary:
	if not _opened:
		return _fail(REASON_UNREADABLE, _open_detail)
	if _short_read:
		return _fail(REASON_UNREADABLE, "the file ended before the requested number of bytes could be read")
	if _hit:
		return _fail(REASON_KEY_MAGIC_FOUND, "the derived key-file magic occurs in the raw pack bytes")
	if not _header.get("ok", false):
		return _fail(String(_header.get("reason", REASON_BAD_HEADER)), String(_header.get("detail", "")))
	return {
		"ok": true,
		"status": STATUS_OK,
		"reason": "",
		"detail": "",
		"entries": int(_header.get("entries", 0)),
	}


## The one machine-readable line, exactly as the command line prints it and as
## the gate's regular expressions expect it.
func format_verdict(pack_path: String) -> String:
	var verdict := get_verdict()
	if verdict["ok"]:
		return "DSPKG_SCAN status=%s entries=%d file=%s" % [STATUS_OK, int(verdict["entries"]), pack_path]
	return "DSPKG_SCAN status=%s reason=%s file=%s" % [STATUS_FAIL, String(verdict["reason"]), pack_path]


## 0 only for `ok`, as the command line's process exit code.
func exit_code() -> int:
	return EXIT_OK if get_verdict()["ok"] else EXIT_FAIL


## Releases the file handle early. Safe to call more than once.
func close() -> void:
	_close_file()


func _close_file() -> void:
	if _file != null:
		_file.close()
		_file = null


func _fail(reason: String, detail: String) -> Dictionary:
	return {"ok": false, "status": STATUS_FAIL, "reason": reason, "detail": detail, "entries": 0}


func _derive_key_magic() -> PackedByteArray:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(KEY_MAGIC_LABEL.to_utf8_buffer())
	return context.finish().slice(0, KEY_MAGIC_SIZE)


## Accepts the formats the engine reads: v2 keeps the directory immediately after
## the header, v3/v4 store its offset in the header. The entry count is plaintext
## in both the encrypted and the unencrypted directory, because the exporter
## writes it before the directory body is encrypted.
func _read_pack_header() -> Dictionary:
	if _size < PACK_V2_HEADER_SIZE:
		return _header_failure(REASON_BAD_HEADER, "the file is %d bytes, shorter than the smallest pack header" % _size)

	_file.seek(0)
	if _file.get_32() != PACK_HEADER_MAGIC:
		return _header_failure(REASON_BAD_HEADER, "the file does not start with the GDPC pack magic")
	var version := _file.get_32()
	if version < PACK_FORMAT_VERSION_V2 or version > PACK_FORMAT_VERSION_V4:
		return _header_failure(REASON_BAD_HEADER, "unsupported pack-format version %d (expected 2 to 4)" % version)

	var header_size := PACK_V2_HEADER_SIZE
	var dir_offset := PACK_V2_HEADER_SIZE
	if version != PACK_FORMAT_VERSION_V2:
		if _size < PACK_V3_V4_HEADER_SIZE:
			return _header_failure(REASON_BAD_HEADER, "the file is %d bytes, shorter than the v3/v4 pack header" % _size)
		header_size = PACK_V3_V4_HEADER_SIZE
		_file.seek(32)
		dir_offset = _file.get_64()

	_file.seek(20)
	var pack_flags := _file.get_32()

	# The directory-encrypted bit is the only header field that says whether the
	# pack this file describes is protected. It is recorded as unknown when the
	# header stops parsing before this word (bad magic, unsupported version).
	_dir_encrypted_known = true
	_dir_encrypted = (pack_flags & PACK_DIR_ENCRYPTED) != 0

	# A 64-bit value above 2^63 reads back negative: the range overflowed.
	if dir_offset < 0:
		return _header_failure(REASON_DIRECTORY_PAST_EOF, "the directory offset does not fit in a signed 64-bit range")
	if dir_offset < header_size:
		return _header_failure(REASON_BAD_HEADER, "the directory offset %d points inside the %d-byte header" % [dir_offset, header_size])
	if dir_offset > _size - 4:
		return _header_failure(REASON_DIRECTORY_PAST_EOF, "the directory offset %d leaves no room for the entry count in a %d-byte file" % [dir_offset, _size])

	_file.seek(dir_offset)
	var entry_count := _file.get_32()
	if entry_count == 0:
		return _header_failure(REASON_ZERO_ENTRIES, "the directory declares zero entries")

	var available := _size - dir_offset - 4
	if (pack_flags & PACK_DIR_ENCRYPTED) == 0:
		var needed := entry_count * PACK_MIN_ENTRY_SIZE
		if needed > available:
			return _header_failure(REASON_DIRECTORY_PAST_EOF, "the directory declares %d entries needing at least %d bytes, but only %d bytes follow the count" % [entry_count, needed, available])
	elif available <= 0:
		return _header_failure(REASON_DIRECTORY_PAST_EOF, "the encrypted directory declares %d entries but no directory bytes follow the count" % entry_count)

	return {"ok": true, "entries": entry_count}


func _header_failure(reason: String, detail: String) -> Dictionary:
	return {"ok": false, "reason": reason, "detail": detail}


## Single-pass anchor search over one slice, with a bounded fallback to the
## 8-alignment word walk so pathological data full of `_needle[0]` cannot
## regress below the original scan speed.
func _window_contains_needle(window: PackedByteArray) -> bool:
	if _native_scanner != null:
		return bool(_native_scanner.call(&"buffer_contains_bytes", window, _needle))

	var last_start := window.size() - _needle.size()
	if last_start < 0:
		return false

	var first_byte := int(_needle[0])
	var pos := window.find(first_byte, 0)
	var candidates := 0
	while pos >= 0 and pos <= last_start:
		if window.decode_s64(pos) == _anchor and window.slice(pos, pos + _needle.size()) == _needle:
			return true
		candidates += 1
		if candidates >= SCAN_CANDIDATE_BUDGET:
			return _window_contains_needle_aligned(window.slice(pos + 1))
		pos = window.find(first_byte, pos + 1)
	return false


## Byte-wise search of one slice, done natively: the 8-byte needle prefix is
## looked up in a packed 64-bit view of the slice for each of the 8 possible
## alignments, which covers every byte offset exactly once. Only an anchor match
## is compared against the full needle. Both the packed view and `decode_s64`
## read little-endian words, as every supported host does.
func _window_contains_needle_aligned(window: PackedByteArray) -> bool:
	var last_start := window.size() - _needle.size()
	if last_start < 0:
		return false

	var alignment := 0
	while alignment < ANCHOR_SIZE:
		var length := window.size() - alignment
		if length < ANCHOR_SIZE:
			break
		var lane := window.slice(alignment, alignment + length - (length % ANCHOR_SIZE))
		var words := lane.to_int64_array()
		var hit := words.find(_anchor)
		while hit >= 0:
			var start := alignment + hit * ANCHOR_SIZE
			if start <= last_start and window.slice(start, start + _needle.size()) == _needle:
				return true
			hit = words.find(_anchor, hit + 1)
		alignment += 1
	return false
