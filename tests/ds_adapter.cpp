#include "ds_adapter.h"

#include "register_types.h"

// The test host's entry symbol (gdextest_library_init) only dispatches the
// adapter lifecycle; it never calls the extension's own module initializer.
// Because register_types.cpp is compiled INTO the test library (compile-in
// route), this adapter forwards the real module lifecycle so the DS classes
// (DSContext / DSTextureCache / DSResourceFormatLoader / ...) are registered
// in ClassDB and their singletons exist. Without this the engine-coupled
// suites abort with "Cannot get class 'DSTextureCache'".
// Prefer per-test ctx.add_teardown() over global cache cleanup.
// Note: TestContext::fail takes (file, line, msg) — never ctx.fail("...").

void DSExtensionAdapter::on_initialize(godot::ModuleInitializationLevel level) {
    initialize_directstorage_accelerator_module(level);
}

void DSExtensionAdapter::on_uninitialize(godot::ModuleInitializationLevel level) {
    uninitialize_directstorage_accelerator_module(level);
}

void DSExtensionAdapter::on_ready(godot::Node *tree_node) {
    (void)tree_node;
}

GDEX_REGISTER_ADAPTER(DSExtensionAdapter);
