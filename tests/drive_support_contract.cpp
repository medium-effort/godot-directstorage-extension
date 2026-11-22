// Invariant: the UNC rejection verdict comes from the single source of truth in
// resolve_drive_support(), and the resolver's is_unc input has a production caller.
// The returned dictionary is unchanged; this case guards provenance, not output.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>

#include <string>

namespace {

using std::string;

bool contains(const string &p_text, const char *p_needle) {
	return p_text.find(p_needle) != string::npos;
}

godot::String find_repo_root() {
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (!ps) {
		return godot::String();
	}
	godot::String dir = ps->globalize_path("res://");
	for (int i = 0; i < 6; ++i) {
		if (dir.is_empty()) {
			break;
		}
		const godot::String probe = dir.path_join("src/platform/win32/ds_win32_backend.cpp");
		if (godot::FileAccess::file_exists(probe)) {
			return dir;
		}
		const godot::String parent = dir.get_base_dir();
		if (parent == dir) {
			break;
		}
		dir = parent;
	}
	return godot::String();
}

bool read_source(const godot::String &p_relative, string &r_text) {
	const godot::String root = find_repo_root();
	if (root.is_empty()) {
		return false;
	}
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(root.path_join(p_relative), godot::FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	r_text = ::gdextest::to_std_string(file->get_as_text());
	file->close();
	return !r_text.empty();
}

} // namespace

GDEX_TEST(drive_support_contract, unc_path_uses_single_source_of_truth) {
	(void)ctx;
	string backend;
	GDEX_EXPECT_TRUE(read_source("src/platform/win32/ds_win32_backend.cpp", backend));
	if (backend.empty()) {
		GDEX_FAIL("could not read the Win32 backend source");
		return;
	}

	// Scope to the UNC branch only: the same assignments exist in the local leg.
	const size_t start = backend.find("norm_path.begins_with");
	const size_t end = start == string::npos ? string::npos : backend.find("return res;", start);
	GDEX_EXPECT_TRUE(start != string::npos && end != string::npos && end > start);
	if (start == string::npos || end == string::npos || end <= start) {
		return;
	}
	const string unc_slice = backend.substr(start, end - start);

	GDEX_EXPECT_FALSE(contains(unc_slice, "DirectStorage and BypassIO are not supported on UNC network shares."));
	GDEX_EXPECT_TRUE(contains(unc_slice, "unc_input.is_unc = true;"));
	GDEX_EXPECT_TRUE(contains(unc_slice, "resolve_drive_support(unc_input)"));
	GDEX_EXPECT_TRUE(contains(unc_slice, "res[\"bypassio_supported\"] = support.bypassio_supported;"));
	GDEX_EXPECT_TRUE(contains(unc_slice, "res[\"force_file_buffering\"] = support.force_file_buffering;"));
	GDEX_EXPECT_TRUE(contains(unc_slice, "res[\"directstorage_supported\"] = support.directstorage_supported;"));

	// Positive control: the sentence and the is_unc arm still live in the shared header.
	string header;
	GDEX_EXPECT_TRUE(read_source("src/core/ds_drive_support.h", header));
	GDEX_EXPECT_TRUE(contains(header, "if (p_input.is_unc)"));
	GDEX_EXPECT_TRUE(contains(header, "DirectStorage and BypassIO are not supported on UNC network shares."));
}
