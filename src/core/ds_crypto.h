// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef DS_CRYPTO_H
#define DS_CRYPTO_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Native cryptographic helpers for the runtime installer. SHA-512 is what
// NuGet publishes for package integrity (the packageHash field and the .sha512
// sidecar), and Godot's HashingContext only exposes MD5/SHA-1/SHA-256.
class DSCrypto : public RefCounted {
	GDCLASS(DSCrypto, RefCounted);

protected:
	static void _bind_methods();

public:
	// Returns the raw 64-byte SHA-512 digest of p_path, or an empty array when
	// the file cannot be read or the platform has no SHA-512 backend. Windows
	// uses CNG (bcrypt); every other platform currently returns empty.
	static PackedByteArray sha512_file(const String &p_path);
};

} // namespace godot

#endif // DS_CRYPTO_H
