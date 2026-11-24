#include "ds_build_provenance.h"
#include "ds_log.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace godot {

namespace {

constexpr const char *MANIFEST_FILE = "build_manifest.json";
// The editor and a debug player load the debug binary, an exported player loads
// the release one; both live in this one directory, so the manifest sits next to
// whichever binary is running.
constexpr const char *ADDON_BIN_DIR = "res://addons/directstorage_accelerator/bin";

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
constexpr bool RUNNING_DEBUG_BUILD = true;
#else
constexpr bool RUNNING_DEBUG_BUILD = false;
#endif

// The manifest stores an ISO timestamp with the offset of the machine that built
// it. Rendering from the unix field in the reader's own zone avoids parsing a
// timestamp that may belong to a different machine.
String _format_local_time(int64_t p_unix_time) {
	Time *time = Time::get_singleton();
	if (!time || p_unix_time <= 0) {
		return String();
	}
	const Dictionary system_zone = time->get_time_zone_from_system();
	const int64_t bias_minutes = system_zone.get("bias", 0);
	const Dictionary dt = time->get_datetime_dict_from_unix_time(p_unix_time + bias_minutes * 60);
	return String::num_int64(dt.get("year", 0)) + "-" +
			String::num_int64(dt.get("month", 0)).pad_zeros(2) + "-" +
			String::num_int64(dt.get("day", 0)).pad_zeros(2) + " " +
			String::num_int64(dt.get("hour", 0)).pad_zeros(2) + ":" +
			String::num_int64(dt.get("minute", 0)).pad_zeros(2);
}

String _describe_gap(int64_t p_seconds) {
	if (p_seconds < 60) {
		return "less than a minute";
	}
	if (p_seconds < 3600) {
		const int64_t minutes = p_seconds / 60;
		return String::num_int64(minutes) + (minutes == 1 ? " minute" : " minutes");
	}
	if (p_seconds < 86400) {
		const int64_t hours = p_seconds / 3600;
		return String::num_int64(hours) + (hours == 1 ? " hour" : " hours");
	}
	const int64_t days = p_seconds / 86400;
	return String::num_int64(days) + (days == 1 ? " day" : " days");
}

// A record only describes the binary on disk when its content hash still matches.
// Size and mtime are not enough: the release binary of a different build can have
// the same size, and SCons' shared cache restores artifacts with their original
// mtime.
bool _entry_matches_file(const String &p_dir, const String &p_file, const Dictionary &p_entry) {
	if (!bool(p_entry.get("exists", false))) {
		return false;
	}
	const String expected = String(p_entry.get("sha256", String())).to_lower();
	if (expected.is_empty()) {
		return false;
	}
	const String path = p_dir.path_join(p_file);
	if (!FileAccess::file_exists(path)) {
		return false;
	}
	return FileAccess::get_sha256(path).to_lower() == expected;
}

String _describe_entry(const String &p_file, const Dictionary &p_entry, const String &p_role) {
	String line = "  " + p_file + " (" + p_role + "): commit " + String(p_entry.get("last_commit", "unknown"));
	const String built = _format_local_time(p_entry.get("last_built_unix", 0));
	if (!built.is_empty()) {
		line += ", built " + built;
	}
	if (bool(p_entry.get("last_dirty", false))) {
		line += " (uncommitted changes)";
	}
	return line;
}

String _build_warning(const String &p_dir, const BinaryProvenancePair &p_pair) {
	const String other_role = RUNNING_DEBUG_BUILD ? "for export" : "editor/debug";
	String message = String(dsgd_log::EXTENSION_PREFIX) + "Binary mismatch in '" + p_dir + "':\n";
	message += _describe_entry(p_pair.this_file, p_pair.this_entry, "this build");
	message += "\n" + _describe_entry(p_pair.other_file, p_pair.other_entry, other_role);

	const int64_t this_built = p_pair.this_entry.get("last_built_unix", 0);
	const int64_t other_built = p_pair.other_entry.get("last_built_unix", 0);
	if (this_built > 0 && other_built > 0 && this_built != other_built) {
		const int64_t delta = this_built - other_built;
		message += "  (" + _describe_gap(delta < 0 ? -delta : delta) + (delta > 0 ? " earlier)" : " later)");
	}
	message += "\n";
	if (RUNNING_DEBUG_BUILD) {
		message += "Exports will not run the binary you are testing. Rebuild the release target to match.";
	} else {
		message += "What you tested in the editor or a debug build is not this binary.";
	}
	return message;
}

} // namespace

BinaryProvenancePair ds_select_binary_provenance(const Dictionary &p_targets, bool p_debug_build) {
	BinaryProvenancePair pair;
	const Array files = p_targets.keys();
	for (int i = 0; i < files.size(); i++) {
		const Variant value = p_targets.get(files[i], Variant());
		if (value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = value;
		const String type = String(entry.get("target_type", String()));
		// "editor" is how a target=editor build (the gdextest fixture) stamps the
		// same dsgd_64.debug.dll, so both count as the debug binary.
		const bool is_debug = type == "template_debug" || type == "editor";
		const bool is_release = type == "template_release";
		if (is_debug == is_release) {
			continue; // Unknown/custom target: never guess what it is.
		}
		const bool matches_running = is_debug == p_debug_build;
		if (matches_running && pair.this_file.is_empty()) {
			pair.this_file = files[i];
			pair.this_entry = entry;
		} else if (!matches_running && pair.other_file.is_empty()) {
			pair.other_file = files[i];
			pair.other_entry = entry;
		}
	}
	pair.valid = !pair.this_file.is_empty() && !pair.other_file.is_empty();
	return pair;
}

bool ds_binary_provenance_mismatch(const BinaryProvenancePair &p_pair) {
	if (!p_pair.valid) {
		return false;
	}
	const String this_source = String(p_pair.this_entry.get("source_id", String()));
	const String other_source = String(p_pair.other_entry.get("source_id", String()));
	if (this_source.is_empty() || other_source.is_empty()) {
		return false;
	}
	return this_source != other_source;
}

void ds_check_binary_provenance() {
	PackedStringArray dirs;
	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings) {
		const String addon_bin = project_settings->globalize_path(ADDON_BIN_DIR);
		if (!addon_bin.is_empty()) {
			dirs.push_back(addon_bin);
		}
	}
	OS *os = OS::get_singleton();
	if (os) {
		// Exported builds place the extension binaries flat next to the executable,
		// so the res:// path above exists only in the editor.
		const String exe_dir = os->get_executable_path().get_base_dir();
		if (!exe_dir.is_empty() && !dirs.has(exe_dir)) {
			dirs.push_back(exe_dir);
		}
	}

	for (int i = 0; i < dirs.size(); i++) {
		const String warning = ds_binary_provenance_warning_for_dir(dirs[i]);
		if (!warning.is_empty()) {
			UtilityFunctions::push_warning(warning);
			return; // One warning per process; a second copy would only repeat it.
		}
	}
}

String ds_binary_provenance_warning_for_dir(const String &p_dir) {
	if (p_dir.is_empty()) {
		return String();
	}
	const String manifest_path = p_dir.path_join(MANIFEST_FILE);
	if (!FileAccess::file_exists(manifest_path)) {
		return String();
	}

	const Variant parsed = JSON::parse_string(FileAccess::get_file_as_string(manifest_path));
	if (parsed.get_type() != Variant::DICTIONARY) {
		return String();
	}
	const Dictionary manifest = parsed;
	const Variant targets_value = manifest.get("targets", Variant());
	if (targets_value.get_type() != Variant::DICTIONARY) {
		return String();
	}

	const BinaryProvenancePair pair = ds_select_binary_provenance(targets_value, RUNNING_DEBUG_BUILD);
	if (!ds_binary_provenance_mismatch(pair)) {
		return String();
	}
	// Only the mismatch path reads both binaries: the common case costs one small
	// JSON read, and a hash is what turns a manifest claim into proof that each
	// entry still describes the file on disk.
	if (!_entry_matches_file(p_dir, pair.this_file, pair.this_entry) ||
			!_entry_matches_file(p_dir, pair.other_file, pair.other_entry)) {
		return String();
	}
	return _build_warning(p_dir, pair);
}

} // namespace godot
