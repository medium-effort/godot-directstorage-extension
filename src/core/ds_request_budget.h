// Shared request-budget arithmetic for finding 1.3 (Phase A).
//
// Three distinct budgets must not be conflated:
//   1. slot admission ceiling - hard hardware bound, shared with Win32Backend;
//   2. flush granularity - tuned for two-slot + worker-build pipeline overlap;
//   3. GPU callback slice - bounds render-thread texture_create/decompress work.
// Background-paced gameplay keeps strict small caps on every device; only
// loading/uncapped streaming scales with queue capacity.

#ifndef DS_REQUEST_BUDGET_H
#define DS_REQUEST_BUDGET_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dsgd_budget {

inline size_t per_slot_admission_capacity(uint32_t p_capacity) {
	return p_capacity >= 8u ? static_cast<size_t>(p_capacity) / 2u : static_cast<size_t>(p_capacity);
}

// Maximum logical/child operations one backend batch slot can hold after
// reserving two queue operations for EnqueueSignal + EnqueueSetEvent.
inline size_t per_slot_request_ceiling(uint32_t p_capacity) {
	const size_t slot = per_slot_admission_capacity(p_capacity);
	return slot < 3u ? 1u : slot - 2u;
}

// Subresource-cost ceiling for flush_stream_async(), aligned to the 2-op
// signal overhead so multi-mip batches cannot trip TRANSIENT tail requeues.
inline size_t per_flush_subresource_budget(uint16_t p_capacity) {
	return std::min<size_t>(1024u, per_slot_request_ceiling(p_capacity));
}

// Logical requests one flush_stream_async() call may drain.
// Background pacing caps at 128 to prevent bursts on large scenes; loading
// scales at cap/4 so medium/large scenes still split across both 1.1 slots.
inline size_t per_flush_request_budget(uint16_t p_capacity, bool p_background_pacing) {
	const size_t subresource_budget = per_flush_subresource_budget(p_capacity);
	const size_t target = p_background_pacing
			? 128u
			: std::max<size_t>(128u, static_cast<size_t>(p_capacity) / 4u);
	return std::min(subresource_budget, target);
}

// Requests one render-thread GPU prewarm callback may stage. Background
// pacing caps at 64; loading scales conservatively at cap/8 and never exceeds
// one backend slot's ceiling.
inline size_t gpu_callback_request_budget(uint16_t p_capacity, bool p_background_pacing) {
	const size_t slot_ceiling = per_slot_request_ceiling(p_capacity);
	const size_t target = p_background_pacing
			? 64u
			: std::max<size_t>(64u, static_cast<size_t>(p_capacity) / 8u);
	return std::min(slot_ceiling, target);
}

} // namespace dsgd_budget

#endif // DS_REQUEST_BUDGET_H
