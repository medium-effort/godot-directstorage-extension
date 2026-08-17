#include "ds_win32_backend.h"
#include "../../core/ds_context.h"
#include "../../core/ds_error_handler.h"
#include "../../core/ds_profiler.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <winioctl.h>
#endif

#include <string>

namespace godot {

void ds_profiler_set_batch_metrics(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes, int p_request_count, int64_t p_decompression_duration_us, int p_error_state);

#ifndef FSCTL_GET_BYPASSIO_STATE
#define FSCTL_GET_BYPASSIO_STATE CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 238, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

struct DS_BYPASSIO_OUTPUT {
	uint32_t State;
	uint32_t Reason;
	uint32_t VolumeDriverState;
	uint32_t StorageDriverState;
};

#ifdef _WIN32
struct DSRuntimeProbe {
	bool available = false;
	String reason;
};

static void unload_runtime_modules(HMODULE p_core, HMODULE p_dstorage) {
	if (p_dstorage) {
		FreeLibrary(p_dstorage);
	}
	if (p_core) {
		FreeLibrary(p_core);
	}
}

struct DllDirectoryScope {
	wchar_t previous[MAX_PATH] = {0};
	bool had_previous = false;

	DllDirectoryScope() {
		had_previous = GetDllDirectoryW(MAX_PATH, previous) > 0;
	}

	~DllDirectoryScope() {
		SetDllDirectoryW(had_previous ? previous : nullptr);
	}
};

static Win32Backend *s_backend_instance = nullptr;
static ID3D12Fence *s_completion_fence = nullptr;
static HANDLE s_completion_event = nullptr;
static uint64_t s_completion_fence_value = 0;
static uint64_t s_completed_fence_value = 0;

static void release_completion_sync() {
	if (s_completion_event) {
		CloseHandle(s_completion_event);
		s_completion_event = nullptr;
	}
	if (s_completion_fence) {
		s_completion_fence->Release();
		s_completion_fence = nullptr;
	}
	s_completion_fence_value = 0;
	s_completed_fence_value = 0;
}

uint64_t ds_win32_get_completion_fence(PlatformBackend *p_backend) {
	if (p_backend != s_backend_instance || !s_completion_fence) {
		return 0;
	}
	return s_completion_fence_value;
}

bool ds_win32_wait_for_completion(PlatformBackend *p_backend, uint64_t p_fence_value, uint32_t p_timeout_msec) {
	if (p_fence_value == 0) {
		return true;
	}
	if (p_backend != s_backend_instance || !s_completion_fence || !s_completion_event) {
		return false;
	}

	uint64_t completed_value = s_completion_fence->GetCompletedValue();
	if (completed_value == UINT64_MAX) {
		return false;
	}
	if (completed_value < p_fence_value) {
		if (FAILED(s_completion_fence->SetEventOnCompletion(p_fence_value, s_completion_event))) {
			return false;
		}
		DWORD wait_result = WaitForSingleObject(s_completion_event, static_cast<DWORD>(p_timeout_msec));
		if (wait_result != WAIT_OBJECT_0) {
			return false;
		}
		completed_value = s_completion_fence->GetCompletedValue();
	}

	if (completed_value < p_fence_value || completed_value == UINT64_MAX) {
		return false;
	}
	s_completed_fence_value = completed_value;
	return true;
}

static DSRuntimeProbe probe_runtime(const String &p_bin_dir, uint16_t p_queue_capacity) {
	DSRuntimeProbe result;
	HMODULE core = nullptr;
	HMODULE dstorage = nullptr;
	String core_path = p_bin_dir + String("/dstoragecore.dll");
	String dstorage_path = p_bin_dir + String("/dstorage.dll");
	std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data()));
	std::wstring w_core_path(reinterpret_cast<const wchar_t *>(core_path.utf16().get_data()));
	std::wstring w_dstorage_path(reinterpret_cast<const wchar_t *>(dstorage_path.utf16().get_data()));

	DllDirectoryScope dll_directory_scope;
	SetDllDirectoryW(w_bin_dir.c_str());
	core = LoadLibraryW(w_core_path.c_str());
	if (!core) {
		result.reason = "DirectStorage runtime DLL 'dstoragecore.dll' could not be loaded from '" + p_bin_dir + "'.";
		return result;
	}
	dstorage = LoadLibraryW(w_dstorage_path.c_str());
	if (!dstorage) {
		result.reason = "DirectStorage runtime DLL 'dstorage.dll' could not be loaded from '" + p_bin_dir + "'.";
		unload_runtime_modules(core, nullptr);
		return result;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : "";
	if (driver_name != "d3d12") {
		result.reason = "Rendering driver is '" + driver_name + "' (Direct3D 12 is required for DirectStorage GPU queues).";
		unload_runtime_modules(core, dstorage);
		return result;
	}

	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	uint64_t device_handle = rd ? rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0) : 0;
	if (!rd || device_handle == 0) {
		result.reason = "A usable D3D12 RenderingDevice is not active.";
		unload_runtime_modules(core, dstorage);
		return result;
	}

	IDStorageFactory *probe_factory = nullptr;
	HRESULT hr = DStorageGetFactory(IID_PPV_ARGS(&probe_factory));
	if (FAILED(hr) || !probe_factory) {
		result.reason = "DStorageGetFactory failed (HRESULT: 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ").";
		unload_runtime_modules(core, dstorage);
		return result;
	}

	DSTORAGE_QUEUE_DESC queue_desc = {};
	queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	queue_desc.Capacity = p_queue_capacity;
	queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
	queue_desc.Name = "DirectStorageCapabilityProbe";
	queue_desc.Device = reinterpret_cast<ID3D12Device *>(device_handle);
	IDStorageQueue1 *probe_queue = nullptr;
	hr = probe_factory->CreateQueue(&queue_desc, IID_PPV_ARGS(&probe_queue));
	if (FAILED(hr) || !probe_queue) {
		result.reason = "DirectStorage queue creation failed (HRESULT: 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ").";
		probe_factory->Release();
		unload_runtime_modules(core, dstorage);
		return result;
	}

	probe_queue->Close();
	probe_queue->Release();
	probe_factory->Release();
	unload_runtime_modules(core, dstorage);
	result.available = true;
	result.reason = "DirectStorage runtime, D3D12 device, factory, and queue are available.";
	return result;
}
#endif

Dictionary Win32Backend::get_drive_info_for_path(const String &p_path) {
	Dictionary res;
	res["drive"] = "";
	res["bus_type"] = "Unknown";
	res["file_system"] = "Unknown";
	res["bypassio_state"] = "Unsupported";
	res["directstorage_supported"] = false;
	res["reason"] = "";

#ifdef _WIN32
	String global_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (p_path.begins_with("res://") || p_path.begins_with("user://"))) {
		global_path = ps->globalize_path(p_path);
	}
	if (global_path.strip_edges().is_empty()) {
		global_path = "C:\\";
	}

	String norm_path = global_path.replace("/", "\\");
	std::wstring w_norm_path(reinterpret_cast<const wchar_t *>(norm_path.utf16().get_data()));
	wchar_t volume_path[MAX_PATH] = {0};
	String drive_root;
	if (GetVolumePathNameW(w_norm_path.c_str(), volume_path, MAX_PATH)) {
		drive_root = String(reinterpret_cast<const char16_t *>(volume_path));
	} else {
		drive_root = norm_path.substr(0, 2) + "\\";
		if (drive_root.length() < 3 || drive_root[1] != ':') {
			drive_root = "C:\\";
		}
	}
	if (!drive_root.ends_with("\\")) {
		drive_root += "\\";
	}
	res["drive"] = drive_root;

	// 3. Query FileSystem Name
	wchar_t fs_name[MAX_PATH] = {0};
	wchar_t vol_name[MAX_PATH] = {0};
	std::wstring w_drive_root(reinterpret_cast<const wchar_t *>(drive_root.utf16().get_data()));
	if (GetVolumeInformationW(w_drive_root.c_str(), vol_name, MAX_PATH, nullptr, nullptr, nullptr, fs_name, MAX_PATH)) {
		res["file_system"] = String(reinterpret_cast<const char16_t *>(fs_name));
	}

	// 4. Query Storage BusType & BypassIO State
	String volume_device = "\\\\.\\" + drive_root.substr(0, 2);
	std::wstring w_volume_device(reinterpret_cast<const wchar_t *>(volume_device.utf16().get_data()));

	HANDLE hDevice = CreateFileW(
			w_volume_device.c_str(),
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr);

	if (hDevice != INVALID_HANDLE_VALUE) {
		STORAGE_PROPERTY_QUERY query = {};
		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;

		STORAGE_DEVICE_DESCRIPTOR dev_desc = {};
		DWORD bytes_returned = 0;

		if (DeviceIoControl(
					hDevice,
					IOCTL_STORAGE_QUERY_PROPERTY,
					&query,
					sizeof(query),
					&dev_desc,
					sizeof(dev_desc),
					&bytes_returned,
					nullptr)) {

			switch (dev_desc.BusType) {
				case BusTypeNvme:
					res["bus_type"] = "NVMe";
					break;
				case BusTypeSata:
					res["bus_type"] = "SATA";
					break;
				case BusTypeUsb:
					res["bus_type"] = "USB";
					break;
				case BusTypeSd:
					res["bus_type"] = "SD";
					break;
				case BusTypeMmc:
					res["bus_type"] = "MMC";
					break;
				case BusTypeVirtual:
					res["bus_type"] = "Virtual";
					break;
				default:
					res["bus_type"] = "BusType_" + String::num_int64(dev_desc.BusType);
					break;
			}
		}

		// Query Windows Kernel BypassIO state
		DS_BYPASSIO_OUTPUT bypassio_out = {};
		DWORD bypassio_bytes = 0;
		if (DeviceIoControl(
					hDevice,
					FSCTL_GET_BYPASSIO_STATE,
					nullptr,
					0,
					&bypassio_out,
					sizeof(bypassio_out),
					&bypassio_bytes,
					nullptr)) {
			if (bypassio_out.State == 0) {
				res["bypassio_state"] = "Enabled";
			} else if (bypassio_out.State == 1) {
				res["bypassio_state"] = "Disabled";
			} else {
				res["bypassio_state"] = "Unsupported";
			}
		}

		CloseHandle(hDevice);
	}

	bool is_nvme = (res["bus_type"] == "NVMe");
	bool is_ntfs = (res["file_system"] == "NTFS");

	if (!is_nvme) {
		res["directstorage_supported"] = false;
		res["reason"] = "Drive is non-NVMe (BusType: " + String(res["bus_type"]) + "). DirectStorage requires NVMe for hardware acceleration.";
	} else if (!is_ntfs) {
		res["directstorage_supported"] = false;
		res["reason"] = "File system is " + String(res["file_system"]) + ". DirectStorage requires NTFS.";
	} else {
		res["directstorage_supported"] = true;
		res["reason"] = "Hardware NVMe drive and NTFS file system support DirectStorage hardware acceleration.";
	}

#else
	res["reason"] = "Platform is non-Windows.";
#endif

	return res;
}

bool Win32Backend::is_available() const {
#ifdef _WIN32
	Dictionary drive_info = get_drive_info_for_path("res://");
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::print("[DirectStorage] Backend unavailable: " + String(drive_info["reason"]));
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		UtilityFunctions::print("[DirectStorage] Backend unavailable: ProjectSettings is not initialized.");
		return false;
	}

	DSRuntimeProbe probe = probe_runtime(ps->globalize_path("res://addons/directstorage_accelerator/bin"), 2000);
	if (!probe.available) {
		UtilityFunctions::print("[DirectStorage] Backend unavailable: " + probe.reason);
	}
	return probe.available;
#else
	return false;
#endif
}

String Win32Backend::get_name() const {
	return "Win32DirectStorage";
}

bool Win32Backend::initialize() {
	return initialize("");
}

bool Win32Backend::is_path_valid(const String &p_path) {
#ifdef _WIN32
	if (p_path.strip_edges().is_empty()) {
		return false;
	}

	String global_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (p_path.begins_with("res://") || p_path.begins_with("user://"))) {
		global_path = ps->globalize_path(p_path);
	}

	String norm_path = global_path.replace("/", "\\");
	if (norm_path.length() < 3 || (norm_path[1] != ':' && !norm_path.begins_with("\\\\"))) {
		return false;
	}

	std::wstring w_path(reinterpret_cast<const wchar_t *>(norm_path.utf16().get_data()));
	DWORD attributes = GetFileAttributesW(w_path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	// If specific file does not exist yet, check its parent directory
	String parent_dir = norm_path.get_base_dir();
	if (!parent_dir.is_empty() && parent_dir != norm_path) {
		std::wstring w_parent(reinterpret_cast<const wchar_t *>(parent_dir.utf16().get_data()));
		DWORD parent_attribs = GetFileAttributesW(w_parent.c_str());
		if (parent_attribs != INVALID_FILE_ATTRIBUTES && (parent_attribs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			return true;
		}
	}

	// Check if the drive root volume itself is valid
	wchar_t volume_path[MAX_PATH] = {0};
	if (GetVolumePathNameW(w_path.c_str(), volume_path, MAX_PATH)) {
		DWORD vol_attribs = GetFileAttributesW(volume_path);
		if (vol_attribs != INVALID_FILE_ATTRIBUTES) {
			return true;
		}
	}

	return false;
#else
	return false;
#endif
}

bool Win32Backend::initialize(const String &p_path) {
#ifdef _WIN32
	s_backend_instance = this;
	s_completion_fence_value = 0;
	s_completed_fence_value = 0;
	DSContext *ctx = DSContext::get_singleton();
	if (!ctx) {
		return false;
	}

	String validation_path = p_path.is_empty() ? "res://" : p_path;
	if (!p_path.is_empty() && !is_path_valid(p_path)) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Path '" + p_path + "' does not exist or is not a valid path.");
		return false;
	}

	Dictionary drive_info = get_drive_info_for_path(validation_path);
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Drive " + String(drive_info["drive"]) + " is unsupported (" + String(drive_info["reason"]) + ").");
		return false;
	}

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: ProjectSettings is not initialized.");
		return false;
	}
	{
		String bin_dir = ps->globalize_path("res://addons/directstorage_accelerator/bin");
		uint16_t capacity = ctx->get_queue_capacity();
		DSRuntimeProbe probe = probe_runtime(bin_dir, capacity);
		if (!probe.available) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: " + probe.reason);
			return false;
		}
		String dstoragecore_path = bin_dir + "/dstoragecore.dll";
		String dstorage_path = bin_dir + "/dstorage.dll";

		std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(bin_dir.utf16().get_data()));
		std::wstring w_core_path(reinterpret_cast<const wchar_t *>(dstoragecore_path.utf16().get_data()));
		std::wstring w_ds_path(reinterpret_cast<const wchar_t *>(dstorage_path.utf16().get_data()));

		SetDllDirectoryW(w_bin_dir.c_str());
		dstoragecore_module = LoadLibraryW(w_core_path.c_str());
		dstorage_module = LoadLibraryW(w_ds_path.c_str());
		if (!dstoragecore_module || !dstorage_module) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: DirectStorage runtime DLLs could not be loaded from '" + bin_dir + "'.");
			unload_runtime_modules(dstoragecore_module, dstorage_module);
			dstoragecore_module = nullptr;
			dstorage_module = nullptr;
			return false;
		}
	}

	uint32_t staging_mb = ctx->get_staging_buffer_size_mb();
	bool disable_gpu = ctx->get_disable_gpu_decompression();
	uint16_t capacity = ctx->get_queue_capacity();

	DSTORAGE_CONFIGURATION config = {};
	config.DisableTelemetry = TRUE;
	config.DisableGpuDecompression = disable_gpu;
	config.DisableGpuDecompressionMetacommand = disable_gpu;

	// DStorageSetConfiguration is optional. Ignore non-fatal configuration errors.
	DStorageSetConfiguration(&config);

	String step_name = "DStorageGetFactory";
	HRESULT hr = DStorageGetFactory(IID_PPV_ARGS(&factory));

	if (SUCCEEDED(hr) && factory) {
		factory->SetDebugFlags(DSTORAGE_DEBUG_NONE);
		factory->SetStagingBufferSize(staging_mb * 1024 * 1024);

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = capacity;
		queueDesc.Priority = DSTORAGE_PRIORITY_NORMAL;
		queueDesc.Name = "DirectStorageMemoryQueue";
		RenderingServer *rs = RenderingServer::get_singleton();
		String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : "";
		if (driver_name != "d3d12") {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Rendering driver is '" + driver_name + "' (Direct3D 12 is required).");
			return false;
		}
		RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
		uint64_t device_handle = rd ? rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0) : 0;
		if (!rd || device_handle == 0) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: A usable D3D12 RenderingDevice is not active.");
			return false;
		}
		queueDesc.Device = reinterpret_cast<ID3D12Device *>(device_handle);

		if (FAILED(reinterpret_cast<ID3D12Device *>(device_handle)->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s_completion_fence))) || !s_completion_fence) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the D3D12 completion fence.");
			return false;
		}
		s_completion_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!s_completion_event) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the completion event.");
			return false;
		}

		step_name = "CreateQueue";
		hr = factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&memory_queue));
	}

	if (SUCCEEDED(hr) && memory_queue) {
		batch_event = CreateEvent(NULL, FALSE, FALSE, NULL);
		if (!batch_event) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the batch completion event.");
			return false;
		}

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->register_queue(memory_queue);
		}
		RenderingServer *rs = RenderingServer::get_singleton();
		String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : "unknown";
		UtilityFunctions::print("[DirectStorage] Win32DirectStorage ready: driver=" + driver_name +
				", staging_buffer=" + String::num_uint64(staging_mb) + " MiB, queue_capacity=" +
				String::num_uint64(capacity) + ", gpu_decompression=" + (disable_gpu ? "disabled" : "enabled"));
		return true;
	} else {
		uint32_t u_hr = static_cast<uint32_t>(hr);
		String hr_hex = String::num_uint64(u_hr, 16);
		UtilityFunctions::print("[DirectStorage] Win32Backend DirectStorage " + step_name + " failed (HRESULT: 0x" + hr_hex + ").");
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

	for (int i = 0; i < open_files.size(); i++) {
		if (open_files[i]) {
			open_files[i]->Close();
			open_files[i]->Release();
		}
	}
	open_files.clear();
	in_flight_requests.clear();
	release_completion_sync();
	if (s_backend_instance == this) {
		s_backend_instance = nullptr;
	}
	unload_runtime_modules(dstoragecore_module, dstorage_module);
	dstoragecore_module = nullptr;
	dstorage_module = nullptr;
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
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (path.begins_with("res://") || path.begins_with("user://"))) {
		path = ps->globalize_path(path);
	}
	String norm_file_path = path.replace("/", "\\");
	std::wstring wpath(reinterpret_cast<const wchar_t *>(norm_file_path.utf16().get_data()));

	IDStorageFile *file = nullptr;
	HRESULT hr = factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr) || !file) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend: Failed to open file '" + path + "'.");
		return false;
	}

	uint32_t src_size = p_request->get_size();
	uint32_t uncompressed_size = p_request->get_uncompressed_size();
	if (p_request->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = src_size;
	}

	if (src_size == 0 || uncompressed_size == 0) {
		file->Close();
		file->Release();
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").");
		return false;
	}

	DSContext *ctx = DSContext::get_singleton();
	uint64_t staging_size = ctx ? static_cast<uint64_t>(ctx->get_staging_buffer_size_mb()) * 1024ULL * 1024ULL : 0;
	uint64_t request_size = MAX(static_cast<uint64_t>(src_size), static_cast<uint64_t>(uncompressed_size));
	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE && staging_size > 0 && request_size > staging_size) {
		file->Close();
		file->Release();
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("DirectStorage request rejected: file '" + path + "' has compressed payload " +
				String::num_uint64(src_size) + " bytes and decompressed payload " + String::num_uint64(uncompressed_size) +
				" bytes, but the staging buffer is " + String::num_uint64(ctx->get_staging_buffer_size_mb()) +
				" MiB. Increase Project Settings > DirectStorage > Staging Buffer Size to at least " +
				String::num_uint64((request_size + 1048575ULL) / 1048576ULL) + " MiB.");
		return false;
	}

	bool gpu_buffer_destination = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool gpu_texture_destination = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *destination_buffer = nullptr;
	ID3D12Resource *destination_texture = nullptr;
	if (gpu_buffer_destination) {
		String destination_error;
		RenderingServer *rs = RenderingServer::get_singleton();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU buffer destination requires the D3D12 rendering driver.";
		} else {
			RenderingDevice *rd = rs->get_rendering_device();
			RID destination_rid = p_request->get_destination_resource();
			if (!rd) {
				destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU buffer destination.";
			} else if (!destination_rid.is_valid()) {
				destination_error = "Win32Backend: GPU buffer destination resource RID is invalid.";
			} else {
				// Godot exposes the backend resource through its public driver-resource API.
				// The returned pointer is owned by RenderingDevice; the caller must keep the
					// RID alive until the synchronously flushed DirectStorage batch completes.
					uint64_t resource_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_BUFFER, destination_rid, 0);
					destination_buffer = reinterpret_cast<ID3D12Resource *>(resource_handle);
					if (!destination_buffer) {
						destination_error = "Win32Backend: GPU buffer RID could not be mapped to an ID3D12Resource.";
					} else {
						D3D12_RESOURCE_DESC buffer_desc = destination_buffer->GetDesc();
						if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || uncompressed_size > buffer_desc.Width) {
							destination_error = "Win32Backend: GPU buffer destination size exceeds the mapped D3D12 buffer resource.";
						}
					}
				}
		}

		if (!destination_error.is_empty()) {
			file->Close();
			file->Release();
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message(destination_error);
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(destination_error);
			}
			return false;
		}
	}
	if (gpu_texture_destination) {
		String destination_error;
		RenderingServer *rs = RenderingServer::get_singleton();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU texture destination requires the D3D12 rendering driver.";
		} else {
			RenderingDevice *rd = rs->get_rendering_device();
			RID destination_rid = p_request->get_destination_resource();
			if (!rd) {
				destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU texture destination.";
			} else if (!destination_rid.is_valid()) {
				destination_error = "Win32Backend: GPU texture destination resource RID is invalid.";
			} else {
				uint64_t resource_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE, destination_rid, 0);
				destination_texture = reinterpret_cast<ID3D12Resource *>(resource_handle);
				if (!destination_texture) {
					destination_error = "Win32Backend: GPU texture RID could not be mapped to an ID3D12Resource.";
				} else {
					D3D12_RESOURCE_DESC texture_desc = destination_texture->GetDesc();
					if (texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
							texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
							texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
						destination_error = "Win32Backend: GPU texture destination RID does not reference a texture resource.";
					} else {
						uint64_t mip_levels = texture_desc.MipLevels > 0 ? texture_desc.MipLevels : 1;
						uint64_t array_size = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : texture_desc.DepthOrArraySize;
						uint64_t subresource_total = mip_levels * array_size;
						uint32_t first_subresource = p_request->get_first_subresource();
						uint32_t subresource_count = p_request->get_subresource_count();
						if (subresource_count == 0 || first_subresource >= subresource_total ||
								subresource_count > subresource_total - first_subresource) {
							destination_error = "Win32Backend: GPU texture subresource range is out of bounds (first=" +
									String::num_uint64(first_subresource) + ", count=" + String::num_uint64(subresource_count) +
									", available=" + String::num_uint64(subresource_total) + ").";
						} else if (p_request->get_texture_layout() != 0 &&
								p_request->get_texture_layout() != static_cast<uint32_t>(texture_desc.Layout)) {
							destination_error = "Win32Backend: GPU texture layout metadata does not match the destination resource.";
						} else if (p_request->get_texture_format() != 0) {
							uint64_t native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, destination_rid, 0);
							if (native_format == 0 || p_request->get_texture_format() != native_format) {
								destination_error = "Win32Backend: GPU texture format metadata does not match the destination resource.";
							}
						}
					}
				}
			}
		}

		if (!destination_error.is_empty()) {
			file->Close();
			file->Release();
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message(destination_error);
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(destination_error);
			}
			return false;
		}
	}

	DSTORAGE_REQUEST request = {};
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = gpu_buffer_destination ? DSTORAGE_REQUEST_DESTINATION_BUFFER :
				gpu_texture_destination ? (p_request->get_subresource_count() > 1 ? DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE : DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION) :
				DSTORAGE_REQUEST_DESTINATION_MEMORY;
	request.Options.CompressionFormat = (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)
			? DSTORAGE_COMPRESSION_FORMAT_GDEFLATE
			: DSTORAGE_COMPRESSION_FORMAT_NONE;

	request.Source.File.Source = file;
	request.Source.File.Offset = p_request->get_offset();
	request.Source.File.Size = src_size;

	if (gpu_buffer_destination) {
		request.Destination.Buffer.Resource = destination_buffer;
		request.Destination.Buffer.Offset = 0;
		request.Destination.Buffer.Size = uncompressed_size;
	} else if (gpu_texture_destination) {
		D3D12_RESOURCE_DESC texture_desc = destination_texture->GetDesc();
		uint64_t mip_levels = texture_desc.MipLevels > 0 ? texture_desc.MipLevels : 1;
		uint32_t first_subresource = p_request->get_first_subresource();
		uint32_t subresource_count = p_request->get_subresource_count();
		if (subresource_count > 1) {
			request.Destination.MultipleSubresourcesRange.Resource = destination_texture;
			request.Destination.MultipleSubresourcesRange.FirstSubresource = first_subresource;
			request.Destination.MultipleSubresourcesRange.NumSubresources = subresource_count;
		} else {
			uint64_t mip_level = first_subresource % mip_levels;
			uint64_t width = MAX(static_cast<uint64_t>(1), texture_desc.Width >> mip_level);
			uint64_t height = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D ? 1 : MAX(static_cast<uint64_t>(1), static_cast<uint64_t>(texture_desc.Height) >> mip_level);
			uint64_t depth = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? MAX(static_cast<uint64_t>(1), static_cast<uint64_t>(texture_desc.DepthOrArraySize) >> mip_level) : 1;
			request.Destination.Texture.Resource = destination_texture;
			request.Destination.Texture.SubresourceIndex = first_subresource;
			request.Destination.Texture.Region = { 0, 0, 0, static_cast<UINT>(width), static_cast<UINT>(height), static_cast<UINT>(depth) };
		}
	} else {
		PackedByteArray &dest = p_request->get_destination_buffer_ref();
		if (dest.size() < static_cast<int64_t>(uncompressed_size)) {
			dest.resize(static_cast<int64_t>(uncompressed_size));
		}

		request.Destination.Memory.Buffer = dest.ptrw();
		request.Destination.Memory.Size = uncompressed_size;
	}
	request.UncompressedSize = uncompressed_size;
	request.CancellationTag = p_request->get_cancellation_tag();

	memory_queue->EnqueueRequest(&request);
	open_files.push_back(file);

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

	uint64_t total_compressed_bytes = 0;
	uint64_t total_uncompressed_bytes = 0;
	int request_count = 0;
	for (int i = 0; i < in_flight_requests.size(); i++) {
		if (in_flight_requests[i].is_valid()) {
			uint32_t req_size = in_flight_requests[i]->get_size();
			uint32_t req_unc_size = in_flight_requests[i]->get_uncompressed_size();
			total_compressed_bytes += req_size;
			total_uncompressed_bytes += (req_unc_size > 0) ? req_unc_size : req_size;
			request_count++;
		}
	}

	uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->clear_error();
	}

	bool batch_submitted = true;
	if (!batch_event || !s_completion_fence || !s_completion_event) {
		UtilityFunctions::push_error("[DirectStorage] GPU/DirectStorage completion synchronization is unavailable.");
		for (int i = 0; i < in_flight_requests.size(); i++) {
			if (in_flight_requests[i].is_valid()) {
				in_flight_requests[i]->set_status(DSRequest::STATUS_FAILED);
				in_flight_requests[i]->set_error_message("DirectStorage completion fence is unavailable.");
			}
		}
		batch_submitted = false;
	} else {
		uint64_t completion_value = s_completion_fence_value + 1;
		ResetEvent(s_completion_event);
		if (FAILED(s_completion_fence->SetEventOnCompletion(completion_value, s_completion_event))) {
			s_completion_fence_value = completion_value;
			UtilityFunctions::push_error("[DirectStorage] Failed to arm the D3D12 completion fence.");
			for (int i = 0; i < in_flight_requests.size(); i++) {
				if (in_flight_requests[i].is_valid()) {
					in_flight_requests[i]->set_status(DSRequest::STATUS_FAILED);
					in_flight_requests[i]->set_error_message("Failed to arm the DirectStorage completion fence.");
				}
			}
			batch_submitted = false;
		} else {
			s_completion_fence_value = completion_value;
			memory_queue->EnqueueSignal(s_completion_fence, completion_value);
			memory_queue->EnqueueSetEvent(batch_event);
		}
	}
	if (batch_submitted) {
		memory_queue->Submit();
	}

	if (batch_submitted) {
		DWORD wait_res = WaitForSingleObject(batch_event, 5000);
		if (wait_res == WAIT_OBJECT_0) {
			s_completed_fence_value = s_completion_fence ? s_completion_fence->GetCompletedValue() : s_completed_fence_value;
			uint64_t end_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			uint64_t duration_us = (end_time > start_time) ? (end_time - start_time) : 1;
			if (err_handler) {
				err_handler->process_queue_errors();
			}

			for (int i = 0; i < in_flight_requests.size(); i++) {
				if (in_flight_requests[i].is_valid()) {
					if (err_handler && err_handler->has_error()) {
						in_flight_requests[i]->set_status(DSRequest::STATUS_FAILED);
						in_flight_requests[i]->set_error_message(err_handler->get_last_error());
					} else {
						in_flight_requests[i]->set_status(DSRequest::STATUS_COMPLETED);
					}
				}
			}

			DSProfiler *profiler = DSProfiler::get_singleton();
			if (profiler) {
				ds_profiler_set_batch_metrics(
						total_compressed_bytes,
						total_uncompressed_bytes,
						request_count,
						-1, // GPU decompression is included in completion time, not separately measurable.
						(err_handler && err_handler->has_error()) ? 1 : 0);
				profiler->record_workload(total_compressed_bytes, duration_us);
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

	for (int i = 0; i < open_files.size(); i++) {
		if (open_files[i]) {
			open_files[i]->Close();
			open_files[i]->Release();
		}
	}
	open_files.clear();
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
