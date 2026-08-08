#ifndef DS_ERROR_HANDLER_H
#define DS_ERROR_HANDLER_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#ifdef _WIN32
#include <windows.h>
#include <dstorage.h>
#endif

namespace godot {

class DSErrorHandler : public Object {
	GDCLASS(DSErrorHandler, Object);

private:
	static DSErrorHandler *singleton;
	String last_error;
	bool error_flag = false;

#ifdef _WIN32
	HANDLE registered_wait_handle = nullptr;
	IDStorageQueue *observed_queue = nullptr;

	static VOID NTAPI error_callback(PVOID pData, BOOLEAN bTimeout);
#endif

protected:
	static void _bind_methods();

public:
	static DSErrorHandler *get_singleton();

	DSErrorHandler();
	~DSErrorHandler();

#ifdef _WIN32
	bool register_queue(IDStorageQueue *p_queue);
	void unregister_queue();
#endif

	bool has_error() const;
	String get_last_error() const;
	void clear_error();
	void report_error(const String &p_msg);
};

} // namespace godot

#endif // DS_ERROR_HANDLER_H
