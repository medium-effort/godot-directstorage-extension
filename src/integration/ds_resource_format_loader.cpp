#include "ds_resource_format_loader.h"
#include "ds_texture_cache.h"
#include "../compression/gdeflate_decompressor.h"
#include "../core/ds_context.h"
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

void DSResourceFormatLoader::_bind_methods() {
}

DSResourceFormatLoader::DSResourceFormatLoader() {
}

DSResourceFormatLoader::~DSResourceFormatLoader() {
}

PackedStringArray DSResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray ext;
	ext.append("dstex");
	ext.append("dspkg");
	ext.append("png");
	ext.append("jpg");
	ext.append("jpeg");
	ext.append("webp");
	return ext;
}

bool DSResourceFormatLoader::_recognize_path(const String &p_path, const StringName &p_type) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "dstex" || ext == "dspkg" || ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
		return true;
	}
	return false;
}

bool DSResourceFormatLoader::_handles_type(const StringName &p_type) const {
	String type_str = p_type;
	if (type_str.is_empty() || type_str == "Texture2D" || type_str == "CompressedTexture2D" || type_str == "Resource" || type_str == "Texture" || type_str == "RefCounted" || type_str == "Object") {
		return true;
	}
	return false;
}

String DSResourceFormatLoader::_get_resource_type(const String &p_path) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "dstex" || ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp") {
		return "Texture2D";
	}
	if (ext == "dspkg") {
		return "Resource";
	}
	return "";
}

bool DSResourceFormatLoader::_exists(const String &p_path) const {
	if (FileAccess::file_exists(p_path)) {
		return true;
	}
	if (FileAccess::file_exists("res://textures.dspkg")) {
		Ref<DSPkg> pkg;
		pkg.instantiate();
		if (pkg->open("res://textures.dspkg") == OK && pkg->has_texture(p_path)) {
			return true;
		}
	}
	return false;
}

Variant DSResourceFormatLoader::_load_texture_payload(
		const String &p_file_path,
		uint64_t p_offset,
		uint64_t p_compressed_size,
		uint64_t p_original_size,
		uint32_t p_width,
		uint32_t p_height,
		uint32_t p_pixel_format,
		uint32_t p_mip_count,
		const String &p_cache_key,
		const String &p_original_cache_key,
		int32_t p_cache_mode) const {

	if (p_compressed_size > UINT32_MAX || p_original_size > UINT32_MAX) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Payload size exceeds 32-bit limit (compressed=", p_compressed_size, ", original=", p_original_size, ")");
		return Variant();
	}

	Ref<DSRequest> req;
	req.instantiate();
	req->set_file_path(p_file_path);
	req->set_offset(p_offset);
	req->set_size(static_cast<uint32_t>(p_compressed_size));
	req->set_uncompressed_size(static_cast<uint32_t>(p_original_size));
	req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
	req->get_destination_buffer_ref().resize(static_cast<int64_t>(p_original_size));

	DSContext *ds_ctx = DSContext::get_singleton();
	if (!ds_ctx) {
		UtilityFunctions::push_error("DSResourceFormatLoader: DSContext singleton not found.");
		return Variant();
	}

	if (!ds_ctx->is_initialized()) {
		ds_ctx->initialize_ds();
	}

	uint64_t staging_size = static_cast<uint64_t>(ds_ctx->get_staging_buffer_size_mb()) * 1024ULL * 1024ULL;
	uint64_t request_size = MAX(p_compressed_size, p_original_size);
	bool oversized = staging_size > 0 && request_size > staging_size;
	bool enqueued = !oversized && ds_ctx->enqueue_request(req);
	PackedByteArray decompressed_data;
	if (enqueued) {
		ds_ctx->flush_batch();
	}

	if (!enqueued || req->get_status() != DSRequest::STATUS_COMPLETED) {
		if (oversized || req->get_error_message().find("staging buffer") >= 0) {
			Ref<GDeflateDecompressor> cpu_decompressor;
			cpu_decompressor.instantiate();
			Ref<FileAccess> file = FileAccess::open(p_file_path, FileAccess::READ);
			if (file.is_valid()) {
				file->seek(p_offset);
				PackedByteArray compressed_data = file->get_buffer(static_cast<int64_t>(p_compressed_size));
				PackedByteArray cpu_data = cpu_decompressor->decompress(compressed_data, static_cast<int>(p_original_size));
				if (cpu_data.size() == static_cast<int64_t>(p_original_size)) {
					UtilityFunctions::push_warning("DSResourceFormatLoader: Using CPU GDeflate fallback for '" + p_cache_key +
							"'. Increase Project Settings > DirectStorage > Staging Buffer Size to at least " +
							String::num_uint64((request_size + 1048575ULL) / 1048576ULL) + " MiB to restore DirectStorage acceleration.");
					decompressed_data = cpu_data;
				} else {
					UtilityFunctions::push_error("DSResourceFormatLoader: CPU fallback decompression failed for ", p_cache_key);
					return Variant();
				}
			} else {
				UtilityFunctions::push_error("DSResourceFormatLoader: DirectStorage load failed for ", p_cache_key, ": ", req->get_error_message());
				return Variant();
			}
		} else {
			UtilityFunctions::push_error("DSResourceFormatLoader: DirectStorage load failed for ", p_cache_key, ": ", req->get_error_message());
			return Variant();
		}
	} else {
		decompressed_data = req->get_destination_buffer();
	}
	Ref<Texture2D> result_texture;

	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;

	if (rd) {
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

		TypedArray<PackedByteArray> data_array;
		data_array.append(decompressed_data);

		RID tex_rid = rd->texture_create(rd_fmt, rd_view, data_array);
		if (tex_rid.is_valid()) {
			Ref<Texture2DRD> tex_rd;
			tex_rd.instantiate();
			tex_rd->set_texture_rd_rid(tex_rid);
			result_texture = tex_rd;
		} else {
			UtilityFunctions::push_warning("DSResourceFormatLoader: RenderingDevice texture_create failed for ", p_cache_key, ", falling back to ImageTexture.");
		}
	}

	if (result_texture.is_null()) {
		Image::Format img_fmt = static_cast<Image::Format>(DSTex::rd_format_to_godot_format(p_pixel_format));
		Ref<Image> image = Image::create_from_data(p_width, p_height, p_mip_count > 1, img_fmt, decompressed_data);
		if (image.is_valid() && !image->is_empty()) {
			result_texture = ImageTexture::create_from_image(image);
		}
	}

	if (result_texture.is_valid()) {
		if (p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP) {
			DSTextureCache *cache = DSTextureCache::get_singleton();
			if (cache) {
				cache->store(p_cache_key, result_texture);
				if (!p_original_cache_key.is_empty() && p_original_cache_key != p_cache_key) {
					cache->store(p_original_cache_key, result_texture);
				}
			}
		}
		return result_texture;
	}

	UtilityFunctions::push_error("DSResourceFormatLoader: Failed to create GPU/Image texture for ", p_cache_key);
	return Variant();
}

Variant DSResourceFormatLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	if (p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP) {
		DSTextureCache *cache = DSTextureCache::get_singleton();
		if (cache) {
			Ref<Texture2D> cached = cache->get_cached(p_path);
			if (cached.is_valid()) {
				return cached;
			}
			if (!p_original_path.is_empty() && p_original_path != p_path) {
				cached = cache->get_cached(p_original_path);
				if (cached.is_valid()) {
					return cached;
				}
			}
		}
	}

	String target_path = p_path;
	bool in_dspkg = false;
	Ref<DSPkg> dspkg_instance;

	if (!FileAccess::file_exists(p_path)) {
		if (FileAccess::file_exists("res://textures.dspkg")) {
			dspkg_instance.instantiate();
			if (dspkg_instance->open("res://textures.dspkg") == OK) {
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
			uint32_t mip_count = entry_info["mip_count"];

			return _load_texture_payload(archive_path, data_offset, comp_size, orig_size, width, height, pixel_format, mip_count, p_path, p_original_path, p_cache_mode);
		}
		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to get entry info from res://textures.dspkg for ", target_path);
		return Variant();
	}

	String ext = p_path.get_extension().to_lower();

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
		uint32_t mip_count = header["mip_count"];
		uint64_t original_size = header["original_size"];
		uint64_t compressed_size = header["compressed_size"];

		uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntry) * mip_count);

		return _load_texture_payload(p_path, data_offset, compressed_size, original_size, width, height, pixel_format, mip_count, p_path, p_original_path, p_cache_mode);
	}

	if (ext == "dspkg") {
		Ref<DSPkg> pkg;
		pkg.instantiate();
		Error err = pkg->open(p_path);
		if (err == OK) {
			return pkg;
		}
		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to open DSPkg archive: ", p_path);
		return Variant();
	}

	return Variant();
}

} // namespace godot
