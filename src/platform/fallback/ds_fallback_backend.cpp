#include "ds_fallback_backend.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "GDeflate.h"

namespace godot {

FallbackBackend::FallbackBackend() {
	OS *os = OS::get_singleton();
	if (os) {
		int cpus = os->get_processor_count();
		num_workers = static_cast<uint32_t>(Math::clamp(cpus, 1, 8));
	} else {
		num_workers = 4;
	}
}

FallbackBackend::~FallbackBackend() {
	shutdown();
}

bool FallbackBackend::is_tag_cancelled_locked(uint64_t p_tag) const {
	return p_tag != 0 && cancelled_tags.find(p_tag) != cancelled_tags.end();
}

bool FallbackBackend::should_drop_request(const Ref<DSRequest> &p_request, bool p_mark) {
	if (p_request.is_null()) {
		return true;
	}
	// Terminal already (e.g. cancelled while still queued): drop silently,
	// keep the cancellation message intact.
	if (p_request->get_status() != DSRequest::STATUS_SUBMITTED) {
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		if (!is_tag_cancelled_locked(p_request->get_cancellation_tag())) {
			return false;
		}
	}
	// Tag cancelled after this batch was popped: fail terminal without
	// touching destination buffers. No writes to freed resources.
	if (p_mark) {
		if (p_request->get_status() == DSRequest::STATUS_SUBMITTED) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("Request cancelled by tag.");
		}
	}
	return true;
}

void FallbackBackend::completion_thread_func() {
	while (true) {
		Vector<Ref<DSRequest>> batch;
		AsyncBatchInfo batch_info;
		{
			std::unique_lock<std::mutex> lock(batch_mutex);
			batch_cv.wait(lock, [this]() {
				return completion_thread_stop.load() || !completion_queue.empty();
			});
			if (completion_queue.empty() && completion_thread_stop.load()) {
				batch_in_flight.store(false);
				return;
			}
			batch = completion_queue.front();
			completion_queue.pop_front();
			if (!in_flight_batch_infos.empty()) {
				batch_info = in_flight_batch_infos.front();
				in_flight_batch_infos.pop_front();
			}
		}
		worker_processing.store(true);

		// CPU file I/O + GDeflate decompression stay on this worker thread,
		// never on the main thread. No Godot scene/render API calls here;
		// completion is observed via status polling (is_batch_complete).
		for (int i = 0; i < batch.size(); i++) {
			if (should_drop_request(batch[i], true)) {
				continue;
			}
			process_request(batch[i]);
		}

		worker_processing.store(false);
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			completed_batches.push_back(batch);
			if (completed_batches.size() > MAX_COMPLETED_BATCHES) {
				completed_batches.pop_front();
			}
			if (batch_info.batch_id != 0) {
				completed_batch_infos.push_back(batch_info);
				if (completed_batch_infos.size() > MAX_COMPLETED_BATCHES) {
					completed_batch_infos.pop_front();
				}
			}
			batch_in_flight.store(!completion_queue.empty());
			batch_cv.notify_all();
		}
	}
}

bool FallbackBackend::is_available() const {
	return true;
}

String FallbackBackend::get_name() const {
	return "SoftwareFallback";
}

bool FallbackBackend::initialize() {
	completion_thread_stop.store(false);
	batch_in_flight.store(false);
	worker_processing.store(false);
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		pending_requests.clear();
		completion_queue.clear();
		completed_batches.clear();
		in_flight_batch_infos.clear();
		completed_batch_infos.clear();
		cancelled_tags.clear();
		next_async_batch_id = 1;
		async_batches_submitted = 0;
	}
	completion_thread = std::thread(&FallbackBackend::completion_thread_func, this);
	UtilityFunctions::print("[DirectStorage] FallbackBackend initialized (CPU Workers: " + String::num_int64(num_workers) + ").");
	return true;
}

void FallbackBackend::shutdown() {
	completion_thread_stop.store(true);
	batch_cv.notify_all();
	if (completion_thread.joinable()) {
		completion_thread.join();
	}
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_queue.clear();
		completed_batches.clear();
		in_flight_batch_infos.clear();
		completed_batch_infos.clear();
		cancelled_tags.clear();
		pending_requests.clear();
	}
	batch_in_flight.store(false);
}

bool FallbackBackend::enqueue_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}

	p_request->set_status(DSRequest::STATUS_SUBMITTED);
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		pending_requests.push_back(p_request);
	}
	return true;
}

bool FallbackBackend::process_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}
	// Drop cancelled requests before any I/O or buffer writes. Terminal
	// FAILED (incl. "cancelled by tag") is how DSContext per-tag progress
	// converges, so every path below must leave a terminal status.
	if (should_drop_request(p_request, true)) {
		return false;
	}

	String path = p_request->get_file_path();
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("FallbackBackend: Failed to open file '" + path + "'.");
		return false;
	}

	uint64_t offset = p_request->get_offset();
	uint32_t read_size = p_request->get_size();
	uint32_t uncompressed_size = p_request->get_uncompressed_size();
	if (p_request->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = read_size;
	}

	if (read_size == 0 || uncompressed_size == 0) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("FallbackBackend: Invalid size parameters (read_size=" + String::num_uint64(read_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").");
		return false;
	}

	if (offset > 0) {
		file->seek(offset);
	}

	PackedByteArray raw_bytes = file->get_buffer(read_size);
	if (raw_bytes.size() < read_size) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("FallbackBackend: Failed to read complete buffer from file.");
		return false;
	}

	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE) {
		// Re-check cancellation after file I/O: the tag may have been
		// cancelled while this request was being read. Never write to
		// destination buffers of a dropped request.
		if (should_drop_request(p_request, true)) {
			return false;
		}
		PackedByteArray &dest = p_request->get_destination_buffer_ref();
		if (dest.size() != static_cast<int64_t>(uncompressed_size)) {
			dest.resize(static_cast<int64_t>(uncompressed_size));
		}
		if (dest.size() == 0) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: Destination buffer unallocated for GDeflate decompression.");
			return false;
		}

		bool ok = GDeflate::Decompress(
				dest.ptrw(),
				static_cast<size_t>(uncompressed_size),
				raw_bytes.ptr(),
				raw_bytes.size(),
				num_workers);

		if (!ok) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: CPU GDeflate decompression failed.");
			return false;
		}
	} else {
		// Same pre-write guard for the uncompressed path.
		if (should_drop_request(p_request, true)) {
			return false;
		}
		p_request->set_destination_buffer(raw_bytes);
	}

	p_request->set_status(DSRequest::STATUS_COMPLETED);
	return true;
}

void FallbackBackend::submit_batch() {
	Vector<Ref<DSRequest>> batch;
	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		batch_cv.wait(lock, [this]() { return !batch_in_flight.load(); });
		batch = pending_requests;
		pending_requests.clear();
	}
	for (int i = 0; i < batch.size(); i++) {
		process_request(batch[i]);
	}
}

void FallbackBackend::submit_async() {
	Vector<Ref<DSRequest>> batch;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		if (pending_requests.size() == 0) {
			return;
		}
		batch = pending_requests;
		pending_requests.clear();
		AsyncBatchInfo info;
		info.batch_id = next_async_batch_id++;
		info.total_requests = static_cast<size_t>(batch.size());
		for (int i = 0; i < batch.size(); i++) {
			if (batch[i].is_valid()) {
				info.per_tag_counts[batch[i]->get_cancellation_tag()]++;
			}
		}
		completion_queue.push_back(batch);
		in_flight_batch_infos.push_back(info);
		async_batches_submitted++;
		batch_in_flight.store(true);
	}
	batch_cv.notify_one();
}

bool FallbackBackend::is_batch_complete() const {
	return !batch_in_flight.load();
}

uint64_t FallbackBackend::get_async_batches_submitted() const {
	std::lock_guard<std::mutex> lock(batch_mutex);
	return async_batches_submitted;
}

size_t FallbackBackend::get_in_flight_async_batch_count() const {
	std::lock_guard<std::mutex> lock(batch_mutex);
	return in_flight_batch_infos.size();
}

void FallbackBackend::collect_completed_results() {
	std::lock_guard<std::mutex> lock(batch_mutex);
	completed_batches.clear();
	completed_batch_infos.clear();
}

void FallbackBackend::cancel_requests_with_tag(uint64_t p_tag) {
	if (p_tag == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	// Remember the tag so the worker drops a request that was already
	// popped from the queue (mid-processing race) before buffer writes.
	// Bounded: tags are never reused, eviction only weakens the race guard
	// (queued batches are still covered by per-request status checks).
	if (cancelled_tags.find(p_tag) == cancelled_tags.end()) {
		if (cancelled_tags.size() >= MAX_CANCELLED_TAGS) {
			cancelled_tags.clear();
		}
		cancelled_tags.insert(p_tag);
	}
	Vector<Ref<DSRequest>> remaining;
	for (int i = 0; i < pending_requests.size(); i++) {
		if (pending_requests[i].is_valid() && pending_requests[i]->get_cancellation_tag() == p_tag) {
			if (pending_requests[i]->get_status() == DSRequest::STATUS_SUBMITTED) {
				pending_requests[i]->set_status(DSRequest::STATUS_FAILED);
				pending_requests[i]->set_error_message("Request cancelled by tag.");
			}
		} else {
			remaining.push_back(pending_requests[i]);
		}
	}
	pending_requests = remaining;
	// Same treatment for async batches submitted but not yet processed:
	// mark terminal FAILED (progress-ready) and erase so the worker never
	// touches their buffers. Batches drained empty are removed with their
	// accounting info (parallel FIFO with completion_queue).
	for (size_t q = 0; q < completion_queue.size();) {
		Vector<Ref<DSRequest>> kept;
		for (int i = 0; i < completion_queue[q].size(); i++) {
			Ref<DSRequest> req = completion_queue[q][i];
			if (req.is_valid() && req->get_cancellation_tag() == p_tag &&
					req->get_status() == DSRequest::STATUS_SUBMITTED) {
				req->set_status(DSRequest::STATUS_FAILED);
				req->set_error_message("Request cancelled by tag.");
			} else {
				kept.push_back(req);
			}
		}
		if (kept.size() == 0) {
			completion_queue.erase(completion_queue.begin() + q);
			if (q < in_flight_batch_infos.size()) {
				in_flight_batch_infos.erase(in_flight_batch_infos.begin() + q);
			}
		} else {
			completion_queue[q] = kept;
			q++;
		}
	}
	batch_in_flight.store(!completion_queue.empty() || worker_processing.load());
	batch_cv.notify_all();
}

} // namespace godot
