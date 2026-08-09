#include "ds_context.h"
#include "ds_error_handler.h"
#include "ds_profiler.h"
#include "../platform/fallback/ds_fallback_backend.h"
#ifdef _WIN32
#include "../platform/win32/ds_win32_backend.h"
#endif

#include <godot_cpp/classes/os.hpp>
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
	if (active_backend) {
		return active_backend->get_name() == "Win32DirectStorage";
	}
#ifdef _WIN32
	Win32Backend backend;
	return backend.is_available();
#else
	return false;
#endif
}

bool DSContext::is_d3d12_backend() const {
	if (!is_supported()) {
		return false;
	}
#ifdef _WIN32
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
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

bool DSContext::is_current_drive_supported() const {
	OS *os = OS::get_singleton();
	String path = "res://";
	if (os) {
		String exec_path = os->get_executable_path();
		if (!exec_path.is_empty()) {
			path = exec_path;
		}
	}
	return is_path_supported(path);
}

bool DSContext::is_path_supported(const String &p_path) const {
	Dictionary info = get_drive_info(p_path);
	return info.get("directstorage_supported", false);
}

Dictionary DSContext::get_drive_info(const String &p_path) const {
#ifdef _WIN32
	return Win32Backend::get_drive_info_for_path(p_path);
#else
	Dictionary res;
	res["drive"] = "";
	res["bus_type"] = "Non-Windows";
	res["file_system"] = "Unknown";
	res["directstorage_supported"] = false;
	res["reason"] = "DirectStorage hardware acceleration is only supported on Windows.";
	return res;
#endif
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
	active_backend = memnew(Win32Backend);
	if (active_backend->initialize()) {
		initialized = true;
		UtilityFunctions::print("[DirectStorage] DirectStorage Context initialized with Win32Backend.");
		return true;
	} else {
		memdelete(active_backend);
		active_backend = nullptr;
		UtilityFunctions::print("[DirectStorage] Win32DirectStorage initialization failed or unavailable. Falling back to FallbackBackend.");
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

	initialized = false;
	UtilityFunctions::print("[DirectStorage] DirectStorage Context shut down.");
}

bool DSContext::enqueue_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}

	if (!initialized) {
		initialize_ds();
	}

	if (!initialized || !active_backend) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("DirectStorage Context not initialized.");
		return false;
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
	ClassDB::bind_method(D_METHOD("is_current_drive_supported"), &DSContext::is_current_drive_supported);
	ClassDB::bind_method(D_METHOD("is_path_supported", "path"), &DSContext::is_path_supported);
	ClassDB::bind_method(D_METHOD("get_drive_info", "path"), &DSContext::get_drive_info, DEFVAL("res://"));
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
