#include "ds_request.h"

namespace godot {

DSRequest::DSRequest() {}

DSRequest::~DSRequest() {}

void DSRequest::set_file_path(const String &p_path) {
	file_path = p_path;
}

String DSRequest::get_file_path() const {
	return file_path;
}

void DSRequest::set_offset(uint64_t p_offset) {
	offset = p_offset;
}

uint64_t DSRequest::get_offset() const {
	return offset;
}

void DSRequest::set_size(uint32_t p_size) {
	size = p_size;
}

uint32_t DSRequest::get_size() const {
	return size;
}

void DSRequest::set_uncompressed_size(uint32_t p_size) {
	uncompressed_size = p_size;
}

uint32_t DSRequest::get_uncompressed_size() const {
	return uncompressed_size;
}

void DSRequest::set_cancellation_tag(uint64_t p_tag) {
	cancellation_tag = p_tag;
}

uint64_t DSRequest::get_cancellation_tag() const {
	return cancellation_tag;
}

void DSRequest::set_compression_format(CompressionFormat p_format) {
	compression_format = p_format;
}

DSRequest::CompressionFormat DSRequest::get_compression_format() const {
	return compression_format;
}

void DSRequest::set_destination_buffer(const PackedByteArray &p_buffer) {
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

void DSRequest::set_status(Status p_status) {
	status = p_status;
}

DSRequest::Status DSRequest::get_status() const {
	return status;
}

void DSRequest::set_error_message(const String &p_msg) {
	error_message = p_msg;
}

String DSRequest::get_error_message() const {
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

	ClassDB::bind_method(D_METHOD("set_status", "status"), &DSRequest::set_status);
	ClassDB::bind_method(D_METHOD("get_status"), &DSRequest::get_status);

	ClassDB::bind_method(D_METHOD("set_error_message", "message"), &DSRequest::set_error_message);
	ClassDB::bind_method(D_METHOD("get_error_message"), &DSRequest::get_error_message);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file_path"), "set_file_path", "get_file_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "offset"), "set_offset", "get_offset");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "size"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "uncompressed_size"), "set_uncompressed_size", "get_uncompressed_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cancellation_tag"), "set_cancellation_tag", "get_cancellation_tag");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "compression_format"), "set_compression_format", "get_compression_format");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_BYTE_ARRAY, "destination_buffer"), "set_destination_buffer", "get_destination_buffer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "status"), "set_status", "get_status");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_message"), "set_error_message", "get_error_message");

	BIND_ENUM_CONSTANT(STATUS_PENDING);
	BIND_ENUM_CONSTANT(STATUS_SUBMITTED);
	BIND_ENUM_CONSTANT(STATUS_COMPLETED);
	BIND_ENUM_CONSTANT(STATUS_FAILED);

	BIND_ENUM_CONSTANT(COMPRESSION_NONE);
	BIND_ENUM_CONSTANT(COMPRESSION_GDEFLATE);
}

} // namespace godot
