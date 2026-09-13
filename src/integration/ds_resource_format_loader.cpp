#include "ds_resource_format_loader.h"
#include "ds_texture_cache.h"
#include "../compression/gdeflate_decompressor.h"
#include "../core/ds_context.h"
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <thread>

namespace godot {

void DSResourceFormatLoader::_bind_methods() {
}

DSResourceFormatLoader::DSResourceFormatLoader() {
}

DSResourceFormatLoader::~DSResourceFormatLoader() {
	std::lock_guard<std::mutex> lock(archive_mutex);
	cached_dspkg.unref();
	cached_archive_path = String();
}

PackedStringArray DSResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray ext;
	ext.append("dstex");
	ext.append("dspkg");
	return ext;
}

bool DSResourceFormatLoader::_recognize_path(const String &p_path, const StringName &p_type) const {
	if (!_handles_type(p_type)) {
		return false;
	}

	String ext = p_path.get_extension().to_lower();
	if (ext == "dstex" || ext == "dspkg") {
		return true;
	}

	// Physical source images belong to the importer; only virtual archive entries
	// should be recognized for their source extensions.
	return !FileAccess::file_exists(p_path) && _exists(p_path);
}

bool DSResourceFormatLoader::_handles_type(const StringName &p_type) const {
	String type_str = p_type;
	if (type_str == "Texture3D" || type_str == "Texture2DArray" || type_str == "TextureLayered" || type_str == "Cubemap" || type_str == "TextureCube") {
		UtilityFunctions::push_error("DSResourceFormatLoader: Unsupported non-2D texture type '" + type_str + "'; only Texture2D resources are supported.");
		return false;
	}
	if (type_str.is_empty() || type_str == "Texture2D" || type_str == "CompressedTexture2D" || type_str == "Resource" || type_str == "Texture" || type_str == "RefCounted" || type_str == "Object") {
		return true;
	}
	return false;
}

String DSResourceFormatLoader::_get_resource_type(const String &p_path) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "dstex") {
		return "Texture2D";
	}
	if (ext == "dspkg") {
		return "Resource";
	}
	if (_is_texture_archive_entry(p_path)) {
		return "Texture2D";
	}
	return "";
}

String DSResourceFormatLoader::_resolve_archive_path(const String &p_logical_path) const {
	if (FileAccess::file_exists(p_logical_path)) {
		return p_logical_path;
	}

	if (p_logical_path != "res://textures.dspkg") {
		return String();
	}

	OS *os = OS::get_singleton();
	if (os) {
		String executable_archive = os->get_executable_path().get_base_dir().path_join("textures.dspkg");
		if (FileAccess::file_exists(executable_archive)) {
			return executable_archive;
		}
	}

	ProjectSettings *project_settings = ProjectSettings::get_singleton();
	if (project_settings) {
		String globalized_archive = project_settings->globalize_path(p_logical_path);
		if (FileAccess::file_exists(globalized_archive)) {
			return globalized_archive;
		}
	}

	return String();
}

Ref<DSPkg> DSResourceFormatLoader::_get_or_open_archive(const String &p_archive_path) const {
	if (p_archive_path.is_empty()) {
		return Ref<DSPkg>();
	}

	std::lock_guard<std::mutex> lock(archive_mutex);
	if (cached_archive_path == p_archive_path && cached_dspkg.is_valid()) {
		return cached_dspkg;
	}

	Ref<DSPkg> archive;
	archive.instantiate();
	if (archive->open(p_archive_path) != OK) {
		return Ref<DSPkg>();
	}

	cached_archive_path = p_archive_path;
	cached_dspkg = archive;
	return cached_dspkg;
}

bool DSResourceFormatLoader::_is_texture_archive_entry(const String &p_path) const {
	Ref<DSPkg> archive = _get_or_open_archive(_resolve_archive_path("res://textures.dspkg"));
	return archive.is_valid() && archive->has_texture(p_path);
}

bool DSResourceFormatLoader::_exists(const String &p_path) const {
	if (FileAccess::file_exists(p_path)) {
		return true;
	}
	return _is_texture_archive_entry(p_path);
}

Variant DSResourceFormatLoader::_load_texture_payload(
		const String &p_file_path,
		uint64_t p_offset,
		uint64_t p_compressed_size,
		uint64_t p_original_size,
		uint32_t p_width,
		uint32_t p_height,
		uint32_t p_pixel_format,
		bool p_is_srgb,
		uint32_t p_mip_count,
		bool p_is_v2,
		const PackedInt64Array &p_mip_offsets,
		const PackedInt32Array &p_mip_sizes,
		const PackedInt32Array &p_mip_uncompressed_sizes,
		const PackedInt32Array &p_mip_row_pitches,
		const String &p_cache_key,
		const String &p_original_cache_key,
		int32_t p_cache_mode,
		bool p_use_sub_threads,
		bool p_as_image_texture) const {
	if (p_width == 0 || p_height == 0 || p_mip_count == 0) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Invalid 2D texture dimensions or mip count for '", p_cache_key, "' (width=", p_width, ", height=", p_height, ", mips=", p_mip_count, ").");
		return Variant();
	}

	if (p_pixel_format >= RenderingDevice::DATA_FORMAT_MAX) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Unsupported texture data format ", p_pixel_format, " for '", p_cache_key, "'.");
		return Variant();
	}

	const int img_fmt = DSTex::rd_format_to_godot_format(p_pixel_format);
	if (img_fmt == Image::FORMAT_MAX || !DSTex::is_format_supported(img_fmt)) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Texture data format ", p_pixel_format, " cannot be represented as a supported 2D image for '", p_cache_key, "'.");
		return Variant();
	}

	if (p_compressed_size > UINT32_MAX || p_original_size > UINT32_MAX) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Payload size exceeds 32-bit limit (compressed=", p_compressed_size, ", original=", p_original_size, ")");
		return Variant();
	}

	DSContext *ds_ctx = DSContext::get_singleton();
	if (ds_ctx && !ds_ctx->is_initialized()) {
		ds_ctx->initialize_ds();
	}

	Ref<Texture2D> result_texture;

	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	bool gpu_active = ds_ctx ? ds_ctx->is_gpu_decompression_enabled() : false;

	// Thread-affinity gate for GPU DirectStorage decompression (C2).
	// Direct GPU decompression requires the render thread because
	// RenderingDevice::get_driver_resource() and free_rid() assert
	// is_on_render_thread() and will fail or spam errors on workers.
	// Off-render-thread (main thread, ResourceLoader worker threads)
	// we fall back cleanly to memory destination + CPU GDeflate + ImageTexture.
	// Alignment: v2 textures carry 256-byte row-pitch / 512-byte placement
	// footprints; GPU path uploads the padded footprints directly while the
	// CPU fallback below unpads rows to natural image data via DSTex::get_mip_layout().
	// ResourceLoader sub-thread loads stay on the CPU path because GPU texture
	// resources are render-thread-affine.
	bool can_attempt_gpu = false;
	if (p_as_image_texture) {
		UtilityFunctions::print_verbose("DSResourceFormatLoader: Texture requested as ImageTexture for '" + p_cache_key + "'; bypassing GPU direct upload.");
	} else if (gpu_active && p_is_v2 && !p_use_sub_threads) {
		if (!rs || !rs->is_on_render_thread()) {
			UtilityFunctions::print_verbose("DSResourceFormatLoader: GPU decompression unavailable off render thread for '" + p_cache_key + "' (thread affinity fallback); using CPU memory decompression.");
		} else if (rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			String driver_name = rs->get_current_rendering_driver_name();
			UtilityFunctions::print_verbose("DSResourceFormatLoader: GPU decompression requires D3D12 driver for '" + p_cache_key + "' (current: '" + driver_name + "'); using CPU memory decompression.");
		} else if (!rd) {
			UtilityFunctions::print_verbose("DSResourceFormatLoader: GPU decompression unavailable (no RenderingDevice) for '" + p_cache_key + "'; using CPU memory decompression.");
		} else if (p_mip_count == 0) {
			UtilityFunctions::print_verbose("DSResourceFormatLoader: GPU decompression requires at least one mip for '" + p_cache_key + "'; using CPU memory decompression.");
		} else {
			can_attempt_gpu = true;
		}
	}

	if (can_attempt_gpu) {
		Ref<RDTextureFormat> rd_fmt;
		rd_fmt.instantiate();
		rd_fmt->set_width(static_cast<int32_t>(p_width));
		rd_fmt->set_height(static_cast<int32_t>(p_height));
		rd_fmt->set_format(static_cast<RenderingDevice::DataFormat>(p_pixel_format));
		rd_fmt->set_mipmaps(static_cast<int32_t>(p_mip_count > 0 ? p_mip_count : 1));
		rd_fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D);
		rd_fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);

		Ref<RDTextureView> rd_view;
		rd_view.instantiate();
		RID tex_rid = rd->texture_create(rd_fmt, rd_view, TypedArray<PackedByteArray>());
		if (tex_rid.is_valid()) {
			uint64_t native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, tex_rid, 0);
			if (native_format != 0) {
				Ref<DSRequest> req;
				req.instantiate();
				req->set_file_path(p_file_path);
				uint64_t start_offset = (p_mip_offsets.size() > 0) ? static_cast<uint64_t>(p_mip_offsets[0]) : p_offset;
				uint64_t total_span = p_compressed_size;
				if (p_mip_offsets.size() >= static_cast<int64_t>(p_mip_count) && p_mip_sizes.size() >= static_cast<int64_t>(p_mip_count) && p_mip_count > 0) {
					uint64_t last_idx = p_mip_count - 1;
					total_span = (static_cast<uint64_t>(p_mip_offsets[last_idx]) + static_cast<uint64_t>(p_mip_sizes[last_idx])) - start_offset;
				}
				req->set_offset(start_offset);
				req->set_size(static_cast<uint32_t>(total_span));
				req->set_uncompressed_size(static_cast<uint32_t>(p_original_size));
				req->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
				req->set_destination_resource(tex_rid);
				req->set_first_subresource(0);
				req->set_subresource_count(p_mip_count);
				req->set_texture_format(static_cast<uint32_t>(native_format));
				req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);

				bool enqueued = ds_ctx->enqueue_request(req);
				if (enqueued) {
					ds_ctx->flush_batch();
				}
				if (enqueued && req->get_status() == DSRequest::STATUS_COMPLETED) {
					Ref<Texture2DRD> tex_rd;
					tex_rd.instantiate();
					tex_rd->set_texture_rd_rid(tex_rid);
					result_texture = tex_rd;
				} else {
					UtilityFunctions::push_warning("DSResourceFormatLoader: GPU texture load failed for ", p_cache_key, "; falling back to CPU decompression.");
				}
			} else {
				UtilityFunctions::push_warning("DSResourceFormatLoader: Could not retrieve the native texture format for ", p_cache_key, "; falling back to CPU decompression.");
			}

			if (result_texture.is_null()) {
				rd->free_rid(tex_rid);
			}
		}
	}

	if (result_texture.is_null()) {
		uint32_t mip_count = p_mip_count;
		auto wait_for_async_requests = [](DSContext *p_context, const Vector<Ref<DSRequest>> &p_requests) -> bool {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (true) {
				p_context->collect_completed_results();
				bool all_complete = true;
				for (int i = 0; i < p_requests.size(); ++i) {
					if (p_requests[i].is_valid() && p_requests[i]->get_status() != DSRequest::STATUS_COMPLETED && p_requests[i]->get_status() != DSRequest::STATUS_FAILED) {
						all_complete = false;
						break;
					}
				}
				if (all_complete) {
					return true;
				}
				if (std::chrono::steady_clock::now() >= deadline) {
					return false;
				}
				std::this_thread::yield();
			}
		};
		auto submit_memory_requests = [&](const Vector<Ref<DSRequest>> &p_requests) -> bool {
			if (p_requests.size() == 0) {
				return true;
			}
			if (p_use_sub_threads) {
				ds_ctx->flush_batch_async();
				return wait_for_async_requests(ds_ctx, p_requests);
			}
			ds_ctx->flush_batch();
			return true;
		};
		auto apply_srgb_fallback = [&](Ref<Image> &p_image) {
			if (!p_is_srgb || p_image.is_null()) {
				return;
			}
			if (p_image->get_format() != Image::FORMAT_RGB8 && p_image->get_format() != Image::FORMAT_RGBA8) {
				p_image->decompress();
				if (p_image->get_format() != Image::FORMAT_RGB8 && p_image->get_format() != Image::FORMAT_RGBA8) {
					p_image->convert(Image::FORMAT_RGBA8);
				}
			}
			if (p_image->get_format() == Image::FORMAT_RGB8 || p_image->get_format() == Image::FORMAT_RGBA8) {
				p_image->srgb_to_linear();
			}
		};
		PackedByteArray natural_texture_data;
		if (p_is_v2 && p_mip_offsets.size() >= static_cast<int64_t>(mip_count) && p_mip_sizes.size() >= static_cast<int64_t>(mip_count)) {
			// Decompress each mip independently and unpad from 256-byte pitch footprint to natural image data
			bool ds_usable = ds_ctx && ds_ctx->is_initialized();
			Vector<Ref<DSRequest>> mip_requests;
			if (ds_usable) {
				mip_requests.resize(mip_count);
				Vector<Ref<DSRequest>> queued_requests;
				for (uint32_t i = 0; i < mip_count; ++i) {
					Ref<DSRequest> req;
					req.instantiate();
					req->set_file_path(p_file_path);
					req->set_offset(static_cast<uint64_t>(p_mip_offsets[i]));
					req->set_size(static_cast<uint32_t>(p_mip_sizes[i]));
					uint32_t uncomp_size = (p_mip_uncompressed_sizes.size() > static_cast<int64_t>(i)) ? static_cast<uint32_t>(p_mip_uncompressed_sizes[i]) : 0;
					req->set_uncompressed_size(uncomp_size);
					req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
					req->get_destination_buffer_ref().resize(static_cast<int64_t>(uncomp_size));
					if (ds_ctx->enqueue_request(req)) {
						mip_requests.write[i] = req;
						queued_requests.push_back(req);
					}
				}
				if (!submit_memory_requests(queued_requests) && p_use_sub_threads) {
					UtilityFunctions::push_warning("DSResourceFormatLoader: Asynchronous DirectStorage memory load timed out for ", p_cache_key, "; using CPU decompression fallback.");
				}
			}

			Ref<GDeflateDecompressor> gdeflate_decompressor;
			gdeflate_decompressor.instantiate();
			Ref<FileAccess> file;
			uint32_t cur_w = p_width;
			uint32_t cur_h = p_height;
			bool all_mips_ok = true;

			for (uint32_t i = 0; i < mip_count; ++i) {
				PackedByteArray mip_decompressed;
				uint32_t expected_footprint_size = (p_mip_uncompressed_sizes.size() > static_cast<int64_t>(i)) ? static_cast<uint32_t>(p_mip_uncompressed_sizes[i]) : 0;

				if (mip_requests.size() > static_cast<int>(i) && mip_requests[i].is_valid() && mip_requests[i]->get_status() == DSRequest::STATUS_COMPLETED) {
					mip_decompressed = mip_requests[i]->get_destination_buffer();
				}

				if (mip_decompressed.size() == 0) {
					if (file.is_null()) {
						file = FileAccess::open(p_file_path, FileAccess::READ);
					}
					if (file.is_valid() && file->is_open()) {
						file->seek(static_cast<uint64_t>(p_mip_offsets[i]));
						PackedByteArray compressed_mip = file->get_buffer(static_cast<int64_t>(p_mip_sizes[i]));
						if (compressed_mip.size() == static_cast<int64_t>(p_mip_sizes[i])) {
							mip_decompressed = gdeflate_decompressor->decompress(compressed_mip, expected_footprint_size);
						}
					}
				}

				if (mip_decompressed.size() == 0) {
					UtilityFunctions::push_error("DSResourceFormatLoader: Failed to decompress mip ", i, " for ", p_cache_key);
					all_mips_ok = false;
					break;
				}

				DSTexMipLayout layout = DSTex::get_mip_layout(cur_w, cur_h, img_fmt);
				if (layout.row_pitch > 0 && layout.source_row_bytes > 0 && mip_decompressed.size() == static_cast<int64_t>(layout.footprint_size)) {
					int64_t natural_mip_size = static_cast<int64_t>(layout.source_size);
					int64_t current_total_size = natural_texture_data.size();
					natural_texture_data.resize(current_total_size + natural_mip_size);
					uint8_t *dst_ptr = natural_texture_data.ptrw() + current_total_size;
					const uint8_t *src_ptr = mip_decompressed.ptr();
					for (uint64_t r = 0; r < layout.row_count; ++r) {
						memcpy(dst_ptr + r * layout.source_row_bytes, src_ptr + r * layout.row_pitch, static_cast<size_t>(layout.source_row_bytes));
					}
				} else {
					natural_texture_data.append_array(mip_decompressed);
				}

				cur_w = std::max<uint32_t>(1, cur_w / 2);
				cur_h = std::max<uint32_t>(1, cur_h / 2);
			}

			if (all_mips_ok && natural_texture_data.size() > 0) {
				Ref<Image> image = Image::create_from_data(p_width, p_height, mip_count > 1, static_cast<Image::Format>(img_fmt), natural_texture_data);
				if (image.is_valid() && !image->is_empty()) {
					apply_srgb_fallback(image);
					result_texture = ImageTexture::create_from_image(image);
				}
			}
		} else {
			// Legacy v1 path (single continuous stream, not footprint padded)
			PackedByteArray decompressed_data;
			bool ds_usable = ds_ctx && ds_ctx->is_initialized();
			if (ds_usable) {
				Ref<DSRequest> req;
				Vector<Ref<DSRequest>> queued_requests;
				req.instantiate();
				req->set_file_path(p_file_path);
				req->set_offset(p_offset);
				req->set_size(static_cast<uint32_t>(p_compressed_size));
				req->set_uncompressed_size(static_cast<uint32_t>(p_original_size));
				req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
				req->get_destination_buffer_ref().resize(static_cast<int64_t>(p_original_size));

				if (ds_ctx->enqueue_request(req)) {
					queued_requests.push_back(req);
					if (!submit_memory_requests(queued_requests) && p_use_sub_threads) {
						UtilityFunctions::push_warning("DSResourceFormatLoader: Asynchronous DirectStorage memory load timed out for ", p_cache_key, "; using CPU decompression fallback.");
					}
				}
				if (req->get_status() == DSRequest::STATUS_COMPLETED) {
					decompressed_data = req->get_destination_buffer();
				}
			}
			if (decompressed_data.size() == 0) {
				Ref<FileAccess> file = FileAccess::open(p_file_path, FileAccess::READ);
				if (file.is_valid() && file->is_open()) {
					file->seek(p_offset);
					PackedByteArray compressed = file->get_buffer(static_cast<int64_t>(p_compressed_size));
					if (compressed.size() == static_cast<int64_t>(p_compressed_size)) {
						Ref<GDeflateDecompressor> gdeflate_decompressor;
						gdeflate_decompressor.instantiate();
						decompressed_data = gdeflate_decompressor->decompress(compressed, static_cast<int>(p_original_size));
					}
				}
			}
			if (decompressed_data.size() > 0) {
				Ref<Image> image = Image::create_from_data(p_width, p_height, mip_count > 1, static_cast<Image::Format>(img_fmt), decompressed_data);
				if (image.is_valid() && !image->is_empty()) {
					apply_srgb_fallback(image);
					result_texture = ImageTexture::create_from_image(image);
				}
			}
		}
	}

	if (result_texture.is_valid()) {
		if (p_cache_mode == ResourceFormatLoader::CACHE_MODE_REUSE || p_cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE) {
			DSTextureCache *cache = DSTextureCache::get_singleton();
			if (cache) {
				cache->store(p_cache_key, result_texture, p_cache_mode);
				if (!p_original_cache_key.is_empty() && p_original_cache_key != p_cache_key) {
					cache->store(p_original_cache_key, result_texture, p_cache_mode);
				}
			}
		}
		return result_texture;
	}

	UtilityFunctions::push_error("DSResourceFormatLoader: Failed to create GPU/Image texture for ", p_cache_key);
	return Variant();
}

Variant DSResourceFormatLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	if (p_cache_mode == ResourceFormatLoader::CACHE_MODE_REUSE) {
		DSTextureCache *cache = DSTextureCache::get_singleton();
		if (cache) {
			Ref<Texture2D> cached = cache->get_cached(p_path, p_cache_mode);
			if (cached.is_valid()) {
				return cached;
			}
			if (!p_original_path.is_empty() && p_original_path != p_path) {
				cached = cache->get_cached(p_original_path, p_cache_mode);
				if (cached.is_valid()) {
					return cached;
				}
			}
		}
	}

	String target_path = p_path;
	bool in_dspkg = false;
	Ref<DSPkg> dspkg_instance;

	String resolved_archive_path = _resolve_archive_path(p_path);
	String ext = p_path.get_extension().to_lower();
	if (ext == "dspkg" && !resolved_archive_path.is_empty()) {
		Ref<DSPkg> pkg = _get_or_open_archive(resolved_archive_path);
		if (pkg.is_valid()) {
			return pkg;
		}
		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to open DSPkg archive: ", p_path);
		return Variant();
	}

	if (!FileAccess::file_exists(p_path)) {
		resolved_archive_path = _resolve_archive_path("res://textures.dspkg");
		if (!resolved_archive_path.is_empty()) {
			dspkg_instance = _get_or_open_archive(resolved_archive_path);
			if (dspkg_instance.is_valid()) {
				if (dspkg_instance->has_texture(p_path)) {
					target_path = p_path;
					in_dspkg = true;
				} else if (!p_original_path.is_empty() && dspkg_instance->has_texture(p_original_path)) {
					target_path = p_original_path;
					in_dspkg = true;
				}
			}
		}
		if (!in_dspkg) {
			return Variant();
		}
	}

	if (in_dspkg && dspkg_instance.is_valid()) {
		Dictionary entry_info = dspkg_instance->get_entry_info(target_path);
		if (entry_info.has("archive_path") && entry_info.has("data_offset")) {
			String archive_path = entry_info["archive_path"];
			uint64_t data_offset = entry_info["data_offset"];
			uint64_t comp_size = entry_info["compressed_size"];
			uint64_t orig_size = entry_info["original_size"];
			uint32_t width = entry_info["width"];
			uint32_t height = entry_info["height"];
			uint32_t pixel_format = entry_info["pixel_format"];
			bool is_srgb = DSTex::is_rd_format_srgb(pixel_format);
			uint32_t mip_count = entry_info["mip_count"];
			bool is_v2 = entry_info.has("has_gpu_metadata") ? (bool)entry_info["has_gpu_metadata"] : false;
			PackedInt64Array mip_offsets = entry_info.has("mip_offsets") ? (PackedInt64Array)entry_info["mip_offsets"] : PackedInt64Array();
			PackedInt32Array mip_sizes = entry_info.has("mip_sizes") ? (PackedInt32Array)entry_info["mip_sizes"] : PackedInt32Array();
			PackedInt32Array mip_uncompressed_sizes = entry_info.has("mip_uncompressed_sizes") ? (PackedInt32Array)entry_info["mip_uncompressed_sizes"] : PackedInt32Array();
			PackedInt32Array mip_row_pitches = entry_info.has("mip_row_pitches") ? (PackedInt32Array)entry_info["mip_row_pitches"] : PackedInt32Array();

			bool as_image_texture = entry_info.has("as_image_texture") ? (bool)entry_info["as_image_texture"] : false;

			return _load_texture_payload(archive_path, data_offset, comp_size, orig_size, width, height, pixel_format, is_srgb, mip_count, is_v2, mip_offsets, mip_sizes, mip_uncompressed_sizes, mip_row_pitches, p_path, p_original_path, p_cache_mode, p_use_sub_threads, as_image_texture);
		}
		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to get entry info from res://textures.dspkg for ", target_path);
		return Variant();
	}

	if (ext == "dstex") {
		Ref<DSTex> dstex;
		dstex.instantiate();
		Dictionary header = dstex->read_header(p_path);
		if (header.is_empty()) {
			return Variant();
		}

		uint32_t width = header["width"];
		uint32_t height = header["height"];
		uint32_t pixel_format = header["pixel_format"];
		bool is_srgb = (header.has("flags") && (static_cast<uint32_t>(header["flags"]) & 1U) != 0) || DSTex::is_rd_format_srgb(pixel_format);
		bool as_image_texture = (header.has("flags") && (static_cast<uint32_t>(header["flags"]) & 2U) != 0) || (header.has("as_image_texture") && (bool)header["as_image_texture"]);
		uint32_t mip_count = header["mip_count"];
		uint64_t original_size = header["original_size"];
		uint64_t compressed_size = header["compressed_size"];
		bool is_v2 = header.has("version") && (int)header["version"] == 2;
		PackedInt64Array mip_offsets = header.has("mip_offsets") ? (PackedInt64Array)header["mip_offsets"] : PackedInt64Array();
		PackedInt32Array mip_sizes = header.has("mip_sizes") ? (PackedInt32Array)header["mip_sizes"] : PackedInt32Array();
		PackedInt32Array mip_uncompressed_sizes = header.has("mip_uncompressed_sizes") ? (PackedInt32Array)header["mip_uncompressed_sizes"] : PackedInt32Array();
		PackedInt32Array mip_row_pitches = header.has("mip_row_pitches") ? (PackedInt32Array)header["mip_row_pitches"] : PackedInt32Array();

		uint64_t data_offset = sizeof(DSTexHeader) + ((is_v2 ? sizeof(DSTexMipEntryV2) : sizeof(DSTexMipEntry)) * mip_count);
		if (is_v2) {
			data_offset = (data_offset + 511) & ~uint64_t(511);
		}

		return _load_texture_payload(p_path, data_offset, compressed_size, original_size, width, height, pixel_format, is_srgb, mip_count, is_v2, mip_offsets, mip_sizes, mip_uncompressed_sizes, mip_row_pitches, p_path, p_original_path, p_cache_mode, p_use_sub_threads, as_image_texture);
	}

	if (ext == "dspkg") {
		Ref<DSPkg> pkg = _get_or_open_archive(p_path);
		if (pkg.is_valid()) {
			return pkg;
		}
		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to open DSPkg archive: ", p_path);
		return Variant();
	}

	return Variant();
}

} // namespace godot
