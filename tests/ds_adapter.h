#pragma once
#include "gdextest/adapter.h"

struct DSExtensionAdapter : public gdextest::ExtensionAdapter {
    void on_initialize(godot::ModuleInitializationLevel level) override;
    void on_uninitialize(godot::ModuleInitializationLevel level) override;
    void on_ready(godot::Node *tree_node) override;
};
