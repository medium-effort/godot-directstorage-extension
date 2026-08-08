#ifndef DS_CONTEXT_H
#define DS_CONTEXT_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/array.hpp>
#include "ds_request.h"

#ifdef _WIN32
#include <windows.h>
#include <dstorage.h>
#endif

namespace godot {

class DSContext : public Object {
	GDCLASS(DSContext, Object);

private:
	static DSContext *singleton;
	bool initialized = false;

	uint32_t staging_buffer_size_mb = 32;
	bool disable_gpu_decompression = false;
	uint16_t queue_capacity = DSTORAGE_MIN_QUEUE_CAPACITY;

#ifdef _WIN32
	IDStorageFactory *factory = nullptr;
	IDStorageQueue *memory_queue = nullptr;
#endif

protected:
	static void _bind_methods();

public:
	static DSContext *get_singleton();

	DSContext();
	~DSContext();

	bool is_supported() const;
	bool is_d3d12_backend() const;
	bool is_initialized() const;

	bool initialize_ds();
	void shutdown_ds();

	bool enqueue_request(Ref<DSRequest> p_request);
	void flush_batch();
	void cancel_requests_with_tag(uint64_t p_tag);

	// Settings getters/setters
	void set_staging_buffer_size_mb(uint32_t p_size_mb);
	uint32_t get_staging_buffer_size_mb() const;

	void set_disable_gpu_decompression(bool p_disable);
	bool get_disable_gpu_decompression() const;

	void set_queue_capacity(uint16_t p_capacity);
	uint16_t get_queue_capacity() const;
};

} // namespace godot

#endif // DS_CONTEXT_H
