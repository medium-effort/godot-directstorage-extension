#ifndef GDEFLATE_DECOMPRESSOR_H
#define GDEFLATE_DECOMPRESSOR_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstdint>

namespace godot {

class GDeflateDecompressor : public RefCounted {
	GDCLASS(GDeflateDecompressor, RefCounted);

private:
	int worker_count = 4;

protected:
	static void _bind_methods();

public:
	GDeflateDecompressor();
	~GDeflateDecompressor();

	PackedByteArray decompress(const PackedByteArray &p_compressed, int p_original_size);

	// Pre-allocation predicate shared with the fallback backend: a stream is
	// acceptable only when its TileStream header is well formed, its declared
	// output matches p_original_size, every tile is non-empty, and no tile
	// claims more output than its compressed bytes can expand to. Pure function;
	// never bound to ClassDB.
	static bool validate_stream(const PackedByteArray &p_compressed, int p_original_size);

	void set_worker_count(int p_count);
	int get_worker_count() const;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only observation point: the byte count of the last destination
	// allocation request, recorded immediately before output.resize(). Never
	// bound to ClassDB; native tests call it directly.
	static int64_t _get_last_resize_request_for_testing();
	static void _reset_last_resize_request_for_testing();
#endif
};

} // namespace godot

#endif // GDEFLATE_DECOMPRESSOR_H
