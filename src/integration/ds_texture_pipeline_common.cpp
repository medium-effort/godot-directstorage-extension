#include "ds_texture_pipeline_common.h"

#include "../core/ds_request.h"
#include "../core/ds_settings.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <limits>

namespace godot {
namespace dsgd_texture_pipeline {

uint32_t full_mip_chain_count(uint32_t p_width, uint32_t p_height) {
	uint32_t n = 1;
	while (p_width > 1 || p_height > 1) {
		if (p_width > 1) {
			p_width /= 2;
		}
		if (p_height > 1) {
			p_height /= 2;
		}
		++n;
	}
	return n;
}

bool archive_resolution_blocked_by_editor() {
	Engine *engine = Engine::get_singleton();
	OS *os = OS::get_singleton();
	const bool force_resolution = dsgd_settings::has_setting("force_archive_resolution", "force_archive_resolution") &&
			static_cast<bool>(dsgd_settings::get_setting("force_archive_resolution", "force_archive_resolution"));
	return !force_resolution && ((engine && engine->is_editor_hint()) || (os && os->has_feature("editor")));
}

String resolve_configured_archive_filename() {
	String archive_filename = "textures.dspkg";
	if (dsgd_settings::has_setting("archive_name", "archive_name")) {
		String custom_name = dsgd_settings::get_setting("archive_name", "archive_name");
		if (!custom_name.is_empty()) {
			archive_filename = custom_name;
		}
	}
	return archive_filename;
}

String probe_physical_archive_candidates(const String &p_archive_filename) {
	OS *os = OS::get_singleton();
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String logical = "res://" + p_archive_filename;

	if (os) {
		const String exe_dir = os->get_executable_path().get_base_dir();
		const String exe_custom = exe_dir.path_join(p_archive_filename);
		const String exe_default = exe_dir.path_join("textures.dspkg");
		if (FileAccess::file_exists(exe_custom)) {
			return exe_custom;
		}
		if (FileAccess::file_exists(exe_default)) {
			return exe_default;
		}
	}

	if (ps) {
		const String globalized_custom = ps->globalize_path(logical);
		const String globalized_default = ps->globalize_path("res://textures.dspkg");
		if (FileAccess::file_exists(globalized_custom)) {
			return globalized_custom;
		}
		if (FileAccess::file_exists(globalized_default)) {
			return globalized_default;
		}
	}

	return String();
}

uint64_t resolve_expected_archive_fingerprint() {
	if (dsgd_settings::has_setting("archive_id", "export/archive_id")) {
		return static_cast<uint64_t>(dsgd_settings::get_setting("archive_id", "export/archive_id"));
	}
	return 0;
}

Ref<DSPkg> open_archive_with_pairing(const String &p_archive_path,
		UnauthorizedReportDedup &r_dedup,
		const char *p_log_prefix,
		bool p_log_other_errors) {
	if (p_archive_path.is_empty()) {
		return Ref<DSPkg>();
	}

	const uint64_t expected_fingerprint = resolve_expected_archive_fingerprint();
	Ref<DSPkg> archive;
	archive.instantiate();
	const Error open_err = archive->open(p_archive_path, expected_fingerprint);
	if (open_err != OK) {
		const String prefix = p_log_prefix ? String(p_log_prefix) : String("DirectStorage");
		if (open_err == ERR_UNAUTHORIZED) {
			std::lock_guard<std::mutex> report_lock(r_dedup.mutex);
			const uint64_t path_hash = p_archive_path.hash();
			if (!r_dedup.has_reported || r_dedup.last_hash != path_hash) {
				r_dedup.has_reported = true;
				r_dedup.last_hash = path_hash;
				UtilityFunctions::push_error(prefix, ": Decryption unauthorized or key mismatch for package '", p_archive_path, "'. The archive key is read from res://.dspkg/toc.key inside the game's encrypted PCK, so the PCK and the archive must ship as a pair.");
			}
		} else if (p_log_other_errors) {
			UtilityFunctions::push_error(prefix, ": Failed to open DirectStorage package '", p_archive_path, "': ", UtilityFunctions::error_string(open_err));
		}
		return Ref<DSPkg>();
	}
	return archive;
}

Ref<RDTextureFormat> build_gpu_destination_texture_format(uint32_t p_width,
		uint32_t p_height,
		uint32_t p_pixel_format,
		uint32_t p_mip_count) {
	Ref<RDTextureFormat> rd_fmt;
	rd_fmt.instantiate();
	rd_fmt->set_width(static_cast<int32_t>(p_width));
	rd_fmt->set_height(static_cast<int32_t>(p_height));
	rd_fmt->set_format(static_cast<RenderingDevice::DataFormat>(p_pixel_format));
	rd_fmt->set_mipmaps(static_cast<int32_t>(p_mip_count > 0 ? p_mip_count : 1));
	rd_fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
	rd_fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	// Shareable-list validation (rendering_device.cpp:1552): a non-empty
	// list MUST contain the base format itself. Add base first, then the
	// sRGB counterpart for 3D PBR albedo sampling.
	if (!DSTex::is_rd_format_srgb(static_cast<int>(p_pixel_format))) {
		const int srgb_fmt = DSTex::get_srgb_rd_format(static_cast<int>(p_pixel_format));
		if (srgb_fmt != RenderingDevice::DATA_FORMAT_MAX && srgb_fmt != static_cast<int>(p_pixel_format)) {
			rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(p_pixel_format));
			rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(srgb_fmt));
		}
	}
	return rd_fmt;
}

bool enumerate_gpu_upload_units(const GpuUploadPlanInput &p_input, std::vector<GpuUploadUnit> &r_units) {
	r_units.clear();
	const int64_t mips = static_cast<int64_t>(p_input.mip_count);
	if (mips <= 0 || !p_input.mip_chunk_counts || !p_input.mip_row_pitches || !p_input.mip_uncompressed_sizes ||
			!p_input.chunk_offsets || !p_input.chunk_compressed_sizes || !p_input.chunk_uncompressed_sizes) {
		return false;
	}
	if (p_input.mip_chunk_counts->size() < mips || p_input.mip_row_pitches->size() < mips || p_input.mip_uncompressed_sizes->size() < mips) {
		return false;
	}
	const int64_t table_size = p_input.chunk_offsets->size();
	if (p_input.chunk_compressed_sizes->size() != table_size ||
			p_input.chunk_uncompressed_sizes->size() != table_size) {
		return false;
	}
	const int img_fmt = DSTex::rd_format_to_godot_format(static_cast<int>(p_input.pixel_format));
	int64_t cursor = 0;
	for (int64_t mip = 0; mip < mips; ++mip) {
		const int32_t count = (*p_input.mip_chunk_counts)[mip];
		// Single-chunk mips have no chunk-table entries.
		if (count <= 1) {
			GpuUploadUnit unit;
			unit.mip = static_cast<uint32_t>(mip);
			unit.chunk_index = -1;
			r_units.push_back(unit);
			continue;
		}
		const int32_t pitch = (*p_input.mip_row_pitches)[mip];
		if (pitch <= 0 || cursor + count > table_size) {
			r_units.clear();
			return false;
		}
		// `row` stays in storage rows (block rows for 4x4 formats) because
		// that is the unit of `pitch` and of the byte-sum check below; the
		// unit receives texel rows, which is what a D3D12_BOX speaks.
		const uint32_t mip_height = std::max<uint32_t>(1u, p_input.height >> static_cast<uint32_t>(mip));
		uint32_t row = 0;
		for (int32_t c = 0; c < count; ++c) {
			const int64_t chunk_index = cursor + c;
			const int64_t uncompressed = static_cast<int64_t>((*p_input.chunk_uncompressed_sizes)[chunk_index]);
			const int64_t compressed = static_cast<int64_t>((*p_input.chunk_compressed_sizes)[chunk_index]);
			if (uncompressed <= 0 || uncompressed > UINT32_MAX || compressed <= 0 || compressed > UINT32_MAX) {
				r_units.clear();
				return false;
			}
			if (uncompressed % pitch != 0) {
				r_units.clear();
				return false;
			}
			const uint32_t rows = static_cast<uint32_t>(uncompressed / pitch);
			uint32_t region_top = 0;
			uint32_t region_bottom = 0;
			if (rows == 0 ||
					!DSTex::get_chunk_texel_region(mip_height, row, rows, img_fmt, region_top, region_bottom)) {
				r_units.clear();
				return false;
			}
			GpuUploadUnit unit;
			unit.mip = static_cast<uint32_t>(mip);
			unit.chunk_index = chunk_index;
			unit.region_top = region_top;
			unit.region_bottom = region_bottom;
			r_units.push_back(unit);
			row += rows;
		}
		if (static_cast<uint64_t>(row) * static_cast<uint64_t>(pitch) !=
				static_cast<uint64_t>((*p_input.mip_uncompressed_sizes)[mip])) {
			r_units.clear();
			return false;
		}
		cursor += count;
	}
	if (cursor != table_size) {
		r_units.clear();
		return false;
	}
	return true;
}

size_t count_gpu_upload_requests(const GpuUploadPlanInput &p_input) {
	if (!p_input.chunked) {
		return 1;
	}
	std::vector<GpuUploadUnit> units;
	if (enumerate_gpu_upload_units(p_input, units) && !units.empty()) {
		return units.size();
	}
	return 1;
}

bool build_gpu_upload_requests(const GpuUploadPlanInput &p_input,
		const RID &p_tex_rid,
		uint64_t p_native_format,
		Vector<Ref<DSRequest>> &r_uploads) {
	r_uploads.clear();
	if (!p_input.file_path || p_input.mip_count == 0) {
		return false;
	}

	const bool is_gdeflate = p_input.compression_format == static_cast<uint8_t>(DSCompressionFormat::GDEFLATE);
	auto make_upload = [&](uint64_t p_upload_offset, uint32_t p_size, uint32_t p_uncompressed,
			uint32_t p_first_subresource, uint32_t p_subresource_count) -> Ref<DSRequest> {
		Ref<DSRequest> req;
		req.instantiate();
		req->set_file_path(*p_input.file_path);
		req->set_offset(p_upload_offset);
		req->set_size(p_size);
		req->set_uncompressed_size(p_uncompressed);
		req->set_compression_format(is_gdeflate ? DSRequest::COMPRESSION_GDEFLATE : DSRequest::COMPRESSION_NONE);
		if (p_input.cancellation_tag != 0) {
			req->set_cancellation_tag(p_input.cancellation_tag);
		}
		req->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
		req->set_destination_resource(p_tex_rid);
		req->set_first_subresource(p_first_subresource);
		req->set_subresource_count(p_subresource_count);
		req->set_texture_format(static_cast<uint32_t>(p_native_format));
		if (p_input.debug_label) {
			req->set_debug_label(*p_input.debug_label);
		}
		if (p_input.gdeflate_prevalidated && is_gdeflate) {
			req->set_gdeflate_stream_prevalidated(*p_input.file_path, p_upload_offset, p_uncompressed);
			if (p_subresource_count > 1) {
				req->set_gdeflate_streams_prevalidated(*p_input.file_path, p_upload_offset, p_first_subresource, p_subresource_count);
			}
		}
		r_uploads.push_back(req);
		return req;
	};

	if (p_input.chunked) {
		const int64_t mips = static_cast<int64_t>(p_input.mip_count);
		if (!p_input.mip_offsets || !p_input.mip_sizes || !p_input.mip_uncompressed_sizes ||
				p_input.mip_offsets->size() < mips || p_input.mip_sizes->size() < mips ||
				p_input.mip_uncompressed_sizes->size() < mips) {
			return false;
		}
		std::vector<GpuUploadUnit> units;
		if (!enumerate_gpu_upload_units(p_input, units) || units.empty()) {
			return false;
		}
		for (const GpuUploadUnit &unit : units) {
			if (unit.chunk_index < 0) {
				// A single-chunk mip has no chunk-table entry and targets ONE
				// subresource: keep it on MULTIPLE_SUBRESOURCES_RANGE (no
				// texture row region) so <4x4 BCn tail mips do not submit
				// non-4-aligned D3D12_BOX extents to TEXTURE_REGION.
				const int64_t mip_idx = static_cast<int64_t>(unit.mip);
				const int64_t mip_offset = (*p_input.mip_offsets)[mip_idx];
				const int32_t mip_size = (*p_input.mip_sizes)[mip_idx];
				const int32_t mip_uncomp = (*p_input.mip_uncompressed_sizes)[mip_idx];
				if (mip_offset < 0 || mip_size <= 0 || mip_uncomp <= 0) {
					r_uploads.clear();
					return false;
				}
				make_upload(static_cast<uint64_t>(mip_offset),
						static_cast<uint32_t>(mip_size),
						static_cast<uint32_t>(mip_uncomp),
						unit.mip, 1);
				continue;
			}
			const int64_t chunk_offset = (*p_input.chunk_offsets)[unit.chunk_index];
			if (chunk_offset < 0) {
				r_uploads.clear();
				return false;
			}
			Ref<DSRequest> req = make_upload(static_cast<uint64_t>(chunk_offset),
					static_cast<uint32_t>((*p_input.chunk_compressed_sizes)[unit.chunk_index]),
					static_cast<uint32_t>((*p_input.chunk_uncompressed_sizes)[unit.chunk_index]),
					unit.mip, 1);
			req->set_texture_region(unit.region_top, unit.region_bottom);
		}
		return !r_uploads.is_empty();
	}

	const int64_t mips = static_cast<int64_t>(p_input.mip_count);
	const uint64_t start_offset = (p_input.mip_offsets && p_input.mip_offsets->size() > 0)
			? static_cast<uint64_t>((*p_input.mip_offsets)[0])
			: p_input.data_offset;
	uint64_t total_span = p_input.compressed_size;
	if (p_input.mip_offsets && p_input.mip_sizes &&
			p_input.mip_offsets->size() >= mips && p_input.mip_sizes->size() >= mips) {
		const int64_t last_idx = mips - 1;
		const uint64_t end_offset = static_cast<uint64_t>((*p_input.mip_offsets)[last_idx]) +
				static_cast<uint64_t>((*p_input.mip_sizes)[last_idx]);
		if (end_offset < start_offset) {
			return false;
		}
		total_span = end_offset - start_offset;
	}
	uint64_t total_uncomp = p_input.original_size;
	if (p_input.mip_uncompressed_sizes && p_input.mip_uncompressed_sizes->size() >= mips) {
		total_uncomp = 0;
		for (int64_t i = 0; i < mips; ++i) {
			total_uncomp += static_cast<uint64_t>((*p_input.mip_uncompressed_sizes)[i]);
		}
	}
	if (total_span == 0 || total_span > std::numeric_limits<uint32_t>::max() ||
			total_uncomp == 0 || total_uncomp > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	make_upload(start_offset, static_cast<uint32_t>(total_span),
			static_cast<uint32_t>(total_uncomp), 0, p_input.mip_count);
	return !r_uploads.is_empty();
}

bool is_dummy_renderer() {
	DisplayServer *ds = DisplayServer::get_singleton();
	if (ds != nullptr && ds->get_name().nocasecmp_to("headless") == 0) {
		return true;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	return rs != nullptr && (rs->get_current_rendering_driver_name().nocasecmp_to("dummy") == 0 ||
			rs->get_current_rendering_method().nocasecmp_to("dummy") == 0);
}

std::mutex &dummy_texture_owner_mutex() {
	static std::mutex mutex;
	return mutex;
}

Ref<ImageTexture> create_image_texture_safe(const Ref<Image> &p_image) {
	if (is_dummy_renderer()) {
		std::lock_guard<std::mutex> lock(dummy_texture_owner_mutex());
		return ImageTexture::create_from_image(p_image);
	}
	return ImageTexture::create_from_image(p_image);
}

} // namespace dsgd_texture_pipeline
} // namespace godot
