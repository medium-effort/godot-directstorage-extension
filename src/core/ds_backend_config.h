#ifndef DS_BACKEND_CONFIG_H
#define DS_BACKEND_CONFIG_H

#include <algorithm>
#include <cstdint>

namespace godot {

// Inputs to the one-shot DirectStorage configuration call. Deliberately free of
// dstorage.h so the mapping can be unit-tested on every platform: the Win32
// backend translates the resolved struct into DSTORAGE_CONFIGURATION(1) inside
// its own #ifdef _WIN32 block.
struct DSBackendConfigOptions {
	bool disable_gpu_decompression = false;
	bool disable_gpu_decompression_metacommand = false;
	bool force_disable_bypass_io = false;
	// Derived from the asset volume: true when the bus/filesystem cannot provide
	// BypassIO (e.g. USB, SD, exFAT, virtual disks). Never set by user settings.
	bool bypassio_unavailable = false;
	bool force_file_buffering = false;
	bool disable_telemetry = true;
	// Maximum threads the runtime may use to decompress built-in formats on the
	// CPU (memory destinations always take this path, and GPU destinations do
	// when GPU decompression is off). 0 keeps the runtime's "system best guess".
	int32_t cpu_decompression_threads = 0;
};

struct DSResolvedBackendConfig {
	bool disable_telemetry = true;
	bool disable_gpu_decompression = false;
	bool disable_gpu_decompression_metacommand = false;
	bool disable_bypass_io = false;
	bool force_file_buffering = false;
	int32_t cpu_decompression_threads = 0;
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
	// ForceFileBuffering is only honoured with BypassIO off, and a volume that
	// cannot provide BypassIO must be configured for standard I/O explicitly.
	resolved.disable_bypass_io = p_options.force_disable_bypass_io ||
			p_options.bypassio_unavailable || p_options.force_file_buffering;
	// 0 means "runtime default"; a negative or absurd project setting must not
	// reach DSTORAGE_CONFIGURATION. 256 is far above any consumer core count and
	// still below the runtime's INT32 range.
	resolved.cpu_decompression_threads = std::clamp<int32_t>(p_options.cpu_decompression_threads, 0, 256);
	return resolved;
}

} // namespace godot

#endif // DS_BACKEND_CONFIG_H
