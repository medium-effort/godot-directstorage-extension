#ifndef DS_ERROR_HANDLER_H
#define DS_ERROR_HANDLER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dstorage.h>
#endif

namespace godot {

class DSErrorHandler : public Object {
	GDCLASS(DSErrorHandler, Object);

private:
	static DSErrorHandler *singleton;
	mutable std::mutex error_mutex;
	String last_error;
	bool error_flag = false;
	// Monotonic count of report_error() calls. clear_error() never rewinds it, so a batch can
	// carry the value it saw at submit and detect an error that landed while it was in flight.
	uint64_t error_sequence = 0;

#ifdef _WIN32
	std::vector<IDStorageQueue *> observed_queues;
	// The backend's queue-access mutex, borrowed for the duration of a queue-error drain so the
	// two RetrieveErrorRecord() callers cannot run concurrently. Null until a backend registers
	// and cleared again on unregister.
	std::mutex *observed_queue_mutex = nullptr;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	std::mutex queue_reader_hook_mutex;
	std::function<void()> queue_reader_hook;
	void _debug_invoke_queue_reader_hook();
#endif
#endif

protected:
	static void _bind_methods();

public:
	static DSErrorHandler *get_singleton();
	static String describe_hresult(uint32_t p_hresult);
	static String describe_command_type(int32_t p_command_type);

	DSErrorHandler();
	~DSErrorHandler();

#ifdef _WIN32
	bool register_queue(IDStorageQueue *p_queue, std::mutex *p_queue_access_mutex = nullptr);
	void unregister_queue();
	void process_queue_errors();
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Test-only seam, never bound in _bind_methods(): the hook runs inside
	// process_queue_errors() after the queue-access lock is taken and before the first
	// RetrieveErrorRecord() call, so a test can probe that lock from another thread.
	void _debug_set_queue_error_reader_hook_for_testing(const std::function<void()> &p_hook);
#endif
#endif

	bool has_error() const;
	String get_last_error() const;
	uint64_t get_error_sequence() const;
	void clear_error();
	void report_error(const String &p_msg);
};

} // namespace godot

#endif // DS_ERROR_HANDLER_H
