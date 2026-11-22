#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"
#include "core/ds_context.h"
#include <godot_cpp/classes/project_settings.hpp>

// dsgd_extension/force_software_fallback must be a release-available selector
// for SoftwareFallback (Mode 3 of the benchmark matrix). The debug-only
// DSGD_BENCH_FORCE_FALLBACK env seam cannot be used in ExportedRelease, and
// --rendering-driver vulkan would confound the D3D12 A/B comparison.
//
// The ProjectSettings value is authoritative on every initialize_ds(), so the
// tests set the key (not just the DSContext setter) and restore it in teardown.

namespace {

constexpr const char *kSetting = "dsgd_extension/force_software_fallback";

} // namespace

GDEX_TEST_T(force_software_fallback, setting_is_registered_with_false_default, TAG_UNIT) {
    (void)ctx;
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    GDEX_ASSERT_TRUE(ps != nullptr);
    GDEX_ASSERT_TRUE(ps->has_setting(kSetting));
    GDEX_EXPECT_EQ(static_cast<bool>(ps->get_setting(kSetting, true)), false);

    godot::DSContext *ds = godot::DSContext::get_singleton();
    GDEX_ASSERT_TRUE(ds != nullptr);
    GDEX_EXPECT_FALSE(ds->get_force_software_fallback());
}

GDEX_TEST_T(force_software_fallback, true_selects_software_fallback_and_resets, TAG_INTEGRATION) {
    godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
    GDEX_ASSERT_TRUE(ps != nullptr);
    godot::DSContext *ds = godot::DSContext::get_singleton();
    GDEX_ASSERT_TRUE(ds != nullptr);

    const bool had_setting = ps->has_setting(kSetting);
    const godot::Variant prev_setting = had_setting ? ps->get_setting(kSetting) : godot::Variant();
    const bool was_initialized = ds->is_initialized();
    ctx.add_teardown([ps, had_setting, prev_setting, was_initialized]() {
        if (ps != nullptr) {
            if (had_setting) {
                ps->set_setting(kSetting, prev_setting);
            } else {
                ps->clear(kSetting);
            }
        }
        godot::DSContext *context = godot::DSContext::get_singleton();
        if (context == nullptr) {
            return;
        }
        context->set_force_software_fallback(false);
        if (context->is_initialized()) {
            context->shutdown_ds();
        }
        if (was_initialized) {
            (void)context->initialize_ds();
        }
    });

    // The setting, not just the in-memory setter, must drive the next init.
    ps->set_setting(kSetting, true);
    ds->set_force_software_fallback(true);
    if (ds->is_initialized()) {
        ds->shutdown_ds();
    }
    GDEX_ASSERT_TRUE(ds->initialize_ds());
    GDEX_EXPECT_TRUE(ds->get_force_software_fallback());
    GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_init_status()),
            static_cast<int64_t>(godot::DSContext::INIT_STATUS_SOFTWARE_FALLBACK));
    GDEX_EXPECT_EQ(gdextest::to_std_string(ds->get_active_backend_name()), std::string("SoftwareFallback"));
    GDEX_EXPECT_FALSE(ds->is_gpu_decompression_enabled());

    // Clearing it must restore the normal backend selection on a DirectStorage
    // D3D12 host. On other hosts the fallback remains correct, so the positive
    // Win32Backend assertion is conditional rather than a SKIP (the forced path
    // above is the behavior under test and always ran).
    ps->set_setting(kSetting, false);
    ds->set_force_software_fallback(false);
    ds->shutdown_ds();
    GDEX_ASSERT_TRUE(ds->initialize_ds());
    GDEX_EXPECT_FALSE(ds->get_force_software_fallback());
    if (ds->is_d3d12_backend()) {
        GDEX_EXPECT_EQ(static_cast<int64_t>(ds->get_init_status()),
                static_cast<int64_t>(godot::DSContext::INIT_STATUS_DIRECTSTORAGE_D3D12));
        GDEX_EXPECT_EQ(gdextest::to_std_string(ds->get_active_backend_name()), std::string("Win32DirectStorage"));
    }
}
