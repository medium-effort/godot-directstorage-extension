// tests/win32_file_cache_tests.cpp
//
// Covers the Win32 open-file cache
// (src/platform/win32/ds_win32_backend.{h,cpp}):
//
// - Per-slot file_cache with close_slot_files() single-owner lifetime.
// - Backend-level persistent_file_cache holding one handle per path with a live
//   owner count: shared by every request in a batch, erased when the last
//   reference goes (nothing is retained between batches).
// - No eager pre-open: initialize() opens nothing, the first request opens
//   lazily.
//
// clear_persistent_files() and close_slot_files() are private, so this suite
// asserts the observable contract at the seams: guard ordering
// (validation/init before any cache lookup), same-archive double enqueue
// reuse, and post-shutdown staleness (no stale handle ever served after
// shutdown). SConstruct filters src/platform/win32 out of the test build on
// non-Windows, so every Win32Backend reference is _WIN32 guarded; off-Windows
// the suite skips green per repo convention.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#include "core/ds_request.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#endif

namespace {

#ifdef _WIN32
constexpr const char *kProbePath = "user://gdextest_win32_filecache.bin";

bool write_probe_file() {
    godot::PackedByteArray payload;
    payload.resize(256);
    for (int64_t i = 0; i < 256; ++i) {
        payload[i] = static_cast<uint8_t>((i * 53 + 7) & 0xFF);
    }
    godot::Ref<godot::FileAccess> out =
        godot::FileAccess::open(godot::String(kProbePath), godot::FileAccess::WRITE);
    if (out.is_null()) {
        return false;
    }
    out->store_buffer(payload);
    out->close();
    return true;
}

void remove_probe_file() {
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open("user://");
    if (dir.is_valid()) {
        (void)dir->remove(godot::String(kProbePath));
    }
}

godot::Ref<godot::DSRequest> make_sized_request(const char *p_path) {
    godot::Ref<godot::DSRequest> req;
    req.instantiate();
    req->set_file_path(godot::String(p_path));
    req->set_size(256);
    return req;
}
#endif

} // namespace

GDEX_TEST_T(win32_file_cache, uninitialized_double_enqueue_fails_cleanly, TAG_INTEGRATION) {
#ifdef _WIN32
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); }); // dtor -> shutdown()
    if (!write_probe_file()) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([]() { remove_probe_file(); });

    // Guard ordering: !factory early-out must precede any file-cache lookup,
    // so repeated enqueues fail identically without opening handles.
    for (int i = 0; i < 2; ++i) {
        godot::Ref<godot::DSRequest> req = make_sized_request(kProbePath);
        GDEX_EXPECT_FALSE(backend->enqueue_request(req));
        GDEX_EXPECT_EQ(static_cast<int64_t>(req->get_status()),
                static_cast<int64_t>(godot::DSRequest::STATUS_FAILED));
    }
    godot::Ref<godot::DSRequest> bad = make_sized_request("res://__gdextest__/does_not_exist.bin");
    GDEX_EXPECT_FALSE(backend->enqueue_request(bad));
#else
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}

GDEX_TEST_T(win32_file_cache, same_archive_double_enqueue_and_shutdown, TAG_INTEGRATION) {
#ifdef _WIN32
    godot::Win32Backend *backend = memnew(godot::Win32Backend);
    ctx.add_teardown([backend]() { memdelete(backend); }); // dtor -> shutdown()
    if (!backend->is_available()) {
        GDEX_SKIP("DirectStorage runtime absent on this machine.");
    }
    if (!backend->initialize()) {
        GDEX_SKIP("Win32Backend initialize() failed (no D3D12 device); cache path needs live queues.");
    }
    if (!write_probe_file()) {
        GDEX_FAIL("failed to stage probe file.");
    }
    ctx.add_teardown([]() { remove_probe_file(); });

    // Same archive twice: the second open must reuse the cached handle
    // (per-slot file_cache, then persistent_file_cache) instead of failing
    // or leaking; both enqueues succeed.
    godot::Ref<godot::DSRequest> first = make_sized_request(kProbePath);
    godot::Ref<godot::DSRequest> second = make_sized_request(kProbePath);
    GDEX_EXPECT_TRUE(backend->enqueue_request(first));
    GDEX_EXPECT_TRUE(backend->enqueue_request(second));

    // Shutdown releases every owned handle: the staged requests' reference is
    // dropped by close_slot_files(), and clear_persistent_files() covers any
    // entry a live batch still owned. A post-shutdown enqueue must fail
    // cleanly rather than serve a stale cached handle, and a second shutdown
    // must be a safe no-op.
    backend->shutdown();
    backend->shutdown();
    godot::Ref<godot::DSRequest> late = make_sized_request(kProbePath);
    GDEX_EXPECT_FALSE(backend->enqueue_request(late));
#else
    GDEX_SKIP("Win32Backend sources are excluded from the test build on non-Windows.");
#endif
}
