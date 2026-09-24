// Live-engine DSContext lifecycle suite.
//
// Requires the REAL extension loaded via [gdextest.consumer_extension]
// (registered classes + DSContext/DSErrorHandler singletons). Because
// consumer_extension is NOT configured in compile-in test mode, these
// tests SKIP (green, non-failing) when singletons are absent and the
// deliverable reports the cause instead of editing TOML.
//
// Link discipline: ds_context.cpp / ds_request.cpp are NOT in the test
// build's compile-in sources (.gdextest.toml [gdextest.tests]), so this TU
// must not reference DSContext / DSRequest symbols directly. Every
// cross-extension call below uses dynamic Object::call dispatch through the
// Engine singleton, which needs only godot-cpp + gdextest headers.
//
// Fallback rule: assertions that can run on the SoftwareFallback backend
// must pass here. Anything requiring Win32 DirectStorage + D3D12 carries
// TAG_SLOW and defers to D3D12 backend tests (skips when the D3D12 backend is idle).

#include "gdextest/assert.h"
#include "gdextest/engine.h"
#include "gdextest/registry.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>
#include <string>

namespace {

// DSContext::InitStatus values (src/core/ds_context.h, verified).
constexpr int64_t INIT_STATUS_MIN = 0; // INIT_STATUS_UNINITIALIZED
constexpr int64_t INIT_STATUS_MAX = 3; // INIT_STATUS_FAILED

// DSRequest::Status::STATUS_FAILED (src/core/ds_request.h, verified).
constexpr int64_t REQUEST_STATUS_FAILED = 3;

// DSRequest::DSDestinationType values (ds_request.h).
constexpr int64_t DESTINATION_MEMORY = 0;
constexpr int64_t DESTINATION_GPU_TEXTURE = 2;

// DSRequest::CompressionFormat::COMPRESSION_GDEFLATE (ds_request.h).
constexpr int64_t COMPRESSION_GDEFLATE = 1;

godot::PackedByteArray create_pattern(int64_t size) {
    godot::PackedByteArray data;
    data.resize(size);
    for (int64_t i = 0; i < size; i++) {
        data[i] = static_cast<uint8_t>((i * 17 + 5) % 256);
    }
    return data;
}

godot::PackedByteArray compress_data(gdextest::TestContext &ctx, const godot::PackedByteArray &data) {
    if (!godot::ClassDB::class_exists(godot::StringName("GDeflateCompressor"))) {
        GDEX_SKIP("GDeflateCompressor class absent.");
    }
    godot::Ref<godot::RefCounted> compressor(
        godot::ClassDB::instantiate(godot::StringName("GDeflateCompressor")));
    GDEX_ASSERT_TRUE(compressor.is_valid());
    godot::Variant compressed_var = compressor->call(godot::StringName("compress"), data, 1);
    return static_cast<godot::PackedByteArray>(compressed_var);
}

// Fetch a live Engine singleton by name, or SKIP with the
// consumer_extension diagnostic (fallback step 2: report, don't fail).
godot::Object *require_singleton(gdextest::TestContext &ctx, const char *p_name) {
    (void)ctx;
    godot::Engine *engine = godot::Engine::get_singleton();
    godot::Object *obj = nullptr;
    if (engine != nullptr && engine->has_singleton(godot::StringName(p_name))) {
        obj = engine->get_singleton(godot::StringName(p_name));
    }
    if (obj == nullptr) {
        GDEX_SKIP(std::string("Engine singleton '") + p_name +
                  "' absent: [gdextest.consumer_extension] library is unset, "
                  "so the real extension (compile-in test mode) is "
                  "not loaded in this fixture.");
    }
    return obj;
}

// Instantiate DSRequest through the engine ClassDB (dynamic dispatch keeps
// this TU free of ds_request.cpp link dependencies). Skips when the real
// extension is absent. The caller's local Ref owns the reference; tracking
// keeps leak detection meaningful.
godot::Ref<godot::RefCounted> make_request(gdextest::TestContext &ctx) {
    if (!godot::ClassDB::class_exists(godot::StringName("DSRequest"))) {
        GDEX_SKIP("DSRequest class absent: real extension not loaded "
                  "(consumer_extension unset).");
    }
    godot::Ref<godot::RefCounted> req(
        godot::ClassDB::instantiate(godot::StringName("DSRequest")));
    GDEX_ASSERT_TRUE(req.is_valid());
    ctx.track_ref(req.ptr());
    return req;
}

} // namespace

GDEX_TEST_T(context_lifecycle, singleton_present, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    GDEX_EXPECT_NOT_NULL(static_cast<void *>(ds));
}

GDEX_TEST_T(context_lifecycle, init_status_is_valid_enum, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    const int64_t status =
        static_cast<int64_t>(ds->call(godot::StringName("get_init_status")));
    GDEX_EXPECT_GE(status, INIT_STATUS_MIN);
    GDEX_EXPECT_LE(status, INIT_STATUS_MAX);
}

GDEX_TEST_T(context_lifecycle, backend_selection_is_sane, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    GDEX_EXPECT_FALSE(backend.is_empty());
    // Backend names observed in-tree: "Win32DirectStorage"
    // (src/core/ds_context.cpp), "SoftwareFallback"
    // (src/platform/fallback/ds_fallback_backend.cpp), "None" (pre-init).
    const bool known = backend == godot::String("Win32DirectStorage") ||
        backend == godot::String("SoftwareFallback") ||
        backend == godot::String("None");
    GDEX_EXPECT_TRUE(known);
    // An active backend implies an initialized context (initialize_ds sets
    // both together); "None" is the legitimate pre-init state.
    if (backend != godot::String("None")) {
        const bool initialized = static_cast<bool>(
            ds->call(godot::StringName("is_initialized")));
        GDEX_EXPECT_TRUE(initialized);
    }
}

GDEX_TEST_T(context_lifecycle, invalid_path_enqueue_fails_with_request_error,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::Ref<godot::RefCounted> req = make_request(ctx);
    // Drain any leftovers on the way out; flush_batch is a safe no-op when
    // the queue is already idle.
    ctx.add_teardown([ds]() {
        ds->call(godot::StringName("flush_batch"));
    });

    req->call(godot::StringName("set_file_path"),
        godot::String("res://__gdextest__/does_not_exist.bin"));
    req->call(godot::StringName("set_size"), 64);

    ds->call(godot::StringName("enqueue_request"), godot::Variant(req));
    ds->call(godot::StringName("flush_batch"));
    // Sync on fallback; fence wait on Win32. Result ignored: terminal status
    // below is the assertion, this just bounds the wait on async backends.
    ds->call(godot::StringName("wait_for_completion"), 5000);

    // FallbackBackend::process_request fails FileAccess::open on a missing
    // file (STATUS_FAILED + message); the Win32 backend likewise reports a
    // terminal failure, so terminal-status + non-empty message holds on both.
    const int64_t status =
        static_cast<int64_t>(req->call(godot::StringName("get_status")));
    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);
    const godot::String err = static_cast<godot::String>(
        req->call(godot::StringName("get_error_message")));
    GDEX_EXPECT_FALSE(err.is_empty());

    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    if (backend == godot::String("SoftwareFallback")) {
        GDEX_EXPECT_STR_CONTAINS(err, "Failed to open file");
    }
}

GDEX_TEST_T(context_lifecycle, invalid_request_reports_through_error_handler,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::Object *err_handler = require_singleton(ctx, "DSErrorHandler");
    godot::Ref<godot::RefCounted> req = make_request(ctx);
    // Restore shared-singleton state on the way out (teardowns run LIFO,
    // before the tracked-ref leak check).
    ctx.add_teardown([err_handler, ds]() {
        err_handler->call(godot::StringName("clear_error"));
        ds->call(godot::StringName("flush_batch"));
    });
    err_handler->call(godot::StringName("clear_error"));

    // GPU destinations are rejected unless the Win32 DirectStorage D3D12
    // backend is live (enqueue_request_internal reports to DSErrorHandler).
    // The D3D12-live half of this contract is tested with D3D12 backend tests; here we assert the
    // fallback-backend rejection path, which must pass here.
    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    const bool d3d12 = static_cast<bool>(
        ds->call(godot::StringName("is_d3d12_backend")));
    if (backend == godot::String("Win32DirectStorage") && d3d12) {
        GDEX_SKIP("D3D12 backend live: GPU-destination acceptance is a "
                  "D3D12 slow-suite assertion.");
    }

    req->call(godot::StringName("set_file_path"),
        godot::String("res://__gdextest__/does_not_exist.bin"));
    req->call(godot::StringName("set_size"), 64);
    req->call(
        godot::StringName("set_destination_type"), DESTINATION_GPU_TEXTURE);

    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    GDEX_EXPECT_FALSE(enqueued);
    const bool has_error = static_cast<bool>(
        err_handler->call(godot::StringName("has_error")));
    GDEX_EXPECT_TRUE(has_error);
    const godot::String last = static_cast<godot::String>(
        err_handler->call(godot::StringName("get_last_error")));
    GDEX_EXPECT_FALSE(last.is_empty());
}

// D3D12-only assertions live here under TAG_SLOW so the default test run
// skips them when the fallback backend is active; D3D12 backend tests run them.
GDEX_TEST_T(context_lifecycle, d3d12_gpu_tier_matches_backend,
        TAG_INTEGRATION | TAG_SLOW) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    const bool d3d12 = static_cast<bool>(
        ds->call(godot::StringName("is_d3d12_backend")));
    if (backend != godot::String("Win32DirectStorage") || !d3d12) {
        GDEX_SKIP("Win32 D3D12 backend not active; D3D12 tier assertions "
                  "deferred to D3D12 backend tests.");
    }
    const int64_t tier = static_cast<int64_t>(ds->call(
        godot::StringName("get_gpu_decompression_support_tier")));
    GDEX_EXPECT_GE(tier, 0);
    const bool available = static_cast<bool>(ds->call(
        godot::StringName("is_gpu_decompression_available")));
    // is_gpu_decompression_available() is tier > 0 by implementation.
    GDEX_EXPECT_EQ(available, tier > 0);
}

GDEX_TEST_T(context_lifecycle, fallback_gdeflate_corrupted_payload_fails_gracefully,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::PackedByteArray original = create_pattern(4096);
    godot::PackedByteArray compressed = compress_data(ctx, original);
    GDEX_ASSERT_TRUE(compressed.size() > 0);

    // Corrupt the compressed payload data (leaves header and tile table intact)
    for (int64_t i = 16; i < compressed.size(); i++) {
        compressed[i] = static_cast<uint8_t>(compressed[i] ^ 0xFF);
    }

    const godot::String file_path = "user://__gdextest_corrupt.bin";
    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(file_path, godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid());
    f->store_buffer(compressed);
    f->close();

    ctx.add_teardown([ds, file_path]() {
        ds->call(godot::StringName("flush_batch"));
        godot::DirAccess::remove_absolute(file_path);
    });

    godot::Ref<godot::RefCounted> req = make_request(ctx);
    req->call(godot::StringName("set_file_path"), file_path);
    req->call(godot::StringName("set_size"), compressed.size());
    req->call(godot::StringName("set_uncompressed_size"), original.size());
    req->call(godot::StringName("set_compression_format"), COMPRESSION_GDEFLATE);
    req->call(godot::StringName("set_destination_type"), DESTINATION_MEMORY);

    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    GDEX_EXPECT_TRUE(enqueued);
    ds->call(godot::StringName("flush_batch"));
    ds->call(godot::StringName("wait_for_completion"), 5000);

    const int64_t status = static_cast<int64_t>(req->call(godot::StringName("get_status")));
    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);
    const godot::String err = static_cast<godot::String>(req->call(godot::StringName("get_error_message")));
    GDEX_EXPECT_FALSE(err.is_empty());

    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    if (backend == godot::String("SoftwareFallback")) {
        GDEX_EXPECT_STR_CONTAINS(err, "CPU GDeflate decompression failed");
    }
}

GDEX_TEST_T(context_lifecycle, fallback_gdeflate_size_mismatch_fails_gracefully,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::PackedByteArray original = create_pattern(4096);
    godot::PackedByteArray compressed = compress_data(ctx, original);
    GDEX_ASSERT_TRUE(compressed.size() > 0);

    const godot::String file_path = "user://__gdextest_size_mismatch.bin";
    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(file_path, godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid());
    f->store_buffer(compressed);
    f->close();

    ctx.add_teardown([ds, file_path]() {
        ds->call(godot::StringName("flush_batch"));
        godot::DirAccess::remove_absolute(file_path);
    });

    godot::Ref<godot::RefCounted> req = make_request(ctx);
    req->call(godot::StringName("set_file_path"), file_path);
    req->call(godot::StringName("set_size"), compressed.size());
    // Mismatched uncompressed size (2048 requested vs 4096 header): must not overflow
    req->call(godot::StringName("set_uncompressed_size"), 2048);
    req->call(godot::StringName("set_compression_format"), COMPRESSION_GDEFLATE);
    req->call(godot::StringName("set_destination_type"), DESTINATION_MEMORY);

    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    GDEX_EXPECT_TRUE(enqueued);
    ds->call(godot::StringName("flush_batch"));
    ds->call(godot::StringName("wait_for_completion"), 5000);

    const int64_t status = static_cast<int64_t>(req->call(godot::StringName("get_status")));
    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);
    const godot::String err = static_cast<godot::String>(req->call(godot::StringName("get_error_message")));
    GDEX_EXPECT_FALSE(err.is_empty());

    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    if (backend == godot::String("SoftwareFallback")) {
        GDEX_EXPECT_STR_CONTAINS(err, "CPU GDeflate decompression failed");
    }
}

GDEX_TEST_T(context_lifecycle, fallback_gdeflate_truncated_header_fails_gracefully,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::PackedByteArray truncated;
    truncated.resize(4);
    truncated[0] = 0x47;
    truncated[1] = 0x44;
    truncated[2] = 0x01;
    truncated[3] = 0x00;

    const godot::String file_path = "user://__gdextest_truncated.bin";
    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(file_path, godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid());
    f->store_buffer(truncated);
    f->close();

    ctx.add_teardown([ds, file_path]() {
        ds->call(godot::StringName("flush_batch"));
        godot::DirAccess::remove_absolute(file_path);
    });

    godot::Ref<godot::RefCounted> req = make_request(ctx);
    req->call(godot::StringName("set_file_path"), file_path);
    req->call(godot::StringName("set_size"), truncated.size());
    req->call(godot::StringName("set_uncompressed_size"), 4096);
    req->call(godot::StringName("set_compression_format"), COMPRESSION_GDEFLATE);
    req->call(godot::StringName("set_destination_type"), DESTINATION_MEMORY);

    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    GDEX_EXPECT_TRUE(enqueued);
    ds->call(godot::StringName("flush_batch"));
    ds->call(godot::StringName("wait_for_completion"), 5000);

    const int64_t status = static_cast<int64_t>(req->call(godot::StringName("get_status")));
    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);
    const godot::String err = static_cast<godot::String>(req->call(godot::StringName("get_error_message")));
    GDEX_EXPECT_FALSE(err.is_empty());

    const godot::String backend = static_cast<godot::String>(
        ds->call(godot::StringName("get_active_backend_name")));
    if (backend == godot::String("SoftwareFallback")) {
        GDEX_EXPECT_STR_CONTAINS(err, "CPU GDeflate decompression failed");
    }
}
