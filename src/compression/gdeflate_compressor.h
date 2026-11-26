// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef GDEFLATE_COMPRESSOR_H
#define GDEFLATE_COMPRESSOR_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

class GDeflateCompressor : public RefCounted {
	GDCLASS(GDeflateCompressor, RefCounted);

private:
	int compression_level = 1;

protected:
	static void _bind_methods();

public:
	GDeflateCompressor();
	~GDeflateCompressor();

	PackedByteArray compress(const PackedByteArray &p_input, int p_level = -1);
	int compress_bound(int p_input_size) const;

	void set_compression_level(int p_level);
	int get_compression_level() const;
};

} // namespace godot

#endif // GDEFLATE_COMPRESSOR_H
