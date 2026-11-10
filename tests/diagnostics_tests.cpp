// tests/diagnostics_tests.cpp
//
// Invariant: the profiler's Performance-monitor registration must not make an engine call while
// holding the mutex that every monitor callable takes. Every `_get_*_monitor` body opens with
// profiler_mutex, and the engine's read path (Performance.get_custom_monitor) evaluates the
// callable synchronously on the caller's thread, so holding profiler_mutex across
// add_custom_monitor()/remove_custom_monitor() puts an engine call inside our lock
// (GODOT_QUIRKS.md Q46).

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "core/ds_profiler.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

GDEX_TEST_T(diagnostics, monitor_registration_holds_no_extension_mutex, TAG_INTEGRATION) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
    // The seam parks the registering thread immediately before the first engine call. From the
    // test thread we then ask whether profiler_mutex is free at that instant: free is the
    // invariant, and a same-thread try_lock would be undefined behaviour, hence the second thread.
    godot::DSProfiler *profiler = godot::DSProfiler::get_singleton();
    if (profiler == nullptr) {
        GDEX_SKIP("DSProfiler singleton absent: real extension not loaded in this fixture.");
    }
    // Teardown runs after the body, so it only touches heap-free state: drop any hook and leave
    // the process with the monitors registered, wherever the body stopped.
    ctx.add_teardown([profiler]() {
        profiler->_debug_set_monitor_registration_hook_for_testing({});
        profiler->register_performance_monitors();
    });

    profiler->unregister_performance_monitors();

    std::atomic<bool> window_open{ false };
    std::atomic<bool> release_window{ false };
    // Self-releasing: a broken build must not hang the suite while a teardown wants the
    // transition mutex back.
    profiler->_debug_set_monitor_registration_hook_for_testing([&window_open, &release_window]() {
        window_open.store(true);
        const auto hook_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!release_window.load() && std::chrono::steady_clock::now() < hook_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    std::thread registrar([profiler]() { profiler->register_performance_monitors(); });

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!window_open.load() && std::chrono::steady_clock::now() < wait_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool entered = window_open.load();
    const bool mutex_free_across_engine_call =
            entered && profiler->_debug_profiler_mutex_is_free_for_testing();

    release_window.store(true);
    registrar.join();
    profiler->_debug_set_monitor_registration_hook_for_testing({});

    // Anti-vacuity: the transition must actually have registered, so the hook cannot fire a
    // second time for a re-registration.
    std::atomic<int> second_registration_hooks{ 0 };
    profiler->_debug_set_monitor_registration_hook_for_testing(
            [&second_registration_hooks]() { second_registration_hooks.fetch_add(1); });
    profiler->register_performance_monitors();
    profiler->_debug_set_monitor_registration_hook_for_testing({});

    GDEX_EXPECT_TRUE(entered);
    // Discriminator: with the engine call inside profiler_mutex the probe finds it taken.
    GDEX_EXPECT_TRUE(mutex_free_across_engine_call);
    GDEX_EXPECT_EQ(static_cast<int64_t>(second_registration_hooks.load()), static_cast<int64_t>(0));
#else
    (void)ctx;
    GDEX_SKIP("The monitor-registration seam is compiled out of release builds.");
#endif
}

// DSRuntime feeds the frame-time window through the registered DSProfiler
// singleton, so the bound surface and the aggregation are one contract: a
// missing binding silently disables the only metric that can report a hitch.
GDEX_TEST_T(diagnostics, frame_window_bindings_and_aggregation, TAG_INTEGRATION) {
    godot::DSProfiler *profiler = godot::DSProfiler::get_singleton();
    if (profiler == nullptr) {
        GDEX_SKIP("DSProfiler singleton absent: real extension not loaded in this fixture.");
    }
    GDEX_EXPECT_TRUE(profiler->has_method("record_frame"));
    GDEX_EXPECT_TRUE(profiler->has_method("begin_load_window"));
    GDEX_EXPECT_TRUE(profiler->has_method("end_load_window"));
    GDEX_EXPECT_TRUE(profiler->has_method("get_frame_window_stats"));

    // 60 idle frames define the baseline exactly: the ring keeps only the most
    // recent 60, so frames recorded by earlier suites cannot influence it.
    for (int i = 0; i < 60; i++) {
        profiler->call("record_frame", 0.010);
    }
    profiler->call("begin_load_window");
    for (int i = 0; i < 8; i++) {
        profiler->call("record_frame", 0.033);
    }
    profiler->call("end_load_window");

    const godot::Dictionary stats = profiler->call("get_frame_window_stats");
    GDEX_EXPECT_FALSE(static_cast<bool>(stats["active"]));
    GDEX_EXPECT_TRUE(std::fabs(static_cast<double>(stats["mean_frame_before_usec"]) - 10000.0) < 1e-6);
    GDEX_EXPECT_TRUE(std::fabs(static_cast<double>(stats["mean_frame_during_usec"]) - 33000.0) < 1e-6);
    GDEX_EXPECT_EQ(static_cast<int64_t>(stats["frame_count"]), static_cast<int64_t>(8));
    GDEX_EXPECT_TRUE(static_cast<int64_t>(stats["completed_windows"]) >= static_cast<int64_t>(1));
}
