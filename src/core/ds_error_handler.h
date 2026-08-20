#ifndef DS_ERROR_HANDLER_H
#define DS_ERROR_HANDLER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <cstdint>
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

#ifdef _WIN32
	std::vector<IDStorageQueue *> observed_queues;
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
#endif

	bool has_error() const;
	String get_last_error() const;
	void clear_error();
	void report_error(const String &p_msg);
};

} // namespace godot

#endif // DS_ERROR_HANDLER_H
