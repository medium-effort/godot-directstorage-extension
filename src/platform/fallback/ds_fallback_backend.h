#ifndef DS_FALLBACK_BACKEND_H
#define DS_FALLBACK_BACKEND_H

#include "../platform_backend.h"
#include <godot_cpp/templates/vector.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace godot {

class FallbackBackend : public PlatformBackend {
public:
	// Per-batch tag + request-count accounting for the async path. Mirrors
	// the Win32 backend's progress contract so
	// DSContext::get_prewarm_progress(tag) converges: every submitted async
	// request reaches a finished status (COMPLETED/FAILED), including
	// requests cancelled after submit (marked FAILED, buffers dropped,
	// never written). Lock discipline: batch_mutex guards everything
	// below except the atomics; it is never held across process_request() I/O.
	struct AsyncBatchInfo {
		uint64_t batch_id = 0;
		size_t total_requests = 0;
		size_t remaining_requests = 0;
		std::unordered_map<uint64_t, size_t> per_tag_counts;
	};

private:
	struct QueuedRequest {
		Ref<DSRequest> request;
		uint64_t batch_id = 0;
	};
	struct WorkerScratch;

	static constexpr size_t MAX_COMPLETED_BATCHES = 64;
	static constexpr size_t MAX_CANCELLED_TAGS = 1024;

	Vector<Ref<DSRequest>> pending_requests;
	uint32_t num_workers = 4;
	std::deque<QueuedRequest> request_queue;
	std::deque<AsyncBatchInfo> in_flight_batch_infos;
	std::deque<AsyncBatchInfo> completed_batch_infos;
	uint64_t next_async_batch_id = 1;
	uint64_t async_batches_submitted = 0;
	// Tags cancelled via cancel_requests_with_tag(). Lets workers drop a
	// request that was already popped from request_queue (mid-processing
	// race) before it writes destination buffers. Tags are context-global
	// monotonic (never reused), so membership never goes stale; bounded with
	// best-effort eviction (status checks still cover queued requests).
	std::unordered_set<uint64_t> cancelled_tags;
	std::vector<std::thread> worker_threads;
	std::atomic<bool> batch_in_flight{false};
	size_t active_workers = 0;
	std::atomic<bool> completion_thread_stop{false};
	// active_workers tracks workers currently executing popped requests
	// (queue empty but work ongoing). Keeps batch_in_flight/is_batch_complete
	// honest when cancel drains the queue mid-processing.
	mutable std::mutex batch_mutex;
	std::condition_variable batch_cv;

	void worker_thread_func();
	void finish_batch_request_locked(uint64_t p_batch_id);
	// True when the worker must drop the request without touching buffers:
	// null, already terminal (cancelled while queued), or tag cancelled
	// after the request was popped. When p_mark is true and the request is
	// still SUBMITTED under a cancelled tag, marks it terminal FAILED.
	bool should_drop_request(const Ref<DSRequest> &p_request, bool p_mark);
	bool is_tag_cancelled_locked(uint64_t p_tag) const;

public:
	FallbackBackend();
	~FallbackBackend() override;

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	void shutdown() override;

	bool enqueue_request(Ref<DSRequest> p_request, PlatformBackend::EnqueueOutcome *r_outcome = nullptr) override;
	void submit_batch() override;
	bool submit_async() override;
	bool is_batch_complete() const override;
	void collect_completed_results() override;
	void cancel_requests_with_tag(uint64_t p_tag) override;

	// Progress-ready observability for the async path (lock-protected).
	uint64_t get_async_batches_submitted() const;
	size_t get_in_flight_async_batch_count() const;

private:
	bool process_request(Ref<DSRequest> p_request, WorkerScratch *p_scratch = nullptr);
};

} // namespace godot

#endif // DS_FALLBACK_BACKEND_H
