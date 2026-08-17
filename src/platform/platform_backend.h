#ifndef PLATFORM_BACKEND_H
#define PLATFORM_BACKEND_H

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>
#include "../core/ds_request.h"

namespace godot {

class PlatformBackend {
public:
	virtual ~PlatformBackend() = default;

	virtual bool is_available() const = 0;
	virtual String get_name() const = 0;

	virtual bool initialize() = 0;
	virtual void shutdown() = 0;

	virtual bool enqueue_request(Ref<DSRequest> p_request) = 0;
	virtual void submit_batch() = 0;
	virtual void submit_async() = 0;
	virtual bool is_batch_complete() const = 0;
	virtual void collect_completed_results() = 0;
	virtual void cancel_requests_with_tag(uint64_t p_tag) = 0;
};

} // namespace godot

#endif // PLATFORM_BACKEND_H
