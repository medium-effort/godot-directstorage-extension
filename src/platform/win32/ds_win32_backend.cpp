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
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <winioctl.h>
#endif

#include <string>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

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
		if (header.magic != 0x58455444 || header.version != 2 || header.mip_count == 0 ||
				header.mip_count > (std::numeric_limits<uint64_t>::max() - sizeof(DSTexHeader)) / sizeof(DSTexMipEntryV2)) {
			return false;
		}

		uint64_t entries_end = sizeof(DSTexHeader) + static_cast<uint64_t>(header.mip_count) * sizeof(DSTexMipEntryV2);
		if (entries_end > file->get_length()) {
			return false;
		}
		r_entries.resize(header.mip_count);
		for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
			PackedByteArray entry_data = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (entry_data.size() != static_cast<int64_t>(sizeof(DSTexMipEntryV2))) {
				r_entries.clear();
				return false;
			}
			std::memcpy(&r_entries.write[mip_index], entry_data.ptr(), sizeof(DSTexMipEntryV2));
			const DSTexMipEntryV2 &entry = r_entries[mip_index];
			if (entry.offset > file->get_length() || entry.compressed_size > file->get_length() - entry.offset) {
				r_entries.clear();
				return false;
			}
		}
		return true;
	}

	if (ext == "dspkg") {
		Ref<FileAccess> file = FileAccess::open(resolved_path, FileAccess::READ);
		if (file.is_null() || file->get_length() < sizeof(DSPkgHeader)) {
			return false;
		}

		PackedByteArray header_data = file->get_buffer(sizeof(DSPkgHeader));
		if (header_data.size() != static_cast<int64_t>(sizeof(DSPkgHeader))) {
			return false;
		}
		DSPkgHeader header = {};
		std::memcpy(&header, header_data.ptr(), sizeof(header));
		if (header.magic != 0x474B5044 || header.version != 2 || (header.flags & 1) == 0 || header.entry_count == 0) {
			return false;
		}

		uint64_t file_len = file->get_length();
		for (uint32_t entry_idx = 0; entry_idx < header.entry_count; entry_idx++) {
			if (file->get_position() + 10 > file_len) {
				return false;
			}
			uint64_t hash = file->get_64();
			uint16_t path_len = file->get_16();
			if (file->get_position() + path_len + 40 > file_len) {
				return false;
			}
			file->seek(file->get_position() + path_len);
			uint64_t entry_data_offset = file->get_64();
			uint64_t comp_size = file->get_64();
			uint64_t orig_size = file->get_64();
			uint32_t width = file->get_32();
			uint32_t height = file->get_32();
			uint32_t pixel_format = file->get_32();
			uint32_t mip_count = file->get_32();

			uint64_t abs_entry_data_offset = header.data_offset + entry_data_offset;
			if (abs_entry_data_offset == p_offset) {
				if (mip_count == 0 || file->get_position() + mip_count * sizeof(DSPkgMipEntry) > file_len) {
					return false;
				}
				r_entries.resize(mip_count);
				for (uint32_t mip_idx = 0; mip_idx < mip_count; mip_idx++) {
					DSPkgMipEntry pkg_mip = {};
					PackedByteArray mip_data = file->get_buffer(sizeof(DSPkgMipEntry));
					if (mip_data.size() != static_cast<int64_t>(sizeof(DSPkgMipEntry))) {
						r_entries.clear();
						return false;
					}
					std::memcpy(&pkg_mip, mip_data.ptr(), sizeof(DSPkgMipEntry));
					r_entries.write[mip_idx].offset = pkg_mip.offset;
					r_entries.write[mip_idx].compressed_size = pkg_mip.compressed_size;
					r_entries.write[mip_idx].uncompressed_size = pkg_mip.uncompressed_size;
					r_entries.write[mip_idx].row_pitch = pkg_mip.row_pitch;
					r_entries.write[mip_idx].reserved = pkg_mip.reserved;
				}
				return true;
			} else {
				file->seek(file->get_position() + mip_count * sizeof(DSPkgMipEntry));
			}
		}
		return false;
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

static bool validate_resource_guard(const Ref<DSRequest> &p_request, String &r_error) {
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

	auto resource_type = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER
			? RenderingDevice::DRIVER_RESOURCE_BUFFER
			: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
	ID3D12Resource *current_resource = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(resource_type, destination_rid, 0));
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

		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_valid()) {
				s_chunk_parents.erase(state.children[i].ptr());
			}
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

bool Win32Backend::is_batch_complete() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	return completion_queue.empty();
#else
	return true;
#endif
}

uint64_t Win32Backend::get_completion_fence() const {
#ifdef _WIN32
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
	ULONGLONG start_time = GetTickCount64();
	for (QueueState &queue : priority_queues) {
		if (!queue.completion_fence || queue.completion_fence_value == 0) {
			continue;
		}
		uint64_t completed_value = queue.completion_fence->GetCompletedValue();
		if (completed_value == UINT64_MAX) {
			return false;
		}
		if (completed_value < queue.completion_fence_value) {
			if (FAILED(queue.completion_fence->SetEventOnCompletion(queue.completion_fence_value, queue.completion_event))) {
				return false;
			}
			ULONGLONG elapsed = GetTickCount64() - start_time;
			DWORD remaining = elapsed >= p_timeout_msec ? 0 : static_cast<DWORD>(p_timeout_msec - elapsed);
			if (WaitForSingleObject(queue.completion_event, remaining) != WAIT_OBJECT_0) {
				return false;
			}
			completed_value = queue.completion_fence->GetCompletedValue();
		}
		if (completed_value < queue.completion_fence_value || completed_value == UINT64_MAX) {
			return false;
		}
		queue.completed_fence_value = completed_value;
	}
	return true;
#else
	return p_fence_value == 0;
#endif
}

void Win32Backend::poll_batch_status() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	int inactive_slot = 1 - active_batch_slot.load();
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		BatchSlot &slot = queue.batch_slots[inactive_slot];
		if (!slot.submitted || slot.completion_ready || !queue.completion_fence || slot.fence_value == 0 ||
				queue.completion_fence->GetCompletedValue() < slot.fence_value) {
			continue;
		}
		for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ++it) {
			if (it->queue_index == queue_index && it->slot_index == inactive_slot) {
				BatchMetadata batch = *it;
				completion_queue.erase(it);
				complete_batch_slot(queue, slot, batch);
				break;
			}
		}
	}
	batch_in_flight.store(!completion_queue.empty());
	batch_cv.notify_all();
#endif
}

void Win32Backend::poll_batch_status(QueueState &p_queue, Vector<Ref<DSRequest>> &p_requests, uint64_t p_fence_value) {
#ifdef _WIN32
	if (!p_queue.completion_fence || p_fence_value == 0 ||
			p_queue.completion_fence->GetCompletedValue() < p_fence_value) {
		return;
	}

	p_queue.completed_fence_value = p_queue.completion_fence->GetCompletedValue();
	String queue_error = retrieve_queue_error(p_queue.queue);

	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid()) {
			String resource_error;
			if (!validate_resource_guard(p_requests[i], resource_error)) {
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
#endif
}

#ifdef _WIN32
void Win32Backend::complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &p_batch) {
	poll_batch_status(p_queue, p_slot.requests, p_slot.fence_value);
	p_slot.completed_count = p_batch.request_count;
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
	release_resource_guards(p_slot.requests);
	close_batch_files(p_slot.files);
	p_slot.submitted = false;
	p_slot.completion_ready = true;
}
#endif

#ifdef _WIN32
void Win32Backend::close_batch_files(Vector<IDStorageFile *> &p_files) {
	for (int i = 0; i < p_files.size(); i++) {
		if (p_files[i]) {
			p_files[i]->Close();
			p_files[i]->Release();
		}
	}
	p_files.clear();
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
			close_batch_files(slot.files);
			complete_chunked_requests();
			release_resource_guards(slot.requests);
			slot.completed_count = batch.request_count;
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

		DWORD wait_result = WaitForMultipleObjects(QUEUE_COUNT * BATCH_SLOT_COUNT + 1, wait_handles, FALSE, INFINITE);
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
			complete_batch_slot(priority_queues[batch.queue_index], priority_queues[batch.queue_index].batch_slots[batch.slot_index], batch);
			batch_in_flight.store(!completion_queue.empty());
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
static ID3D12Device *s_d3d12_device = nullptr;
static std::mutex s_device_mutex;

ID3D12Device *ds_win32_get_d3d12_device() {
	std::lock_guard<std::mutex> lock(s_device_mutex);
	if (s_d3d12_device) {
		return s_d3d12_device;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs && rs->get_current_rendering_driver_name().to_lower() == "d3d12") {
		RenderingDevice *rd = rs->get_rendering_device();
		if (rd && rs->is_on_render_thread()) {
			uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
			if (handle != 0) {
				s_d3d12_device = reinterpret_cast<ID3D12Device *>(handle);
				return s_d3d12_device;
			}
		}
	}

	return nullptr;
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
	unload_runtime_modules(core, dstorage);
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

int Win32Backend::priority_to_queue_index(DSRequest::DSPriority p_priority) {
	int priority = static_cast<int>(p_priority);
	return priority >= DSRequest::PRIORITY_LOW && priority <= DSRequest::PRIORITY_HIGH ? priority : DSRequest::PRIORITY_NORMAL;
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
	active_batch_slot.store(0);
	for (QueueState &queue : priority_queues) {
		queue.completion_fence_value = 0;
		queue.completed_fence_value = 0;
		for (BatchSlot &slot : queue.batch_slots) {
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
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

		DllDirectoryScope dll_scope;
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
		ID3D12Device *d3d12_device = ds_win32_get_d3d12_device();
		if (!d3d12_device) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not acquire a usable D3D12 device.");
			return false;
		}
		queueDesc.Device = d3d12_device;

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
		completion_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!completion_stop_event) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the completion thread stop event.");
			return false;
		}
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

void Win32Backend::shutdown() {
#ifdef _WIN32
	completion_thread_stop.store(true);
	if (completion_stop_event) {
		SetEvent(completion_stop_event);
	}
	batch_cv.notify_all();
	if (completion_thread.joinable()) {
		completion_thread.join();
	}
	batch_in_flight.store(false);

	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->unregister_queue();
	}

	// C7: Drain GPU work - wait on completion fences with bounded timeout before closing handles.
	for (QueueState &queue : priority_queues) {
		if (queue.completion_fence && queue.completion_fence_value != 0) {
			uint64_t completed = queue.completion_fence->GetCompletedValue();
			if (completed != UINT64_MAX && completed < queue.completion_fence_value && queue.completion_event) {
				if (SUCCEEDED(queue.completion_fence->SetEventOnCompletion(queue.completion_fence_value, queue.completion_event))) {
					WaitForSingleObject(queue.completion_event, 2000);
					queue.completed_fence_value = queue.completion_fence->GetCompletedValue();
				}
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		release_all_resource_guards();
	}

	for (QueueState &queue : priority_queues) {
		for (BatchSlot &slot : queue.batch_slots) {
			close_batch_files(slot.files);
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
	}
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		s_d3d12_device = nullptr;
	}
	active_batch_slot.store(0);
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
	if (p_request.is_null() || !factory) {
		if (p_request.is_valid()) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("Win32Backend not initialized.");
		}
		return false;
	}
	int queue_index = priority_to_queue_index(p_request->get_priority());
	QueueState &queue = priority_queues[queue_index];
	if (!queue.queue) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend priority queue is not initialized.");
		return false;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	BatchSlot &slot = queue.batch_slots[active_batch_slot.load()];
	if (slot.submitted || slot.completion_ready) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("Win32Backend batch slot is not available; collect completed results before enqueueing more requests.");
		return false;
	}

	auto enqueue_one = [&](const Ref<DSRequest> &p_child, uint64_t p_destination_offset) -> bool {
		String path = p_child->get_file_path();
		ProjectSettings *ps = ProjectSettings::get_singleton();
		if (ps && (path.begins_with("res://") || path.begins_with("user://"))) {
			path = ps->globalize_path(path);
		}
		String norm_file_path = path.replace("/", "\\");
		std::wstring wpath(reinterpret_cast<const wchar_t *>(norm_file_path.utf16().get_data()));

		IDStorageFile *file = nullptr;
		HRESULT hr = factory->OpenFile(wpath.c_str(), IID_PPV_ARGS(&file));
		if (FAILED(hr) || !file) {
			p_child->set_status(DSRequest::STATUS_FAILED);
			p_child->set_error_message("Win32Backend: Failed to open file '" + path + "'.");
			return false;
		}

	uint32_t src_size = p_child->get_size();
	uint32_t uncompressed_size = p_child->get_uncompressed_size();
	if (p_child->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = src_size;
	}

	if (src_size == 0 || uncompressed_size == 0) {
		file->Close();
		file->Release();
		p_child->set_status(DSRequest::STATUS_FAILED);
		p_child->set_error_message("Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").");
		return false;
	}

	bool gpu_buffer_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool gpu_texture_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *destination_buffer = nullptr;
	ID3D12Resource *destination_texture = nullptr;
	if (gpu_buffer_destination) {
		String destination_error;
		RenderingServer *rs = RenderingServer::get_singleton();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU buffer destination requires the D3D12 rendering driver.";
		} else {
			RenderingDevice *rd = rs->get_rendering_device();
			RID destination_rid = p_child->get_destination_resource();
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
						if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || p_destination_offset + uncompressed_size > buffer_desc.Width) {
							destination_error = "Win32Backend: GPU buffer destination size exceeds the mapped D3D12 buffer resource.";
						}
					}
				}
		}

		if (!destination_error.is_empty()) {
			file->Close();
			file->Release();
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
		} else {
			RenderingDevice *rd = rs->get_rendering_device();
			RID destination_rid = p_child->get_destination_resource();
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
							uint64_t native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, destination_rid, 0);
							if (native_format == 0 || p_child->get_texture_format() != native_format) {
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
		PackedByteArray &dest = p_child->get_destination_buffer_ref();
		if (dest.size() < static_cast<int64_t>(uncompressed_size)) {
			dest.resize(static_cast<int64_t>(uncompressed_size));
		}

		request.Destination.Memory.Buffer = dest.ptrw();
		request.Destination.Memory.Size = uncompressed_size;
	}
	request.UncompressedSize = uncompressed_size;
	request.CancellationTag = p_child->get_cancellation_tag();

	ID3D12Resource *resource_guard = gpu_buffer_destination ? destination_buffer : destination_texture;
	if (resource_guard) {
		resource_guard->AddRef();
		s_resource_guards[p_child.ptr()] = resource_guard;
	}
	queue.queue->EnqueueRequest(&request);
	slot.files.push_back(file);

	p_child->set_status(DSRequest::STATUS_SUBMITTED);
	slot.requests.push_back(p_child);
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

	if (gpu_texture_gdeflate && has_texture_mip_entries && texture_subresource_count > 1) {
		// Multi-mip .dstex files store each mip level as an independent GDeflate compressed stream.
		// DirectStorage's MultipleSubresourcesRange destination for multiple subresources expects a single continuous stream,
		// so multi-mip textures MUST be split into per-mip requests using DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE with NumSubresources = 1.
		if (staging_size > 0 && texture_footprint > staging_size && staging_size < GDEFLATE_PAGE_SIZE) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("DirectStorage cannot split a GDeflate texture with the configured staging buffer.");
			return false;
		}

		if (staging_size > 0 && texture_footprint > staging_size) {
			std::string warning_key = p_request->get_file_path().utf8().get_data();
			if (s_staging_buffer_warnings.find(warning_key) == s_staging_buffer_warnings.end()) {
				s_staging_buffer_warnings.insert(warning_key);
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
		return enqueue_one(p_request, 0);
	}

	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE && staging_size > 0 && request_size > staging_size) {
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
	std::lock_guard<std::mutex> lock(batch_mutex);
	int submit_slot_index = active_batch_slot.load();
	std::deque<BatchMetadata> submitted_batches;
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		BatchSlot &slot = queue.batch_slots[submit_slot_index];
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
		ResetEvent(queue.completion_event);
		if (FAILED(queue.completion_fence->SetEventOnCompletion(completion_value, queue.completion_event))) {
			UtilityFunctions::push_error("[DirectStorage] Failed to arm the D3D12 completion fence for a priority queue.");
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message("Failed to arm the DirectStorage completion fence.");
				}
			}
			close_batch_files(slot.files);
			complete_chunked_requests();
			release_resource_guards(slot.requests);
			slot.completed_count = batch.request_count;
			slot.error_code = -1;
			slot.error_message = "Failed to arm the DirectStorage completion fence.";
			slot.submitted = false;
			slot.completion_ready = true;
			continue;
		}

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
	if (!submitted_batches.empty()) {
		active_batch_slot.store(1 - submit_slot_index);
	}
	batch_in_flight.store(!completion_queue.empty());
	batch_cv.notify_one();
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
			close_batch_files(slot.files);
			slot.requests.clear();
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.completion_ready = false;
			batch_in_flight.store(!completion_queue.empty());
			batch_cv.notify_all();
			return true;
		}
	}
#endif
	return false;
}

void Win32Backend::submit_batch() {
#ifdef _WIN32
	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		batch_cv.wait(lock, [this]() { return completion_queue.empty(); });
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	int submit_slot_index = active_batch_slot.load();
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		BatchSlot &slot = queue.batch_slots[submit_slot_index];
		if (slot.completion_ready) {
			close_batch_files(slot.files);
			slot.requests.clear();
			slot.completion_ready = false;
			slot.fence_value = 0;
		}
		if (!queue.queue || slot.requests.size() == 0) {
			continue;
		}

		uint64_t total_compressed_bytes = 0;
		uint64_t total_uncompressed_bytes = 0;
		int request_count = count_logical_requests(slot.requests);
		for (int i = 0; i < slot.requests.size(); i++) {
			if (slot.requests[i].is_valid()) {
				uint32_t req_size = slot.requests[i]->get_size();
				uint32_t req_unc_size = slot.requests[i]->get_uncompressed_size();
				total_compressed_bytes += req_size;
				total_uncompressed_bytes += (req_unc_size > 0) ? req_unc_size : req_size;
			}
		}

		uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			err_handler->clear_error();
		}

		uint64_t completion_value = queue.completion_fence_value + 1;
		ResetEvent(slot.event);
		ResetEvent(queue.completion_event);
		bool batch_submitted = SUCCEEDED(queue.completion_fence->SetEventOnCompletion(completion_value, queue.completion_event));
		if (!batch_submitted) {
			UtilityFunctions::push_error("[DirectStorage] Failed to arm the D3D12 completion fence.");
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message("Failed to arm the DirectStorage completion fence.");
				}
			}
			complete_chunked_requests();
		} else {
			queue.completion_fence_value = completion_value;
			slot.fence_value = completion_value;
			queue.queue->EnqueueSignal(queue.completion_fence, completion_value);
			queue.queue->EnqueueSetEvent(slot.event);
			queue.queue->Submit();
			DWORD wait_res = WaitForSingleObject(slot.event, 5000);
			if (wait_res == WAIT_OBJECT_0) {
				poll_batch_status(queue, slot.requests, completion_value);
				uint64_t end_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
				uint64_t duration_us = (end_time > start_time) ? (end_time - start_time) : 1;
				DSProfiler *profiler = DSProfiler::get_singleton();
				if (profiler) {
					ds_profiler_set_batch_metrics(
							total_compressed_bytes,
							total_uncompressed_bytes,
							request_count,
							-1,
							(err_handler && err_handler->has_error()) ? 1 : 0);
					profiler->record_workload(total_compressed_bytes, duration_us);
				}
			} else {
				UtilityFunctions::push_error("[DirectStorage] Win32Backend priority queue submit wait timed out or failed.");
				for (int i = 0; i < slot.requests.size(); i++) {
					if (slot.requests[i].is_valid()) {
						slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
						slot.requests[i]->set_error_message("Batch submission timed out.");
					}
				}
				complete_chunked_requests();
			}
		}
		release_resource_guards(slot.requests);
		close_batch_files(slot.files);
		slot.requests.clear();
		slot.fence_value = 0;
		slot.completed_count = 0;
		slot.error_code = 0;
		slot.error_message = String();
		slot.submitted = false;
		slot.completion_ready = false;
	}
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
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid() && (slot.requests[i]->get_cancellation_tag() & UINT64_MAX) == p_tag) {
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message("Request cancelled by tag.");
					if (slot.submitted) {
						remaining.push_back(slot.requests[i]);
					}
				} else {
					remaining.push_back(slot.requests[i]);
				}
			}
			slot.requests = remaining;
		}
	}
#endif
}

} // namespace godot
