#include "ds_win32_backend.h"
#include "../../core/ds_context.h"

namespace godot {

bool Win32Backend::is_available() const {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	return ctx ? ctx->is_supported() : false;
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
	if (ctx) {
		return ctx->initialize_ds();
	}
#endif
	return false;
}

void Win32Backend::shutdown() {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (ctx) {
		ctx->shutdown_ds();
	}
#endif
}

bool Win32Backend::enqueue_request(Ref<DSRequest> p_request) {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (ctx) {
		return ctx->enqueue_request(p_request);
	}
#endif
	return false;
}

void Win32Backend::submit_batch() {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (ctx) {
		ctx->flush_batch();
	}
#endif
}

void Win32Backend::cancel_requests_with_tag(uint64_t p_tag) {
#ifdef _WIN32
	DSContext *ctx = DSContext::get_singleton();
	if (ctx) {
		ctx->cancel_requests_with_tag(p_tag);
	}
#endif
}

} // namespace godot
