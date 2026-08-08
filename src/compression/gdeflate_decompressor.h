#ifndef GDEFLATE_DECOMPRESSOR_H
#define GDEFLATE_DECOMPRESSOR_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

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

	void set_worker_count(int p_count);
	int get_worker_count() const;
};

} // namespace godot

#endif // GDEFLATE_DECOMPRESSOR_H
