#include "ds_error_handler.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <mutex>

#ifdef _WIN32
#include <strsafe.h>
#include <dstorageerr.h>
#endif

namespace godot {

DSErrorHandler *DSErrorHandler::singleton = nullptr;

DSErrorHandler *DSErrorHandler::get_singleton() {
	return singleton;
}

String DSErrorHandler::describe_command_type(int32_t p_command_type) {
	switch (p_command_type) {
		case -1:
			return "NONE";
		case 0:
			return "REQUEST";
		case 1:
			return "STATUS";
		case 2:
			return "SIGNAL";
		case 3:
			return "EVENT";
		default:
			return String::num_int64(static_cast<int64_t>(p_command_type));
	}
}

String DSErrorHandler::describe_hresult(uint32_t p_hresult) {
#ifdef _WIN32
	switch (static_cast<HRESULT>(p_hresult)) {
		// Standard COM / Win32 / DXGI
		case S_OK:
			return "Operation completed successfully (S_OK).";
		case S_FALSE:
			return "Operation completed with alternate success condition (S_FALSE).";
		case E_INVALIDARG:
			return "The parameter is incorrect (E_INVALIDARG).";
		case E_OUTOFMEMORY:
			return "Ran out of memory (E_OUTOFMEMORY).";
		case E_FAIL:
			return "Unspecified failure (E_FAIL).";
		case E_NOTIMPL:
			return "Not implemented (E_NOTIMPL).";
		case E_POINTER:
			return "Invalid pointer (E_POINTER).";
		case E_UNEXPECTED:
			return "Catastrophic failure (E_UNEXPECTED).";
		case E_ACCESSDENIED:
			return "General access denied (E_ACCESSDENIED).";
		case E_HANDLE:
			return "Invalid handle (E_HANDLE).";
		case E_ABORT:
			return "Operation aborted (E_ABORT).";
		case static_cast<HRESULT>(0x887A0005L): // DXGI_ERROR_DEVICE_REMOVED
			return "The GPU device instance has been suspended or removed (DXGI_ERROR_DEVICE_REMOVED).";
		case static_cast<HRESULT>(0x887A0006L): // DXGI_ERROR_DEVICE_RESET
			return "The GPU device instance was reset (DXGI_ERROR_DEVICE_RESET).";
		case static_cast<HRESULT>(0x887A0007L): // DXGI_ERROR_DEVICE_HUNG
			return "The GPU device failed to respond to further commands (DXGI_ERROR_DEVICE_HUNG).";
		case static_cast<HRESULT>(0x887A0020L): // DXGI_ERROR_DRIVER_INTERNAL_ERROR
			return "The GPU driver encountered an internal error (DXGI_ERROR_DRIVER_INTERNAL_ERROR).";
		case static_cast<HRESULT>(0x887A0001L): // DXGI_ERROR_INVALID_CALL
			return "The application made an invalid API call (DXGI_ERROR_INVALID_CALL).";

		// DirectStorage Error Codes
		case E_DSTORAGE_ALREADY_RUNNING:
			return "DirectStorage is already running exclusively.";
		case E_DSTORAGE_NOT_RUNNING:
			return "DirectStorage is not running.";
		case E_DSTORAGE_INVALID_QUEUE_CAPACITY:
			return "Invalid DirectStorage queue capacity parameter.";
		case E_DSTORAGE_XVD_DEVICE_NOT_SUPPORTED:
			return "The specified XVD is not on a supported NVMe device.";
		case E_DSTORAGE_UNSUPPORTED_VOLUME:
			return "The specified volume is not supported by DirectStorage.";
		case E_DSTORAGE_END_OF_FILE:
			return "The DirectStorage request offset and length exceed the file size.";
		case E_DSTORAGE_REQUEST_TOO_LARGE:
			return "The DirectStorage request exceeds the configured staging buffer.";
		case E_DSTORAGE_ACCESS_VIOLATION:
			return "The DirectStorage destination buffer is not accessible.";
		case E_DSTORAGE_UNSUPPORTED_FILE:
			return "The file is not supported by DirectStorage.";
		case E_DSTORAGE_FILE_NOT_OPEN:
			return "The DirectStorage file handle is unavailable or closed.";
		case E_DSTORAGE_RESERVED_FIELDS:
			return "A DirectStorage reserved field is not set to 0.";
		case E_DSTORAGE_INVALID_BCPACK_MODE:
			return "The request has an invalid BCPack decompression mode.";
		case E_DSTORAGE_INVALID_SWIZZLE_MODE:
			return "The request has an invalid swizzle mode.";
		case E_DSTORAGE_INVALID_DESTINATION_SIZE:
			return "The DirectStorage destination size is invalid for the requested decompression mode.";
		case E_DSTORAGE_QUEUE_CLOSED:
			return "The DirectStorage request targets a queue that is closed.";
		case E_DSTORAGE_INVALID_CLUSTER_SIZE:
			return "The volume is formatted with an unsupported cluster size.";
		case E_DSTORAGE_TOO_MANY_QUEUES:
			return "The number of DirectStorage queues has reached the maximum limit.";
		case E_DSTORAGE_INVALID_QUEUE_PRIORITY:
			return "Invalid priority is specified for the DirectStorage queue.";
		case E_DSTORAGE_TOO_MANY_FILES:
			return "The number of DirectStorage files has reached the maximum limit.";
		case E_DSTORAGE_INDEX_BOUND:
			return "The DirectStorage index parameter is out of bounds.";
		case E_DSTORAGE_IO_TIMEOUT:
			return "The DirectStorage IO operation has timed out.";
		case E_DSTORAGE_INVALID_FILE_HANDLE:
			return "The specified file has not been opened in DirectStorage.";
		case E_DSTORAGE_INVALID_FILE_OFFSET:
			return "The request has an invalid file offset for the specified decompression mode.";
		case E_DSTORAGE_INVALID_SOURCE_TYPE:
			return "Invalid DirectStorage source type for the target queue.";
		case E_DSTORAGE_INVALID_INTERMEDIATE_SIZE:
			return "The request has an invalid intermediate size for the specified decompression modes.";
		case E_DSTORAGE_SYSTEM_NOT_SUPPORTED:
			return "DirectStorage is not supported on this platform/generation.";
		case E_DSTORAGE_STAGING_BUFFER_LOCKED:
			return "DirectStorage staging buffer size can only be changed when no queue is created and no file is open.";
		case E_DSTORAGE_INVALID_STAGING_BUFFER_SIZE:
			return "The specified DirectStorage staging buffer size is not valid.";
		case E_DSTORAGE_STAGING_BUFFER_TOO_SMALL:
			return "The DirectStorage staging buffer is not large enough to perform this operation.";
		case E_DSTORAGE_INVALID_FENCE:
			return "The DirectStorage completion fence is invalid or has been released.";
		case E_DSTORAGE_INVALID_STATUS_ARRAY:
			return "The DirectStorage status array is invalid or has been released.";
		case E_DSTORAGE_INVALID_MEMORY_QUEUE_PRIORITY:
			return "Invalid priority specified for the DirectStorage memory queue.";
		case E_DSTORAGE_DECOMPRESSION_ERROR:
			return "A generic error occurred during DirectStorage decompression.";
		case E_DSTORAGE_COMPRESSED_DATA_TOO_LARGE:
			return "The compressed data size is too large for DirectStorage GPU decompression.";
		case E_DSTORAGE_INVALID_DESTINATION_TYPE:
			return "A GPU memory destination request was enqueued into a queue created without a D3D device, or destination type is unknown.";
		case E_DSTORAGE_FILEBUFFERING_REQUIRES_DISABLED_BYPASSIO:
			return "DirectStorage ForceFileBuffering was enabled without disabling BypassIO.";
		default:
			break;
	}

	wchar_t windows_error_buffer[2048] = { 0 };
	DWORD len = FormatMessageW(
			FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			static_cast<DWORD>(p_hresult),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			windows_error_buffer,
			2048,
			NULL);
	if (len > 0) {
		String sys_msg = String(windows_error_buffer).strip_edges();
		if (!sys_msg.is_empty()) {
			return sys_msg;
		}
	}
#else
	switch (p_hresult) {
		case 0x00000000U:
			return "Operation completed successfully (S_OK).";
		case 0x00000001U:
			return "Operation completed with alternate success condition (S_FALSE).";
		case 0x80070057U:
			return "The parameter is incorrect (E_INVALIDARG).";
		case 0x8007000EU:
			return "Ran out of memory (E_OUTOFMEMORY).";
		case 0x80004005U:
			return "Unspecified failure (E_FAIL).";
		case 0x80004001U:
			return "Not implemented (E_NOTIMPL).";
		case 0x80004003U:
			return "Invalid pointer (E_POINTER).";
		case 0x8000FFFFU:
			return "Catastrophic failure (E_UNEXPECTED).";
		case 0x80070005U:
			return "General access denied (E_ACCESSDENIED).";
		case 0x80070006U:
			return "Invalid handle (E_HANDLE).";
		case 0x80004004U:
			return "Operation aborted (E_ABORT).";
		case 0x89240001U:
			return "DirectStorage is already running exclusively.";
		case 0x89240002U:
			return "DirectStorage is not running.";
		case 0x89240003U:
			return "Invalid DirectStorage queue capacity parameter.";
		case 0x89240007U:
			return "The DirectStorage request offset and length exceed the file size.";
		case 0x89240008U:
			return "The DirectStorage request exceeds the configured staging buffer.";
		case 0x89240009U:
			return "The DirectStorage destination buffer is not accessible.";
		case 0x8924000AU:
			return "The file is not supported by DirectStorage.";
		case 0x8924000BU:
			return "The DirectStorage file handle is unavailable or closed.";
		case 0x8924000CU:
			return "A DirectStorage reserved field is not set to 0.";
		case 0x8924000FU:
			return "The DirectStorage destination size is invalid for the requested decompression mode.";
		case 0x89240010U:
			return "The DirectStorage request targets a queue that is closed.";
		case 0x89240012U:
			return "The number of DirectStorage queues has reached the maximum limit.";
		case 0x89240013U:
			return "Invalid priority is specified for the DirectStorage queue.";
		case 0x89240014U:
			return "The number of DirectStorage files has reached the maximum limit.";
		case 0x89240015U:
			return "The DirectStorage index parameter is out of bounds.";
		case 0x89240016U:
			return "The DirectStorage IO operation has timed out.";
		case 0x89240017U:
			return "The specified file has not been opened in DirectStorage.";
		case 0x8924001AU:
			return "The request has an invalid file offset for the specified decompression mode.";
		case 0x8924001BU:
			return "Invalid DirectStorage source type for the target queue.";
		case 0x8924001FU:
			return "DirectStorage staging buffer size can only be changed when no queue is created and no file is open.";
		case 0x89240020U:
			return "The specified DirectStorage staging buffer size is not valid.";
		case 0x89240021U:
			return "The DirectStorage staging buffer is not large enough to perform this operation.";
		case 0x89240022U:
			return "The DirectStorage completion fence is invalid or has been released.";
		case 0x89240023U:
			return "The DirectStorage status array is invalid or has been released.";
		case 0x89240024U:
			return "Invalid priority specified for the DirectStorage memory queue.";
		case 0x89240030U:
			return "A generic error occurred during DirectStorage decompression.";
		case 0x89240039U:
			return "The compressed data size is too large for DirectStorage GPU decompression.";
		case 0x89240040U:
			return "A GPU memory destination request was enqueued into a queue created without a D3D device, or destination type is unknown.";
		case 0x89240041U:
			return "DirectStorage ForceFileBuffering was enabled without disabling BypassIO.";
		default:
			break;
	}
#endif
	return "";
}

DSErrorHandler::DSErrorHandler() {
	if (singleton == nullptr) {
		singleton = this;
	}
}

DSErrorHandler::~DSErrorHandler() {
#ifdef _WIN32
	unregister_queue();
#endif
	if (singleton == this) {
		singleton = nullptr;
	}
}

#ifdef _WIN32
bool DSErrorHandler::register_queue(IDStorageQueue *p_queue, std::mutex *p_queue_access_mutex) {
	if (!p_queue) {
		return false;
	}
	std::lock_guard<std::mutex> lock(error_mutex);
	observed_queue_mutex = p_queue_access_mutex;
	for (IDStorageQueue *queue : observed_queues) {
		if (queue == p_queue) {
			return true;
		}
	}
	p_queue->AddRef();
	observed_queues.push_back(p_queue);
	return true;
}

void DSErrorHandler::unregister_queue() {
	std::lock_guard<std::mutex> lock(error_mutex);
	observed_queue_mutex = nullptr;
	for (IDStorageQueue *queue : observed_queues) {
		if (queue) {
			queue->Release();
		}
	}
	observed_queues.clear();
}

void DSErrorHandler::process_queue_errors() {
	std::vector<IDStorageQueue *> queues_copy;
	std::mutex *queue_access = nullptr;
	{
		std::lock_guard<std::mutex> lock(error_mutex);
		queue_access = observed_queue_mutex;
		queues_copy.reserve(observed_queues.size());
		for (IDStorageQueue *queue : observed_queues) {
			if (queue) {
				queue->AddRef();
				queues_copy.push_back(queue);
			}
		}
	}

	// Serialise this drain with the completion thread's own RetrieveErrorRecord() caller. The
	// SDK ties that call to the error event and never licenses two callers, and FailureCount
	// counts failures "since the last RetrieveErrorRecord call", so racing drains split one
	// failure record between them.
	std::unique_lock<std::mutex> queue_lock;
	if (queue_access) {
		queue_lock = std::unique_lock<std::mutex>(*queue_access);
	}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	_debug_invoke_queue_reader_hook();
#endif

	// Build the messages here; do not call into the engine while the lock is held.
	std::vector<String> messages;
	for (IDStorageQueue *queue : queues_copy) {
		DSTORAGE_ERROR_RECORD errorRecord{};
		queue->RetrieveErrorRecord(&errorRecord);
		if (errorRecord.FailureCount > 0) {
			const auto &failureRecord = errorRecord.FirstFailure;
			String mapped_message = describe_hresult(static_cast<uint32_t>(failureRecord.HResult));
			if (mapped_message.is_empty()) {
				mapped_message = "No system message is available for this DirectStorage HRESULT.";
			}
			String command_type_str = describe_command_type(static_cast<int32_t>(failureRecord.CommandType));
			String err_msg = String("DirectStorage Async Error (HRESULT 0x") +
					String::num_uint64(static_cast<uint32_t>(failureRecord.HResult), 16) +
					", command type " + command_type_str + "): " +
					mapped_message;
			if (failureRecord.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST) {
				err_msg += " [File: " + String(failureRecord.Request.Filename) +
						", RequestName: " + String(failureRecord.Request.RequestName) + "]";
			}
			messages.push_back(err_msg);
		}
	}

	if (queue_lock.owns_lock()) {
		queue_lock.unlock();
	}
	for (IDStorageQueue *queue : queues_copy) {
		queue->Release();
	}

	// Engine calls happen after every extension mutex is released (GODOT_QUIRKS.md Q46).
	for (const String &message : messages) {
		UtilityFunctions::push_error(message);
		if (singleton) {
			singleton->report_error(message);
		}
	}
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void DSErrorHandler::_debug_set_queue_error_reader_hook_for_testing(const std::function<void()> &p_hook) {
	std::lock_guard<std::mutex> lock(queue_reader_hook_mutex);
	queue_reader_hook = p_hook;
}

void DSErrorHandler::_debug_invoke_queue_reader_hook() {
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> lock(queue_reader_hook_mutex);
		hook = queue_reader_hook;
	}
	if (hook) {
		hook();
	}
}
#endif
#endif

bool DSErrorHandler::has_error() const {
	std::lock_guard<std::mutex> lock(error_mutex);
	return error_flag;
}

String DSErrorHandler::get_last_error() const {
	std::lock_guard<std::mutex> lock(error_mutex);
	return last_error;
}

uint64_t DSErrorHandler::get_error_sequence() const {
	std::lock_guard<std::mutex> lock(error_mutex);
	return error_sequence;
}

void DSErrorHandler::clear_error() {
	std::lock_guard<std::mutex> lock(error_mutex);
	last_error = "";
	error_flag = false;
	// error_sequence is deliberately NOT rewound: a clear must not relabel an in-flight batch
	// as failed by moving its snapshot value.
}

void DSErrorHandler::report_error(const String &p_msg) {
	std::lock_guard<std::mutex> lock(error_mutex);
	last_error = p_msg;
	error_flag = true;
	error_sequence++;
}

void DSErrorHandler::_bind_methods() {
	ClassDB::bind_static_method("DSErrorHandler", D_METHOD("describe_hresult", "hresult"), &DSErrorHandler::describe_hresult);
	ClassDB::bind_static_method("DSErrorHandler", D_METHOD("describe_command_type", "command_type"), &DSErrorHandler::describe_command_type);
	ClassDB::bind_method(D_METHOD("has_error"), &DSErrorHandler::has_error);
	ClassDB::bind_method(D_METHOD("get_last_error"), &DSErrorHandler::get_last_error);
	ClassDB::bind_method(D_METHOD("clear_error"), &DSErrorHandler::clear_error);
	ClassDB::bind_method(D_METHOD("report_error", "msg"), &DSErrorHandler::report_error);
#ifdef _WIN32
	ClassDB::bind_method(D_METHOD("process_queue_errors"), &DSErrorHandler::process_queue_errors);
#endif
}

} // namespace godot
