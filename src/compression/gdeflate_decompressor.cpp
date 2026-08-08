#include "gdeflate_decompressor.h"
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "GDeflate.h"

namespace godot {

void GDeflateDecompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("decompress", "compressed", "original_size"), &GDeflateDecompressor::decompress);
	ClassDB::bind_method(D_METHOD("set_worker_count", "count"), &GDeflateDecompressor::set_worker_count);
	ClassDB::bind_method(D_METHOD("get_worker_count"), &GDeflateDecompressor::get_worker_count);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "worker_count"), "set_worker_count", "get_worker_count");
}

GDeflateDecompressor::GDeflateDecompressor() {
	OS *os = OS::get_singleton();
	if (os) {
		int cpus = os->get_processor_count();
		if (cpus < 1) {
			worker_count = 1;
		} else if (cpus > 8) {
			worker_count = 8;
		} else {
			worker_count = cpus;
		}
	} else {
		worker_count = 4;
	}
}

GDeflateDecompressor::~GDeflateDecompressor() {
}

void GDeflateDecompressor::set_worker_count(int p_count) {
	if (p_count < 1) {
		worker_count = 1;
	} else if (p_count > 8) {
		worker_count = 8;
	} else {
		worker_count = p_count;
	}
}

int GDeflateDecompressor::get_worker_count() const {
	return worker_count;
}

PackedByteArray GDeflateDecompressor::decompress(const PackedByteArray &p_compressed, int p_original_size) {
	if (p_compressed.size() == 0 || p_original_size <= 0) {
		return PackedByteArray();
	}

	PackedByteArray output;
	output.resize(p_original_size);

	bool ok = GDeflate::Decompress(
			output.ptrw(),
			static_cast<size_t>(p_original_size),
			p_compressed.ptr(),
			p_compressed.size(),
			static_cast<uint32_t>(worker_count));

	if (!ok) {
		UtilityFunctions::push_error("GDeflateDecompressor: Decompression failed.");
		return PackedByteArray();
	}

	return output;
}

} // namespace godot
