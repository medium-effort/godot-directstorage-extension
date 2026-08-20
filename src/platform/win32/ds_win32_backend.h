#ifndef DS_WIN32_BACKEND_H
#define DS_WIN32_BACKEND_H

#include "../platform_backend.h"
#include <godot_cpp/templates/vector.hpp>
#include <atomic>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dstorage.h>
#endif

namespace godot {

#ifdef _WIN32
ID3D12Device *ds_win32_get_d3d12_device();
#endif

class Win32Backend : public PlatformBackend {
private:
#ifdef _WIN32
	static constexpr int QUEUE_COUNT = 3;
	static constexpr int BATCH_SLOT_COUNT = 2;
	struct BatchSlot {
		HANDLE event = nullptr;
		Vector<Ref<DSRequest>> requests;
		Vector<IDStorageFile *> files;
		uint64_t fence_value = 0;
		int completed_count = 0;
		int error_code = 0;
		String error_message;
		bool submitted = false;
		bool completion_ready = false;
	};
	struct QueueState {
		IDStorageQueue1 *queue = nullptr;
		HANDLE completion_event = nullptr;
		ID3D12Fence *completion_fence = nullptr;
		uint64_t completion_fence_value = 0;
		uint64_t completed_fence_value = 0;
		std::array<BatchSlot, BATCH_SLOT_COUNT> batch_slots;
	};

	IDStorageFactory *factory = nullptr;
	std::array<QueueState, QUEUE_COUNT> priority_queues;
	HMODULE dstoragecore_module = nullptr;
	HMODULE dstorage_module = nullptr;
	struct BatchMetadata {
		int queue_index = 0;
		int slot_index = 0;
		uint64_t fence_value = 0;
		uint64_t compressed_bytes = 0;
		uint64_t uncompressed_bytes = 0;
		int request_count = 0;
		uint64_t start_time = 0;
	};
	std::deque<BatchMetadata> completion_queue;
	HANDLE completion_stop_event = nullptr;
#endif
	std::thread completion_thread;
	std::atomic<bool> batch_in_flight{false};
	std::atomic<bool> sync_batch_in_flight{false};
	std::atomic<bool> completion_thread_stop{false};
	std::atomic<int> active_batch_slot{0};
	mutable std::mutex batch_mutex;
	std::condition_variable batch_cv;

#ifdef _WIN32
	void completion_thread_func();
	void poll_batch_status(QueueState &p_queue, Vector<Ref<DSRequest>> &p_requests, uint64_t p_fence_value);
	void complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &p_batch);
	void update_profiler_queue_depth_locked();
	static void close_batch_files(Vector<IDStorageFile *> &p_files);
	static int priority_to_queue_index(DSRequest::DSPriority p_priority);
#endif

public:
	Win32Backend() = default;
	~Win32Backend() override { shutdown(); }

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	bool initialize(const String &p_path);
	void shutdown() override;

	bool enqueue_request(Ref<DSRequest> p_request) override;
	void submit_batch() override;
	void submit_async() override;
	bool is_batch_complete() const override;
	void collect_completed_results() override;
	void poll_batch_status();
	bool has_pending_results();
	bool collect_completed_batch(uint64_t &r_fence_value, int &r_completed_count, int &r_error_code, String &r_error_message);
	void cancel_requests_with_tag(uint64_t p_tag) override;
	uint64_t get_completion_fence() const;
	bool wait_for_completion(uint64_t p_fence_value, uint32_t p_timeout_msec);

	static bool is_path_valid(const String &p_path);
	static Dictionary get_drive_info_for_path(const String &p_path);
};

} // namespace godot

#endif // DS_WIN32_BACKEND_H
