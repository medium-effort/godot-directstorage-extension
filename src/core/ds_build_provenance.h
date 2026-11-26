// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// The editor and a debug player load dsgd_64.debug.dll; an exported release
// player loads dsgd_64.dll. Only one of the two is ever running, so nothing at
// runtime compares them and a stale release binary stays invisible until an
// export behaves differently from the editor. This check reads the shipped
// build_manifest.json once per process and warns when the two binaries were
// built from different source snapshots.
//
// It never fails a boot. A missing manifest, an entry written by an older build
// tool (no source snapshot), or a binary whose sha256 no longer matches its
// entry all end the check silently: none of them can prove a mismatch, and a
// false warning is worse than none.
void ds_check_binary_provenance();

// Selection result for one manifest "targets" dictionary.
struct BinaryProvenancePair {
	bool valid = false;
	String this_file;
	Dictionary this_entry;
	String other_file;
	Dictionary other_entry;
};

// Pure helpers, exposed for tests.
//
// Picks the entry for the running build type (p_debug_build) and the entry for
// the other one. An entry stamped target_type=editor is the debug binary: a
// target=editor build writes dsgd_64.debug.dll under that type.
BinaryProvenancePair ds_select_binary_provenance(const Dictionary &p_targets, bool p_debug_build);

// True only when both entries carry a source snapshot and the snapshots differ.
bool ds_binary_provenance_mismatch(const BinaryProvenancePair &p_pair);

// Checks one directory and returns the warning text, or an empty String when the
// directory has no manifest, the pair cannot be proven, or the binaries match.
// Exposed so a test can point it at a fixture directory.
String ds_binary_provenance_warning_for_dir(const String &p_dir);

} // namespace godot
