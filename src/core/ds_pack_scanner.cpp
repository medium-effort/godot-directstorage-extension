// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ds_pack_scanner.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace godot {

namespace {

bool scan_span(const uint8_t *p_hay, int64_t p_hay_len, const uint8_t *p_needle, int64_t p_needle_len) {
	if (p_needle_len <= 0 || p_hay_len < p_needle_len) {
		return false;
	}
	const uint8_t first = p_needle[0];
	const uint8_t *cur = p_hay;
	const uint8_t *limit = p_hay + (p_hay_len - p_needle_len) + 1;

	while (cur < limit) {
		const void *hit = std::memchr(cur, first, static_cast<size_t>(limit - cur));
		if (!hit) {
			return false;
		}
		const uint8_t *pos = static_cast<const uint8_t *>(hit);
		if (p_needle_len == 1 || std::memcmp(pos + 1, p_needle + 1, static_cast<size_t>(p_needle_len - 1)) == 0) {
			return true;
		}
		cur = pos + 1;
	}
	return false;
}

} // namespace

void DSPackScanner::_bind_methods() {
	ClassDB::bind_static_method("DSPackScanner", D_METHOD("buffer_contains_bytes", "haystack", "needle"), &DSPackScanner::buffer_contains_bytes);
	ClassDB::bind_static_method("DSPackScanner", D_METHOD("file_contains_bytes", "path", "needle"), &DSPackScanner::file_contains_bytes);
}

bool DSPackScanner::buffer_contains_bytes(const PackedByteArray &p_haystack, const PackedByteArray &p_needle) {
	return scan_span(p_haystack.ptr(), p_haystack.size(), p_needle.ptr(), p_needle.size());
}

Dictionary DSPackScanner::file_contains_bytes(const String &p_path, const PackedByteArray &p_needle) {
	Dictionary result;
	result["ok"] = false;
	result["found"] = false;
	result["size"] = int64_t(0);

	const int64_t needle_len = p_needle.size();
	if (needle_len <= 0) {
		return result;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return result;
	}

	const int64_t total_size = static_cast<int64_t>(file->get_length());
	result["size"] = total_size;
	if (total_size < needle_len) {
		result["ok"] = true;
		return result;
	}

	constexpr int64_t kChunkSize = 4 * 1024 * 1024;
	const int64_t carry_cap = needle_len - 1;
	std::vector<uint8_t> buffer(static_cast<size_t>(carry_cap + kChunkSize));
	const uint8_t *needle_ptr = p_needle.ptr();

	int64_t carry_len = 0;
	int64_t remaining = total_size;
	while (remaining > 0) {
		const int64_t want = std::min(kChunkSize, remaining);
		const uint64_t got = file->get_buffer(buffer.data() + carry_len, static_cast<uint64_t>(want));
		if (static_cast<int64_t>(got) != want) {
			return result;
		}

		const int64_t window_len = carry_len + want;
		if (scan_span(buffer.data(), window_len, needle_ptr, needle_len)) {
			result["ok"] = true;
			result["found"] = true;
			return result;
		}

		carry_len = std::min(carry_cap, window_len);
		if (carry_len > 0) {
			std::memmove(buffer.data(), buffer.data() + (window_len - carry_len), static_cast<size_t>(carry_len));
		}
		remaining -= want;
	}

	result["ok"] = true;
	return result;
}

} // namespace godot
