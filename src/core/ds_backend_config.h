#ifndef DS_BACKEND_CONFIG_H
#define DS_BACKEND_CONFIG_H

namespace godot {

// Inputs to the one-shot DirectStorage configuration call. Deliberately free of
// dstorage.h so the mapping can be unit-tested on every platform: the Win32
// backend translates the resolved struct into DSTORAGE_CONFIGURATION(1) inside
// its own #ifdef _WIN32 block.
struct DSBackendConfigOptions {
	bool disable_gpu_decompression = false;
	bool disable_gpu_decompression_metacommand = false;
	bool force_file_buffering = false;
	bool disable_telemetry = true;
};

struct DSResolvedBackendConfig {
	bool disable_telemetry = true;
	bool disable_gpu_decompression = false;
	bool disable_gpu_decompression_metacommand = false;
	bool disable_bypass_io = false;
	bool force_file_buffering = false;
};

// Resolves the two codec switches independently on purpose. Coupling them (the
// previous behaviour) makes it impossible to keep GPU decompression enabled
// while forcing the reference implementation, which is exactly the comparison a
// user needs to decide whether the driver's optimized metacommand pays off.
inline DSResolvedBackendConfig resolve_backend_config(const DSBackendConfigOptions &p_options) {
	DSResolvedBackendConfig resolved;
	resolved.disable_telemetry = p_options.disable_telemetry;
	resolved.disable_gpu_decompression = p_options.disable_gpu_decompression;
	resolved.disable_gpu_decompression_metacommand = p_options.disable_gpu_decompression_metacommand;
	resolved.force_file_buffering = p_options.force_file_buffering;
	// ForceFileBuffering is only honoured with BypassIO off.
	resolved.disable_bypass_io = p_options.force_file_buffering;
	return resolved;
}

} // namespace godot

#endif // DS_BACKEND_CONFIG_H
