#ifndef DS_RESOURCE_FORMAT_LOADER_H
#define DS_RESOURCE_FORMAT_LOADER_H

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <mutex>

namespace godot {

class DSContext;
class DSPkg;

class DSResourceFormatLoader : public ResourceFormatLoader {
	GDCLASS(DSResourceFormatLoader, ResourceFormatLoader);

	// Thread affinity & GPU Direct alignment:
	// - Direct GPU decompression (DESTINATION_GPU_TEXTURE via DirectStorage)
	//   requires the render thread because RenderingDevice::get_driver_resource()
	//   and free_rid() assert is_on_render_thread(). Off-render-thread callers
	//   (main thread, ResourceLoader worker threads / p_use_sub_threads) cleanly
	//   fall back to memory destination + CPU GDeflate + ImageTexture without
	//   asserting or spamming D3D12 mapping errors.
	// - On the render thread with D3D12, the loader creates a Texture2DRD via
	//   RenderingDevice::texture_create() and submits a GPU texture destination
	//   request through DSContext.
	// - v2 textures carry D3D12 footprint alignment (256-byte row pitch,
	//   512-byte placement); the GPU path uploads padded footprints directly,
	//   while the CPU fallback unpads via DSTex::get_mip_layout().
private:
	mutable std::mutex archive_mutex;
	mutable String cached_archive_path;
	mutable Ref<DSPkg> cached_dspkg;
	// Identity of the file the cached TOC was parsed from. A replaced archive
	// must be re-read instead of serving the previous entries.
	mutable uint64_t cached_archive_size = 0;
	mutable uint64_t cached_archive_write_time = 0;

	String _resolve_archive_path(const String &p_logical_path) const;
	Ref<DSPkg> _get_or_open_archive(const String &p_archive_path) const;
	// Opens p_archive_path into a caller-owned instance. Never reads or writes
	// the shared cached_dspkg: the resource system must not hand out a mutable
	// handle into the loader's archive view, because a failed DSPkg::open()
	// clears the entry tables and would break every in-archive load for the
	// rest of the process.
	Ref<DSPkg> _open_archive_instance(const String &p_archive_path) const;
	bool _cached_archive_is_current(const String &p_archive_path) const;
	bool _is_texture_archive_entry(const String &p_path) const;

	// Initializes the DirectStorage context for a load, without ever waiting for a
	// device acquisition the calling thread cannot serve. Main-thread and
	// render-thread callers initialize inline; a background caller posts one
	// deferred initialization for the main thread and returns true so the load
	// takes the CPU path instead of publishing the software fallback.
	bool _maybe_initialize_context(DSContext *p_ctx) const;

	Variant _load_texture_payload(
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
			const PackedInt32Array &p_mip_chunk_counts,
			const PackedInt64Array &p_chunk_offsets,
			const PackedInt32Array &p_chunk_compressed_sizes,
			const PackedInt32Array &p_chunk_uncompressed_sizes,
			bool p_chunked,
			const String &p_cache_key,
			const String &p_original_cache_key,
			int32_t p_cache_mode,
			bool p_use_sub_threads,
			int p_compression_format,
			bool p_as_image_texture = false,
			uint32_t p_logical_width = 0,
			uint32_t p_logical_height = 0) const;

protected:
	static void _bind_methods();

public:
	DSResourceFormatLoader();
	~DSResourceFormatLoader();

	virtual PackedStringArray _get_recognized_extensions() const override;
	virtual bool _recognize_path(const String &p_path, const StringName &p_type) const override;
	virtual bool _handles_type(const StringName &p_type) const override;
	virtual String _get_resource_type(const String &p_path) const override;
	virtual bool _exists(const String &p_path) const override;
	virtual Variant _load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seams, deliberately NEVER bound to GDScript: run the lazy-init
	// decision on the calling thread, and run a pending deferred initialization
	// synchronously (what the main thread does when it drains the call).
	bool _debug_loader_init_decision_for_testing() const;
	void _debug_run_pending_loader_init_for_testing();
#endif
};

} // namespace godot

#endif // DS_RESOURCE_FORMAT_LOADER_H
