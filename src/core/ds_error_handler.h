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
	// Serialises the two IDStorageQueue::RetrieveErrorRecord() callers (the drain below and the
	// backend's own error-event read) and bounds how long the drain may hold on to backend state.
	// Owned here rather than borrowed from a backend: this singleton outlives every Win32Backend,
	// so a drain can never reach a destroyed mutex, and unregister_queue() can wait for an
	// in-flight drain before teardown frees the queues (or unloads their runtime). Never held
	// while calling into the engine.
	std::mutex queue_drain_mutex;
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
	bool register_queue(IDStorageQueue *p_queue);
	void unregister_queue();
	void process_queue_errors();
	// Taken around IDStorageQueue::RetrieveErrorRecord() by both the drain and the backend's own
	// read, so the two callers are mutually exclusive. Returned by value: the caller owns a
	// correct RAII guard and the mutex lifetime is this singleton's, not a backend's.
	std::unique_lock<std::mutex> acquire_queue_drain_lock();
	// Non-blocking form. On return r_lock owns the mutex only if nothing was inside it.
	bool try_acquire_queue_drain_lock(std::unique_lock<std::mutex> &r_lock);
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
