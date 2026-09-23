// tests/async_streaming_tests.cpp
//
// Async live-engine suite for `prewarm_async` batch
// submission/completion across frames.
//
// Contract macro form (include/gdextest/registry.h:96-104): the tagged async
// macro takes ONE combined bitmask and does NOT auto-add TAG_ASYNC, so the
// mask below repeats TAG_ASYNC explicitly.
//
// Bodies are C++20 coroutines: waits use `co_await ctx.await_frames(n)` with
// the framework default 30s per-wait timeout ([gdextest.test] timeout_ms;
// whole-test budget 120s via isolate_timeout_sec). A stalled wait therefore
// reports fail-with-timed-out via the runner (never hangs CI); this suite
// additionally caps its pump loop and reports the wait chain instead of
// raising timeouts.
//
// Linkage note: compile-in decided, consumer_extension NOT
// configured: DSContext / DSTextureCache / DSPreloader implementations are
// NOT compiled into the test library (.gdextest.toml [gdextest.tests]
// sources), so this file must not call their methods statically (that would
// be an undefined symbol at link). All extension access is dynamic via
// Engine singletons + Object::call, which links against godot-cpp only.
// When the consumer extension is absent the suite SKIPS (precondition), and
// exercises the live path once the orchestrator wires the extension in.
//
// Signal note: the contract names a `prewarm_finished` signal, but no such
// signal exists in src/ (verified: only DSContext `batch_completed(fence,
// count)` / `batch_failed(code, message)` in src/core/ds_context.cpp). Per
// the contract's "where applicable", ctx.signals() SignalMonitor watches the
// real signals instead: `batch_failed` with expected-count 0 (evaluated),
// `batch_completed` for emission counting (diagnostic only — completion
// timing asserts are evaluated in D3D12 backend tests).
//
// Mirrors the authoritative GDScript coverage in
// project/headless_tests/test_async_streaming.gd (Phase 3 import-remap leg)
// without staging files: it prewarms the single fixture asset shipped to the
// fixture (project/test_assets/test.png) and asserts convergence, progress,
// and cache presence. All asserts are backend-agnostic; D3D12
// timing-sensitive asserts are deferred to D3D12 backend tests.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "formats/dstex_format.h"  // DSTex::write_to_file

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/image.hpp>   // Image format constants (FORMAT_RGBA8)
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <string>

namespace {

// Same bound as MAX_PUMP_FRAMES in test_async_streaming.gd: a tag that has
// not converged by then is reported with its wait chain, never awaited
// past the isolate budget.
constexpr int kMaxPumpFrames = 600;

// DSTextureCache::CACHE_MODE_REUSE (src/integration/ds_texture_cache.h).
// Passed as a raw int to stay link-safe (no static reference to the class).
constexpr int kCacheModeReuse = 1;

// Single-texture set keeps the suite fast and backend-agnostic.
constexpr int kSetSize = 1;

// Dynamic singleton lookup: returns null when the consumer extension is not
// loaded (fixture without [gdextest.consumer_extension]). Never throws.
godot::Object *engine_extension_singleton(godot::Engine *engine, const char *name) {
    if (engine == nullptr || !engine->has_singleton(name)) {
        return nullptr;
    }
    godot::Variant holder = engine->get_singleton(name);
    if (holder.get_type() == godot::Variant::NIL) {
        return nullptr;
    }
    godot::Object *obj = holder;
    return obj;
}

bool as_bool(const godot::Variant &v) {
    return static_cast<bool>(v);
}

// Pattern payload for DSTex writer (mirrors resource_loader_tests.cpp helpers).
godot::PackedByteArray make_pattern_payload(int64_t p_size) {
    godot::PackedByteArray payload;
    payload.resize(p_size);
    for (int64_t i = 0; i < p_size; ++i) {
        payload[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    }
    return payload;
}

// Writes a loose v1 .dstex via the real DSTex writer. Returns false on failure.
// Uses small dimensions to keep the file tiny: 64x4 RGBA8 single mip.
bool write_loose_dstex(const char *p_path) {
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    if (dstex.is_null()) {
        return false;
    }
    godot::Error err = dstex->write_to_file(
            godot::String(p_path), make_pattern_payload(1024),
            64, 4, godot::Image::FORMAT_RGBA8, 1, 1);
    return err == godot::OK;
}

void remove_user_file(const char *p_path) {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_valid()) {
        godot::Error err = dir->remove(godot::String(p_path));
        (void)err; // ignore not-found
    }
}

} // namespace

GDEX_TEST_ASYNC_T(streaming, prewarm_completes, TAG_ASYNC | TAG_INTEGRATION | TAG_SLOW) {
    godot::Engine *engine = godot::Engine::get_singleton();
    if (engine == nullptr) {
        GDEX_SKIP("no live engine; async frame pump requires the editor host");
    }
    if (!godot::ClassDB::class_exists("DSContext") ||
            !godot::ClassDB::class_exists("DSTextureCache")) {
        GDEX_SKIP("consumer extension not loaded (DSContext/DSTextureCache classes absent)");
    }
    godot::Object *ds = engine_extension_singleton(engine, "DSContext");
    godot::Object *cache = engine_extension_singleton(engine, "DSTextureCache");
    if (ds == nullptr || cache == nullptr) {
        GDEX_SKIP("consumer extension singletons unavailable (DSContext/DSTextureCache)");
    }

    // Backend precondition: without an initialized DSContext there is no wait
    // chain to exercise. (The headless GDScript suite owns the hard failure
    // for init; here a skip keeps the report unambiguous.)
    const bool init_ok = as_bool(ds->call("initialize_ds"));
    const bool live = as_bool(ds->call("is_initialized"));
    if (!init_ok || !live) {
        GDEX_SKIP("DSContext backend unavailable (initialize_ds failed)");
    }

    // Create a native .dstex file at runtime to bypass ResourceLoader cache:
    // - DSPreloader::prewarm_async skips any path that has_cached(path)=true
    // - The PNG fixture imports during warmup, so rl->has_cached("res://test_assets/...") is true
    // - No public API exists to evict ResourceLoader, so we create our own .dstex
    //   which never enters RL cache until load_time
    constexpr const char *kProbeDstex = "user://gdextest_prewarm_probe.dstex";
    if (!write_loose_dstex(kProbeDstex)) {
        GDEX_FAIL("failed to create probe .dstex file");
        co_return;
    }
    ctx.add_teardown([kProbeDstex]() { remove_user_file(kProbeDstex); });

    // Watch the real completion/failure signals. batch_failed must never
    // fire (expected 0, evaluated at the end); batch_completed is counted
    // for the wait-chain report only (evaluated in backend test suite).
    gdextest::SignalMonitor &monitor = ctx.signals();
    monitor.add(ds, "batch_completed");
    monitor.add(ds, "batch_failed", 0);

    godot::PackedStringArray paths;
    paths.append(godot::String(kProbeDstex));

    const uint64_t tag = cache->call("prewarm_async", paths);
    // NOTE: cache->prewarm_async delegates to DSPreloader::prewarm_async which:
    // 1) resolves paths + imports (.import/package remaps),
    // 2) skips anything already cached (hence our clear() below),
    // 3) issues a fresh tag via DSContext::prewarm_async(PackedStringArray()),
    // 4) expands each payload into N mip requests and calls DSContext::enqueue_request(req, true).
    // Each successful enqueue bumps the tag's completed counter when it finishes.
    if (tag == 0) {
        GDEX_FAIL("prewarm_async returned tag 0 for a non-empty path set");
        co_return;
    }
    ctx.add_teardown([ds, tag]() { ds->call("cancel_prewarm", tag); });

    // Clear both caches to ensure we don't short-circuit on existing data
    // (even though the .dstex was just created, poll_completed may have been
    // called implicitly during some earlier test). This makes prewarm perform
    // actual streaming work.
    cache->call("clear");

    // Pump the tag the way DSBatchManager._process does: harvest finished
    // batches, flush pending stream work, drain completed textures into the
    // cache, then yield one frame. Each wait uses the 30s framework default;
    // the loop cap below turns a stall into a wait-chain failure, never a
    // hang and never a blind timeout raise.
    int frames = 0;
    bool complete = false;
    while (frames < kMaxPumpFrames) {
        if (as_bool(ds->call("is_prewarm_complete", tag))) {
            complete = true;
            break;
        }
        if (as_bool(ds->call("is_batch_complete")) && as_bool(ds->call("has_pending_results"))) {
            ds->call("collect_completed_results");
        }
        if (as_bool(ds->call("has_stream_pending"))) {
            ds->call("flush_stream_async");
        }
        cache->call("poll_completed", tag);
        frames += 1;
        co_await ctx.await_frames(1);
    }
    // One last harvest so counters settle, mirroring _pump_until.
    if (as_bool(ds->call("is_batch_complete")) && as_bool(ds->call("has_pending_results"))) {
        ds->call("collect_completed_results");
    }
    cache->call("poll_completed", tag);

    if (!complete) {
        const godot::Vector2i stuck = ds->call("get_prewarm_progress", tag);
        const int64_t pending = cache->call("get_pending_prewarm_count", tag);
        const int64_t in_flight = ds->call("get_in_flight_async_batch_count");
        const int completed_signals = monitor.get_emission_count(ds, "batch_completed");
        GDEX_FAIL(std::string("prewarm tag ") + std::to_string(tag) + " did not converge in " +
                std::to_string(kMaxPumpFrames) + " frames; wait chain: progress=(" +
                std::to_string(stuck.x) + ", " + std::to_string(stuck.y) + ") pending=" +
                std::to_string(pending) + " in_flight_batches=" + std::to_string(in_flight) +
                " batch_completed_emissions=" + std::to_string(completed_signals));
        co_return;
    }

    // Convergence: progress reaches (N, N).
    const godot::Vector2i progress = ds->call("get_prewarm_progress", tag);
    GDEX_EXPECT_EQ(progress.x, kSetSize);
    GDEX_EXPECT_EQ(progress.y, kSetSize);

    // Textures present in the cache with intact dimensions (backend-agnostic:
    // no exact-size or ImageTexture-type asserts; those are D3D12-sensitive
    // and evaluated in D3D12 backend tests).
    for (int i = 0; i < paths.size(); i++) {
        godot::Variant tex_v = cache->call("get_cached", paths[i], kCacheModeReuse);
        godot::Object *tex_obj = tex_v;
        GDEX_EXPECT_TRUE(tex_obj != nullptr);
        if (tex_obj != nullptr) {
            const int64_t w = tex_obj->call("get_width");
            const int64_t h = tex_obj->call("get_height");
            GDEX_EXPECT_GT(w, 0);
            GDEX_EXPECT_GT(h, 0);
        }
    }
    const int64_t pending_left = cache->call("get_pending_prewarm_count", tag);
    GDEX_EXPECT_EQ(pending_left, 0);

    // No failure signal may have fired; drop the counting watch first so
    // evaluate() only judges batch_failed (expected 0).
    const int completed_count = monitor.get_emission_count(ds, "batch_completed");
    (void)completed_count;
    monitor.remove(ds, "batch_completed");
    GDEX_EXPECT_TRUE(monitor.evaluate());

    ds->call("cancel_prewarm", tag);
    co_return;
}
