#include "ds_context.h"
#include "ds_error_handler.h"
#include "ds_profiler.h"
#include "../platform/fallback/ds_fallback_backend.h"
#include "../platform/win32/ds_win32_backend.h"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

DSContext *DSContext::singleton = nullptr;

DSContext *DSContext::get_singleton() {
	return singleton;
}

DSContext::DSContext() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

DSContext::~DSContext() {
	shutdown_ds();
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool DSContext::is_supported() const {
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

bool DSContext::is_d3d12_backend() const {
#ifdef _WIN32
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return false;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	if (!rd) {
		return false;
	}
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	return (handle != 0);
#else
	return false;
#endif
}

bool DSContext::is_initialized() const {
	return initialized;
}

String DSContext::get_active_backend_name() const {
	if (active_backend) {
		return active_backend->get_name();
	}
	return "None";
}

bool DSContext::initialize_ds() {
	if (initialized) {
		return true;
	}

	// Load settings from ProjectSettings or set defaults
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps) {
		if (ps->has_setting("directstorage/staging_buffer_size_mb")) {
			staging_buffer_size_mb = ps->get_setting("directstorage/staging_buffer_size_mb");
		}
		if (ps->has_setting("directstorage/disable_gpu_decompression")) {
			disable_gpu_decompression = ps->get_setting("directstorage/disable_gpu_decompression");
		}
		if (ps->has_setting("directstorage/queue_capacity")) {
			queue_capacity = ps->get_setting("directstorage/queue_capacity");
		}
	}

#ifdef _WIN32
	// Step 1: Set Configuration
	DSTORAGE_CONFIGURATION config = {};
	config.DisableTelemetry = TRUE;
	config.DisableGpuDecompression = disable_gpu_decompression;
	config.DisableGpuDecompressionMetacommand = disable_gpu_decompression;

	HRESULT hr = DStorageSetConfiguration(&config);
	if (SUCCEEDED(hr)) {
		hr = DStorageGetFactory(IID_PPV_ARGS(&factory));
	}

	if (SUCCEEDED(hr) && factory) {
		factory->SetDebugFlags(DSTORAGE_DEBUG_NONE);
		factory->SetStagingBufferSize(staging_buffer_size_mb * 1024 * 1024);

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = queue_capacity;
		queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
		queueDesc.Name = "DirectStorageMemoryQueue";
		queueDesc.Device = nullptr;

		hr = factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&memory_queue));
	}

	if (SUCCEEDED(hr) && memory_queue) {
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->register_queue(memory_queue);
		}

		active_backend = memnew(Win32Backend);
		active_backend->initialize();
		initialized = true;
		UtilityFunctions::print("[DirectStorage] DirectStorage Context initialized with Win32Backend.");
		return true;
	} else {
		UtilityFunctions::print("[DirectStorage] Win32 DirectStorage initialization failed or unavailable. Falling back to FallbackBackend.");
	}
#endif

	// Fallback backend for non-Windows or if Win32 DirectStorage initialization failed
	active_backend = memnew(FallbackBackend);
	active_backend->initialize();
	initialized = true;
	UtilityFunctions::print("[DirectStorage] DirectStorage Context initialized with FallbackBackend.");
	return true;
}

void DSContext::shutdown_ds() {
	if (!initialized) {
		return;
	}

	if (active_backend) {
		active_backend->shutdown();
		memdelete(active_backend);
		active_backend = nullptr;
	}

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
#endif

	initialized = false;
	UtilityFunctions::print("[DirectStorage] DirectStorage Context shut down.");
}

bool DSContext::enqueue_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}

	if (!initialized || !active_backend) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("DirectStorage Context not initialized.");
		return false;
	}

	if (active_backend->get_name() == "Win32DirectStorage") {
#ifdef _WIN32
		String path = p_request->get_file_path();
		std::wstring wpath = path.utf8().get_data() ? std::wstring(path.wide_string().get_data()) : L"";

		IDStorageFile *file = nullptr;
		HRESULT hr = factory ? factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&file)) : E_FAIL;
		if (FAILED(hr) || !file) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("Failed to open file for DirectStorage access.");
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

		PackedByteArray dest = p_request->get_destination_buffer();
		if (dest.size() < p_request->get_size()) {
			dest.resize(p_request->get_size());
			p_request->set_destination_buffer(dest);
		}

		request.Destination.Memory.Buffer = dest.ptrw();
		request.Destination.Memory.Size = p_request->get_size();
		request.UncompressedSize = p_request->get_size();
		request.CancellationTag = p_request->get_cancellation_tag();

		memory_queue->EnqueueRequest(&request);
		file->Close();
		file->Release();

		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
#endif
	}

	return active_backend->enqueue_request(p_request);
}

void DSContext::flush_batch() {
	if (!initialized || !active_backend) {
		return;
	}
	active_backend->submit_batch();
}

void DSContext::cancel_requests_with_tag(uint64_t p_tag) {
	if (!initialized || !active_backend) {
		return;
	}
	active_backend->cancel_requests_with_tag(p_tag);
}

void DSContext::set_staging_buffer_size_mb(uint32_t p_size_mb) {
	staging_buffer_size_mb = p_size_mb;
}

uint32_t DSContext::get_staging_buffer_size_mb() const {
	return staging_buffer_size_mb;
}

void DSContext::set_disable_gpu_decompression(bool p_disable) {
	disable_gpu_decompression = p_disable;
}

bool DSContext::get_disable_gpu_decompression() const {
	return disable_gpu_decompression;
}

void DSContext::set_queue_capacity(uint16_t p_capacity) {
	queue_capacity = p_capacity;
}

uint16_t DSContext::get_queue_capacity() const {
	return queue_capacity;
}

void DSContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_supported"), &DSContext::is_supported);
	ClassDB::bind_method(D_METHOD("is_d3d12_backend"), &DSContext::is_d3d12_backend);
	ClassDB::bind_method(D_METHOD("is_initialized"), &DSContext::is_initialized);
	ClassDB::bind_method(D_METHOD("get_active_backend_name"), &DSContext::get_active_backend_name);
	ClassDB::bind_method(D_METHOD("initialize_ds"), &DSContext::initialize_ds);
	ClassDB::bind_method(D_METHOD("shutdown_ds"), &DSContext::shutdown_ds);

	ClassDB::bind_method(D_METHOD("enqueue_request", "request"), &DSContext::enqueue_request);
	ClassDB::bind_method(D_METHOD("flush_batch"), &DSContext::flush_batch);
	ClassDB::bind_method(D_METHOD("cancel_requests_with_tag", "tag"), &DSContext::cancel_requests_with_tag);

	ClassDB::bind_method(D_METHOD("set_staging_buffer_size_mb", "size_mb"), &DSContext::set_staging_buffer_size_mb);
	ClassDB::bind_method(D_METHOD("get_staging_buffer_size_mb"), &DSContext::get_staging_buffer_size_mb);

	ClassDB::bind_method(D_METHOD("set_disable_gpu_decompression", "disable"), &DSContext::set_disable_gpu_decompression);
	ClassDB::bind_method(D_METHOD("get_disable_gpu_decompression"), &DSContext::get_disable_gpu_decompression);

	ClassDB::bind_method(D_METHOD("set_queue_capacity", "capacity"), &DSContext::set_queue_capacity);
	ClassDB::bind_method(D_METHOD("get_queue_capacity"), &DSContext::get_queue_capacity);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "staging_buffer_size_mb"), "set_staging_buffer_size_mb", "get_staging_buffer_size_mb");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "disable_gpu_decompression"), "set_disable_gpu_decompression", "get_disable_gpu_decompression");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "queue_capacity"), "set_queue_capacity", "get_queue_capacity");
}

} // namespace godot
