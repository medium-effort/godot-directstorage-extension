#ifndef DS_REQUEST_H
#define DS_REQUEST_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <atomic>
#include <functional>
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
	// C++-only record of a GDeflate tile-stream header that was already
	// validated off-thread by DSPreloader. Not exposed to GDScript: only the
	// preloader stamps it, and the backend honours it only when the request's
	// path/offset/declared size still match the validated triple. A mismatch
	// (or an unstamped request) falls back to the backend's own synchronous
	// check, so direct enqueue_request callers and corruption tests stay
	// fail-closed. Written under payload_mutex like every other payload field.
	bool gdeflate_stream_prevalidated = false;
	String gdeflate_validated_path;
	uint64_t gdeflate_validated_offset = 0;
	uint32_t gdeflate_validated_size = 0;
	PackedByteArray destination_buffer;
	DSDestinationType destination_type = DESTINATION_MEMORY;
	RID destination_resource;
	uint32_t first_subresource = 0;
	uint32_t subresource_count = 1;
	// Row range inside one subresource for chunked uploads, in texel rows
	// (pixels) of the target subresource - the unit D3D12_BOX and
	// DSTORAGE_DESTINATION_TEXTURE_REGION use. Both zero means "every
	// subresource"; a non-zero pair must satisfy top < bottom and is rejected
	// at submit otherwise. The setter is exposed to scripts
	// (see set_texture_region's docs).
	uint32_t texture_region_top = 0;
	uint32_t texture_region_bottom = 0;
	uint32_t texture_format = 0;
	uint32_t texture_layout = 0;
	DSPriority priority = PRIORITY_NORMAL;
	std::atomic<Status> status{ STATUS_PENDING };
	std::atomic<bool> gpu_in_flight{ false };
	std::atomic<uint64_t> cancel_timestamp_usec{ 0 };
	mutable std::mutex error_mutex;
	String error_message;
	// Optional diagnostic label (e.g. engine resource path) for archive-backed errors.
	String debug_label;
	// Guards the payload fields against a submitter. A mutating setter holds it across
	// (is_mutable(), assignment); the submitter holds it across (PENDING -> SUBMITTED).
	mutable std::mutex payload_mutex;

protected:
	static void _bind_methods();

public:
	DSRequest();
	~DSRequest();

	void set_file_path(const String &p_path);
	String get_file_path() const;

	void set_offset(uint64_t p_offset);
	uint64_t get_offset() const;

	void set_size(int64_t p_size);
	uint32_t get_size() const;

	void set_uncompressed_size(int64_t p_size);
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

	void set_texture_region(uint32_t p_top, uint32_t p_bottom);
	uint32_t get_texture_region_top() const;
	uint32_t get_texture_region_bottom() const;

	// True when the caller opted into the single-subresource row band: either
	// bound non-zero. Both zero is the historic "all subresources, whole
	// subresource" destination. A non-zero pair is a contract, not a hint: it
	// must satisfy top < bottom inside one 2D texture subresource, and the
	// backend fails the request before submission otherwise - it is never
	// silently downgraded to a whole-subresource write.
	bool has_texture_row_region() const { return texture_region_top != 0 || texture_region_bottom != 0; }

	void set_texture_format(uint32_t p_format);
	uint32_t get_texture_format() const;

	void set_texture_layout(uint32_t p_layout);
	uint32_t get_texture_layout() const;

	void set_debug_label(const String &p_label);
	String get_debug_label() const;

	void set_status(Status p_status);
	Status get_status() const;

	void set_error_message(const String &p_msg);
	String get_error_message() const;

	bool is_mutable() const { return status.load(std::memory_order_acquire) == STATUS_PENDING && !gpu_in_flight.load(std::memory_order_acquire); }
	bool is_gpu_in_flight() const { return gpu_in_flight.load(std::memory_order_acquire); }
	void set_gpu_in_flight(bool p_in_flight) { gpu_in_flight.store(p_in_flight, std::memory_order_release); }

	// Seals the request for submission: atomically PENDING -> SUBMITTED under the same
	// mutex the setters take. False when the request is already submitted, in flight or
	// terminal - in that case the status is left alone.
	bool claim_submission();

	// C++-only (not bound to GDScript). Stamps the off-thread-validated
	// GDeflate header record. The backend only skips its own validation when
	// all three values still match, which makes a late mutation of the request
	// fall back to the synchronous check instead of trusting a stale validation.
	void set_gdeflate_stream_prevalidated(const String &p_path, uint64_t p_offset, uint32_t p_declared_size);
	bool is_gdeflate_stream_prevalidated(const String &p_path, uint64_t p_offset, uint32_t p_declared_size) const;

	uint64_t get_cancel_timestamp_usec() const { return cancel_timestamp_usec.load(std::memory_order_acquire); }
	void set_cancel_timestamp_usec(uint64_t p_usec) { cancel_timestamp_usec.store(p_usec, std::memory_order_release); }

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only, non-binding. Same claim, non-blocking: false means another thread is
	// currently inside a setter's guarded window.
	bool _debug_try_claim_submission_for_testing();
	// Test-only, non-binding. Runs inside every guarded setter, after the guard and
	// before the assignment, while the payload mutex is held.
	void _debug_set_mutation_window_hook_for_testing(const std::function<void()> &p_hook);
#endif

private:
	void notify_mutation_window() {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (mutation_window_hook) {
			mutation_window_hook();
		}
#endif
	}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	std::function<void()> mutation_window_hook;
#endif
};

} // namespace godot

VARIANT_ENUM_CAST(DSRequest::Status);
VARIANT_ENUM_CAST(DSRequest::CompressionFormat);
VARIANT_ENUM_CAST(DSRequest::DSDestinationType);
VARIANT_ENUM_CAST(DSRequest::DSPriority);

#endif // DS_REQUEST_H
