#include "register_types.h"
#include "compression/gdeflate_compressor.h"
#include "compression/gdeflate_decompressor.h"
#include "compression/gdeflate_parallel.h"
#include "core/ds_build_provenance.h"
#include "core/ds_context.h"
#include "core/ds_crypto.h"
#include "core/ds_error_handler.h"
#include "core/ds_profiler.h"
#include "core/ds_request.h"
#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "integration/ds_resource_format_loader.h"
#include "integration/ds_texture_cache.h"
#include "integration/ds_texture_owner.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "core/ds_settings.h"

#ifdef _WIN32
#include "platform/win32/ds_win32_backend.h"
#endif

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

	// Migrate the pre-rename "directstorage/*" keys first: registering defaults
	// beforehand would occupy the new key and mask the user's legacy value.
	dsgd_settings::migrate_all();

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

	register_setting(dsgd_settings::key("staging_buffer_size_mb"), Variant::INT, 128, PROPERTY_HINT_RANGE, "16,2048,16");
	register_setting(dsgd_settings::key("disable_gpu_decompression"), Variant::BOOL, false, PROPERTY_HINT_NONE, "");
	register_setting(dsgd_settings::key("queue_capacity"), Variant::INT, 512, PROPERTY_HINT_RANGE, "128,8192,64");
	register_setting(dsgd_settings::key("auto_tune_for_integrated_gpu"), Variant::BOOL, true, PROPERTY_HINT_NONE, "");
	register_setting(dsgd_settings::key("texture_cache_enabled"), Variant::BOOL, true, PROPERTY_HINT_NONE, "");
	register_setting(dsgd_settings::key("texture_cache_max_entries"), Variant::INT, 128, PROPERTY_HINT_RANGE, "16,2048,16");
}

void initialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	register_directstorage_project_settings();

	// godot-cpp resolves an engine singleton lazily on first use; do it here, on the
	// main thread, so a loader worker that later dispatches GDeflate tile jobs cannot
	// race the binding's static initialisation. The pool capacity is resolved here too:
	// both values are process-wide and must not be read per decompression.
	(void)WorkerThreadPool::get_singleton();
	ds_gdeflate_init_pool_capacity();

	ClassDB::register_class<DSContext>();
	ClassDB::register_class<DSRequest>();
	ClassDB::register_class<DSErrorHandler>();
	ClassDB::register_class<DSCrypto>();
	// DSProfilerResult must be registered before DSProfiler. It was previously
	// registered inside DSProfiler::_bind_methods() which is re-entrant and unsafe.
	ClassDB::register_class<DSProfilerResult>();
	ClassDB::register_class<DSProfiler>();
	ClassDB::register_class<GDeflateCompressor>();
	ClassDB::register_class<GDeflateDecompressor>();
	ClassDB::register_class<DSTex>();
	ClassDB::register_class<DSPkg>();
	ClassDB::register_class<DSTextureCache>();
	ClassDB::register_class<DSResourceFormatLoader>();
	// Internal helper (not exposed to scripting): lifetime owner for device
	// texture RIDs the cache does not take (#0126).
	ClassDB::register_internal_class<DSTextureOwner>();
	DSTextureOwner::initialize();

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

	// One startup check, before the first asset can load: the editor and a debug
	// player run the debug binary while an export runs the release one, so a
	// stale release binary is otherwise invisible until an export behaves
	// differently from what was tested. Silent unless the shipped manifest proves
	// the two binaries were built from different sources.
	ds_check_binary_provenance();
}

void uninitialize_directstorage_accelerator_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Stop uncached-texture destructors from posting render-thread work before
	// any singleton is torn down. Their RIDs die with the device.
	DSTextureOwner::shutdown();

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

	// DirectStorage must be shut down BEFORE the flush below. The deferred
	// queue only holds RIDs that were held back because a request still
	// targeted them, so freeing them while the backend is live is exactly the
	// use-after-free the queue exists to avoid. shutdown_ds() is idempotent
	// (the destructor calls it again) and was_last_shutdown_clean() reports
	// whether its drain actually completed.
	bool ds_drain_clean = true;
	if (ds_context_singleton) {
		ds_context_singleton->shutdown_ds();
		ds_drain_clean = ds_context_singleton->was_last_shutdown_clean();
		memdelete(ds_context_singleton);
		ds_context_singleton = nullptr;
	}
	// Every process-wide D3D12 reference must be dropped here, while the engine is
	// alive and the loader lock is NOT held. Left to static destruction it would
	// run inside DLL_PROCESS_DETACH, where Release() destroys the D3D12 object
	// under the loader lock and the driver deadlocks.
#ifdef _WIN32
	ds_win32_release_static_d3d12_state();
#endif
	// The cache destructor (clear) can defer RIDs behind an in-flight request.
	// Nothing is writing any more, so release them here rather than letting
	// RenderingDevice::finalize report them as leaked. A drain timeout leaves
	// the queue untouched on purpose. (#0126 drain hardening)
	DSTextureCache::flush_deferred_rid_releases(ds_drain_clean);
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
