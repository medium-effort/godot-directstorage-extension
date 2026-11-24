#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_request.h"
#include "formats/dstex_format.h"
#include "integration/ds_texture_pipeline_common.h"

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <vector>

namespace {

GDEX_TEST_T(texture_pipeline_common, full_mip_chain_count_cases, TAG_UNIT) {
	(void)ctx;
	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::full_mip_chain_count(1, 1), 1u);
	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::full_mip_chain_count(4, 4), 3u);
	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::full_mip_chain_count(256, 256), 9u);
	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::full_mip_chain_count(256, 64), 9u);
	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::full_mip_chain_count(300, 150), 9u);
}

GDEX_TEST_T(texture_pipeline_common, enumerate_gpu_upload_units_valid_mixed_mips, TAG_UNIT) {
	(void)ctx;
	godot::PackedInt64Array mip_offsets;
	mip_offsets.push_back(1024);
	mip_offsets.push_back(2048);

	godot::PackedInt32Array mip_sizes;
	mip_sizes.push_back(512);
	mip_sizes.push_back(128);

	// Mip 0: 8x8 RGBA8 -> 8 rows * 256 pitch = 2048 bytes, split into two 4-row chunks (1024 + 1024).
	// Mip 1: 4x4 RGBA8 -> single-chunk (count == 1), no chunk-table entry.
	godot::PackedInt32Array mip_uncompressed_sizes;
	mip_uncompressed_sizes.push_back(2048);
	mip_uncompressed_sizes.push_back(1024);

	godot::PackedInt32Array mip_row_pitches;
	mip_row_pitches.push_back(256);
	mip_row_pitches.push_back(256);

	godot::PackedInt32Array mip_chunk_counts;
	mip_chunk_counts.push_back(2);
	mip_chunk_counts.push_back(1);

	godot::PackedInt64Array chunk_offsets;
	chunk_offsets.push_back(1024);
	chunk_offsets.push_back(1280);

	godot::PackedInt32Array chunk_compressed_sizes;
	chunk_compressed_sizes.push_back(256);
	chunk_compressed_sizes.push_back(256);

	godot::PackedInt32Array chunk_uncompressed_sizes;
	chunk_uncompressed_sizes.push_back(1024);
	chunk_uncompressed_sizes.push_back(1024);

	godot::dsgd_texture_pipeline::GpuUploadPlanInput input;
	input.width = 8;
	input.height = 8;
	input.pixel_format = static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	input.mip_count = 2;
	input.chunked = true;
	input.mip_offsets = &mip_offsets;
	input.mip_sizes = &mip_sizes;
	input.mip_uncompressed_sizes = &mip_uncompressed_sizes;
	input.mip_row_pitches = &mip_row_pitches;
	input.mip_chunk_counts = &mip_chunk_counts;
	input.chunk_offsets = &chunk_offsets;
	input.chunk_compressed_sizes = &chunk_compressed_sizes;
	input.chunk_uncompressed_sizes = &chunk_uncompressed_sizes;

	std::vector<godot::dsgd_texture_pipeline::GpuUploadUnit> units;
	GDEX_ASSERT_TRUE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(input, units));
	GDEX_ASSERT_EQ(units.size(), static_cast<size_t>(3));

	GDEX_EXPECT_EQ(units[0].mip, 0u);
	GDEX_EXPECT_EQ(units[0].chunk_index, static_cast<int64_t>(0));
	GDEX_EXPECT_EQ(units[0].region_top, 0u);
	GDEX_EXPECT_EQ(units[0].region_bottom, 4u);

	GDEX_EXPECT_EQ(units[1].mip, 0u);
	GDEX_EXPECT_EQ(units[1].chunk_index, static_cast<int64_t>(1));
	GDEX_EXPECT_EQ(units[1].region_top, 4u);
	GDEX_EXPECT_EQ(units[1].region_bottom, 8u);

	GDEX_EXPECT_EQ(units[2].mip, 1u);
	GDEX_EXPECT_EQ(units[2].chunk_index, static_cast<int64_t>(-1));

	GDEX_EXPECT_EQ(godot::dsgd_texture_pipeline::count_gpu_upload_requests(input), static_cast<size_t>(3));
}

GDEX_TEST_T(texture_pipeline_common, enumerate_gpu_upload_units_rejects_malformed_tables, TAG_UNIT) {
	(void)ctx;
	godot::PackedInt32Array mip_uncompressed_sizes;
	mip_uncompressed_sizes.push_back(2048);
	godot::PackedInt32Array mip_row_pitches;
	mip_row_pitches.push_back(256);
	godot::PackedInt32Array mip_chunk_counts;
	mip_chunk_counts.push_back(2);

	godot::PackedInt64Array chunk_offsets;
	chunk_offsets.push_back(100);
	chunk_offsets.push_back(200);
	godot::PackedInt32Array chunk_compressed_sizes;
	chunk_compressed_sizes.push_back(50);
	chunk_compressed_sizes.push_back(50);
	godot::PackedInt32Array chunk_uncompressed_sizes;
	chunk_uncompressed_sizes.push_back(1024);
	chunk_uncompressed_sizes.push_back(1024);

	godot::dsgd_texture_pipeline::GpuUploadPlanInput base;
	base.width = 8;
	base.height = 8;
	base.pixel_format = static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	base.mip_count = 1;
	base.chunked = true;
	base.mip_uncompressed_sizes = &mip_uncompressed_sizes;
	base.mip_row_pitches = &mip_row_pitches;
	base.mip_chunk_counts = &mip_chunk_counts;
	base.chunk_offsets = &chunk_offsets;
	base.chunk_compressed_sizes = &chunk_compressed_sizes;
	base.chunk_uncompressed_sizes = &chunk_uncompressed_sizes;

	std::vector<godot::dsgd_texture_pipeline::GpuUploadUnit> units;
	GDEX_ASSERT_TRUE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));

	// 1. Zero pitch
	mip_row_pitches.set(0, 0);
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));
	GDEX_EXPECT_TRUE(units.empty());
	mip_row_pitches.set(0, 256);

	// 2. Non-pitch-multiple chunk size
	chunk_uncompressed_sizes.set(0, 1000);
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));
	chunk_uncompressed_sizes.set(0, 1024);

	// 3. Mip uncompressed byte-sum mismatch
	mip_uncompressed_sizes.set(0, 4096);
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));
	mip_uncompressed_sizes.set(0, 2048);

	// 4. Trailing extra chunk entry (cursor != table_size)
	chunk_offsets.push_back(300);
	chunk_compressed_sizes.push_back(50);
	chunk_uncompressed_sizes.push_back(1024);
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));

	// 5. Mismatched chunk array lengths
	chunk_offsets.resize(2);
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::enumerate_gpu_upload_units(base, units));
}

GDEX_TEST_T(texture_pipeline_common, build_gpu_upload_requests_plain_and_chunked, TAG_UNIT) {
	(void)ctx;
	const godot::String file_path("res://fixture.dstex");
	const godot::String label("res://fixture.png");
	const godot::RID dummy_rid;

	// Plain (non-chunked) v2 layout
	godot::PackedInt64Array mip_offsets;
	mip_offsets.push_back(128);
	mip_offsets.push_back(384);
	godot::PackedInt32Array mip_sizes;
	mip_sizes.push_back(256);
	mip_sizes.push_back(64);
	godot::PackedInt32Array mip_uncomp;
	mip_uncomp.push_back(1024);
	mip_uncomp.push_back(256);

	godot::dsgd_texture_pipeline::GpuUploadPlanInput plain;
	plain.file_path = &file_path;
	plain.debug_label = &label;
	plain.width = 8;
	plain.height = 8;
	plain.pixel_format = static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	plain.mip_count = 2;
	plain.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::GDEFLATE);
	plain.chunked = false;
	plain.cancellation_tag = 42;
	plain.mip_offsets = &mip_offsets;
	plain.mip_sizes = &mip_sizes;
	plain.mip_uncompressed_sizes = &mip_uncomp;

	godot::Vector<godot::Ref<godot::DSRequest>> uploads;
	GDEX_ASSERT_TRUE(godot::dsgd_texture_pipeline::build_gpu_upload_requests(plain, dummy_rid, 28, uploads));
	GDEX_ASSERT_EQ(uploads.size(), 1);
	GDEX_EXPECT_EQ(uploads[0]->get_offset(), static_cast<uint64_t>(128));
	GDEX_EXPECT_EQ(uploads[0]->get_size(), 320u); // (384 + 64) - 128
	GDEX_EXPECT_EQ(uploads[0]->get_uncompressed_size(), 1280u);
	GDEX_EXPECT_EQ(uploads[0]->get_first_subresource(), 0u);
	GDEX_EXPECT_EQ(uploads[0]->get_subresource_count(), 2u);
	GDEX_EXPECT_EQ(uploads[0]->get_cancellation_tag(), static_cast<uint64_t>(42));
	GDEX_EXPECT_EQ(uploads[0]->get_texture_format(), 28u);

	// Zero-span non-chunked fallback must fail
	plain.mip_offsets = nullptr;
	plain.mip_sizes = nullptr;
	plain.mip_uncompressed_sizes = nullptr;
	plain.compressed_size = 0;
	plain.original_size = 0;
	GDEX_EXPECT_FALSE(godot::dsgd_texture_pipeline::build_gpu_upload_requests(plain, dummy_rid, 28, uploads));
}

GDEX_TEST_T(texture_pipeline_common, build_gpu_destination_texture_format_srgb_shareable, TAG_UNIT) {
	(void)ctx;
	godot::Ref<godot::RDTextureFormat> rgba8 = godot::dsgd_texture_pipeline::build_gpu_destination_texture_format(
			64, 64, static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM), 4);
	GDEX_ASSERT_TRUE(rgba8.is_valid());
	GDEX_EXPECT_EQ(rgba8->get_width(), 64u);
	GDEX_EXPECT_EQ(rgba8->get_height(), 64u);
	GDEX_EXPECT_EQ(rgba8->get_mipmaps(), 4u);
	GDEX_EXPECT_EQ(static_cast<int64_t>(rgba8->get_format()),
			static_cast<int64_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM));
	GDEX_EXPECT_EQ(static_cast<int64_t>(rgba8->get_texture_type()),
			static_cast<int64_t>(godot::RenderingDevice::TEXTURE_TYPE_2D));

	godot::Ref<godot::RDTextureFormat> srgb = godot::dsgd_texture_pipeline::build_gpu_destination_texture_format(
			64, 64, static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB), 0);
	GDEX_ASSERT_TRUE(srgb.is_valid());
	GDEX_EXPECT_EQ(srgb->get_mipmaps(), 1u);
	GDEX_EXPECT_EQ(static_cast<int64_t>(srgb->get_format()),
			static_cast<int64_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_SRGB));

	godot::Ref<godot::RDTextureFormat> bc5 = godot::dsgd_texture_pipeline::build_gpu_destination_texture_format(
			64, 64, static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK), 1);
	GDEX_ASSERT_TRUE(bc5.is_valid());
	GDEX_EXPECT_EQ(static_cast<int64_t>(bc5->get_format()),
			static_cast<int64_t>(godot::RenderingDevice::DATA_FORMAT_BC5_UNORM_BLOCK));
}

} // namespace
