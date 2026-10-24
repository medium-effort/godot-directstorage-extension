#include "ds_resource_format_loader.h"
#include "ds_preloader.h"
#include "ds_texture_cache.h"
#include "../compression/gdeflate_decompressor.h"
#include "../core/ds_context.h"
#include "../core/ds_request.h"
#include "../formats/dspkg_format.h"
#include "../formats/dstex_format.h"

#include <godot_cpp/classes/engine.hpp>
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
#include <cstdlib>
#include <mutex>
#include <thread>

namespace godot {

namespace {

uint64_t resolve_expected_fingerprint(ProjectSettings *p_ps) {
	if (p_ps && p_ps->has_setting("directstorage/expected_archive_fingerprint")) {
		return static_cast<uint64_t>(p_ps->get_setting("directstorage/expected_archive_fingerprint"));
	}
	return 0;
}

// Loader-local archive-path cache. Has its own guard and generation and
// shares no state with the preloader, so the sync and async queues stay
// separate. Caches BOTH outcomes of the "res://textures.dspkg" probe
// (project path + executable-directory join + globalized path): a hit
// serves the found path, a miss serves empty with zero
// FileAccess::file_exists probes (false-fast). Staleness contract (mirrors
// DSPreloader::rescan_archive): a textures.dspkg created after a miss was
// cached is picked up on the next loader teardown (destructor invalidates
// + bumps generation); the positive open-handle cache in
// _get_or_open_archive is untouched.

struct LoaderArchiveResolution {
	std::mutex mutex;
	String resolved_path;
	bool probed = false;
	uint64_t generation = 0;
};

LoaderArchiveResolution &loader_archive_resolution() {
	static LoaderArchiveResolution s;
	return s;
}

// Full Godot mip chain length for Image::create_from_data(use_mipmaps=true).
// Partial chains (1 < mips < full) must fall back to base-mip-only instead of failing.
static uint32_t ds_full_mip_chain_count(uint32_t p_w, uint32_t p_h) {
	uint32_t n = 1;
	while (p_w > 1 || p_h > 1) {
		if (p_w > 1) {
			p_w /= 2;
		}
		if (p_h > 1) {
			p_h /= 2;
		}
		++n;
	}
	return n;
}

} // namespace

void DSResourceFormatLoader::_bind_methods() {
}

DSResourceFormatLoader::DSResourceFormatLoader() {
}

DSResourceFormatLoader::~DSResourceFormatLoader() {
	std::lock_guard<std::mutex> lock(archive_mutex);
	cached_dspkg.unref();
	cached_archive_path = String();
	// Rescan-on-shutdown: drop the cached triple-probe resolution so a
	// textures.dspkg created/imported during this session resolves fresh
	// after teardown/reload. Lock order (archive_mutex -> resolution
	// mutex) is the only nesting site; no inversion possible.
	LoaderArchiveResolution &res = loader_archive_resolution();
	std::lock_guard<std::mutex> res_lock(res.mutex);
	res.resolved_path = String();
	res.probed = false;
	res.generation++;
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
	Engine *engine = Engine::get_singleton();
	OS *os = OS::get_singleton();
	ProjectSettings *project_settings = ProjectSettings::get_singleton();

	bool force_resolution = project_settings && project_settings->has_setting("directstorage/force_archive_resolution") &&
			static_cast<bool>(project_settings->get_setting("directstorage/force_archive_resolution"));

	if (!force_resolution && ((engine && engine->is_editor_hint()) || (os && os->has_feature("editor")))) {
		return String();
	}

	String archive_filename = "textures.dspkg";
	if (project_settings && project_settings->has_setting("directstorage/archive_name")) {
		String custom_name = project_settings->get_setting("directstorage/archive_name");
		if (!custom_name.is_empty()) {
			archive_filename = custom_name;
		}
	}
	String canonical_logical = "res://" + archive_filename;

	if (FileAccess::file_exists(p_logical_path)) {
		if (project_settings && p_logical_path.begins_with("res://")) {
			String globalized_probe = project_settings->globalize_path(p_logical_path);
			if (!FileAccess::file_exists(globalized_probe)) {
				UtilityFunctions::push_error("[DirectStorage] CONFIGURATION ERROR: Archive '", p_logical_path,
						"' is embedded inside the Godot PCK pack! DirectStorage hardware DMA requires a standalone file on the physical disk. "
						"DirectStorage DMA is disabled; degrading to CPU decompression.");
			}
		}
		return p_logical_path;
	}

	if (p_logical_path != canonical_logical && p_logical_path != "res://textures.dspkg") {
		return String();
	}

	// Cached resolution: the candidate probe below used to run on EVERY
	// miss — per _load, _exists, _recognize_path and _get_resource_type
	// call. A cached miss returns empty with zero probes (false-fast);
	// a cached hit skips straight to the caller.
	{
		std::lock_guard<std::mutex> lock(loader_archive_resolution().mutex);
		if (loader_archive_resolution().probed) {
			return loader_archive_resolution().resolved_path;
		}
	}

	String found;
	if (os) {
		String exe_dir = os->get_executable_path().get_base_dir();
		String executable_custom = exe_dir.path_join(archive_filename);
		String executable_default = exe_dir.path_join("textures.dspkg");
		if (FileAccess::file_exists(executable_custom)) {
			found = executable_custom;
		} else if (FileAccess::file_exists(executable_default)) {
			found = executable_default;
		}
	}

	if (found.is_empty() && project_settings) {
		String globalized_custom = project_settings->globalize_path(canonical_logical);
		String globalized_default = project_settings->globalize_path("res://textures.dspkg");
		if (FileAccess::file_exists(globalized_custom)) {
			found = globalized_custom;
		} else if (FileAccess::file_exists(globalized_default)) {
			found = globalized_default;
		}
	}

	{
		std::lock_guard<std::mutex> lock(loader_archive_resolution().mutex);
		LoaderArchiveResolution &res = loader_archive_resolution();
		if (!res.probed) {
			// First publisher wins; callers in this generation agree.
			res.resolved_path = found;
			res.probed = true;
		} else {
			// Lost a probe race while unlocked: honor the winner.
			found = res.resolved_path;
		}
	}

	if (found.is_empty() && project_settings &&
			static_cast<bool>(project_settings->get_setting("directstorage/export/bundle_dspkg", false))) {
		String exe_dir = os ? os->get_executable_path().get_base_dir() : String();
		String candidates_str = String("'res://") + archive_filename + String("', '") + exe_dir.path_join(archive_filename) +
				String("', '") + project_settings->globalize_path(canonical_logical) + String("'");
		if (archive_filename != "textures.dspkg") {
			candidates_str += String(", 'res://textures.dspkg', '") + exe_dir.path_join("textures.dspkg") +
					String("', '") + project_settings->globalize_path("res://textures.dspkg") + String("'");
		}
		UtilityFunctions::print("[DirectStorage] Archive '", archive_filename, "' not found. Searched candidates: ", candidates_str, ".");
	}

	return found;
}

Ref<DSPkg> DSResourceFormatLoader::_get_or_open_archive(const String &p_archive_path) const {
	if (p_archive_path.is_empty()) {
		return Ref<DSPkg>();
	}

	std::lock_guard<std::mutex> lock(archive_mutex);
	if (cached_archive_path == p_archive_path && cached_dspkg.is_valid()) {
		return cached_dspkg;
	}

	uint64_t expected_fingerprint = resolve_expected_fingerprint(ProjectSettings::get_singleton());

	Ref<DSPkg> archive;
	archive.instantiate();
	Error open_err = archive->open(p_archive_path, expected_fingerprint);
	if (open_err != OK) {
		if (open_err == ERR_UNAUTHORIZED) {
			static String s_last_reported_unauth_path;
			if (s_last_reported_unauth_path != p_archive_path) {
				s_last_reported_unauth_path = p_archive_path;
				UtilityFunctions::push_error("DirectStorage: Decryption failed for package '" + p_archive_path + "'. Ensure valid encryption key is configured via DSPkg::set_global_encryption_key().");
			}
		}
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
	(void)p_is_srgb;
	(void)p_mip_row_pitches;
	if (p_width == 0 || p_height == 0 || p_mip_count == 0) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Invalid 2D texture dimensions or mip count for '", p_cache_key, "' (width=", p_width, ", height=", p_height, ", mips=", p_mip_count, ").");
		return Variant();
	}
	if (!dstex_sizes_are_sane(p_width, p_height, p_original_size, p_compressed_size)) {
		UtilityFunctions::push_error("DSResourceFormatLoader: Payload exceeds sane texture bounds for '", p_cache_key, "' (FMT-01).");
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
	// True once the GPU branch has handed its RID to the cache. Guards the
	// end-of-function store() so a GPU texture is never stored twice.
	bool ownership_transferred = false;

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
	bool is_physical_source = true;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	OS *os = OS::get_singleton();
	if (p_file_path.begins_with("res://")) {
		if (os && !os->has_feature("editor") && ps && !static_cast<bool>(ps->get_setting("directstorage/export/bundle_dspkg", false))) {
			is_physical_source = false;
		} else {
			String global_path = ps ? ps->globalize_path(p_file_path) : p_file_path;
			is_physical_source = FileAccess::file_exists(global_path);
		}
	} else if (p_file_path.begins_with("user://")) {
		String global_path = ps ? ps->globalize_path(p_file_path) : p_file_path;
		is_physical_source = FileAccess::file_exists(global_path);
	}

	// Ownership pre-check (#0126). A Texture2DRD created below is a non-owning
	// wrapper: only DSTextureCache's evict/replace/clear paths ever call
	// free_rid() on its device RID (GODOT_QUIRKS.md section 27). Ownership is
	// transferred by the store() call inside the GPU branch, so the GPU path
	// is only worth attempting when the cache will accept the texture: cache
	// singleton present, enabled, and a non-ignored cache mode. Ignored modes
	// (IGNORE / IGNORE_DEEP) and a disabled cache fall through to the CPU
	// path, whose ImageTexture owns and frees its own RenderingServer RID.
	// This is a PRE-CHECK, not a reservation: set_enabled() can be called from
	// another thread and invalidate it. The hard guarantee lives in the GPU
	// branch, which frees the RID itself whenever store() reports refusal.
	DSTextureCache *ownership_cache = DSTextureCache::get_singleton();
	const bool cache_will_own = ownership_cache != nullptr && ownership_cache->can_own(p_cache_mode);

	bool can_attempt_gpu = false;
	if (p_as_image_texture) {
		UtilityFunctions::print_verbose("DSResourceFormatLoader: Texture requested as ImageTexture for '" + p_cache_key + "'; bypassing GPU direct upload.");
	} else if (!is_physical_source) {
		UtilityFunctions::print_verbose("DSResourceFormatLoader: Source file is embedded in PCK archive for '" + p_cache_key + "'; bypassing DirectStorage GPU upload.");
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
		} else if (!cache_will_own) {
			UtilityFunctions::print_verbose("DSResourceFormatLoader: Texture cache will not own the texture for '" + p_cache_key + "' (cache missing, disabled or ignored cache mode " + String::num_int64(p_cache_mode) + "); using CPU memory decompression to avoid an orphaned GPU RID.");
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
		// Shareable-list validation (rendering_device.cpp:1552): a non-empty
		// list MUST contain the base format itself. Add base first, then the
		// sRGB counterpart for 3D PBR albedo sampling.
		if (!DSTex::is_rd_format_srgb(static_cast<int>(p_pixel_format))) {
			int srgb_fmt = DSTex::get_srgb_rd_format(static_cast<int>(p_pixel_format));
			if (srgb_fmt != RenderingDevice::DATA_FORMAT_MAX && srgb_fmt != static_cast<int>(p_pixel_format)) {
				rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(p_pixel_format));
				rd_fmt->add_shareable_format(static_cast<RenderingDevice::DataFormat>(srgb_fmt));
			}
		}

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

				// Sync _load path: explicit from_stream=false so the batch
				// manager's stream flush can never steal a sync request.
				// Stays fully synchronous: enqueue + flush_batch + status poll.
				bool enqueued = ds_ctx->enqueue_request(req, false);
				if (enqueued) {
					ds_ctx->flush_batch();
				}
				if (enqueued && req->get_status() == DSRequest::STATUS_COMPLETED) {
					Ref<Texture2DRD> tex_rd;
					tex_rd.instantiate();
					tex_rd->set_texture_rd_rid(tex_rid);
					// Adopt-or-free, and do it HERE rather than at the end of
					// the function: the ownership decision happens exactly
					// once, and a refusal falls through to the CPU path with
					// the RID freed below. The can_own() gate above is only
					// work-avoidance; THIS is the soft-null guarantee (a
					// cross-thread set_enabled(false) between gate and store
					// cannot orphan the RID).
					const String alias = (!p_original_cache_key.is_empty() && p_original_cache_key != p_cache_key) ? p_original_cache_key : String();
					// Re-fetch rather than reusing the gate's pointer: a singleton
					// replacement between the gate and here (shutdown-only window)
					// would otherwise leave this store on a stale instance. The CPU
					// path below re-fetches for the same reason.
					DSTextureCache *store_cache = DSTextureCache::get_singleton();
					if (store_cache != nullptr && store_cache->store(p_cache_key, tex_rd, p_cache_mode, alias)) {
						result_texture = tex_rd;
						ownership_transferred = true;
					} else {
						UtilityFunctions::push_warning("DSResourceFormatLoader: Texture cache refused ownership of '", p_cache_key, "' (disabled or raced); releasing the GPU texture and falling back to CPU decompression.");
					}
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
			if (!p_context) {
				return false;
			}
			// On D3D12, do a kernel-level fence wait (0% CPU, immediate wakeup)
			if (p_context->is_d3d12_backend()) {
				if (!p_context->wait_for_completion(5000)) {
					return false;
				}
				p_context->collect_completed_results();
				for (int i = 0; i < p_requests.size(); ++i) {
					if (p_requests[i].is_valid() && p_requests[i]->get_status() == DSRequest::STATUS_FAILED) {
						return false;
					}
				}
				return true;
			}
			// Fallback backend (non-Windows / software): bounded drain loop
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (std::chrono::steady_clock::now() < deadline) {
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
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return false;
		};
		auto submit_memory_requests = [&](const Vector<Ref<DSRequest>> &p_requests) -> bool {
			if (p_requests.size() == 0) {
				return true;
			}
			if (p_use_sub_threads) {
				Error err = ds_ctx->flush_batch_async();
				if (err != OK) {
					return false;
				}
				return wait_for_async_requests(ds_ctx, p_requests);
			}
			ds_ctx->flush_batch();
			return true;
		};

		PackedByteArray natural_texture_data;
		if (p_is_v2 && p_mip_offsets.size() >= static_cast<int64_t>(mip_count) && p_mip_sizes.size() >= static_cast<int64_t>(mip_count)) {
			// Decompress each mip independently and unpad from 256-byte pitch footprint to natural image data
			bool ds_usable = is_physical_source && ds_ctx && ds_ctx->is_initialized();
			if (p_use_sub_threads && ds_usable) {
				if (!ds_ctx->is_batch_complete() || ds_ctx->has_stream_pending()) {
					ds_usable = false;
				}
			}
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
					// Sync path: from_stream=false; stream queue untouched.
					if (ds_ctx->enqueue_request(req, false)) {
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
				// Graceful Base Fallback: use_mipmaps=true demands the full chain.
				// Partial chains slice mip 0 and build without mipmaps + warning.
				bool use_mipmaps = mip_count > 1;
				PackedByteArray image_data;
				const uint32_t full_chain = ds_full_mip_chain_count(p_width, p_height);
				if (use_mipmaps && mip_count < full_chain) {
					const DSTexMipLayout base_layout = DSTex::get_mip_layout(p_width, p_height, img_fmt);
					const int64_t base_size = static_cast<int64_t>(base_layout.source_size);
					if (base_size > 0 && natural_texture_data.size() >= base_size) {
						image_data.resize(base_size);
						memcpy(image_data.ptrw(), natural_texture_data.ptr(), static_cast<size_t>(base_size));
						use_mipmaps = false;
						UtilityFunctions::push_warning("DSResourceFormatLoader: Partial mip chain (", mip_count, " of ", full_chain, ") for '", p_cache_key, "'; using base mip without mipmaps.");
					} else {
						UtilityFunctions::push_error("DSResourceFormatLoader: Partial mip chain with bad base size for '", p_cache_key, "' (mips=", mip_count, ", full=", full_chain, ").");
					}
				} else {
					image_data = natural_texture_data;
				}
				Ref<Image> image;
				if (image_data.size() > 0) {
					image = Image::create_from_data(p_width, p_height, use_mipmaps, static_cast<Image::Format>(img_fmt), image_data);
				}
				if (image.is_valid() && !image->is_empty()) {
					result_texture = ImageTexture::create_from_image(image);
				}
			}
		} else {
			// Legacy v1 path (single continuous stream, not footprint padded)
			PackedByteArray decompressed_data;
			bool ds_usable = is_physical_source && ds_ctx && ds_ctx->is_initialized();
			if (p_use_sub_threads && ds_usable) {
				if (!ds_ctx->is_batch_complete() || ds_ctx->has_stream_pending()) {
					ds_usable = false;
				}
			}
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

				// Sync path: from_stream=false; stream queue untouched.
				if (ds_ctx->enqueue_request(req, false)) {
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
				// Graceful Base Fallback (v1): partial chains slice mip 0, no mipmaps.
				bool use_mipmaps_v1 = mip_count > 1;
				PackedByteArray image_data_v1;
				const uint32_t full_chain_v1 = ds_full_mip_chain_count(p_width, p_height);
				if (use_mipmaps_v1 && mip_count < full_chain_v1) {
					const DSTexMipLayout base_layout_v1 = DSTex::get_mip_layout(p_width, p_height, img_fmt);
					const int64_t base_size_v1 = static_cast<int64_t>(base_layout_v1.source_size);
					if (base_size_v1 > 0 && decompressed_data.size() >= base_size_v1) {
						image_data_v1.resize(base_size_v1);
						memcpy(image_data_v1.ptrw(), decompressed_data.ptr(), static_cast<size_t>(base_size_v1));
						use_mipmaps_v1 = false;
						UtilityFunctions::push_warning("DSResourceFormatLoader: Partial mip chain (", mip_count, " of ", full_chain_v1, ") for '", p_cache_key, "'; using base mip without mipmaps.");
					} else {
						UtilityFunctions::push_error("DSResourceFormatLoader: Partial mip chain with bad base size for '", p_cache_key, "' (mips=", mip_count, ", full=", full_chain_v1, ").");
					}
				} else {
					image_data_v1 = decompressed_data;
				}
				Ref<Image> image;
				if (image_data_v1.size() > 0) {
					image = Image::create_from_data(p_width, p_height, use_mipmaps_v1, static_cast<Image::Format>(img_fmt), image_data_v1);
				}
				if (image.is_valid() && !image->is_empty()) {
					result_texture = ImageTexture::create_from_image(image);
				}
			}
		}
	}

	if (result_texture.is_valid()) {
		// GPU texture: ownership was already transferred (or refused and freed)
		// inside the GPU branch. Only the CPU-built ImageTexture reaches here
		// without an owner; a refused store is harmless for it because its
		// RenderingServer RID frees itself when the Ref drops.
		if (!ownership_transferred &&
				(p_cache_mode == ResourceFormatLoader::CACHE_MODE_REUSE || p_cache_mode == ResourceFormatLoader::CACHE_MODE_REPLACE)) {
			DSTextureCache *cache = DSTextureCache::get_singleton();
			if (cache) {
				String alias = (!p_original_cache_key.is_empty() && p_original_cache_key != p_cache_key) ? p_original_cache_key : String();
				cache->store(p_cache_key, result_texture, p_cache_mode, alias);
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

			// In-flight stream entries are pumped on the main/render thread.
			// Worker threads must never block: fall through to CPU path.
			String wait_path;
			if (DSPreloader::is_texture_pending(p_path)) {
				wait_path = p_path;
			} else if (!p_original_path.is_empty() && DSPreloader::is_texture_pending(p_original_path)) {
				wait_path = p_original_path;
			}

			if (!wait_path.is_empty()) {
				RenderingServer *rs_tmp = RenderingServer::get_singleton();
				bool on_render = rs_tmp != nullptr && rs_tmp->is_on_render_thread();
				if (on_render) {
					cached = DSPreloader::await_pending_texture(wait_path, 0);
					if (cached.is_valid()) {
						return cached;
					}
				}
				if (!p_original_path.is_empty() && p_original_path != wait_path) {
					cached = cache->get_cached(p_original_path, p_cache_mode);
					if (cached.is_valid()) {
						return cached;
					}
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
			bool is_srgb = (entry_info.has("flags") && (static_cast<uint32_t>(entry_info["flags"]) & 1U) != 0) ||
					(entry_info.has("is_srgb") && (bool)entry_info["is_srgb"]) ||
					DSTex::is_rd_format_srgb(pixel_format);
			uint32_t mip_count = entry_info["mip_count"];
			bool is_v2 = entry_info.has("has_gpu_metadata") ? (bool)entry_info["has_gpu_metadata"] : false;
			PackedInt64Array mip_offsets = entry_info.has("mip_offsets") ? (PackedInt64Array)entry_info["mip_offsets"] : PackedInt64Array();
			PackedInt32Array mip_sizes = entry_info.has("mip_sizes") ? (PackedInt32Array)entry_info["mip_sizes"] : PackedInt32Array();
			PackedInt32Array mip_uncompressed_sizes = entry_info.has("mip_uncompressed_sizes") ? (PackedInt32Array)entry_info["mip_uncompressed_sizes"] : PackedInt32Array();
			PackedInt32Array mip_row_pitches = entry_info.has("mip_row_pitches") ? (PackedInt32Array)entry_info["mip_row_pitches"] : PackedInt32Array();

			bool as_image_texture = (entry_info.has("flags") && (static_cast<uint32_t>(entry_info["flags"]) & 2U) != 0) ||
					(entry_info.has("as_image_texture") && (bool)entry_info["as_image_texture"]);

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
