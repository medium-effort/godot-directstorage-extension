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
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

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

    // Rejection may land at enqueue (Win32 validates the GDeflate stream header
    // against the declared size before submission) or at completion (software
    // fallback decodes and fails); both are graceful failures.
    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    if (enqueued) {
        ds->call(godot::StringName("flush_batch"));
        ds->call(godot::StringName("wait_for_completion"), 5000);
    }

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

// Sentinel proof for the GDeflate size contract. The declared uncompressed size
// is what sizes the destination, so decompression must never write past it. The
// CPU fallback enforces that in GDeflateDecompressor::validate_stream; the
// hardware path must reject the request before submission instead of letting
// DirectStorage decode the tiles the stream itself declares.
GDEX_TEST_T(context_lifecycle, fallback_gdeflate_size_mismatch_never_overruns_destination,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    godot::PackedByteArray original = create_pattern(4096);
    godot::PackedByteArray compressed = compress_data(ctx, original);
    GDEX_ASSERT_TRUE(compressed.size() > 0);

    const godot::String file_path = "user://__gdextest_size_mismatch_canary.bin";
    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(file_path, godot::FileAccess::WRITE);
    GDEX_ASSERT_TRUE(f.is_valid());
    f->store_buffer(compressed);
    f->close();

    ctx.add_teardown([ds, file_path]() {
        ds->call(godot::StringName("flush_batch"));
        godot::DirAccess::remove_absolute(file_path);
    });

    // 4096-byte destination with a 2048-byte declared size: bytes
    // [2048, 4096) are the canary and must survive any completion. A driver
    // that decodes the 4096-byte stream regardless of the declaration
    // overwrites them - with a destination sized from the declaration that is
    // a heap overflow.
    godot::PackedByteArray canary;
    canary.resize(4096);
    for (int64_t i = 0; i < 4096; ++i) {
        canary[i] = 0xAA;
    }

    godot::Ref<godot::RefCounted> req = make_request(ctx);
    req->call(godot::StringName("set_file_path"), file_path);
    req->call(godot::StringName("set_size"), compressed.size());
    req->call(godot::StringName("set_uncompressed_size"), 2048);
    req->call(godot::StringName("set_compression_format"), COMPRESSION_GDEFLATE);
    req->call(godot::StringName("set_destination_type"), DESTINATION_MEMORY);
    req->call(godot::StringName("set_destination_buffer"), canary);

    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    if (enqueued) {
        ds->call(godot::StringName("flush_batch"));
        ds->call(godot::StringName("wait_for_completion"), 5000);
    }

    const int64_t status = static_cast<int64_t>(req->call(godot::StringName("get_status")));
    godot::PackedByteArray result = static_cast<godot::PackedByteArray>(
            req->call(godot::StringName("get_destination_buffer")));

    // The software fallback resizes the destination to the declared size before
    // decoding, so the canary can only be observed on a path that never wrote
    // (the hardware rejection); scan whatever is left.
    int64_t first_overrun = -1;
    for (int64_t i = 2048; i < result.size(); ++i) {
        if (result[i] != 0xAA) {
            first_overrun = i;
            break;
        }
    }
    if (first_overrun >= 0) {
        GDEX_FAIL("destination overrun: canary byte " + std::to_string(first_overrun) +
                " changed although only 2048 uncompressed bytes were declared (status=" +
                std::to_string(status) + ")");
    }

    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);
    const godot::String err = static_cast<godot::String>(req->call(godot::StringName("get_error_message")));
    GDEX_EXPECT_FALSE(err.is_empty());
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

    // Rejection may land at enqueue (Win32 rejects a payload shorter than a
    // tile-stream header) or at completion (software fallback); both are
    // graceful failures.
    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    if (enqueued) {
        ds->call(godot::StringName("flush_batch"));
        ds->call(godot::StringName("wait_for_completion"), 5000);
    }

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

GDEX_TEST_T(context_lifecycle, shutdown_drain_rejects_subsequent_enqueue,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");

    // Explicitly call shutdown_ds to put backend into shutdown state
    ds->call(godot::StringName("shutdown_ds"));
    const int64_t init_status = static_cast<int64_t>(ds->call(godot::StringName("get_init_status")));
    GDEX_EXPECT_EQ(init_status, 0);

    // New request creation
    godot::Ref<godot::RefCounted> req = make_request(ctx);
    req->call(godot::StringName("set_file_path"), "user://dummy.bin");
    req->call(godot::StringName("set_size"), 1024);
    req->call(godot::StringName("set_destination_type"), DESTINATION_MEMORY);

    // Enqueue must immediately reject and return false
    const bool enqueued = static_cast<bool>(ds->call(
        godot::StringName("enqueue_request"), godot::Variant(req)));
    GDEX_EXPECT_FALSE(enqueued);

    const int64_t status = static_cast<int64_t>(req->call(godot::StringName("get_status")));
    GDEX_EXPECT_EQ(status, REQUEST_STATUS_FAILED);

    // Re-initialize DSContext cleanly so subsequent tests have a valid state
    ds->call(godot::StringName("initialize_ds"));
}

GDEX_TEST_T(context_lifecycle, concurrent_lazy_initialization_stress, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    ds->call(godot::StringName("shutdown_ds"));

    // Spawn 16 threads attempting to initialize concurrently
    constexpr int NUM_THREADS = 16;
    std::vector<std::thread> workers;
    workers.reserve(NUM_THREADS);
    std::atomic<int> success_count{0};

    for (int i = 0; i < NUM_THREADS; ++i) {
        workers.emplace_back([ds, &success_count]() {
            bool ok = static_cast<bool>(ds->call(godot::StringName("initialize_ds")));
            if (ok) {
                success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto &t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    GDEX_EXPECT_EQ(success_count.load(), NUM_THREADS);
    GDEX_EXPECT_TRUE(static_cast<bool>(ds->call(godot::StringName("is_initialized"))));
}

GDEX_TEST_T(context_lifecycle, concurrent_reader_queries_during_shutdown_stress, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");

    // Reader count follows the host: this test exists to race reader queries
    // against shutdown/init, not to saturate the CPU. GitHub-hosted runners
    // report 2 processors, and 8 pure-spinning readers there preempt the lock
    // holders the main thread needs (exclusive backend_lifecycle_mutex +
    // context_mutex), so 20 cycles cost ~4.3 s on windows-latest CI while the
    // same 1000 cycles without readers cost ~0.4 s. Clamp 2..8 keeps the 8-way
    // churn on 8+ core dev machines and shrinks to 2 on a 2-vCPU runner.
    godot::OS *os = godot::OS::get_singleton();
    const int host_cpus = os ? os->get_processor_count() : 4;
    const int num_readers = std::max(2, std::min(host_cpus, 8));

    std::atomic<bool> stop_flag{false};
    std::vector<std::thread> readers;
    readers.reserve(num_readers);

    for (int i = 0; i < num_readers; ++i) {
        readers.emplace_back([ds, &stop_flag]() {
            while (!stop_flag.load(std::memory_order_relaxed)) {
                ds->call(godot::StringName("get_async_batches_submitted"));
                ds->call(godot::StringName("get_in_flight_async_batch_count"));
                ds->call(godot::StringName("get_completion_fence"));
                ds->call(godot::StringName("wait_for_completion"), 0);
                // Yield between passes. A bare spin keeps the reader runnable
                // even while a shutdown/init holds a lock, so on an
                // oversubscribed runner the holder is preempted mid critical
                // section and every acquisition becomes a scheduler wait.
                // The cycles still interleave with thousands of query passes.
                std::this_thread::yield();
            }
        });
    }

    // Main test thread alternates shutdown and initialization 20 times
    for (int cycle = 0; cycle < 20; ++cycle) {
        ds->call(godot::StringName("shutdown_ds"));
        ds->call(godot::StringName("initialize_ds"));
    }

    stop_flag.store(true, std::memory_order_relaxed);
    for (auto &t : readers) {
        if (t.joinable()) {
            t.join();
        }
    }

    GDEX_EXPECT_TRUE(static_cast<bool>(ds->call(godot::StringName("is_initialized"))));
}

GDEX_TEST_T(context_lifecycle, auto_tune_setting_toggle_and_defaults, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    // Verify default auto-tune setting is true (reduction-only UMA clamp active)
    const bool default_auto_tune = static_cast<bool>(
        ds->call(godot::StringName("get_auto_tune_for_integrated_gpu")));
    GDEX_EXPECT_TRUE(default_auto_tune);

    // Ensure state cleans up on teardown
    ctx.add_teardown([ds, default_auto_tune]() {
        ds->call(godot::StringName("set_auto_tune_for_integrated_gpu"), default_auto_tune);
    });

    // Toggle off
    ds->call(godot::StringName("set_auto_tune_for_integrated_gpu"), false);
    GDEX_EXPECT_FALSE(static_cast<bool>(
        ds->call(godot::StringName("get_auto_tune_for_integrated_gpu"))));

    // Toggle back on
    ds->call(godot::StringName("set_auto_tune_for_integrated_gpu"), true);
    GDEX_EXPECT_TRUE(static_cast<bool>(
        ds->call(godot::StringName("get_auto_tune_for_integrated_gpu"))));
}

GDEX_TEST_T(context_lifecycle, gpu_adapter_info_schema_and_uma_invariants, TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");

    // Call get_gpu_adapter_info and verify Dictionary schema
    godot::Variant adapter_var = ds->call(godot::StringName("get_gpu_adapter_info"));
    GDEX_ASSERT_EQ(adapter_var.get_type(), godot::Variant::DICTIONARY);
    godot::Dictionary adapter = static_cast<godot::Dictionary>(adapter_var);

    // Required schema keys
    GDEX_EXPECT_TRUE(adapter.has("available"));
    GDEX_EXPECT_TRUE(adapter.has("adapter_name"));
    GDEX_EXPECT_TRUE(adapter.has("adapter_vendor"));
    GDEX_EXPECT_TRUE(adapter.has("vendor_id"));
    GDEX_EXPECT_TRUE(adapter.has("device_id"));
    GDEX_EXPECT_TRUE(adapter.has("dedicated_vram_mb"));
    GDEX_EXPECT_TRUE(adapter.has("shared_system_mb"));
    GDEX_EXPECT_TRUE(adapter.has("software_adapter"));
    GDEX_EXPECT_TRUE(adapter.has("is_uma"));
    GDEX_EXPECT_TRUE(adapter.has("is_cache_coherent_uma"));
    GDEX_EXPECT_TRUE(adapter.has("integrated_gpu_guess"));

    // Check capability report schema
    godot::Variant cap_var = ds->call(godot::StringName("get_capability_report"));
    GDEX_ASSERT_EQ(cap_var.get_type(), godot::Variant::DICTIONARY);
    godot::Dictionary cap = static_cast<godot::Dictionary>(cap_var);
    GDEX_EXPECT_TRUE(cap.has("is_uma"));
    GDEX_EXPECT_TRUE(cap.has("is_cache_coherent_uma"));

    // Direct methods vs Dictionary consistency
    const bool is_uma_method = static_cast<bool>(ds->call(godot::StringName("is_uma")));
    const bool is_uma_dict = static_cast<bool>(adapter.get("is_uma", false));
    GDEX_EXPECT_EQ(is_uma_method, is_uma_dict);

    const bool is_igpu_method = static_cast<bool>(ds->call(godot::StringName("is_integrated_gpu")));

    // Logical invariant: if hardware reports UMA == true, it MUST be recognized as integrated GPU
    if (is_uma_method) {
        GDEX_EXPECT_TRUE(is_igpu_method);
    }
}

// Regression for the lost-wakeup hang in FallbackBackend::shutdown(): the stop
// flag used to be stored OUTSIDE batch_mutex, so a completion worker that had
// just sampled its wait predicate (which reads the flag under that mutex) but
// had not yet registered as a waiter could miss notify_all() and sleep forever,
// blocking join() and freezing the whole suite. An initialize/shutdown pair
// with nothing in between is the worst case for that window, so repeat it.
GDEX_TEST_T(context_lifecycle, repeated_initialize_shutdown_has_no_lost_wakeup,
        TAG_INTEGRATION) {
    godot::Object *ds = require_singleton(ctx, "DSContext");
    ds->call(godot::StringName("shutdown_ds"));

    // Each cycle costs one thread spawn + join (~0.1 ms), so 1000 cycles is a
    // fraction of a second while exercising the start/stop race thousands of
    // times per run instead of once per suite.
    constexpr int kCycles = 1000;
    for (int i = 0; i < kCycles; ++i) {
        GDEX_ASSERT_TRUE(static_cast<bool>(ds->call(godot::StringName("initialize_ds"))));
        ds->call(godot::StringName("shutdown_ds"));
        GDEX_EXPECT_FALSE(static_cast<bool>(ds->call(godot::StringName("is_initialized"))));
    }

    // Leave the singleton initialized; the rest of the suite expects that.
    GDEX_EXPECT_TRUE(static_cast<bool>(ds->call(godot::StringName("initialize_ds"))));
}

