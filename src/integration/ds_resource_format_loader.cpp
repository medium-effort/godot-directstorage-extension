#include "ds_resource_format_loader.h"
#include "ds_texture_cache.h"
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
	return ext;
}

bool DSResourceFormatLoader::_recognize_path(const String &p_path, const StringName &p_type) const {
	String ext = p_path.get_extension().to_lower();
	if (ext == "dstex" || ext == "dspkg") {
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
	if (ext == "dstex") {
		return "Texture2D";
	}
	if (ext == "dspkg") {
		return "Resource";
	}
	return "";
}

bool DSResourceFormatLoader::_exists(const String &p_path) const {
	return FileAccess::file_exists(p_path);
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

	if (!FileAccess::file_exists(p_path)) {
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

		int width = header["width"];
		int height = header["height"];
		int pixel_format = header["pixel_format"];
		int mip_count = header["mip_count"];
		uint64_t original_size = header["original_size"];
		uint64_t compressed_size = header["compressed_size"];

		uint64_t data_offset = sizeof(DSTexHeader) + (sizeof(DSTexMipEntry) * mip_count);

		Ref<DSRequest> req;
		req.instantiate();
		req->set_file_path(p_path);
		req->set_offset(data_offset);
		req->set_size(static_cast<uint32_t>(compressed_size));
		req->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
		req->get_destination_buffer_ref().resize(static_cast<int64_t>(original_size));

		DSContext *ds_ctx = DSContext::get_singleton();
		if (!ds_ctx) {
			UtilityFunctions::push_error("DSResourceFormatLoader: DSContext singleton not found.");
			return Variant();
		}

		if (!ds_ctx->is_initialized()) {
			ds_ctx->initialize_ds();
		}

		ds_ctx->enqueue_request(req);
		ds_ctx->flush_batch();

		if (req->get_status() != DSRequest::STATUS_COMPLETED) {
			UtilityFunctions::push_error("DSResourceFormatLoader: DirectStorage load failed for ", p_path, ": ", req->get_error_message());
			return Variant();
		}

		PackedByteArray decompressed_data = req->get_destination_buffer();
		Ref<Texture2D> result_texture;

		Image::Format img_fmt = static_cast<Image::Format>(DSTex::rd_format_to_godot_format(pixel_format));
		Ref<Image> image = Image::create_from_data(width, height, mip_count > 1, img_fmt, decompressed_data);
		if (image.is_valid() && !image->is_empty()) {
			result_texture = ImageTexture::create_from_image(image);
		}

		if (result_texture.is_valid()) {
			if (p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE && p_cache_mode != ResourceFormatLoader::CACHE_MODE_IGNORE_DEEP) {
				DSTextureCache *cache = DSTextureCache::get_singleton();
				if (cache) {
					cache->store(p_path, result_texture);
					if (!p_original_path.is_empty() && p_original_path != p_path) {
						cache->store(p_original_path, result_texture);
					}
				}
			}
			return result_texture;
		}

		UtilityFunctions::push_error("DSResourceFormatLoader: Failed to create GPU/Image texture for ", p_path);
		return Variant();
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
