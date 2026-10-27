extends SceneTree

## Decision-table test for the exporter's shipping rules.
##
## Pure logic: no native extension, no export run, no EditorExportPlugin
## context. It asserts the same helpers the exporter calls, so a change to the
## policy that would ship a wrong build fails here first.
##
## Run (with --path, --script is resolved under res://, so do not prefix project/):
##   godot --headless --path project --script headless_tests/test_export_policy.gd

const Policy = preload("uid://bpbf4g8bgjeec")

var _checks := 0
var _failures := 0


func _init() -> void:
	print("--- Export policy decision table ---")

	# resolve_bundle_effective(bundle_enabled, project_has_textures,
	#                          bundle_possible, skipped_payload_count)
	_check("bundle.healthy", Policy.resolve_bundle_effective(true, true, true, 2), true)
	# Regression #0159: a filter that removes every texture skips nothing, so
	# the PCK is self-sufficient and no archive may be claimed.
	_check("bundle.no_skips", Policy.resolve_bundle_effective(true, true, true, 0), false)
	# Regression #0160: a protected export that dies before the first add skips
	# payloads, so the game must look for an archive and fail loudly without one.
	_check("bundle.protected_failure_skipped", Policy.resolve_bundle_effective(true, true, true, 1), true)
	_check("bundle.no_textures", Policy.resolve_bundle_effective(true, false, true, 0), false)
	_check("bundle.option_off", Policy.resolve_bundle_effective(false, true, true, 2), false)
	_check("bundle.delivery_disabled", Policy.resolve_bundle_effective(true, true, false, 2), false)

	# resolve_file_action(bundle_enabled, archive_failed, archive_available, mode)
	_check("file.bundling_off", Policy.resolve_file_action(false, false, true, Policy.MODE_NONE), Policy.ACTION_PACK)
	_check("file.archive_failed_unprotected", Policy.resolve_file_action(true, true, false, Policy.MODE_NONE), Policy.ACTION_PACK)
	_check("file.archive_failed_protected", Policy.resolve_file_action(true, true, false, Policy.MODE_PCK), Policy.ACTION_SKIP)
	_check("file.archive_missing_unprotected", Policy.resolve_file_action(true, false, false, Policy.MODE_NONE), Policy.ACTION_PACK)
	_check("file.archive_missing_protected", Policy.resolve_file_action(true, false, false, Policy.MODE_PCK), Policy.ACTION_SKIP)
	_check("file.healthy_unprotected", Policy.resolve_file_action(true, false, true, Policy.MODE_NONE), Policy.ACTION_ADD)
	_check("file.healthy_protected", Policy.resolve_file_action(true, false, true, Policy.MODE_PCK), Policy.ACTION_ADD)

	# resolve_failure_action(mode, archive_entry_count)
	_check("failure.none_no_entries", Policy.resolve_failure_action(Policy.MODE_NONE, 0), Policy.ACTION_DELETE)
	_check("failure.none_with_entries", Policy.resolve_failure_action(Policy.MODE_NONE, 3), Policy.ACTION_SALVAGE)
	_check("failure.pck_with_entries", Policy.resolve_failure_action(Policy.MODE_PCK, 3), Policy.ACTION_DELETE)
	_check("failure.pck_no_entries", Policy.resolve_failure_action(Policy.MODE_PCK, 0), Policy.ACTION_DELETE)

	print("RESULT ", "PASS" if _failures == 0 else "FAIL", " checks=", _checks, " failures=", _failures)
	quit(0 if _failures == 0 else 1)


func _check(p_name: String, p_actual: Variant, p_expected: Variant) -> void:
	_checks += 1
	if p_actual == p_expected:
		print("[PASS] ", p_name, " :: ", p_actual)
	else:
		_failures += 1
		printerr("[FAIL] ", p_name, " :: expected=", p_expected, " actual=", p_actual)
