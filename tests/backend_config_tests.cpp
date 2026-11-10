#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "core/ds_backend_config.h"

// The DirectStorage configuration call is one-shot and process-global, so the
// only cross-platform unit under test is the pure mapping that decides what it
// receives. This pins the invariant that regression-proofs the feature: the
// space-heuristic switch (GPU decompression) and the implementation switch
// (driver metacommand vs reference) are independent, so a machine can keep GPU
// decompression on while forcing the reference path to compare the two.
GDEX_TEST(backend_config, codec_switches_are_independent) {
	godot::DSBackendConfigOptions defaults;
	const godot::DSResolvedBackendConfig off = godot::resolve_backend_config(defaults);
	GDEX_EXPECT_TRUE(off.disable_telemetry); // always off for a game extension
	GDEX_EXPECT_FALSE(off.disable_gpu_decompression);
	GDEX_EXPECT_FALSE(off.disable_gpu_decompression_metacommand);
	GDEX_EXPECT_FALSE(off.disable_bypass_io);
	GDEX_EXPECT_FALSE(off.force_file_buffering);

	// Disabling GPU decompression must not silently disable the metacommand too.
	godot::DSBackendConfigOptions gpu_off;
	gpu_off.disable_gpu_decompression = true;
	const godot::DSResolvedBackendConfig gpu_off_resolved = godot::resolve_backend_config(gpu_off);
	GDEX_EXPECT_TRUE(gpu_off_resolved.disable_gpu_decompression);
	GDEX_EXPECT_FALSE(gpu_off_resolved.disable_gpu_decompression_metacommand);

	// The other direction: GPU decompression stays enabled.
	godot::DSBackendConfigOptions metacommand_off;
	metacommand_off.disable_gpu_decompression_metacommand = true;
	const godot::DSResolvedBackendConfig metacommand_off_resolved = godot::resolve_backend_config(metacommand_off);
	GDEX_EXPECT_FALSE(metacommand_off_resolved.disable_gpu_decompression);
	GDEX_EXPECT_TRUE(metacommand_off_resolved.disable_gpu_decompression_metacommand);

	// Both at once is a legal configuration (the reference path on the CPU).
	godot::DSBackendConfigOptions both;
	both.disable_gpu_decompression = true;
	both.disable_gpu_decompression_metacommand = true;
	const godot::DSResolvedBackendConfig both_resolved = godot::resolve_backend_config(both);
	GDEX_EXPECT_TRUE(both_resolved.disable_gpu_decompression);
	GDEX_EXPECT_TRUE(both_resolved.disable_gpu_decompression_metacommand);
}

// ForceFileBuffering is only honoured with BypassIO off, and selecting an I/O
// mode must never disturb the codec switches.
GDEX_TEST(backend_config, force_file_buffering_implies_bypassio_off) {
	godot::DSBackendConfigOptions options;
	options.force_file_buffering = true;
	options.disable_gpu_decompression_metacommand = true;
	const godot::DSResolvedBackendConfig resolved = godot::resolve_backend_config(options);
	GDEX_EXPECT_TRUE(resolved.force_file_buffering);
	GDEX_EXPECT_TRUE(resolved.disable_bypass_io);
	GDEX_EXPECT_FALSE(resolved.disable_gpu_decompression);
	GDEX_EXPECT_TRUE(resolved.disable_gpu_decompression_metacommand);
}

GDEX_TEST(backend_config, telemetry_flag_is_carried_through) {
	godot::DSBackendConfigOptions options;
	options.disable_telemetry = false;
	GDEX_EXPECT_FALSE(godot::resolve_backend_config(options).disable_telemetry);
}
