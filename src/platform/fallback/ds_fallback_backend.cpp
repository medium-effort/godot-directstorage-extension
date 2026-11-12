#include "ds_fallback_backend.h"
#include "core/ds_settings.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include "GDeflate.h"
#include "TileStream.h"
#include "../../compression/gdeflate_decompressor.h"
#include "../../compression/gdeflate_parallel.h"
#include "../../formats/dstex_format.h"

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
	{
		// Store the stop flag under batch_mutex. The worker evaluates its wait
		// predicate while holding that mutex, so this store is either observed
		// by a worker that is already blocked (the notify below wakes it) or by
		// a worker that has not yet entered wait() (its predicate then sees the
		// flag immediately). Notifying without the lock leaves a window where a
		// worker that already checked the predicate but has not yet registered
		// as a waiter misses the notify and blocks in join() forever.
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_thread_stop.store(true);
	}
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

bool FallbackBackend::enqueue_request(Ref<DSRequest> p_request, PlatformBackend::EnqueueOutcome *r_outcome) {
	if (p_request.is_null()) {
		if (r_outcome != nullptr) {
			*r_outcome = EnqueueOutcome::TERMINAL;
		}
		return false;
	}
	if (r_outcome != nullptr) {
		*r_outcome = EnqueueOutcome::STAGED;
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
		if (static_cast<uint64_t>(uncompressed_size) > DSTEX_MAX_TOTAL_BYTES) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: GDeflate output size " + String::num_uint64(uncompressed_size) + " exceeds the 512 MiB limit.");
			return false;
		}
		// Same predicate GDeflateDecompressor::decompress() applies: the stream
		// header, its declared output, and the per-tile expansion bound must all
		// agree with the request before anything sizes a destination buffer.
		if (!GDeflateDecompressor::validate_stream(raw_bytes, static_cast<int>(uncompressed_size))) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: Invalid GDeflate stream (header, tile table, or expansion bound).");
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

		uint32_t effective_workers = 1;
		if (raw_bytes.size() >= static_cast<int64_t>(sizeof(GDeflate::TileStream))) {
			const GDeflate::TileStream *header = reinterpret_cast<const GDeflate::TileStream *>(raw_bytes.ptr());
			if (header && header->numTiles > 2) {
				effective_workers = std::min(num_workers, static_cast<uint32_t>(header->numTiles / 2u));
			}
		}

		bool ok = GDeflate::Decompress(
				dest.ptrw(),
				static_cast<size_t>(uncompressed_size),
				raw_bytes.ptr(),
				raw_bytes.size(),
				effective_workers,
				&ds_gdeflate_parallel_for);

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
		// Deliver through the unguarded reference. enqueue_request already moved
		// this request to STATUS_SUBMITTED, so set_destination_buffer() is
		// rejected by the payload-immutability guard and the payload was
		// silently dropped: the request reported COMPLETED with an empty
		// destination buffer. Same accessor the GDeflate branch above uses.
		p_request->get_destination_buffer_ref() = raw_bytes;
	}

	p_request->set_status(DSRequest::STATUS_COMPLETED);
	return true;
}

void FallbackBackend::submit_batch() {
	uint32_t sync_timeout_ms = 3000;
	if (dsgd_settings::has_setting("sync_flush_timeout_ms", "sync_flush_timeout_ms")) {
		sync_timeout_ms = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(dsgd_settings::get_setting("sync_flush_timeout_ms", "sync_flush_timeout_ms")), 500, 10000));
	}
	uint32_t half_timeout_ms = sync_timeout_ms / 2;

	Vector<Ref<DSRequest>> batch;
	bool timed_out = false;
	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		if (!batch_cv.wait_for(lock, std::chrono::milliseconds(half_timeout_ms), [this]() {
				return !batch_in_flight.load();
			})) {
			timed_out = true;
		}
		batch = pending_requests;
		pending_requests.clear();
	}
	if (timed_out) {
		UtilityFunctions::push_error("[DirectStorage] Timed out waiting for the previous batch to complete.");
		for (int i = 0; i < batch.size(); i++) {
			if (batch[i].is_valid() && batch[i]->get_status() == DSRequest::STATUS_SUBMITTED) {
				batch[i]->set_status(DSRequest::STATUS_FAILED);
				batch[i]->set_error_message("Timed out waiting for the previous batch to complete.");
			}
		}
		return;
	}
	for (int i = 0; i < batch.size(); i++) {
		process_request(batch[i]);
	}
}

bool FallbackBackend::submit_async() {
	Vector<Ref<DSRequest>> batch;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		if (pending_requests.size() == 0) {
			return true;
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
	return true;
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
