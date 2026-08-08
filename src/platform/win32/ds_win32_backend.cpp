#include "ds_win32_backend.h"
#include "../../core/ds_context.h"
#include "../../core/ds_error_handler.h"
#include "../../core/ds_profiler.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <string>

namespace godot {

bool Win32Backend::is_available() const {
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

String Win32Backend::get_name() const {
	return "Win32DirectStorage";
}

bool Win32Backend::initialize() {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (!ctx) {
		return false;
	}

	uint32_t staging_mb = ctx->get_staging_buffer_size_mb();
	bool disable_gpu = ctx->get_disable_gpu_decompression();
	uint16_t capacity = ctx->get_queue_capacity();

	DSTORAGE_CONFIGURATION config = {};
	config.DisableTelemetry = TRUE;
	config.DisableGpuDecompression = disable_gpu;
	config.DisableGpuDecompressionMetacommand = disable_gpu;

	HRESULT hr = DStorageSetConfiguration(&config);
	if (SUCCEEDED(hr)) {
		hr = DStorageGetFactory(IID_PPV_ARGS(&factory));
	}

	if (SUCCEEDED(hr) && factory) {
		factory->SetDebugFlags(DSTORAGE_DEBUG_NONE);
		factory->SetStagingBufferSize(staging_mb * 1024 * 1024);

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = capacity;
		queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
		queueDesc.Name = "DirectStorageMemoryQueue";
		queueDesc.Device = nullptr;

		hr = factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&memory_queue));
	}

	if (SUCCEEDED(hr) && memory_queue) {
		batch_event = CreateEvent(NULL, FALSE, FALSE, NULL);

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->register_queue(memory_queue);
		}
		UtilityFunctions::print("[DirectStorage] Win32Backend DirectStorage Queue created successfully.");
		return true;
	} else {
		UtilityFunctions::print("[DirectStorage] Win32Backend DirectStorage Queue creation failed.");
		return false;
	}
#else
	return false;
#endif
}

void Win32Backend::shutdown() {
#ifdef _WIN32
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->unregister_queue();
	}

	if (memory_queue) {
		memory_queue->Close();
		memory_queue->Release();
		memory_queue = nullptr;
	}

	if (factory) {
		factory->Release();
		factory = nullptr;
	}

	if (batch_event) {
		CloseHandle(batch_event);
		batch_event = nullptr;
	}

	in_flight_requests.clear();
#endif
}

bool Win32Backend::enqueue_request(Ref<DSRequest> p_request) {
#ifdef _WIN32
	if (p_request.is_null() || !factory || !memory_queue) {
		if (p_request.is_valid()) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("Win32Backend not initialized.");
		}
		return false;
	}

	String path = p_request->get_file_path();
	std::wstring wpath = path.utf8().get_data() ? std::wstring(path.wide_string().get_data()) : L"";

	IDStorageFile *file = nullptr;
	HRESULT hr = factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr) || !file) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend: Failed to open file '" + path + "'.");
		return false;
	}

	DSTORAGE_REQUEST request = {};
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
	request.Options.CompressionFormat = (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)
			? DSTORAGE_COMPRESSION_FORMAT_GDEFLATE
			: DSTORAGE_COMPRESSION_FORMAT_NONE;

	request.Source.File.Source = file;
	request.Source.File.Offset = p_request->get_offset();
	request.Source.File.Size = p_request->get_size();

	PackedByteArray &dest = p_request->get_destination_buffer_ref();
	if (dest.size() < p_request->get_size()) {
		dest.resize(p_request->get_size());
	}

	request.Destination.Memory.Buffer = dest.ptrw();
	request.Destination.Memory.Size = p_request->get_size();
	request.UncompressedSize = p_request->get_size();
	request.CancellationTag = p_request->get_cancellation_tag();

	memory_queue->EnqueueRequest(&request);
	file->Close();
	file->Release();

	p_request->set_status(DSRequest::STATUS_SUBMITTED);
	in_flight_requests.push_back(p_request);
	return true;
#else
	return false;
#endif
}

void Win32Backend::submit_batch() {
#ifdef _WIN32
	if (!memory_queue || in_flight_requests.size() == 0) {
		return;
	}

	uint64_t total_bytes = 0;
	for (int i = 0; i < in_flight_requests.size(); i++) {
		if (in_flight_requests[i].is_valid()) {
			total_bytes += in_flight_requests[i]->get_size();
		}
	}

	uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;

	if (batch_event) {
		memory_queue->EnqueueSetEvent(batch_event);
	}
	memory_queue->Submit();

	if (batch_event) {
		DWORD wait_res = WaitForSingleObject(batch_event, 5000);
		if (wait_res == WAIT_OBJECT_0) {
			uint64_t end_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			uint64_t duration_us = (end_time > start_time) ? (end_time - start_time) : 1;

			for (int i = 0; i < in_flight_requests.size(); i++) {
				if (in_flight_requests[i].is_valid()) {
					in_flight_requests[i]->set_status(DSRequest::STATUS_COMPLETED);
				}
			}

			DSProfiler *profiler = DSProfiler::get_singleton();
			if (profiler) {
				profiler->record_workload(total_bytes, duration_us);
			}
		} else {
			UtilityFunctions::push_error("[DirectStorage] Win32Backend batch submit wait timed out or failed.");
			for (int i = 0; i < in_flight_requests.size(); i++) {
				if (in_flight_requests[i].is_valid()) {
					in_flight_requests[i]->set_status(DSRequest::STATUS_FAILED);
					in_flight_requests[i]->set_error_message("Batch submission timed out.");
				}
			}
		}
	}

	in_flight_requests.clear();
#endif
}

void Win32Backend::cancel_requests_with_tag(uint64_t p_tag) {
#ifdef _WIN32
	if (memory_queue) {
		memory_queue->CancelRequestsWithTag(UINT64_MAX, p_tag);
	}
	Vector<Ref<DSRequest>> remaining;
	for (int i = 0; i < in_flight_requests.size(); i++) {
		if (in_flight_requests[i].is_valid() && (in_flight_requests[i]->get_cancellation_tag() & UINT64_MAX) == p_tag) {
			in_flight_requests[i]->set_status(DSRequest::STATUS_FAILED);
			in_flight_requests[i]->set_error_message("Request cancelled by tag.");
		} else {
			remaining.push_back(in_flight_requests[i]);
		}
	}
	in_flight_requests = remaining;
#endif
}

} // namespace godot
