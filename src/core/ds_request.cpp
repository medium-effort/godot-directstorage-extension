#include "ds_request.h"
#include <godot_cpp/variant/utility_functions.hpp>

namespace godot {

DSRequest::DSRequest() {}

DSRequest::~DSRequest() {}

void DSRequest::set_file_path(const String &p_path) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	file_path = p_path;
}

String DSRequest::get_file_path() const {
	return file_path;
}

void DSRequest::set_offset(uint64_t p_offset) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	offset = p_offset;
}

uint64_t DSRequest::get_offset() const {
	return offset;
}

void DSRequest::set_size(int64_t p_size) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	if (p_size < 0 || p_size > static_cast<int64_t>(UINT32_MAX)) {
		UtilityFunctions::push_error("DSRequest: Request size " + String::num_int64(p_size) + " exceeds maximum supported DirectStorage size (4 GiB).");
		return;
	}
	notify_mutation_window();
	size = static_cast<uint32_t>(p_size);
}

uint32_t DSRequest::get_size() const {
	return size;
}

void DSRequest::set_uncompressed_size(int64_t p_size) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	if (p_size < 0 || p_size > static_cast<int64_t>(UINT32_MAX)) {
		UtilityFunctions::push_error("DSRequest: Request uncompressed_size " + String::num_int64(p_size) + " exceeds maximum supported DirectStorage size (4 GiB).");
		return;
	}
	notify_mutation_window();
	uncompressed_size = static_cast<uint32_t>(p_size);
}

uint32_t DSRequest::get_uncompressed_size() const {
	return uncompressed_size;
}

void DSRequest::set_cancellation_tag(uint64_t p_tag) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	cancellation_tag = p_tag;
}

uint64_t DSRequest::get_cancellation_tag() const {
	return cancellation_tag;
}

void DSRequest::set_compression_format(CompressionFormat p_format) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	compression_format = p_format;
}

DSRequest::CompressionFormat DSRequest::get_compression_format() const {
	return compression_format;
}

void DSRequest::set_gdeflate_stream_prevalidated(const String &p_path, uint64_t p_offset, uint32_t p_declared_size) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot stamp a GDeflate validation record while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	gdeflate_stream_prevalidated = true;
	gdeflate_validated_path = p_path;
	gdeflate_validated_offset = p_offset;
	gdeflate_validated_size = p_declared_size;
}

bool DSRequest::is_gdeflate_stream_prevalidated(const String &p_path, uint64_t p_offset, uint32_t p_declared_size) const {
	// Getters are unguarded by design: these fields are written only before
	// claim_submission(), after which they are immutable for the submitter.
	return gdeflate_stream_prevalidated &&
			gdeflate_validated_path == p_path &&
			gdeflate_validated_offset == p_offset &&
			gdeflate_validated_size == p_declared_size;
}

void DSRequest::set_destination_buffer(const PackedByteArray &p_buffer) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	destination_buffer = p_buffer;
}

PackedByteArray DSRequest::get_destination_buffer() const {
	return destination_buffer;
}

PackedByteArray &DSRequest::get_destination_buffer_ref() {
	return destination_buffer;
}

uint8_t *DSRequest::get_destination_buffer_ptrw() {
	return destination_buffer.ptrw();
}

bool DSRequest::ensure_destination_buffer_capacity(uint64_t p_required_size) {
	if (destination_buffer.size() < static_cast<int64_t>(p_required_size)) {
		destination_buffer.resize(static_cast<int64_t>(p_required_size));
	}
	return true;
}

bool DSRequest::resize_destination_buffer(uint64_t p_size) {
	if (destination_buffer.size() != static_cast<int64_t>(p_size)) {
		destination_buffer.resize(static_cast<int64_t>(p_size));
	}
	return true;
}

void DSRequest::set_destination_type(DSDestinationType p_type) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	destination_type = p_type;
}

DSRequest::DSDestinationType DSRequest::get_destination_type() const {
	return destination_type;
}

void DSRequest::set_priority(DSPriority p_priority) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	priority = p_priority;
}

DSRequest::DSPriority DSRequest::get_priority() const {
	return priority;
}

void DSRequest::set_destination_resource(const RID &p_resource) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	destination_resource = p_resource;
}

RID DSRequest::get_destination_resource() const {
	return destination_resource;
}

void DSRequest::set_first_subresource(uint32_t p_subresource) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	first_subresource = p_subresource;
}

uint32_t DSRequest::get_first_subresource() const {
	return first_subresource;
}

void DSRequest::set_subresource_count(uint32_t p_count) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	subresource_count = p_count;
}

uint32_t DSRequest::get_subresource_count() const {
	return subresource_count;
}

void DSRequest::set_texture_region(uint32_t p_top, uint32_t p_bottom) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	texture_region_top = p_top;
	texture_region_bottom = p_bottom;
}

uint32_t DSRequest::get_texture_region_top() const {
	return texture_region_top;
}

uint32_t DSRequest::get_texture_region_bottom() const {
	return texture_region_bottom;
}

void DSRequest::set_debug_label(const String &p_label) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		return;
	}
	debug_label = p_label;
}

String DSRequest::get_debug_label() const {
	std::lock_guard<std::mutex> lock(payload_mutex);
	return debug_label;
}

void DSRequest::set_texture_format(uint32_t p_format) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	texture_format = p_format;
}

uint32_t DSRequest::get_texture_format() const {
	return texture_format;
}

void DSRequest::set_texture_layout(uint32_t p_layout) {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		UtilityFunctions::push_error("DSRequest: Cannot modify request payload while request is in-flight or completed.");
		return;
	}
	notify_mutation_window();
	texture_layout = p_layout;
}

uint32_t DSRequest::get_texture_layout() const {
	return texture_layout;
}

void DSRequest::set_status(Status p_status) {
	status.store(p_status, std::memory_order_release);
}

DSRequest::Status DSRequest::get_status() const {
	return status.load(std::memory_order_acquire);
}

bool DSRequest::claim_submission() {
	std::lock_guard<std::mutex> lock(payload_mutex);
	if (!is_mutable()) {
		return false;
	}
	status.store(STATUS_SUBMITTED, std::memory_order_release);
	return true;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
bool DSRequest::_debug_try_claim_submission_for_testing() {
	std::unique_lock<std::mutex> lock(payload_mutex, std::try_to_lock);
	if (!lock.owns_lock() || !is_mutable()) {
		return false;
	}
	status.store(STATUS_SUBMITTED, std::memory_order_release);
	return true;
}

void DSRequest::_debug_set_mutation_window_hook_for_testing(const std::function<void()> &p_hook) {
	mutation_window_hook = p_hook;
}
#endif

void DSRequest::set_error_message(const String &p_msg) {
	std::lock_guard<std::mutex> lock(error_mutex);
	error_message = p_msg;
}

String DSRequest::get_error_message() const {
	std::lock_guard<std::mutex> lock(error_mutex);
	return error_message;
}

void DSRequest::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_file_path", "path"), &DSRequest::set_file_path);
	ClassDB::bind_method(D_METHOD("get_file_path"), &DSRequest::get_file_path);

	ClassDB::bind_method(D_METHOD("set_offset", "offset"), &DSRequest::set_offset);
	ClassDB::bind_method(D_METHOD("get_offset"), &DSRequest::get_offset);

	ClassDB::bind_method(D_METHOD("set_size", "size"), &DSRequest::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &DSRequest::get_size);

	ClassDB::bind_method(D_METHOD("set_uncompressed_size", "uncompressed_size"), &DSRequest::set_uncompressed_size);
	ClassDB::bind_method(D_METHOD("get_uncompressed_size"), &DSRequest::get_uncompressed_size);

	ClassDB::bind_method(D_METHOD("set_cancellation_tag", "tag"), &DSRequest::set_cancellation_tag);
	ClassDB::bind_method(D_METHOD("get_cancellation_tag"), &DSRequest::get_cancellation_tag);

	ClassDB::bind_method(D_METHOD("set_compression_format", "format"), &DSRequest::set_compression_format);
	ClassDB::bind_method(D_METHOD("get_compression_format"), &DSRequest::get_compression_format);

	ClassDB::bind_method(D_METHOD("set_destination_buffer", "buffer"), &DSRequest::set_destination_buffer);
	ClassDB::bind_method(D_METHOD("get_destination_buffer"), &DSRequest::get_destination_buffer);

	ClassDB::bind_method(D_METHOD("set_destination_type", "type"), &DSRequest::set_destination_type);
	ClassDB::bind_method(D_METHOD("get_destination_type"), &DSRequest::get_destination_type);

	ClassDB::bind_method(D_METHOD("set_priority", "priority"), &DSRequest::set_priority);
	ClassDB::bind_method(D_METHOD("get_priority"), &DSRequest::get_priority);

	ClassDB::bind_method(D_METHOD("set_destination_resource", "resource"), &DSRequest::set_destination_resource);
	ClassDB::bind_method(D_METHOD("get_destination_resource"), &DSRequest::get_destination_resource);

	ClassDB::bind_method(D_METHOD("set_first_subresource", "subresource"), &DSRequest::set_first_subresource);
	ClassDB::bind_method(D_METHOD("get_first_subresource"), &DSRequest::get_first_subresource);

	ClassDB::bind_method(D_METHOD("set_subresource_count", "count"), &DSRequest::set_subresource_count);
	ClassDB::bind_method(D_METHOD("get_subresource_count"), &DSRequest::get_subresource_count);

	ClassDB::bind_method(D_METHOD("set_texture_region", "top", "bottom"), &DSRequest::set_texture_region);
	ClassDB::bind_method(D_METHOD("get_texture_region_top"), &DSRequest::get_texture_region_top);
	ClassDB::bind_method(D_METHOD("get_texture_region_bottom"), &DSRequest::get_texture_region_bottom);

	ClassDB::bind_method(D_METHOD("set_texture_format", "format"), &DSRequest::set_texture_format);
	ClassDB::bind_method(D_METHOD("get_texture_format"), &DSRequest::get_texture_format);

	ClassDB::bind_method(D_METHOD("set_texture_layout", "layout"), &DSRequest::set_texture_layout);
	ClassDB::bind_method(D_METHOD("get_texture_layout"), &DSRequest::get_texture_layout);

	ClassDB::bind_method(D_METHOD("get_status"), &DSRequest::get_status);

	ClassDB::bind_method(D_METHOD("set_error_message", "message"), &DSRequest::set_error_message);
	ClassDB::bind_method(D_METHOD("get_error_message"), &DSRequest::get_error_message);

	ClassDB::bind_method(D_METHOD("is_gpu_in_flight"), &DSRequest::is_gpu_in_flight);
	ClassDB::bind_method(D_METHOD("get_cancel_timestamp_usec"), &DSRequest::get_cancel_timestamp_usec);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file_path"), "set_file_path", "get_file_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "offset"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "size"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "uncompressed_size"), "set_uncompressed_size", "get_uncompressed_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cancellation_tag"), "set_cancellation_tag", "get_cancellation_tag");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "compression_format"), "set_compression_format", "get_compression_format");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "destination_buffer"), "set_destination_buffer", "get_destination_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "destination_type"), "set_destination_type", "get_destination_type");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_ENUM, "Low,Normal,High"), "set_priority", "get_priority");
	ADD_PROPERTY(PropertyInfo(Variant::RID, "destination_resource"), "set_destination_resource", "get_destination_resource");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "first_subresource"), "set_first_subresource", "get_first_subresource");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "subresource_count"), "set_subresource_count", "get_subresource_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_format"), "set_texture_format", "get_texture_format");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_layout"), "set_texture_layout", "get_texture_layout");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "status"), "", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_message"), "set_error_message", "get_error_message");

	BIND_ENUM_CONSTANT(STATUS_PENDING);
	BIND_ENUM_CONSTANT(STATUS_SUBMITTED);
	BIND_ENUM_CONSTANT(STATUS_COMPLETED);
	BIND_ENUM_CONSTANT(STATUS_FAILED);

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_GDEFLATE);

	BIND_ENUM_CONSTANT(DESTINATION_MEMORY);
	BIND_ENUM_CONSTANT(DESTINATION_GPU_BUFFER);
	BIND_ENUM_CONSTANT(DESTINATION_GPU_TEXTURE);

	BIND_ENUM_CONSTANT(PRIORITY_LOW);
	BIND_ENUM_CONSTANT(PRIORITY_NORMAL);
	BIND_ENUM_CONSTANT(PRIORITY_HIGH);
}

} // namespace godot
