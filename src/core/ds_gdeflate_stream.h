// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// Shared GDeflate tile-stream header validation.
//
// The Win32 backend validates a GDeflate payload's 8-byte tile-stream header
// before handing it to DirectStorage (issues #0181 / #0252 / #0257): a header
// that declares a different uncompressed size than the request makes the GPU
// compute shader write past its destination while still reporting success.
// DSPreloader now performs the same validation off-thread so the main/render
// thread does not pay a synchronous FileAccess open/read per mip. Keeping the
// rule in one inline function stops the two call sites from drifting.

#ifndef DS_GDEFLATE_STREAM_H
#define DS_GDEFLATE_STREAM_H

#include "TileStream.h" // thirdparty/gdeflate, on the SCons CPPPATH

#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dsgd_gdeflate {

// Validates an already-read tile-stream header. p_prefix/p_path/p_offset are
// used only to build the same error text the backend has always emitted, so
// callers can report "Win32Backend: ..." or "DSPreloader: ..." consistently.
inline bool validate_stream_header(const uint8_t *p_bytes, size_t p_len,
		uint64_t p_source_offset, uint64_t p_declared_size,
		const godot::String &p_prefix, const godot::String &p_path,
		godot::String &r_error) {
	if (p_bytes == nullptr || p_len < sizeof(GDeflate::TileStream)) {
		r_error = p_prefix + godot::String("GDeflate payload at offset ") +
				godot::String::num_uint64(p_source_offset) + godot::String(" in '") + p_path +
				godot::String("' is shorter than a tile-stream header.");
		return false;
	}

	// TileStream has no default constructor; copy the packed 8 bytes into an
	// aligned buffer instead of reading through a potentially unaligned pointer.
	alignas(8) uint8_t header_bytes[sizeof(GDeflate::TileStream)] = {};
	std::memcpy(header_bytes, p_bytes, sizeof(GDeflate::TileStream));
	const GDeflate::TileStream *header = reinterpret_cast<const GDeflate::TileStream *>(header_bytes);
	if (!header->IsValid() || header->id != GDeflate::kGDeflateId || header->tileSizeIdx != 1 ||
			header->numTiles == 0 || header->lastTileSize > GDeflate::kDefaultTileSize) {
		r_error = p_prefix + godot::String("Invalid GDeflate tile-stream header at offset ") +
				godot::String::num_uint64(p_source_offset) + godot::String(" in '") + p_path +
				godot::String("'.");
		return false;
	}

	const uint64_t stream_size = static_cast<uint64_t>(header->GetUncompressedSize());
	if (stream_size != p_declared_size) {
		r_error = p_prefix + godot::String("GDeflate stream declares ") + godot::String::num_uint64(stream_size) +
				godot::String(" uncompressed bytes but the request declared ") + godot::String::num_uint64(p_declared_size) +
				godot::String("; refusing to decompress into a destination sized by the declared value.");
		return false;
	}
	return true;
}

} // namespace dsgd_gdeflate

#endif // DS_GDEFLATE_STREAM_H
