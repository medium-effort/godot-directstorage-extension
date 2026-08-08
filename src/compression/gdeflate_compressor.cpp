#include "gdeflate_compressor.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include "GDeflate.h"

namespace godot {

void GDeflateCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("compress", "input", "level"), &GDeflateCompressor::compress, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("compress_bound", "input_size"), &GDeflateCompressor::compress_bound);
	ClassDB::bind_method(D_METHOD("set_compression_level", "level"), &GDeflateCompressor::set_compression_level);
	ClassDB::bind_method(D_METHOD("get_compression_level"), &GDeflateCompressor::get_compression_level);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "compression_level"), "set_compression_level", "get_compression_level");
}

GDeflateCompressor::GDeflateCompressor() {
	compression_level = 1;
}

GDeflateCompressor::~GDeflateCompressor() {
}

void GDeflateCompressor::set_compression_level(int p_level) {
	if (p_level < 1) {
		compression_level = 1;
	} else if (p_level > 12) {
		compression_level = 12;
	} else {
		compression_level = p_level;
	}
}

int GDeflateCompressor::get_compression_level() const {
	return compression_level;
}

int GDeflateCompressor::compress_bound(int p_input_size) const {
	if (p_input_size <= 0) {
		return 0;
	}
	return static_cast<int>(GDeflate::CompressBound(static_cast<size_t>(p_input_size)));
}

PackedByteArray GDeflateCompressor::compress(const PackedByteArray &p_input, int p_level) {
	if (p_input.size() == 0) {
		return PackedByteArray();
	}

	int level = p_level;
	if (level < 1 || level > 12) {
		level = compression_level;
	}

	size_t bound = GDeflate::CompressBound(p_input.size());
	PackedByteArray output;
	output.resize(static_cast<int64_t>(bound));

	size_t out_size = bound;
	bool ok = GDeflate::Compress(
			output.ptrw(),
			&out_size,
			p_input.ptr(),
			p_input.size(),
			static_cast<uint32_t>(level),
			0);

	if (!ok) {
		UtilityFunctions::push_error("GDeflateCompressor: Compression failed.");
		return PackedByteArray();
	}

	output.resize(static_cast<int64_t>(out_size));
	return output;
}

} // namespace godot
