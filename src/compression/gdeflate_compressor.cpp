// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "gdeflate_compressor.h"
#include "gdeflate_decompressor.h"
#include "gdeflate_parallel.h"
#include "../core/ds_cpu_budget.h"
#include "../core/ds_error_handler.h"
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include "GDeflate.h"

namespace godot {

namespace {

void report_compression_error(const String &p_message) {
	UtilityFunctions::push_error(p_message);
	DSErrorHandler *error_handler = DSErrorHandler::get_singleton();
	if (error_handler) {
		error_handler->report_error(p_message);
	}
}

bool verify_round_trip(const PackedByteArray &p_data) {
	GDeflateCompressor compressor;
	PackedByteArray compressed = compressor.compress(p_data);
	if (p_data.size() == 0) {
		return compressed.size() == 0;
	}
	if (compressed.size() == 0) {
		return false;
	}

	GDeflateDecompressor decompressor;
	PackedByteArray decompressed = decompressor.decompress(compressed, p_data.size());
	return decompressed.size() == p_data.size() && decompressed == p_data;
}

Dictionary compress_with_stats(const PackedByteArray &p_data) {
	GDeflateCompressor compressor;
	PackedByteArray compressed = compressor.compress(p_data);
	Dictionary result;
	result["compressed_data"] = compressed;
	result["original_size"] = p_data.size();
	result["compressed_size"] = compressed.size();
	result["compression_ratio"] = p_data.size() > 0 ? static_cast<double>(compressed.size()) / p_data.size() : 0.0;
	result["success"] = p_data.size() == 0 || compressed.size() > 0;
	return result;
}

} // namespace

void GDeflateCompressor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("compress", "input", "level"), &GDeflateCompressor::compress, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("compress_bound", "input_size"), &GDeflateCompressor::compress_bound);
	ClassDB::bind_method(D_METHOD("set_compression_level", "level"), &GDeflateCompressor::set_compression_level);
	ClassDB::bind_method(D_METHOD("get_compression_level"), &GDeflateCompressor::get_compression_level);
	ClassDB::bind_static_method("GDeflateCompressor", D_METHOD("verify_round_trip", "data"), &verify_round_trip);
	ClassDB::bind_static_method("GDeflateCompressor", D_METHOD("compress_with_stats", "data"), &compress_with_stats);

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
			0,
			&ds_gdeflate_parallel_for,
			static_cast<uint32_t>(ds_get_max_cpu_threads()));

	if (!ok) {
		report_compression_error("GDeflateCompressor: Compression failed.");
		return PackedByteArray();
	}

	output.resize(static_cast<int64_t>(out_size));
	return output;
}

} // namespace godot
