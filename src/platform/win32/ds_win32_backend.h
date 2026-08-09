#ifndef DS_WIN32_BACKEND_H
#define DS_WIN32_BACKEND_H

#include "../platform_backend.h"
#include <godot_cpp/templates/vector.hpp>

#ifdef _WIN32
#include <windows.h>
#include <dstorage.h>
#endif

namespace godot {

class Win32Backend : public PlatformBackend {
private:
#ifdef _WIN32
	IDStorageFactory *factory = nullptr;
	IDStorageQueue1 *memory_queue = nullptr;
	HANDLE batch_event = nullptr;
	HMODULE dstoragecore_module = nullptr;
	HMODULE dstorage_module = nullptr;
	Vector<Ref<DSRequest>> in_flight_requests;
	Vector<IDStorageFile *> open_files;
#endif

public:
	Win32Backend() = default;
	~Win32Backend() override { shutdown(); }

	bool is_available() const override;
	String get_name() const override;

	bool initialize() override;
	void shutdown() override;

	bool enqueue_request(Ref<DSRequest> p_request) override;
	void submit_batch() override;
	void cancel_requests_with_tag(uint64_t p_tag) override;

	static Dictionary get_drive_info_for_path(const String &p_path);
};

} // namespace godot

#endif // DS_WIN32_BACKEND_H
