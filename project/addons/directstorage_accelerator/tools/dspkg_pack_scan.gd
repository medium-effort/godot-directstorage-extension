extends SceneTree

## Command-line front end for the DSPkg pack scan.
##
## Usage (the pack path follows the `--` separator, as a user argument):
##   godot --headless --path project --script res://addons/directstorage_accelerator/tools/dspkg_pack_scan.gd -- --pack <path>
##
## A relative path is resolved by the engine against the project directory that
## `--path` selects (the engine changes into it at startup); pass an absolute
## path when the pack lives outside the project, and quote it if it contains
## spaces.
##
## Verdict (stdout, exactly one machine line):
##   DSPKG_SCAN status=ok entries=<n> file=<path>
##   DSPKG_SCAN status=fail reason=<bad_header|zero_entries|directory_past_eof|key_magic_found|unreadable> file=<path>
##
## A human-readable detail line is printed on every failure (stderr). The exit
## code is 0 only for `status=ok`, 1 for every failure verdict and 2 for a
## usage error.
##
## The scan itself lives in dspkg_pack_scan_core.gd, which the editor dialog
## also uses: this file only parses arguments, drives the scan to completion and
## prints the shared verdict. The same command runs from a terminal, from CI, or
## from the add-on's Project > Tools entry, with identical output.

const ScanCore := preload("uid://cgmf75ap4elct")


func _init() -> void:
	var pack_path := _pack_path_from_args()
	if pack_path.is_empty():
		printerr(ScanCore.USAGE)
		quit(ScanCore.EXIT_USAGE)
		return

	var core := ScanCore.new()
	var opened: Dictionary = core.start(pack_path)
	if opened["ok"]:
		core.scan_all()

	var verdict: Dictionary = core.get_verdict()
	if not verdict["ok"]:
		printerr("pack scan %s: %s" % [verdict["reason"], verdict["detail"]])
	print(core.format_verdict(pack_path))
	quit(core.exit_code())


## Returns the path after `--pack`, in either `--pack <path>` or `--pack=<path>`
## form, or an empty string when the argument is missing.
func _pack_path_from_args() -> String:
	var args := OS.get_cmdline_user_args()
	var index := 0
	while index < args.size():
		var arg := args[index]
		if arg == "--pack":
			if index + 1 >= args.size():
				return ""
			return args[index + 1]
		if arg.begins_with("--pack="):
			return arg.substr("--pack=".length())
		index += 1
	return ""
