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
ID3D12Device *ds_win32_get_d3d12_device();
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

	// DirectStorage GPU synchronization contract (enforced, not advisory):
	// - Transfers write directly into D3D12 resources on the DirectStorage queue and are
	//   synchronized via ID3D12Fence / IDStorageQueue completion fences.
	// - Godot's main RenderingDevice submits command buffers per-frame; manual submit()/sync()
	//   on the main instance is prohibited (Condition "is_main_instance" is true).
	// - DirectStorage SDK requirement: destination resources for GPU decompression
	//   (DSTORAGE_DESTINATION_MULTIPLE_SUBRESOURCES / buffer destinations)
	//   MUST be in D3D12_RESOURCE_STATE_COMMON. Enforcement (Win32Backend::enqueue_request):
	//   (1) ID3D12Resource exposes no current-state query, so COMMON can only be
	//       guaranteed by the barrier owner - Godot's RenderingDevice. This extension
	//       therefore never records its own ResourceBarrier: a foreign barrier would
	//       race Godot's per-frame command lists and corrupt RD state tracking.
	//       Reject-with-clear-error is the safer option, documented here.
	//   (2) A destination owned by an in-flight DirectStorage batch (possibly
	//       mid-decompression, post-render-target use, state unknowable) fails GPU
	//       enqueue loudly instead of proceeding. No silent CPU fallback: re-targeting
	//       to memory behind the caller's back would leave the GPU texture unwritten
	//       while the caller samples it as success.
	// - Async no-sample-before-batch_completed is enforced the same way: destinations
	//   stay flagged in-flight until the completion fence confirms and results are
	//   collected, so any sample/reuse-before-complete surfaces as a loud enqueue
	//   error pointing at batch_completed / wait_for_completion, and batch_completed
	//   itself only fires after fence-confirmed collection. This gate below (reject
	//   GPU work when D3D12 is not active) plus the fence wait in wait_for_completion
	//   are the context-level halves of that contract.
	// - Hazard note: CPU fence wait orders the DS copy before subsequently submitted
	//   Godot command lists (safe for sync flush_batch, exposed for async
	//   flush_batch_async until collect_completed_results).
	return true;
#else
	UtilityFunctions::push_error("[DirectStorage] RenderingDevice synchronization unavailable during " + p_phase + ": platform is not Windows.");
	return false;
#endif
}

DSContext *DSContext::singleton = nullptr;
std::once_flag DSContext::support_probe_once;
bool DSContext::supported_probe_result = false;

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

void DSContext::evict_old_reports_if_needed_locked() {
	while (request_execution_reports.size() > (int)MAX_EXECUTION_REPORTS && !report_order.empty()) {
		int64_t request_id = report_order.front();
		if (pending_request_ids.count(request_id) > 0) {
			break;
		}
		std::unordered_map<int64_t, Ref<DSRequest>>::const_iterator tracked_it = tracked_requests.find(request_id);
		if (tracked_it != tracked_requests.end() && tracked_it->second.is_valid()) {
			DSRequest::Status st = tracked_it->second->get_status();
			if (st != DSRequest::STATUS_COMPLETED && st != DSRequest::STATUS_FAILED) {
				break;
			}
		}
		report_order.pop_front();
		request_execution_reports.erase(request_id);
		tracked_requests.erase(request_id);
	}
	// Observability for the 32768-cap risk: eviction legitimately blocks on
	// pending/in-flight IDs (they must not be freed). If the deque is still
	// over capacity here, growth is unbounded until those requests terminate.
	// Verbose-only to avoid spamming the main-thread flush path per request.
	if (request_execution_reports.size() > (int)MAX_EXECUTION_REPORTS) {
		UtilityFunctions::print_verbose("[DirectStorage] Execution reports over cap (", (int64_t)request_execution_reports.size(), " > ", (int64_t)MAX_EXECUTION_REPORTS, ") with pending/in-flight IDs blocking eviction; growth bounded by request termination.");
	}
}

void DSContext::remove_report_locked(int64_t p_request_id) {
	request_execution_reports.erase(p_request_id);
	tracked_requests.erase(p_request_id);
	for (std::deque<int64_t>::iterator it = report_order.begin(); it != report_order.end();) {
		if (*it == p_request_id) {
			it = report_order.erase(it);
		} else {
			++it;
		}
	}
}

void DSContext::count_stream_request_terminal(const Ref<DSRequest> &p_request) {
	// Takes stream_mutex only; safe both inside finalize_completed_reports_locked
	// (context_mutex held, order context -> stream) and on its own.
	if (p_request.is_null()) {
		return;
	}
	uint64_t tag = p_request->get_cancellation_tag();
	std::lock_guard<std::mutex> stream_lock(stream_mutex);
	std::unordered_map<uint64_t, StreamTagInfo>::iterator it = stream_tags.find(tag);
	if (it != stream_tags.end()) {
		it->second.completed++;
	}
}

void DSContext::finalize_completed_reports_locked() {
	for (std::unordered_map<int64_t, Ref<DSRequest>>::iterator it = tracked_requests.begin(); it != tracked_requests.end();) {
		Ref<DSRequest> request = it->second;
		DSRequest::Status status = request->get_status();
		if (status != DSRequest::STATUS_COMPLETED && status != DSRequest::STATUS_FAILED) {
			++it;
			continue;
		}

		int64_t request_id = it->first;
		Dictionary report = request_execution_reports.get(request_id, Dictionary());
		if (report.size() > 0) {
			bool gpu_decompression_pending = report.get("gpu_decompression_pending", false);
			report["gpu_decompression_pending"] = false;
			report["was_gpu_decompression_used"] = gpu_decompression_pending && status == DSRequest::STATUS_COMPLETED &&
				String(report.get("actual_backend", "None")) == "Win32DirectStorage" && is_gpu_decompression_enabled();
			request_execution_reports[request_id] = report;
		}
		count_stream_request_terminal(request);
		remove_report_locked(request_id);
		it = tracked_requests.begin();
	}
}

bool DSContext::is_supported() const {
	if (active_backend) {
		return active_backend->get_name() == "Win32DirectStorage";
	}
	std::call_once(support_probe_once, []() {
#ifdef _WIN32
		Win32Backend backend;
		DSContext::supported_probe_result = backend.is_available();
#else
		DSContext::supported_probe_result = false;
#endif
	});
	return supported_probe_result;
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
	return rd != nullptr;
#else
	return false;
#endif
}

bool DSContext::is_initialized() const {
	return initialized;
}

DSContext::InitStatus DSContext::get_init_status() const {
	return init_status;
}

namespace {

#ifdef _WIN32
static DSTORAGE_COMPRESSION_SUPPORT query_gpu_decompression_support(uint16_t p_queue_capacity) {
	static DSTORAGE_COMPRESSION_SUPPORT s_cached_support = DSTORAGE_COMPRESSION_SUPPORT_NONE;
	static bool s_support_cached = false;
	if (s_support_cached) {
		return s_cached_support;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return DSTORAGE_COMPRESSION_SUPPORT_NONE;
	}

	RenderingDevice *rd = rs->get_rendering_device();
	if (!rd) {
		return DSTORAGE_COMPRESSION_SUPPORT_NONE;
	}

	ID3D12Device *d3d12_device = ds_win32_get_d3d12_device();
	if (!d3d12_device) {
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
			queue_desc.Device = d3d12_device;

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
	s_cached_support = support;
	s_support_cached = true;
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
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		if (request_execution_reports.has(request_id)) {
			std::unordered_map<int64_t, Ref<DSRequest>>::const_iterator tracked = tracked_requests.find(request_id);
			if (tracked != tracked_requests.end() && tracked->second.ptr() == p_request.ptr()) {
				return request_execution_reports.get(request_id, report);
			}
		}
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

uint64_t DSContext::get_async_batches_submitted() const {
	// Counts submitted batches so callers can verify batching efficiency
	// (many textures per submit). No queue merge; the sync and async
	// queues stay separate.
	if (!initialized || !active_backend) {
		return 0;
	}
	if (active_backend->get_name() != "Win32DirectStorage") {
		const FallbackBackend *fallback = static_cast<const FallbackBackend *>(active_backend);
		return fallback->get_async_batches_submitted();
	}
	std::lock_guard<std::mutex> lock(context_mutex);
	return async_batches_submitted_cache;
}

uint64_t DSContext::get_in_flight_async_batch_count() const {
	if (!initialized || !active_backend) {
		return 0;
	}
	if (active_backend->get_name() != "Win32DirectStorage") {
		const FallbackBackend *fallback = static_cast<const FallbackBackend *>(active_backend);
		return static_cast<uint64_t>(fallback->get_in_flight_async_batch_count());
	}
	// Win32 exposes no per-slot counter; the completion gate is the only
	// observable in-flight state (0 or 1 from the caller's viewpoint).
	return active_backend->is_batch_complete() ? 0 : 1;
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
	std::lock_guard<std::mutex> lock(context_mutex);
	return completion_fence;
}

bool DSContext::wait_for_completion(int p_timeout_msec) {
	if (p_timeout_msec < 0) {
		UtilityFunctions::push_error("[DirectStorage] Completion wait rejected: timeout must not be negative.");
		return false;
	}

	uint64_t fence_value = get_completion_fence();
	if (fence_value == 0) {
		bool has_gpu_pending = false;
		{
			std::lock_guard<std::mutex> lock(context_mutex);
			has_gpu_pending = gpu_requests_pending;
		}
		if (has_gpu_pending) {
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
	if (p_path.strip_edges().is_empty()) {
		UtilityFunctions::push_warning("[DirectStorage] Empty path supplied; falling back to res:// initialization.");
		return initialize_ds() ? OK : ERR_CANT_CREATE;
	}

#ifdef _WIN32
	if (!Win32Backend::is_path_valid(p_path)) {
		UtilityFunctions::push_warning("[DirectStorage] Path '" + p_path + "' is invalid; falling back to res:// initialization.");
		if (initialized) {
			shutdown_ds();
		}
		return initialize_ds() ? OK : ERR_CANT_CREATE;
	}

	Dictionary drive_info = Win32Backend::get_drive_info_for_path(p_path);
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::push_warning("[DirectStorage] Path '" + p_path + "' is on an unsupported volume; falling back to res:// initialization.");
		if (initialized) {
			shutdown_ds();
		}
		return initialize_ds() ? OK : ERR_CANT_CREATE;
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
	return initialize_ds() ? OK : ERR_CANT_CREATE;
#else
	UtilityFunctions::push_warning("[DirectStorage] Path-aware initialization is unavailable on this platform; falling back to res:// initialization.");
	return initialize_ds() ? OK : ERR_CANT_CREATE;
#endif
}

bool DSContext::initialize_ds() {
	if (initialized) {
		return true;
	}
	shut_down = false;

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
		init_status = INIT_STATUS_DIRECTSTORAGE_D3D12;
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
	if (!active_backend->initialize()) {
		memdelete(active_backend);
		active_backend = nullptr;
		init_status = INIT_STATUS_FAILED;
		UtilityFunctions::push_error("[DirectStorage] DirectStorage Context initialization failed: FallbackBackend could not start.");
		return false;
	}
	initialized = true;
	init_status = INIT_STATUS_SOFTWARE_FALLBACK;
	UtilityFunctions::print("[DirectStorage] DirectStorage Context initialized with FallbackBackend.");
	return true;
}

void DSContext::shutdown_ds() {
	shut_down = true;
	init_status = INIT_STATUS_UNINITIALIZED;
	if (!initialized) {
		return;
	}

	if (active_backend) {
		active_backend->shutdown();
		memdelete(active_backend);
		active_backend = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(context_mutex);
		gpu_requests_pending = false;
		completion_fence = 0;
		pending_request_count = 0;
		async_batches_submitted_cache = 0;
		pending_batch_request_counts.clear();
		request_execution_reports.clear();
		tracked_requests.clear();
		pending_request_ids.clear();
		report_order.clear();
	}
	{
		// Stream state: drop queued Refs, freeze tags as cancelled so any
		// late-arriving stream request with a stale tag is marked finished
		// instead of queueing against a dead backend.
		std::lock_guard<std::mutex> stream_lock(stream_mutex);
		stream_queue.clear();
		for (std::unordered_map<uint64_t, StreamTagInfo>::iterator it = stream_tags.begin(); it != stream_tags.end(); ++it) {
			it->second.cancel_requested = true;
		}
	}
	initialized = false;
	UtilityFunctions::print("[DirectStorage] DirectStorage Context shut down.");
}

bool DSContext::enqueue_request(Ref<DSRequest> p_request, bool p_from_stream) {
	if (p_from_stream) {
		// Async prewarm path: queue only, never touch the sync loader path
		// (pending_request_count / pending_request_ids / gpu flags), so the
		// batch manager's stream flush cannot steal sync _load requests.
		// The Ref held in stream_queue keeps the request alive until
		// flush_stream_async() drains it or cancel_prewarm() cancels it.
		// Backend init is deferred to flush time to keep the enqueue call
		// non-blocking on the calling thread.
		if (p_request.is_null()) {
			return false;
		}
		if (shut_down) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage Context is shut down.");
			return false;
		}
		uint64_t tag = p_request->get_cancellation_tag();
		std::lock_guard<std::mutex> lock(stream_mutex);
		StreamTagInfo &info = stream_tags[tag]; // auto-create ad-hoc tags
		info.tag = tag;
		if (info.cancel_requested) {
			info.cancelled++;
			info.completed++;
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("Prewarm cancelled before enqueue.");
			return false;
		}
		stream_queue.push_back(p_request);
		if (!info.fixed_total) {
			info.total++;
		}
		return true;
	}
	return enqueue_request_internal(p_request);
}

bool DSContext::enqueue_request_internal(Ref<DSRequest> p_request) {
	return enqueue_request_impl(p_request, false);
}

bool DSContext::enqueue_stream_request_internal(Ref<DSRequest> p_request) {
	return enqueue_request_impl(p_request, true);
}

// Shared validation + tracking for the sync and stream enqueue paths.
// p_skip_maintenance=true (stream flush) omits the per-request
// collect_completed_results() / finalize+evict maintenance: the flush caller
// hoists both once per batch (see flush_stream_async), turning up to 256
// O(tracked) scans per main-thread frame into one. Report bookkeeping
// (including report_order) is identical for both paths.
bool DSContext::enqueue_request_impl(Ref<DSRequest> p_request, bool p_skip_maintenance) {
	if (p_request.is_null()) {
		return false;
	}

	if (shut_down) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("DirectStorage Context is shut down.");
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

	bool needs_sync = false;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		needs_sync = gpu_destination && !gpu_requests_pending;
	}
	if (needs_sync && !synchronize_rendering_device("DirectStorage GPU request")) {
		String error_message = "GPU destination request rejected: RenderingDevice could not be synchronized.";
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message(error_message);
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->report_error(error_message);
		}
		return false;
	}

	// Sync path harvests inline so batch slots free immediately; the stream
	// flush hoists this once per batch instead (see flush_stream_async).
	if (!p_skip_maintenance && has_pending_results()) {
		collect_completed_results();
	}

	bool enqueued = active_backend->enqueue_request(p_request);
	if (enqueued) {
		std::lock_guard<std::mutex> lock(context_mutex);
		if (gpu_destination) {
			gpu_requests_pending = true;
		}
		pending_request_count++;
		Dictionary report;
		report["actual_destination_type"] = static_cast<int>(p_request->get_destination_type());
		report["actual_backend"] = active_backend->get_name();
		report["was_gpu_decompression_used"] = false;
		report["gpu_decompression_pending"] = p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE &&
				active_backend->get_name() == "Win32DirectStorage";
		int64_t request_id = static_cast<int64_t>(p_request->get_instance_id());
		remove_report_locked(request_id);
		request_execution_reports[request_id] = report;
		tracked_requests[request_id] = p_request;
		pending_request_ids.insert(request_id);
		report_order.push_back(request_id);
		// Per-request finalize+evict only on the sync path; the stream flush
		// runs both once after submit (see flush_stream_async).
		if (!p_skip_maintenance) {
			finalize_completed_reports_locked();
			evict_old_reports_if_needed_locked();
		}
	}
	return enqueued;
}

int DSContext::get_pending_request_count() const {
	std::lock_guard<std::mutex> lock(context_mutex);
	return pending_request_count;
}

void DSContext::flush_batch() {
	if (!initialized || !active_backend) {
		return;
	}
	collect_completed_results();
	bool had_gpu_requests = false;
	int submitted_request_count = 0;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		had_gpu_requests = gpu_requests_pending;
		submitted_request_count = pending_request_count;
	}
	active_backend->submit_batch();
	uint64_t backend_fence = 0;
#ifdef _WIN32
	if (active_backend->get_name() == "Win32DirectStorage") {
		backend_fence = ds_win32_get_completion_fence(active_backend);
	}
#endif
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		pending_request_ids.clear();
		pending_request_count = 0;
		if (submitted_request_count > 0) {
			completion_fence = backend_fence != 0 ? backend_fence : completion_fence + 1;
		}
	}
	if (had_gpu_requests) {
		std::lock_guard<std::mutex> lock(context_mutex);
		gpu_requests_pending = false;
	}

	{
		std::lock_guard<std::mutex> lock(context_mutex);
		finalize_completed_reports_locked();
		evict_old_reports_if_needed_locked();
	}
}

Error DSContext::flush_batch_async() {
	if (!initialized || !active_backend) {
		return ERR_UNCONFIGURED;
	}
	int submitted_request_count = 0;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		submitted_request_count = pending_request_count;
	}
	if (submitted_request_count == 0) {
		return OK;
	}
	if (!active_backend->is_batch_complete()) {
		// All batch slots are occupied: the caller must wait/poll
		// (is_batch_complete / batch_completed) before flushing again.
		// Non-silent busy feedback instead of silently dropping the batch.
		return ERR_BUSY;
	}
	bool is_win32_backend = active_backend->get_name() == "Win32DirectStorage";
	active_backend->submit_async();
	uint64_t backend_fence = 0;
#ifdef _WIN32
	if (is_win32_backend) {
		backend_fence = ds_win32_get_completion_fence(active_backend);
	}
#endif
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		if (!is_win32_backend && submitted_request_count > 0) {
			pending_batch_request_counts.push_back(submitted_request_count);
		}
		if (is_win32_backend && submitted_request_count > 0) {
			async_batches_submitted_cache++;
		}
		pending_request_count = 0;
		if (submitted_request_count > 0) {
			completion_fence = backend_fence != 0 ? backend_fence : completion_fence + 1;
		}
		pending_request_ids.clear();
		finalize_completed_reports_locked();
		evict_old_reports_if_needed_locked();
	}
	return OK;
}

Error DSContext::flush_stream_async() {
	// Batch-manager-only entry point for the async prewarm queue.
	// Never routes through sync flush_batch(): sync pending counts, pending id
	// sets and gpu flags are untouched. Drains stream_queue into the backend
	// via the stream internal enqueue (validation + tracking + Ref holding;
	// per-request collect/finalize hoisted to once per flush),
	// then submits non-blocking via submit_async().
	if (!initialized || !active_backend) {
		return ERR_UNCONFIGURED;
	}
	std::deque<Ref<DSRequest>> to_submit;
	{
		std::lock_guard<std::mutex> lock(stream_mutex);
		if (stream_queue.empty()) {
			return OK;
		}
	}
	// Harvest finished batches once (outside stream_mutex: collect takes
	// context_mutex + backend locks, and the established order is
	// context -> stream, never reversed) before checking slot state, so a
	// just-completed batch frees slots immediately instead of forcing an
	// ERR_BUSY retry next frame (one wasted main-thread frame).
	collect_completed_results();
	{
		std::lock_guard<std::mutex> lock(stream_mutex);
		if (stream_queue.empty()) {
			return OK;
		}
		if (!active_backend->is_batch_complete()) {
			// All batch slots occupied: keep the queue intact so the batch
			// manager can retry next frame instead of losing requests.
			return ERR_BUSY;
		}
		uint16_t cap = get_queue_capacity();
		size_t max_batch_requests = (cap > 64) ? (cap / 2) : 64;
		if (max_batch_requests > 256) {
			max_batch_requests = 256;
		}
		size_t count_to_pop = std::min(stream_queue.size(), max_batch_requests);
		for (size_t i = 0; i < count_to_pop; ++i) {
			to_submit.push_back(stream_queue.front());
			stream_queue.pop_front();
		}
	}
	int submitted_request_count = 0;
	for (std::deque<Ref<DSRequest>>::iterator it = to_submit.begin(); it != to_submit.end(); ++it) {
		Ref<DSRequest> request = *it;
		if (!request.is_valid()) {
			continue;
		}
		if (request->get_status() == DSRequest::STATUS_FAILED) {
			// Cancelled while queued (terminal already counted by cancel_prewarm).
			continue;
		}
		// Stream fast path: per-request collect/finalize hoisted out of this
		// main-thread loop (see enqueue_stream_request_internal); tag-reject
		// accounting stays inline so progress cannot stall.
		if (enqueue_stream_request_internal(request)) {
			submitted_request_count++;
		} else {
			// Backend rejected: request is terminal (FAILED); count it so tag
			// progress cannot stall waiting for a request that will never run.
			count_stream_request_terminal(request);
		}
	}
	if (submitted_request_count == 0) {
		return OK;
	}
	bool is_win32_backend = active_backend->get_name() == "Win32DirectStorage";
	active_backend->submit_async();
	uint64_t backend_fence = 0;
#ifdef _WIN32
	if (is_win32_backend) {
		backend_fence = ds_win32_get_completion_fence(active_backend);
	}
#endif
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		if (!is_win32_backend && submitted_request_count > 0) {
			pending_batch_request_counts.push_back(submitted_request_count);
		}
		if (is_win32_backend && submitted_request_count > 0) {
			async_batches_submitted_cache++;
		}
		if (submitted_request_count > 0) {
			completion_fence = backend_fence != 0 ? backend_fence : completion_fence + 1;
		}
		// Stream-only: sync pending counts/ids belong to flush_batch paths.
		finalize_completed_reports_locked();
		evict_old_reports_if_needed_locked();
	}
	return OK;
}

bool DSContext::has_stream_pending() const {
	std::lock_guard<std::mutex> lock(stream_mutex);
	return !stream_queue.empty();
}

int DSContext::get_stream_pending_count() const {
	std::lock_guard<std::mutex> lock(stream_mutex);
	return static_cast<int>(stream_queue.size());
}

uint64_t DSContext::prewarm_async(const PackedStringArray &p_paths) {
	// Tag issuance: allocates a fresh tag and registers the expected request
	// total for progress queries. Path remapping, dedup and actual DSRequest
	// creation/enqueue belong to the preloader, which fills this tag via
	// enqueue_request(req, true) with a matching cancellation_tag.
	std::lock_guard<std::mutex> lock(stream_mutex);
	uint64_t tag = next_stream_tag++;
	if (next_stream_tag == 0) {
		next_stream_tag = 1; // wrap guard: tag 0 stays reserved for untagged
	}
	StreamTagInfo info;
	info.tag = tag;
	info.total = p_paths.size();
	stream_tags[tag] = info;
	return tag;
}

bool DSContext::is_prewarm_complete(uint64_t p_tag) const {
	std::lock_guard<std::mutex> lock(stream_mutex);
	std::unordered_map<uint64_t, StreamTagInfo>::const_iterator it = stream_tags.find(p_tag);
	if (it == stream_tags.end()) {
		return true; // unknown tag: nothing pending
	}
	const StreamTagInfo &info = it->second;
	if (info.cancel_requested) {
		for (std::deque<Ref<DSRequest>>::const_iterator qit = stream_queue.begin(); qit != stream_queue.end(); ++qit) {
			if (qit->is_valid() && (*qit)->get_cancellation_tag() == p_tag) {
				return false;
			}
		}
		return true;
	}
	if (info.completed < info.total) {
		return false;
	}
	for (std::deque<Ref<DSRequest>>::const_iterator qit = stream_queue.begin(); qit != stream_queue.end(); ++qit) {
		if (qit->is_valid() && (*qit)->get_cancellation_tag() == p_tag) {
			return false;
		}
	}
	return true;
}

Vector2i DSContext::get_prewarm_progress(uint64_t p_tag) const {
	std::lock_guard<std::mutex> lock(stream_mutex);
	std::unordered_map<uint64_t, StreamTagInfo>::const_iterator it = stream_tags.find(p_tag);
	if (it == stream_tags.end()) {
		return Vector2i(0, 0);
	}
	int done = MIN(it->second.completed, it->second.total);
	return Vector2i(done, it->second.total);
}

void DSContext::set_prewarm_total(uint64_t p_tag, int p_total) {
	std::lock_guard<std::mutex> lock(stream_mutex);
	StreamTagInfo &info = stream_tags[p_tag];
	info.tag = p_tag;
	info.total = MAX(0, p_total);
	info.fixed_total = true;
}

void DSContext::count_stream_terminal_failure(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(stream_mutex);
	std::unordered_map<uint64_t, StreamTagInfo>::iterator it = stream_tags.find(p_tag);
	if (it != stream_tags.end()) {
		it->second.completed++;
	}
}

void DSContext::cancel_prewarm(uint64_t p_tag) {
	// Cancel semantics: DSRequest has no cancelled status value, so cancelled
	// requests are marked finished FAILED with a "cancelled" message.
	// Completion threads treat non-pending requests as dropped: buffers are
	// released and freed RIDs are never written. Refs stay held
	// (stream_queue until drained here, tracked_requests until finalize)
	// so there is no use-after-free.
	const String cancel_message = "Prewarm cancelled (tag " + String::num_uint64(p_tag) + ").";
	{
		std::lock_guard<std::mutex> lock(stream_mutex);
		StreamTagInfo &info = stream_tags[p_tag];
		info.tag = p_tag;
		info.cancel_requested = true;
		std::deque<Ref<DSRequest>> remaining;
		for (std::deque<Ref<DSRequest>>::iterator it = stream_queue.begin(); it != stream_queue.end(); ++it) {
			Ref<DSRequest> request = *it;
			if (request.is_valid() && request->get_cancellation_tag() == p_tag &&
					request->get_status() != DSRequest::STATUS_COMPLETED &&
					request->get_status() != DSRequest::STATUS_FAILED) {
				request->set_status(DSRequest::STATUS_FAILED);
				request->set_error_message(cancel_message);
				info.cancelled++;
				info.completed++;
			} else {
				remaining.push_back(request);
			}
		}
		stream_queue.swap(remaining);
	}
	if (!initialized || !active_backend) {
		return;
	}
	active_backend->cancel_requests_with_tag(p_tag);
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		for (std::unordered_map<int64_t, Ref<DSRequest>>::iterator it = tracked_requests.begin(); it != tracked_requests.end(); ++it) {
			Ref<DSRequest> request = it->second;
			if (request.is_valid() && request->get_cancellation_tag() == p_tag) {
				DSRequest::Status status = request->get_status();
				if (status != DSRequest::STATUS_COMPLETED && status != DSRequest::STATUS_FAILED) {
					request->set_status(DSRequest::STATUS_FAILED);
					request->set_error_message(cancel_message);
				}
			}
		}
		finalize_completed_reports_locked(); // releases terminal Refs; hook bumps per-tag completed
		evict_old_reports_if_needed_locked();
	}
}

bool DSContext::is_tag_cancelled(uint64_t p_tag) const {
	if (p_tag == 0) {
		return false;
	}
	std::lock_guard<std::mutex> lock(stream_mutex);
	std::unordered_map<uint64_t, StreamTagInfo>::const_iterator it = stream_tags.find(p_tag);
	return it != stream_tags.end() && it->second.cancel_requested;
}

bool DSContext::is_batch_complete() const {
	if (!active_backend) {
		return true;
	}
#ifdef _WIN32
	if (active_backend->get_name() == "Win32DirectStorage") {
		return active_backend->is_batch_complete();
	}
#endif
	// For fallback backend, completion is determined by backend state; guard pending counts.
	std::lock_guard<std::mutex> lock(context_mutex);
	return active_backend->is_batch_complete();
}

bool DSContext::has_pending_results() const {
	if (!initialized || !active_backend) {
		return false;
	}
#ifdef _WIN32
	if (active_backend->get_name() == "Win32DirectStorage") {
		return static_cast<Win32Backend *>(active_backend)->has_pending_results();
	}
#endif
	std::lock_guard<std::mutex> lock(context_mutex);
	return active_backend->is_batch_complete() && !pending_batch_request_counts.empty();
}

void DSContext::collect_completed_results() {
	if (!initialized || !active_backend) {
		return;
	}

	#ifdef _WIN32
	if (active_backend->get_name() == "Win32DirectStorage") {
		Win32Backend *backend = static_cast<Win32Backend *>(active_backend);
		if (backend->has_pending_results()) {
			bool has_gpu_pending = false;
			{
				std::lock_guard<std::mutex> lock(context_mutex);
				has_gpu_pending = gpu_requests_pending;
			}
			if (has_gpu_pending && !synchronize_rendering_device("DirectStorage async completion")) {
				return;
			}
			uint64_t fence_value = 0;
			int completed_count = 0;
			int error_code = 0;
			String error_message;
			while (backend->collect_completed_batch(fence_value, completed_count, error_code, error_message)) {
				if (error_code != 0) {
					call_deferred("emit_signal", "batch_failed", error_code, error_message);
				} else {
					call_deferred("emit_signal", "batch_completed", static_cast<int64_t>(fence_value), completed_count);
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(context_mutex);
			finalize_completed_reports_locked();
			evict_old_reports_if_needed_locked();
			if (gpu_requests_pending && backend->is_batch_complete()) {
				gpu_requests_pending = false;
			}
		}
		return;
	}
	#endif

	{
		std::lock_guard<std::mutex> lock(context_mutex);
		if (!active_backend->is_batch_complete() || pending_batch_request_counts.empty()) {
			return;
		}
	}
	active_backend->collect_completed_results();
	int completed_count = 0;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		if (pending_batch_request_counts.empty()) {
			return;
		}
		int submitted_count = pending_batch_request_counts.front();
		pending_batch_request_counts.pop_front();
		// Report actual succeeded requests, not merely submitted ones:
		// failed requests carry STATUS_FAILED and must not inflate the count.
		int succeeded_count = 0;
		for (std::unordered_map<int64_t, Ref<DSRequest>>::const_iterator it = tracked_requests.begin(); it != tracked_requests.end(); ++it) {
			if (it->second.is_valid() && it->second->get_status() == DSRequest::STATUS_COMPLETED) {
				succeeded_count++;
			}
		}
		completed_count = succeeded_count <= submitted_count ? succeeded_count : submitted_count;
		finalize_completed_reports_locked();
		evict_old_reports_if_needed_locked();
	}
	bool has_gpu_pending = false;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		has_gpu_pending = gpu_requests_pending;
	}
	if (has_gpu_pending) {
		if (!synchronize_rendering_device("DirectStorage async completion")) {
			return;
		}
		std::lock_guard<std::mutex> lock(context_mutex);
		gpu_requests_pending = false;
	}
	call_deferred("emit_signal", "batch_completed", static_cast<int64_t>(get_completion_fence()), completed_count);
}

void DSContext::cancel_requests_with_tag(uint64_t p_tag) {
	if (!initialized || !active_backend) {
		return;
	}
	std::unordered_set<int64_t> cancelled_pending_request_ids;
	{
		std::lock_guard<std::mutex> lock(context_mutex);
		for (std::unordered_set<int64_t>::const_iterator it = pending_request_ids.begin(); it != pending_request_ids.end(); ++it) {
			std::unordered_map<int64_t, Ref<DSRequest>>::const_iterator request_it = tracked_requests.find(*it);
			if (request_it != tracked_requests.end() && request_it->second.is_valid() &&
					request_it->second->get_cancellation_tag() == p_tag) {
				cancelled_pending_request_ids.insert(*it);
			}
		}
	}
	active_backend->cancel_requests_with_tag(p_tag);
	std::lock_guard<std::mutex> lock(context_mutex);
	for (std::unordered_set<int64_t>::const_iterator it = cancelled_pending_request_ids.begin(); it != cancelled_pending_request_ids.end(); ++it) {
		pending_request_ids.erase(*it);
		// Mark finished (FAILED + message) so completion threads drop buffers
		// and never write freed RIDs. There is no cancelled status on
		// DSRequest; FAILED is the finished-dropped state. Refs stay held
		// in tracked_requests until finalize below: no use-after-free.
		std::unordered_map<int64_t, Ref<DSRequest>>::const_iterator request_it = tracked_requests.find(*it);
		if (request_it != tracked_requests.end() && request_it->second.is_valid()) {
			DSRequest::Status status = request_it->second->get_status();
			if (status != DSRequest::STATUS_COMPLETED && status != DSRequest::STATUS_FAILED) {
				request_it->second->set_status(DSRequest::STATUS_FAILED);
				request_it->second->set_error_message("Request cancelled (tag " + String::num_uint64(p_tag) + ").");
			}
		}
	}
	if (cancelled_pending_request_ids.size() >= static_cast<size_t>(pending_request_count)) {
		pending_request_count = 0;
	} else {
		pending_request_count -= static_cast<int>(cancelled_pending_request_ids.size());
	}
	if (!pending_batch_request_counts.empty()) {
		pending_batch_request_counts.clear();
	}
	finalize_completed_reports_locked();
	evict_old_reports_if_needed_locked();
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
	ClassDB::bind_method(D_METHOD("get_init_status"), &DSContext::get_init_status);
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

	ClassDB::bind_method(D_METHOD("enqueue_request", "request", "from_stream"), &DSContext::enqueue_request, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_pending_request_count"), &DSContext::get_pending_request_count);
	ClassDB::bind_method(D_METHOD("flush_batch"), &DSContext::flush_batch);
	ClassDB::bind_method(D_METHOD("flush_batch_async"), &DSContext::flush_batch_async);
	ClassDB::bind_method(D_METHOD("flush_stream_async"), &DSContext::flush_stream_async);
	ClassDB::bind_method(D_METHOD("has_stream_pending"), &DSContext::has_stream_pending);
	ClassDB::bind_method(D_METHOD("get_stream_pending_count"), &DSContext::get_stream_pending_count);
	ClassDB::bind_method(D_METHOD("prewarm_async", "paths"), &DSContext::prewarm_async);
	ClassDB::bind_method(D_METHOD("is_prewarm_complete", "tag"), &DSContext::is_prewarm_complete);
	ClassDB::bind_method(D_METHOD("get_prewarm_progress", "tag"), &DSContext::get_prewarm_progress);
	ClassDB::bind_method(D_METHOD("set_prewarm_total", "tag", "total"), &DSContext::set_prewarm_total);
	ClassDB::bind_method(D_METHOD("count_stream_terminal_failure", "tag"), &DSContext::count_stream_terminal_failure);
	ClassDB::bind_method(D_METHOD("cancel_prewarm", "tag"), &DSContext::cancel_prewarm);
	ClassDB::bind_method(D_METHOD("is_tag_cancelled", "tag"), &DSContext::is_tag_cancelled);
	ClassDB::bind_method(D_METHOD("is_batch_complete"), &DSContext::is_batch_complete);
	ClassDB::bind_method(D_METHOD("has_pending_results"), &DSContext::has_pending_results);
	ClassDB::bind_method(D_METHOD("collect_completed_results"), &DSContext::collect_completed_results);
	ClassDB::bind_method(D_METHOD("get_completion_fence"), &DSContext::get_completion_fence);
	ClassDB::bind_method(D_METHOD("wait_for_completion", "timeout_msec"), &DSContext::wait_for_completion);
	ClassDB::bind_method(D_METHOD("cancel_requests_with_tag", "tag"), &DSContext::cancel_requests_with_tag);
	ClassDB::bind_method(D_METHOD("get_async_batches_submitted"), &DSContext::get_async_batches_submitted);
	ClassDB::bind_method(D_METHOD("get_in_flight_async_batch_count"), &DSContext::get_in_flight_async_batch_count);
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

	BIND_ENUM_CONSTANT(INIT_STATUS_UNINITIALIZED);
	BIND_ENUM_CONSTANT(INIT_STATUS_DIRECTSTORAGE_D3D12);
	BIND_ENUM_CONSTANT(INIT_STATUS_SOFTWARE_FALLBACK);
	BIND_ENUM_CONSTANT(INIT_STATUS_FAILED);

	ADD_SIGNAL(MethodInfo("batch_completed",
		PropertyInfo(Variant::INT, "fence_value"),
		PropertyInfo(Variant::INT, "completed_count")));
	ADD_SIGNAL(MethodInfo("batch_failed",
		PropertyInfo(Variant::INT, "error_code"),
		PropertyInfo(Variant::STRING, "error_message")));
}

} // namespace godot
