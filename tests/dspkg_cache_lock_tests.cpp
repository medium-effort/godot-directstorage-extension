// tests/dspkg_cache_lock_tests.cpp
//
// Invariant: the Win32Backend .dspkg archive cache is an instance-owned
// Ref<DSPkg>/String pair. enqueue_request() calls get_texture_mip_entries()
// before taking batch_mutex, so the cache cannot rely on that lock. These
// cases scan the current source and assert that every cache access is inside
// a dedicated short critical section while the archive open stays outside it.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cctype>
#include <string>
#include <vector>

namespace {

using std::string;

struct ScannedSite {
	int line = 0;
	string guard;
	string text;
};

bool contains(const string &p_text, const char *p_needle) {
	return p_text.find(p_needle) != string::npos;
}

std::vector<string> split_lines(const string &p_text) {
	std::vector<string> lines;
	string current;
	for (const char c : p_text) {
		if (c == '\n') {
			lines.push_back(current);
			current.clear();
		} else if (c != '\r') {
			current.push_back(c);
		}
	}
	if (!current.empty()) {
		lines.push_back(current);
	}
	return lines;
}

// Records the lock_guard variable in scope at each access site. A guard
// applies from its declaration to the end of the enclosing brace scope.
std::vector<ScannedSite> scan_guarded_sites(const std::vector<string> &p_lines, const char *p_needle) {
	std::vector<string> guard_stack;
	std::vector<ScannedSite> sites;
	const string marker = "std::lock_guard<std::mutex>";
	for (size_t i = 0; i < p_lines.size(); ++i) {
		const string &line = p_lines[i];
		if (line.find(p_needle) != string::npos) {
			ScannedSite site;
			site.line = static_cast<int>(i) + 1;
			site.guard = guard_stack.empty() ? string() : guard_stack.back();
			site.text = line;
			sites.push_back(site);
		}
		const size_t at = line.find(marker);
		if (at != string::npos) {
			size_t name_start = at + marker.size();
			while (name_start < line.size() && line[name_start] == ' ') {
				++name_start;
			}
			size_t name_end = name_start;
			while (name_end < line.size() &&
					(std::isalnum(static_cast<unsigned char>(line[name_end])) || line[name_end] == '_')) {
				++name_end;
			}
			if (!guard_stack.empty()) {
				guard_stack.back() = line.substr(name_start, name_end - name_start);
			}
		}
		for (const char c : line) {
			if (c == '{') {
				guard_stack.push_back(string());
			} else if (c == '}') {
				if (!guard_stack.empty()) {
					guard_stack.pop_back();
				}
			}
		}
	}
	return sites;
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

GDEX_TEST(dspkg_cache_lock, win32_backend_dspkg_cache_accesses_are_locked) {
	string header;
	GDEX_EXPECT_TRUE(read_source("src/platform/win32/ds_win32_backend.h", header));
	string source;
	GDEX_EXPECT_TRUE(read_source("src/platform/win32/ds_win32_backend.cpp", source));
	if (header.empty() || source.empty()) {
		GDEX_FAIL("could not read the Win32 backend sources");
		return;
	}

	GDEX_EXPECT_TRUE(contains(header, "mutable std::mutex dspkg_cache_mutex;"));
	GDEX_EXPECT_FALSE(contains(header, "All access happens under batch_mutex (enqueue_request holds it across the"));

	const std::vector<ScannedSite> sites = scan_guarded_sites(split_lines(source), "dspkg_cache.");
	GDEX_EXPECT_TRUE(sites.size() >= 4);
	for (size_t i = 0; i < sites.size(); ++i) {
		if (sites[i].guard != "cache_lock") {
			GDEX_FAIL("dspkg_cache access at line " + std::to_string(sites[i].line) +
					" is not guarded by dspkg_cache_mutex (guard='" + sites[i].guard + "')");
		}
	}
}

GDEX_TEST(dspkg_cache_lock, archive_open_stays_outside_cache_lock) {
	string source;
	GDEX_EXPECT_TRUE(read_source("src/platform/win32/ds_win32_backend.cpp", source));
	if (source.empty()) {
		GDEX_FAIL("could not read the Win32 backend source");
		return;
	}

	const std::vector<string> lines = split_lines(source);
	const std::vector<ScannedSite> open_sites = scan_guarded_sites(lines, "fresh->open(resolved_path");
	GDEX_EXPECT_EQ(open_sites.size(), static_cast<size_t>(1));
	if (open_sites.empty()) {
		GDEX_FAIL("archive open site not found");
		return;
	}
	GDEX_EXPECT_TRUE(open_sites[0].guard.empty());

	const std::vector<ScannedSite> cache_sites = scan_guarded_sites(lines, "dspkg_cache.");
	GDEX_EXPECT_TRUE(cache_sites.size() >= 4);
	if (cache_sites.empty()) {
		return;
	}
	GDEX_EXPECT_TRUE(cache_sites.front().line < open_sites[0].line);
	GDEX_EXPECT_TRUE(cache_sites.back().line > open_sites[0].line);
}
