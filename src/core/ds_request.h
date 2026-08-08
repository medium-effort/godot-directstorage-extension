#ifndef DS_REQUEST_H
#define DS_REQUEST_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

class DSRequest : public RefCounted {
	GDCLASS(DSRequest, RefCounted);

public:
	enum Status {
		STATUS_PENDING = 0,
		STATUS_SUBMITTED = 1,
		STATUS_COMPLETED = 2,
		STATUS_FAILED = 3,
	};

	enum CompressionFormat {
		COMPRESSION_NONE = 0,
		COMPRESSION_GDEFLATE = 1,
	};

private:
	String file_path;
	uint64_t offset = 0;
	uint32_t size = 0;
	uint64_t cancellation_tag = 0;
	CompressionFormat compression_format = COMPRESSION_NONE;
	PackedByteArray destination_buffer;
	Status status = STATUS_PENDING;
	String error_message;

protected:
	static void _bind_methods();

public:
	DSRequest();
	~DSRequest();

	void set_file_path(const String &p_path);
	String get_file_path() const;

	void set_offset(uint64_t p_offset);
	uint64_t get_offset() const;

	void set_size(uint32_t p_size);
	uint32_t get_size() const;

	void set_cancellation_tag(uint64_t p_tag);
	uint64_t get_cancellation_tag() const;

	void set_compression_format(CompressionFormat p_format);
	CompressionFormat get_compression_format() const;

	void set_destination_buffer(const PackedByteArray &p_buffer);
	PackedByteArray get_destination_buffer() const;

	void set_status(Status p_status);
	Status get_status() const;

	void set_error_message(const String &p_msg);
	String get_error_message() const;
};

} // namespace godot

VARIANT_ENUM_CAST(DSRequest::Status);
VARIANT_ENUM_CAST(DSRequest::CompressionFormat);

#endif // DS_REQUEST_H
