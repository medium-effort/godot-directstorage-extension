#ifndef DS_REQUEST_H
#define DS_REQUEST_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <atomic>
#include <mutex>

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

	enum DSDestinationType {
		DESTINATION_MEMORY = 0,
		DESTINATION_GPU_BUFFER = 1,
		DESTINATION_GPU_TEXTURE = 2,
	};

	enum DSPriority {
		PRIORITY_LOW = 0,
		PRIORITY_NORMAL = 1,
		PRIORITY_HIGH = 2,
	};

private:
	String file_path;
	uint64_t offset = 0;
	uint32_t size = 0;
	uint32_t uncompressed_size = 0;
	uint64_t cancellation_tag = 0;
	CompressionFormat compression_format = COMPRESSION_NONE;
	PackedByteArray destination_buffer;
	DSDestinationType destination_type = DESTINATION_MEMORY;
	RID destination_resource;
	uint32_t first_subresource = 0;
	uint32_t subresource_count = 1;
	uint32_t texture_format = 0;
	uint32_t texture_layout = 0;
	DSPriority priority = PRIORITY_NORMAL;
	std::atomic<Status> status{ STATUS_PENDING };
	std::atomic<bool> gpu_in_flight{ false };
	std::atomic<uint64_t> cancel_timestamp_usec{ 0 };
	mutable std::mutex error_mutex;
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

	void set_uncompressed_size(uint32_t p_size);
	uint32_t get_uncompressed_size() const;

	void set_cancellation_tag(uint64_t p_tag);
	uint64_t get_cancellation_tag() const;

	void set_compression_format(CompressionFormat p_format);
	CompressionFormat get_compression_format() const;

	void set_destination_buffer(const PackedByteArray &p_buffer);
	PackedByteArray get_destination_buffer() const;
	PackedByteArray &get_destination_buffer_ref();
	uint8_t *get_destination_buffer_ptrw();

	bool ensure_destination_buffer_capacity(uint64_t p_required_size);
	bool resize_destination_buffer(uint64_t p_size);

	void set_destination_type(DSDestinationType p_type);
	DSDestinationType get_destination_type() const;

	void set_priority(DSPriority p_priority);
	DSPriority get_priority() const;

	void set_destination_resource(const RID &p_resource);
	RID get_destination_resource() const;

	void set_first_subresource(uint32_t p_subresource);
	uint32_t get_first_subresource() const;

	void set_subresource_count(uint32_t p_count);
	uint32_t get_subresource_count() const;

	void set_texture_format(uint32_t p_format);
	uint32_t get_texture_format() const;

	void set_texture_layout(uint32_t p_layout);
	uint32_t get_texture_layout() const;

	void set_status(Status p_status);
	Status get_status() const;

	void set_error_message(const String &p_msg);
	String get_error_message() const;

	bool is_mutable() const { return status.load(std::memory_order_acquire) == STATUS_PENDING && !gpu_in_flight.load(std::memory_order_acquire); }
	bool is_gpu_in_flight() const { return gpu_in_flight.load(std::memory_order_acquire); }
	void set_gpu_in_flight(bool p_in_flight) { gpu_in_flight.store(p_in_flight, std::memory_order_release); }

	uint64_t get_cancel_timestamp_usec() const { return cancel_timestamp_usec.load(std::memory_order_acquire); }
	void set_cancel_timestamp_usec(uint64_t p_usec) { cancel_timestamp_usec.store(p_usec, std::memory_order_release); }
};

} // namespace godot

VARIANT_ENUM_CAST(DSRequest::Status);
VARIANT_ENUM_CAST(DSRequest::CompressionFormat);
VARIANT_ENUM_CAST(DSRequest::DSDestinationType);
VARIANT_ENUM_CAST(DSRequest::DSPriority);

#endif // DS_REQUEST_H
