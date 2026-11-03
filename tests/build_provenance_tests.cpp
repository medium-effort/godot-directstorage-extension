// tests/build_provenance_tests.cpp
//
// Invariant: the startup provenance check only speaks when the shipped manifest
// proves the debug and release binaries came from different source snapshots.
// A false warning is worse than none, so a missing snapshot, an unknown target
// type, or an unprovable pair must all stay silent.
//
// Selection and the decision are pure (no filesystem, no module init), which is
// what lets these cases run as TAG_UNIT in the compile-in fixture.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"
#include "core/ds_build_provenance.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace {

bool write_text_file(const godot::String &p_path, const godot::String &p_text) {
    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(p_path, godot::FileAccess::WRITE);
    if (file.is_null()) {
        return false;
    }
    file->store_string(p_text);
    file->close();
    return true;
}

// One manifest "targets" entry. An empty snapshot id is how an entry written by
// an older build tool looks.
godot::Dictionary manifest_entry(const godot::String &p_target_type, const godot::String &p_source_id) {
    godot::Dictionary entry;
    entry["target_type"] = p_target_type;
    if (!p_source_id.is_empty()) {
        entry["source_id"] = p_source_id;
    }
    return entry;
}

godot::Dictionary file_entry(const godot::String &p_target_type, const godot::String &p_source_id,
        const godot::String &p_path, const godot::String &p_sha256) {
    godot::Dictionary entry = manifest_entry(p_target_type, p_source_id);
    entry["exists"] = true;
    entry["last_commit"] = "08d2bcf";
    entry["last_built_unix"] = 1700000000;
    entry["sha256"] = p_sha256.is_empty() ? godot::FileAccess::get_sha256(p_path) : p_sha256;
    return entry;
}

godot::BinaryProvenancePair make_pair(const godot::String &p_this_source, const godot::String &p_other_source) {
    godot::BinaryProvenancePair pair;
    pair.valid = true;
    pair.this_file = "dsgd_64.debug.dll";
    pair.this_entry = manifest_entry("template_debug", p_this_source);
    pair.other_file = "dsgd_64.dll";
    pair.other_entry = manifest_entry("template_release", p_other_source);
    return pair;
}

} // namespace

GDEX_TEST(build_provenance, selects_debug_and_release) {
    (void)ctx;
    godot::Dictionary targets;
    targets["dsgd_64.debug.dll"] = manifest_entry("template_debug", "aaaa");
    targets["dsgd_64.dll"] = manifest_entry("template_release", "bbbb");

    const godot::BinaryProvenancePair debug = godot::ds_select_binary_provenance(targets, true);
    GDEX_EXPECT_TRUE(debug.valid);
    GDEX_EXPECT_EQ(::gdextest::to_std_string(debug.this_file), std::string("dsgd_64.debug.dll"));
    GDEX_EXPECT_EQ(::gdextest::to_std_string(debug.other_file), std::string("dsgd_64.dll"));
    GDEX_EXPECT_TRUE(godot::ds_binary_provenance_mismatch(debug));

    const godot::BinaryProvenancePair release = godot::ds_select_binary_provenance(targets, false);
    GDEX_EXPECT_TRUE(release.valid);
    GDEX_EXPECT_EQ(::gdextest::to_std_string(release.this_file), std::string("dsgd_64.dll"));
    GDEX_EXPECT_EQ(::gdextest::to_std_string(release.other_file), std::string("dsgd_64.debug.dll"));
}

// A target=editor build (the gdextest fixture) writes the same debug binary and
// stamps it target_type=editor, so it must still count as the debug entry.
GDEX_TEST(build_provenance, editor_target_is_the_debug_binary) {
    (void)ctx;
    godot::Dictionary targets;
    targets["dsgd_64.debug.dll"] = manifest_entry("editor", "aaaa");
    targets["dsgd_64.dll"] = manifest_entry("template_release", "bbbb");

    const godot::BinaryProvenancePair pair = godot::ds_select_binary_provenance(targets, true);
    GDEX_EXPECT_TRUE(pair.valid);
    GDEX_EXPECT_EQ(::gdextest::to_std_string(pair.this_file), std::string("dsgd_64.debug.dll"));
}

// An entry whose type is neither debug nor release describes no known binary, so
// it can never be compared: guessing would warn about the wrong file.
GDEX_TEST(build_provenance, unknown_target_types_never_guess) {
    (void)ctx;
    godot::Dictionary targets;
    targets["custom.dll"] = manifest_entry("custom", "aaaa");
    targets["dsgd_64.dll"] = manifest_entry("template_release", "bbbb");

    const godot::BinaryProvenancePair pair = godot::ds_select_binary_provenance(targets, true);
    GDEX_EXPECT_FALSE(pair.valid);
    GDEX_EXPECT_FALSE(godot::ds_binary_provenance_mismatch(pair));
}

GDEX_TEST(build_provenance, mismatch_needs_both_snapshots) {
    (void)ctx;

    // Same snapshot: the two binaries are the two build types of one source state.
    GDEX_EXPECT_FALSE(godot::ds_binary_provenance_mismatch(make_pair("aaaa", "aaaa")));

    // Either side missing a snapshot is unprovable, not a mismatch.
    GDEX_EXPECT_FALSE(godot::ds_binary_provenance_mismatch(make_pair("aaaa", "")));
    GDEX_EXPECT_FALSE(godot::ds_binary_provenance_mismatch(make_pair("", "bbbb")));

    // An invalid selection carries no opinion even if the entries disagree.
    godot::BinaryProvenancePair invalid = make_pair("aaaa", "bbbb");
    invalid.valid = false;
    GDEX_EXPECT_FALSE(godot::ds_binary_provenance_mismatch(invalid));

    // Different snapshots: the only case that may produce a warning.
    GDEX_EXPECT_TRUE(godot::ds_binary_provenance_mismatch(make_pair("aaaa", "bbbb")));
}

// The I/O half of the contract: a manifest is only believed when the hash of each
// file still matches its entry. A stale record must stay silent, because it cannot
// prove what is on disk.
GDEX_TEST(build_provenance, manifest_warning_requires_matching_hashes) {
    const godot::String dir = "user://ds_build_provenance_test";
    godot::ProjectSettings *settings = godot::ProjectSettings::get_singleton();
    const godot::String global_dir = settings ? settings->globalize_path(dir) : dir;
    const godot::String debug_path = global_dir.path_join("debug.bin");
    const godot::String release_path = global_dir.path_join("release.bin");
    const godot::String manifest_path = global_dir.path_join("build_manifest.json");

    ctx.add_teardown([global_dir, debug_path, release_path, manifest_path]() {
        const godot::String files[] = { debug_path, release_path, manifest_path };
        for (const godot::String &file : files) {
            if (godot::FileAccess::file_exists(file)) {
                godot::DirAccess::remove_absolute(file);
            }
        }
        godot::DirAccess::remove_absolute(global_dir);
    });

    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DirAccess::make_dir_recursive_absolute(global_dir)),
            static_cast<int64_t>(godot::OK));
    GDEX_EXPECT_TRUE(write_text_file(debug_path, "debug-binary"));
    GDEX_EXPECT_TRUE(write_text_file(release_path, "release-binary"));

    const auto publish = [&](const godot::String &p_release_source, const godot::String &p_release_sha) {
        godot::Dictionary targets;
        targets["debug.bin"] = file_entry("template_debug", "snapshot-a", debug_path, godot::String());
        targets["release.bin"] = file_entry("template_release", p_release_source, release_path, p_release_sha);
        godot::Dictionary manifest;
        manifest["targets"] = targets;
        return write_text_file(manifest_path, godot::JSON::stringify(manifest));
    };

    // Verified records that disagree: the only state that may warn.
    GDEX_EXPECT_TRUE(publish("snapshot-b", godot::String()));
    const godot::String warning = godot::ds_binary_provenance_warning_for_dir(dir);
    GDEX_EXPECT_FALSE(warning.is_empty());
    GDEX_EXPECT_TRUE(warning.find(godot::String("debug.bin")) != -1);
    GDEX_EXPECT_TRUE(warning.find(godot::String("release.bin")) != -1);

    // Same source snapshot: the two build types of one state, nothing to report.
    GDEX_EXPECT_TRUE(publish("snapshot-a", godot::String()));
    GDEX_EXPECT_TRUE(godot::ds_binary_provenance_warning_for_dir(dir).is_empty());

    // The record no longer describes the file on disk (the copied-DLL-without-
    // manifest case): unprovable, so silent.
    GDEX_EXPECT_TRUE(publish("snapshot-b", "0000000000000000000000000000000000000000000000000000000000000000"));
    GDEX_EXPECT_TRUE(godot::ds_binary_provenance_warning_for_dir(dir).is_empty());
}
