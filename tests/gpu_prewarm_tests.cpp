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

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr int kGpuTexWidth = 64;
constexpr int kGpuTexHeight = 64;
constexpr int kGpuMipCount = 2;
// Base 16384 (64x64 RGBA8) + mip1 4096 (32x32 RGBA8) natural bytes.
constexpr int64_t kGpuRawSize = 20480;
constexpr int64_t kGpuBaseSize = 16384;

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
bool stage_gpu_dspkg(const char *p_dstex_path, const char *p_dspkg_path, const char *p_virtual_path, const char *p_control_path) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    if (dstex->write_to_file(godot::String(p_dstex_path), make_gpu_raw(),
                kGpuTexWidth, kGpuTexHeight, godot::Image::FORMAT_RGBA8, kGpuMipCount, 1) != godot::OK) {
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
        remove_abs("res://textures.dspkg");
        if (godot::DirAccess::copy_absolute(godot::String(p_dspkg_path), godot::String("res://textures.dspkg")) == godot::OK) {
            godot::Ref<godot::DSPkg> verify;
            verify.instantiate();
            if (!verify.is_null() && verify->open(godot::String("res://textures.dspkg")) == godot::OK &&
                    verify->has_texture(godot::String(p_virtual_path)) &&
                    verify->has_texture(godot::String(p_control_path))) {
                return true;
            }
        }
        if (attempt == 0) {
            godot::OS *os = godot::OS::get_singleton();
            if (os != nullptr) {
                os->delay_msec(500);
            }
        }
    }
    fprintf(stderr, "[gpu_prewarm] stage self-verify failed for %s + %s\n", p_virtual_path, p_control_path);
    return false;
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
        // Shutdown FIRST: the Win32 backend persistently caches open
        // IDStorageFile handles, which lock res://textures.dspkg on
        // Windows until shutdown releases them. Removing first fails
        // silently and poisons the next suite with a stale archive.
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
