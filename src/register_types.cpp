#include "register_types.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include "core/ds_context.h"
#include "core/ds_error_handler.h"
#include "core/ds_profiler.h"
#include "core/ds_request.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "integration/ds_resource_format_loader.h"
#include "integration/ds_texture_cache.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

static DSContext *ds_context_singleton = nullptr;
static DSErrorHandler *ds_error_handler_singleton = nullptr;
static DSProfiler *ds_profiler_singleton = nullptr;
static DSTextureCache *ds_texture_cache_singleton = nullptr;
static Ref<DSResourceFormatLoader> ds_resource_format_loader;

static void register_directstorage_project_settings() {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		return;
	}

	auto register_setting = [ps](const StringName &p_name, Variant::Type p_type, const Variant &p_default, PropertyHint p_hint, const String &p_hint_string) {
		if (!ps->has_setting(p_name)) {
			ps->set_setting(p_name, p_default);
		}

		ps->set_initial_value(p_name, p_default);

		Dictionary property_info;
		property_info["name"] = p_name;
		property_info["type"] = static_cast<int>(p_type);
		property_info["hint"] = static_cast<int>(p_hint);
		property_info["hint_string"] = p_hint_string;
		ps->add_property_info(property_info);
		ps->set_as_basic(p_name, true);
	};

	register_setting("directstorage/staging_buffer_size_mb", Variant::INT, 128, PROPERTY_HINT_RANGE, "16,1024,16");
	register_setting("directstorage/disable_gpu_decompression", Variant::BOOL, false, PROPERTY_HINT_NONE, "");
	register_setting("directstorage/queue_capacity", Variant::INT, 512, PROPERTY_HINT_RANGE, "64,8192,64");
	register_setting("directstorage/texture_cache_enabled", Variant::BOOL, true, PROPERTY_HINT_NONE, "");
	register_setting("directstorage/texture_cache_max_entries", Variant::INT, 128, PROPERTY_HINT_RANGE, "16,2048,16");
}

void initialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	register_directstorage_project_settings();

	ClassDB::register_class<DSContext>();
	ClassDB::register_class<DSRequest>();
	ClassDB::register_class<DSErrorHandler>();
	ClassDB::register_class<DSProfiler>();
	ClassDB::register_class<GDeflateCompressor>();
	ClassDB::register_class<GDeflateDecompressor>();
	ClassDB::register_class<DSTex>();
	ClassDB::register_class<DSPkg>();
	ClassDB::register_class<DSTextureCache>();
	ClassDB::register_class<DSResourceFormatLoader>();

	ds_context_singleton = memnew(DSContext);
	ds_error_handler_singleton = memnew(DSErrorHandler);
	ds_profiler_singleton = memnew(DSProfiler);
	ds_texture_cache_singleton = memnew(DSTextureCache);

	Engine::get_singleton()->register_singleton("DSContext", DSContext::get_singleton());
	Engine::get_singleton()->register_singleton("DSErrorHandler", DSErrorHandler::get_singleton());
	Engine::get_singleton()->register_singleton("DSProfiler", DSProfiler::get_singleton());
	Engine::get_singleton()->register_singleton("DSTextureCache", DSTextureCache::get_singleton());

	ds_resource_format_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(ds_resource_format_loader, true);
}

void uninitialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	if (ds_resource_format_loader.is_valid()) {
		ResourceLoader::get_singleton()->remove_resource_format_loader(ds_resource_format_loader);
		ds_resource_format_loader.unref();
	}

	Engine *engine = Engine::get_singleton();
	if (engine) {
		if (engine->has_singleton("DSTextureCache")) {
			engine->unregister_singleton("DSTextureCache");
		}
		if (engine->has_singleton("DSProfiler")) {
			engine->unregister_singleton("DSProfiler");
		}
		if (engine->has_singleton("DSErrorHandler")) {
			engine->unregister_singleton("DSErrorHandler");
		}
		if (engine->has_singleton("DSContext")) {
			engine->unregister_singleton("DSContext");
		}
	}

	if (ds_texture_cache_singleton) {
		memdelete(ds_texture_cache_singleton);
		ds_texture_cache_singleton = nullptr;
	}
	if (ds_profiler_singleton) {
		memdelete(ds_profiler_singleton);
		ds_profiler_singleton = nullptr;
	}
	if (ds_error_handler_singleton) {
		memdelete(ds_error_handler_singleton);
		ds_error_handler_singleton = nullptr;
	}
	if (ds_context_singleton) {
		memdelete(ds_context_singleton);
		ds_context_singleton = nullptr;
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
