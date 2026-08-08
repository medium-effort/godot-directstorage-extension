#include "ds_context.h"
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

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
	if (singleton == this) {
		singleton = nullptr;
	}
}

bool DSContext::is_supported() const {
#ifdef _WIN32
	return true;
#else
	return false;
#endif
}

bool DSContext::initialize_ds() {
	UtilityFunctions::print("DirectStorage Context initialized.");
	initialized = true;
	return true;
}

void DSContext::shutdown_ds() {
	initialized = false;
	UtilityFunctions::print("DirectStorage Context shut down.");
}

void DSContext::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_supported"), &DSContext::is_supported);
	ClassDB::bind_method(D_METHOD("initialize_ds"), &DSContext::initialize_ds);
	ClassDB::bind_method(D_METHOD("shutdown_ds"), &DSContext::shutdown_ds);
}

} // namespace godot
