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
	if (global_path.is_empty()) {
		global_path = "C:\\";
	}

	// Format drive root cleanly as "X:\"
	String drive_root = global_path.substr(0, 2) + "\\";
	if (drive_root.length() < 3 || drive_root[1] != ':') {
		drive_root = "C:\\";
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
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (!ctx) {
		return false;
	}

	Dictionary drive_info = get_drive_info_for_path("res://");
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

		step_name = "CreateQueue";
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
	std::wstring wpath = path.utf8().get_data() ? std::wstring(path.wide_string().get_data()) : L"";

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
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").");
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
	request.Source.File.Size = src_size;

	PackedByteArray &dest = p_request->get_destination_buffer_ref();
	if (dest.size() < static_cast<int64_t>(uncompressed_size)) {
		dest.resize(static_cast<int64_t>(uncompressed_size));
	}

	request.Destination.Memory.Buffer = dest.ptrw();
	request.Destination.Memory.Size = uncompressed_size;
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

	uint64_t total_bytes = 0;
	for (int i = 0; i < in_flight_requests.size(); i++) {
		if (in_flight_requests[i].is_valid()) {
			uint32_t req_unc_size = in_flight_requests[i]->get_uncompressed_size();
			total_bytes += (req_unc_size > 0) ? req_unc_size : in_flight_requests[i]->get_size();
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
