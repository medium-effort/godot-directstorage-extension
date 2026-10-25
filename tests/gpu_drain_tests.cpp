#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "core/ds_context.h"
#include "core/ds_request.h"
#include "formats/dstex_format.h"
#include "integration/ds_texture_cache.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>
#include <string>

// Deferred-RID release queue (#0126 drain hardening).
//
// Suite is named gpu_drain, not drain, so the default -Filter 'gpu_*' of
// .vscode/run_d3d12_windowed.ps1 actually selects it.
//
// Needs a live D3D12 RenderingDevice: SKIPs under the normal headless run and
// is exercised with .vscode/run_d3d12_windowed.ps1.
//
// Pins three things: an RID released while a submitted request still targets it
// is queued instead of freed; the teardown flush respects the "DS shutdown did
// not drain" flag (leave the queue alone) and frees only after a clean drain;
// and the request that was in flight really completed, so the deferred state is
// never reached with a dead request.
//
// Environment assumption (GODOT_QUIRKS.md section 2): in Godot's default Safe
// thread model the main thread IS the render/server thread. That is what allows
// free_rid()/texture_is_valid() from a test body and is now required by
// flush_deferred_rid_releases(), not merely convenient.

namespace {

// 2048^2 RGBA8, one mip, an incompressible LCG payload: the DMA + GDeflate
// decode window is tens of milliseconds, which is what makes "the request is
// still in flight when we release the RID" deterministic rather than a race.
constexpr int DRAIN_TEX = 2048;
// The observation still races the DMA retiring, so a lost race retries with a
// fresh texture instead of failing the suite. A failed request never retries.
constexpr int DEFERRAL_ATTEMPTS = 3;

godot::PackedByteArray make_noise_rgba8(int p_size) {
	godot::PackedByteArray data;
	data.resize(static_cast<int64_t>(p_size) * static_cast<int64_t>(p_size) * 4);
	uint8_t *w = data.ptrw();
	uint32_t state = 0x9E3779B9u;
	for (int64_t i = 0; i < data.size(); ++i) {
		state = state * 1664525u + 1013904223u;
		w[i] = static_cast<uint8_t>(state >> 24);
	}
	return data;
}

} // namespace

GDEX_TEST_T(gpu_drain, flush_honours_unclean_ds_flag, TAG_INTEGRATION) {
	godot::RenderingServer *rs = godot::RenderingServer::get_singleton();
	godot::RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (rd == nullptr) {
		GDEX_SKIP("RenderingDevice unavailable (run windowed with --rendering-driver d3d12)");
	}
	godot::DSContext *ds = godot::DSContext::get_singleton();
	if (ds == nullptr || !ds->initialize_ds() || !ds->is_d3d12_backend() || !ds->is_gpu_decompression_enabled()) {
		GDEX_SKIP("D3D12 GPU decompression unavailable on this machine");
	}

	const godot::String fixture("user://gdextest_drain_flush.dstex");

	// Shared RID state + the ONLY teardown for this test. `abandoned` marks the
	// case where a request may still be STAGED in the context and could be
	// flushed later: freeing it while DS is live would be a use-after-free, so
	// the pre-shutdown pass skips it and the post-shutdown pass reclaims it.
	struct RidGuard {
		godot::RID rid;
		bool abandoned = false;
	};
	auto guard = std::make_shared<RidGuard>();

	// ONE teardown, with its steps ordered internally on purpose:
	//   1. free the RID while DS is still live (is_resource_in_flight() is the
	//      only safe liveness test, and it short-circuits to false once the
	//      context is shut down),
	//   2. shut DirectStorage down, which drops the backend's cached
	//      IDStorageFile handles and answers the "is DMA still live?" question,
	//   3. reclaim whatever step 1 had to skip, but only on a clean drain,
	//   4. delete the ~16 MiB fixture.
	// Folding these together removes any dependency on gdextest's LIFO teardown
	// order: a separate shutdown teardown that happened to run first would make
	// the liveness test below return false and let a RID under live DMA be
	// freed - the exact bug this suite exists to prove is handled.
	ctx.add_teardown([rd, ds, guard, fixture]() {
		if (!guard->abandoned && guard->rid.is_valid() && rd->texture_is_valid(guard->rid)) {
			// Never free just because the wait returned: a timed-out wait
			// (GPU hang/TDR) still leaves live DMA, and a leak on a failure
			// path is the safer outcome.
			ds->wait_for_completion(5000);
			ds->collect_completed_results();
			if (!ds->is_resource_in_flight(guard->rid) && rd->texture_is_valid(guard->rid)) {
				rd->free_rid(guard->rid);
			}
		}
		ds->shutdown_ds();
		// Step 1 refused two kinds of RID: an abandoned one (its request was
		// still staged in the context) and one still in flight. shutdown_ds()
		// has now dropped the staged request and, on a clean drain, retired
		// every in-flight one - so the DMA question is answered and both can be
		// reclaimed. A drain that timed out means DMA may still be live, so that
		// leak is kept on purpose (same rule as flush_deferred_rid_releases()).
		if (ds->was_last_shutdown_clean() && guard->rid.is_valid() && rd->texture_is_valid(guard->rid)) {
			rd->free_rid(guard->rid);
		}
		godot::ProjectSettings *ps = godot::ProjectSettings::get_singleton();
		if (ps) {
			godot::DirAccess::remove_absolute(ps->globalize_path(fixture));
		}
	});
	godot::RID &texture_rid = guard->rid;

	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	const godot::PackedByteArray raw = make_noise_rgba8(DRAIN_TEX);
	const godot::Error write_err = dstex->write_to_file(fixture, raw, DRAIN_TEX, DRAIN_TEX, godot::Image::FORMAT_RGBA8, 1, 1);
	GDEX_ASSERT_EQ(static_cast<int64_t>(write_err), static_cast<int64_t>(godot::OK));

	const godot::Dictionary header = dstex->read_header(fixture);
	GDEX_ASSERT_TRUE(!header.is_empty());
	const int mip_count = header["mip_count"];
	const godot::PackedInt64Array mip_offsets = header["mip_offsets"];
	const godot::PackedInt32Array mip_sizes = header["mip_sizes"];
	const uint64_t original_size = header["original_size"];
	GDEX_ASSERT_TRUE(mip_count >= 1);
	GDEX_ASSERT_TRUE(mip_offsets.size() >= mip_count);
	GDEX_ASSERT_TRUE(mip_sizes.size() >= mip_count);

	godot::Ref<godot::RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_width(DRAIN_TEX);
	fmt->set_height(DRAIN_TEX);
	fmt->set_format(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	fmt->set_mipmaps(mip_count);
	fmt->set_texture_type(godot::RenderingDevice::TEXTURE_TYPE_2D);
	fmt->set_usage_bits(godot::RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			godot::RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	godot::Ref<godot::RDTextureView> view;
	view.instantiate();

	const uint64_t start = static_cast<uint64_t>(mip_offsets[0]);
	const uint64_t end = static_cast<uint64_t>(mip_offsets[mip_count - 1]) + static_cast<uint64_t>(mip_sizes[mip_count - 1]);

	bool in_flight = false;
	godot::Ref<godot::DSRequest> request;
	for (int attempt = 0; attempt < DEFERRAL_ATTEMPTS && !in_flight; ++attempt) {
		texture_rid = rd->texture_create(fmt, view, godot::TypedArray<godot::PackedByteArray>());
		GDEX_ASSERT_TRUE(texture_rid.is_valid());

		request.instantiate();
		request->set_file_path(fixture);
		request->set_offset(start);
		request->set_size(static_cast<uint32_t>(end - start));
		request->set_uncompressed_size(static_cast<uint32_t>(original_size));
		request->set_destination_type(godot::DSRequest::DESTINATION_GPU_TEXTURE);
		request->set_destination_resource(texture_rid);
		request->set_first_subresource(0);
		request->set_subresource_count(static_cast<uint32_t>(mip_count));
		request->set_compression_format(godot::DSRequest::COMPRESSION_GDEFLATE);
		request->set_texture_format(0); // 0 => the backend skips the driver-format equality check.
		if (!ds->enqueue_request(request)) {
			const std::string why = ::gdextest::to_std_string(request->get_error_message());
			GDEX_FAIL(std::string("enqueue_request rejected the GPU texture request: ") + why);
			return;
		}
		godot::Error flush_err = ds->flush_batch_async();
		if (flush_err == godot::ERR_BUSY) {
			// ERR_BUSY is the documented transient (a batch is already in
			// flight; the staged requests are kept). Drain it and retry once so
			// the test does not depend on which suite ran before it.
			ds->wait_for_completion(5000);
			ds->collect_completed_results();
			flush_err = ds->flush_batch_async();
		}
		if (flush_err != godot::OK) {
			// The request was staged but not submitted, so a later flush (batch
			// manager tick or another suite) could still enqueue DMA into this
			// destination. Mark the RID abandoned: the guard must not free it.
			guard->abandoned = true;
			ds->wait_for_completion(5000);
			ds->collect_completed_results();
			GDEX_FAIL(std::string("flush_batch_async failed with error ") + std::to_string(static_cast<int>(flush_err)));
			return;
		}

		in_flight = ds->is_resource_in_flight(texture_rid);
		if (in_flight) {
			break;
		}

		// Lost the race, or the request failed straight away. Retire it; a
		// failed request is a defect, not a race to retry.
		ds->wait_for_completion(5000);
		ds->collect_completed_results();
		if (request->get_status() == godot::DSRequest::STATUS_FAILED) {
			const std::string why = ::gdextest::to_std_string(request->get_error_message());
			GDEX_FAIL(std::string("GPU texture request failed: ") + why);
			return;
		}
		if (request->get_status() != godot::DSRequest::STATUS_COMPLETED) {
			// Timed out while still submitted: freeing here would race live DMA.
			// The teardown guard re-checks in-flight state and frees only if the
			// request has retired by then.
			GDEX_FAIL("GPU request did not reach a terminal state; its RID is left to the teardown guard");
			return;
		}
		if (rd->texture_is_valid(texture_rid)) {
			rd->free_rid(texture_rid);
		}
	}
	if (!in_flight) {
		GDEX_FAIL("could not observe the GPU request in flight in 3 attempts; the deferral path was not exercised");
		return;
	}

	// Released while in flight: must be queued, NOT freed.
	godot::DSTextureCache::release_owned_rid(texture_rid);
	GDEX_EXPECT_TRUE(rd->texture_is_valid(texture_rid));

	// Unclean DS shutdown (hardware DMA possibly live): leave the queue alone.
	godot::DSTextureCache::flush_deferred_rid_releases(false);
	GDEX_EXPECT_TRUE(rd->texture_is_valid(texture_rid));

	// Retire the request, prove it actually completed, then a clean flush must
	// free the queued RID. Without the status check a dead request would satisfy
	// the same valid -> invalid transition.
	ds->wait_for_completion(5000);
	ds->collect_completed_results();
	GDEX_EXPECT_EQ(static_cast<int64_t>(request->get_status()), static_cast<int64_t>(godot::DSRequest::STATUS_COMPLETED));
	godot::DSTextureCache::flush_deferred_rid_releases(true);
	GDEX_EXPECT_FALSE(rd->texture_is_valid(texture_rid));
}
