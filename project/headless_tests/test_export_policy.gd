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

	# resolve_bundle_effective(bundle_enabled, skipped_payload_count)
	_check("bundle.option_off", Policy.resolve_bundle_effective(false, 2), false)
	# A filter that removes every texture skips nothing, so the PCK is
	# self-sufficient and no archive may be claimed.
	_check("bundle.no_skips", Policy.resolve_bundle_effective(true, 0), false)
	# The first skipped payload is the moment the archive becomes mandatory.
	_check("bundle.first_skip", Policy.resolve_bundle_effective(true, 1), true)
	_check("bundle.many_skips", Policy.resolve_bundle_effective(true, 4), true)

	# resolve_file_action(outcome, archive_failed, archive_available)
	_check("file.disabled", Policy.resolve_file_action(Policy.OUTCOME_DISABLED, false, true), Policy.ACTION_PACK)
	_check("file.plaintext.healthy", Policy.resolve_file_action(Policy.OUTCOME_PLAINTEXT, false, true), Policy.ACTION_ADD)
	# Outcome B cannot build the archive: keep the payload so CPU mode works.
	_check("file.plaintext.archive_failed", Policy.resolve_file_action(Policy.OUTCOME_PLAINTEXT, true, false), Policy.ACTION_PACK)
	_check("file.protected.healthy", Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, false, true), Policy.ACTION_ADD)
	# Outcome C must not leave a plaintext copy when the archive is gone.
	_check("file.protected.archive_failed", Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, true, false), Policy.ACTION_SKIP)
	_check("file.protected.archive_missing", Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, false, false), Policy.ACTION_SKIP)
	# Outcome D is fail-closed even before anything was attempted.
	_check("file.failed_gate", Policy.resolve_file_action(Policy.OUTCOME_FAILED, false, true), Policy.ACTION_SKIP)
	_check("file.failed_gate_archive_gone", Policy.resolve_file_action(Policy.OUTCOME_FAILED, true, false), Policy.ACTION_SKIP)

	# resolve_failure_action(outcome, archive_entry_count)
	_check("failure.plaintext_with_entries", Policy.resolve_failure_action(Policy.OUTCOME_PLAINTEXT, 3), Policy.ACTION_SALVAGE)
	_check("failure.plaintext_no_entries", Policy.resolve_failure_action(Policy.OUTCOME_PLAINTEXT, 0), Policy.ACTION_DELETE)
	_check("failure.protected_with_entries", Policy.resolve_failure_action(Policy.OUTCOME_PROTECTED, 3), Policy.ACTION_DELETE)
	_check("failure.protected_no_entries", Policy.resolve_failure_action(Policy.OUTCOME_PROTECTED, 0), Policy.ACTION_DELETE)
	_check("failure.failed_gate", Policy.resolve_failure_action(Policy.OUTCOME_FAILED, 0), Policy.ACTION_DELETE)

	print("RESULT ", "PASS" if _failures == 0 else "FAIL", " checks=", _checks, " failures=", _failures)
	quit(0 if _failures == 0 else 1)


func _check(p_name: String, p_actual: Variant, p_expected: Variant) -> void:
	_checks += 1
	if p_actual == p_expected:
		print("[PASS] ", p_name, " :: ", p_actual)
	else:
		_failures += 1
		printerr("[FAIL] ", p_name, " :: expected=", p_expected, " actual=", p_actual)
