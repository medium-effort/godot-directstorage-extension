#ifndef DS_PACK_SCANNER_H
#define DS_PACK_SCANNER_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Native byte-pattern scanner for exported Godot PCK verification.
// Provides a SIMD-accelerated (memchr) fast path for dspkg_pack_scan_core.gd
// while allowing GDScript to fall back cleanly when the native library is absent.
class DSPackScanner : public RefCounted {
	GDCLASS(DSPackScanner, RefCounted);

protected:
	static void _bind_methods();

public:
	// Returns true if p_needle occurs anywhere in p_haystack.
	static bool buffer_contains_bytes(const PackedByteArray &p_haystack, const PackedByteArray &p_needle);

	// Scans p_path in 4 MiB chunks using a single reusable buffer (no per-chunk
	// PackedByteArray allocations). Returns {"ok": bool, "found": bool, "size": int}.
	static Dictionary file_contains_bytes(const String &p_path, const PackedByteArray &p_needle);
};

} // namespace godot

#endif // DS_PACK_SCANNER_H
