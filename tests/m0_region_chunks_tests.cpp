// M0 spike: does DirectStorage accept a GDeflate upload into a ROW SUB-BOX of
// one subresource (DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION) when the mip is
// several times larger than the staging buffer?
//
// Why it exists: the packaging-time chunking plan (dstex v3 chunk tables) only
// helps if a chunk smaller than a mip can be written straight into its rows of
// the destination texture. Microsoft's samples only ever use TEXTURE_REGION for
// a WHOLE subresource, so the sub-box case is unproven.
//
// Shape of the proof:
//   1. 2048x2048 RGBA8 noise (incompressible, so compressed ~= uncompressed),
//      8 chunks of 256 rows = 2 MiB each, whole mip = 16 MiB.
//   2. Staging forced to 4 MiB: below the whole mip, above one chunk.
//   3. Negative control on its own texture: the whole mip as ONE request must
//      FAIL, otherwise the staging buffer is not the constraint we think and the
//      positive result below proves nothing.
//   4. Positive: 8 row-range chunk requests into one texture must COMPLETE, and
//      the GPU readback must equal the source bytes.
//
// The seam (ds_win32_debug_probe_texture_region) is DEBUG/DEV-only and never
// bound to GDScript. This file is deleted with the spike once M0 is answered.
//
// Needs a live D3D12 RenderingDevice: SKIPs under the headless run and is
// exercised with .vscode/run_d3d12_windowed.ps1 -Filter 'm0_region_chunks*'.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "compression/gdeflate_compressor.h"
#include "core/ds_context.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>
#include <string>

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#endif

namespace {

constexpr int PROBE_TEX = 2048;
constexpr int CHUNK_ROWS = 256;
constexpr int CHUNK_COUNT = PROBE_TEX / CHUNK_ROWS;
constexpr uint32_t STAGING_MB = 4;

godot::PackedByteArray make_noise_rgba8(int p_size) {
	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(p_size) * static_cast<int64_t>(p_size) * 4);
	uint8_t *w = data.ptrw();
	uint32_t state = 0x9E3779B9u;
	for (int64_t i = 0; i < data.size(); ++i) {
		state = state * 1664525u + 1013904223u;
		w[i] = static_cast<uint8_t>(state >> 24);
	}
	return data;
}

godot::RID create_probe_texture(godot::RenderingDevice *p_rd) {
	godot::Ref<godot::RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_width(PROBE_TEX);
	fmt->set_height(PROBE_TEX);
	fmt->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fmt->set_mipmaps(1);
	fmt->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
	fmt->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	godot::Ref<godot::RDTextureView> view;
	view.instantiate();
	return p_rd->texture_create(fmt, view, godot::TypedArray<godot::PackedByteArray>());
}

struct ProbeCleanup {
	godot::RenderingDevice *rd = nullptr;
	godot::RID texture;
	godot::String file;
	~ProbeCleanup() {
		if (rd != nullptr && texture.is_valid() && rd->texture_is_valid(texture)) {
			rd->free_rid(texture);
		}
		if (!file.is_empty()) {
			godot::DirAccess::remove_absolute(file);
		}
	}
};

} // namespace

GDEX_TEST_T(m0_region_chunks, row_range_upload, TAG_INTEGRATION | TAG_SLOW) {
#if !defined(_WIN32) || (!defined(DEBUG_ENABLED) && !defined(DEV_ENABLED))
	(void)ctx;
	GDEX_SKIP("M0 row-range probe is a Windows debug/dev-only seam.");
#else
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ps == nullptr) {
		GDEX_SKIP("ProjectSettings singleton unavailable.");
	}

	// The staging size is locked once a factory queue exists
	// (E_DSTORAGE_STAGING_BUFFER_LOCKED), so it must be configured through the
	// project setting and a fresh DSContext init. 16 MiB mip against 4 MiB
	// staging: every chunk fits, the whole mip cannot.
	const godot::String staging_key = "dsgd_extension/staging_buffer_size_mb";
	const bool had_staging = ps->has_setting(staging_key);
	const godot::Variant previous_staging = had_staging ? ps->get_setting(staging_key) : godot::Variant();
	if (ds->is_initialized()) {
		ds->shutdown_ds();
	}
	ps->set_setting(staging_key, static_cast<int64_t>(STAGING_MB));
	ctx.add_teardown([ds, ps, staging_key, had_staging, previous_staging]() {
		if (ds->is_initialized()) {
			ds->shutdown_ds();
		}
		if (had_staging) {
			ps->set_setting(staging_key, previous_staging);
		} else {
			ps->clear(staging_key);
		}
		ds->initialize_ds();
	});
	if (!ds->initialize_ds() || !ds->is_initialized()) {
		GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
	}
	if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver).");
	}
	if (ds->get_staging_buffer_size_mb() != STAGING_MB) {
		GDEX_FAIL(std::string("staging buffer setting was not applied (got ") +
				std::to_string(ds->get_staging_buffer_size_mb()) + " MiB)");
		return;
	}

	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("No RenderingDevice for the probe.");
	}

	ds->wait_for_completion(5000);
	ds->collect_completed_results();

	ProbeCleanup cleanup;
	cleanup.rd = rd;

	const godot::String rel_path = "user://m0_region_probe.bin";
	cleanup.file = ps->globalize_path(rel_path);

	const int64_t row_bytes = static_cast<int64_t>(PROBE_TEX) * 4;
	const int64_t chunk_bytes = static_cast<int64_t>(CHUNK_ROWS) * row_bytes;
	const godot::PackedByteArray pixels = make_noise_rgba8(PROBE_TEX);

	godot::Ref<godot::GDeflateCompressor> compressor;
	compressor.instantiate();

	godot::PackedByteArray chunk_stream[CHUNK_COUNT];
	uint64_t chunk_offset[CHUNK_COUNT] = {};
	uint32_t chunk_size[CHUNK_COUNT] = {};
	godot::PackedByteArray whole_stream;

	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(cleanup.file, godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		GDEX_FAIL("could not open the probe payload file");
		return;
	}
	uint64_t cursor = 0;
	for (int i = 0; i < CHUNK_COUNT; ++i) {
		godot::PackedByteArray chunk;
		chunk.resize(chunk_bytes);
		memcpy(chunk.ptrw(), pixels.ptr() + static_cast<int64_t>(i) * chunk_bytes, static_cast<size_t>(chunk_bytes));
		chunk_stream[i] = compressor->compress(chunk, 1);
		if (chunk_stream[i].size() == 0) {
			file->close();
			GDEX_FAIL("chunk compression produced no bytes");
			return;
		}
		chunk_offset[i] = cursor;
		chunk_size[i] = static_cast<uint32_t>(chunk_stream[i].size());
		file->store_buffer(chunk_stream[i]);
		if (file->get_error() != godot::OK) {
			file->close();
			GDEX_FAIL("writing a chunk stream failed");
			return;
		}
		cursor += static_cast<uint64_t>(chunk_stream[i].size());
	}

	whole_stream = compressor->compress(pixels, 1);
	if (whole_stream.size() == 0) {
		file->close();
		GDEX_FAIL("whole-mip compression produced no bytes");
		return;
	}
	const uint64_t whole_offset = cursor;
	const uint32_t whole_size = static_cast<uint32_t>(whole_stream.size());
	file->store_buffer(whole_stream);
	if (file->get_error() != godot::OK) {
		file->close();
		GDEX_FAIL("writing the whole-mip control stream failed");
		return;
	}
	file->close();

	// Positive: row-range chunks into one texture, staging 4 MiB.
	cleanup.texture = create_probe_texture(rd);
	GDEX_ASSERT_TRUE(cleanup.texture.is_valid());

	godot::String error;
	for (int i = 0; i < CHUNK_COUNT; ++i) {
		error = godot::String();
		const bool ok = godot::ds_win32_debug_probe_texture_region(
				cleanup.texture, 0, static_cast<uint32_t>(i * CHUNK_ROWS),
				static_cast<uint32_t>((i + 1) * CHUNK_ROWS), cleanup.file,
				chunk_offset[i], chunk_size[i], static_cast<uint32_t>(chunk_bytes), error);
		if (!ok) {
			GDEX_FAIL(std::string("row-range chunk ") + std::to_string(i) +
					" failed: " + ::gdextest::to_std_string(error));
		}
	}

	const godot::PackedByteArray readback = rd->texture_get_data(cleanup.texture, 0);
	GDEX_EXPECT_EQ(readback.size(), pixels.size());
	if (readback.size() == pixels.size()) {
		const bool identical = memcmp(readback.ptr(), pixels.ptr(), static_cast<size_t>(pixels.size())) == 0;
		GDEX_EXPECT_TRUE(identical);
		if (!identical) {
			int64_t first = -1;
			for (int64_t i = 0; i < readback.size(); ++i) {
				if (readback[i] != pixels[i]) {
					first = i;
					break;
				}
			}
			GDEX_FAIL(std::string("readback differs from the source at byte ") + std::to_string(first) +
					" (chunk upload landed at the wrong rows)");
		}
	}

	// Negative control: the whole 16 MiB mip as ONE request must not fit.
	const godot::RID control = create_probe_texture(rd);
	GDEX_ASSERT_TRUE(control.is_valid());
	error = godot::String();
	const bool whole_ok = godot::ds_win32_debug_probe_texture_region(
			control, 0, 0, PROBE_TEX, cleanup.file, whole_offset, whole_size,
			static_cast<uint32_t>(PROBE_TEX) * static_cast<uint32_t>(row_bytes), error);
	if (whole_ok) {
		GDEX_FAIL("the whole 16 MiB mip completed with a 4 MiB staging buffer; the staging "
				  "constraint is not what the chunk plan assumes, so the positive result proves nothing");
	} else {
		// Expected: the SDK rejects a request larger than the staging buffer.
		UtilityFunctions::print("[M0] whole-mip control rejected as expected: ", error);
		GDEX_EXPECT_TRUE(!::gdextest::to_std_string(error).empty());
	}
	if (rd->texture_is_valid(control)) {
		rd->free_rid(control);
	}
#endif
}
