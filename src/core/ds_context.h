#ifndef DS_CONTEXT_H
#define DS_CONTEXT_H

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace godot {

class DSContext : public Object {
	GDCLASS(DSContext, Object);

private:
	static DSContext *singleton;
	bool initialized = false;

protected:
	static void _bind_methods();

public:
	static DSContext *get_singleton();

	DSContext();
	~DSContext();

	bool is_supported() const;
	bool initialize_ds();
	void shutdown_ds();
};

} // namespace godot

#endif // DS_CONTEXT_H
