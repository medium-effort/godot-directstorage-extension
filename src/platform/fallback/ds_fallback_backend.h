#ifndef DS_FALLBACK_BACKEND_H
#define DS_FALLBACK_BACKEND_H

#include "../platform_backend.h"
#include <godot_cpp/templates/vector.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace godot {

class FallbackBackend : public PlatformBackend {
private:
	Vector<Ref<DSRequest>> pending_requests;
	uint32_t num_workers = 4;
	std::deque<Vector<Ref<DSRequest>>> completion_queue;
	std::deque<Vector<Ref<DSRequest>>> completed_batches;
	std::thread completion_thread;
	std::atomic<bool> batch_in_flight{false};
	std::atomic<bool> completion_thread_stop{false};
	std::mutex batch_mutex;
	std::condition_variable batch_cv;

	void completion_thread_func();

public:
	FallbackBackend();
	~FallbackBackend() override = default;

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	void shutdown() override;

	bool enqueue_request(Ref<DSRequest> p_request) override;
	void submit_batch() override;
	void submit_async() override;
	bool is_batch_complete() const override;
	void collect_completed_results() override;
	void cancel_requests_with_tag(uint64_t p_tag) override;

private:
	bool process_request(Ref<DSRequest> p_request);
};

} // namespace godot

#endif // DS_FALLBACK_BACKEND_H
