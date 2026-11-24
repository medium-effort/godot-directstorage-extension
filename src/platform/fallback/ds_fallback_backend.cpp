#include "ds_fallback_backend.h"
#include "core/ds_cpu_budget.h"
#include "core/ds_log.h"
#include "core/ds_profiler.h"
#include "core/ds_settings.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include "GDeflate.h"
#include "TileStream.h"
#include "../../compression/gdeflate_decompressor.h"
#include "../../compression/gdeflate_parallel.h"
#include "../../formats/dstex_format.h"

namespace godot {

// Declared next to DSProfiler's implementation, not in its public header
// (the Win32 backend carries the same forward declaration).
void ds_profiler_set_batch_metrics(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes,
		int p_request_count, int64_t p_decompression_duration_us, int p_error_state);
void ds_profiler_set_batch_metrics_timed(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes,
		int p_request_count, int64_t p_decompression_duration_us, int p_error_state,
		int64_t p_start_time_usec, int64_t p_end_time_usec);

namespace {
uint64_t fallback_now_us() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

struct FallbackBackend::WorkerScratch {
	static constexpr size_t MAX_OPEN_FILES = 16;
	std::unordered_map<std::string, Ref<FileAccess>> open_files;
	// Batch whose handles are cached here; 0 means none yet. Async batch ids
	// start at 1 (submit_async, next_async_batch_id), so 0 is a safe sentinel.
	uint64_t batch_id = 0;
};

FallbackBackend::FallbackBackend() {
	num_workers = static_cast<uint32_t>(ds_get_max_cpu_threads());
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

bool FallbackBackend::finish_batch_request_locked(uint64_t p_batch_id, const Ref<DSRequest> &p_request, BatchMetrics &r_completed) {
	if (p_batch_id == 0) {
		return false;
	}
	for (auto it = in_flight_batch_infos.begin(); it != in_flight_batch_infos.end(); ++it) {
		if (it->batch_id == p_batch_id) {
			if (p_request.is_valid() && p_request->get_status() == DSRequest::STATUS_FAILED) {
				it->had_failure = true;
			}
			if (it->remaining_requests > 1) {
				it->remaining_requests--;
			} else {
				it->remaining_requests = 0;
				r_completed.compressed_bytes = it->compressed_bytes;
				r_completed.uncompressed_bytes = it->uncompressed_bytes;
				r_completed.start_time_us = it->start_time_us;
				r_completed.request_count = it->request_count;
				r_completed.error_state = it->had_failure ? 1 : 0;
				completed_batch_infos.push_back(*it);
				if (completed_batch_infos.size() > MAX_COMPLETED_BATCHES) {
					completed_batch_infos.pop_front();
				}
				in_flight_batch_infos.erase(it);
				return true;
			}
			break;
		}
	}
	return false;
}

void FallbackBackend::report_batch_metrics(const BatchMetrics &p_metrics) {
	const uint64_t end_us = fallback_now_us();
	const uint64_t duration_us = (end_us > p_metrics.start_time_us) ? (end_us - p_metrics.start_time_us) : 1;
	DSProfiler *profiler = DSProfiler::get_singleton();
	if (!profiler) {
		return;
	}
	// The metrics bridge stores a thread_local pending slot that record_workload()
	// consumes, so this pair must stay on the thread that observed completion.
	// Neither call touches a Godot allocator and record_workload() takes only
	// profiler_mutex (never batch_mutex), so the Q46 lock order is preserved.
	ds_profiler_set_batch_metrics_timed(
			p_metrics.compressed_bytes,
			p_metrics.uncompressed_bytes,
			p_metrics.request_count,
			-1,
			p_metrics.error_state,
			static_cast<int64_t>(p_metrics.start_time_us),
			static_cast<int64_t>(end_us));
	profiler->record_workload(p_metrics.compressed_bytes, duration_us);
}

void FallbackBackend::clear_worker_file_cache(WorkerScratch &r_scratch) {
	if (r_scratch.open_files.empty()) {
		return;
	}
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	const size_t dropped = r_scratch.open_files.size();
	worker_file_cache_entries.fetch_sub(dropped, std::memory_order_relaxed);
#endif
	r_scratch.open_files.clear();
}

void FallbackBackend::worker_thread_func() {
	WorkerScratch scratch;
	while (true) {
		QueuedRequest item;
		bool stopping = false;
		{
			std::unique_lock<std::mutex> lock(batch_mutex);
			// Nothing is queued and this worker is about to block. Release its
			// cached input handles: one held while idle keeps the archive locked
			// against replacement on Windows and keeps serving the unlinked
			// inode on POSIX. Closed outside batch_mutex - closing is a syscall.
			if (request_queue.empty() && !scratch.open_files.empty()) {
				lock.unlock();
				clear_worker_file_cache(scratch);
				lock.lock();
			}
			batch_cv.wait(lock, [this]() {
				return completion_thread_stop.load() || !request_queue.empty();
			});
			if (request_queue.empty() && completion_thread_stop.load()) {
				stopping = true;
			} else {
				item = request_queue.front();
				request_queue.pop_front();
				active_workers++;
				batch_in_flight.store(true);
			}
		}
		if (stopping) {
			clear_worker_file_cache(scratch);
			return;
		}

		// A cached handle belongs to the batch that opened it. Between two batches
		// the archive may have been replaced or rewritten in place, so reusing it
		// would serve stale bytes - the same reason Win32Backend resolves a source
		// file per batch and drops it on completion. This also closes the window
		// where a submit lands before a worker reaches its idle clear.
		if (scratch.batch_id != item.batch_id) {
			clear_worker_file_cache(scratch);
			scratch.batch_id = item.batch_id;
		}

		// CPU file I/O + GDeflate decompression stay on this worker thread,
		// never on the main thread. No Godot scene/render/WorkerThreadPool API
		// calls here (Q22/Q48); completion is observed via status polling.
		if (!should_drop_request(item.request, true)) {
			process_request(item.request, &scratch);
		}

		BatchMetrics completed_metrics;
		bool batch_completed = false;
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			batch_completed = finish_batch_request_locked(item.batch_id, item.request, completed_metrics);
			if (active_workers > 0) {
				active_workers--;
			}
			const bool still_busy = !request_queue.empty() || active_workers > 0;
			batch_in_flight.store(still_busy);
			if (!still_busy) {
				batch_cv.notify_all();
			}
		}
		// Outside batch_mutex: DSProfiler takes its own mutex and may call into
		// the engine, so it must never be reached while the batch lock is held.
		if (batch_completed) {
			report_batch_metrics(completed_metrics);
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
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		active_workers = 0;
		pending_requests.clear();
		request_queue.clear();
		in_flight_batch_infos.clear();
		completed_batch_infos.clear();
		cancelled_tags.clear();
		next_async_batch_id = 1;
		async_batches_submitted = 0;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		worker_file_cache_entries.store(0, std::memory_order_relaxed);
		worker_file_cache_inserts.store(0, std::memory_order_relaxed);
#endif
	}
	worker_threads.clear();
	worker_threads.reserve(num_workers);
	for (uint32_t i = 0; i < num_workers; ++i) {
		worker_threads.emplace_back(&FallbackBackend::worker_thread_func, this);
	}
	UtilityFunctions::print(String(dsgd_log::EXTENSION_PREFIX) + "FallbackBackend initialized (CPU Workers: " + String::num_int64(num_workers) + ").");
	return true;
}

void FallbackBackend::shutdown() {
	{
		// Store the stop flag under batch_mutex. Workers evaluate their wait
		// predicate while holding that mutex, so this store is either observed
		// by a worker that is already blocked (the notify below wakes it) or by
		// a worker that has not yet entered wait() (its predicate then sees the
		// flag immediately).
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_thread_stop.store(true);
	}
	batch_cv.notify_all();
	for (std::thread &t : worker_threads) {
		if (t.joinable()) {
			t.join();
		}
	}
	worker_threads.clear();
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		active_workers = 0;
		request_queue.clear();
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

	if (!p_request->claim_submission()) {
		const String claim_error = "FallbackBackend: request was already submitted or is no longer mutable.";
		if (p_request->get_error_message().is_empty()) {
			p_request->set_error_message(claim_error);
		}
		UtilityFunctions::push_error(claim_error);
		if (r_outcome != nullptr) {
			*r_outcome = EnqueueOutcome::TERMINAL;
		}
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		pending_requests.push_back(p_request);
	}
	return true;
}

bool FallbackBackend::process_request(Ref<DSRequest> p_request, WorkerScratch *p_scratch) {
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
	Ref<FileAccess> file;
	std::string path_key;
	if (p_scratch != nullptr) {
		const CharString utf8 = path.utf8();
		path_key.assign(utf8.get_data() ? utf8.get_data() : "", static_cast<size_t>(utf8.length()));
		auto it = p_scratch->open_files.find(path_key);
		if (it != p_scratch->open_files.end()) {
			file = it->second;
		}
	}
	if (file.is_null()) {
		file = FileAccess::open(path, FileAccess::READ);
		if (file.is_null()) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: Failed to open file '" + path + "'.");
			return false;
		}
		if (p_scratch != nullptr) {
			if (p_scratch->open_files.size() >= WorkerScratch::MAX_OPEN_FILES) {
				clear_worker_file_cache(*p_scratch);
			}
			if (p_scratch->open_files.emplace(std::move(path_key), file).second) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
				worker_file_cache_entries.fetch_add(1, std::memory_order_relaxed);
				worker_file_cache_inserts.fetch_add(1, std::memory_order_relaxed);
#endif
			}
		}
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

	// Always seek when reusing per-worker FileAccess handles (offset 0 included).
	file->seek(offset);

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
		GDeflate::ParallelForFn parallel_fn = nullptr;
		if (p_scratch == nullptr && raw_bytes.size() >= static_cast<int64_t>(sizeof(GDeflate::TileStream))) {
			const GDeflate::TileStream *header = reinterpret_cast<const GDeflate::TileStream *>(raw_bytes.ptr());
			if (header && header->numTiles > 2) {
				effective_workers = std::min(num_workers, static_cast<uint32_t>(header->numTiles / 2u));
				parallel_fn = &ds_gdeflate_parallel_for;
			}
		}

		bool ok = GDeflate::Decompress(
				dest.ptrw(),
				static_cast<size_t>(uncompressed_size),
				raw_bytes.ptr(),
				raw_bytes.size(),
				effective_workers,
				parallel_fn);

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
		UtilityFunctions::push_error(String(dsgd_log::EXTENSION_PREFIX) + "Timed out waiting for the previous batch to complete.");
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
		info.remaining_requests = static_cast<size_t>(batch.size());
		info.start_time_us = fallback_now_us();
		// Same byte accounting Win32Backend::submit_async() uses for BatchMetadata:
		// compressed bytes actually read, logical bytes produced after GDeflate.
		for (int i = 0; i < batch.size(); i++) {
			if (batch[i].is_valid()) {
				info.per_tag_counts[batch[i]->get_cancellation_tag()]++;
				const uint32_t compressed = batch[i]->get_size();
				const uint32_t uncompressed = batch[i]->get_uncompressed_size();
				info.compressed_bytes += compressed;
				info.uncompressed_bytes += (uncompressed > 0 ? uncompressed : compressed);
			}
			request_queue.push_back(QueuedRequest{batch[i], info.batch_id});
		}
		// Physical queued requests. Win32 counts logical requests (which collapse
		// chunk children); FallbackBackend has no chunk children here, so the two
		// counts coincide for the paths the benchmark exercises.
		info.request_count = static_cast<int>(batch.size());
		in_flight_batch_infos.push_back(info);
		async_batches_submitted++;
		batch_in_flight.store(true);
	}
	batch_cv.notify_all();
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
	completed_batch_infos.clear();
}

void FallbackBackend::cancel_requests_with_tag(uint64_t p_tag) {
	if (p_tag == 0) {
		return;
	}
	std::unique_lock<std::mutex> lock(batch_mutex);
	// Remember the tag so workers drop a request that was already popped
	// from the queue (mid-processing race) before buffer writes.
	// Bounded: tags are never reused, eviction only weakens the race guard
	// (queued requests are still covered by per-request status checks).
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
	// Same treatment for async requests submitted but not yet popped:
	// mark terminal FAILED (progress-ready) and erase so workers never
	// touch their buffers. Decrement remaining_requests so in_flight_batch_infos
	// stays accurate.
	std::vector<BatchMetrics> completed_metrics;
	for (auto it = request_queue.begin(); it != request_queue.end();) {
		Ref<DSRequest> req = it->request;
		if (req.is_valid() && req->get_cancellation_tag() == p_tag &&
				req->get_status() == DSRequest::STATUS_SUBMITTED) {
			req->set_status(DSRequest::STATUS_FAILED);
			req->set_error_message("Request cancelled by tag.");
			BatchMetrics metrics;
			if (finish_batch_request_locked(it->batch_id, req, metrics)) {
				completed_metrics.push_back(metrics);
			}
			it = request_queue.erase(it);
		} else {
			++it;
		}
	}
	batch_in_flight.store(!request_queue.empty() || active_workers > 0);
	batch_cv.notify_all();
	lock.unlock();
	// Report only after batch_mutex is released; a cancelled batch is a failed
	// workload, which the profiler records with error_state = 1.
	for (const BatchMetrics &metrics : completed_metrics) {
		report_batch_metrics(metrics);
	}
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
size_t FallbackBackend::_get_cached_file_handle_count_for_testing() const {
	return worker_file_cache_entries.load(std::memory_order_relaxed);
}

uint64_t FallbackBackend::_get_cached_file_handle_inserts_for_testing() const {
	return worker_file_cache_inserts.load(std::memory_order_relaxed);
}
#endif

} // namespace godot
