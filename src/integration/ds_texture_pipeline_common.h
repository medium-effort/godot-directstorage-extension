#ifndef DS_TEXTURE_PIPELINE_COMMON_H
#define DS_TEXTURE_PIPELINE_COMMON_H

#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace godot {

class DSPkg;
class DSRequest;
class Image;
class ImageTexture;

namespace dsgd_texture_pipeline {

// Full Godot mip chain length for Image::create_from_data(use_mipmaps=true).
uint32_t full_mip_chain_count(uint32_t p_width, uint32_t p_height);

// True when running in the editor without dsgd_extension/force_archive_resolution.
bool archive_resolution_blocked_by_editor();

// Returns the configured archive filename (default "textures.dspkg").
String resolve_configured_archive_filename();

// Probes executable-directory and globalized-res:// candidate paths for a
// physical archive file on disk. Returns the first existing path, or empty.
String probe_physical_archive_candidates(const String &p_archive_filename);

// Reads dsgd_extension/archive_id (legacy export/archive_id) as a 64-bit
// pairing fingerprint; returns 0 when unpaired.
uint64_t resolve_expected_archive_fingerprint();

// Native-only dedup state for unauthorized-archive errors (safe at DLL unload).
struct UnauthorizedReportDedup {
	std::mutex mutex;
	uint64_t last_hash = 0;
	bool has_reported = false;
};

// Opens a .dspkg archive with pairing fingerprint validation and deduplicated
// unauthorized-key error logging.
Ref<DSPkg> open_archive_with_pairing(const String &p_archive_path,
		UnauthorizedReportDedup &r_dedup,
		const char *p_log_prefix,
		bool p_log_other_errors);

// Builds an RDTextureFormat suitable for DirectStorage GPU texture uploads,
// including base + sRGB shareable formats for 3D PBR albedo sampling.
Ref<RDTextureFormat> build_gpu_destination_texture_format(uint32_t p_width,
		uint32_t p_height,
		uint32_t p_pixel_format,
		uint32_t p_mip_count);

// Non-owning input descriptor for GPU upload unit enumeration and DSRequest
// assembly, shared between DSResourceFormatLoader and DSPreloader.
struct GpuUploadPlanInput {
	const String *file_path = nullptr;
	const String *debug_label = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pixel_format = 0;
	uint32_t mip_count = 0;
	uint8_t compression_format = 1; // DSCompressionFormat::GDEFLATE
	uint64_t data_offset = 0;
	uint64_t compressed_size = 0;
	uint64_t original_size = 0;
	bool chunked = false;
	bool gdeflate_prevalidated = false;
	uint64_t cancellation_tag = 0;
	const PackedInt64Array *mip_offsets = nullptr;
	const PackedInt32Array *mip_sizes = nullptr;
	const PackedInt32Array *mip_uncompressed_sizes = nullptr;
	const PackedInt32Array *mip_row_pitches = nullptr;
	const PackedInt32Array *mip_chunk_counts = nullptr;
	const PackedInt64Array *chunk_offsets = nullptr;
	const PackedInt32Array *chunk_compressed_sizes = nullptr;
	const PackedInt32Array *chunk_uncompressed_sizes = nullptr;
};

struct GpuUploadUnit {
	uint32_t mip = 0;
	int64_t chunk_index = -1; // -1 for single-chunk whole-subresource mips
	uint32_t region_top = 0;
	uint32_t region_bottom = 0;
};

// Validates a chunked mip/chunk table and emits one GpuUploadUnit per upload
// request (row-band for chunked mips, whole-subresource for single-chunk mips).
// Clears r_units and returns false on any malformed table or overflow.
bool enumerate_gpu_upload_units(const GpuUploadPlanInput &p_input, std::vector<GpuUploadUnit> &r_units);

// Returns the number of GPU upload requests that will be staged for p_input
// (1 for non-chunked, or validated unit count for chunked; 1 on malformed table).
size_t count_gpu_upload_requests(const GpuUploadPlanInput &p_input);

// Builds the DESTINATION_GPU_TEXTURE DSRequests targeting p_tex_rid.
bool build_gpu_upload_requests(const GpuUploadPlanInput &p_input,
		const RID &p_tex_rid,
		uint64_t p_native_format,
		Vector<Ref<DSRequest>> &r_uploads);

// True when Godot is running on the headless Dummy renderer (godot#124139).
bool is_dummy_renderer();

// Leaf mutex serializing DummyTexture RID allocation/freeing on Godot < 4.8.
std::mutex &dummy_texture_owner_mutex();

// Worker-safe ImageTexture::create_from_image wrapper that serializes under
// dummy_texture_owner_mutex() when is_dummy_renderer() is true.
Ref<ImageTexture> create_image_texture_safe(const Ref<Image> &p_image);

} // namespace dsgd_texture_pipeline
} // namespace godot

#endif // DS_TEXTURE_PIPELINE_COMMON_H
