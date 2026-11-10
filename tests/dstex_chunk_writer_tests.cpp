// Chunked-v3 writer: the format must be able to emit a file where no single
// request carries a whole mip, and the chunks must decode back to the exact
// source pixels.
//
// These tests exercise the writer through its public API and validate the
// result with the same parser the runtime uses (read_header -> ds_parse_v3_chunks),
// plus a CPU GDeflate decode of every chunk. They do not need a RenderingDevice,
// so they run in the headless suite.
//
// Note on the chunk arrays: ds_parse_v3_chunks only records entries for mips
// with chunk_count > 1. A single-chunk mip lives in the mip table only, so both
// the layout checks and the decoder must branch on the mip's chunk count.
//
// Invariants pinned here:
//  - chunk_size_kb == 0 keeps the historic single-stream-per-mip v2 layout;
//  - a chunk table sets CAP_CHUNKED, the mip sums match the chunks, the first
//    chunk starts at the mip offset (512-aligned) and later chunks are 8-aligned;
//  - block formats keep chunk rows on 4-row boundaries so the runtime can
//    express a chunk as a legal TEXTURE_REGION box;
//  - each chunk is an independent GDeflate stream of exactly its declared
//    uncompressed size, and the decoded mips equal the source data.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "compression/gdeflate_decompressor.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstring>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 64;
constexpr int kMips = 3;

// Natural (unpadded) mip chain. Byte patterns vary per row so a chunk landing
// at the wrong row offset cannot accidentally compare equal. Block formats are
// synthesized at their exact byte size: the writer treats the payload as opaque
// rows and the reader only unpads, so a real BC encoder is not needed.
godot::PackedByteArray make_chain(int p_format, int p_mips) {
	godot::PackedByteArray data;
	int w = kWidth;
	int h = kHeight;
	for (int i = 0; i < p_mips; ++i) {
		godot::PackedByteArray bytes;
		if (p_format == godot::Image::FORMAT_RGBA8) {
			godot::Ref<godot::Image> mip = godot::Image::create(w, h, false, godot::Image::FORMAT_RGBA8);
			mip->fill(godot::Color(0.1f * static_cast<float>(i + 1), 0.3f, 0.6f, 1.0f));
			bytes = mip->get_data();
		} else {
			// DXT5 == 16 bytes per 4x4 block.
			const int64_t blocks = static_cast<int64_t>(std::max(1, (w + 3) / 4)) *
					static_cast<int64_t>(std::max(1, (h + 3) / 4));
			bytes.resize(blocks * 16);
		}
		for (int64_t b = 0; b < bytes.size(); ++b) {
			bytes[b] = static_cast<uint8_t>((b * 31 + i * 7 + (b >> 8) * 13) & 0xFF);
		}
		data.append_array(bytes);
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

bool write_chain(const char *p_path, const godot::PackedByteArray &p_data, int p_format, int p_chunk_kb) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex->write_to_file(godot::String(p_path), p_data, kWidth, kHeight, p_format, kMips, 1,
			false, false, 0, 0, 0, p_chunk_kb) == godot::OK;
}

void track_cleanup(::gdextest::TestContext &ctx, const char *p_path) {
	const godot::String path(p_path);
	ctx.add_teardown([path]() {
		godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
		if (ps != nullptr) {
			godot::DirAccess::remove_absolute(ps->globalize_path(path));
		}
	});
}

// Decodes every mip from its chunks (or its single stream) and compares the
// unpadded result with the natural source chain.
bool decodes_to_source(const char *p_path, const godot::PackedByteArray &p_source, int p_format) {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(p_path));
	if (header.is_empty()) {
		return false;
	}
	const godot::PackedInt32Array mip_sizes = header["mip_sizes"];
	const godot::PackedInt64Array mip_offsets = header["mip_offsets"];
	const godot::PackedInt32Array chunk_counts = header["mip_chunk_counts"];
	const godot::PackedInt64Array chunk_offsets = header["chunk_offsets"];
	const godot::PackedInt32Array chunk_compressed = header["chunk_compressed_sizes"];
	const godot::PackedInt32Array chunk_uncompressed = header["chunk_uncompressed_sizes"];

	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(godot::String(p_path), godot::FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();

	int64_t chunk_cursor = 0;
	int64_t source_offset = 0;
	int w = kWidth;
	int h = kHeight;
	for (int mip = 0; mip < kMips; ++mip) {
		const godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(w, h, p_format);
		const int chunk_count = chunk_counts.size() > mip ? chunk_counts[mip] : 1;
		godot::PackedByteArray footprint;
		footprint.resize(static_cast<int64_t>(layout.footprint_size));
		uint64_t rows_written = 0;
		for (int c = 0; c < chunk_count; ++c) {
			uint64_t offset = 0;
			uint32_t compressed_size = 0;
			uint32_t uncompressed_size = 0;
			if (chunk_count > 1) {
				offset = static_cast<uint64_t>(chunk_offsets[chunk_cursor]);
				compressed_size = static_cast<uint32_t>(chunk_compressed[chunk_cursor]);
				uncompressed_size = static_cast<uint32_t>(chunk_uncompressed[chunk_cursor]);
				++chunk_cursor;
			} else {
				offset = static_cast<uint64_t>(mip_offsets[mip]);
				compressed_size = static_cast<uint32_t>(mip_sizes[mip]);
				uncompressed_size = static_cast<uint32_t>(layout.footprint_size);
			}
			if (uncompressed_size % layout.row_pitch != 0) {
				return false;
			}
			file->seek(offset);
			godot::PackedByteArray compressed = file->get_buffer(static_cast<int64_t>(compressed_size));
			if (compressed.size() != static_cast<int64_t>(compressed_size)) {
				return false;
			}
			godot::PackedByteArray decoded = decompressor->decompress(compressed, uncompressed_size);
			if (decoded.size() != static_cast<int64_t>(uncompressed_size)) {
				return false;
			}
			const uint64_t rows = uncompressed_size / layout.row_pitch;
			if (rows_written + rows > layout.row_count) {
				return false;
			}
			memcpy(footprint.ptrw() + rows_written * layout.row_pitch, decoded.ptr(), decoded.size());
			rows_written += rows;
		}
		if (rows_written != layout.row_count) {
			return false;
		}
		godot::PackedByteArray natural = godot::DSTex::unpad_footprint_mip(footprint, w, h, p_format);
		if (natural.size() != static_cast<int64_t>(layout.source_size) ||
				source_offset + natural.size() > p_source.size()) {
			return false;
		}
		if (memcmp(natural.ptr(), p_source.ptr() + source_offset, static_cast<size_t>(natural.size())) != 0) {
			return false;
		}
		source_offset += natural.size();
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	file->close();
	return source_offset == p_source.size();
}

} // namespace

GDEX_TEST(dstex_chunks, rgba8_chunk_layout_and_round_trip) {
	const char *path = "user://gdextest_chunk_rgba8.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain(godot::Image::FORMAT_RGBA8, kMips);
	GDEX_ASSERT_TRUE(write_chain(path, source, godot::Image::FORMAT_RGBA8, 4));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["version"]), static_cast<int64_t>(3));
	GDEX_EXPECT_TRUE(static_cast<bool>(header["chunked"]));

	// 64 rows / 16 rows-per-chunk at 4 KiB and a 256-byte pitch.
	const godot::PackedInt32Array counts = header["mip_chunk_counts"];
	GDEX_ASSERT_EQ(counts.size(), static_cast<int64_t>(kMips));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[0]), static_cast<int64_t>(4));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[1]), static_cast<int64_t>(2));
	GDEX_EXPECT_EQ(static_cast<int64_t>(counts[2]), static_cast<int64_t>(1));

	// Sums, alignment and the per-request staging floor. The chunk arrays only
	// carry mips with chunk_count > 1.
	const godot::PackedInt64Array chunk_offsets = header["chunk_offsets"];
	const godot::PackedInt32Array chunk_compressed = header["chunk_compressed_sizes"];
	const godot::PackedInt32Array chunk_uncompressed = header["chunk_uncompressed_sizes"];
	const godot::PackedInt64Array mip_offsets = header["mip_offsets"];
	const godot::PackedInt32Array mip_sizes = header["mip_sizes"];
	const godot::PackedInt32Array mip_uncompressed = header["mip_uncompressed_sizes"];
	int64_t cursor = 0;
	int64_t max_request = 0;
	for (int64_t mip = 0; mip < counts.size(); ++mip) {
		if (counts[mip] <= 1) {
			max_request = std::max<int64_t>(max_request,
					std::max<int64_t>(mip_sizes[mip], mip_uncompressed[mip]));
			continue;
		}
		int64_t compressed_sum = 0;
		int64_t uncompressed_sum = 0;
		for (int64_t c = 0; c < counts[mip]; ++c) {
			const int64_t offset = static_cast<int64_t>(chunk_offsets[cursor]);
			const int64_t compressed = static_cast<int64_t>(chunk_compressed[cursor]);
			const int64_t uncompressed = static_cast<int64_t>(chunk_uncompressed[cursor]);
			GDEX_EXPECT_EQ(offset % (c == 0 ? 512 : 8), static_cast<int64_t>(0));
			if (c == 0) {
				GDEX_EXPECT_EQ(offset, static_cast<int64_t>(mip_offsets[mip]));
			}
			compressed_sum += compressed;
			uncompressed_sum += uncompressed;
			max_request = std::max(max_request, std::max(compressed, uncompressed));
			++cursor;
		}
		GDEX_EXPECT_EQ(compressed_sum, static_cast<int64_t>(mip_sizes[mip]));
		GDEX_EXPECT_EQ(uncompressed_sum, static_cast<int64_t>(mip_uncompressed[mip]));
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["min_staging_bytes"]), max_request);
	GDEX_ASSERT_TRUE(decodes_to_source(path, source, godot::Image::FORMAT_RGBA8));
}

GDEX_TEST(dstex_chunks, bc3_chunk_rows_stay_block_aligned) {
	const char *path = "user://gdextest_chunk_bc3.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain(godot::Image::FORMAT_DXT5, kMips);
	GDEX_ASSERT_TRUE(write_chain(path, source, godot::Image::FORMAT_DXT5, 1));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_TRUE(static_cast<bool>(header["chunked"]));

	const godot::PackedInt32Array counts = header["mip_chunk_counts"];
	const godot::PackedInt32Array chunk_uncompressed = header["chunk_uncompressed_sizes"];
	const godot::PackedInt32Array mip_uncompressed = header["mip_uncompressed_sizes"];
	const godot::PackedInt32Array mip_pitches = header["mip_row_pitches"];
	int64_t cursor = 0;
	for (int64_t mip = 0; mip < counts.size(); ++mip) {
		const int64_t pitch = static_cast<int64_t>(mip_pitches[mip]);
		GDEX_ASSERT_TRUE(pitch > 0);
		GDEX_EXPECT_EQ(static_cast<int64_t>(mip_uncompressed[mip]) % pitch, static_cast<int64_t>(0));
		if (counts[mip] <= 1) {
			continue;
		}
		for (int64_t c = 0; c < counts[mip]; ++c) {
			const int64_t uncompressed = static_cast<int64_t>(chunk_uncompressed[cursor]);
			GDEX_EXPECT_EQ(uncompressed % pitch, static_cast<int64_t>(0));
			const int64_t rows = uncompressed / pitch;
			// Every chunk but the last of a mip must sit on a 4-row block boundary.
			if (c + 1 < counts[mip]) {
				GDEX_EXPECT_EQ(rows % 4, static_cast<int64_t>(0));
			}
			++cursor;
		}
	}
	GDEX_ASSERT_TRUE(decodes_to_source(path, source, godot::Image::FORMAT_DXT5));
}

GDEX_TEST(dstex_chunks, chunking_off_keeps_v2_single_streams) {
	const char *path = "user://gdextest_chunk_off.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_chain(godot::Image::FORMAT_RGBA8, kMips);
	GDEX_ASSERT_TRUE(write_chain(path, source, godot::Image::FORMAT_RGBA8, 0));

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(header["version"]), static_cast<int64_t>(2));
	GDEX_EXPECT_FALSE(static_cast<bool>(header["chunked"]));
	GDEX_ASSERT_TRUE(decodes_to_source(path, source, godot::Image::FORMAT_RGBA8));
}
