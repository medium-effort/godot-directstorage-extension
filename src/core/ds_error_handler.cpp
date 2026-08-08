#include "ds_error_handler.h"
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <strsafe.h>
#endif

namespace godot {

DSErrorHandler *DSErrorHandler::singleton = nullptr;

DSErrorHandler *DSErrorHandler::get_singleton() {
	return singleton;
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
	HANDLE error_event = p_queue->GetErrorEvent();

	BOOL success = RegisterWaitForSingleObject(
			&registered_wait_handle,
			error_event,
			error_callback,
			p_queue,
			INFINITE,
			WT_EXECUTEDEFAULT);

	return (success != 0);
}

void DSErrorHandler::unregister_queue() {
	if (registered_wait_handle != nullptr) {
		UnregisterWait(registered_wait_handle);
		registered_wait_handle = nullptr;
	}
	observed_queue = nullptr;
}

VOID NTAPI DSErrorHandler::error_callback(PVOID pData, BOOLEAN bTimeout) {
	if (!pData) {
		return;
	}

	IDStorageQueue *queue = reinterpret_cast<IDStorageQueue *>(pData);
	DSTORAGE_ERROR_RECORD errorRecord{};
	queue->RetrieveErrorRecord(&errorRecord);

	if (errorRecord.FailureCount > 0) {
		const auto &failureRecord = errorRecord.FirstFailure;

		wchar_t windowsErrorBuffer[2048] = { 0 };
		FormatMessageW(
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL,
				failureRecord.HResult,
				MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				windowsErrorBuffer,
				2048,
				NULL);

		String err_msg = String("DirectStorage Async Error (HRESULT 0x") +
				String::num_uint64(static_cast<uint32_t>(failureRecord.HResult), 16) + "): " +
				String(windowsErrorBuffer);

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
	ClassDB::bind_method(D_METHOD("has_error"), &DSErrorHandler::has_error);
	ClassDB::bind_method(D_METHOD("get_last_error"), &DSErrorHandler::get_last_error);
	ClassDB::bind_method(D_METHOD("clear_error"), &DSErrorHandler::clear_error);
	ClassDB::bind_method(D_METHOD("report_error", "msg"), &DSErrorHandler::report_error);
}

} // namespace godot
