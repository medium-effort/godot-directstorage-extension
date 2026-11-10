// Staging default policy and configuration plumbing.
//
// The default staging buffer is deliberately small now that imports are chunked:
// a request is bounded by one chunk (8 MiB by default), not by a whole mip. These
// tests pin the default, the reduction-only auto-tune interaction, and the fact
// that the effective value actually reaches IDStorageFactory (the factory has no
// getter, hence the debug seam).
//
// Ordering hazard: SetStagingBufferSize is locked once a queue or file exists
// (E_DSTORAGE_STAGING_BUFFER_LOCKED), so every configuration test goes through
// shutdown_ds() -> ProjectSettings -> initialize_ds() and restores the prior
// state in teardown. Setting the value on an initialized context must be inert.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_context.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <algorithm>
#include <cstdint>

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#endif

namespace {

constexpr const char *kStagingKey = "dsgd_extension/staging_buffer_size_mb";
constexpr const char *kUmaKey = "dsgd_extension/auto_tune_for_integrated_gpu";
constexpr const char *kLowVramKey = "dsgd_extension/auto_tune_for_low_vram_gpu";
constexpr uint32_t kDefaultMb = 32;

// Shutdown -> apply settings -> init, with a teardown that restores the prior
// values and leaves the context initialized as it found it.
struct StagingScope {
	godot::DSContext *ds = nullptr;
	godot::ProjectSettings *ps = nullptr;
	bool had_staging = false;
	bool had_uma = false;
	bool had_vram = false;
	godot::Variant prev_staging;
	godot::Variant prev_uma;
	godot::Variant prev_vram;

	StagingScope() {
		ds = godot::DSContext::get_singleton();
		ps = godot::ProjectSettings::get_singleton();
		if (ds == nullptr || ps == nullptr) {
			return;
		}
		had_staging = ps->has_setting(kStagingKey);
		had_uma = ps->has_setting(kUmaKey);
		had_vram = ps->has_setting(kLowVramKey);
		prev_staging = had_staging ? ps->get_setting(kStagingKey) : godot::Variant();
		prev_uma = had_uma ? ps->get_setting(kUmaKey) : godot::Variant();
		prev_vram = had_vram ? ps->get_setting(kLowVramKey) : godot::Variant();	}

	void apply(uint32_t p_mb, bool p_auto_tune) {
		if (ds == nullptr || ps == nullptr) {
			return;
		}
		if (ds->is_initialized()) {
			ds->shutdown_ds();
		}
		ps->set_setting(kStagingKey, static_cast<int64_t>(p_mb));
		ps->set_setting(kUmaKey, p_auto_tune);
		ps->set_setting(kLowVramKey, p_auto_tune);
		ds->initialize_ds();
	}

	~StagingScope() {
		if (ds == nullptr || ps == nullptr) {
			return;
		}
		if (ds->is_initialized()) {
			ds->shutdown_ds();
		}
		auto restore = [this](const char *p_key, bool p_had, const godot::Variant &p_prev) {
			if (p_had) {
				ps->set_setting(p_key, p_prev);
			} else {
				ps->clear(p_key);
			}
		};
		restore(kStagingKey, had_staging, prev_staging);
		restore(kUmaKey, had_uma, prev_uma);
		restore(kLowVramKey, had_vram, prev_vram);
		ds->initialize_ds();
	}
};

godot::PackedByteArray make_noise(int p_width, int p_height, int p_mips) {
	godot::PackedByteArray data;
	int w = p_width;
	int h = p_height;
	for (int mip = 0; mip < p_mips; ++mip) {
		const int64_t bytes = static_cast<int64_t>(w) * static_cast<int64_t>(h) * 4;
		const int64_t start = data.size();
		data.resize(start + bytes);
		uint8_t *dst = data.ptrw() + start;
		uint32_t state = 0x9E3779B9u ^ static_cast<uint32_t>(mip * 7919);
		for (int64_t i = 0; i < bytes; ++i) {
			state = state * 1664525u + 1013904223u;
			dst[i] = static_cast<uint8_t>(state >> 24);
		}
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	return data;
}

void track_cleanup(::gdextest::TestContext &ctx, const char *p_path) {
	const godot::String path(p_path);
	ctx.add_teardown([path]() {
		godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
		if (ps != nullptr) {
			godot::DirAccess::remove_absolute(ps->globalize_path(path));
		}
	});
}

} // namespace

GDEX_TEST(staging_default, constant_is_lowered_to_32) {
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DSContext::DEFAULT_STAGING_BUFFER_SIZE_MB),
			static_cast<int64_t>(kDefaultMb));
	GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DSContext::clamp_staging_buffer_size_mb(kDefaultMb)),
			static_cast<int64_t>(kDefaultMb));
}

GDEX_TEST(staging_default, auto_tune_tiers_never_go_below_the_default) {
	// The default must be at or below every reduction-only tier, otherwise the
	// auto-tune would turn the default into a clamped value and silently create
	// load failures. 0 means "no cap" and is fine.
	const int64_t tiers[] = { 0, 1024, 2047, 2048, 4096, 6144, 8191, 8192, 24576 };
	for (const int64_t vram : tiers) {
		const uint32_t cap = godot::DSContext::low_vram_staging_cap_mb(vram);
		GDEX_EXPECT_TRUE(cap == 0 || cap >= godot::DSContext::DEFAULT_STAGING_BUFFER_SIZE_MB);
	}
	GDEX_EXPECT_TRUE(godot::DSContext::UMA_MAX_STAGING_BUFFER_SIZE_MB >= godot::DSContext::DEFAULT_STAGING_BUFFER_SIZE_MB);
}

GDEX_TEST_T(staging_default, effective_value_reaches_the_factory, TAG_INTEGRATION) {
	StagingScope scope;
	GDEX_ASSERT_TRUE(scope.ds != nullptr && scope.ps != nullptr);
	scope.apply(kDefaultMb, /*auto_tune=*/false);
	if (!scope.ds->is_d3d12_backend()) {
		GDEX_SKIP("D3D12 backend unavailable (headless or non-D3D12 driver).");
	}
	GDEX_EXPECT_EQ(static_cast<int64_t>(scope.ds->get_staging_buffer_size_mb()), static_cast<int64_t>(kDefaultMb));
#ifdef _WIN32
	GDEX_EXPECT_EQ(godot::Win32Backend::_debug_last_applied_staging_bytes(),
			static_cast<int64_t>(kDefaultMb) * 1024 * 1024);
#else
	GDEX_SKIP("Win32 staging seam is Windows-only.");
#endif
}

GDEX_TEST(staging_default, explicit_value_is_applied_over_the_default) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr) {
		GDEX_SKIP("DSContext singleton unavailable.");
	}
	StagingScope scope;
	scope.apply(16, /*auto_tune=*/false);
	GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_staging_buffer_size_mb()), static_cast<int64_t>(16));
}

GDEX_TEST(staging_default, post_init_change_does_not_move_the_effective_value) {
	godot::DSContext *ds = godot::DSContext::get_singleton();
	godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
	if (ds == nullptr || ps == nullptr) {
		GDEX_SKIP("DSContext/ProjectSettings unavailable.");
	}
	StagingScope scope;
	scope.apply(kDefaultMb, /*auto_tune=*/false);
	// Changing the setting while initialized must not affect the locked runtime.
	ps->set_setting(kStagingKey, static_cast<int64_t>(64));
	GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_staging_buffer_size_mb()), static_cast<int64_t>(kDefaultMb));
	// A fresh init (what the next launch does) picks it up.
	if (ds->is_initialized()) {
		ds->shutdown_ds();
	}
	ds->initialize_ds();
	GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_staging_buffer_size_mb()), static_cast<int64_t>(64));
}

GDEX_TEST(staging_default, imported_chunk_requirement_fits_the_default) {
	// The import default (8 MiB chunks) and the staging default must agree: a
	// 2048x2048 RGBA8 mip is 16 MiB and splits into two 8 MiB chunks.
	const char *path = "user://gdextest_staging_default_req.dstex";
	track_cleanup(ctx, path);
	const godot::PackedByteArray source = make_noise(2048, 2048, 1);
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	GDEX_ASSERT_EQ(dstex->write_to_file(godot::String(path), source, 2048, 2048,
						   godot::Image::FORMAT_RGBA8, 1, 1, false, false, 0, 0, 0, 8192),
			godot::OK);
	const godot::Dictionary header = dstex->read_header(godot::String(path));
	GDEX_ASSERT_TRUE(!header.is_empty());
	GDEX_ASSERT_TRUE(static_cast<bool>(header["chunked"]));
	const int64_t required = static_cast<int64_t>(header["min_staging_bytes"]);
	GDEX_EXPECT_TRUE(required > 0);
	GDEX_EXPECT_TRUE(required <= static_cast<int64_t>(godot::DSContext::DEFAULT_STAGING_BUFFER_SIZE_MB) * 1024 * 1024);
}
