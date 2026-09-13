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

class DSPkg;

class DSResourceFormatLoader : public ResourceFormatLoader {
	GDCLASS(DSResourceFormatLoader, ResourceFormatLoader);

	// Thread affinity & GPU Direct alignment (C2):
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

	String _resolve_archive_path(const String &p_logical_path) const;
	Ref<DSPkg> _get_or_open_archive(const String &p_archive_path) const;
	bool _is_texture_archive_entry(const String &p_path) const;

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
			const String &p_cache_key,
			const String &p_original_cache_key,
			int32_t p_cache_mode,
			bool p_use_sub_threads,
			bool p_as_image_texture = false) const;

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
};

} // namespace godot

#endif // DS_RESOURCE_FORMAT_LOADER_H
