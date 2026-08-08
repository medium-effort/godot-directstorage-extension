#include "register_types.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include "core/ds_context.h"
#include "core/ds_error_handler.h"
#include "core/ds_profiler.h"
#include "core/ds_request.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static DSContext *ds_context_singleton = nullptr;
static DSErrorHandler *ds_error_handler_singleton = nullptr;
static DSProfiler *ds_profiler_singleton = nullptr;

void initialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ClassDB::register_class<DSContext>();
	ClassDB::register_class<DSRequest>();
	ClassDB::register_class<DSErrorHandler>();
	ClassDB::register_class<DSProfiler>();
	ClassDB::register_class<GDeflateCompressor>();
	ClassDB::register_class<GDeflateDecompressor>();
	ClassDB::register_class<DSTex>();
	ClassDB::register_class<DSPkg>();

	ds_context_singleton = memnew(DSContext);
	ds_error_handler_singleton = memnew(DSErrorHandler);
	ds_profiler_singleton = memnew(DSProfiler);

	Engine::get_singleton()->register_singleton("DSContext", DSContext::get_singleton());
	Engine::get_singleton()->register_singleton("DSErrorHandler", DSErrorHandler::get_singleton());
	Engine::get_singleton()->register_singleton("DSProfiler", DSProfiler::get_singleton());
}

void uninitialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	Engine *engine = Engine::get_singleton();
	if (engine) {
		if (engine->has_singleton("DSContext")) {
			engine->unregister_singleton("DSContext");
		}
		if (engine->has_singleton("DSErrorHandler")) {
			engine->unregister_singleton("DSErrorHandler");
		}
		if (engine->has_singleton("DSProfiler")) {
			engine->unregister_singleton("DSProfiler");
		}
	}

	if (ds_context_singleton) {
		memdelete(ds_context_singleton);
		ds_context_singleton = nullptr;
	}
	if (ds_error_handler_singleton) {
		memdelete(ds_error_handler_singleton);
		ds_error_handler_singleton = nullptr;
	}
	if (ds_profiler_singleton) {
		memdelete(ds_profiler_singleton);
		ds_profiler_singleton = nullptr;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT directstorage_accelerator_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_directstorage_accelerator_module);
	init_obj.register_terminator(uninitialize_directstorage_accelerator_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
