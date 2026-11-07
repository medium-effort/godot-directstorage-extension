// tests/gpu_prewarm_tests.cpp
//
// D3D12-only end-to-end coverage for the Phase-2 prewarm-owned GPU path
// (TAG_SLOW; run windowed via .vscode/run_d3d12_windowed.ps1, default
// filter "gpu_*"). Headless CI skips the DMA case: is_d3d12_backend() is
// false without a real D3D12 RenderingDevice.
//
// - gpu_prewarm.dma_fills_vram_texture stages a small v2 .dstex behind
//   res://textures.dspkg, runs prewarm_gpu_async(), drives the chunk
//   render callback + stream flush + completion pump until the tag
//   settles, then proves the cached resident is a Texture2DRD (DMA-filled
//   VRAM texture), not an ImageTexture fallback.
// - gpu_prewarm.graceful_memory_fallback forces prefer_gpu=false and
//   proves the same entry completes as an ImageTexture on any driver
//   (also guards the #0039 partial-chain fallback through the prewarm
//   memory path: 2 mips of a 7-mip chain arrive without mipmaps).
// - gpu_prewarm.linear_formats_fill_vram_texture stages BC5 and BC4 v2
//   .dstex fixtures (linear-only block formats with no sRGB view) and
//   proves both now take the GPU path: the cached resident is a
//   Texture2DRD whose RD format/mip count match, and whose mip bytes equal
//   the natural payload that was written (tight or row-padded readback).
//
// Linkage: extension sources compile into the test library
// (.gdextest.toml [gdextest.tests] sources), so direct includes are used.
// DSContext/DSTextureCache instances are owned per-test (teardown
// shutdown + memdelete); the static singletons are restored by the
// destructors. DSPreloader::shutdown() is called defensively at start so
// a stale archive resolution from an earlier suite cannot mask the
// staged textures.dspkg.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "core/ds_context.h"
#include "core/ds_request.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "integration/ds_preloader.h"
#include "integration/ds_resource_format_loader.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2drd.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kGpuTexWidth = 64;
constexpr int kGpuTexHeight = 64;
constexpr int kGpuMipCount = 2;
// Base 16384 (64x64 RGBA8) + mip1 4096 (32x32 RGBA8) natural bytes.
constexpr int64_t kGpuRawSize = 20480;
constexpr int64_t kGpuBaseSize = 16384;

// Linear-only fixtures (BC4/BC5). 64px keeps the base mip's compressed row
// exactly one 256B pitch, while mip 1 (32px) exercises the row-padding trim.
constexpr int kLinearTexWidth = 64;
constexpr int kLinearTexHeight = 64;
constexpr int kLinearMipCount = 2;

godot::PackedByteArray make_gpu_raw() {
    godot::PackedByteArray raw;
    raw.resize(kGpuRawSize);
    uint8_t *w = raw.ptrw();
    for (int64_t i = 0; i < kGpuRawSize; ++i) {
        w[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    return raw;
}

void remove_abs(const char *p_path);

// Trivial render-thread payload for the post-mechanism smoke test.
// Plain function + atomic flag (no Godot types at file scope, so no
// DllMain static-init hazard).
namespace {
std::atomic<bool> g_render_cb_ran{ false };
void test_render_cb_body() {
    g_render_cb_ran.store(true);
}
} // namespace

// Writes a 2-mip v2 .dstex, wraps it in a textures.dspkg with entries for
// p_virtual_path and p_control_path, and publishes the archive at
// res://textures.dspkg. Does NOT register teardowns: the caller owns
// cleanup ordering.
bool stage_gpu_dspkg_impl(const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path, const char *p_control_path,
        const godot::PackedByteArray &p_raw, int p_width, int p_height, int p_mip_count) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    if (dstex->write_to_file(godot::String(p_dstex_path), p_raw,
                p_width, p_height, godot::Image::FORMAT_RGBA8, p_mip_count, 1) != godot::OK) {
        return false;
    }
    godot::Ref<godot::DSPkg> builder;
    builder.instantiate();
    if (builder.is_null()) {
        return false;
    }
    if (builder->create(godot::String(p_dspkg_path)) != godot::OK) {
        return false;
    }
    if (builder->add_texture(godot::String(p_virtual_path), godot::String(p_dstex_path)) != godot::OK) {
        return false;
    }
    if (builder->add_texture(godot::String(p_control_path), godot::String(p_dstex_path)) != godot::OK) {
        return false;
    }
    if (builder->finalize() != godot::OK) {
        return false;
    }
    // Remove-then-copy with one retry: transient file locks (Defender
    // scan windows on fresh res:// files) have failed both the remove
    // and the safe-save commit before. A stale res://textures.dspkg
    // poisons every later resolve, so a failed verify retries once
    // after a short yield instead of trusting the copy return code.
    for (int attempt = 0; attempt < 2; ++attempt) {
        godot::Error remove_err = godot::DirAccess::remove_absolute(godot::String("res://textures.dspkg"));
        godot::Error copy_err = godot::DirAccess::copy_absolute(godot::String(p_dspkg_path), godot::String("res://textures.dspkg"));
        const bool exists = godot::FileAccess::file_exists(godot::String("res://textures.dspkg"));

        int64_t staged_len = -1;
        {
            godot::Ref<godot::FileAccess> staged = godot::FileAccess::open(godot::String("res://textures.dspkg"), godot::FileAccess::READ);
            if (staged.is_valid()) {
                staged_len = staged->get_length();
                staged->close();
            }
        }

        godot::Error open_err = godot::ERR_CANT_OPEN;
        int64_t entries = -1;
        bool has_virtual = false;
        bool has_control = false;
        godot::Ref<godot::DSPkg> verify;
        verify.instantiate();
        if (!verify.is_null()) {
            open_err = verify->open(godot::String("res://textures.dspkg"));
            entries = verify->get_entry_count();
            has_virtual = verify->has_texture(godot::String(p_virtual_path));
            has_control = verify->has_texture(godot::String(p_control_path));
        }

        if (exists && open_err == godot::OK && has_virtual && has_control) {
            return true;
        }

        godot::UtilityFunctions::push_warning(
                "gpu_prewarm stage attempt ", attempt,
                ": remove=", static_cast<int64_t>(remove_err),
                " copy=", static_cast<int64_t>(copy_err),
                " exists=", exists,
                " staged_len=", staged_len,
                " open=", static_cast<int64_t>(open_err),
                " entries=", entries,
                " has_virtual=", has_virtual,
                " has_control=", has_control);

        if (attempt == 0) {
            godot::OS *os = godot::OS::get_singleton();
            if (os != nullptr) {
                os->delay_msec(500);
            }
        }
    }
    return false;
}

bool stage_gpu_dspkg(const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path, const char *p_control_path) {
    return stage_gpu_dspkg_impl(p_dstex_path, p_dspkg_path, p_virtual_path, p_control_path,
            make_gpu_raw(), kGpuTexWidth, kGpuTexHeight, kGpuMipCount);
}

// Same staging, but with a payload large enough that the GPU copy is still
// running when the test cancels. kGpuTexWidth (64) retires in microseconds, so
// an in-flight assertion against it would be a coin flip; 2048^2 incompressible
// RGBA8 keeps the DMA + GDeflate decode window at tens of milliseconds (the
// shape gpu_drain_tests.cpp already relies on).
bool stage_large_gpu_dspkg(const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path, const char *p_control_path) {
    constexpr int kLargeSize = 2048;
    godot::PackedByteArray raw;
    raw.resize(static_cast<int64_t>(kLargeSize) * static_cast<int64_t>(kLargeSize) * 4);
    uint8_t *writer = raw.ptrw();
    uint32_t state = 0x9E3779B9u;
    for (int64_t i = 0; i < raw.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        writer[i] = static_cast<uint8_t>(state >> 24);
    }
    return stage_gpu_dspkg_impl(p_dstex_path, p_dspkg_path, p_virtual_path, p_control_path,
            raw, kLargeSize, kLargeSize, 1);
}

// Deterministic *natural* (unpadded) block bytes for a linear-only RD format.
// Arbitrary block bytes are valid BC4/BC5 data (each block is an independent
// endpoint pair plus indices), and using them instead of a compressor output
// makes the readback equality check stricter: any misplaced byte, swapped row
// or wrong mip boundary shows up as a mismatch.
godot::PackedByteArray make_linear_raw(godot::Image::Format p_format, int p_width, int p_height, int p_mip_count) {
    int64_t total = 0;
    uint32_t mip_width = static_cast<uint32_t>(p_width);
    uint32_t mip_height = static_cast<uint32_t>(p_height);
    for (int i = 0; i < p_mip_count; ++i) {
        godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(mip_width, mip_height, p_format);
        total += static_cast<int64_t>(layout.source_size);
        mip_width = mip_width > 1 ? (mip_width >> 1) : 1;
        mip_height = mip_height > 1 ? (mip_height >> 1) : 1;
    }
    godot::PackedByteArray raw;
    raw.resize(total);
    uint8_t *writer = raw.ptrw();
    uint32_t state = 0x9E3779B9u ^ static_cast<uint32_t>(p_format);
    for (int64_t i = 0; i < total; ++i) {
        state = state * 1664525u + 1013904223u;
        writer[i] = static_cast<uint8_t>(state >> 24);
    }
    return raw;
}

// Chain totals for one mip set: natural (tight rows) and 256B-row footprints.
void linear_chain_sizes(godot::Image::Format p_format, int p_width, int p_height, int p_mip_count,
        int64_t &r_natural, int64_t &r_footprint) {
    r_natural = 0;
    r_footprint = 0;
    uint32_t mip_width = static_cast<uint32_t>(p_width);
    uint32_t mip_height = static_cast<uint32_t>(p_height);
    for (int i = 0; i < p_mip_count; ++i) {
        godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(mip_width, mip_height, p_format);
        r_natural += static_cast<int64_t>(layout.source_size);
        r_footprint += static_cast<int64_t>(layout.footprint_size);
        mip_width = mip_width > 1 ? (mip_width >> 1) : 1;
        mip_height = mip_height > 1 ? (mip_height >> 1) : 1;
    }
}

bool write_linear_dstex(const char *p_path, godot::Image::Format p_format, int p_width, int p_height,
        int p_mip_count, const godot::PackedByteArray &p_raw) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    return dstex->write_to_file(godot::String(p_path), p_raw, p_width, p_height,
                   static_cast<int>(p_format), p_mip_count, 1) == godot::OK;
}

// Compares an RD readback against the natural payload that went into the
// fixture. The D3D12 copy path returns either tight rows or 256B-padded rows
// depending on the format path, so exactly one of those two shapes is accepted
// and compared byte-for-byte.
bool readback_matches_natural(const godot::PackedByteArray &p_readback, const godot::PackedByteArray &p_expected,
        godot::Image::Format p_format, int p_width, int p_height, int p_mip_count, std::string &r_detail) {
    int64_t natural_total = 0;
    int64_t footprint_total = 0;
    linear_chain_sizes(p_format, p_width, p_height, p_mip_count, natural_total, footprint_total);
    if (p_readback.size() == natural_total) {
        r_detail = "tight";
        return memcmp(p_readback.ptr(), p_expected.ptr(), static_cast<size_t>(natural_total)) == 0;
    }
    if (p_readback.size() != footprint_total) {
        r_detail = "unexpected readback size " + std::to_string(p_readback.size()) +
                " (tight=" + std::to_string(natural_total) + ", padded=" + std::to_string(footprint_total) + ")";
        return false;
    }
    r_detail = "padded";
    int64_t read_offset = 0;
    int64_t expected_offset = 0;
    uint32_t mip_width = static_cast<uint32_t>(p_width);
    uint32_t mip_height = static_cast<uint32_t>(p_height);
    for (int i = 0; i < p_mip_count; ++i) {
        godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(mip_width, mip_height, p_format);
        godot::PackedByteArray padded = p_readback.slice(read_offset, read_offset + static_cast<int64_t>(layout.footprint_size));
        godot::PackedByteArray natural = godot::DSTex::unpad_footprint_mip(padded, mip_width, mip_height, p_format);
        if (natural.size() != static_cast<int64_t>(layout.source_size) ||
                memcmp(natural.ptr(), p_expected.ptr() + expected_offset, static_cast<size_t>(layout.source_size)) != 0) {
            r_detail = "padded mip " + std::to_string(i) + " mismatch";
            return false;
        }
        read_offset += static_cast<int64_t>(layout.footprint_size);
        expected_offset += static_cast<int64_t>(layout.source_size);
        mip_width = mip_width > 1 ? (mip_width >> 1) : 1;
        mip_height = mip_height > 1 ? (mip_height >> 1) : 1;
    }
    return true;
}

void remove_abs(const char *p_path) {
    // Loud (not silent): a leftover res://textures.dspkg poisons every
    // later resolve, and silent ignores hid a locked-file incident for
    // hours. Failure here is advisory (callers re-verify after copy).
    godot::Error err = godot::DirAccess::remove_absolute(godot::String(p_path));
    if (err != godot::OK && err != godot::ERR_FILE_NOT_FOUND) {
        godot::UtilityFunctions::push_warning("gpu_prewarm test: remove failed for ", godot::String(p_path),
                " (leaving a possibly stale file): ", (int64_t)err);
    }
}

godot::DSContext *require_singleton_context(::gdextest::TestContext &ctx) {
    // Module-lifecycle singleton: register_types memnews DSContext at load
    // and the static singleton keeps the first instance, so a test memnew
    // can NEVER replace it (the ctor only sets the static when null).
    // Drive the real singleton: preloader/loader resolve through
    // get_singleton(), while a private instance would observe vacuous tag
    // progress (unknown tags read complete) and miss the cache.
    godot::DSContext *ds = godot::DSContext::get_singleton();
    if (ds == nullptr) {
        GDEX_SKIP("DSContext singleton unavailable (module init did not run)");
    }
    if (!ds->is_initialized() && (!ds->initialize_ds() || !ds->is_initialized())) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed).");
    }
    ctx.add_teardown([]() {
        godot::DSContext *live = godot::DSContext::get_singleton();
        if (live != nullptr) {
            live->shutdown_ds();
        }
    });
    return ds;
}

godot::DSTextureCache *new_test_cache(::gdextest::TestContext &ctx) {
    godot::DSTextureCache *cache = memnew(godot::DSTextureCache);
    cache->set_enabled(true);
    ctx.add_teardown([cache]() {
        cache->clear();
        memdelete(cache);
    });
    return cache;
}

// Drives stream flush + completion collect + budgeted build until the tag
// settles or the deadline passes. The windowed render thread executes the
// posted GPU chunk callback on its own frames; this loop only pumps the
// main-thread side. (Time, not OS, carries get_ticks_msec in godot-cpp.)
bool drive_prewarm_to_settled(godot::DSContext *ds, godot::DSTextureCache *cache, uint64_t p_tag, uint64_t p_deadline_ms) {
    godot::OS *os = godot::OS::get_singleton();
    godot::Time *time = godot::Time::get_singleton();
    if (os == nullptr || time == nullptr) {
        return false;
    }
    while (time->get_ticks_msec() < p_deadline_ms) {
        cache->pump_gpu_prewarm();
        if (ds->has_stream_pending()) {
            ds->flush_stream_async();
        }
        ds->collect_completed_results();
        cache->poll_completed(p_tag, 0, 0);
        if (ds->is_prewarm_complete(p_tag)) {
            return true;
        }
        os->delay_msec(100);
    }
    return ds->is_prewarm_complete(p_tag);
}

} // namespace

GDEX_TEST(gpu_prewarm, render_post_smoke) {
    // Proves the call_on_render_thread post mechanism itself (no prewarm
    // state involved). Runs everywhere: the no-throw assertion holds on
    // any driver; the ran-flag is only asserted under d3d12 (headless
    // dummy has no pumping render thread).
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    if (rs == nullptr) {
        GDEX_SKIP("no RenderingServer singleton");
    }
    g_render_cb_ran.store(false);
    try {
        godot::Callable cb = callable_mp_static(test_render_cb_body);
        rs->call_on_render_thread(cb);
    } catch (const std::exception &e) {
        GDEX_FAIL(std::string("trivial render post threw std: ") + e.what());
    } catch (...) {
        GDEX_FAIL("trivial render post threw unknown.");
    }
    if (rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
        return;
    }
    godot::OS *os = godot::OS::get_singleton();
    for (int i = 0; i < 40 && !g_render_cb_ran.load(); ++i) {
        if (os != nullptr) {
            os->delay_msec(50);
        }
    }
    GDEX_EXPECT_TRUE(g_render_cb_ran.load());
}

GDEX_TEST_T(gpu_prewarm, dma_fills_vram_texture, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    // Drop file handles an earlier suite may still have in flight before
    // staging: a previously loaded res://textures.dspkg cannot be removed or
    // replaced while a batch owns its handle (the copy reports success and the
    // safe-save commit rolls back). Handles close at batch completion now, so
    // this only covers a batch that has not retired yet.
    // require_singleton_context re-initializes right after.
    if (godot::DSContext *pre = godot::DSContext::get_singleton()) {
        pre->shutdown_ds();
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
        GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver)");
    }
    if (!godot::DSPreloader::is_gpu_prewarm_available()) {
        GDEX_SKIP("GPU prewarm unavailable (no live D3D12 RenderingDevice)");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *dstex_path = "user://gdextest_gpu_prewarm_dma.dstex";
    const char *dspkg_path = "user://gdextest_gpu_prewarm_dma.dspkg";
    const char *virtual_path = "res://gdextest_gpu_prewarm_dma.png";
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        ps->set_setting("directstorage/force_archive_resolution", true);
    }
    ctx.add_teardown([ds, ps]() {
        if (ps) {
            ps->set_setting("directstorage/force_archive_resolution", false);
        }
        // Shutdown FIRST: a batch can still own the IDStorageFile handle for
        // res://textures.dspkg, and Windows refuses the remove while it does
        // (removing first fails silently and poisons the next suite with a
        // stale archive).
        ds->shutdown_ds();
        remove_abs("res://textures.dspkg");
        remove_abs("user://gdextest_gpu_prewarm_dma.dstex");
        remove_abs("user://gdextest_gpu_prewarm_dma.dspkg");
        godot::DSPreloader::shutdown();
    });
    if (!stage_gpu_dspkg(dstex_path, dspkg_path, virtual_path, "res://gdextest_gpu_prewarm_dctl.png")) {
        GDEX_ABORT_TEST("textures.dspkg staging failed");
    }

    // Staging proof (mirrors the mem test): the DMA virtual path must
    // resolve with v2 GPU metadata before the prewarm calls run.
    {
        godot::Ref<godot::DSPkg> probe;
        probe.instantiate();
        GDEX_ASSERT_TRUE(probe.is_valid());
        GDEX_ASSERT_EQ((int64_t)probe->open(godot::String("res://textures.dspkg")), (int64_t)godot::OK);
        GDEX_EXPECT_TRUE(probe->has_texture(godot::String(virtual_path)));
        godot::Dictionary info = probe->get_entry_info(godot::String(virtual_path));
        GDEX_EXPECT_TRUE((bool)info.get("has_gpu_metadata", false));
        GDEX_EXPECT_EQ((int64_t)info.get("mip_count", 0), (int64_t)kGpuMipCount);
    }

    // Old-path probe removed after diagnosis (the control in
    // graceful_memory_fallback covers the shared path permanently):
    // resolve works for both entries; the DMA miss was vacuous
    // completion, not resolution.
    godot::PackedStringArray paths;
    paths.push_back(godot::String(virtual_path));
    // Phase wrappers: neither block contains GDEX macros (which must
    // propagate), so a C++ exception here is named instead of surfacing
    // as "unexpected exception in test body".
    uint64_t tag = 0;
    try {
        tag = cache->prewarm_gpu_async(paths, 16, true);
    } catch (const std::exception &e) {
        GDEX_FAIL(std::string("prewarm_gpu_async threw: ") + e.what());
    } catch (...) {
        GDEX_FAIL("prewarm_gpu_async threw an unknown exception.");
    }
    GDEX_ASSERT_TRUE(tag != 0);
    // Denominator contract proof: the expected total must be established upfront
    // at frame 0, before driving completion, so progress ratio never regresses.
    {
        godot::Vector2i initial_prog = ds->get_prewarm_progress(tag);
        GDEX_EXPECT_GT((int64_t)initial_prog.y, (int64_t)0);
    }

    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 20000;
    bool settled = false;
    try {
        settled = drive_prewarm_to_settled(ds, cache, tag, deadline);
    } catch (const std::exception &e) {
        GDEX_FAIL(std::string("drive loop threw: ") + e.what());
    } catch (...) {
        GDEX_FAIL("drive loop threw an unknown exception.");
    }
    if (!settled) {
        godot::Vector2i progress = ds->get_prewarm_progress(tag);
        GDEX_FAIL(std::string("GPU prewarm tag did not settle (done=") +
                std::to_string(progress.x) + ", total=" + std::to_string(progress.y) + ").");
    }
    // Enqueue proof: the render-thread chunk must have attached at least
    // one request (total>0). total==0 with a settled tag means the chunk
    // never allocated/enqueued (vacuous completion).
    {
        godot::Vector2i progress = ds->get_prewarm_progress(tag);
        GDEX_EXPECT_GT((int64_t)progress.y, (int64_t)0);
        GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(tag), 0);
    }

    godot::Ref<godot::Texture2D> tex =
            cache->get_cached(godot::String(virtual_path), godot::DSTextureCache::CACHE_MODE_REUSE);
    GDEX_ASSERT_TRUE(tex.is_valid());
    GDEX_EXPECT_EQ(tex->get_width(), kGpuTexWidth);
    GDEX_EXPECT_EQ(tex->get_height(), kGpuTexHeight);
    // DMA proof: the resident must be a VRAM-backed Texture2DRD, not the
    // ImageTexture the CPU fallback would have built.
    godot::Ref<godot::Texture2DRD> gpu = tex;
    GDEX_EXPECT_TRUE(gpu.is_valid());
}

GDEX_TEST_T(gpu_prewarm, linear_formats_fill_vram_texture, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
        GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver)");
    }
    if (!godot::DSPreloader::is_gpu_prewarm_available()) {
        GDEX_SKIP("GPU prewarm unavailable (no live D3D12 RenderingDevice)");
    }
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("no RenderingDevice for the readback check");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    struct LinearCase {
        const char *path;
        godot::Image::Format format;
        const char *label;
    };
    const LinearCase cases[] = {
        { "user://gdextest_gpu_prewarm_bc5.dstex", godot::Image::FORMAT_RGTC_RG, "BC5" },
        { "user://gdextest_gpu_prewarm_bc4.dstex", godot::Image::FORMAT_RGTC_R, "BC4" },
    };
    const int case_count = static_cast<int>(sizeof(cases) / sizeof(cases[0]));

    ctx.add_teardown([ds]() {
        // Shutdown FIRST: a batch may still own the staged file handle, and a
        // failed remove poisons later suites through a stale fixture.
        ds->shutdown_ds();
        remove_abs("user://gdextest_gpu_prewarm_bc5.dstex");
        remove_abs("user://gdextest_gpu_prewarm_bc4.dstex");
        godot::DSPreloader::shutdown();
    });

    godot::PackedStringArray paths;
    godot::PackedByteArray expected[2];
    for (int i = 0; i < case_count; ++i) {
        expected[i] = make_linear_raw(cases[i].format, kLinearTexWidth, kLinearTexHeight, kLinearMipCount);
        if (!write_linear_dstex(cases[i].path, cases[i].format, kLinearTexWidth, kLinearTexHeight, kLinearMipCount, expected[i])) {
            GDEX_ABORT_TEST("linear .dstex staging failed");
        }
        paths.push_back(godot::String(cases[i].path));
    }

    uint64_t tag = 0;
    try {
        tag = cache->prewarm_gpu_async(paths, 16, true);
    } catch (const std::exception &e) {
        GDEX_FAIL(std::string("prewarm_gpu_async threw: ") + e.what());
    } catch (...) {
        GDEX_FAIL("prewarm_gpu_async threw an unknown exception.");
    }
    GDEX_ASSERT_TRUE(tag != 0);
    {
        // Both payloads must be counted up front; a memory-path fallback would
        // also report this total, so this only guards the denominator contract.
        godot::Vector2i initial_prog = ds->get_prewarm_progress(tag);
        GDEX_EXPECT_EQ(static_cast<int64_t>(initial_prog.y), static_cast<int64_t>(case_count));
    }

    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 20000;
    if (!drive_prewarm_to_settled(ds, cache, tag, deadline)) {
        godot::Vector2i progress = ds->get_prewarm_progress(tag);
        GDEX_FAIL(std::string("linear prewarm tag did not settle (done=") + std::to_string(progress.x) +
                ", total=" + std::to_string(progress.y) + ").");
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DSPreloader::get_unbuilt_prewarm_count(tag)), static_cast<int64_t>(0));

    for (int i = 0; i < case_count; ++i) {
        const std::string label(cases[i].label);
        godot::Ref<godot::Texture2D> tex =
                cache->get_cached(godot::String(cases[i].path), godot::DSTextureCache::CACHE_MODE_REUSE);
        GDEX_ASSERT_TRUE(tex.is_valid());
        GDEX_EXPECT_EQ(static_cast<int64_t>(tex->get_width()), static_cast<int64_t>(kLinearTexWidth));
        GDEX_EXPECT_EQ(static_cast<int64_t>(tex->get_height()), static_cast<int64_t>(kLinearTexHeight));
        // The widened gate must put this entry on the GPU path: before the
        // change the identical payload completed as an ImageTexture.
        godot::Ref<godot::Texture2DRD> gpu = tex;
        GDEX_EXPECT_TRUE(gpu.is_valid());
        if (gpu.is_null()) {
            continue;
        }
        godot::RID texture_rid = gpu->get_texture_rd_rid();
        GDEX_EXPECT_TRUE(texture_rid.is_valid());
        if (!texture_rid.is_valid()) {
            continue;
        }
        godot::Ref<godot::RDTextureFormat> rd_format = rd->texture_get_format(texture_rid);
        GDEX_EXPECT_TRUE(rd_format.is_valid());
        if (rd_format.is_valid()) {
            GDEX_EXPECT_EQ(static_cast<int64_t>(rd_format->get_format()),
                    static_cast<int64_t>(godot::DSTex::godot_format_to_rd_format(static_cast<int>(cases[i].format))));
            GDEX_EXPECT_EQ(static_cast<int64_t>(rd_format->get_mipmaps()), static_cast<int64_t>(kLinearMipCount));
        }
        // Content proof: DMA-filled bytes must equal the natural payload that
        // went into the fixture (padded rows trimmed).
        godot::PackedByteArray readback = rd->texture_get_data(texture_rid, 0);
        std::string detail;
        if (!readback_matches_natural(readback, expected[i], cases[i].format,
                    kLinearTexWidth, kLinearTexHeight, kLinearMipCount, detail)) {
            GDEX_FAIL(label + " GPU readback mismatch (readback=" + std::to_string(readback.size()) + "): " + detail);
        }
    }
}

GDEX_TEST_T(gpu_prewarm, bc7_fill_vram_texture, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
        GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver)");
    }
    if (!godot::DSPreloader::is_gpu_prewarm_available()) {
        GDEX_SKIP("GPU prewarm unavailable (no live D3D12 RenderingDevice)");
    }
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("no RenderingDevice for the readback check");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *path = "user://gdextest_gpu_prewarm_bc7.dstex";
    ctx.add_teardown([ds, path]() {
        ds->shutdown_ds();
        remove_abs(path);
        godot::DSPreloader::shutdown();
    });

    // Arbitrary bytes are valid as BC7 texture data (the GPU samples, it does
    // not decode on upload), and they make any misplaced byte visible.
    godot::PackedByteArray expected =
            make_linear_raw(godot::Image::FORMAT_BPTC_RGBA, kLinearTexWidth, kLinearTexHeight, kLinearMipCount);
    if (!write_linear_dstex(path, godot::Image::FORMAT_BPTC_RGBA, kLinearTexWidth, kLinearTexHeight,
                kLinearMipCount, expected)) {
        GDEX_ABORT_TEST("BC7 .dstex staging failed");
    }

    godot::PackedStringArray paths;
    paths.push_back(godot::String(path));
    uint64_t tag = 0;
    try {
        tag = cache->prewarm_gpu_async(paths, 16, true);
    } catch (const std::exception &e) {
        GDEX_FAIL(std::string("prewarm_gpu_async threw: ") + e.what());
    } catch (...) {
        GDEX_FAIL("prewarm_gpu_async threw an unknown exception.");
    }
    GDEX_ASSERT_TRUE(tag != 0);

    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 20000;
    if (!drive_prewarm_to_settled(ds, cache, tag, deadline)) {
        godot::Vector2i progress = ds->get_prewarm_progress(tag);
        GDEX_FAIL(std::string("BC7 prewarm tag did not settle (done=") + std::to_string(progress.x) +
                ", total=" + std::to_string(progress.y) + ").");
    }
    GDEX_EXPECT_EQ(static_cast<int64_t>(godot::DSPreloader::get_unbuilt_prewarm_count(tag)), static_cast<int64_t>(0));

    godot::Ref<godot::Texture2D> tex = cache->get_cached(godot::String(path), godot::DSTextureCache::CACHE_MODE_REUSE);
    GDEX_ASSERT_TRUE(tex.is_valid());
    GDEX_EXPECT_EQ(static_cast<int64_t>(tex->get_width()), static_cast<int64_t>(kLinearTexWidth));
    GDEX_EXPECT_EQ(static_cast<int64_t>(tex->get_height()), static_cast<int64_t>(kLinearTexHeight));

    godot::Ref<godot::Texture2DRD> gpu = tex;
    GDEX_EXPECT_TRUE(gpu.is_valid());
    if (gpu.is_null()) {
        return;
    }
    godot::RID texture_rid = gpu->get_texture_rd_rid();
    GDEX_EXPECT_TRUE(texture_rid.is_valid());
    if (!texture_rid.is_valid()) {
        return;
    }
    godot::Ref<godot::RDTextureFormat> rd_format = rd->texture_get_format(texture_rid);
    GDEX_EXPECT_TRUE(rd_format.is_valid());
    if (rd_format.is_valid()) {
        GDEX_EXPECT_EQ(static_cast<int64_t>(rd_format->get_format()),
                static_cast<int64_t>(godot::DSTex::godot_format_to_rd_format(static_cast<int>(godot::Image::FORMAT_BPTC_RGBA))));
        GDEX_EXPECT_EQ(static_cast<int64_t>(rd_format->get_mipmaps()), static_cast<int64_t>(kLinearMipCount));
    }

    godot::PackedByteArray readback = rd->texture_get_data(texture_rid, 0);
    std::string detail;
    if (!readback_matches_natural(readback, expected, godot::Image::FORMAT_BPTC_RGBA,
                kLinearTexWidth, kLinearTexHeight, kLinearMipCount, detail)) {
        GDEX_FAIL("BC7 GPU readback mismatch (readback=" + std::to_string(readback.size()) + "): " + detail);
    }
}

GDEX_TEST_T(gpu_prewarm, graceful_memory_fallback, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *dstex_path = "user://gdextest_gpu_prewarm_mem.dstex";
    const char *dspkg_path = "user://gdextest_gpu_prewarm_mem.dspkg";
    const char *virtual_path = "res://gdextest_gpu_prewarm_mem.png";
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        ps->set_setting("directstorage/force_archive_resolution", true);
    }
    ctx.add_teardown([ds, ps]() {
        if (ps) {
            ps->set_setting("directstorage/force_archive_resolution", false);
        }
        // Shutdown first (see dma test): release backend file handles
        // before deleting the archive they may hold open.
        ds->shutdown_ds();
        remove_abs("res://textures.dspkg");
        remove_abs("user://gdextest_gpu_prewarm_mem.dstex");
        remove_abs("user://gdextest_gpu_prewarm_mem.dspkg");
        godot::DSPreloader::shutdown();
    });
    if (!stage_gpu_dspkg(dstex_path, dspkg_path, virtual_path, "res://gdextest_gpu_prewarm_ctl.png")) {
        GDEX_ABORT_TEST("textures.dspkg staging failed");
    }

    // Staging proof (no backend involved): the virtual path resolves with
    // v2 GPU metadata. If this fails, the fixture (not the prewarm path)
    // is broken.
    {
        godot::Ref<godot::DSPkg> probe;
        probe.instantiate();
        GDEX_ASSERT_TRUE(probe.is_valid());
        GDEX_ASSERT_EQ((int64_t)probe->open(godot::String("res://textures.dspkg")), (int64_t)godot::OK);
        GDEX_EXPECT_TRUE(probe->has_texture(godot::String(virtual_path)));
        godot::Dictionary info = probe->get_entry_info(godot::String(virtual_path));
        GDEX_EXPECT_TRUE((bool)info.get("has_gpu_metadata", false));
        GDEX_EXPECT_EQ((int64_t)info.get("mip_count", 0), (int64_t)kGpuMipCount);
    }

    // Loader cross-check (loud errors): the same virtual asset through the
    // sync CPU path. Pass => asset + backend read/decompress are fine and
    // the fault is in the prewarm completion path; fail => asset/backend.
    {
        godot::Ref<godot::DSResourceFormatLoader> loader;
        loader.instantiate();
        GDEX_ASSERT_TRUE(loader.is_valid());
        godot::Variant lres = loader->_load(godot::String(virtual_path), godot::String(""),
                false, godot::ResourceFormatLoader::CACHE_MODE_IGNORE);
        godot::Ref<godot::Texture2D> ltex(lres);
        GDEX_EXPECT_TRUE(ltex.is_valid());
    }

    // Old-path control: the pre-existing prewarm_async() on the second
    // archive entry. Hit => shared poll/store works and the fault is in
    // the new prewarm_gpu_async split; miss => shared/backend issue.
    const char *control_path = "res://gdextest_gpu_prewarm_ctl.png";
    godot::PackedStringArray ctl_paths;
    ctl_paths.push_back(godot::String(control_path));
    const uint64_t ctl_tag = cache->prewarm_async(ctl_paths);
    GDEX_ASSERT_TRUE(ctl_tag != 0);
    const uint64_t ctl_deadline = godot::Time::get_singleton()->get_ticks_msec() + 20000;
    GDEX_ASSERT_TRUE(drive_prewarm_to_settled(ds, cache, ctl_tag, ctl_deadline));
    godot::Ref<godot::Texture2D> ctl_tex =
            cache->get_cached(godot::String(control_path), godot::DSTextureCache::CACHE_MODE_REUSE);
    // Post-drive state split: unbuilt>0 => poll never consumed the entry;
    // unbuilt==0 + miss => entry consumed but store/get lost it.
    {
        godot::Vector2i progress = ds->get_prewarm_progress(ctl_tag);
        GDEX_EXPECT_EQ((int64_t)progress.y, (int64_t)2);
        GDEX_EXPECT_EQ((int64_t)progress.x, (int64_t)2);
        GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(ctl_tag), 0);
    }
    GDEX_EXPECT_TRUE(ctl_tex.is_valid());

    // Backend probe (no preloader involved): one v2 mip request straight
    // at the staged archive, pumped like stream_maintenance. Proves the
    // fallback backend can read + GDeflate-decompress archive spans, and
    // surfaces the backend error_message when it cannot.
    {
        godot::Ref<godot::DSPkg> probe;
        probe.instantiate();
        GDEX_ASSERT_TRUE(probe.is_valid());
        GDEX_ASSERT_EQ((int64_t)probe->open(godot::String("res://textures.dspkg")), (int64_t)godot::OK);
        godot::Dictionary info = probe->get_entry_info(godot::String(virtual_path));
        godot::PackedInt64Array offs = info.get("mip_offsets", godot::PackedInt64Array());
        godot::PackedInt32Array sizes = info.get("mip_sizes", godot::PackedInt32Array());
        godot::PackedInt32Array uncomps = info.get("mip_uncompressed_sizes", godot::PackedInt32Array());
        GDEX_ASSERT_TRUE(offs.size() > 0 && sizes.size() > 0 && uncomps.size() > 0);
        godot::String archive = info.get("archive_path", godot::String("res://textures.dspkg"));
        godot::Ref<godot::DSRequest> req;
        req.instantiate();
        req->set_file_path(archive);
        req->set_offset((uint64_t)offs[0]);
        req->set_size((uint32_t)sizes[0]);
        req->set_uncompressed_size((uint32_t)uncomps[0]);
        req->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
        req->get_destination_buffer_ref().resize(uncomps[0]);
        GDEX_ASSERT_TRUE(ds->enqueue_request(req, true));
        GDEX_ASSERT_EQ((int64_t)ds->flush_stream_async(), (int64_t)godot::OK);
        bool terminal = false;
        for (int i = 0; i < 100; ++i) {
            ds->flush_stream_async();
            ds->wait_for_completion(500);
            godot::OS::get_singleton()->delay_msec(50);
            ds->flush_stream_async();
            const int64_t st = static_cast<int64_t>(req->get_status());
            if (st == static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED) ||
                    st == static_cast<int64_t>(godot::DSRequest::STATUS_FAILED)) {
                terminal = true;
                break;
            }
        }
        GDEX_ASSERT_TRUE(terminal);
        if (static_cast<int64_t>(req->get_status()) != static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED)) {
            GDEX_FAIL(std::string("archive mip request failed: ") +
                    ::gdextest::to_std_string(req->get_error_message()));
        }
        GDEX_EXPECT_GT(req->get_destination_buffer().size(), 0);
    }

    // prefer_gpu=false forces the memory path on any driver (including
    // headless CI): identical contract, ImageTexture result.
    godot::PackedStringArray paths;
    paths.push_back(godot::String(virtual_path));
    const uint64_t tag = cache->prewarm_gpu_async(paths, 16, false);
    GDEX_ASSERT_TRUE(tag != 0);
    {
        godot::Vector2i initial_prog = ds->get_prewarm_progress(tag);
        GDEX_EXPECT_GT((int64_t)initial_prog.y, (int64_t)0);
    }
    // Resolve proof: the payload must be tracked (not skipped as
    // unresolvable) before driving completion.
    GDEX_EXPECT_GT(godot::DSPreloader::get_unbuilt_prewarm_count(tag), 0);

    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 20000;
    if (!drive_prewarm_to_settled(ds, cache, tag, deadline)) {
        godot::Vector2i progress = ds->get_prewarm_progress(tag);
        GDEX_FAIL(std::string("memory-fallback prewarm tag did not settle (done=") +
                std::to_string(progress.x) + ", total=" + std::to_string(progress.y) +
                ", unbuilt=" + std::to_string(godot::DSPreloader::get_unbuilt_prewarm_count(tag)) + ").");
    }

    godot::Ref<godot::Texture2D> tex =
            cache->get_cached(godot::String(virtual_path), godot::DSTextureCache::CACHE_MODE_REUSE);
    GDEX_ASSERT_TRUE(tex.is_valid());
    GDEX_EXPECT_EQ(tex->get_width(), kGpuTexWidth);
    GDEX_EXPECT_EQ(tex->get_height(), kGpuTexHeight);
    godot::Ref<godot::ImageTexture> img_tex = tex;
    GDEX_ASSERT_TRUE(img_tex.is_valid());
    godot::Ref<godot::Image> img = img_tex->get_image();
    GDEX_ASSERT_TRUE(img.is_valid());
    // Partial chain (2 of 7) arrives as base-only without mipmaps (#0039).
    GDEX_EXPECT_FALSE(img->has_mipmaps());
    GDEX_EXPECT_EQ(img->get_data().size(), kGpuBaseSize);
}

GDEX_TEST_T(gpu_prewarm, shutdown_while_prewarm_in_flight_drains_safely, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *dstex_path = "user://gdextest_gpu_prewarm_drain.dstex";
    const char *dspkg_path = "user://gdextest_gpu_prewarm_drain.dspkg";
    const char *virtual_path = "res://gdextest_gpu_prewarm_drain.png";
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        ps->set_setting("directstorage/force_archive_resolution", true);
        ps->set_setting("directstorage/shutdown_drain_timeout_ms", 5000);
    }
    ctx.add_teardown([ds, ps]() {
        if (ps) {
            ps->set_setting("directstorage/force_archive_resolution", false);
        }
        ds->shutdown_ds();
        remove_abs("res://textures.dspkg");
        remove_abs("user://gdextest_gpu_prewarm_drain.dstex");
        remove_abs("user://gdextest_gpu_prewarm_drain.dspkg");
        godot::DSPreloader::shutdown();
    });
    if (!stage_gpu_dspkg(dstex_path, dspkg_path, virtual_path, "res://gdextest_gpu_prewarm_drain_ctl.png")) {
        GDEX_ABORT_TEST("textures.dspkg staging failed");
    }

    godot::PackedStringArray paths;
    paths.push_back(godot::String(virtual_path));
    const uint64_t tag = cache->prewarm_gpu_async(paths, 16, false);
    GDEX_ASSERT_TRUE(tag != 0);

    // Immediately trigger shutdown while prewarm batch is in flight / queued.
    // Must drain queue safely without access violation, deadlock, or crash.
    ds->shutdown_ds();
    GDEX_EXPECT_FALSE(ds->is_initialized());
}

GDEX_TEST_T(gpu_prewarm, cancellation_while_gpu_in_flight_retires_safely, TAG_INTEGRATION | TAG_SLOW) {
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *dstex_path = "user://gdextest_gpu_cancel_drain.dstex";
    const char *dspkg_path = "user://gdextest_gpu_cancel_drain.dspkg";
    const char *virtual_path = "res://gdextest_gpu_cancel_drain.png";
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        ps->set_setting("directstorage/force_archive_resolution", true);
    }
    ctx.add_teardown([ds, ps]() {
        if (ps) {
            ps->set_setting("directstorage/force_archive_resolution", false);
        }
        ds->shutdown_ds();
        remove_abs("res://textures.dspkg");
        remove_abs("user://gdextest_gpu_cancel_drain.dstex");
        remove_abs("user://gdextest_gpu_cancel_drain.dspkg");
        godot::DSPreloader::shutdown();
    });
    if (!stage_gpu_dspkg(dstex_path, dspkg_path, virtual_path, "res://gdextest_gpu_cancel_drain_ctl.png")) {
        GDEX_ABORT_TEST("textures.dspkg staging failed");
    }

    godot::PackedStringArray paths;
    paths.push_back(godot::String(virtual_path));
    const uint64_t tag = cache->prewarm_gpu_async(paths, 16, true);
    GDEX_ASSERT_TRUE(tag != 0);

    // Immediately cancel the in-flight GPU prewarm
    ds->cancel_prewarm(tag);

    // Drive frames to pump completion and orphan collection. The context pump is
    // part of it: a cancelled GPU prewarm keeps its parent request in flight until
    // the batch fence retires, so the per-tag report is finalized by the next
    // collect_completed_results() (DSRuntime does this every frame) rather than by
    // cancel_prewarm() alone.
    const uint64_t deadline = godot::Time::get_singleton()->get_ticks_msec() + 10000;
    while (godot::Time::get_singleton()->get_ticks_msec() < deadline) {
        ds->collect_completed_results();
        cache->poll_completed(tag, 16, 4000);
        if (ds->is_prewarm_complete(tag) && godot::DSPreloader::get_unbuilt_prewarm_count(tag) == 0) {
            break;
        }
        godot::OS::get_singleton()->delay_usec(2000);
    }

    // Must have settled without leaking pending entries or hanging
    GDEX_EXPECT_TRUE(ds->is_prewarm_complete(tag));
    GDEX_EXPECT_EQ(godot::DSPreloader::get_unbuilt_prewarm_count(tag), 0);
}

GDEX_TEST_T(gpu_prewarm, cancel_in_flight_keeps_parent_and_rid_owned, TAG_INTEGRATION | TAG_SLOW) {
#if !defined(DEBUG_ENABLED) && !defined(DEV_ENABLED)
    (void)ctx;
    GDEX_SKIP("The prewarm tracking seam is compiled out of release builds.");
#else
    // Cancelling an in-flight GPU prewarm must not let the
    // destination RID be freed while DirectStorage is still writing into it.
    // The preloader decides that from the PARENT request's is_gpu_in_flight(), so
    // the parent flag and the chunked-state lifetime are the things under test.
    if (!godot::ClassDB::class_exists("DSContext") || !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("DSContext/DSTextureCache classes unavailable");
    }
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
    }
    godot::DSContext *ds = require_singleton_context(ctx);
    if (!ds->initialize_ds() || !ds->is_initialized()) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }
    if (!ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
        GDEX_SKIP("D3D12 GPU decompression unavailable (headless or non-D3D12 driver)");
    }
    godot::DSTextureCache *cache = new_test_cache(ctx);
    godot::DSPreloader::shutdown();

    const char *dstex_path = "user://gdextest_crit01_cancel_inflight.dstex";
    const char *dspkg_path = "user://gdextest_crit01_cancel_inflight.dspkg";
    const char *virtual_path = "res://gdextest_crit01_cancel_inflight.png";
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    if (ps) {
        ps->set_setting("directstorage/force_archive_resolution", true);
    }
    ctx.add_teardown([ds, ps]() {
        if (ps) {
            ps->set_setting("directstorage/force_archive_resolution", false);
        }
        ds->shutdown_ds();
        remove_abs("res://textures.dspkg");
        remove_abs("user://gdextest_crit01_cancel_inflight.dstex");
        remove_abs("user://gdextest_crit01_cancel_inflight.dspkg");
        godot::DSPreloader::shutdown();
    });
    if (!stage_large_gpu_dspkg(dstex_path, dspkg_path, virtual_path, "res://gdextest_crit01_cancel_ctl.png")) {
        GDEX_ABORT_TEST("textures.dspkg staging failed");
    }

    godot::PackedStringArray paths;
    paths.push_back(godot::String(virtual_path));
    const uint64_t tag = cache->prewarm_gpu_async(paths, 16, true);
    GDEX_ASSERT_TRUE(tag != 0);

    // Drive until the tracked entry exists and its parent request is live.
    godot::RID rid;
    godot::Ref<godot::DSRequest> parent;
    const uint64_t observe_deadline = godot::Time::get_singleton()->get_ticks_msec() + 15000;
    while (godot::Time::get_singleton()->get_ticks_msec() < observe_deadline) {
        godot::DSPreloader::pump_gpu_prewarm();
        ds->flush_stream_async();
        rid = godot::DSPreloader::_get_gpu_prewarm_rid_for_testing(tag);
        parent = godot::DSPreloader::_get_gpu_prewarm_request_for_testing(tag);
        if (rid.is_valid() && parent.is_valid()) {
            break;
        }
        ds->wait_for_completion(50);
        godot::OS::get_singleton()->delay_usec(2000);
    }
    GDEX_ASSERT_TRUE(rid.is_valid());
    GDEX_ASSERT_TRUE(parent.is_valid());

    // 1) A GPU-texture chunked parent must report the hardware window. Before the
    //    fix the parent was never marked, so the preloader believed a cancel had
    //    already retired a batch that was still writing.
    GDEX_ASSERT_TRUE(parent->is_gpu_in_flight());

    // 2) Cancellation cannot make it look terminal while the fence is unsignaled.
    ds->cancel_prewarm(tag);
    GDEX_EXPECT_TRUE(parent->is_gpu_in_flight());
    GDEX_EXPECT_FALSE(godot::DSPreloader::is_terminal(parent));

    // 3) ... which is what keeps the tracked entry alive. A pre-fix build consumes
    //    it here (get_unbuilt_prewarm_count drops to 0) and hands the destination
    //    RID to the orphan free path.
    cache->poll_completed(tag, 16, 4000);
    GDEX_EXPECT_TRUE(godot::DSPreloader::get_unbuilt_prewarm_count(tag) > 0);

    // The RID may only disappear once the hardware is done with it.
    GDEX_EXPECT_TRUE(rd->texture_is_valid(rid) || !ds->is_resource_in_flight(rid));

    // 4) Retirement must still finish: no permanently stuck in-flight flag.
    const uint64_t retire_deadline = godot::Time::get_singleton()->get_ticks_msec() + 15000;
    while (godot::Time::get_singleton()->get_ticks_msec() < retire_deadline) {
        cache->poll_completed(tag, 16, 4000);
        godot::DSPreloader::pump_gpu_prewarm();
        ds->collect_completed_results();
        if (!ds->is_resource_in_flight(rid) && !parent->is_gpu_in_flight()) {
            break;
        }
        ds->wait_for_completion(20);
        godot::OS::get_singleton()->delay_usec(2000);
    }
    GDEX_EXPECT_FALSE(parent->is_gpu_in_flight());
    GDEX_EXPECT_TRUE(godot::DSPreloader::is_terminal(parent));

    // Deliberate leak guard: never free under live DMA, but do not leave the RID
    // behind either.
    if (rd->texture_is_valid(rid)) {
        if (ds->is_resource_in_flight(rid)) {
            GDEX_FAIL("destination RID stayed in flight past the retirement deadline");
        } else {
            rd->free_rid(rid);
        }
    }
#endif
}

