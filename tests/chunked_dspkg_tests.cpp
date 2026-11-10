// Archive round trip for chunked textures: DSPkg must stage a chunked v3 source
// with its chunk table in a v5 archive, and the archived chunks must decode back
// to the source pixels.
//
// This is the packaging half of the chunking feature: the loader already reads
// chunk tables from DSPkg entry info, but until now the builder rejected chunked
// sources outright.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "compression/gdeflate_decompressor.h"
#include "formats/dspkg_format.h"
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
constexpr int kMips = 2;
constexpr uint64_t kArchiveId = 0x5EED1234ULL;

godot::PackedByteArray make_chain() {
	godot::PackedByteArray data;
	int w = kWidth;
	int h = kHeight;
	for (int i = 0; i < kMips; ++i) {
		godot::Ref<godot::Image> mip = godot::Image::create(w, h, false, godot::Image::FORMAT_RGBA8);
		mip->fill(godot::Color(0.2f * static_cast<float>(i + 1), 0.4f, 0.7f, 1.0f));
		godot::PackedByteArray bytes = mip->get_data();
		for (int64_t b = 0; b < bytes.size(); ++b) {
			bytes[b] = static_cast<uint8_t>((bytes[b] + b * 23 + i * 11) & 0xFF);
		}
		data.append_array(bytes);
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

void track_cleanup(::gdextest::TestContext &ctx, std::initializer_list<const char *> p_paths) {
	for (const char *p : p_paths) {
		const godot::String path(p);
		ctx.add_teardown([path]() {
			godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
			if (ps != nullptr) {
				godot::DirAccess::remove_absolute(ps->globalize_path(path));
			}
		});
	}
}

} // namespace

GDEX_TEST_T(chunked_dspkg, chunked_texture_archive_round_trip, TAG_INTEGRATION) {
	const char *src_path = "user://gdextest_chunked_src.dstex";
	const char *archive_path = "user://gdextest_chunked_archive.dspkg";
	track_cleanup(ctx, { src_path, archive_path });

	const godot::PackedByteArray source = make_chain();
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	GDEX_ASSERT_EQ(dstex->write_to_file(godot::String(src_path), source, kWidth, kHeight,
						   godot::Image::FORMAT_RGBA8, kMips, 1, false, false, 0, 0, 0, 1),
			godot::OK);
	const godot::Dictionary src_header = dstex->read_header(godot::String(src_path));
	GDEX_ASSERT_TRUE(!src_header.is_empty());
	GDEX_ASSERT_TRUE(static_cast<bool>(src_header["chunked"]));
	const godot::PackedInt32Array src_counts = src_header["mip_chunk_counts"];
	const godot::PackedInt32Array src_compressed = src_header["chunk_compressed_sizes"];
	const godot::PackedInt32Array src_uncompressed = src_header["chunk_uncompressed_sizes"];

	godot::Ref<godot::DSPkg> builder;
	builder.instantiate();
	GDEX_ASSERT_EQ(builder->create(godot::String(archive_path), kArchiveId), godot::OK);
	GDEX_ASSERT_EQ(builder->add_texture(godot::String("res://chunked_texture.dstex"),
						   godot::String(src_path), godot::Dictionary()),
			godot::OK);
	GDEX_ASSERT_EQ(builder->finalize(), godot::OK);

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_EQ(reader->open(godot::String(archive_path), kArchiveId), godot::OK);
	const godot::Dictionary info = reader->get_entry_info(godot::String("res://chunked_texture.dstex"));
	GDEX_ASSERT_TRUE(!info.is_empty());
	GDEX_EXPECT_EQ(static_cast<int64_t>(info["format_version"]), static_cast<int64_t>(5));
	GDEX_EXPECT_TRUE(static_cast<bool>(info["chunked"]));
	GDEX_EXPECT_EQ(static_cast<int64_t>(info["mip_count"]), static_cast<int64_t>(kMips));

	const godot::PackedInt32Array counts = info["mip_chunk_counts"];
	const godot::PackedInt64Array offsets = info["chunk_offsets"];
	const godot::PackedInt32Array compressed = info["chunk_compressed_sizes"];
	const godot::PackedInt32Array uncompressed = info["chunk_uncompressed_sizes"];
	GDEX_EXPECT_TRUE(counts == src_counts);
	GDEX_EXPECT_TRUE(compressed == src_compressed);
	GDEX_EXPECT_TRUE(uncompressed == src_uncompressed);
	GDEX_ASSERT_EQ(offsets.size(), compressed.size());

	// Decode the archived chunks and compare with the source chain.
	godot::Ref<godot::FileAccess> archive = godot::FileAccess::open(godot::String(archive_path), godot::FileAccess::READ);
	GDEX_ASSERT_TRUE(archive.is_valid());
	godot::Ref<godot::GDeflateDecompressor> decompressor;
	decompressor.instantiate();
	const godot::PackedInt32Array pitches = info["mip_row_pitches"];
	int64_t cursor = 0;
	int64_t source_offset = 0;
	int w = kWidth;
	int h = kHeight;
	for (int mip = 0; mip < kMips; ++mip) {
		const godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(w, h, godot::Image::FORMAT_RGBA8);
		godot::PackedByteArray footprint;
		footprint.resize(static_cast<int64_t>(layout.footprint_size));
		uint64_t rows_written = 0;
		GDEX_ASSERT_TRUE(pitches[mip] == static_cast<int32_t>(layout.row_pitch));
		for (int32_t c = 0; c < counts[mip]; ++c, ++cursor) {
			archive->seek(static_cast<uint64_t>(offsets[cursor]));
			godot::PackedByteArray chunk = archive->get_buffer(static_cast<int64_t>(compressed[cursor]));
			GDEX_ASSERT_EQ(chunk.size(), static_cast<int64_t>(compressed[cursor]));
			godot::PackedByteArray decoded = decompressor->decompress(chunk, static_cast<uint32_t>(uncompressed[cursor]));
			GDEX_ASSERT_EQ(decoded.size(), static_cast<int64_t>(uncompressed[cursor]));
			GDEX_ASSERT_TRUE(rows_written + static_cast<uint64_t>(uncompressed[cursor]) / layout.row_pitch <= layout.row_count);
			memcpy(footprint.ptrw() + rows_written * layout.row_pitch, decoded.ptr(), decoded.size());
			rows_written += static_cast<uint64_t>(uncompressed[cursor]) / layout.row_pitch;
		}
		GDEX_ASSERT_EQ(rows_written, layout.row_count);
		godot::PackedByteArray natural = godot::DSTex::unpad_footprint_mip(footprint, w, h, godot::Image::FORMAT_RGBA8);
		GDEX_ASSERT_EQ(natural.size(), static_cast<int64_t>(layout.source_size));
		GDEX_EXPECT_TRUE(memcmp(natural.ptr(), source.ptr() + source_offset, static_cast<size_t>(natural.size())) == 0);
		source_offset += natural.size();
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	archive->close();
	GDEX_EXPECT_EQ(source_offset, source.size());
}
