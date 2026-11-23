extends GutTest

const Policy = preload("uid://bpbf4g8bgjeec")


func test_resolve_bundle_effective() -> void:
	assert_false(Policy.resolve_bundle_effective(false, 2), "bundle.option_off")
	assert_false(Policy.resolve_bundle_effective(true, 0), "bundle.no_skips")
	assert_true(Policy.resolve_bundle_effective(true, 1), "bundle.first_skip")
	assert_true(Policy.resolve_bundle_effective(true, 4), "bundle.many_skips")


func test_resolve_file_action() -> void:
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_DISABLED, false, true), Policy.ACTION_PACK, "file.disabled")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_PLAINTEXT, false, true), Policy.ACTION_ADD, "file.plaintext.healthy")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_PLAINTEXT, true, false), Policy.ACTION_PACK, "file.plaintext.archive_failed")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, false, true), Policy.ACTION_ADD, "file.protected.healthy")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, true, false), Policy.ACTION_SKIP, "file.protected.archive_failed")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_PROTECTED, false, false), Policy.ACTION_SKIP, "file.protected.archive_missing")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_FAILED, false, true), Policy.ACTION_SKIP, "file.failed_gate")
	assert_eq(Policy.resolve_file_action(Policy.OUTCOME_FAILED, true, false), Policy.ACTION_SKIP, "file.failed_gate_archive_gone")


func test_resolve_failure_action() -> void:
	assert_eq(Policy.resolve_failure_action(Policy.OUTCOME_PLAINTEXT, 3), Policy.ACTION_SALVAGE, "failure.plaintext_with_entries")
	assert_eq(Policy.resolve_failure_action(Policy.OUTCOME_PLAINTEXT, 0), Policy.ACTION_DELETE, "failure.plaintext_no_entries")
	assert_eq(Policy.resolve_failure_action(Policy.OUTCOME_PROTECTED, 3), Policy.ACTION_DELETE, "failure.protected_with_entries")
	assert_eq(Policy.resolve_failure_action(Policy.OUTCOME_PROTECTED, 0), Policy.ACTION_DELETE, "failure.protected_no_entries")
	assert_eq(Policy.resolve_failure_action(Policy.OUTCOME_FAILED, 0), Policy.ACTION_DELETE, "failure.failed_gate")
