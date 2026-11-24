// tests/static_lifetime_tests.cpp
//
// Invariant: unauthorized-archive log dedup state is native and trivially
// destructible. A function-local static godot::String only defers construction;
// its destructor runs at DLL unload, after Godot's allocator has been torn down
// (GODOT_QUIRKS.md Section 6). These cases scan the three source files that
// used that pattern and assert the native replacement is present and the
// diagnostic contract survived.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cctype>
#include <string>

namespace {

using std::string;

bool contains(const string &p_text, const char *p_needle) {
	return p_text.find(p_needle) != string::npos;
}

// Counts declarations of the form `static String s_name ... ;` on a single
// line. The s_ prefix and the trailing semicolon keep this from matching the
// safe free functions that merely return a String.
int count_static_godot_string_decls(const string &p_text) {
	const string prefix = "static String s_";
	int count = 0;
	size_t search_from = 0;
	while (true) {
		const size_t at = p_text.find(prefix, search_from);
		if (at == string::npos) {
			break;
		}
		const size_t semi = p_text.find(';', at + prefix.size());
		const size_t newline = p_text.find('\n', at + prefix.size());
		if (semi != string::npos && (newline == string::npos || semi < newline)) {
			++count;
		}
		search_from = at + prefix.size();
	}
	return count;
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

void assert_shared_invariants(gdextest::TestContext &ctx, const string &p_text, const char *p_message_fragment,
		const char *p_error_predicate, const char *p_failure_contract,
		const char *p_first_state, const char *p_second_state, const char *p_mutex) {
	GDEX_EXPECT_FALSE(count_static_godot_string_decls(p_text) != 0);
	GDEX_EXPECT_TRUE(contains(p_text, p_first_state));
	GDEX_EXPECT_TRUE(contains(p_text, p_second_state));
	GDEX_EXPECT_TRUE(contains(p_text, p_mutex));
	GDEX_EXPECT_TRUE(contains(p_text, p_message_fragment));
	GDEX_EXPECT_TRUE(contains(p_text, p_error_predicate));
	GDEX_EXPECT_TRUE(contains(p_text, p_failure_contract));
}

} // namespace

GDEX_TEST(static_lifetime, win32_backend_dedup_has_no_static_godot_string) {
	string text;
	GDEX_EXPECT_TRUE(read_source("src/platform/win32/ds_win32_backend.cpp", text));
	if (text.empty()) {
		GDEX_FAIL("could not read the Win32 backend source");
		return;
	}
	GDEX_EXPECT_FALSE(contains(text, "static String s_last_backend_unauth_path"));
	assert_shared_invariants(ctx, text,
			"Decryption unauthorized or key mismatch for archive",
			"open_err == ERR_UNAUTHORIZED",
			"return false;",
			"s_last_backend_unauth_hash", "s_has_backend_unauth_path", "s_backend_unauth_log_mutex");
}

GDEX_TEST(static_lifetime, resource_loader_dedup_has_no_static_godot_string) {
	string loader_text;
	string common_hdr;
	string common_src;
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_resource_format_loader.cpp", loader_text));
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_texture_pipeline_common.h", common_hdr));
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_texture_pipeline_common.cpp", common_src));
	if (loader_text.empty() || common_hdr.empty() || common_src.empty()) {
		GDEX_FAIL("could not read resource loader or shared pipeline sources");
		return;
	}
	GDEX_EXPECT_FALSE(count_static_godot_string_decls(loader_text) != 0);
	GDEX_EXPECT_FALSE(count_static_godot_string_decls(common_hdr) != 0);
	GDEX_EXPECT_FALSE(contains(loader_text, "static String s_last_reported_unauth_path"));
	GDEX_EXPECT_TRUE(contains(loader_text, "UnauthorizedReportDedup s_loader_unauth_dedup"));
	GDEX_EXPECT_TRUE(contains(loader_text, "open_archive_with_pairing"));
	GDEX_EXPECT_TRUE(contains(loader_text, "\"DirectStorage\""));
	GDEX_EXPECT_TRUE(contains(common_hdr, "struct UnauthorizedReportDedup"));
	GDEX_EXPECT_TRUE(contains(common_hdr, "std::mutex mutex;"));
	GDEX_EXPECT_TRUE(contains(common_hdr, "uint64_t last_hash"));
	GDEX_EXPECT_TRUE(contains(common_hdr, "bool has_reported"));
	assert_shared_invariants(ctx, common_src,
			"Decryption unauthorized or key mismatch for package",
			"open_err == ERR_UNAUTHORIZED",
			"return Ref<DSPkg>();",
			"r_dedup.last_hash", "r_dedup.has_reported", "r_dedup.mutex");
}

GDEX_TEST(static_lifetime, preloader_dedup_has_no_static_godot_string) {
	string preloader_text;
	string common_hdr;
	string common_src;
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_preloader.cpp", preloader_text));
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_texture_pipeline_common.h", common_hdr));
	GDEX_EXPECT_TRUE(read_source("src/integration/ds_texture_pipeline_common.cpp", common_src));
	if (preloader_text.empty() || common_hdr.empty() || common_src.empty()) {
		GDEX_FAIL("could not read preloader or shared pipeline sources");
		return;
	}
	GDEX_EXPECT_FALSE(count_static_godot_string_decls(preloader_text) != 0);
	GDEX_EXPECT_FALSE(count_static_godot_string_decls(common_hdr) != 0);
	GDEX_EXPECT_FALSE(contains(preloader_text, "static String s_last_reported_preloader_unauth"));
	GDEX_EXPECT_TRUE(contains(preloader_text, "UnauthorizedReportDedup s_preloader_unauth_dedup"));
	GDEX_EXPECT_TRUE(contains(preloader_text, "open_archive_with_pairing"));
	GDEX_EXPECT_TRUE(contains(preloader_text, "\"DirectStorage Preloader\""));
	GDEX_EXPECT_TRUE(contains(preloader_text, "return false;"));
	assert_shared_invariants(ctx, common_src,
			"Decryption unauthorized or key mismatch for package",
			"open_err == ERR_UNAUTHORIZED",
			"return Ref<DSPkg>();",
			"r_dedup.last_hash", "r_dedup.has_reported", "r_dedup.mutex");
}
