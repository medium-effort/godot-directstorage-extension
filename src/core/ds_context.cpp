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

#ifdef _WIN32
uint64_t ds_win32_get_completion_fence(PlatformBackend *p_backend);
bool ds_win32_wait_for_completion(PlatformBackend *p_backend, uint64_t p_fence_value, uint32_t p_timeout_msec);
#endif

static bool synchronize_rendering_device(const String &p_phase) {
#ifdef _WIN32
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		UtilityFunctions::push_error("[DirectStorage] RenderingDevice synchronization unavailable during " + p_phase + ": D3D12 is not active.");
		return false;
	}

	RenderingDevice *rd = rs->get_rendering_device();
	if (!rd) {
		UtilityFunctions::push_error("[DirectStorage] RenderingDevice synchronization unavailable during " + p_phase + ": no RenderingDevice is active.");
		return false;
	}

	uint64_t device_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	uint64_t command_queue_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	if (device_handle == 0 || command_queue_handle == 0) {
		UtilityFunctions::push_error("[DirectStorage] RenderingDevice synchronization unavailable during " + p_phase + ": native D3D12 device or command queue is invalid.");
		return false;
	}

	// DirectStorage requires GPU destinations to be in COMMON/COPY_DEST. Godot
	// 4.7 exposes no per-resource D3D12 state transition API, so submit/sync is
	// the only supported boundary and the resource must not be in an active draw.
	rd->submit();
	rd->sync();
	return true;
#else
	UtilityFunctions::push_error("[DirectStorage] RenderingDevice synchronization unavailable during " + p_phase + ": platform is not Windows.");
	return false;
#endif
}

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

namespace {

#ifdef _WIN32
static DSTORAGE_COMPRESSION_SUPPORT query_gpu_decompression_support(uint16_t p_queue_capacity) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return DSTORAGE_COMPRESSION_SUPPORT_NONE;
	}

	RenderingDevice *rd = rs->get_rendering_device();
	uint64_t device_handle = rd ? rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0) : 0;
	if (!rd || device_handle == 0) {
		return DSTORAGE_COMPRESSION_SUPPORT_NONE;
	}

	HMODULE core_module = GetModuleHandleW(L"dstoragecore.dll");
	HMODULE dstorage_module = GetModuleHandleW(L"dstorage.dll");
	bool release_core_module = false;
	bool release_dstorage_module = false;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if ((!core_module || !dstorage_module) && ps) {
		String bin_dir = ps->globalize_path("res://addons/directstorage_accelerator/bin");
		String core_path = bin_dir + "/dstoragecore.dll";
		String dstorage_path = bin_dir + "/dstorage.dll";
		std::wstring w_core_path(reinterpret_cast<const wchar_t *>(core_path.utf16().get_data()));
		std::wstring w_dstorage_path(reinterpret_cast<const wchar_t *>(dstorage_path.utf16().get_data()));
		if (!core_module) {
			core_module = LoadLibraryW(w_core_path.c_str());
			release_core_module = core_module != nullptr;
		}
		if (!dstorage_module) {
			dstorage_module = LoadLibraryW(w_dstorage_path.c_str());
			release_dstorage_module = dstorage_module != nullptr;
		}
	}

	DSTORAGE_COMPRESSION_SUPPORT support = DSTORAGE_COMPRESSION_SUPPORT_NONE;
	if (dstorage_module) {
		using DStorageGetFactoryFunction = HRESULT(WINAPI *)(REFIID, void **);
		DStorageGetFactoryFunction get_factory = reinterpret_cast<DStorageGetFactoryFunction>(GetProcAddress(dstorage_module, "DStorageGetFactory"));
		IDStorageFactory *factory = nullptr;
		if (get_factory && SUCCEEDED(get_factory(__uuidof(IDStorageFactory), reinterpret_cast<void **>(&factory))) && factory) {
			DSTORAGE_QUEUE_DESC queue_desc = {};
			queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
			queue_desc.Capacity = MAX(static_cast<uint16_t>(DSTORAGE_MIN_QUEUE_CAPACITY), p_queue_capacity);
			queue_desc.Capacity = MIN(static_cast<uint16_t>(DSTORAGE_MAX_QUEUE_CAPACITY), queue_desc.Capacity);
			queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
			queue_desc.Name = "DirectStorageCapabilityReport";
			queue_desc.Device = reinterpret_cast<ID3D12Device *>(device_handle);

			IDStorageQueue1 *queue = nullptr;
			if (SUCCEEDED(factory->CreateQueue(&queue_desc, __uuidof(IDStorageQueue1), reinterpret_cast<void **>(&queue))) && queue) {
				IDStorageQueue2 *queue2 = nullptr;
				if (SUCCEEDED(queue->QueryInterface(__uuidof(IDStorageQueue2), reinterpret_cast<void **>(&queue2))) && queue2) {
					support = queue2->GetCompressionSupport(DSTORAGE_COMPRESSION_FORMAT_GDEFLATE);
					queue2->Close();
					queue2->Release();
				} else {
					queue->Close();
				}
				queue->Release();
			}
			factory->Release();
		}
	}

	if (release_dstorage_module) {
		FreeLibrary(dstorage_module);
	}
	if (release_core_module) {
		FreeLibrary(core_module);
	}
	return support;
}
#endif

} // namespace

bool DSContext::is_gpu_decompression_available() const {
	return get_gpu_decompression_support_tier() > 0;
}

bool DSContext::is_gpu_decompression_enabled() const {
	return is_gpu_decompression_available() && !disable_gpu_decompression;
}

int DSContext::get_gpu_decompression_support_tier() const {
#ifdef _WIN32
	if (!is_d3d12_backend()) {
		return 0;
	}

	DSTORAGE_COMPRESSION_SUPPORT support = query_gpu_decompression_support(queue_capacity);
	if ((support & DSTORAGE_COMPRESSION_SUPPORT_GPU_OPTIMIZED) != 0) {
		return 2;
	}
	if ((support & DSTORAGE_COMPRESSION_SUPPORT_GPU_FALLBACK) != 0) {
		return 1;
	}
#endif
	return 0;
}

Dictionary DSContext::get_capability_report() const {
	Dictionary report;
	int gpu_tier = get_gpu_decompression_support_tier();
	Array supported_destinations;
	supported_destinations.push_back(DSRequest::DESTINATION_MEMORY);
	if (is_d3d12_backend()) {
		supported_destinations.push_back(DSRequest::DESTINATION_GPU_BUFFER);
		supported_destinations.push_back(DSRequest::DESTINATION_GPU_TEXTURE);
	}

#ifdef _WIN32
	String directstorage_version = String::num_int64(DSTORAGE_SDK_VERSION / 100) + "." + String::num_int64(DSTORAGE_SDK_VERSION % 100);
#else
	String directstorage_version = "Unavailable";
#endif
	report["directstorage_version"] = directstorage_version;
	report["gpu_decompression_available"] = gpu_tier > 0;
	report["gpu_decompression_enabled"] = gpu_tier > 0 && !disable_gpu_decompression;
	report["gpu_decompression_tier"] = gpu_tier;
	report["active_backend"] = get_active_backend_name();
	report["supported_destination_types"] = supported_destinations;
	return report;
}

Dictionary DSContext::get_request_execution_report(Ref<DSRequest> p_request) const {
	Dictionary report;
	report["actual_destination_type"] = -1;
	report["actual_backend"] = "None";
	report["was_gpu_decompression_used"] = false;
	if (p_request.is_null()) {
		return report;
	}

	int64_t request_id = static_cast<int64_t>(p_request->get_instance_id());
	if (request_execution_reports.has(request_id)) {
		return request_execution_reports.get(request_id, report);
	}
	return report;
}

DSRequest::DSDestinationType DSContext::get_actual_destination_type(Ref<DSRequest> p_request) const {
	Dictionary report = get_request_execution_report(p_request);
	int actual_destination_type = report.get("actual_destination_type", -1);
	return actual_destination_type >= DSRequest::DESTINATION_MEMORY && actual_destination_type <= DSRequest::DESTINATION_GPU_TEXTURE
			? static_cast<DSRequest::DSDestinationType>(actual_destination_type)
			: DSRequest::DESTINATION_MEMORY;
}

String DSContext::get_actual_backend(Ref<DSRequest> p_request) const {
	return get_request_execution_report(p_request).get("actual_backend", "None");
}

bool DSContext::was_gpu_decompression_used(Ref<DSRequest> p_request) const {
	return get_request_execution_report(p_request).get("was_gpu_decompression_used", false);
}

uint64_t DSContext::get_completion_fence() const {
#ifdef _WIN32
	if (active_backend && active_backend->get_name() == "Win32DirectStorage") {
		uint64_t backend_fence = ds_win32_get_completion_fence(active_backend);
		if (backend_fence != 0) {
			return backend_fence;
		}
	}
#endif
	return completion_fence;
}

bool DSContext::wait_for_completion(int p_timeout_msec) {
	if (p_timeout_msec < 0) {
		UtilityFunctions::push_error("[DirectStorage] Completion wait rejected: timeout must not be negative.");
		return false;
	}

	uint64_t fence_value = get_completion_fence();
	if (fence_value == 0) {
		if (gpu_requests_pending) {
			UtilityFunctions::push_error("[DirectStorage] Completion wait failed: GPU requests have no completion fence.");
			return false;
		}
		return true;
	}

#ifdef _WIN32
	if (active_backend && active_backend->get_name() == "Win32DirectStorage" &&
			!ds_win32_wait_for_completion(active_backend, fence_value, static_cast<uint32_t>(p_timeout_msec))) {
		UtilityFunctions::push_error("[DirectStorage] Completion fence wait timed out or failed.");
		return false;
	}
#endif

	if (active_backend && active_backend->get_name() == "Win32DirectStorage" &&
			!synchronize_rendering_device("DirectStorage completion")) {
		return false;
	}
	return true;
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

Error DSContext::initialize_ds_for_path(const String &p_path) {
	if (p_path.is_empty()) {
		UtilityFunctions::push_warning("[DirectStorage] Empty path supplied; falling back to res:// initialization.");
		return initialize_ds() ? OK : ERR_CANT_CREATE;
	}

#ifdef _WIN32
	if (!Win32Backend::is_path_valid(p_path)) {
		UtilityFunctions::push_warning("[DirectStorage] Path '" + p_path + "' is invalid; falling back to res:// initialization.");
		if (initialized) {
			shutdown_ds();
		}
		return initialize_ds() ? ERR_FILE_NOT_FOUND : ERR_CANT_CREATE;
	}

	Dictionary drive_info = Win32Backend::get_drive_info_for_path(p_path);
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::push_warning("[DirectStorage] Path '" + p_path + "' is on an unsupported volume; falling back to res:// initialization.");
		if (initialized) {
			shutdown_ds();
		}
		return initialize_ds() ? ERR_UNAVAILABLE : ERR_CANT_CREATE;
	}

	if (initialized) {
		shutdown_ds();
	}

	initialization_path = p_path;
	bool path_initialized = initialize_ds();
	initialization_path = "";
	if (path_initialized && active_backend && active_backend->get_name() == "Win32DirectStorage") {
		UtilityFunctions::print("[DirectStorage] DirectStorage Context initialized for path '" + p_path + "'.");
		return OK;
	}

	if (initialized) {
		shutdown_ds();
	}
	UtilityFunctions::push_warning("[DirectStorage] Path-aware DirectStorage initialization failed; falling back to res:// initialization.");
	initialize_ds();
	return ERR_CANT_CREATE;
#else
	UtilityFunctions::push_warning("[DirectStorage] Path-aware initialization is unavailable on this platform; falling back to res:// initialization.");
	return initialize_ds() ? ERR_UNAVAILABLE : ERR_CANT_CREATE;
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
			uint32_t configured_size = ps->get_setting("directstorage/staging_buffer_size_mb");
			staging_buffer_size_mb = configured_size > 0 ? configured_size : DEFAULT_STAGING_BUFFER_SIZE_MB;
		}
		if (ps->has_setting("directstorage/disable_gpu_decompression")) {
			disable_gpu_decompression = ps->get_setting("directstorage/disable_gpu_decompression");
		}
		if (ps->has_setting("directstorage/queue_capacity")) {
			queue_capacity = ps->get_setting("directstorage/queue_capacity");
		}
	}

#ifdef _WIN32
	Win32Backend *backend = memnew(Win32Backend);
	active_backend = backend;
	bool backend_initialized = initialization_path.is_empty() ? backend->initialize() : backend->initialize(initialization_path);
	if (backend_initialized) {
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
	gpu_requests_pending = false;
	completion_fence = 0;
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

	bool gpu_destination = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER ||
			p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	if (gpu_destination && (active_backend->get_name() != "Win32DirectStorage" || !is_d3d12_backend())) {
		String error_message = "GPU destinations require an active Win32 DirectStorage D3D12 backend.";
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message(error_message);
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->report_error(error_message);
		}
		return false;
	}

	if (gpu_destination && !gpu_requests_pending && !synchronize_rendering_device("DirectStorage GPU request")) {
		String error_message = "GPU destination request rejected: RenderingDevice could not be synchronized.";
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message(error_message);
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->report_error(error_message);
		}
		return false;
	}

	bool enqueued = active_backend->enqueue_request(p_request);
	if (enqueued && gpu_destination) {
		gpu_requests_pending = true;
	}
	if (enqueued) {
		Dictionary report;
		report["actual_destination_type"] = static_cast<int>(p_request->get_destination_type());
		report["actual_backend"] = active_backend->get_name();
		report["was_gpu_decompression_used"] = p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE && is_gpu_decompression_enabled();
		request_execution_reports[static_cast<int64_t>(p_request->get_instance_id())] = report;
	}
	return enqueued;
}

void DSContext::flush_batch() {
	if (!initialized || !active_backend) {
		return;
	}
	bool had_gpu_requests = gpu_requests_pending;
	active_backend->submit_batch();
	if (had_gpu_requests) {
		if (get_completion_fence() == 0 || !wait_for_completion(5000)) {
			UtilityFunctions::push_error("[DirectStorage] GPU destination completion was not made visible to RenderingDevice.");
			return;
		}
		gpu_requests_pending = false;
	}

	uint64_t backend_fence = get_completion_fence();
	completion_fence = backend_fence != 0 ? backend_fence : completion_fence + 1;
}

void DSContext::cancel_requests_with_tag(uint64_t p_tag) {
	if (!initialized || !active_backend) {
		return;
	}
	active_backend->cancel_requests_with_tag(p_tag);
}

void DSContext::set_staging_buffer_size_mb(uint32_t p_size_mb) {
	staging_buffer_size_mb = p_size_mb > 0 ? p_size_mb : DEFAULT_STAGING_BUFFER_SIZE_MB;
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
	ClassDB::bind_method(D_METHOD("is_gpu_decompression_available"), &DSContext::is_gpu_decompression_available);
	ClassDB::bind_method(D_METHOD("is_gpu_decompression_enabled"), &DSContext::is_gpu_decompression_enabled);
	ClassDB::bind_method(D_METHOD("get_gpu_decompression_support_tier"), &DSContext::get_gpu_decompression_support_tier);
	ClassDB::bind_method(D_METHOD("get_capability_report"), &DSContext::get_capability_report);
	ClassDB::bind_method(D_METHOD("is_current_drive_supported"), &DSContext::is_current_drive_supported);
	ClassDB::bind_method(D_METHOD("is_path_supported", "path"), &DSContext::is_path_supported);
	ClassDB::bind_method(D_METHOD("get_drive_info", "path"), &DSContext::get_drive_info, DEFVAL("res://"));
	ClassDB::bind_method(D_METHOD("initialize_ds"), &DSContext::initialize_ds);
	ClassDB::bind_method(D_METHOD("initialize_ds_for_path", "path"), &DSContext::initialize_ds_for_path);
	ClassDB::bind_method(D_METHOD("shutdown_ds"), &DSContext::shutdown_ds);

	ClassDB::bind_method(D_METHOD("enqueue_request", "request"), &DSContext::enqueue_request);
	ClassDB::bind_method(D_METHOD("flush_batch"), &DSContext::flush_batch);
	ClassDB::bind_method(D_METHOD("get_completion_fence"), &DSContext::get_completion_fence);
	ClassDB::bind_method(D_METHOD("wait_for_completion", "timeout_msec"), &DSContext::wait_for_completion);
	ClassDB::bind_method(D_METHOD("cancel_requests_with_tag", "tag"), &DSContext::cancel_requests_with_tag);
	ClassDB::bind_method(D_METHOD("get_request_execution_report", "request"), &DSContext::get_request_execution_report);
	ClassDB::bind_method(D_METHOD("get_actual_destination_type", "request"), &DSContext::get_actual_destination_type);
	ClassDB::bind_method(D_METHOD("get_actual_backend", "request"), &DSContext::get_actual_backend);
	ClassDB::bind_method(D_METHOD("was_gpu_decompression_used", "request"), &DSContext::was_gpu_decompression_used);

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
