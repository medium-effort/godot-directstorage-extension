#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "platform/win32/ds_render_handoff_queue.h"

#include <cstddef>
#include <memory>

// Render-thread handoff regression guard.
//
// The Win32 destination-resource handoff used to be a single slot. Two submits
// that resolved different destination RIDs before the render thread ran
// overwrote each other; the callback serviced the newer context, and the older
// caller burned its whole 250 ms budget waiting for a completion that could
// never arrive, then rejected a perfectly valid request with "GPU buffer/texture
// RID could not be mapped to an ID3D12Resource".
//
// The fix parks one context per submit in a FIFO. These tests pin the two
// properties that make it correct and that a single slot silently violated:
//   1. N pushes are serviced by N pops, in order - nobody is overwritten.
//   2. A submitter that gives up prunes its own entry, so a late callback cannot
//      service a caller that already returned, and later submitters are not
//      shifted behind a ghost entry.
//
// No D3D12, no RenderingDevice, no Windows headers are involved: the queue is a
// plain container, which is why these run on every platform in the headless suite.
namespace {

struct ProbeContext {
	int id = 0;
};

using ProbeQueue = godot::DSRenderHandoffQueue<ProbeContext>;

} // namespace

GDEX_TEST(render_handoff_queue, concurrent_pushes_are_all_serviced) {
	(void)ctx;
	ProbeQueue queue;
	auto first = std::make_shared<ProbeContext>();
	auto second = std::make_shared<ProbeContext>();
	first->id = 1;
	second->id = 2;

	// Two concurrent submits park two contexts. The single slot kept only `second`.
	queue.push(first);
	queue.push(second);
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(2));

	// One posted render-thread callback per submit, so one pop per callback.
	std::shared_ptr<ProbeContext> serviced_first = queue.take_front();
	std::shared_ptr<ProbeContext> serviced_second = queue.take_front();
	GDEX_EXPECT_TRUE(serviced_first != nullptr);
	GDEX_EXPECT_TRUE(serviced_second != nullptr);
	if (serviced_first && serviced_second) {
		GDEX_EXPECT_EQ(serviced_first->id, 1);
		GDEX_EXPECT_EQ(serviced_second->id, 2);
	}
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(0));

	// A callback that still fires after the queue drained must be a no-op.
	GDEX_EXPECT_TRUE(queue.take_front() == nullptr);
}

GDEX_TEST(render_handoff_queue, timed_out_submitter_leaves_no_ghost) {
	(void)ctx;
	ProbeQueue queue;
	auto abandoned = std::make_shared<ProbeContext>();
	auto survivor = std::make_shared<ProbeContext>();
	abandoned->id = 1;
	survivor->id = 2;

	queue.push(abandoned);
	queue.push(survivor);

	// The abandoned caller timed out and gave up on its own context.
	GDEX_EXPECT_TRUE(queue.prune(abandoned));
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(1));

	// Its late callback now pops the survivor instead. That is harmless - each
	// context carries its own RID and results - and the following callback finds
	// an empty queue rather than the abandoned context.
	std::shared_ptr<ProbeContext> popped = queue.take_front();
	GDEX_EXPECT_TRUE(popped == survivor);
	GDEX_EXPECT_TRUE(queue.take_front() == nullptr);

	// Pruning again (the callback already took it, or the timeout ran twice) is a
	// no-op, not corruption.
	GDEX_EXPECT_FALSE(queue.prune(abandoned));
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(0));
}

GDEX_TEST(render_handoff_queue, clear_drops_every_parked_context) {
	(void)ctx;
	ProbeQueue queue;
	GDEX_EXPECT_TRUE(queue.take_front() == nullptr);
	GDEX_EXPECT_FALSE(queue.prune(std::make_shared<ProbeContext>()));

	queue.push(std::make_shared<ProbeContext>());
	queue.push(std::make_shared<ProbeContext>());
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(2));

	// Module teardown path (ds_win32_release_static_d3d12_state): draining must
	// leave nothing for a later callback to service.
	queue.clear();
	GDEX_EXPECT_EQ(queue.size(), static_cast<size_t>(0));
	GDEX_EXPECT_TRUE(queue.take_front() == nullptr);
}
