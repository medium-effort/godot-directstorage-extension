#include "ds_error_handler.h"
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <strsafe.h>
#include <dstorageerr.h>
#endif

namespace godot {

DSErrorHandler *DSErrorHandler::singleton = nullptr;

DSErrorHandler *DSErrorHandler::get_singleton() {
	return singleton;
}

String DSErrorHandler::describe_hresult(uint32_t p_hresult) {
#ifdef _WIN32
	switch (static_cast<HRESULT>(p_hresult)) {
		case E_DSTORAGE_END_OF_FILE:
			return "The DirectStorage request offset and length exceed the file size.";
		case E_DSTORAGE_REQUEST_TOO_LARGE:
			return "The DirectStorage request exceeds the configured staging buffer.";
		case E_DSTORAGE_ACCESS_VIOLATION:
			return "The DirectStorage destination buffer is not accessible.";
		case E_DSTORAGE_FILE_NOT_OPEN:
			return "The DirectStorage file handle is unavailable or closed.";
		default:
			break;
	}
#else
	switch (p_hresult) {
		case 0x89240007U:
			return "The DirectStorage request offset and length exceed the file size.";
		case 0x89240008U:
			return "The DirectStorage request exceeds the configured staging buffer.";
		case 0x89240009U:
			return "The DirectStorage destination buffer is not accessible.";
		case 0x8924000BU:
			return "The DirectStorage file handle is unavailable or closed.";
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
bool DSErrorHandler::register_queue(IDStorageQueue *p_queue) {
	if (!p_queue) {
		return false;
	}
	unregister_queue();

	observed_queue = p_queue;
	return true;
}

void DSErrorHandler::unregister_queue() {
	if (registered_wait_handle != nullptr) {
		UnregisterWait(registered_wait_handle);
		registered_wait_handle = nullptr;
	}
	observed_queue = nullptr;
}


void DSErrorHandler::process_queue_errors() {
	if (!observed_queue) {
		return;
	}

	DSTORAGE_ERROR_RECORD errorRecord{};
	observed_queue->RetrieveErrorRecord(&errorRecord);

	if (errorRecord.FailureCount > 0) {
		const auto &failureRecord = errorRecord.FirstFailure;

		String mapped_message = describe_hresult(static_cast<uint32_t>(failureRecord.HResult));
		wchar_t windowsErrorBuffer[2048] = { 0 };
		if (mapped_message.is_empty()) {
			FormatMessageW(
					FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					NULL,
					failureRecord.HResult,
					MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
					windowsErrorBuffer,
					2048,
					NULL);
			mapped_message = String(windowsErrorBuffer).strip_edges();
		}
		if (mapped_message.is_empty()) {
			mapped_message = "No system message is available for this DirectStorage HRESULT.";
		}

		String err_msg = String("DirectStorage Async Error (HRESULT 0x") +
				String::num_uint64(static_cast<uint32_t>(failureRecord.HResult), 16) + "): " +
				mapped_message;

		if (failureRecord.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST) {
			err_msg += " [File: " + String(failureRecord.Request.Filename) +
					", RequestName: " + String(failureRecord.Request.RequestName) + "]";
		}

		UtilityFunctions::push_error(err_msg);

		if (singleton) {
			singleton->report_error(err_msg);
		}
	}
}
#endif

bool DSErrorHandler::has_error() const {
	return error_flag;
}

String DSErrorHandler::get_last_error() const {
	return last_error;
}

void DSErrorHandler::clear_error() {
	last_error = "";
	error_flag = false;
}

void DSErrorHandler::report_error(const String &p_msg) {
	last_error = p_msg;
	error_flag = true;
}

void DSErrorHandler::_bind_methods() {
	ClassDB::bind_static_method("DSErrorHandler", D_METHOD("describe_hresult", "hresult"), &DSErrorHandler::describe_hresult);
	ClassDB::bind_method(D_METHOD("has_error"), &DSErrorHandler::has_error);
	ClassDB::bind_method(D_METHOD("get_last_error"), &DSErrorHandler::get_last_error);
	ClassDB::bind_method(D_METHOD("clear_error"), &DSErrorHandler::clear_error);
	ClassDB::bind_method(D_METHOD("report_error", "msg"), &DSErrorHandler::report_error);
#ifdef _WIN32
	ClassDB::bind_method(D_METHOD("process_queue_errors"), &DSErrorHandler::process_queue_errors);
#endif
}

} // namespace godot
