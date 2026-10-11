#include "ds_win32_backend.h"
#include "../../core/ds_context.h"
#include "../../core/ds_error_handler.h"
#include "../../core/ds_profiler.h"
#include "../../formats/dstex_format.h"
#include "../../formats/dspkg_format.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <winioctl.h>
#include <dxgi.h>
#endif

#include <cstdio>
#include <string>
#include <chrono>
#include <cstring>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <utility>
#include <algorithm>

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

static const uint64_t GDEFLATE_PAGE_SIZE = 64ULL * 1024ULL;

struct DSChunkBoundary {
	uint64_t source_offset = 0;
	uint64_t source_size = 0;
	uint64_t destination_offset = 0;
	uint64_t destination_size = 0;
};

// Cached DSPkg for the active .dspkg archive path. get_texture_mip_entries()
// runs on every texture enqueue; reopening/reparsing the archive per enqueue
// costs disk I/O plus full entry-table validation. The cache holds the last
// successfully opened package keyed by resolved path. A path switch frees the
// old package and opens the new one; an open failure clears the cache and
// returns empty (no crash). All access happens under Win32Backend::batch_mutex
// (enqueue_request holds it across the get_texture_mip_entries call), so no
// additional locking is needed here.
static struct DSPkgCache {
	String path;
	Ref<DSPkg> package;
} *s_dspkg_cache = nullptr;

static bool get_dstex_mip_chunks(const String &p_path, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t p_capacity, Vector<DSChunkBoundary> &r_chunks) {
	if (p_path.get_extension().to_lower() != "dstex") {
		return false;
	}

	Ref<DSTex> dstex;
	dstex.instantiate();
	Dictionary header = dstex->read_header(p_path);
	if (header.is_empty() || !header.has("mip_count")) {
		return false;
	}
	uint32_t mip_count = header["mip_count"];
	if (mip_count == 0) {
		return false;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null()) {
		return false;
	}
	file->seek(sizeof(DSTexHeader));
	PackedByteArray raw_entries = file->get_buffer(static_cast<int64_t>(sizeof(DSTexMipEntryV2) * mip_count));
	if (raw_entries.size() != static_cast<int64_t>(sizeof(DSTexMipEntryV2) * mip_count)) {
		return false;
	}

	uint64_t destination_offset = 0;
	for (uint32_t mip_index = 0; mip_index < mip_count; mip_index++) {
		DSTexMipEntryV2 mip_entry = {};
		const uint8_t *entry_data = raw_entries.ptr() + sizeof(DSTexMipEntryV2) * mip_index;
		std::memcpy(&mip_entry, entry_data, sizeof(DSTexMipEntryV2));
		uint64_t source_offset = mip_entry.offset;
		uint64_t source_size = mip_entry.compressed_size;
		uint64_t destination_size = mip_entry.uncompressed_size;
		bool relative_range = source_offset + source_size <= p_payload_size;
		bool absolute_range = source_offset >= p_payload_offset && source_offset + source_size <= p_payload_offset + p_payload_size;
		if (!relative_range && absolute_range) {
			source_offset -= p_payload_offset;
		}
		if (source_size == 0 || destination_size == 0 || source_offset + source_size > p_payload_size || destination_size > p_capacity) {
			r_chunks.clear();
			return false;
		}

		DSChunkBoundary boundary;
		boundary.source_offset = source_offset;
		boundary.source_size = source_size;
		boundary.destination_offset = destination_offset;
		boundary.destination_size = destination_size;
		// Never merge consecutive mips: each mip is an independent GDeflate TileStream.
		r_chunks.push_back(boundary);
		destination_offset += destination_size;
	}
	return !r_chunks.is_empty();
}

static bool get_texture_mip_entries(const String &p_path, uint64_t p_offset, Vector<DSTexMipEntryV2> &r_entries) {
	r_entries.clear();
	String resolved_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (resolved_path.begins_with("res://") || resolved_path.begins_with("user://"))) {
		resolved_path = ps->globalize_path(resolved_path);
	}
	String ext = resolved_path.get_extension().to_lower();
	if (ext == "dstex") {
		Ref<FileAccess> file = FileAccess::open(resolved_path, FileAccess::READ);
		if (file.is_null() || file->get_length() < sizeof(DSTexHeader)) {
			return false;
		}

		PackedByteArray header_data = file->get_buffer(sizeof(DSTexHeader));
		if (header_data.size() != static_cast<int64_t>(sizeof(DSTexHeader))) {
			return false;
		}
		DSTexHeader header = {};
		std::memcpy(&header, header_data.ptr(), sizeof(header));
		if (header.magic != 0x58455444 || header.version != 2 || header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT ||
				header.compressed_size == 0 || header.original_size == 0 || header.reserved != 0 ||
				!dstex_sizes_are_sane(header.width, header.height, header.original_size, header.compressed_size) ||
				header.mip_count > (std::numeric_limits<uint64_t>::max() - sizeof(DSTexHeader)) / sizeof(DSTexMipEntryV2)) {
			return false;
		}

		uint64_t entries_end = sizeof(DSTexHeader) + static_cast<uint64_t>(header.mip_count) * sizeof(DSTexMipEntryV2);
		uint64_t file_length = static_cast<uint64_t>(file->get_length());
		if (entries_end > file_length || header.compressed_size > file_length - entries_end) {
			return false;
		}
		r_entries.resize(header.mip_count);
		uint64_t compressed_size = 0;
		uint64_t uncompressed_size = 0;
		uint64_t previous_end = entries_end;
		for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
			PackedByteArray entry_data = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (entry_data.size() != static_cast<int64_t>(sizeof(DSTexMipEntryV2))) {
				r_entries.clear();
				return false;
			}
			std::memcpy(&r_entries.write[mip_index], entry_data.ptr(), sizeof(DSTexMipEntryV2));
			const DSTexMipEntryV2 &entry = r_entries[mip_index];
			uint64_t entry_end = 0;
			if (entry.offset < previous_end || entry.offset > file_length || entry.compressed_size == 0 ||
					entry.compressed_size > file_length - entry.offset || entry.offset > UINT64_MAX - entry.compressed_size ||
					entry.reserved != 0 || entry.uncompressed_size == 0 || entry.row_pitch == 0 || entry.row_pitch % 256 != 0 ||
					compressed_size > UINT64_MAX - entry.compressed_size || uncompressed_size > UINT64_MAX - entry.uncompressed_size) {
				r_entries.clear();
				return false;
			}
			entry_end = entry.offset + entry.compressed_size;
			compressed_size += entry.compressed_size;
			uncompressed_size += entry.uncompressed_size;
			previous_end = entry_end;
		}
		if (compressed_size != header.compressed_size || uncompressed_size != header.original_size) {
			r_entries.clear();
			return false;
		}
		return true;
	}

	if (ext == "dspkg") {
		// Reuse the cached package for the active archive path instead of
		// reopening/reparsing the .dspkg on every enqueue. Path switch drops
		// the old package; open failure clears the cache and returns false.
		if (!s_dspkg_cache) {
			s_dspkg_cache = memnew(DSPkgCache);
		}
		if (s_dspkg_cache->package.is_null() || s_dspkg_cache->path != resolved_path) {
			Ref<DSPkg> fresh;
			fresh.instantiate();
			if (fresh.is_null() || fresh->open(resolved_path) != OK) {
				s_dspkg_cache->package.unref();
				s_dspkg_cache->path = String();
				return false;
			}
			s_dspkg_cache->package = fresh;
			s_dspkg_cache->path = resolved_path;
		}
		// O(1) direct lookup by absolute payload offset
		// (header.data_offset + entry.data_offset). Miss (false) means a
		// legacy entry without GPU mip metadata or an unknown offset — fall
		// back to the safe error path in enqueue_request, no crash.
		Vector<DSPkgMipEntry> package_entries;
		if (!s_dspkg_cache->package->get_mip_entries_for_offset(p_offset, package_entries) || package_entries.is_empty()) {
			return false;
		}
		r_entries.resize(package_entries.size());
		for (int mip_idx = 0; mip_idx < package_entries.size(); mip_idx++) {
			const DSPkgMipEntry &package_entry = package_entries[mip_idx];
			r_entries.write[mip_idx].offset = package_entry.offset;
			r_entries.write[mip_idx].compressed_size = package_entry.compressed_size;
			r_entries.write[mip_idx].uncompressed_size = package_entry.uncompressed_size;
			r_entries.write[mip_idx].row_pitch = package_entry.row_pitch;
			r_entries.write[mip_idx].reserved = package_entry.reserved;
		}
		return true;
	}

	return false;
}

struct DSChunkedRequestState {
	Ref<DSRequest> parent;
	Vector<Ref<DSRequest>> children;
	Vector<uint64_t> destination_offsets;
};

static std::unordered_map<DSRequest *, DSChunkedRequestState> s_chunked_requests;
static std::unordered_map<DSRequest *, DSRequest *> s_chunk_parents;
static std::unordered_map<DSRequest *, ID3D12Resource *> s_resource_guards;
static std::unordered_set<std::string> s_staging_buffer_warnings;

static String retrieve_queue_error(IDStorageQueue1 *p_queue) {
	if (!p_queue) {
		return String();
	}

	DSTORAGE_ERROR_RECORD error_record = {};
	p_queue->RetrieveErrorRecord(&error_record);
	if (error_record.FailureCount == 0) {
		return String();
	}

	const DSTORAGE_ERROR_FIRST_FAILURE &failure = error_record.FirstFailure;
	String hresult_desc = DSErrorHandler::describe_hresult(static_cast<uint32_t>(failure.HResult));
	if (hresult_desc.is_empty()) {
		hresult_desc = "No system message is available for this DirectStorage HRESULT.";
	}

	String command_type_str = DSErrorHandler::describe_command_type(static_cast<int32_t>(failure.CommandType));

	String message = "DirectStorage queue error (HRESULT 0x" +
			String::num_uint64(static_cast<uint32_t>(failure.HResult), 16) +
			", command type " + command_type_str + "): " +
			hresult_desc;

	if (failure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST) {
		message += " [File: " + String(failure.Request.Filename) + ", RequestName: " + String(failure.Request.RequestName) + "]";
	}

#ifdef _WIN32
	const HRESULT hr_aborted = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
	if (failure.HResult == hr_aborted) {
		// Cancellation via CancelRequestsWithTag() is an intentional application event,
		// not a fatal failure. Suppress push_error/report_error and log verbosely.
		UtilityFunctions::print_verbose("[DirectStorage] Request aborted on queue: " + message);
		return String();
	}
#endif

	UtilityFunctions::push_error(message);
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->report_error(message);
	}
	return message;
}

static void release_resource_guards(const Vector<Ref<DSRequest>> &p_requests) {
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_null()) {
			continue;
		}
		std::unordered_map<DSRequest *, ID3D12Resource *>::iterator it = s_resource_guards.find(p_requests[i].ptr());
		if (it != s_resource_guards.end()) {
			if (it->second) {
				it->second->Release();
			}
			s_resource_guards.erase(it);
		}
	}
}

static void release_all_resource_guards() {
	for (std::unordered_map<DSRequest *, ID3D12Resource *>::iterator it = s_resource_guards.begin(); it != s_resource_guards.end(); ++it) {
		if (it->second) {
			it->second->Release();
		}
	}
	s_resource_guards.clear();
}

// Render-thread-affine guard re-query used by validate_resource_guard().
// Returns true and fills r_resource when the query runs against a live
// RenderingDevice inline on the render thread. When called off the render
// thread (e.g. completion thread under batch_mutex), immediately returns false
// without blocking, allowing validate_resource_guard() to safely fall back to
// the AddRef'd guard resource without stalling frames or risking deadlocks.
static bool ds_win32_query_guard_resource_on_render_thread(bool p_is_buffer_request, const RID &p_destination_rid, ID3D12Resource *&r_resource) {
	r_resource = nullptr;
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs) {
		return false;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (!rd) {
			return false;
		}
		auto resource_type = p_is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		r_resource = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(resource_type, p_destination_rid, 0));
		return true;
	}
	return false;
}


// Render-thread destination query context for enqueue_request().
// Marshals RenderingDevice::get_driver_resource() and ID3D12Resource::GetDesc()
// to the render thread when called from main/worker threads.
// Managed via std::shared_ptr so that if the calling thread times out after 250ms,
// any late-arriving render-thread callback safely references a live heap object
// rather than a destroyed stack frame (HIGH-01).
struct DSGPUDestinationContext {
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
	bool is_buffer_request = false;
	RID destination_rid;
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	uint64_t native_format = 0;
	bool rd_available = false;
};
static std::shared_ptr<DSGPUDestinationContext> s_gpu_dest_query_ctx;
static std::mutex s_dest_query_mutex;

static void ds_win32_fetch_destination_resource_on_render_thread() {
	std::shared_ptr<DSGPUDestinationContext> ctx;
	{
		std::lock_guard<std::mutex> query_lock(s_dest_query_mutex);
		ctx = std::move(s_gpu_dest_query_ctx);
	}
	if (!ctx) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	uint64_t native_format = 0;
	bool rd_available = false;
	if (rd) {
		rd_available = true;
		auto resource_type = ctx->is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		uint64_t handle = rd->get_driver_resource(resource_type, ctx->destination_rid, 0);
		resource = reinterpret_cast<ID3D12Resource *>(handle);
		if (resource) {
			desc = resource->GetDesc();
			if (!ctx->is_buffer_request) {
				native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, ctx->destination_rid, 0);
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->resource = resource;
		ctx->desc = desc;
		ctx->native_format = native_format;
		ctx->rd_available = rd_available;
		ctx->completed = true;
		ctx->cv.notify_one(); // HIGH-01: Notify while holding mutex
	}
}

static bool ds_win32_query_destination_resource_on_render_thread(
		bool p_is_buffer_request,
		const RID &p_destination_rid,
		ID3D12Resource *&r_resource,
		D3D12_RESOURCE_DESC &r_desc,
		uint64_t &r_native_format,
		bool &r_rd_available) {
	r_resource = nullptr;
	r_desc = {};
	r_native_format = 0;
	r_rd_available = false;

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return false;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (!rd) {
			return false;
		}
		r_rd_available = true;
		auto resource_type = p_is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		uint64_t handle = rd->get_driver_resource(resource_type, p_destination_rid, 0);
		r_resource = reinterpret_cast<ID3D12Resource *>(handle);
		if (r_resource) {
			r_desc = r_resource->GetDesc();
			if (!p_is_buffer_request) {
				r_native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_destination_rid, 0);
			}
		}
		return true;
	}

	auto ctx = std::make_shared<DSGPUDestinationContext>();
	ctx->is_buffer_request = p_is_buffer_request;
	ctx->destination_rid = p_destination_rid;

	{
		std::lock_guard<std::mutex> query_lock(s_dest_query_mutex);
		s_gpu_dest_query_ctx = ctx;
	}
	rs->call_on_render_thread(callable_mp_static(ds_win32_fetch_destination_resource_on_render_thread));

	{
		std::unique_lock<std::mutex> lock(ctx->mutex);
		constexpr std::chrono::milliseconds kDestQueryTimeout(250);
		bool signaled = ctx->cv.wait_for(lock, kDestQueryTimeout, [&ctx] { return ctx->completed; });
		if (!signaled) {
			// HIGH-02: Bounded timeout with no secondary unbounded wait. Clear handoff slot if still pending.
			std::lock_guard<std::mutex> query_lock(s_dest_query_mutex);
			if (s_gpu_dest_query_ctx == ctx) {
				s_gpu_dest_query_ctx.reset();
			}
			return false;
		}
		r_rd_available = ctx->rd_available;
		r_resource = ctx->resource;
		r_desc = ctx->desc;
		r_native_format = ctx->native_format;
		return true;
	}
}

static bool validate_resource_guard(const Ref<DSRequest> &p_request, String &r_error, std::unordered_map<int64_t, ID3D12Resource *> *p_rid_cache = nullptr) {
	if (p_request.is_null() || (p_request->get_destination_type() != DSRequest::DESTINATION_GPU_BUFFER &&
			p_request->get_destination_type() != DSRequest::DESTINATION_GPU_TEXTURE)) {
		return true;
	}

	std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard = s_resource_guards.find(p_request.ptr());
	RID destination_rid = p_request->get_destination_resource();
	if (guard == s_resource_guards.end() || !guard->second || !destination_rid.is_valid()) {
		r_error = "DirectStorage GPU resource RID was freed before batch completion.";
		return false;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12" || !rd) {
		r_error = "DirectStorage GPU resource could not be validated after batch completion.";
		return false;
	}

	// get_driver_resource() is render-thread-affine but this runs on the
	// completion (or polling) thread, so the re-query is marshalled above.
	// When the render thread cannot answer, fall back to the cached guard
	// only: the AddRef'd ID3D12Resource captured at enqueue time is still
	// alive, so the DirectStorage write landed in the right memory and only
	// the RID-replacement check is skipped as unverifiable.
	bool is_buffer_request = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	ID3D12Resource *current_resource = nullptr;
	bool has_cached = false;
	int64_t rid_id = destination_rid.get_id();
	if (p_rid_cache) {
		std::unordered_map<int64_t, ID3D12Resource *>::const_iterator cache_it = p_rid_cache->find(rid_id);
		if (cache_it != p_rid_cache->end()) {
			current_resource = cache_it->second;
			has_cached = true;
		}
	}
	if (!has_cached) {
		if (!ds_win32_query_guard_resource_on_render_thread(is_buffer_request, destination_rid, current_resource)) {
			if (p_rid_cache) {
				(*p_rid_cache)[rid_id] = guard->second;
			}
			return true;
		}
		if (p_rid_cache) {
			(*p_rid_cache)[rid_id] = current_resource;
		}
	}
	if (!current_resource || current_resource != guard->second) {
		r_error = "DirectStorage GPU resource RID was freed or replaced before batch completion.";
		return false;
	}
	return true;
}

static DSRequest *get_logical_request(DSRequest *p_request) {
	std::unordered_map<DSRequest *, DSRequest *>::const_iterator it = s_chunk_parents.find(p_request);
	return it != s_chunk_parents.end() ? it->second : p_request;
}

static int count_logical_requests(const Vector<Ref<DSRequest>> &p_requests) {
	std::unordered_set<DSRequest *> logical_requests;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid()) {
			logical_requests.insert(get_logical_request(p_requests[i].ptr()));
		}
	}
	return static_cast<int>(logical_requests.size());
}

static int count_completed_requests(const Vector<Ref<DSRequest>> &p_requests) {
	int completed_count = 0;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid() && p_requests[i]->get_status() == DSRequest::STATUS_COMPLETED) {
			completed_count++;
		}
	}
	return completed_count;
}

static void complete_chunked_requests() {
	for (std::unordered_map<DSRequest *, DSChunkedRequestState>::iterator it = s_chunked_requests.begin(); it != s_chunked_requests.end();) {
		DSChunkedRequestState &state = it->second;
		bool all_complete = true;
		bool failed = false;
		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_null()) {
				failed = true;
				continue;
			}
			DSRequest::Status status = state.children[i]->get_status();
			if (status == DSRequest::STATUS_FAILED) {
				failed = true;
			} else if (status != DSRequest::STATUS_COMPLETED) {
				all_complete = false;
			}
		}
		if (!all_complete) {
			++it;
			continue;
		}

		if (failed) {
			state.parent->set_status(DSRequest::STATUS_FAILED);
			for (int i = 0; i < state.children.size(); i++) {
				if (state.children[i].is_valid() && !state.children[i]->get_error_message().is_empty()) {
					state.parent->set_error_message(state.children[i]->get_error_message());
					break;
				}
			}
			if (state.parent->get_error_message().is_empty()) {
				state.parent->set_error_message("DirectStorage GDeflate chunk failed.");
			}
		} else {
			if (state.parent->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE ||
					state.parent->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER) {
				state.parent->set_status(DSRequest::STATUS_COMPLETED);
			} else {
				PackedByteArray &destination = state.parent->get_destination_buffer_ref();
				bool valid_destination = true;
				for (int i = 0; i < state.children.size(); i++) {
					PackedByteArray child_data = state.children[i]->get_destination_buffer();
					uint64_t destination_offset = state.destination_offsets[i];
					if (destination_offset + static_cast<uint64_t>(child_data.size()) > static_cast<uint64_t>(destination.size())) {
						valid_destination = false;
						break;
					}
					if (child_data.size() > 0) {
						std::memcpy(destination.ptrw() + destination_offset, child_data.ptr(), child_data.size());
					}
				}
				if (valid_destination) {
					state.parent->set_status(DSRequest::STATUS_COMPLETED);
				} else {
					state.parent->set_status(DSRequest::STATUS_FAILED);
					state.parent->set_error_message("DirectStorage GDeflate chunk output exceeded the parent destination buffer.");
				}
			}
		}

		bool any_child_gpu_in_flight = false;
		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_valid()) {
				if (state.children[i]->is_gpu_in_flight()) {
					any_child_gpu_in_flight = true;
				}
				s_chunk_parents.erase(state.children[i].ptr());
			}
		}
		if (!any_child_gpu_in_flight && state.parent.is_valid()) {
			state.parent->set_gpu_in_flight(false);
		}
		it = s_chunked_requests.erase(it);
	}
}

uint64_t ds_win32_get_completion_fence(PlatformBackend *p_backend) {
	if (p_backend != s_backend_instance) {
		return 0;
	}
	return static_cast<Win32Backend *>(p_backend)->get_completion_fence();
}

bool ds_win32_wait_for_completion(PlatformBackend *p_backend, uint64_t p_fence_value, uint32_t p_timeout_msec) {
	if (p_backend != s_backend_instance) {
		return false;
	}
	return static_cast<Win32Backend *>(p_backend)->wait_for_completion(p_fence_value, p_timeout_msec);
}
#endif

#ifdef _WIN32
bool Win32Backend::has_in_flight_batches_locked() const {
	if (!completion_queue.empty()) {
		return true;
	}
	for (const QueueState &queue : priority_queues) {
		for (const BatchSlot &slot : queue.batch_slots) {
			if (slot.submitted) {
				return true;
			}
		}
	}
	return false;
}
#endif

bool Win32Backend::is_batch_complete() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	return !has_in_flight_batches_locked();
#else
	return true;
#endif
}

uint64_t Win32Backend::get_completion_fence() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	uint64_t latest_fence = 0;
	for (const QueueState &queue : priority_queues) {
		latest_fence = MAX(latest_fence, queue.completion_fence_value);
	}
	return latest_fence;
#else
	return 0;
#endif
}

bool Win32Backend::wait_for_completion(uint64_t p_fence_value, uint32_t p_timeout_msec) {
#ifdef _WIN32
	if (p_fence_value == 0) {
		return true;
	}
	if (!factory) {
		return false;
	}
	uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
	std::vector<std::pair<ID3D12Fence *, uint64_t>> fences_to_wait;
	uint64_t max_completion_fence = 0;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int q = 0; q < QUEUE_COUNT; q++) {
			const QueueState &queue = priority_queues[q];
			max_completion_fence = MAX(max_completion_fence, queue.completion_fence_value);
			if (queue.completion_fence && queue.completion_fence_value >= p_fence_value &&
					queue.completed_fence_value < p_fence_value) {
				fences_to_wait.push_back({ queue.completion_fence, queue.completion_fence_value });
			}
		}
	}
	if (p_fence_value > max_completion_fence) {
		if (p_timeout_msec > 0) {
			ID3D12Fence *fence = priority_queues[0].completion_fence;
			if (fence) {
				HANDLE wait_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (wait_event) {
					HRESULT hr = fence->SetEventOnCompletion(p_fence_value, wait_event);
					if (SUCCEEDED(hr)) {
						WaitForSingleObject(wait_event, p_timeout_msec);
					}
					CloseHandle(wait_event);
				}
			} else {
				Sleep(p_timeout_msec);
			}
		}
		return false;
	}
	if (fences_to_wait.empty()) {
		return true;
	}

	for (size_t i = 0; i < fences_to_wait.size(); i++) {
		ID3D12Fence *fence = fences_to_wait[i].first;
		uint64_t target_value = fences_to_wait[i].second;
		uint64_t completed_val = fence->GetCompletedValue();
		if (completed_val == UINT64_MAX) {
			return false;
		}
		if (completed_val < target_value) {
			uint64_t now = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
			uint32_t elapsed = (now >= start_time) ? static_cast<uint32_t>(now - start_time) : 0;
			if (elapsed >= p_timeout_msec) {
				return false;
			}
			uint32_t remaining_ms = p_timeout_msec - elapsed;
			HANDLE wait_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!wait_event) {
				return false;
			}
			HRESULT hr = fence->SetEventOnCompletion(target_value, wait_event);
			DWORD wait_result = SUCCEEDED(hr) ? WaitForSingleObject(wait_event, remaining_ms) : WAIT_FAILED;
			CloseHandle(wait_event);
			if (FAILED(hr) || wait_result != WAIT_OBJECT_0) {
				return false;
			}
			completed_val = fence->GetCompletedValue();
		}
		if (completed_val < target_value || completed_val == UINT64_MAX) {
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			for (QueueState &queue : priority_queues) {
				if (queue.completion_fence == fence) {
					queue.completed_fence_value = completed_val;
					break;
				}
			}
		}
	}
	return true;
#else
	return p_fence_value == 0;
#endif
}

void Win32Backend::poll_batch_status() {
#ifdef _WIN32
	Vector<GPUTargetTransition> post_transitions;
	std::deque<BatchMetadata> completed_batches;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (std::vector<ZombieBatchSlot>::iterator it = zombie_slots.begin(); it != zombie_slots.end(); ) {
			ID3D12Fence *fence = priority_queues[it->queue_index].completion_fence;
			if (!fence || fence->GetCompletedValue() >= it->fence_value) {
				for (int r = 0; r < it->requests.size(); r++) {
					if (it->requests[r].is_valid()) {
						it->requests[r]->set_gpu_in_flight(false);
					}
				}
				complete_chunked_requests();
				release_resource_guards(it->requests);
				for (int f = 0; f < it->files.size(); f++) {
					if (it->files[f]) {
						it->files[f]->Close();
					}
				}
				it = zombie_slots.erase(it);
			} else {
				++it;
			}
		}
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
				BatchSlot &slot = queue.batch_slots[slot_index];
				if (!slot.submitted || slot.completion_ready || !queue.completion_fence || slot.fence_value == 0 ||
						queue.completion_fence->GetCompletedValue() < slot.fence_value) {
					continue;
				}
				for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ++it) {
					if (it->queue_index == queue_index && it->slot_index == slot_index) {
						completed_batches.push_back(*it);
						completion_queue.erase(it);
						for (int t = 0; t < slot.gpu_transitions.size(); t++) {
							post_transitions.push_back(slot.gpu_transitions[t]);
						}
						slot.gpu_transitions.clear();
						break;
					}
				}
			}
		}
	}

	if (!post_transitions.is_empty()) {
		execute_transitions(post_transitions, false);
	}

	if (!completed_batches.empty()) {
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (const BatchMetadata &batch : completed_batches) {
			complete_batch_slot(priority_queues[batch.queue_index], priority_queues[batch.queue_index].batch_slots[batch.slot_index], batch);
		}
		batch_in_flight.store(has_in_flight_batches_locked());
		update_profiler_queue_depth_locked();
		batch_cv.notify_all();
	}
#endif
}

#ifdef _WIN32
void Win32Backend::poll_batch_status(QueueState &p_queue, Vector<Ref<DSRequest>> &p_requests, uint64_t p_fence_value) {
	if (!p_queue.completion_fence || p_fence_value == 0 ||
			p_queue.completion_fence->GetCompletedValue() < p_fence_value) {
		return;
	}

	p_queue.completed_fence_value = p_queue.completion_fence->GetCompletedValue();
	String queue_error = retrieve_queue_error(p_queue.queue);

	std::unordered_map<int64_t, ID3D12Resource *> rid_cache;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid()) {
			String resource_error;
			if (!validate_resource_guard(p_requests[i], resource_error, &rid_cache)) {
				UtilityFunctions::push_warning(resource_error);
				p_requests[i]->set_status(DSRequest::STATUS_FAILED);
				p_requests[i]->set_error_message(resource_error);
			} else if (!queue_error.is_empty()) {
				p_requests[i]->set_status(DSRequest::STATUS_FAILED);
				p_requests[i]->set_error_message(queue_error);
			} else if (p_requests[i]->get_status() != DSRequest::STATUS_FAILED) {
				p_requests[i]->set_status(DSRequest::STATUS_COMPLETED);
			}
		}
	}
	complete_chunked_requests();
}
#endif

#ifdef _WIN32
void Win32Backend::update_profiler_queue_depth_locked() {
	int pending_request_count = 0;
	for (const QueueState &queue : priority_queues) {
		for (const BatchSlot &slot : queue.batch_slots) {
			if (!slot.completion_ready) {
				pending_request_count += count_logical_requests(slot.requests);
			}
		}
	}
	DSProfiler *profiler = DSProfiler::get_singleton();
	if (profiler) {
		profiler->set_queue_depth(pending_request_count);
	}
}

void Win32Backend::complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &) {
	poll_batch_status(p_queue, p_slot.requests, p_slot.fence_value);
	p_slot.completed_count = count_completed_requests(p_slot.requests);
	p_slot.error_code = 0;
	p_slot.error_message = String();
	for (int i = 0; i < p_slot.requests.size(); i++) {
		if (p_slot.requests[i].is_valid() && p_slot.requests[i]->get_status() == DSRequest::STATUS_FAILED) {
			p_slot.error_code = -1;
			p_slot.error_message = p_slot.requests[i]->get_error_message();
			if (p_slot.error_message.is_empty()) {
				p_slot.error_message = "DirectStorage GPU batch failed.";
			}
			break;
		}
	}
	for (int i = 0; i < p_slot.requests.size(); i++) {
		if (p_slot.requests[i].is_valid()) {
			p_slot.requests[i]->set_gpu_in_flight(false);
		}
	}
	release_resource_guards(p_slot.requests);
	close_slot_files(p_slot);
	p_slot.submitted = false;
	p_slot.completion_ready = true;
}
#endif

#ifdef _WIN32
// Slot lifetime close: releases every slot-owned file handle, empties the
// vector, and drops the open-file cache in one step so a cleared slot can
// never serve a stale handle. Sole closer for BatchSlot files; all reset,
// completion, cancel, and shutdown paths go through here.
void Win32Backend::close_slot_files(BatchSlot &p_slot) {
	for (int i = 0; i < p_slot.files.size(); i++) {
		if (p_slot.files[i]) {
			p_slot.files[i]->Close();
			p_slot.files[i]->Release();
		}
	}
	p_slot.files.clear();
	p_slot.file_cache.clear();
	p_slot.gpu_transitions.clear();
}

// Releases every backend-level cached file handle. Caller must hold
// batch_mutex or guarantee no concurrent enqueue (init before the
// completion thread starts, shutdown after it joins).
void Win32Backend::clear_persistent_files() {
	for (int i = 0; i < persistent_files.size(); i++) {
		if (persistent_files[i]) {
			persistent_files[i]->Close();
			persistent_files[i]->Release();
		}
	}
	persistent_files.clear();
	persistent_file_cache.clear();
}

// Inserts an OpenFile'd handle into the backend-level cache. Returns false
// (and leaves the map untouched) on null handle or duplicate path — the
// caller keeps ownership on failure. Requires batch_mutex held, except
// during initialize() before the completion thread starts.
bool Win32Backend::cache_file_handle(const std::wstring &p_wpath, IDStorageFile *p_file) {
	if (!p_file || p_wpath.empty()) {
		return false;
	}
	if (persistent_file_cache.find(p_wpath) != persistent_file_cache.end()) {
		return false;
	}
	persistent_files.push_back(p_file);
	persistent_file_cache[p_wpath] = p_file;
	return true;
}

void Win32Backend::completion_thread_func() {
	HANDLE wait_handles[QUEUE_COUNT * BATCH_SLOT_COUNT + 1] = { completion_stop_event };
	for (int i = 0; i < QUEUE_COUNT; i++) {
		for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
			wait_handles[1 + i * BATCH_SLOT_COUNT + slot_index] = priority_queues[i].batch_slots[slot_index].event;
		}
	}
	auto fail_pending_batches = [this](const String &p_message) {
		for (BatchMetadata &batch : completion_queue) {
			QueueState &queue = priority_queues[batch.queue_index];
			BatchSlot &slot = queue.batch_slots[batch.slot_index];
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message(p_message);
				}
			}
			bool fence_completed = queue.completion_fence &&
					queue.completion_fence->GetCompletedValue() >= slot.fence_value;
			if (fence_completed || !slot.submitted) {
				close_slot_files(slot);
				release_resource_guards(slot.requests);
				for (int i = 0; i < slot.requests.size(); i++) {
					if (slot.requests[i].is_valid()) {
						slot.requests[i]->set_gpu_in_flight(false);
					}
				}
			}
			complete_chunked_requests();
			slot.completed_count = count_completed_requests(slot.requests);
			slot.error_code = -1;
			slot.error_message = p_message;
			slot.submitted = false;
			slot.completion_ready = true;
		}
		completion_queue.clear();
		batch_in_flight.store(false);
	};
	while (true) {
		{
			std::unique_lock<std::mutex> lock(batch_mutex);
			batch_cv.wait(lock, [this]() {
				return completion_thread_stop.load() || !completion_queue.empty();
			});
			if (completion_thread_stop.load()) {
				fail_pending_batches("DirectStorage backend shut down before batch completion.");
				return;
			}
		}

		DWORD wait_result = WaitForMultipleObjects(QUEUE_COUNT * BATCH_SLOT_COUNT + 1, wait_handles, FALSE, 500);
		if (wait_result == WAIT_TIMEOUT) {
			continue;
		}
		if (wait_result == WAIT_OBJECT_0) {
			std::lock_guard<std::mutex> lock(batch_mutex);
			fail_pending_batches("DirectStorage backend shut down before batch completion.");
			return;
		}
		if (wait_result < WAIT_OBJECT_0 + 1 || wait_result >= WAIT_OBJECT_0 + QUEUE_COUNT * BATCH_SLOT_COUNT + 1) {
			std::lock_guard<std::mutex> lock(batch_mutex);
			fail_pending_batches("DirectStorage batch completion wait failed.");
			return;
		}

		int completed_handle_index = static_cast<int>(wait_result - WAIT_OBJECT_0 - 1);
		int completed_queue_index = completed_handle_index / BATCH_SLOT_COUNT;
		int completed_slot_index = completed_handle_index % BATCH_SLOT_COUNT;
		BatchMetadata batch;
		Vector<GPUTargetTransition> post_transitions;
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			std::deque<BatchMetadata>::iterator batch_it = completion_queue.end();
			for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ++it) {
				if (it->queue_index == completed_queue_index && it->slot_index == completed_slot_index) {
					batch_it = it;
					break;
				}
			}
			if (batch_it == completion_queue.end()) {
				continue;
			}
			batch = *batch_it;
			completion_queue.erase(batch_it);
			BatchSlot &completed_slot = priority_queues[batch.queue_index].batch_slots[batch.slot_index];
			post_transitions = completed_slot.gpu_transitions;
			completed_slot.gpu_transitions.clear();
		}

		if (!post_transitions.is_empty()) {
			execute_transitions(post_transitions, false);
		}

		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			complete_batch_slot(priority_queues[batch.queue_index], priority_queues[batch.queue_index].batch_slots[batch.slot_index], batch);
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
		}

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();

		uint64_t end_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
		uint64_t duration_us = (end_time > batch.start_time) ? (end_time - batch.start_time) : 1;
		DSProfiler *profiler = DSProfiler::get_singleton();
		if (profiler) {
			ds_profiler_set_batch_metrics(
					batch.compressed_bytes,
					batch.uncompressed_bytes,
					batch.request_count,
					-1,
					(err_handler && err_handler->has_error()) ? 1 : 0);
				profiler->record_workload(batch.compressed_bytes, duration_us);
		}
		batch_cv.notify_all();
	}
}
#endif

#ifdef _WIN32
static Microsoft::WRL::ComPtr<ID3D12Device> s_d3d12_device;
static Microsoft::WRL::ComPtr<ID3D12CommandQueue> s_d3d12_command_queue;
static std::mutex s_device_mutex;

// One-shot render-thread acquisition context.
// Managed via std::shared_ptr so that if waiting times out, any late-running
// render-thread worker accesses a live heap context without touching a destroyed stack frame (HIGH-01).
struct DSDeviceAcquireContext {
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
};
static std::mutex s_device_acquire_mutex;
static std::shared_ptr<DSDeviceAcquireContext> s_device_acquire_ctx;

// Log-only adapter identity. Compares Godot's D3D12 adapter LUID against
// the default DXGI adapter; a mismatch is informational only (multi-GPU)
// and never fails acquisition.
static void ds_win32_log_d3d12_adapter_identity(ID3D12Device *p_device) {
	if (!p_device) {
		return;
	}
	LUID luid = p_device->GetAdapterLuid();
	char luid_buf[64];
	snprintf(luid_buf, sizeof(luid_buf), "%08X-%08X", (unsigned int)luid.HighPart, (unsigned int)luid.LowPart);
	UtilityFunctions::print_verbose(String("[DirectStorage] D3D12 device adapter LUID: ") + String(luid_buf) + ".");
	IDXGIFactory1 *factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
		IDXGIAdapter1 *default_adapter = nullptr;
		if (SUCCEEDED(factory->EnumAdapters1(0, &default_adapter)) && default_adapter) {
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(default_adapter->GetDesc1(&desc))) {
				bool match = (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart);
				if (match) {
					UtilityFunctions::print_verbose("[DirectStorage] D3D12 adapter is the default DXGI adapter.");
				} else {
					UtilityFunctions::print_verbose("[DirectStorage] D3D12 adapter is NOT the default DXGI adapter (multi-GPU system; informational only).");
				}
			}
			default_adapter->Release();
		}
		factory->Release();
	}
}

// Direct fetch; the caller must already be on the render thread.
static Microsoft::WRL::ComPtr<ID3D12Device> ds_win32_fetch_d3d12_device_direct() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return nullptr;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	if (!rd) {
		return nullptr;
	}
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	if (handle == 0) {
		return nullptr;
	}
	ID3D12Device *device = reinterpret_cast<ID3D12Device *>(handle);
	uint64_t queue_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	ID3D12CommandQueue *queue = reinterpret_cast<ID3D12CommandQueue *>(queue_handle);
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		s_d3d12_device = device;
		s_d3d12_command_queue = queue;
	}
	ds_win32_log_d3d12_adapter_identity(device);
	return s_d3d12_device;
}

// Render-thread worker posted via RenderingServer::call_on_render_thread().
// Must be a file-static function: PlatformBackend is not a Godot Object, so
// callable_mp on `this` is impossible and callable_mp_static is required.
static void ds_win32_fetch_d3d12_device_on_render_thread() {
	ds_win32_fetch_d3d12_device_direct();
	std::shared_ptr<DSDeviceAcquireContext> ctx;
	{
		std::lock_guard<std::mutex> lock(s_device_acquire_mutex);
		ctx = std::move(s_device_acquire_ctx);
	}
	if (ctx) {
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->completed = true;
		ctx->cv.notify_one(); // HIGH-01: Notify while holding mutex
	}
}

// D3D12-02 / HIGH-03: release owned COM references and clear the cache. Safe from any
// thread; COM Release is thread-safe. Called on shutdown and on
// device-removed detection so a stale pointer can never be returned.
void ds_win32_invalidate_d3d12_device() {
	std::lock_guard<std::mutex> lock(s_device_mutex);
	s_d3d12_device.Reset();
	s_d3d12_command_queue.Reset();
}

Microsoft::WRL::ComPtr<ID3D12Device> ds_win32_get_d3d12_device() {
	// D3D12-02 / HIGH-03: validate cached device against device-removed (TDR / driver update).
	Microsoft::WRL::ComPtr<ID3D12Device> cached;
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		cached = s_d3d12_device;
	}
	if (cached) {
		HRESULT removed = cached->GetDeviceRemovedReason();
		if (FAILED(removed)) {
			ds_win32_invalidate_d3d12_device();
			cached.Reset();
		} else {
			return cached;
		}
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return nullptr;
	}

	// Fast path: already on the render thread, fetch directly.
	if (rs->is_on_render_thread()) {
		return ds_win32_fetch_d3d12_device_direct();
	}

	// Marshal acquisition to the render thread with a bounded wait.
	auto ctx = std::make_shared<DSDeviceAcquireContext>();
	{
		std::lock_guard<std::mutex> lock(s_device_acquire_mutex);
		s_device_acquire_ctx = ctx;
	}
	rs->call_on_render_thread(callable_mp_static(ds_win32_fetch_d3d12_device_on_render_thread));
	{
		std::unique_lock<std::mutex> lock(ctx->mutex);
		if (!ctx->cv.wait_for(lock, std::chrono::seconds(5), [&ctx] { return ctx->completed; })) {
			std::lock_guard<std::mutex> acq_lock(s_device_acquire_mutex);
			if (s_device_acquire_ctx == ctx) {
				s_device_acquire_ctx.reset();
			}
			UtilityFunctions::print("[DirectStorage] D3D12 device acquisition timed out waiting for the render thread; continuing without a device.");
			return nullptr;
		}
	}

	std::lock_guard<std::mutex> lock(s_device_mutex);
	return s_d3d12_device;
}

Microsoft::WRL::ComPtr<ID3D12CommandQueue> ds_win32_get_d3d12_command_queue() {
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		if (s_d3d12_command_queue) {
			return s_d3d12_command_queue;
		}
	}

	ds_win32_get_d3d12_device();

	std::lock_guard<std::mutex> lock(s_device_mutex);
	return s_d3d12_command_queue;
}

// Shared DirectStorage runtime probe state. `is_available()` loads the
// runtime DLLs to verify factory/queue creation, then stashes the still-loaded
// modules here instead of unloading them. `initialize()` claims them for the
// same bin directory, so each process performs a single LoadLibrary pair.
// Guarded by s_probe_mutex; `shutdown()` releases any modules the probe
// stashed but `initialize()` never claimed.
static std::mutex s_probe_mutex;
static HMODULE s_probed_core_module = nullptr;
static HMODULE s_probed_dstorage_module = nullptr;
static std::wstring s_probed_bin_dir;

// Returns true and fills r_core/r_dstorage when a previous successful probe
// stashed loaded modules for the same bin directory. Caller takes ownership.
static bool claim_probed_runtime_modules(const String &p_bin_dir, HMODULE &r_core, HMODULE &r_dstorage) {
	std::lock_guard<std::mutex> lock(s_probe_mutex);
	std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data()));
	if (!s_probed_core_module || !s_probed_dstorage_module || s_probed_bin_dir != w_bin_dir) {
		return false;
	}
	r_core = s_probed_core_module;
	r_dstorage = s_probed_dstorage_module;
	s_probed_core_module = nullptr;
	s_probed_dstorage_module = nullptr;
	s_probed_bin_dir.clear();
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
	if (!rd) {
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
	queue_desc.Device = nullptr;
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
	{
		// Hand the still-loaded modules to initialize() instead of
		// unloading them; a newer probe replaces (and unloads) the old stash.
		std::lock_guard<std::mutex> lock(s_probe_mutex);
		unload_runtime_modules(s_probed_core_module, s_probed_dstorage_module);
		s_probed_core_module = core;
		s_probed_dstorage_module = dstorage;
		s_probed_bin_dir = reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data());
	}
	result.available = true;
	result.reason = "DirectStorage runtime, D3D12 driver, factory, and queue are available.";
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

#ifdef _WIN32
int Win32Backend::priority_to_queue_index(DSRequest::DSPriority p_priority) {
	int priority = static_cast<int>(p_priority);
	return priority >= DSRequest::PRIORITY_LOW && priority <= DSRequest::PRIORITY_HIGH ? priority : DSRequest::PRIORITY_NORMAL;
}
#endif

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

#ifdef _WIN32
bool Win32Backend::init_barrier_context(ID3D12Device *p_device, ID3D12CommandQueue *p_queue) {
	if (!p_device) {
		return false;
	}
	std::lock_guard<std::mutex> lock(barrier_context.mutex);
	HRESULT hr = p_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&barrier_context.cmd_allocator));
	if (FAILED(hr) || !barrier_context.cmd_allocator) {
		return false;
	}
	hr = p_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, barrier_context.cmd_allocator, nullptr, IID_PPV_ARGS(&barrier_context.cmd_list));
	if (FAILED(hr) || !barrier_context.cmd_list) {
		cleanup_barrier_context();
		return false;
	}
	barrier_context.cmd_list->Close();

	hr = p_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&barrier_context.transition_fence));
	if (FAILED(hr) || !barrier_context.transition_fence) {
		cleanup_barrier_context();
		return false;
	}
	barrier_context.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	if (!barrier_context.fence_event) {
		cleanup_barrier_context();
		return false;
	}
	barrier_context.fence_value = 0;
	barrier_context.command_queue = p_queue ? p_queue : ds_win32_get_d3d12_command_queue();
	return true;
}

void Win32Backend::cleanup_barrier_context() {
	std::lock_guard<std::mutex> lock(barrier_context.mutex);
	if (barrier_context.transition_fence) {
		if (barrier_context.fence_value > 0 && barrier_context.fence_event) {
			if (barrier_context.transition_fence->GetCompletedValue() < barrier_context.fence_value) {
				barrier_context.transition_fence->SetEventOnCompletion(barrier_context.fence_value, barrier_context.fence_event);
				WaitForSingleObject(barrier_context.fence_event, 1000);
			}
		}
		barrier_context.transition_fence->Release();
		barrier_context.transition_fence = nullptr;
	}
	if (barrier_context.fence_event) {
		CloseHandle(barrier_context.fence_event);
		barrier_context.fence_event = nullptr;
	}
	if (barrier_context.cmd_list) {
		barrier_context.cmd_list->Release();
		barrier_context.cmd_list = nullptr;
	}
	if (barrier_context.cmd_allocator) {
		barrier_context.cmd_allocator->Release();
		barrier_context.cmd_allocator = nullptr;
	}
	barrier_context.command_queue.Reset();
	barrier_context.fence_value = 0;
}

struct DSTransitionHandoffContext {
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
	bool success = false;
	Win32Backend *backend = nullptr;
	Vector<Win32Backend::GPUTargetTransition> transitions;
	bool to_common = false;
};
static std::mutex s_transition_dispatch_mutex;
static std::vector<std::shared_ptr<DSTransitionHandoffContext>> s_transition_queue;

void ds_win32_execute_transitions_on_render_thread() {
	std::shared_ptr<DSTransitionHandoffContext> ctx;
	{
		std::lock_guard<std::mutex> lock(s_transition_dispatch_mutex);
		if (!s_transition_queue.empty()) {
			ctx = s_transition_queue.front();
			s_transition_queue.erase(s_transition_queue.begin());
		}
	}
	if (!ctx || !ctx->backend) {
		return;
	}
	bool ok = ctx->backend->execute_transitions_internal(ctx->transitions, ctx->to_common);
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->success = ok;
		ctx->completed = true;
		ctx->cv.notify_one();
	}
}

bool Win32Backend::execute_transitions(const Vector<GPUTargetTransition> &p_transitions, bool p_to_common) {
	if (p_transitions.is_empty()) {
		return true;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return false;
	}

	// Fast-path: Check if any transition actually changes resource state.
	// In the common case (e.g. textures created in COMMON state), state_before == state_after
	// and no barrier is required. Skipping dispatch avoids unnecessary thread marshaling and CV wait.
	bool has_effective_transitions = false;
	for (int i = 0; i < p_transitions.size(); i++) {
		const GPUTargetTransition &t = p_transitions[i];
		if (!t.resource) continue;
		D3D12_RESOURCE_STATES state_before = p_to_common ? t.original_state : D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES state_after = p_to_common ? D3D12_RESOURCE_STATE_COMMON : t.original_state;
		if (state_before != state_after) {
			has_effective_transitions = true;
			break;
		}
	}
	if (!has_effective_transitions) {
		return true;
	}

	if (!rs->is_on_render_thread()) {
		auto ctx = std::make_shared<DSTransitionHandoffContext>();
		ctx->backend = this;
		ctx->transitions = p_transitions;
		ctx->to_common = p_to_common;
		{
			std::lock_guard<std::mutex> lock(s_transition_dispatch_mutex);
			s_transition_queue.push_back(ctx);
		}
		rs->call_on_render_thread(callable_mp_static(ds_win32_execute_transitions_on_render_thread));
		{
			std::unique_lock<std::mutex> lock(ctx->mutex);
			constexpr std::chrono::milliseconds kTransitionTimeout(1000);
			bool signaled = ctx->cv.wait_for(lock, kTransitionTimeout, [&ctx] { return ctx->completed; });
			if (!signaled) {
				{
					std::lock_guard<std::mutex> lock_q(s_transition_dispatch_mutex);
					auto it = std::find(s_transition_queue.begin(), s_transition_queue.end(), ctx);
					if (it != s_transition_queue.end()) {
						s_transition_queue.erase(it);
					}
				}
				UtilityFunctions::push_error("[DirectStorage] Render-thread transition dispatch timed out.");
				return false;
			}
			return ctx->success;
		}
	}

	return execute_transitions_internal(p_transitions, p_to_common);
}

bool Win32Backend::execute_transitions_internal(const Vector<GPUTargetTransition> &p_transitions, bool p_to_common) {
	std::lock_guard<std::mutex> lock(barrier_context.mutex);
	if (!barrier_context.cmd_allocator || !barrier_context.cmd_list || !barrier_context.transition_fence) {
		return false;
	}

	Microsoft::WRL::ComPtr<ID3D12CommandQueue> godot_queue = barrier_context.command_queue ? barrier_context.command_queue : ds_win32_get_d3d12_command_queue();
	if (!godot_queue) {
		return false;
	}
	barrier_context.command_queue = godot_queue;

	// HIGH-06: Guard allocator reset with a bounded wait on transition_fence if previous barrier list is still executing
	if (barrier_context.fence_value > 0 &&
			barrier_context.transition_fence->GetCompletedValue() < barrier_context.fence_value) {
		if (barrier_context.fence_event) {
			HRESULT wait_hr = barrier_context.transition_fence->SetEventOnCompletion(barrier_context.fence_value, barrier_context.fence_event);
			if (SUCCEEDED(wait_hr)) {
				WaitForSingleObject(barrier_context.fence_event, 100);
			}
		}
		if (barrier_context.transition_fence->GetCompletedValue() < barrier_context.fence_value) {
			UtilityFunctions::push_error("[DirectStorage] Previous barrier command list still running on GPU after bounded wait; aborting transition.");
			return false;
		}
	}

	HRESULT hr = barrier_context.cmd_allocator->Reset();
	if (FAILED(hr)) return false;
	hr = barrier_context.cmd_list->Reset(barrier_context.cmd_allocator, nullptr);
	if (FAILED(hr)) return false;

	std::set<std::pair<ID3D12Resource *, uint32_t>> transitioned_subresources;
	for (int i = 0; i < p_transitions.size(); i++) {
		const GPUTargetTransition &t = p_transitions[i];
		if (!t.resource) continue;
		D3D12_RESOURCE_STATES state_before = p_to_common ? t.original_state : D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES state_after = p_to_common ? D3D12_RESOURCE_STATE_COMMON : t.original_state;
		if (state_before == state_after) continue;

		for (uint32_t s = 0; s < t.subresource_count; s++) {
			uint32_t sub_idx = t.first_subresource + s;
			if (!transitioned_subresources.insert(std::make_pair(t.resource.Get(), sub_idx)).second) {
				continue;
			}
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
			b.Transition.pResource = t.resource.Get();
			b.Transition.Subresource = sub_idx;
			b.Transition.StateBefore = state_before;
			b.Transition.StateAfter = state_after;
			barrier_context.cmd_list->ResourceBarrier(1, &b);
		}
	}
	hr = barrier_context.cmd_list->Close();
	if (FAILED(hr)) return false;

	uint64_t target_fence = ++barrier_context.fence_value;
	ID3D12CommandList *lists[] = { barrier_context.cmd_list };
	godot_queue->ExecuteCommandLists(1, lists);
	godot_queue->Signal(barrier_context.transition_fence, target_fence);

	if (barrier_context.transition_fence->GetCompletedValue() < target_fence) {
		hr = barrier_context.transition_fence->SetEventOnCompletion(target_fence, barrier_context.fence_event);
		if (SUCCEEDED(hr)) {
			WaitForSingleObject(barrier_context.fence_event, 500);
		}
	}
	return barrier_context.transition_fence->GetCompletedValue() >= target_fence;
}
#endif

bool Win32Backend::initialize(const String &p_path) {
#ifdef _WIN32
	s_backend_instance = this;
	active_batch_slot.store(0);
	sync_batch_in_flight.store(false);
	for (QueueState &queue : priority_queues) {
		queue.completion_fence_value = 0;
		queue.completed_fence_value = 0;
		for (BatchSlot &slot : queue.batch_slots) {
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.closing = false;
			slot.submitted = false;
			slot.completion_ready = false;
		}
	}
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
		HMODULE claimed_core = nullptr;
		HMODULE claimed_dstorage = nullptr;
		if (claim_probed_runtime_modules(bin_dir, claimed_core, claimed_dstorage)) {
			// Reuse the modules is_available() already loaded and verified;
			// this is the single LoadLibrary pair for the process.
			dstoragecore_module = claimed_core;
			dstorage_module = claimed_dstorage;
		} else {
			String dstoragecore_path = bin_dir + "/dstoragecore.dll";
			String dstorage_path = bin_dir + "/dstorage.dll";

			std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(bin_dir.utf16().get_data()));
			std::wstring w_core_path(reinterpret_cast<const wchar_t *>(dstoragecore_path.utf16().get_data()));
			std::wstring w_ds_path(reinterpret_cast<const wchar_t *>(dstorage_path.utf16().get_data()));

			DllDirectoryScope dll_scope;
			SetDllDirectoryW(w_bin_dir.c_str());
			dstoragecore_module = LoadLibraryW(w_core_path.c_str());
			if (!dstoragecore_module) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: DirectStorage runtime DLL 'dstoragecore.dll' could not be loaded from '" + bin_dir + "'.");
				return false;
			}
			dstorage_module = LoadLibraryW(w_ds_path.c_str());
			if (!dstorage_module) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: DirectStorage runtime DLL 'dstorage.dll' could not be loaded from '" + bin_dir + "'.");
				unload_runtime_modules(dstoragecore_module, dstorage_module);
				dstoragecore_module = nullptr;
				dstorage_module = nullptr;
				return false;
			}
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
#ifdef DEBUG_ENABLED
		factory->SetDebugFlags(DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_RECORD_OBJECT_NAMES);
#else
		factory->SetDebugFlags(DSTORAGE_DEBUG_NONE);
#endif
		factory->SetStagingBufferSize(staging_mb * 1024 * 1024);

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = capacity;
		RenderingServer *rs = RenderingServer::get_singleton();
		String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : "";
		if (driver_name != "d3d12") {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Rendering driver is '" + driver_name + "' (Direct3D 12 is required).");
			return false;
		}
		RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
		if (!rd) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: A usable D3D12 RenderingDevice is not active.");
			return false;
		}
		Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device = ds_win32_get_d3d12_device();
		if (!d3d12_device) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not acquire a usable D3D12 device.");
			return false;
		}
		queueDesc.Device = d3d12_device.Get();

		step_name = "CreateQueue";
		for (int i = 0; i < QUEUE_COUNT && SUCCEEDED(hr); i++) {
			QueueState &queue = priority_queues[i];
			queueDesc.Priority = i == DSRequest::PRIORITY_LOW ? DSTORAGE_PRIORITY_LOW :
					i == DSRequest::PRIORITY_HIGH ? DSTORAGE_PRIORITY_HIGH : DSTORAGE_PRIORITY_NORMAL;
			queueDesc.Name = i == DSRequest::PRIORITY_LOW ? "DirectStorageLowPriorityQueue" :
					i == DSRequest::PRIORITY_HIGH ? "DirectStorageHighPriorityQueue" : "DirectStorageNormalPriorityQueue";
			if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.completion_fence))) || !queue.completion_fence) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the D3D12 completion fence.");
				hr = E_FAIL;
				break;
			}
			queue.completion_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			for (BatchSlot &slot : queue.batch_slots) {
				slot.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			}
			if (!queue.completion_event || !queue.batch_slots[0].event || !queue.batch_slots[1].event) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create a queue completion event.");
				hr = E_FAIL;
				break;
			}
			hr = factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&queue.queue));
		}
	}

	bool queues_ready = SUCCEEDED(hr);
	for (const QueueState &queue : priority_queues) {
		queues_ready = queues_ready && queue.queue != nullptr && queue.completion_event != nullptr && queue.batch_slots[0].event != nullptr && queue.batch_slots[1].event != nullptr && queue.completion_fence != nullptr;
	}
	if (queues_ready) {
		init_barrier_context(ds_win32_get_d3d12_device().Get(), ds_win32_get_d3d12_command_queue().Get());
		// Eager archive pre-open: move the first OpenFile + BypassIO
		// negotiation for the exported texture archive off the first
		// prewarm frame and into init (autoload _ready, behind the menu).
		// Single-threaded here (completion thread not started yet), so no
		// lock needed. Silent on failure — the enqueue path opens lazily.
		if (factory) {
			String archive_logical = "res://textures.dspkg";
			String archive_global = archive_logical;
			if (ps && (archive_logical.begins_with("res://") || archive_logical.begins_with("user://"))) {
				archive_global = ps->globalize_path(archive_logical);
			}
			if (FileAccess::file_exists(archive_global)) {
				String norm_archive = archive_global.replace("/", "\\");
				std::wstring w_archive(reinterpret_cast<const wchar_t *>(norm_archive.utf16().get_data()));
				IDStorageFile *archive_file = nullptr;
				if (SUCCEEDED(factory->OpenFile(w_archive.c_str(), IID_PPV_ARGS(&archive_file))) && archive_file) {
					if (!cache_file_handle(w_archive, archive_file)) {
						archive_file->Close();
						archive_file->Release();
					}
				}
			} else {
				OS *os = OS::get_singleton();
				if (os) {
					String exe_archive = os->get_executable_path().get_base_dir().path_join("textures.dspkg");
					if (FileAccess::file_exists(exe_archive)) {
						String norm_exe = exe_archive.replace("/", "\\");
						std::wstring w_exe(reinterpret_cast<const wchar_t *>(norm_exe.utf16().get_data()));
						IDStorageFile *exe_file = nullptr;
						if (SUCCEEDED(factory->OpenFile(w_exe.c_str(), IID_PPV_ARGS(&exe_file))) && exe_file) {
							if (!cache_file_handle(w_exe, exe_file)) {
								exe_file->Close();
								exe_file->Release();
							}
						}
					}
				}
			}
		}
		completion_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!completion_stop_event) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the completion thread stop event.");
			clear_persistent_files();
			return false;
		}
			accepting_requests.store(true);
			completion_thread_stop.store(false);
			batch_in_flight.store(false);
			{
				std::lock_guard<std::mutex> lock(batch_mutex);
				completion_queue.clear();
			}
		completion_thread = std::thread(&Win32Backend::completion_thread_func, this);

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			for (const QueueState &queue : priority_queues) {
				err_handler->register_queue(queue.queue);
			}
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

#ifdef _WIN32
bool Win32Backend::drain_all_queues(uint32_t p_timeout_ms) {
	HANDLE wait_events[QUEUE_COUNT];
	int event_count = 0;

	for (int i = 0; i < QUEUE_COUNT; i++) {
		QueueState &queue = priority_queues[i];
		if (queue.completion_fence && queue.completion_fence_value != 0 && queue.completion_event) {
			uint64_t completed = queue.completion_fence->GetCompletedValue();
			if (completed != UINT64_MAX && completed < queue.completion_fence_value) {
				if (SUCCEEDED(queue.completion_fence->SetEventOnCompletion(queue.completion_fence_value, queue.completion_event))) {
					wait_events[event_count++] = queue.completion_event;
				}
			}
		}
	}

	if (event_count == 0) {
		return true;
	}

	DWORD wait_result = WaitForMultipleObjects(event_count, wait_events, TRUE, p_timeout_ms);
	if (wait_result >= WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + event_count) {
		for (int i = 0; i < QUEUE_COUNT; i++) {
			if (priority_queues[i].completion_fence) {
				priority_queues[i].completed_fence_value = priority_queues[i].completion_fence->GetCompletedValue();
			}
		}
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (size_t z = 0; z < zombie_slots.size(); z++) {
			for (int r = 0; r < zombie_slots[z].requests.size(); r++) {
				if (zombie_slots[z].requests[r].is_valid()) {
					zombie_slots[z].requests[r]->set_gpu_in_flight(false);
				}
			}
			complete_chunked_requests();
			release_resource_guards(zombie_slots[z].requests);
			for (int f = 0; f < zombie_slots[z].files.size(); f++) {
				if (zombie_slots[z].files[f]) {
					zombie_slots[z].files[f]->Close();
				}
			}
		}
		zombie_slots.clear();
		return true;
	}

	return false;
}
#endif

void Win32Backend::shutdown() {
#ifdef _WIN32
	// 1. Immediately refuse any new incoming work.
	accepting_requests.store(false);

	// 2. Drain all GPU DMA work across all priority queues concurrently BEFORE
	// stopping the completion thread or touching any file handles or resource guards.
	ProjectSettings *ps = ProjectSettings::get_singleton();
	uint32_t timeout_ms = 10000;
	if (ps && ps->has_setting("directstorage/shutdown_drain_timeout_ms")) {
		timeout_ms = static_cast<uint32_t>(MAX(100, int64_t(ps->get_setting("directstorage/shutdown_drain_timeout_ms"))));
	}
	bool drained = drain_all_queues(timeout_ms);

	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->unregister_queue();
	}

	if (!drained) {
		// Drain timed out (GPU hardware hang, TDR, or driver deadlock).
		// Releasing guards, closing in-flight files, or unloading DirectStorage DLLs
		// while hardware or driver DMA is active causes an immediate unhandled Access Violation.
		// Instead: signal stop, detach completion_thread to avoid std::terminate(), leak handles,
		// and skip unloading runtime modules. The OS kernel process teardown reclaims all resources cleanly.
		UtilityFunctions::push_error("[DirectStorage] Shutdown drain timed out after " + String::num_uint64(timeout_ms) +
				" ms (GPU/DMA hung); leaking handles and skipping DLL unload to avoid use-after-free.");
		completion_thread_stop.store(true);
		sync_batch_in_flight.store(false);
		if (completion_stop_event) {
			SetEvent(completion_stop_event);
		}
		batch_cv.notify_all();
		if (completion_thread.joinable()) {
			completion_thread.detach();
		}
		if (s_backend_instance == this) {
			s_backend_instance = nullptr;
		}
		return;
	}

	// 3. Normal graceful path: all DMA is drained. Signal and join the completion thread.
	completion_thread_stop.store(true);
	sync_batch_in_flight.store(false);
	if (completion_stop_event) {
		SetEvent(completion_stop_event);
	}
	batch_cv.notify_all();
	if (completion_thread.joinable()) {
		completion_thread.join();
	}
	batch_in_flight.store(false);

	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		release_all_resource_guards();
	}

	for (QueueState &queue : priority_queues) {
		for (BatchSlot &slot : queue.batch_slots) {
			close_slot_files(slot);
			{
				std::lock_guard<std::mutex> lock(batch_mutex);
				slot.requests.clear();
			}
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.submitted = false;
			slot.completion_ready = false;
			if (slot.event) {
				CloseHandle(slot.event);
				slot.event = nullptr;
			}
		}
		if (queue.queue) {
			queue.queue->Close();
			queue.queue->Release();
			queue.queue = nullptr;
		}
		if (queue.completion_event) {
			CloseHandle(queue.completion_event);
			queue.completion_event = nullptr;
		}
		if (queue.completion_fence) {
			queue.completion_fence->Release();
			queue.completion_fence = nullptr;
		}
		queue.completion_fence_value = 0;
		queue.completed_fence_value = 0;
	}

	cleanup_barrier_context();

	// Completion thread is joined above, so no concurrent enqueue can run;
	// take the lock for consistency with the enqueue path.
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		clear_persistent_files();
	}

	if (factory) {
		factory->Release();
		factory = nullptr;
	}

	if (completion_stop_event) {
		CloseHandle(completion_stop_event);
		completion_stop_event = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_queue.clear();
		s_chunked_requests.clear();
		s_chunk_parents.clear();
		s_staging_buffer_warnings.clear();
		if (s_dspkg_cache) {
			memdelete(s_dspkg_cache);
			s_dspkg_cache = nullptr;
		}
	}
	{
		ds_win32_invalidate_d3d12_device();
	}
	active_batch_slot.store(0);
	if (s_backend_instance == this) {
		s_backend_instance = nullptr;
	}
	unload_runtime_modules(dstoragecore_module, dstorage_module);
	dstoragecore_module = nullptr;
	dstorage_module = nullptr;
	{
		// Release any probe-stashed modules initialize() never claimed
		// (probe succeeded but init never ran) so they cannot leak.
		std::lock_guard<std::mutex> lock(s_probe_mutex);
		unload_runtime_modules(s_probed_core_module, s_probed_dstorage_module);
		s_probed_core_module = nullptr;
		s_probed_dstorage_module = nullptr;
		s_probed_bin_dir.clear();
	}
#endif
}

bool Win32Backend::enqueue_request(Ref<DSRequest> p_request) {
#ifdef _WIN32
	if (!accepting_requests.load() || p_request.is_null() || !factory) {
		if (p_request.is_valid()) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message(!accepting_requests.load() ? "Win32Backend is shutting down." : "Win32Backend not initialized.");
		}
		return false;
	}
	if (sync_batch_in_flight.load()) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend synchronous batch submission is in progress.");
		return false;
	}
	int queue_index = priority_to_queue_index(p_request->get_priority());
	QueueState &queue = priority_queues[queue_index];
	if (!queue.queue) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend priority queue is not initialized.");
		return false;
	}

	// Query GPU destination resource info once on the render thread BEFORE taking batch_mutex.
	// This covers single requests and multi-subresource/chunk splits without
	// issuing redundant cross-thread round-trips for each mip/chunk, and prevents
	// holding batch_mutex across cross-thread render-thread waits.
	bool is_gpu_buffer = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool is_gpu_texture = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *cached_dest_resource = nullptr;
	D3D12_RESOURCE_DESC cached_dest_desc = {};
	uint64_t cached_dest_native_format = 0;
	bool cached_rd_available = false;
	bool cached_dest_resolved = false;
	if (is_gpu_buffer || is_gpu_texture) {
		RID dest_rid = p_request->get_destination_resource();
		if (dest_rid.is_valid()) {
			cached_dest_resolved = ds_win32_query_destination_resource_on_render_thread(
					is_gpu_buffer, dest_rid, cached_dest_resource, cached_dest_desc, cached_dest_native_format, cached_rd_available);
		}
	}

	std::lock_guard<std::mutex> lock(batch_mutex);
	if (sync_batch_in_flight.load()) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend synchronous batch submission is in progress.");
		return false;
	}
	int current_slot = active_batch_slot.load();
	BatchSlot *slot_ptr = &queue.batch_slots[current_slot];
	if (!slot_ptr->submitted && !slot_ptr->closing && slot_ptr->completion_ready) {
		close_slot_files(*slot_ptr);
		slot_ptr->requests.clear();
		slot_ptr->fence_value = 0;
		slot_ptr->completed_count = 0;
		slot_ptr->error_code = 0;
		slot_ptr->error_message = String();
		slot_ptr->completion_ready = false;
	} else if (slot_ptr->submitted || slot_ptr->closing) {
		int alternate_slot = 1 - current_slot;
		BatchSlot &alt_slot = queue.batch_slots[alternate_slot];
		if (!alt_slot.submitted && !alt_slot.closing && alt_slot.completion_ready) {
			close_slot_files(alt_slot);
			alt_slot.requests.clear();
			alt_slot.fence_value = 0;
			alt_slot.completed_count = 0;
			alt_slot.error_code = 0;
			alt_slot.error_message = String();
			alt_slot.completion_ready = false;
		}
		if (!alt_slot.submitted && !alt_slot.closing && !alt_slot.completion_ready) {
			active_batch_slot.store(alternate_slot);
			current_slot = alternate_slot;
			slot_ptr = &queue.batch_slots[current_slot];
		}
	}
	BatchSlot &slot = *slot_ptr;
	if (slot.submitted || slot.closing || slot.completion_ready) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend batch slot is not available; collect completed results before enqueueing more requests.");
		return false;
	}

	// GPU synchronization contract - in-flight destination scan. A destination
	// ID3D12Resource (AddRef'd in s_resource_guards at enqueue time, so the pointer
	// is stable while DirectStorage may own it) is in flight while it appears under
	// guard in any slot with submitted && !completion_ready. That is exactly the
	// window where D3D12_RESOURCE_STATE_COMMON cannot be established by anyone but
	// the barrier owner (Godot RD) and where sampling/reuse is undefined, so a new
	// GPU enqueue against it is rejected loudly instead of proceeding silently.
	// Design notes:
	// - DirectStorage requires destination resources to be in D3D12_RESOURCE_STATE_COMMON
	//   during GPU decompression. The extension transitions destination textures to COMMON
	//   at submit time and restores them afterwards.
	// - No silent CPU fallback: re-targeting to a memory destination behind the
	//   caller's back would leave the GPU texture unwritten while the caller samples
	//   it as if DirectStorage had filled it. The request fails with an error that
	//   names the CPU-memory alternative instead.
	// - Duplicates inside the slot currently being filled are exempt by construction
	//   (the filling slot is never submitted - see the guard above): multi-mip
	//   GDeflate splits enqueue several children against one texture into a single
	//   slot that submits/completes atomically. Same-submit aliases in one batch rely
	//   on non-overlapping ranges plus fence gating, unchanged.
	// - Slots with completion_ready (results collected or awaiting collection) are
	//   exempt: DirectStorage has finished writing, so the resource is Godot-owned
	//   again; batch_completed / wait_for_completion remains the sanctioned gate for
	//   sampling. Must be called with batch_mutex held (enqueue_request holds it
	//   here; every site flipping submitted/completion_ready holds it too).
	auto gpu_destination_in_flight = [&](ID3D12Resource *p_resource) -> bool {
		if (!p_resource) {
			return false;
		}
		for (int check_queue = 0; check_queue < QUEUE_COUNT; check_queue++) {
			const QueueState &check_state = priority_queues[check_queue];
			for (int check_slot = 0; check_slot < BATCH_SLOT_COUNT; check_slot++) {
				const BatchSlot &check = check_state.batch_slots[check_slot];
				if (!check.submitted || check.completion_ready) {
					continue;
				}
				for (int check_request = 0; check_request < check.requests.size(); check_request++) {
					if (check.requests[check_request].is_null()) {
						continue;
					}
					std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard_it = s_resource_guards.find(check.requests[check_request].ptr());
					if (guard_it != s_resource_guards.end() && guard_it->second == p_resource) {
						return true;
					}
				}
			}
		}
		return false;
	};

	auto enqueue_one = [&](const Ref<DSRequest> &p_child, uint64_t p_destination_offset) -> bool {
		String path = p_child->get_file_path();
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (ps && (path.begins_with("res://") || path.begins_with("user://"))) {
			path = ps->globalize_path(path);
		}
		String norm_file_path = path.replace("/", "\\");
		std::wstring wpath(reinterpret_cast<const wchar_t *>(norm_file_path.utf16().get_data()));

		IDStorageFile *file = nullptr;
		// Backend-level cache first: survives batch-slot recycling, so the
		// shared archive pays OpenFile once per process instead of once per
		// slot. Persistent handles are owned by persistent_files and must
		// never be pushed into slot.files (close_slot_files would close a
		// handle this map still serves). batch_mutex is held by the caller.
		std::unordered_map<std::wstring, IDStorageFile *>::const_iterator persistent_handle = persistent_file_cache.find(wpath);
		if (persistent_handle != persistent_file_cache.end()) {
			file = persistent_handle->second;
		} else {
			std::unordered_map<std::wstring, IDStorageFile *>::const_iterator cached_handle = slot.file_cache.find(wpath);
			if (cached_handle != slot.file_cache.end()) {
				file = cached_handle->second;
			} else {
				HRESULT hr = factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&file));
				if (FAILED(hr) || !file) {
					p_child->set_status(DSRequest::STATUS_FAILED);
					p_child->set_error_message("Win32Backend: Failed to open file '" + path + "'.");
					return false;
				}
				// New opens join the persistent cache (slot files stay for
				// legacy slot-owned handles only).
				if (!cache_file_handle(wpath, file)) {
					// Duplicate race: another path cached it first — use the
					// winner and release ours.
					file->Close();
					file->Release();
					persistent_handle = persistent_file_cache.find(wpath);
					if (persistent_handle != persistent_file_cache.end()) {
						file = persistent_handle->second;
					} else {
						p_child->set_status(DSRequest::STATUS_FAILED);
						p_child->set_error_message("Win32Backend: Failed to cache file '" + path + "'.");
						return false;
					}
				}
			}
		}

	uint32_t src_size = p_child->get_size();
	uint32_t uncompressed_size = p_child->get_uncompressed_size();
	if (p_child->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = src_size;
	}

	if (src_size == 0 || uncompressed_size == 0) {
		// Slot-owned handle: close_slot_files() releases it at batch
		// completion; never closed on per-request error paths.
		p_child->set_status(DSRequest::STATUS_FAILED);
		p_child->set_error_message("Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").");
		return false;
	}

	bool gpu_buffer_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool gpu_texture_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *destination_buffer = nullptr;
	ID3D12Resource *destination_texture = nullptr;

	// Resolve destination resource info via render-thread marshaling.
	ID3D12Resource *dest_res = cached_dest_resource;
	D3D12_RESOURCE_DESC dest_desc = cached_dest_desc;
	uint64_t dest_native_format = cached_dest_native_format;
	bool dest_rd_avail = cached_rd_available;
	bool dest_res_ok = cached_dest_resolved;
	if ((gpu_buffer_destination || gpu_texture_destination) &&
			p_child->get_destination_resource() != p_request->get_destination_resource()) {
		dest_res_ok = ds_win32_query_destination_resource_on_render_thread(
				gpu_buffer_destination, p_child->get_destination_resource(), dest_res, dest_desc, dest_native_format, dest_rd_avail);
	}

	if (gpu_buffer_destination) {
		String destination_error;
		RenderingServer *rs = RenderingServer::get_singleton();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU buffer destination requires the D3D12 rendering driver.";
		} else if (!dest_rd_avail) {
			destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU buffer destination.";
		} else if (!p_child->get_destination_resource().is_valid()) {
			destination_error = "Win32Backend: GPU buffer destination resource RID is invalid.";
		} else if (!dest_res_ok || !dest_res) {
			destination_error = "Win32Backend: GPU buffer RID could not be mapped to an ID3D12Resource.";
		} else {
			destination_buffer = dest_res;
			const D3D12_RESOURCE_DESC &buffer_desc = dest_desc;
			if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || p_destination_offset + uncompressed_size > buffer_desc.Width) {
				destination_error = "Win32Backend: GPU buffer destination size exceeds the mapped D3D12 buffer resource.";
			} else if (gpu_destination_in_flight(destination_buffer)) {
				// COMMON cannot be established while DirectStorage owns this
				// buffer - reject loudly, do not fall back silently (see above).
				destination_error = "Win32Backend: GPU buffer destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
			}
		}

		if (!destination_error.is_empty()) {
			// Slot-owned handle: close_slot_files() releases it at batch
			// completion; never closed on per-request error paths.
			p_child->set_status(DSRequest::STATUS_FAILED);
			p_child->set_error_message(destination_error);
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
		} else if (!dest_rd_avail) {
			destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU texture destination.";
		} else if (!p_child->get_destination_resource().is_valid()) {
			destination_error = "Win32Backend: GPU texture destination resource RID is invalid.";
		} else if (!dest_res_ok || !dest_res) {
			destination_error = "Win32Backend: GPU texture RID could not be mapped to an ID3D12Resource.";
		} else {
			destination_texture = dest_res;
			const D3D12_RESOURCE_DESC &texture_desc = dest_desc;
			if (texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
					texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
					texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
				destination_error = "Win32Backend: GPU texture destination RID does not reference a texture resource.";
			} else {
				uint64_t mip_levels = texture_desc.MipLevels > 0 ? texture_desc.MipLevels : 1;
				uint64_t array_size = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : texture_desc.DepthOrArraySize;
				uint64_t subresource_total = mip_levels * array_size;
				uint32_t first_subresource = p_child->get_first_subresource();
				uint32_t subresource_count = p_child->get_subresource_count();
				if (subresource_count == 0 || first_subresource >= subresource_total ||
						subresource_count > subresource_total - first_subresource) {
					destination_error = "Win32Backend: GPU texture subresource range is out of bounds (first=" +
							String::num_uint64(first_subresource) + ", count=" + String::num_uint64(subresource_count) +
							", available=" + String::num_uint64(subresource_total) + ").";
				} else if (p_child->get_texture_layout() != 0 &&
						p_child->get_texture_layout() != static_cast<uint32_t>(texture_desc.Layout)) {
					destination_error = "Win32Backend: GPU texture layout metadata does not match the destination resource.";
				} else if (p_child->get_texture_format() != 0) {
					if (dest_native_format == 0 || p_child->get_texture_format() != dest_native_format) {
						destination_error = "Win32Backend: GPU texture format metadata does not match the destination resource.";
					} else if (gpu_destination_in_flight(destination_texture)) {
						// MULTIPLE_SUBRESOURCES_RANGE destinations must be in COMMON;
						// state is unknowable while DirectStorage owns this texture -
						// reject loudly, do not fall back silently (see above).
						destination_error = "Win32Backend: GPU texture destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before sampling or reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
					}
				} else if (gpu_destination_in_flight(destination_texture)) {
					// Same in-flight rejection when no format metadata was supplied
					// (the format check above is skipped, the contract still applies).
					destination_error = "Win32Backend: GPU texture destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before sampling or reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
				}
			}
		}

		if (!destination_error.is_empty()) {
			// Slot-owned handle: close_slot_files() releases it at batch
			// completion; never closed on per-request error paths.
			p_child->set_status(DSRequest::STATUS_FAILED);
			p_child->set_error_message(destination_error);
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
			gpu_texture_destination ? DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE :
			DSTORAGE_REQUEST_DESTINATION_MEMORY;
	request.Options.CompressionFormat = (p_child->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)
			? DSTORAGE_COMPRESSION_FORMAT_GDEFLATE
			: DSTORAGE_COMPRESSION_FORMAT_NONE;

	request.Source.File.Source = file;
	request.Source.File.Offset = p_child->get_offset();
	request.Source.File.Size = src_size;

	if (gpu_buffer_destination) {
		request.Destination.Buffer.Resource = destination_buffer;
		request.Destination.Buffer.Offset = p_destination_offset;
		request.Destination.Buffer.Size = uncompressed_size;
	} else if (gpu_texture_destination) {
		request.Destination.MultipleSubresourcesRange.Resource = destination_texture;
		request.Destination.MultipleSubresourcesRange.FirstSubresource = p_child->get_first_subresource();
		request.Destination.MultipleSubresourcesRange.NumSubresources = p_child->get_subresource_count();
	} else {
		p_child->ensure_destination_buffer_capacity(uncompressed_size);

		// Transition status to STATUS_SUBMITTED before acquiring the writable pointer
		// so is_mutable() returns false and prevents any concurrent script mutations/reallocations.
		p_child->set_status(DSRequest::STATUS_SUBMITTED);
		request.Destination.Memory.Buffer = p_child->get_destination_buffer_ptrw();
		request.Destination.Memory.Size = uncompressed_size;
	}
	request.UncompressedSize = uncompressed_size;
	request.CancellationTag = p_child->get_cancellation_tag();

	ID3D12Resource *resource_guard = gpu_buffer_destination ? destination_buffer : destination_texture;
	if (resource_guard) {
		resource_guard->AddRef();
		s_resource_guards[p_child.ptr()] = resource_guard;
	}
	if (gpu_texture_destination && destination_texture) {
		GPUTargetTransition transition;
		transition.resource = destination_texture;
		transition.original_state = D3D12_RESOURCE_STATE_COMMON;
		transition.first_subresource = p_child->get_first_subresource();
		transition.subresource_count = p_child->get_subresource_count();
		slot.gpu_transitions.push_back(transition);
	}
	if (gpu_buffer_destination || gpu_texture_destination) {
		p_child->set_gpu_in_flight(true);
		p_child->set_status(DSRequest::STATUS_SUBMITTED);
	}
	slot.requests.push_back(p_child);
	queue.queue->EnqueueRequest(&request);
	// slot.files push happens once at open time in the cache-miss branch
	// above; cache hits reuse the already-tracked handle.
	return true;
	};

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

	DSContext *ctx = DSContext::get_singleton();
	uint64_t staging_size = ctx ? static_cast<uint64_t>(ctx->get_staging_buffer_size_mb()) * 1024ULL * 1024ULL : 0;
	uint64_t request_size = MAX(static_cast<uint64_t>(src_size), static_cast<uint64_t>(uncompressed_size));
	Vector<DSTexMipEntryV2> texture_mip_entries;
	bool gpu_texture_gdeflate = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE &&
			p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE;
	bool has_texture_mip_entries = gpu_texture_gdeflate && get_texture_mip_entries(p_request->get_file_path(), p_request->get_offset(), texture_mip_entries);
	uint64_t texture_footprint = 0;
	uint32_t texture_first_subresource = p_request->get_first_subresource();
	uint32_t texture_subresource_count = p_request->get_subresource_count();
	if (has_texture_mip_entries) {
		if (texture_first_subresource >= static_cast<uint32_t>(texture_mip_entries.size()) ||
				texture_subresource_count == 0 ||
				texture_subresource_count > static_cast<uint32_t>(texture_mip_entries.size()) - texture_first_subresource) {
			has_texture_mip_entries = false;
		} else {
			for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
				const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
				if (mip_entry.compressed_size == 0 || mip_entry.uncompressed_size == 0 ||
						texture_footprint > UINT64_MAX - mip_entry.uncompressed_size) {
					has_texture_mip_entries = false;
					break;
				}
				texture_footprint += mip_entry.uncompressed_size;
			}
		}
	}

	if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE && !has_texture_mip_entries) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend: GPU texture destinations require v2 footprint-padded formats.");
		return false;
	}

	if (gpu_texture_gdeflate && has_texture_mip_entries && texture_subresource_count > 1) {
		// Multi-mip .dstex files store each mip level as an independent GDeflate compressed stream.
		// DirectStorage's MultipleSubresourcesRange destination for multiple subresources expects a single continuous stream,
		// so multi-mip textures MUST be split into per-mip requests using DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE with NumSubresources = 1.
		// STAGING INVARIANT (multi-mip): textures split per-mip, so the whole-texture
		// footprint MAY exceed staging_size, but EACH mip must independently satisfy
		// MAX(compressed_size, uncompressed_size) <= staging_size (enforced per-mip
		// in the loop below). A mip larger than staging cannot be split further
		// without corrupting its GDeflate tile stream, so it fails loudly here.
		if (staging_size > 0 && texture_footprint > staging_size && staging_size < GDEFLATE_PAGE_SIZE) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage cannot split a GDeflate texture with the configured staging buffer.");
			return false;
		}

		if (staging_size > 0 && texture_footprint > staging_size) {
			std::string warning_key = p_request->get_file_path().utf8().get_data();
			if (s_staging_buffer_warnings.insert(warning_key).second) {
				UtilityFunctions::push_warning(
						"[DirectStorage] Texture '", p_request->get_file_path(), "' uncompressed footprint (",
						String::num_uint64(texture_footprint / (1024 * 1024)), " MiB) exceeds DirectStorage staging buffer size (",
						String::num_uint64(staging_size / (1024 * 1024)), " MiB). Splitting into per-mip requests. "
						"To eliminate per-mip request overhead, consider increasing Project Settings > DirectStorage > Staging Buffer Size.");
			}
		}

		DSChunkedRequestState state;
		state.parent = p_request;
		for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
			const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
			if (staging_size > 0 && (mip_entry.compressed_size > staging_size || mip_entry.uncompressed_size > staging_size ||
					mip_entry.offset > UINT64_MAX - mip_entry.compressed_size)) {
				p_request->set_status(DSRequest::STATUS_FAILED);
				p_request->set_error_message("DirectStorage GDeflate payload exceeds staging buffer capacity; increase directstorage/staging_buffer_size_mb");
				return false;
			}

			Ref<DSRequest> child;
			child.instantiate();
			child->set_file_path(p_request->get_file_path());
			child->set_offset(mip_entry.offset);
			child->set_size(mip_entry.compressed_size);
			child->set_uncompressed_size(mip_entry.uncompressed_size);
			child->set_compression_format(DSRequest::COMPRESSION_GDEFLATE);
			child->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
			child->set_priority(p_request->get_priority());
			child->set_cancellation_tag(p_request->get_cancellation_tag());
			child->set_destination_resource(p_request->get_destination_resource());
			child->set_first_subresource(texture_first_subresource + mip_index);
			child->set_subresource_count(1);
			child->set_texture_format(p_request->get_texture_format());
			child->set_texture_layout(p_request->get_texture_layout());
			if (!enqueue_one(child, 0)) {
				p_request->set_status(DSRequest::STATUS_FAILED);
				p_request->set_error_message("DirectStorage failed to enqueue a GDeflate texture mip.");
				return false;
			}
			state.children.push_back(child);
			state.destination_offsets.push_back(0);
			s_chunk_parents[child.ptr()] = p_request.ptr();
		}
		s_chunked_requests[p_request.ptr()] = state;
		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
	}
	if (gpu_texture_gdeflate && has_texture_mip_entries && texture_subresource_count == 1) {
		// STAGING INVARIANT (single-mip): a single mip is one indivisible GDeflate
		// tile stream and cannot be split across chunks by DirectStorage without
		// corrupting it, so it requires MAX(compressed_size, uncompressed_size)
		// <= staging_size (enforced below). Oversize single-mip payloads fail
		// loudly instead of being chunked.
		const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource];
		if (staging_size > 0 && (mip_entry.compressed_size > staging_size || mip_entry.uncompressed_size > staging_size)) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage GDeflate payload exceeds staging buffer capacity; increase directstorage/staging_buffer_size_mb");
			return false;
		}
		return enqueue_one(p_request, 0);
	}

	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE && staging_size > 0 && request_size > staging_size) {
		// Non-texture (or non-GPU-texture) GDeflate payloads larger than staging may
		// be page-chunked below; GPU-texture destinations never reach here chunked —
		// they are governed by the single- vs multi-mip staging invariant above
		// (single-mip no-split, multi-mip per-mip limit) and fail loudly instead.
		if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE || staging_size < GDEFLATE_PAGE_SIZE) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage cannot split this oversized GDeflate destination with the configured staging buffer.");
			return false;
		}

		uint64_t chunk_capacity = staging_size - (staging_size % GDEFLATE_PAGE_SIZE);
		Vector<DSChunkBoundary> dstex_chunks;
		bool use_dstex_chunks = get_dstex_mip_chunks(p_request->get_file_path(), p_request->get_offset(), src_size, chunk_capacity, dstex_chunks);
		if (!use_dstex_chunks) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage GDeflate payload exceeds staging buffer capacity; increase directstorage/staging_buffer_size_mb");
			return false;
		}
		uint64_t chunk_count = dstex_chunks.size();
		DSChunkedRequestState state;
		state.parent = p_request;
		for (uint64_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
			uint64_t src_begin = 0;
			uint64_t src_end = 0;
			uint64_t destination_begin = 0;
			uint64_t destination_end = 0;
			const DSChunkBoundary &boundary = dstex_chunks[chunk_index];
			src_begin = boundary.source_offset;
			src_end = boundary.source_offset + boundary.source_size;
			destination_begin = boundary.destination_offset;
			destination_end = boundary.destination_offset + boundary.destination_size;
			if (src_end <= src_begin || destination_end <= destination_begin || src_end - src_begin > chunk_capacity || destination_end - destination_begin > chunk_capacity) {
				p_request->set_status(DSRequest::STATUS_FAILED);
				p_request->set_error_message("DirectStorage GDeflate payload exceeds staging buffer capacity; increase directstorage/staging_buffer_size_mb");
				return false;
			}

			Ref<DSRequest> child;
			child.instantiate();
			child->set_file_path(p_request->get_file_path());
			child->set_offset(p_request->get_offset() + src_begin);
			child->set_size(static_cast<uint32_t>(src_end - src_begin));
			child->set_uncompressed_size(static_cast<uint32_t>(destination_end - destination_begin));
			child->set_compression_format(p_request->get_compression_format());
			child->set_destination_type(p_request->get_destination_type());
			child->set_priority(p_request->get_priority());
			child->set_cancellation_tag(p_request->get_cancellation_tag());
			child->set_destination_resource(p_request->get_destination_resource());
			child->set_first_subresource(p_request->get_first_subresource());
			child->set_subresource_count(p_request->get_subresource_count());
			child->set_texture_format(p_request->get_texture_format());
			child->set_texture_layout(p_request->get_texture_layout());
			child->get_destination_buffer_ref().resize(static_cast<int64_t>(destination_end - destination_begin));
			if (!enqueue_one(child, destination_begin)) {
				p_request->set_status(DSRequest::STATUS_FAILED);
				p_request->set_error_message("DirectStorage failed to enqueue an oversized GDeflate chunk.");
				return false;
			}
			state.children.push_back(child);
			state.destination_offsets.push_back(destination_begin);
			s_chunk_parents[child.ptr()] = p_request.ptr();
		}
		s_chunked_requests[p_request.ptr()] = state;
		if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER ||
				p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE) {
			p_request->set_gpu_in_flight(true);
		}
		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
	}

	return enqueue_one(p_request, 0);
#else
	return false;
#endif
}

void Win32Backend::submit_async() {
#ifdef _WIN32
	if (!accepting_requests.load() || sync_batch_in_flight.load()) {
		return;
	}
	int submit_slot_index = active_batch_slot.load();
	Vector<GPUTargetTransition> pre_transitions;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		bool has_work = false;
		for (const QueueState &queue : priority_queues) {
			const BatchSlot &slot = queue.batch_slots[submit_slot_index];
			if (slot.submitted || slot.closing || slot.completion_ready) {
				return;
			}
			if (slot.requests.size() > 0) {
				has_work = true;
			}
			for (int t = 0; t < slot.gpu_transitions.size(); t++) {
				pre_transitions.push_back(slot.gpu_transitions[t]);
			}
		}
		if (!has_work) {
			return;
		}
		for (QueueState &queue : priority_queues) {
			queue.batch_slots[submit_slot_index].closing = true;
		}
		active_batch_slot.store(1 - submit_slot_index);
	}

	if (!pre_transitions.is_empty()) {
		if (!execute_transitions(pre_transitions, true)) {
			UtilityFunctions::push_error("[DirectStorage] Failed to transition GPU destination textures to D3D12_RESOURCE_STATE_COMMON; aborting batch submit.");
			std::lock_guard<std::mutex> lock(batch_mutex);
			for (QueueState &queue : priority_queues) {
				BatchSlot &slot = queue.batch_slots[submit_slot_index];
				for (int i = 0; i < slot.requests.size(); i++) {
					if (slot.requests[i].is_valid()) {
						slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
						slot.requests[i]->set_error_message("GPU resource barrier transition failed.");
						slot.requests[i]->set_gpu_in_flight(false);
					}
				}
				complete_chunked_requests();
				release_resource_guards(slot.requests);
				close_slot_files(slot);
				slot.requests.clear();
				slot.gpu_transitions.clear();
				slot.closing = false;
				slot.submitted = false;
				slot.completion_ready = false;
			}
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return;
		}
	}
	std::deque<BatchMetadata> submitted_batches;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		BatchSlot &slot = queue.batch_slots[submit_slot_index];
		slot.closing = false;
		if (!queue.queue || slot.requests.size() == 0) {
			continue;
		}
		if (slot.submitted || slot.completion_ready) {
			continue;
		}

		BatchMetadata batch;
		batch.queue_index = queue_index;
		batch.slot_index = submit_slot_index;
		batch.start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
		batch.request_count = count_logical_requests(slot.requests);
		// Per-batch tag accounting: group logical requests by
		// cancellation_tag. Chunk children resolve to their logical parent
		// via get_logical_request (same tag) and count once, matching
		// count_logical_requests(). Untagged sync requests land under tag 0.
		{
			std::unordered_map<uint64_t, int> tag_totals;
			std::unordered_set<DSRequest *> seen_logical;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_null()) {
					continue;
				}
				DSRequest *logical = get_logical_request(slot.requests[i].ptr());
				if (!logical || !seen_logical.insert(logical).second) {
					continue;
				}
				tag_totals[slot.requests[i]->get_cancellation_tag()]++;
			}
			for (std::unordered_map<uint64_t, int>::const_iterator tag_it = tag_totals.begin(); tag_it != tag_totals.end(); ++tag_it) {
				BatchMetadata::BatchTagCount tag_count;
				tag_count.tag = tag_it->first;
				tag_count.request_count = tag_it->second;
				batch.tag_counts.push_back(tag_count);
			}
		}
		for (int i = 0; i < slot.requests.size(); i++) {
			if (slot.requests[i].is_valid()) {
				uint32_t request_size = slot.requests[i]->get_size();
				uint32_t uncompressed_size = slot.requests[i]->get_uncompressed_size();
				batch.compressed_bytes += request_size;
				batch.uncompressed_bytes += uncompressed_size > 0 ? uncompressed_size : request_size;
			}
		}

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->clear_error();
		}
		uint64_t completion_value = queue.completion_fence_value + 1;
		ResetEvent(slot.event);

		queue.completion_fence_value = completion_value;
		batch.fence_value = completion_value;
		slot.fence_value = completion_value;
		slot.submitted = true;
		slot.completion_ready = false;
		slot.error_code = 0;
		slot.error_message = String();
		queue.queue->EnqueueSignal(queue.completion_fence, completion_value);
		queue.queue->EnqueueSetEvent(slot.event);
		queue.queue->Submit();
		submitted_batches.push_back(batch);
	}

	for (BatchMetadata &batch : submitted_batches) {
		completion_queue.push_back(batch);
	}
	batch_in_flight.store(has_in_flight_batches_locked());
	update_profiler_queue_depth_locked();
	batch_cv.notify_one();
	}
#endif
}

void Win32Backend::collect_completed_results() {
#ifdef _WIN32
	uint64_t fence_value = 0;
	int completed_count = 0;
	int error_code = 0;
	String error_message;
	while (collect_completed_batch(fence_value, completed_count, error_code, error_message)) {
	}
#endif
}

bool Win32Backend::has_pending_results() {
#ifdef _WIN32
	poll_batch_status();
	std::lock_guard<std::mutex> lock(batch_mutex);
	int active_slot_index = active_batch_slot.load();
	for (const QueueState &queue : priority_queues) {
		if (queue.batch_slots[1 - active_slot_index].completion_ready || queue.batch_slots[active_slot_index].completion_ready) {
			return true;
		}
	}
#endif
	return false;
}

bool Win32Backend::collect_completed_batch(uint64_t &r_fence_value, int &r_completed_count, int &r_error_code, String &r_error_message) {
#ifdef _WIN32
	poll_batch_status();
	std::lock_guard<std::mutex> lock(batch_mutex);
	int active_slot_index = active_batch_slot.load();
	for (int pass = 0; pass < 2; pass++) {
		int slot_index = pass == 0 ? 1 - active_slot_index : active_slot_index;
		for (QueueState &queue : priority_queues) {
			BatchSlot &slot = queue.batch_slots[slot_index];
			if (!slot.completion_ready) {
				continue;
			}
			r_fence_value = slot.fence_value;
			r_completed_count = slot.completed_count;
			r_error_code = slot.error_code;
			r_error_message = slot.error_message;
			close_slot_files(slot);
			slot.requests.clear();
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.completion_ready = false;
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return true;
		}
	}
#endif
	return false;
}

void Win32Backend::submit_batch() {
#ifdef _WIN32
	if (!accepting_requests.load()) {
		return;
	}
	int submit_slot_index = -1;
	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		if (!batch_cv.wait_for(lock, std::chrono::milliseconds(5000), [this]() {
				return !has_in_flight_batches_locked() && !sync_batch_in_flight.load();
			})) {
			UtilityFunctions::push_error("[DirectStorage] Timed out waiting for the previous batch to complete.");
			return;
		}
		sync_batch_in_flight.store(true);
		submit_slot_index = active_batch_slot.load();
		auto slot_is_available = [this](int p_slot_index) {
			for (const QueueState &queue : priority_queues) {
				const BatchSlot &slot = queue.batch_slots[p_slot_index];
				if (slot.submitted || slot.completion_ready) {
					return false;
				}
			}
			return true;
		};
		auto reclaim_slot_if_completed = [this](int p_slot_index) {
			for (QueueState &queue : priority_queues) {
				BatchSlot &slot = queue.batch_slots[p_slot_index];
				if (!slot.submitted && slot.completion_ready) {
					close_slot_files(slot);
					slot.requests.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = 0;
					slot.error_message = String();
					slot.completion_ready = false;
				}
			}
		};
		reclaim_slot_if_completed(submit_slot_index);
		if (!slot_is_available(submit_slot_index)) {
			reclaim_slot_if_completed(1 - submit_slot_index);
			if (slot_is_available(1 - submit_slot_index)) {
				submit_slot_index = 1 - submit_slot_index;
				active_batch_slot.store(submit_slot_index);
			}
		}
		if (!slot_is_available(submit_slot_index)) {
			sync_batch_in_flight.store(false);
			UtilityFunctions::push_error("[DirectStorage] No batch slot is available for synchronous submission.");
			batch_cv.notify_all();
			return;
		}
		update_profiler_queue_depth_locked();
	}

	Vector<GPUTargetTransition> pre_transitions;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			BatchSlot &slot = priority_queues[queue_index].batch_slots[submit_slot_index];
			slot.closing = true;
			for (int t = 0; t < slot.gpu_transitions.size(); t++) {
				pre_transitions.push_back(slot.gpu_transitions[t]);
			}
		}
		active_batch_slot.store(1 - submit_slot_index);
	}
	if (!pre_transitions.is_empty()) {
		if (!execute_transitions(pre_transitions, true)) {
			UtilityFunctions::push_error("[DirectStorage] Failed to transition GPU destination textures to D3D12_RESOURCE_STATE_COMMON; aborting batch submit.");
			std::lock_guard<std::mutex> lock(batch_mutex);
			for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
				BatchSlot &slot = priority_queues[queue_index].batch_slots[submit_slot_index];
				for (int i = 0; i < slot.requests.size(); i++) {
					if (slot.requests[i].is_valid()) {
						slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
						slot.requests[i]->set_error_message("GPU resource barrier transition failed.");
						slot.requests[i]->set_gpu_in_flight(false);
					}
				}
				complete_chunked_requests();
				release_resource_guards(slot.requests);
				close_slot_files(slot);
				slot.requests.clear();
				slot.gpu_transitions.clear();
				slot.closing = false;
				slot.submitted = false;
				slot.completion_ready = false;
			}
			sync_batch_in_flight.store(false);
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return;
		}
	}

	Vector<int> submitted_queues;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			BatchSlot &slot = queue.batch_slots[submit_slot_index];
			slot.closing = false;
			if (!queue.queue || !queue.completion_fence || slot.requests.size() == 0) {
				continue;
			}
			uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->clear_error();
			}
			int request_count = count_logical_requests(slot.requests);
			uint64_t total_compressed_bytes = 0;
			uint64_t total_uncompressed_bytes = 0;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					uint32_t req_size = slot.requests[i]->get_size();
					uint32_t req_unc_size = slot.requests[i]->get_uncompressed_size();
					total_compressed_bytes += req_size;
					total_uncompressed_bytes += req_unc_size > 0 ? req_unc_size : req_size;
				}
			}
			BatchMetadata batch;
			batch.queue_index = queue_index;
			batch.slot_index = submit_slot_index;
			batch.start_time = start_time;
			batch.compressed_bytes = total_compressed_bytes;
			batch.uncompressed_bytes = total_uncompressed_bytes;
			batch.request_count = request_count;
			uint64_t completion_value = queue.completion_fence_value + 1;
			ResetEvent(slot.event);
			queue.completion_fence_value = completion_value;
			batch.fence_value = completion_value;
			slot.fence_value = completion_value;
			slot.submitted = true;
			slot.completion_ready = false;
			slot.error_code = 0;
			slot.error_message = String();
			queue.queue->EnqueueSignal(queue.completion_fence, completion_value);
			queue.queue->EnqueueSetEvent(slot.event);
			queue.queue->Submit();
			completion_queue.push_back(batch);
			submitted_queues.push_back(queue_index);
		}
		if (submitted_queues.is_empty()) {
			sync_batch_in_flight.store(false);
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return;
		}
		batch_in_flight.store(!completion_queue.empty());
		update_profiler_queue_depth_locked();
	}
	batch_cv.notify_one();

	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		bool completed = batch_cv.wait_for(lock, std::chrono::milliseconds(5000), [this, submit_slot_index, &submitted_queues]() {
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				const BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				if (slot.submitted || !slot.completion_ready) {
					return false;
				}
			}
			return true;
		});

		if (completed) {
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				slot.requests.clear();
				slot.gpu_transitions.clear();
				slot.fence_value = 0;
				slot.completed_count = 0;
				slot.error_code = 0;
				slot.error_message = String();
				slot.closing = false;
				slot.completion_ready = false;
			}
		} else {
			UtilityFunctions::push_error("[DirectStorage] Win32Backend priority queue submit wait timed out or failed.");
			for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ) {
				if (it->slot_index == submit_slot_index) {
					it = completion_queue.erase(it);
				} else {
					++it;
				}
			}
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				if (slot.submitted || !slot.completion_ready) {
					std::unordered_set<uint64_t> tags;
					for (int r = 0; r < slot.requests.size(); r++) {
						if (slot.requests[r].is_valid()) {
							tags.insert(slot.requests[r]->get_cancellation_tag());
							slot.requests[r]->set_status(DSRequest::STATUS_FAILED);
							slot.requests[r]->set_error_message("Batch submission timed out.");
						}
					}
					if (priority_queues[q_idx].queue) {
						for (uint64_t tag : tags) {
							priority_queues[q_idx].queue->CancelRequestsWithTag(UINT64_MAX, tag);
						}
					}
					ZombieBatchSlot zombie;
					zombie.queue_index = q_idx;
					zombie.fence_value = slot.fence_value;
					zombie.requests = slot.requests;
					zombie.files = slot.files;
					zombie.gpu_transitions = slot.gpu_transitions;
					zombie_slots.push_back(zombie);

					slot.requests.clear();
					slot.files.clear();
					slot.file_cache.clear();
					slot.gpu_transitions.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = -1;
					slot.error_message = "Batch submission timed out.";
					slot.closing = false;
					slot.submitted = false;
					slot.completion_ready = false;
				} else {
					close_slot_files(slot);
					slot.requests.clear();
					slot.gpu_transitions.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = 0;
					slot.error_message = String();
					slot.closing = false;
					slot.completion_ready = false;
				}
			}
		}
		sync_batch_in_flight.store(false);
		batch_in_flight.store(has_in_flight_batches_locked());
		update_profiler_queue_depth_locked();
	}
	batch_cv.notify_all();
	return;

#endif
}

void Win32Backend::cancel_requests_with_tag(uint64_t p_tag) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	for (QueueState &queue : priority_queues) {
		if (queue.queue) {
			queue.queue->CancelRequestsWithTag(UINT64_MAX, p_tag);
		}
		for (BatchSlot &slot : queue.batch_slots) {
			Vector<Ref<DSRequest>> remaining;
			Vector<Ref<DSRequest>> dropped;
			uint64_t now_usec = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid() && slot.requests[i]->get_cancellation_tag() == p_tag) {
					slot.requests[i]->set_cancel_timestamp_usec(now_usec);
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message("Request cancelled by tag.");
					if (slot.submitted) {
						// In-flight batch: keep the request in the slot so the
						// completion thread tears it down uniformly (guard
						// release, file close, counts). It is already terminal
						// FAILED, so poll_batch_status() leaves it dropped and
						// never promotes it to COMPLETED.
						// gpu_in_flight remains true until the batch fence retires.
						remaining.push_back(slot.requests[i]);
					} else {
						// Never submitted: drop from the slot so a later
						// Submit() cannot pick up a cancelled request. The
						// DirectStorage-level request was already cancelled
						// above, so the GPU will never write the destination:
						// release the AddRef'd guard (no leak, freed RIDs stay
						// unwritten) and detach chunk tracking. Parent
						// reconciliation runs below via complete_chunked_requests().
						slot.requests[i]->set_gpu_in_flight(false);
						dropped.push_back(slot.requests[i]);
					}
				} else {
					remaining.push_back(slot.requests[i]);
				}
			}
			slot.requests = remaining;
			if (!dropped.is_empty()) {
				// HIGH-07 guard: purge transitions whose requests were dropped
				Vector<GPUTargetTransition> remaining_transitions;
				std::unordered_set<ID3D12Resource *> remaining_resources;
				for (int r = 0; r < remaining.size(); r++) {
					if (remaining[r].is_valid()) {
						std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard_it = s_resource_guards.find(remaining[r].ptr());
						if (guard_it != s_resource_guards.end()) {
							remaining_resources.insert(guard_it->second);
						}
					}
				}
				for (int t = 0; t < slot.gpu_transitions.size(); t++) {
					if (remaining_resources.count(slot.gpu_transitions[t].resource.Get()) > 0) {
						remaining_transitions.push_back(slot.gpu_transitions[t]);
					}
				}
				slot.gpu_transitions = remaining_transitions;

				release_resource_guards(dropped);
				for (int i = 0; i < dropped.size(); i++) {
					if (dropped[i].is_valid()) {
						s_chunk_parents.erase(dropped[i].ptr());
					}
				}
			}
		}
	}
	// Reconcile chunked parents whose children all went terminal via the
	// drops above (parent marked FAILED with the child cancel message; maps
	// released). Touches only Ref statuses and memcpys for memory
	// destinations — no render/scene APIs, safe on the calling thread under
	// batch_mutex (same discipline as every other call site).
	complete_chunked_requests();
#endif
}

} // namespace godot
