#ifndef DS_WIN32_BACKEND_H
#define DS_WIN32_BACKEND_H

#include "../platform_backend.h"

namespace godot {

class Win32Backend : public PlatformBackend {
public:
	Win32Backend() = default;
	~Win32Backend() override = default;

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	void shutdown() override;

	bool enqueue_request(Ref<DSRequest> p_request) override;
	void submit_batch() override;
	void cancel_requests_with_tag(uint64_t p_tag) override;
};

} // namespace godot

#endif // DS_WIN32_BACKEND_H
