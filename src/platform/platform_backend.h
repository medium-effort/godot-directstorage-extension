#ifndef PLATFORM_BACKEND_H
#define PLATFORM_BACKEND_H

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include "../core/ds_request.h"

namespace godot {

class PlatformBackend {
public:
	virtual ~PlatformBackend() = default;

	virtual bool is_available() const = 0;
	virtual String get_name() const = 0;

	virtual bool initialize() = 0;
	virtual void shutdown() = 0;
	// True when the last shutdown() drained every in-flight GPU request. A
	// false result means DMA may still be active in hardware, so device
	// resources must be left alone (Win32Backend's drain-timeout path leaks on
	// purpose for exactly this reason).
	virtual bool was_drain_clean() const { return true; }

	// How a backend refusal should be treated by the caller's retry loop.
	// STAGED is "accepted". TRANSIENT is "resource busy, retry once the slot
	// retires". TERMINAL is "this request cannot succeed, do not retry".
	// A caller that does not ask for the outcome is unaffected; a caller that
	// does must treat an unset value as TERMINAL, so a refusal nobody classified
	// can never be retried forever.
	enum class EnqueueOutcome { STAGED, TRANSIENT, TERMINAL };

	virtual bool enqueue_request(Ref<DSRequest> p_request, EnqueueOutcome *r_outcome = nullptr) = 0;
	virtual void submit_batch() = 0;
	// Submits the staged batch. Returns false when another batch is already in
	// flight and the caller should retry later (transient, requests stay staged).
	virtual bool submit_async() = 0;
	virtual bool is_batch_complete() const = 0;
	virtual void collect_completed_results() = 0;
	virtual void cancel_requests_with_tag(uint64_t p_tag) = 0;
	// Highest completion fence across every queue this backend owns. Backends
	// without a fence model return 0, which DSContext reads as "no fence to wait
	// on". Virtual dispatch is what makes the answer belong to the object the
	// caller holds: there is no process-global "live instance" to compare against.
	virtual uint64_t get_completion_fence() const { return 0; }
	// Bounded wait for the given fence. Backends without a fence model return
	// false; DSContext only calls it when the backend reports a fence.
	virtual bool wait_for_completion(uint64_t /*p_fence_value*/, uint32_t /*p_timeout_msec*/) { return false; }
	// Bounded wait for how far the batch on one priority lane has got. A lane is
	// a backend queue with its own fence, so a slow batch on another lane must not
	// extend this wait. The default is false: a backend without a per-lane fence
	// cannot promise anything narrower than its all-queue signal. An initialized
	// lane with no work is complete, so callers are not forced to pre-check.
	virtual bool wait_for_priority_completion(int /*p_priority*/, uint32_t /*p_timeout_msec*/) { return false; }
	virtual bool is_resource_in_flight(const RID & /*p_rid*/) const { return false; }
};

} // namespace godot

#endif // PLATFORM_BACKEND_H
