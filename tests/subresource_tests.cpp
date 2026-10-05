// tests/subresource_tests.cpp
//
// C++ suite for the subresource-range area. C++ port of the checks
// in the authoritative GDScript headless tests (project/headless_tests/ —
// NOT modified here, they stay authoritative):
//   - test_subresource_range.gd  (mip-chain range config + footprints)
//   - test_footprint_calc.gd     (row-pitch alignment + pad/unpad round-trip)
//   - test_oversized_texture.gd  (staging-split bounds, per-mip requests)
//
// Real APIs (headers read first, per contract):
//   - src/formats/dstex_format.h : DSTex::get_mip_layout /
//     DSTex::unpad_footprint_mip (pure; compiled into the test lib via
//     [gdextest.tests] src/formats/*.cpp).
//   - src/core/ds_request.h      : DSRequest subresource-range config
//     (first_subresource / subresource_count / texture_format /
//     destination_type / compression_format / status).
//     LINK BOUNDARY: src/core/ds_request.cpp is NOT in [gdextest.tests]
//     sources (only ds_error_handler.cpp is) and consumer_extension is NOT
//     configured — so this suite MUST NOT ODR-use DSRequest methods or its
//     ctor (that would break the test-library link for every suite).
//     Driver-dependent cases therefore drive DSRequest through the engine
//     ClassDB by string name (exact bound names from DSRequest::_bind_methods
//     in src/core/ds_request.cpp, DSContext methods from
//     src/core/ds_context.h) and GDEX_SKIP when classes/singletons are
//     absent. Enum constants (STATUS_COMPLETED, ...) are compile-time only,
//     so including the header is link-safe.
//
// Tagging: pure footprint / range-tiling cases are untagged (default
// TAG_UNIT — get_mip_layout / unpad_footprint_mip need no engine).
// Anything touching the driver is GDEX_TEST_T(..., TAG_INTEGRATION |
// TAG_SLOW) and is verified on hardware in D3D12 backend tests, not here.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_request.h"
#include "formats/dstex_format.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Deterministic footprint-pattern payload: each source row filled with its
// row index, D3D12 padding bytes filled with 0xA5 so unpad tests can prove
// the padding was stripped (not just resized away).
godot::PackedByteArray make_pattern_footprint(uint32_t p_width, uint32_t p_height, int p_format) {
    godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(p_width, p_height, p_format);
    godot::PackedByteArray footprint;
    footprint.resize((int64_t)layout.footprint_size);
    uint8_t *dst = footprint.ptrw();
    for (uint64_t r = 0; r < layout.row_count; ++r) {
        memset(dst + r * layout.row_pitch, (int)(r & 0xFF), (size_t)layout.source_row_bytes);
        if (layout.row_pitch > layout.source_row_bytes) {
            memset(dst + r * layout.row_pitch + layout.source_row_bytes, 0xA5,
                    (size_t)(layout.row_pitch - layout.source_row_bytes));
        }
    }
    return footprint;
}

// ClassDB-driven singleton lookup: link-safe (godot_cpp core only).
godot::Object *ds_singleton(const char *p_name) {
    godot::Engine *engine = godot::Engine::get_singleton();
    if (engine == nullptr || !engine->has_singleton(p_name)) {
        return nullptr;
    }
    return engine->get_singleton(p_name);
}

// RenderingDevice::free_rid asserts the render thread, and gdextest runs on
// the main thread. The slot is a plain integer, created on first use: a
// file-scope godot::RID/Variant constructs during DLL load, before
// gdextension_interface exists, and that fails DllMain with Error 1114.
godot::RID *rid_free_slot() {
    static godot::RID *slot = new godot::RID();
    return slot;
}

void free_posted_rid_on_render_thread() {
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
    godot::RID *slot = rid_free_slot();
    if (rd != nullptr && slot->is_valid()) {
        rd->free_rid(*slot);
    }
    *slot = godot::RID();
}

void free_texture_rid(const godot::RID &p_rid) {
    if (!p_rid.is_valid()) {
        return;
    }
    godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
    if (rs == nullptr) {
        return;
    }
    if (rs->is_on_render_thread()) {
        godot::RenderingDevice *rd = rs->get_rendering_device();
        if (rd != nullptr) {
            rd->free_rid(p_rid);
        }
        return;
    }
    *rid_free_slot() = p_rid;
    rs->call_on_render_thread(callable_mp_static(free_posted_rid_on_render_thread));
    for (int i = 0; i < 100 && rid_free_slot()->is_valid(); ++i) {
        rs->force_draw(false, 0.0);
    }
}

bool write_payload_file(const godot::String &p_path, const godot::PackedByteArray &p_bytes) {
    godot::Ref<godot::FileAccess> f = godot::FileAccess::open(p_path, godot::FileAccess::WRITE);
    if (!f.is_valid()) {
        return false;
    }
    if (!f->store_buffer(p_bytes)) {
        return false;
    }
    f->close();
    return true;
}

} // namespace

// --- Pure cases (untagged) -------------------------------------------------

GDEX_TEST(subresource, mip_chain_footprints_aligned_and_contiguous) {
    // Mirrors test_subresource_range.gd: WIDTH=128 HEIGHT=96 MIP_COUNT=8, RGBA8.
    const uint32_t widths[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
    const uint32_t heights[8] = { 96, 48, 24, 12, 6, 3, 1, 1 };
    uint64_t total = 0;
    for (int mip = 0; mip < 8; ++mip) {
        godot::DSTexMipLayout layout =
                godot::DSTex::get_mip_layout(widths[mip], heights[mip], godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_GT(layout.footprint_size, 0ULL);
        GDEX_EXPECT_EQ(layout.row_pitch % 256ULL, 0ULL);
        GDEX_EXPECT_GE(layout.row_pitch, layout.source_row_bytes);
        GDEX_EXPECT_EQ(layout.footprint_size, layout.row_pitch * layout.row_count);
        GDEX_EXPECT_EQ(layout.source_size, layout.source_row_bytes * layout.row_count);
        total += layout.footprint_size; // contiguous packing, mirrors payload assembly
    }
    // Anchors (hand-verified): mip0 128x96 RGBA8 -> row 512, pitch 512, 49152 B.
    godot::DSTexMipLayout mip0 = godot::DSTex::get_mip_layout(128, 96, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_EQ(mip0.source_row_bytes, 512ULL);
    GDEX_EXPECT_EQ(mip0.row_count, 96ULL);
    GDEX_EXPECT_EQ(mip0.row_pitch, 512ULL);
    GDEX_EXPECT_EQ(mip0.source_size, 49152ULL);
    GDEX_EXPECT_EQ(mip0.footprint_size, 49152ULL);
    // Mip2 anchor: 32x24 RGBA8 -> row 128, pitch 256, footprint 6144.
    godot::DSTexMipLayout mip2 = godot::DSTex::get_mip_layout(32, 24, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_EQ(mip2.row_pitch, 256ULL);
    GDEX_EXPECT_EQ(mip2.footprint_size, 6144ULL);
    // Full-chain anchor: 49152+12288+6144+3072+1536+768+256+256 = 73472.
    GDEX_EXPECT_EQ(total, 73472ULL);
}

GDEX_TEST(subresource, footprint_unpad_roundtrip_rgba8) {
    // Mirrors test_footprint_calc.gd sizes.
    const uint32_t kDims[4][2] = { { 513, 513 }, { 1024, 768 }, { 319, 127 }, { 2049, 17 } };
    for (int i = 0; i < 4; ++i) {
        const uint32_t w = kDims[i][0];
        const uint32_t h = kDims[i][1];
        godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(w, h, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_EQ(layout.row_pitch % 256ULL, 0ULL);
        GDEX_EXPECT_GE(layout.row_pitch, layout.source_row_bytes);
        GDEX_EXPECT_EQ(layout.footprint_size, layout.row_pitch * (uint64_t)h);
        godot::PackedByteArray footprint = make_pattern_footprint(w, h, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_EQ((uint64_t)footprint.size(), layout.footprint_size);
        godot::PackedByteArray plain =
                godot::DSTex::unpad_footprint_mip(footprint, w, h, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_EQ((uint64_t)plain.size(), layout.source_size);
        // Row payload preserved verbatim, padding stripped.
        const uint8_t *src = footprint.ptr();
        const uint8_t *dst = plain.ptr();
        bool rows_match = true;
        for (uint64_t r = 0; r < layout.row_count && rows_match; ++r) {
            if (memcmp(src + r * layout.row_pitch, dst + r * layout.source_row_bytes,
                            (size_t)layout.source_row_bytes) != 0) {
                rows_match = false;
            }
        }
        GDEX_EXPECT_TRUE(rows_match);
    }
}

GDEX_TEST(subresource, footprint_unpad_passthrough_cases) {
    // Already-natural input passes through untouched.
    godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(64, 48, godot::Image::FORMAT_RGBA8);
    godot::PackedByteArray natural;
    natural.resize((int64_t)layout.source_size);
    memset(natural.ptrw(), 0x3C, (size_t)natural.size());
    godot::PackedByteArray out =
            godot::DSTex::unpad_footprint_mip(natural, 64, 48, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_EQ(out.size(), natural.size());
    // Wrong-sized input (neither source nor footprint size) passes through.
    godot::PackedByteArray odd;
    odd.resize(7);
    GDEX_EXPECT_EQ(godot::DSTex::unpad_footprint_mip(odd, 64, 48, godot::Image::FORMAT_RGBA8).size(), 7);
    // Degenerate inputs yield empty.
    GDEX_EXPECT_EQ(godot::DSTex::unpad_footprint_mip(natural, 0, 48, godot::Image::FORMAT_RGBA8).size(), 0);
    godot::PackedByteArray empty;
    GDEX_EXPECT_EQ(godot::DSTex::unpad_footprint_mip(empty, 64, 48, godot::Image::FORMAT_RGBA8).size(), 0);
    // Unsupported formats have no layout: input passes through unchanged.
    GDEX_EXPECT_EQ(
            godot::DSTex::unpad_footprint_mip(natural, 64, 48, godot::Image::FORMAT_MAX).size(),
            natural.size());
}

GDEX_TEST(subresource, oversized_chain_exceeds_staging_buffer) {
    // Mirrors test_oversized_texture.gd: 5000x5000, 14 mips, 128 MiB staging.
    const uint64_t kStagingBytes = 128ULL * 1024ULL * 1024ULL;
    uint32_t w = 5000;
    uint32_t h = 5000;
    uint64_t total = 0;
    uint32_t requests = 0;
    for (int mip = 0; mip < 14; ++mip) {
        godot::DSTexMipLayout layout = godot::DSTex::get_mip_layout(w, h, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_GT(layout.footprint_size, 0ULL);
        // Each mip fits one TEXTURE_REGION staging request...
        GDEX_EXPECT_LE(layout.footprint_size, kStagingBytes);
        GDEX_EXPECT_EQ(layout.row_pitch % 256ULL, 0ULL);
        total += layout.footprint_size;
        ++requests;
        w = w > 1 ? (w >> 1) : 1;
        h = h > 1 ? (h >> 1) : 1;
    }
    // ...but the chain as a whole exceeds the staging buffer: one request per mip.
    GDEX_EXPECT_GT(total, kStagingBytes);
    GDEX_EXPECT_EQ(requests, 14u);
    GDEX_EXPECT_LE(14u, godot::DSTEX_MAX_MIP_COUNT);
    // Mip0 anchor (hand-verified): row 20000 -> pitch 20224, footprint 101120000.
    godot::DSTexMipLayout mip0 = godot::DSTex::get_mip_layout(5000, 5000, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_EQ(mip0.row_pitch, 20224ULL);
    GDEX_EXPECT_EQ(mip0.footprint_size, 101120000ULL);
    // Tail reaches the 1x1 floor (D3D12 mip tail).
    godot::DSTexMipLayout tail = godot::DSTex::get_mip_layout(1, 1, godot::Image::FORMAT_RGBA8);
    GDEX_EXPECT_GT(tail.footprint_size, 0ULL);
}

GDEX_TEST(subresource, subresource_range_tiling_covers_chain) {
    // Range-config contract behind DSRequest.first_subresource /
    // subresource_count (verified against the real objects in the TAG_SLOW
    // cases below): full-range (0, N) and per-mip (i, 1) tilings address
    // exactly [0, N) — cf. test_subresource_range.gd (first=0, count=8) and
    // test_oversized_texture.gd (first=i, count=1).
    const uint32_t kMipCount = 8;
    // Full-range request covers every subresource exactly once.
    bool full_covered[8] = {};
    {
        const uint32_t first = 0;
        const uint32_t count = kMipCount;
        GDEX_EXPECT_EQ(count, kMipCount);
        for (uint32_t s = first; s < first + count; ++s) {
            GDEX_EXPECT_TRUE(s < kMipCount);
            GDEX_EXPECT_FALSE(full_covered[s]);
            full_covered[s] = true;
        }
    }
    for (uint32_t s = 0; s < kMipCount; ++s) {
        GDEX_EXPECT_TRUE(full_covered[s]);
    }
    // Per-mip tiling: one (i, 1) request per mip, no gaps, no overlaps.
    bool tiled[8] = {};
    for (uint32_t i = 0; i < kMipCount; ++i) {
        const uint32_t first = i;
        const uint32_t count = 1;
        GDEX_EXPECT_TRUE(first + count <= kMipCount);
        GDEX_EXPECT_FALSE(tiled[first]);
        tiled[first] = true;
    }
    for (uint32_t s = 0; s < kMipCount; ++s) {
        GDEX_EXPECT_TRUE(tiled[s]);
    }
}

// --- Driver-dependent cases (TAG_SLOW; verified in D3D12 backend tests) ------

GDEX_TEST_T(subresource, gpu_mip_range_submit, TAG_INTEGRATION | TAG_SLOW) {
    // Ports test_subresource_range.gd::_run_gpu_request (128x96, 8 mips,
    // MULTIPLE_SUBRESOURCES_RANGE-style request with first=0, count=8).
    if (!godot::ClassDB::class_exists("DSRequest") || !godot::ClassDB::class_exists("DSContext")) {
        GDEX_SKIP("DSRequest/DSContext classes unavailable (consumer_extension not configured)");
    }
    godot::Object *ds_ctx = ds_singleton("DSContext");
    if (ds_ctx == nullptr) {
        GDEX_SKIP("DSContext singleton unavailable");
    }
    if (!(bool)ds_ctx->call("initialize_ds") || !(bool)ds_ctx->call("is_d3d12_backend")) {
        GDEX_SKIP("D3D12 DirectStorage backend unavailable");
    }
    // Range config round-trip through the real bound properties (ds_request.h).
    godot::Variant req_holder = godot::ClassDB::instantiate("DSRequest");
    godot::Object *req = req_holder;
    GDEX_ASSERT_NOT_NULL(req);
    req->set("first_subresource", 0);
    req->set("subresource_count", 8);
    req->set("texture_format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    req->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
    req->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
    GDEX_EXPECT_EQ((int64_t)req->get("first_subresource"), 0);
    GDEX_EXPECT_EQ((int64_t)req->get("subresource_count"), 8);
    // Mip-chain texture (RenderingDevice setup mirrors the GDScript).
    godot::RenderingDevice *rd = godot::RenderingServer::get_singleton()->get_rendering_device();
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable");
    }
    godot::Ref<godot::RDTextureFormat> fmt;
    fmt.instantiate();
    godot::Ref<godot::RDTextureView> view;
    view.instantiate();
    fmt->set("format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt->set("width", 128);
    fmt->set("height", 96);
    fmt->set("mipmaps", 8);
    fmt->set("usage_bits", (int64_t)(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
    godot::RID texture = rd->texture_create(fmt, view);
    GDEX_ASSERT_TRUE(texture.is_valid());
    // GPU texture enqueue only accepts a v2 GDeflate .dstex (Win32Backend
    // rejects anything without per-mip footprint entries). Build the same
    // concatenated footprint payload the GDScript subresource test uses.
    godot::PackedByteArray payload;
    {
        uint32_t w = 128;
        uint32_t h = 96;
        for (int mip = 0; mip < 8; ++mip) {
            godot::PackedByteArray mip_data =
                    make_pattern_footprint(w, h, godot::Image::FORMAT_RGBA8);
            const int64_t old_size = payload.size();
            payload.resize(old_size + mip_data.size());
            memcpy(payload.ptrw() + old_size, mip_data.ptr(), (size_t)mip_data.size());
            w = w > 1 ? (w >> 1) : 1;
            h = h > 1 ? (h >> 1) : 1;
        }
    }
    const godot::String kPath = "user://gdex_subresource_range.dstex";
    godot::Ref<godot::DSTex> dstex;
    dstex.instantiate();
    GDEX_ASSERT_TRUE(dstex.is_valid());
    GDEX_ASSERT_EQ((int64_t)dstex->write_to_file(kPath, payload, 128, 96, godot::Image::FORMAT_RGBA8, 8, 1), (int64_t)godot::OK);
    godot::Dictionary header = dstex->read_header(kPath);
    GDEX_ASSERT_EQ((int64_t)header.get("version", 0), (int64_t)2);
    req->set("file_path", kPath);
    req->set("size", (int64_t)header.get("compressed_size", 0));
    req->set("uncompressed_size", (int64_t)header.get("original_size", 0));
    req->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
    godot::Variant native_format = rd->call("get_driver_resource",
            (int64_t)godot::RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture, (int64_t)0);
    req->set("texture_format", native_format);
    req->set("destination_resource", texture);
    const bool enqueued = (bool)ds_ctx->call("enqueue_request", req_holder);
    if (!enqueued) {
        GDEX_FAIL(std::string("enqueue_request rejected GPU mip range: ") +
                gdextest::to_std_string((godot::String)req->get("error_message")));
    }
    GDEX_EXPECT_TRUE(enqueued);
    ds_ctx->call("flush_batch");
    GDEX_EXPECT_TRUE((bool)ds_ctx->call("wait_for_completion", 5000));
    GDEX_EXPECT_EQ((int64_t)req->get("status"), (int64_t)godot::DSRequest::STATUS_COMPLETED);
    if ((int64_t)req->get("status") == (int64_t)godot::DSRequest::STATUS_COMPLETED) {
        // texture_get_data's argument is the layer, not the mip. Layer 0
        // returns every mip concatenated, each at its footprint size.
        // texture_get_data returns tightly packed pixels (4 bytes per texel),
        // not the 256-byte-padded footprint the file stores.
        godot::PackedByteArray readback = rd->texture_get_data(texture, 0);
        int64_t read_offset = 0;
        int64_t payload_offset = 0;
        uint32_t w = 128;
        uint32_t h = 96;
        for (int mip = 0; mip < 8; ++mip) {
            godot::DSTexMipLayout layout =
                    godot::DSTex::get_mip_layout(w, h, godot::Image::FORMAT_RGBA8);
            const int64_t tight_size = (int64_t)w * (int64_t)h * 4;
            godot::PackedByteArray expected = payload.slice(
                    payload_offset, payload_offset + (int64_t)layout.footprint_size);
            godot::PackedByteArray mip_read = readback.slice(read_offset, read_offset + tight_size);
            payload_offset += (int64_t)layout.footprint_size;
            read_offset += tight_size;
            godot::PackedByteArray natural =
                    godot::DSTex::unpad_footprint_mip(expected, w, h, godot::Image::FORMAT_RGBA8);
            godot::PackedByteArray got = mip_read;
            GDEX_EXPECT_EQ(got.size(), natural.size());
            if (got.size() == natural.size() && natural.size() > 0 &&
                    memcmp(got.ptr(), natural.ptr(), (size_t)natural.size()) != 0) {
                int64_t diff = 0;
                const uint8_t *a = got.ptr();
                const uint8_t *b = natural.ptr();
                while (diff < natural.size() && a[diff] == b[diff]) {
                    diff++;
                }
                char buf[200];
                snprintf(buf, sizeof(buf),
                        "mip %d byte %lld got %u expected %u (compared %lld, mip readback %lld, footprint %llu)",
                        mip, (long long)diff, diff < natural.size() ? a[diff] : 0,
                        diff < natural.size() ? b[diff] : 0, (long long)natural.size(),
                        (long long)mip_read.size(), (unsigned long long)layout.footprint_size);
                GDEX_FAIL(std::string(buf));
            }
            w = w > 1 ? (w >> 1) : 1;
            h = h > 1 ? (h >> 1) : 1;
        }
    }
    free_texture_rid(texture);
}

GDEX_TEST_T(subresource, gpu_oversized_per_mip_requests, TAG_INTEGRATION | TAG_SLOW) {
    // Ports test_oversized_texture.gd (5000x5000, 14 mips, one TEXTURE_REGION
    // request per mip with first=i, count=1).
    if (!godot::ClassDB::class_exists("DSRequest") || !godot::ClassDB::class_exists("DSContext")) {
        GDEX_SKIP("DSRequest/DSContext classes unavailable (consumer_extension not configured)");
    }
    // Per-mip config round-trip: no driver needed, runs wherever the
    // extension classes are registered.
    // NOTE: 5000x5000 allows floor(log2(5000))+1 = 13 mips; requesting 14
    // makes texture_create fail validation (invalid RID).
    {
        uint32_t w = 5000;
        uint32_t h = 5000;
        for (int mip = 0; mip < 13; ++mip) {
            godot::DSTexMipLayout layout =
                    godot::DSTex::get_mip_layout(w, h, godot::Image::FORMAT_RGBA8);
            GDEX_EXPECT_GT(layout.footprint_size, 0ULL);
            godot::Variant req_holder = godot::ClassDB::instantiate("DSRequest");
            godot::Object *req = req_holder;
            GDEX_ASSERT_NOT_NULL(req);
            req->set("first_subresource", mip);
            req->set("subresource_count", 1);
            req->set("texture_format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
            req->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
            GDEX_EXPECT_EQ((int64_t)req->get("first_subresource"), (int64_t)mip);
            GDEX_EXPECT_EQ((int64_t)req->get("subresource_count"), 1);
            w = w > 1 ? (w >> 1) : 1;
            h = h > 1 ? (h >> 1) : 1;
        }
    }
    godot::Object *ds_ctx = ds_singleton("DSContext");
    if (ds_ctx == nullptr) {
        GDEX_SKIP("DSContext singleton unavailable");
    }
    if (!(bool)ds_ctx->call("initialize_ds") || !(bool)ds_ctx->call("is_d3d12_backend")) {
        GDEX_SKIP("D3D12 DirectStorage backend unavailable");
    }
    if (godot::OS::get_singleton()->get_environment("DS_GDEX_RUN_LARGE") != godot::String("1")) {
        GDEX_SKIP("128+ MiB upload is opt-in via DS_GDEX_RUN_LARGE=1 (mirrors --run-large)");
    }
    godot::RenderingDevice *rd = godot::RenderingServer::get_singleton()->get_rendering_device();
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable");
    }
    godot::Ref<godot::RDTextureFormat> fmt;
    fmt.instantiate();
    godot::Ref<godot::RDTextureView> view;
    view.instantiate();
    fmt->set("format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt->set("width", 5000);
    fmt->set("height", 5000);
    fmt->set("mipmaps", 13);
    fmt->set("usage_bits", (int64_t)(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
    godot::RID texture = rd->texture_create(fmt, view);
    GDEX_ASSERT_TRUE(texture.is_valid());
    // Native texture data format for the metadata check (mirrors
    // gpu_mip_range_submit): the backend rejects a hardcoded enum that does
    // not match the driver's mapped format.
    godot::Variant native_format = rd->call("get_driver_resource",
            (int64_t)godot::RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture, (int64_t)0);
    // Payloads stream through one mip at a time (peak host footprint ~96 MiB
    // for mip0); request handles stay alive through completion.
    // GPU-texture destinations require a v2 GDeflate .dstex with mip entries
    // (Win32Backend::enqueue_request rejects raw payloads), so the full chain
    // is written once as a 13-mip .dstex and each request addresses its mip by
    // file offset — mirroring how the backend splits multi-mip textures.
    godot::Variant held[13];
    godot::Ref<godot::DSTex> mip_dstex;
    mip_dstex.instantiate();
    GDEX_ASSERT_TRUE(mip_dstex.is_valid());
    godot::PackedByteArray chain_payload;
    {
        uint32_t w = 5000;
        uint32_t h = 5000;
        for (int mip = 0; mip < 13; ++mip) {
            godot::PackedByteArray mip_payload =
                    make_pattern_footprint(w, h, godot::Image::FORMAT_RGBA8);
            const int64_t old_size = chain_payload.size();
            chain_payload.resize(old_size + mip_payload.size());
            memcpy(chain_payload.ptrw() + old_size, mip_payload.ptr(), (size_t)mip_payload.size());
            w = w > 1 ? (w >> 1) : 1;
            h = h > 1 ? (h >> 1) : 1;
        }
    }
    const godot::String kChainPath = "user://gdex_oversized_chain.dstex";
    GDEX_ASSERT_EQ((int64_t)mip_dstex->write_to_file(kChainPath, chain_payload, 5000, 5000,
                    godot::Image::FORMAT_RGBA8, 13, 1), (int64_t)godot::OK);
    godot::Dictionary chain_header = mip_dstex->read_header(kChainPath);
    GDEX_ASSERT_EQ((int64_t)chain_header.get("version", 0), (int64_t)2);
    GDEX_ASSERT_EQ((int64_t)chain_header.get("mip_count", 0), (int64_t)13);
    godot::PackedInt64Array chain_offsets = chain_header.get("mip_offsets", godot::PackedInt64Array());
    godot::PackedInt32Array chain_sizes = chain_header.get("mip_sizes", godot::PackedInt32Array());
    godot::PackedInt32Array chain_uncompressed = chain_header.get("mip_uncompressed_sizes", godot::PackedInt32Array());
    GDEX_ASSERT_EQ(chain_offsets.size(), 13);
    for (int mip = 0; mip < 13; ++mip) {
        godot::Variant req_holder = godot::ClassDB::instantiate("DSRequest");
        godot::Object *req = req_holder;
        GDEX_ASSERT_NOT_NULL(req);
        req->set("file_path", kChainPath);
        req->set("offset", chain_offsets[mip]);
        req->set("size", chain_sizes[mip]);
        req->set("uncompressed_size", chain_uncompressed[mip]);
        req->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
        req->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
        req->set("destination_resource", texture);
        req->set("first_subresource", mip);
        req->set("subresource_count", 1);
        req->set("texture_format", native_format);
        const bool enqueued = (bool)ds_ctx->call("enqueue_request", req_holder);
        if (!enqueued) {
            GDEX_FAIL(std::string("enqueue_request rejected oversized mip ") +
                    std::to_string(mip) + ": " +
                    gdextest::to_std_string((godot::String)req->get("error_message")));
        }
        GDEX_EXPECT_TRUE(enqueued);
        held[mip] = req_holder;
    }
    ds_ctx->call("flush_batch");
    GDEX_EXPECT_TRUE((bool)ds_ctx->call("wait_for_completion", 30000));
    for (int mip = 0; mip < 13; ++mip) {
        godot::Object *req = held[mip];
        GDEX_EXPECT_EQ((int64_t)req->get("status"), (int64_t)godot::DSRequest::STATUS_COMPLETED);
    }
    free_texture_rid(texture);
}

GDEX_TEST_T(subresource, gpu_bracket_barrier_multi_texture_async, TAG_INTEGRATION | TAG_SLOW) {
    // Tests D3D12 bracket barrier transitions:
    // 1. Multi-texture batching: Transitions 2 distinct hardware textures in one batch.
    // 2. Async dispatch: Verifies worker-thread -> render-thread barrier dispatch
    //    via s_barrier_dispatch_ctx and condition variable synchronization in submit_async().
    // 3. Sequential batch reuse: Submits a second batch immediately to verify
    //    D3D12BarrierContext command allocator reset and fence monotonicity.
    // 4. Data integrity: Verifies readback matches written pattern payload for both textures.
    if (!godot::ClassDB::class_exists("DSRequest") || !godot::ClassDB::class_exists("DSContext")) {
        GDEX_SKIP("DSRequest/DSContext classes unavailable");
    }
    godot::Object *ds_ctx = ds_singleton("DSContext");
    if (ds_ctx == nullptr) {
        GDEX_SKIP("DSContext singleton unavailable");
    }
    if (!(bool)ds_ctx->call("initialize_ds") || !(bool)ds_ctx->call("is_d3d12_backend")) {
        GDEX_SKIP("D3D12 DirectStorage backend unavailable");
    }

    godot::RenderingDevice *rd = godot::RenderingServer::get_singleton()->get_rendering_device();
    if (rd == nullptr) {
        GDEX_SKIP("RenderingDevice unavailable");
    }

    // 1. Create two distinct textures (Texture A: 64x64, Texture B: 128x128).
    godot::Ref<godot::RDTextureFormat> fmt_a;
    fmt_a.instantiate();
    godot::Ref<godot::RDTextureView> view_a;
    view_a.instantiate();
    fmt_a->set("format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt_a->set("width", 64);
    fmt_a->set("height", 64);
    fmt_a->set("mipmaps", 1);
    fmt_a->set("usage_bits", (int64_t)(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
    godot::RID texture_a = rd->texture_create(fmt_a, view_a);
    GDEX_ASSERT_TRUE(texture_a.is_valid());

    godot::Ref<godot::RDTextureFormat> fmt_b;
    fmt_b.instantiate();
    godot::Ref<godot::RDTextureView> view_b;
    view_b.instantiate();
    fmt_b->set("format", (int64_t)godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
    fmt_b->set("width", 128);
    fmt_b->set("height", 128);
    fmt_b->set("mipmaps", 1);
    fmt_b->set("usage_bits", (int64_t)(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
                                            godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT));
    godot::RID texture_b = rd->texture_create(fmt_b, view_b);
    GDEX_ASSERT_TRUE(texture_b.is_valid());

    godot::Variant native_format_a = rd->call("get_driver_resource",
            (int64_t)godot::RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture_a, (int64_t)0);
    godot::Variant native_format_b = rd->call("get_driver_resource",
            (int64_t)godot::RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, texture_b, (int64_t)0);

    // 2. Build payload files (.dstex v2 with footprint).
    godot::PackedByteArray payload_a = make_pattern_footprint(64, 64, godot::Image::FORMAT_RGBA8);
    const godot::String kPathA = "user://gdex_multi_tex_a.dstex";
    godot::Ref<godot::DSTex> dstex_a;
    dstex_a.instantiate();
    GDEX_ASSERT_EQ((int64_t)dstex_a->write_to_file(kPathA, payload_a, 64, 64, godot::Image::FORMAT_RGBA8, 1, 1), (int64_t)godot::OK);
    godot::Dictionary header_a = dstex_a->read_header(kPathA);
    godot::PackedInt64Array offsets_a = header_a.get("mip_offsets", godot::PackedInt64Array());
    godot::PackedInt32Array sizes_a = header_a.get("mip_sizes", godot::PackedInt32Array());
    godot::PackedInt32Array unc_a = header_a.get("mip_uncompressed_sizes", godot::PackedInt32Array());
    GDEX_ASSERT_EQ(offsets_a.size(), 1);

    godot::PackedByteArray payload_b = make_pattern_footprint(128, 128, godot::Image::FORMAT_RGBA8);
    const godot::String kPathB = "user://gdex_multi_tex_b.dstex";
    godot::Ref<godot::DSTex> dstex_b;
    dstex_b.instantiate();
    GDEX_ASSERT_EQ((int64_t)dstex_b->write_to_file(kPathB, payload_b, 128, 128, godot::Image::FORMAT_RGBA8, 1, 1), (int64_t)godot::OK);
    godot::Dictionary header_b = dstex_b->read_header(kPathB);
    godot::PackedInt64Array offsets_b = header_b.get("mip_offsets", godot::PackedInt64Array());
    godot::PackedInt32Array sizes_b = header_b.get("mip_sizes", godot::PackedInt32Array());
    godot::PackedInt32Array unc_b = header_b.get("mip_uncompressed_sizes", godot::PackedInt32Array());
    GDEX_ASSERT_EQ(offsets_b.size(), 1);

    // 3. Batch 1: Enqueue both Texture A and Texture B concurrently in one batch.
    godot::Variant req_a_holder = godot::ClassDB::instantiate("DSRequest");
    godot::Object *req_a = req_a_holder;
    req_a->set("file_path", kPathA);
    req_a->set("offset", offsets_a[0]);
    req_a->set("size", sizes_a[0]);
    req_a->set("uncompressed_size", unc_a[0]);
    req_a->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
    req_a->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
    req_a->set("destination_resource", texture_a);
    req_a->set("first_subresource", 0);
    req_a->set("subresource_count", 1);
    req_a->set("texture_format", native_format_a);

    godot::Variant req_b_holder = godot::ClassDB::instantiate("DSRequest");
    godot::Object *req_b = req_b_holder;
    req_b->set("file_path", kPathB);
    req_b->set("offset", offsets_b[0]);
    req_b->set("size", sizes_b[0]);
    req_b->set("uncompressed_size", unc_b[0]);
    req_b->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
    req_b->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
    req_b->set("destination_resource", texture_b);
    req_b->set("first_subresource", 0);
    req_b->set("subresource_count", 1);
    req_b->set("texture_format", native_format_b);

    GDEX_ASSERT_TRUE((bool)ds_ctx->call("enqueue_request", req_a_holder));
    GDEX_ASSERT_TRUE((bool)ds_ctx->call("enqueue_request", req_b_holder));

    // Submit asynchronously (tests worker-thread post-DMA barrier restoration).
    godot::Error flush_err = (godot::Error)(int64_t)ds_ctx->call("flush_batch_async");
    GDEX_ASSERT_EQ((int64_t)flush_err, (int64_t)godot::OK);

    GDEX_EXPECT_TRUE((bool)ds_ctx->call("wait_for_completion", 10000));
    for (int i = 0; i < 100 && !(bool)ds_ctx->call("is_batch_complete"); ++i) {
        ds_ctx->call("collect_completed_results");
        godot::OS::get_singleton()->delay_usec(50000);
    }
    ds_ctx->call("collect_completed_results");
    GDEX_EXPECT_TRUE((bool)ds_ctx->call("is_batch_complete"));
    GDEX_EXPECT_EQ((int64_t)req_a->get("status"), (int64_t)godot::DSRequest::STATUS_COMPLETED);
    GDEX_EXPECT_EQ((int64_t)req_b->get("status"), (int64_t)godot::DSRequest::STATUS_COMPLETED);

    // 4. Batch 2: Immediate sequential synchronous batch to test allocator reset and fence monotonicity.
    godot::Variant req_a2_holder = godot::ClassDB::instantiate("DSRequest");
    godot::Object *req_a2 = req_a2_holder;
    req_a2->set("file_path", kPathA);
    req_a2->set("offset", offsets_a[0]);
    req_a2->set("size", sizes_a[0]);
    req_a2->set("uncompressed_size", unc_a[0]);
    req_a2->set("compression_format", (int64_t)godot::DSRequest::COMPRESSION_GDEFLATE);
    req_a2->set("destination_type", (int64_t)godot::DSRequest::DESTINATION_GPU_TEXTURE);
    req_a2->set("destination_resource", texture_a);
    req_a2->set("first_subresource", 0);
    req_a2->set("subresource_count", 1);
    req_a2->set("texture_format", native_format_a);

    GDEX_ASSERT_TRUE((bool)ds_ctx->call("enqueue_request", req_a2_holder));
    ds_ctx->call("flush_batch");
    GDEX_EXPECT_TRUE((bool)ds_ctx->call("wait_for_completion", 10000));
    GDEX_EXPECT_EQ((int64_t)req_a2->get("status"), (int64_t)godot::DSRequest::STATUS_COMPLETED);

    // 5. Readback verification for both textures.
    {
        godot::PackedByteArray readback_a = rd->texture_get_data(texture_a, 0);
        godot::PackedByteArray natural_a = godot::DSTex::unpad_footprint_mip(payload_a, 64, 64, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_EQ(readback_a.size(), natural_a.size());
        if (readback_a.size() == natural_a.size() && natural_a.size() > 0) {
            GDEX_EXPECT_EQ(memcmp(readback_a.ptr(), natural_a.ptr(), (size_t)natural_a.size()), 0);
        }
    }
    {
        godot::PackedByteArray readback_b = rd->texture_get_data(texture_b, 0);
        godot::PackedByteArray natural_b = godot::DSTex::unpad_footprint_mip(payload_b, 128, 128, godot::Image::FORMAT_RGBA8);
        GDEX_EXPECT_EQ(readback_b.size(), natural_b.size());
        if (readback_b.size() == natural_b.size() && natural_b.size() > 0) {
            GDEX_EXPECT_EQ(memcmp(readback_b.ptr(), natural_b.ptr(), (size_t)natural_b.size()), 0);
        }
    }

    free_texture_rid(texture_a);
    free_texture_rid(texture_b);
}

