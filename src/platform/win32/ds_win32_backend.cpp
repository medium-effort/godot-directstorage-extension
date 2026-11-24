#include "ds_win32_backend.h"
#include "ds_render_handoff_queue.h"
#include "../../core/ds_backend_config.h"
#include "../../core/ds_context.h"
#include "../../core/ds_cpu_budget.h"
#include "../../core/ds_drive_support.h"
#include "../../core/ds_error_handler.h"
#include "../../core/ds_gdeflate_stream.h"
#include "../../core/ds_profiler.h"
#include "core/ds_settings.h"
#include "../../formats/dstex_format.h"
#include "../../formats/dspkg_format.h"

#ifdef _WIN32
#include "TileStream.h"
#endif

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/thread.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef _WIN32
#include <winioctl.h>
#include <winver.h>
#include <dxgi.h>
#if defined(_MSC_VER)
#include <delayimp.h>
#endif
#endif

#include <cstdio>
#include <string>
#include <chrono>
#include <cstring>
#include <memory>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <algorithm>
#include <mutex>
#include <atomic>

namespace godot {

// In template_release (!DEBUG_ENABLED), godot-cpp's RenderingServer::get_singleton()
// passes nullptr to object_get_instance_binding() when RenderingServer is not
// registered yet (e.g. MODULE_INITIALIZATION_LEVEL_SCENE), crashing with 0xC0000005.
static RenderingServer *safe_get_rendering_server() {
	Engine *engine = Engine::get_singleton();
	if (!engine || !engine->has_singleton("RenderingServer")) {
		return nullptr;
	}
	return RenderingServer::get_singleton();
}

// Queue-admission observability. Process-wide because a process has one
// Win32Backend: rejected_admissions counts staging attempts refused for want of
// operation budget, peak_staged_ops is the high-water mark of
// (staged requests + EnqueueSignal + EnqueueSetEvent) seen in one batch slot.
static std::atomic<uint64_t> s_rejected_admission_count{ 0 };
static std::atomic<uint32_t> s_peak_staged_ops{ 0 };

static void ds_note_staged_ops(uint32_t p_ops) {
	uint32_t previous = s_peak_staged_ops.load(std::memory_order_relaxed);
	while (p_ops > previous &&
			!s_peak_staged_ops.compare_exchange_weak(previous, p_ops, std::memory_order_relaxed)) {
	}
}

#ifdef _WIN32
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Test-only override for the admission-refusal sentence (header seam). Native
// std::string, never a Godot object at static scope: a file-scope godot::String
// runs its constructor in DllMain, before gdextension_interface is valid, and
// the whole DLL then fails to load with Windows error 1114 (GODOT_QUIRKS Q06).
static std::string &ds_capacity_reject_override() {
	static std::string text;
	return text;
}
#endif

// One sentence for all four admission refusals: the retry classification
// travels beside it as an EnqueueOutcome, so rewording this text (or overriding
// it in a test) cannot change whether a request is retried.
static String ds_capacity_reject_message(uint32_t p_queue_capacity) {
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	if (!ds_capacity_reject_override().empty()) {
		return String(ds_capacity_reject_override().c_str());
	}
#endif
	return "Win32Backend queue capacity (" + String::num_uint64(p_queue_capacity) + " operations) exceeded for active batch slot; a batch may stage at most " + String::num_uint64(p_queue_capacity >= 2 ? p_queue_capacity - 2 : 0) + " requests plus its signal and event.";
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void Win32Backend::_set_capacity_reject_text_for_testing(const String &p_text) {
	ds_capacity_reject_override() = std::string(p_text.utf8().get_data());
}
#endif
#endif

void ds_profiler_set_batch_metrics(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes, int p_request_count, int64_t p_decompression_duration_us, int p_error_state);
void ds_profiler_set_batch_metrics_timed(uint64_t p_compressed_bytes, uint64_t p_uncompressed_bytes, int p_request_count, int64_t p_decompression_duration_us, int p_error_state, int64_t p_start_time_usec, int64_t p_end_time_usec);

#ifdef _WIN32
#ifndef FSCTL_MANAGE_BYPASS_IO
#define FSCTL_MANAGE_BYPASS_IO CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 274, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#ifdef FS_BPIO_INFLAG_NONE
using DSBpioInput = FS_BPIO_INPUT;
using DSBpioOutput = FS_BPIO_OUTPUT;
constexpr uint32_t DS_BPIO_OP_QUERY = FS_BPIO_OP_QUERY;
constexpr uint32_t DS_BPIO_OUTFLAG_VOLUME_STACK_BYPASS_PAUSED = 0x1U;
#else
struct DSBpioInput {
	uint32_t Operation;
	uint32_t InFlags;
	uint64_t Reserved1;
	uint64_t Reserved2;
};

struct DSBpioResults {
	int32_t OpStatus;
	uint16_t FailingDriverNameLen;
	wchar_t FailingDriverName[32];
	uint16_t FailureReasonLen;
	wchar_t FailureReason[128];
};

struct DSBpioOutput {
	uint32_t Operation;
	uint32_t OutFlags;
	uint64_t Reserved1;
	uint64_t Reserved2;
	union {
		DSBpioResults Enable;
		DSBpioResults Query;
		DSBpioResults VolumeStackResume;
		DSBpioResults StreamResume;
		uint8_t RawPayload[512];
	};
};

constexpr uint32_t DS_BPIO_OP_QUERY = 3U;
constexpr uint32_t DS_BPIO_OUTFLAG_VOLUME_STACK_BYPASS_PAUSED = 0x1U;
#endif
#endif

#ifdef _WIN32
struct DSRuntimeProbe {
	bool available = false;
	String reason;
};

// Configuration is applied to a *loaded runtime instance*, not once per process.
// The settings live inside dstorage.dll, so unloading the module discards them and
// the next load has to be configured again; a process-wide std::once_flag left every
// instance after the first running on SDK defaults (telemetry on, the GPU
// decompression preference ignored).
static std::mutex s_dstorage_config_mutex;
// The instance the marker names, as a (module, load epoch) pair. A release only drops one
// reference, and GetModuleHandleW() cannot tell "our last reference went away" from "the
// loader still has the DLL resident for a delay-load binding" - so identity is tracked by
// OUR own reference count instead. The epoch rises when that count goes 0 -> 1, i.e. a new
// instance cycle begins: a release that leaves another of our references alive cannot
// invalidate the configuration, while the next load after the last release re-applies it.
static HMODULE s_dstorage_configured_module = nullptr;
static uint64_t s_dstorage_configured_epoch = 0;
static uint64_t s_dstorage_load_epoch = 0;
static uint32_t s_dstorage_ref_count = 0;
static std::atomic<int> s_dstorage_config_apply_count{ 0 };
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Test-only: last byte count handed to SetStagingBufferSize (no factory getter).
static std::atomic<int64_t> s_last_applied_staging_bytes{ 0 };
#endif
// Desired I/O mode for the one-shot DStorageSetConfiguration call. Set from
// the asset volume's media class immediately before the runtime is configured,
// so a rotational volume can request ForceFileBuffering while flash keeps the
// default unbuffered path.
static std::atomic<bool> s_desired_file_buffering{ false };
// True when the asset volume cannot provide BypassIO (non-NVMe bus or non-NTFS
// filesystem). Fed into DSBackendConfigOptions before the one-shot runtime
// configuration, exactly like s_desired_file_buffering.
static std::atomic<bool> s_bypassio_unavailable{ false };
// Mode actually applied to s_dstorage_configured_module, for mismatch warnings.
static std::atomic<bool> s_applied_file_buffering{ false };

// Caller holds s_dstorage_config_mutex.
static bool has_live_dstorage_configuration_locked() {
	return s_dstorage_configured_module != nullptr &&
			s_dstorage_ref_count > 0 &&
			s_dstorage_configured_epoch == s_dstorage_load_epoch;
}

static void unload_runtime_modules(HMODULE p_core, HMODULE p_dstorage) {
	// Only the reference goes away. Whether the settings survive is decided by the load
	// epoch, not by this call: another owner may still hold the same instance.
	Win32Backend::release_runtime_module(p_dstorage);
	if (p_core) {
		FreeLibrary(p_core);
	}
}

// The one search policy for the DirectStorage runtime. DLL_LOAD_DIR covers the
// addon-bin layout, where dstorage.dll sits beside the dstoragecore.dll it loads
// at runtime; DEFAULT_DIRS covers the dependencies the SDK pulls from the system.
// The legacy-order locations that are not wanted here - the current directory and
// every PATH entry - are excluded by construction rather than by convention, so a
// dependency the shipped set does not provide fails the load instead of being
// satisfied by whatever happens to sit in the working directory.
static constexpr uint32_t kRuntimeModuleSearchFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
static std::mutex s_runtime_load_record_mutex;
static std::wstring s_last_runtime_load_path;
static uint32_t s_last_runtime_load_flags = 0;
#endif

HMODULE Win32Backend::load_runtime_module(const String &p_dir, const char *p_dll_name) {
	const String absolute_path = p_dir + String("/") + String(p_dll_name);
	std::wstring w_path(reinterpret_cast<const wchar_t *>(absolute_path.utf16().get_data()));
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	{
		// Recorded before the call so a failed attempt is visible to the seam too.
		std::lock_guard<std::mutex> lock(s_runtime_load_record_mutex);
		s_last_runtime_load_path = w_path;
		s_last_runtime_load_flags = kRuntimeModuleSearchFlags;
	}
#endif
	// Taking the lock across the load and the count update makes the epoch bump atomic
	// against release_runtime_module() and apply_dstorage_configuration().
	std::lock_guard<std::mutex> lock(s_dstorage_config_mutex);
	HMODULE module = LoadLibraryExW(w_path.c_str(), nullptr, kRuntimeModuleSearchFlags);
	if (module != nullptr && std::strcmp(p_dll_name, "dstorage.dll") == 0) {
		// An own-reference count, not GetModuleHandleW(): the loader can keep the DLL
		// resident for a delay-load binding long after we dropped our last reference.
		if (s_dstorage_ref_count == 0) {
			++s_dstorage_load_epoch;
		}
		++s_dstorage_ref_count;
	}
	return module;
}

#if defined(_WIN32) && defined(_MSC_VER)
static FARPROC WINAPI ds_delay_load_failure_hook(unsigned dliNotify, PDelayLoadInfo pdli) {
	if (dliNotify == dliFailLoadLib || dliNotify == dliFailGetProc) {
		UtilityFunctions::push_error(
				String("[DirectStorage] Delay-load failed for: ") +
				(pdli && pdli->szDll ? pdli->szDll : "unknown"));
	}
	return nullptr;
}
extern "C" const PfnDliHook __pfnDliFailureHook2 = ds_delay_load_failure_hook;
#endif

static const uint64_t GDEFLATE_PAGE_SIZE = 64ULL * 1024ULL;

// DXGI block-compressed formats: a row-range request must land on 4x4 block
// boundaries (top aligned, bottom aligned unless it is the mip edge), or the
// runtime's CopyTextureRegion rejects the box.
static bool dxgi_format_is_block_compressed(DXGI_FORMAT p_format) {
	switch (p_format) {
		case DXGI_FORMAT_BC1_TYPELESS:
		case DXGI_FORMAT_BC1_UNORM:
		case DXGI_FORMAT_BC1_UNORM_SRGB:
		case DXGI_FORMAT_BC2_TYPELESS:
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC2_UNORM_SRGB:
		case DXGI_FORMAT_BC3_TYPELESS:
		case DXGI_FORMAT_BC3_UNORM:
		case DXGI_FORMAT_BC3_UNORM_SRGB:
		case DXGI_FORMAT_BC4_TYPELESS:
		case DXGI_FORMAT_BC4_UNORM:
		case DXGI_FORMAT_BC4_SNORM:
		case DXGI_FORMAT_BC5_TYPELESS:
		case DXGI_FORMAT_BC5_UNORM:
		case DXGI_FORMAT_BC5_SNORM:
		case DXGI_FORMAT_BC6H_TYPELESS:
		case DXGI_FORMAT_BC6H_UF16:
		case DXGI_FORMAT_BC6H_SF16:
		case DXGI_FORMAT_BC7_TYPELESS:
		case DXGI_FORMAT_BC7_UNORM:
		case DXGI_FORMAT_BC7_UNORM_SRGB:
			return true;
		default:
			return false;
	}
}

// A row-range request addresses ONE subresource, so every box coordinate is
// subresource-relative: DSTORAGE_DESTINATION_TEXTURE_REGION::Region is in
// pixels of that subresource, and a mip's dimensions are
// max(1, resource dimension >> mip). The subresource index packs
// mip + array_slice * MipLevels, so the mip is the modulo. One definition keeps
// the validation site and the D3D12_BOX site from disagreeing.
static uint32_t subresource_mip_index(UINT p_first_subresource, UINT64 p_mip_levels) {
	const uint32_t levels = static_cast<uint32_t>(std::max<uint64_t>(1, p_mip_levels));
	return static_cast<uint32_t>(p_first_subresource % levels);
}

struct DSChunkBoundary {
	uint64_t source_offset = 0;
	uint64_t source_size = 0;
	uint64_t destination_offset = 0;
	uint64_t destination_size = 0;
};

bool Win32Backend::validate_dstex_chunk_range(uint64_t p_source_offset, uint64_t p_source_size, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t &r_resolved_offset) {
	if (p_source_size == 0 || p_payload_size > UINT64_MAX - p_payload_offset) {
		return false;
	}
	bool relative_range = (p_source_offset <= p_payload_size) && (p_source_size <= p_payload_size - p_source_offset);
	bool absolute_range = (p_source_offset >= p_payload_offset) &&
			(p_payload_size >= p_source_size) &&
			((p_source_offset - p_payload_offset) <= (p_payload_size - p_source_size));
	if (!relative_range && absolute_range) {
		r_resolved_offset = p_source_offset - p_payload_offset;
		return true;
	} else if (relative_range) {
		r_resolved_offset = p_source_offset;
		return true;
	}
	return false;
}

// Validates the 8-byte GDeflate tile-stream header against the uncompressed
// size the request declared (issue #0181). The declared size is what sizes a
// memory/buffer destination, while DirectStorage decodes the tiles the stream
// header itself declares - a mismatch therefore writes past the destination
// and still reports success with no error record. The CPU fallback applies the
// same rule in GDeflateDecompressor::validate_stream.
#ifdef _WIN32
namespace {
	// FileAccess handles reused by the fallback GDeflate header check. Direct
	// enqueue_request callers (and the corruption tests) are not pre-validated
	// by DSPreloader, so they still come through here. Reusing one handle per
	// path removes the per-mip CreateFileW/CloseHandle pair without weakening
	// the check; a changed mtime reopens the file.
	struct CachedValidationFile {
		Ref<FileAccess> file;
		uint64_t mtime = 0;
	};
	std::mutex g_gdeflate_validation_mutex;
	std::unordered_map<std::string, CachedValidationFile> g_gdeflate_validation_files;
	constexpr size_t kMaxCachedValidationFiles = 32;
} // namespace

void clear_gdeflate_validation_cache() {
	std::lock_guard<std::mutex> lock(g_gdeflate_validation_mutex);
	g_gdeflate_validation_files.clear();
}

static bool validate_gdeflate_stream_size(const String &p_path, uint64_t p_source_offset, uint64_t p_declared_size, String &r_error) {
	const std::string key = p_path.utf8().get_data();
	PackedByteArray raw_header;
	{
		std::lock_guard<std::mutex> lock(g_gdeflate_validation_mutex);
		const uint64_t mtime = FileAccess::get_modified_time(p_path);
		std::unordered_map<std::string, CachedValidationFile>::iterator it = g_gdeflate_validation_files.find(key);
		Ref<FileAccess> file;
		if (it != g_gdeflate_validation_files.end() && it->second.file.is_valid() &&
				it->second.file->is_open() && it->second.mtime == mtime) {
			file = it->second.file;
		} else {
			file = FileAccess::open(p_path, FileAccess::READ);
			if (file.is_null() || !file->is_open()) {
				r_error = "Win32Backend: Failed to open '" + p_path + "' to validate its GDeflate stream header.";
				return false;
			}
			if (g_gdeflate_validation_files.size() >= kMaxCachedValidationFiles) {
				g_gdeflate_validation_files.clear();
			}
			CachedValidationFile entry;
			entry.file = file;
			entry.mtime = mtime;
			g_gdeflate_validation_files[key] = entry;
		}
		file->seek(static_cast<int64_t>(p_source_offset));
		if (file->get_position() != p_source_offset) {
			r_error = "Win32Backend: Cannot seek to GDeflate payload offset " + String::num_uint64(p_source_offset) + " in '" + p_path + "'.";
			return false;
		}
		// Held under the same mutex as the cached handle: two threads must not
		// seek the shared FileAccess concurrently. The buffer is our own copy.
		raw_header = file->get_buffer(static_cast<int64_t>(sizeof(GDeflate::TileStream)));
	}
	return dsgd_gdeflate::validate_stream_header(
			reinterpret_cast<const uint8_t *>(raw_header.ptr()), static_cast<size_t>(raw_header.size()),
			p_source_offset, p_declared_size, "Win32Backend: ", p_path, r_error);
}
#endif // _WIN32

// LEGACY LAYOUT SUPPORT - not a general-purpose splitter.
//
// Chunked payloads became the importer default, so a mip carries its own chunk
// table and the loader issues one request per chunk. This splitter only exists
// for assets that still store a whole mip as one stream: v1/v2 files and v3
// files written with chunk_size_kb = 0. It deliberately refuses chunked
// payloads (see the "chunked" guard below) - do NOT add a chunked branch here,
// or an already-chunked mip would be split twice and the row offsets would
// drift. Retire it after an import migration, not because chunking became the
// default.
static bool get_dstex_mip_chunks(const String &p_path, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t p_capacity, Vector<DSChunkBoundary> &r_chunks) {
	if (p_path.get_extension().to_lower() != "dstex") {
		return false;
	}

	Ref<DSTex> dstex;
	dstex.instantiate();
	Dictionary header = dstex->read_header(p_path);
	if (header.is_empty() || !header.has("mip_count")) {
		return false;
	}
	// Chunked v3 payloads are not contiguous; the staging splitter must not
	// treat a chunk table sum as one source range.
	if (header.has("chunked") && static_cast<bool>(header["chunked"])) {
		return false;
	}
	if (((int)header.get("compression_format", (int)DSCompressionFormat::GDEFLATE) != (int)DSCompressionFormat::GDEFLATE &&
				(int)header.get("compression_format", (int)DSCompressionFormat::GDEFLATE) != (int)DSCompressionFormat::NONE) ||
			(int)header.get("gacl_transform", (int)DSGaclTransform::NONE) != (int)DSGaclTransform::NONE) {
		// The splitter emits children in the parent's codec; Zstd/GACL payloads
		// must wait for Step 4's request fields.
		return false;
	}
	uint32_t mip_count = header["mip_count"];
	if (mip_count == 0) {
		return false;
	}

	// The mip table comes from DSTex::read_header(), which located it with
	// dstex_header_size(version) and already validated bounds, per-version
	// alignment, chunk counts and the per-mip footprint. Re-reading the file here
	// would reintroduce a second parser that must be kept in sync with the header
	// layout, which is exactly how a v3 file came to be decoded with a v2 offset.
	const PackedInt64Array mip_offsets = header["mip_offsets"];
	const PackedInt32Array mip_sizes = header["mip_sizes"];
	const PackedInt32Array mip_uncompressed_sizes = header["mip_uncompressed_sizes"];
	if (mip_offsets.size() != static_cast<int64_t>(mip_count) ||
			mip_sizes.size() != static_cast<int64_t>(mip_count) ||
			mip_uncompressed_sizes.size() != static_cast<int64_t>(mip_count)) {
		return false;
	}

	uint64_t destination_offset = 0;
	for (uint32_t mip_index = 0; mip_index < mip_count; mip_index++) {
		// A v1 mip record has no uncompressed size, so read_header leaves this
		// table zero-filled for v1 files. This splitter sizes destinations from
		// that table, so it must refuse v1 rather than take a size from another
		// field.
		const int32_t compressed_size = mip_sizes[mip_index];
		const int32_t uncompressed_size = mip_uncompressed_sizes[mip_index];
		if (compressed_size <= 0 || uncompressed_size <= 0) {
			r_chunks.clear();
			return false;
		}
		uint64_t source_offset = static_cast<uint64_t>(mip_offsets[mip_index]);
		uint64_t source_size = static_cast<uint64_t>(compressed_size);
		uint64_t destination_size = static_cast<uint64_t>(uncompressed_size);
		if (destination_size > p_capacity) {
			r_chunks.clear();
			return false;
		}
		uint64_t resolved_offset = 0;
		if (!Win32Backend::validate_dstex_chunk_range(source_offset, source_size, p_payload_offset, p_payload_size, resolved_offset)) {
			r_chunks.clear();
			return false;
		}
		source_offset = resolved_offset;

		DSChunkBoundary boundary;
		boundary.source_offset = source_offset;
		boundary.source_size = source_size;
		boundary.destination_offset = destination_offset;
		boundary.destination_size = destination_size;
		// Never merge consecutive mips: each mip is an independent GDeflate TileStream.
		r_chunks.push_back(boundary);
		destination_offset += destination_size;
	}
	return !r_chunks.is_empty();
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Test-only view of the legacy unchunked splitter above, so the v1/v2 layout
// tests can assert row boundaries without hardware. Not a production path.
Dictionary Win32Backend::_debug_get_dstex_mip_chunks_for_testing(const String &p_path, uint64_t p_payload_offset, uint64_t p_payload_size, uint64_t p_capacity) {
	Vector<DSChunkBoundary> chunks;
	if (!get_dstex_mip_chunks(p_path, p_payload_offset, p_payload_size, p_capacity, chunks)) {
		return Dictionary();
	}
	PackedInt64Array source_offsets;
	PackedInt64Array source_sizes;
	PackedInt64Array destination_offsets;
	PackedInt64Array destination_sizes;
	for (int64_t i = 0; i < chunks.size(); ++i) {
		source_offsets.push_back(static_cast<int64_t>(chunks[i].source_offset));
		source_sizes.push_back(static_cast<int64_t>(chunks[i].source_size));
		destination_offsets.push_back(static_cast<int64_t>(chunks[i].destination_offset));
		destination_sizes.push_back(static_cast<int64_t>(chunks[i].destination_size));
	}
	Dictionary result;
	result["source_offsets"] = source_offsets;
	result["source_sizes"] = source_sizes;
	result["destination_offsets"] = destination_offsets;
	result["destination_sizes"] = destination_sizes;
	return result;
}

Dictionary Win32Backend::_debug_get_queue_admission_stats_for_testing() {
	Dictionary result;
	result["rejected_admissions"] = static_cast<int64_t>(s_rejected_admission_count.load(std::memory_order_relaxed));
	result["peak_staged_ops"] = static_cast<int64_t>(s_peak_staged_ops.load(std::memory_order_relaxed));
	return result;
}
#endif

// Unauthorized-archive log dedup state. Native, trivially destructible types only:
// a godot::String here would destruct at DLL unload, after Godot's allocator is
// gone (GODOT_QUIRKS.md Section 6). Mirrors DSPkg's own dedup state.
static std::mutex s_backend_unauth_log_mutex;
static uint64_t s_last_backend_unauth_hash = 0;
static bool s_has_backend_unauth_path = false;

bool Win32Backend::get_texture_mip_entries(const String &p_path, uint64_t p_offset, Vector<DSTexMipEntryV2> &r_entries, DSCompressionFormat *r_container_format, int64_t p_single_mip_index) {
	r_entries.clear();
	String resolved_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (resolved_path.begins_with("res://") || resolved_path.begins_with("user://"))) {
		resolved_path = ps->globalize_path(resolved_path);
	}
	String ext = resolved_path.get_extension().to_lower();
	if (ext == "dstex") {
		Ref<FileAccess> file = FileAccess::open(resolved_path, FileAccess::READ);
		if (file.is_null() || file->get_length() < sizeof(DSTexHeader)) {
			return false;
		}

		PackedByteArray header_data = file->get_buffer(sizeof(DSTexHeader));
		if (header_data.size() != static_cast<int64_t>(sizeof(DSTexHeader))) {
			return false;
		}
		DSTexHeader header = {};
		std::memcpy(&header, header_data.ptr(), sizeof(header));
		if (header.magic != 0x58455444 || (header.version != 2 && header.version != 3) ||
				header.mip_count == 0 || header.mip_count > DSTEX_MAX_MIP_COUNT ||
				header.compressed_size == 0 || header.original_size == 0 ||
				(header.flags & ~(DSTEX_FLAG_SRGB | DSTEX_FLAG_AS_IMAGE_TEXTURE)) != 0 ||
				!dstex_sizes_are_sane(header.width, header.height, header.original_size, header.compressed_size) ||
				header.mip_count > (std::numeric_limits<uint64_t>::max() - sizeof(DSTexHeader)) / sizeof(DSTexMipEntryV2)) {
			return false;
		}

		const bool is_v3 = header.version == 3;
		uint64_t capabilities = 0;
		if (!is_v3) {
			if (header.reserved != 0) {
				return false;
			}
		} else {
			if (file->get_length() < sizeof(DSTexHeaderV3)) {
				return false;
			}
			PackedByteArray ext_data = file->get_buffer(sizeof(DSTexHeaderV3Ext));
			if (ext_data.size() != static_cast<int64_t>(sizeof(DSTexHeaderV3Ext))) {
				return false;
			}
			DSTexHeaderV3Ext v3_ext = {};
			std::memcpy(&v3_ext, ext_data.ptr(), sizeof(v3_ext));
			if (v3_ext.reserved0 != 0) {
				return false;
			}
			if (v3_ext.logical_width > header.width || v3_ext.logical_height > header.height) {
				return false;
			}
			if (v3_ext.depth_or_layers > 1) {
				return false; // Fail closed until 3D/array textures are wired to GPU DMA
			}
			capabilities = header.reserved;
			if ((capabilities & ~DSTEX_CAP_KNOWN_MASK) != 0) {
				return false;
			}
			const DSCompressionFormat format = DSTex::get_compression_format(capabilities);
			const DSGaclTransform gacl = DSTex::get_gacl_transform(capabilities);
			if (!ds_compression_format_is_valid(static_cast<uint8_t>(format)) ||
					!ds_gacl_transform_is_valid(static_cast<uint8_t>(gacl)) ||
					!DSTex::gacl_transform_matches_pixel_format(gacl, static_cast<int>(header.pixel_format)) ||
					(gacl != DSGaclTransform::NONE && format == DSCompressionFormat::NONE)) {
				return false;
			}
			if (format == DSCompressionFormat::NONE && header.compressed_size != header.original_size) {
				return false;
			}
			// GPU staging decodes one GDeflate frame per mip, or copies raw NONE
			// bytes. Zstd, GACL-shuffled and chunked payloads must fail closed
			// until Step 4 wires their request/destination paths.
			if ((format != DSCompressionFormat::GDEFLATE && format != DSCompressionFormat::NONE) ||
					gacl != DSGaclTransform::NONE) {
				return false;
			}
		}

		uint64_t file_length = static_cast<uint64_t>(file->get_length());
		uint64_t header_size = dstex_header_size(header.version);
		uint64_t mip_table_end = header_size + static_cast<uint64_t>(header.mip_count) * sizeof(DSTexMipEntryV2);
		if (mip_table_end > file_length) {
			return false;
		}
		r_entries.resize(header.mip_count);
		for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
			PackedByteArray entry_data = file->get_buffer(sizeof(DSTexMipEntryV2));
			if (entry_data.size() != static_cast<int64_t>(sizeof(DSTexMipEntryV2))) {
				r_entries.clear();
				return false;
			}
			std::memcpy(&r_entries.write[mip_index], entry_data.ptr(), sizeof(DSTexMipEntryV2));
			const DSTexMipEntryV2 &entry = r_entries[mip_index];
			if (!is_v3 && entry.reserved != 0) {
				r_entries.clear();
				return false;
			}
			if (is_v3 && (entry.chunk_count == 0 || entry.chunk_count > DSTEX_MAX_CHUNKS_PER_MIP)) {
				r_entries.clear();
				return false;
			}
			if (is_v3 && (entry.row_pitch == 0 || entry.row_pitch % 256 != 0 ||
					entry.uncompressed_size == 0 ||
					entry.uncompressed_size % entry.row_pitch != 0)) {
				// row_pitch == 0 must short-circuit before the modulo above; the
				// v3 chunk loop divides by this value.
				r_entries.clear();
				return false;
			}
		}

		if (is_v3) {
			if (p_single_mip_index >= 0) {
				if (static_cast<uint64_t>(p_single_mip_index) >= header.mip_count ||
						r_entries[static_cast<int64_t>(p_single_mip_index)].chunk_count > 1) {
					r_entries.clear();
					return false;
				}
			} else {
				for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
					if (r_entries[mip_index].chunk_count > 1) {
						// Multi-mip whole-texture requests cannot target chunked mips.
						r_entries.clear();
						return false;
					}
				}
			}
		}

		// Same structural contract as DSTex::read_header/preloader: each mip
		// footprint must match the declared layout and the table must add up to
		// the header totals.
		if (!DSTex::mip_table_is_sane(header, r_entries.ptr())) {
			r_entries.clear();
			return false;
		}

		// v3 chunk tables follow the whole mip table. They are part of the TOC
		// envelope, so entries_end must include them before any payload starts.
		std::vector<std::vector<DSChunkEntry>> chunk_tables;
		uint64_t total_chunk_entries = 0;
		if (is_v3) {
			chunk_tables.resize(header.mip_count);
			for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
				const uint32_t chunk_count = r_entries[mip_index].chunk_count;
				if (chunk_count > 1) {
					if (total_chunk_entries > std::numeric_limits<uint64_t>::max() - chunk_count) {
						r_entries.clear();
						return false;
					}
					total_chunk_entries += chunk_count;
				}
			}
		}
		uint64_t chunk_table_end = 0;
		if (total_chunk_entries > (std::numeric_limits<uint64_t>::max() - mip_table_end) / sizeof(DSChunkEntry)) {
			r_entries.clear();
			return false;
		}
		chunk_table_end = mip_table_end + total_chunk_entries * sizeof(DSChunkEntry);
		if (chunk_table_end > file_length) {
			r_entries.clear();
			return false;
		}

		if (is_v3) {
			const DSCompressionFormat format = DSTex::get_compression_format(capabilities);
			bool any_chunked = false;
			for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
				const DSTexMipEntryV2 &mip_entry = r_entries[mip_index];
				const uint32_t chunk_count = mip_entry.chunk_count;
				if (chunk_count == 1) {
					continue;
				}
				any_chunked = true;
				chunk_tables[mip_index].resize(chunk_count);
				uint64_t sum_compressed = 0;
				uint64_t sum_uncompressed = 0;
				for (uint32_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
					PackedByteArray chunk_data = file->get_buffer(sizeof(DSChunkEntry));
					if (chunk_data.size() != static_cast<int64_t>(sizeof(DSChunkEntry))) {
						r_entries.clear();
						return false;
					}
					DSChunkEntry &chunk = chunk_tables[mip_index][chunk_index];
					std::memcpy(&chunk, chunk_data.ptr(), sizeof(DSChunkEntry));
					if (chunk.compressed_size == 0 || chunk.uncompressed_size == 0 ||
							chunk.uncompressed_size % mip_entry.row_pitch != 0 ||
							chunk.offset > file_length ||
							chunk.compressed_size > file_length - chunk.offset ||
							chunk.offset % (chunk_index == 0 ? 512 : 8) != 0 ||
							(chunk_index == 0 && chunk.offset != mip_entry.offset) ||
							(format == DSCompressionFormat::NONE && chunk.compressed_size != chunk.uncompressed_size)) {
						r_entries.clear();
						return false;
					}
					sum_compressed += chunk.compressed_size;
					sum_uncompressed += chunk.uncompressed_size;
				}
				if (sum_compressed != mip_entry.compressed_size ||
						sum_uncompressed != mip_entry.uncompressed_size) {
					r_entries.clear();
					return false;
				}
			}
			if (any_chunked != ((capabilities & DSTEX_CAP_CHUNKED) != 0)) {
				r_entries.clear();
				return false;
			}
		}

		uint64_t compressed_size = 0;
		uint64_t uncompressed_size = 0;
		uint64_t previous_end = chunk_table_end;
		for (uint32_t mip_index = 0; mip_index < header.mip_count; mip_index++) {
			const DSTexMipEntryV2 &entry = r_entries[mip_index];
			if (entry.compressed_size == 0 || entry.uncompressed_size == 0 ||
					entry.row_pitch == 0 || entry.row_pitch % 256 != 0 ||
					entry.offset % 512 != 0 ||
					entry.offset > file_length ||
					entry.compressed_size > file_length - entry.offset ||
					compressed_size > UINT64_MAX - entry.compressed_size ||
					uncompressed_size > UINT64_MAX - entry.uncompressed_size) {
				r_entries.clear();
				return false;
			}
			if (entry.offset < previous_end) {
				r_entries.clear();
				return false;
			}
			if (!is_v3 || entry.chunk_count == 1) {
				previous_end = entry.offset + entry.compressed_size;
			} else {
				for (const DSChunkEntry &chunk : chunk_tables[mip_index]) {
					if (chunk.offset < previous_end) {
						r_entries.clear();
						return false;
					}
					previous_end = chunk.offset + chunk.compressed_size;
				}
			}
			compressed_size += entry.compressed_size;
			uncompressed_size += entry.uncompressed_size;
		}
		if (compressed_size != header.compressed_size || uncompressed_size != header.original_size) {
			r_entries.clear();
			return false;
		}
		if (r_container_format != nullptr) {
			*r_container_format = is_v3 ? DSTex::get_compression_format(capabilities) : DSCompressionFormat::GDEFLATE;
		}
		return true;
	}

	if (ext == "dspkg") {
		// Reuse the cached package for the active archive path instead of
		// reopening/reparsing the .dspkg on every enqueue. Path switch drops
		// the old package; open failure clears the cache and returns false.
		Ref<DSPkg> cached_package;
		String cached_path;
		{
			std::lock_guard<std::mutex> cache_lock(dspkg_cache_mutex);
			cached_package = dspkg_cache.package;
			cached_path = dspkg_cache.path;
		}
		if (cached_package.is_null() || cached_path != resolved_path) {
			Ref<DSPkg> fresh;
			fresh.instantiate();
			// 0 means the archive is unpaired; pairing is the random archive id,
			// compared in full 64-bit width when the expected value is non-zero.
			uint64_t expected_fingerprint = 0;
			if (dsgd_settings::has_setting("archive_id", "export/archive_id")) {
				expected_fingerprint = static_cast<uint64_t>(dsgd_settings::get_setting("archive_id", "export/archive_id"));
			}
			Error open_err = (fresh.is_valid()) ? fresh->open(resolved_path, expected_fingerprint) : ERR_CANT_CREATE;
			if (open_err != OK) {
				{
					std::lock_guard<std::mutex> cache_lock(dspkg_cache_mutex);
					dspkg_cache.package.unref();
					dspkg_cache.path = String();
				}
				if (open_err == ERR_UNAUTHORIZED) {
					std::lock_guard<std::mutex> report_lock(s_backend_unauth_log_mutex);
					const uint64_t path_hash = resolved_path.hash();
					if (!s_has_backend_unauth_path || s_last_backend_unauth_hash != path_hash) {
						s_has_backend_unauth_path = true;
						s_last_backend_unauth_hash = path_hash;
						UtilityFunctions::push_error(vformat("DirectStorage Win32 Backend: Decryption unauthorized or key mismatch for archive '%s'. The archive key is read from res://.dspkg/toc.key inside the game's encrypted PCK, so the PCK and the archive must ship as a pair.", resolved_path));
					}
				}
				return false;
			}
			{
				std::lock_guard<std::mutex> cache_lock(dspkg_cache_mutex);
				dspkg_cache.package = fresh;
				dspkg_cache.path = resolved_path;
			}
			cached_package = fresh;
		}
		// O(1) direct lookup by absolute payload offset
		// (header.data_offset + entry.data_offset). Miss (false) means a
		// legacy entry without GPU mip metadata or an unknown offset — fall
		// back to the safe error path in enqueue_request, no crash.
		Vector<DSPkgMipEntry> package_entries;
		uint8_t entry_format = 0;
		uint8_t entry_gacl = 0;
		bool entry_chunked = false;
		if (p_single_mip_index >= 0) {
			if (!cached_package->get_gpu_entry_for_mip_offset(p_offset, static_cast<uint32_t>(p_single_mip_index),
						package_entries, entry_format, entry_gacl, entry_chunked) ||
					package_entries.is_empty() ||
					p_single_mip_index >= package_entries.size() ||
					package_entries[p_single_mip_index].chunk_count > 1) {
				return false;
			}
		} else {
			if (!cached_package->get_mip_entries_for_offset(p_offset, package_entries) || package_entries.is_empty()) {
				return false;
			}
			if (!cached_package->get_entry_features_for_offset(p_offset, entry_format, entry_gacl, entry_chunked) ||
					entry_chunked) {
				return false;
			}
		}
		if ((entry_format != static_cast<uint8_t>(DSCompressionFormat::GDEFLATE) &&
					entry_format != static_cast<uint8_t>(DSCompressionFormat::NONE)) ||
				entry_gacl != static_cast<uint8_t>(DSGaclTransform::NONE)) {
			// The un-chunked GPU staging path can only submit one GDeflate stream
			// or a raw NONE block per mip. Zstd/GACL entries fail closed.
			return false;
		}
		r_entries.resize(package_entries.size());
		for (int mip_idx = 0; mip_idx < package_entries.size(); mip_idx++) {
			const DSPkgMipEntry &package_entry = package_entries[mip_idx];
			r_entries.write[mip_idx].offset = package_entry.offset;
			r_entries.write[mip_idx].compressed_size = package_entry.compressed_size;
			r_entries.write[mip_idx].uncompressed_size = package_entry.uncompressed_size;
			r_entries.write[mip_idx].row_pitch = package_entry.row_pitch;
			r_entries.write[mip_idx].reserved = package_entry.reserved;
		}
		if (r_container_format != nullptr) {
			*r_container_format = static_cast<DSCompressionFormat>(entry_format);
		}
		return true;
	}

	return false;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Test-only seam state (never bound in _bind_methods()). The hook runs immediately
// before a captured queue error is pushed to the engine, so a test can prove the
// push happens with batch_mutex released.
static std::mutex s_queue_error_report_hook_mutex;
static std::function<void()> s_queue_error_report_hook;
static std::atomic<int> s_error_record_read_count{ 0 };
#endif

Win32Backend::QueueErrorInfo Win32Backend::read_queue_error(IDStorageQueue *p_queue) {
	QueueErrorInfo info;
	if (!p_queue) {
		return info;
	}

	DSTORAGE_ERROR_RECORD error_record = {};
	// Serialise with DSErrorHandler's drain. Both backend callers already hold batch_mutex
	// (handle_queue_error_event and slot completion), so the order is batch_mutex -> the
	// handler's drain lock; the drain side takes only the drain lock, so the pair cannot invert.
	// Held across the SDK call and nothing else.
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	std::unique_lock<std::mutex> drain_lock;
	if (err_handler) {
		drain_lock = err_handler->acquire_queue_drain_lock();
	}
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	s_error_record_read_count.fetch_add(1);
#endif
	p_queue->RetrieveErrorRecord(&error_record);
	if (drain_lock.owns_lock()) {
		drain_lock.unlock();
	}
	if (error_record.FailureCount == 0) {
		return info;
	}

	info.present = true;
	const DSTORAGE_ERROR_FIRST_FAILURE &failure = error_record.FirstFailure;
	String hresult_desc = DSErrorHandler::describe_hresult(static_cast<uint32_t>(failure.HResult));
	if (hresult_desc.is_empty()) {
		hresult_desc = "No system message is available for this DirectStorage HRESULT.";
	}

	String command_type_str = DSErrorHandler::describe_command_type(static_cast<int32_t>(failure.CommandType));

	info.message = "DirectStorage queue error (HRESULT 0x" +
			String::num_uint64(static_cast<uint32_t>(failure.HResult), 16) +
			", command type " + command_type_str + "): " +
			hresult_desc;

	if (failure.CommandType == DSTORAGE_COMMAND_TYPE_REQUEST) {
		String req_name_str = String(failure.Request.RequestName);
		info.failed_request_name = req_name_str;
		info.failed_filename = String(failure.Request.Filename);
	}

	const HRESULT hr_aborted = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
	if (failure.HResult == hr_aborted) {
		// Cancellation via CancelRequestsWithTag() is an intentional application
		// event, not a fatal failure: log verbosely, never fail the batch for it.
		info.fatal = false;
	}
	return info;
}

// Renders the record exactly once: base line, then the request bracket (the only
// place that text is built), then the attribution details. Re-rendering the same
// record therefore cannot duplicate anything, which is what the enrichment
// callers rely on.
String Win32Backend::QueueErrorInfo::format_message() const {
	String text = message;
	if (!failed_request_name.is_empty()) {
		const String filename = failed_filename.is_empty() ? resolved_file_path : failed_filename;
		text += " [File: " + filename + ", RequestName: " + failed_request_name + "]";
	}
	text += request_details;
	return text;
}

void Win32Backend::enrich_queue_error_from_slot(const BatchSlot &p_slot, QueueErrorInfo &r_error) {
	if (!r_error.present || r_error.failed_request_name.is_empty()) {
		return;
	}
	const CharString utf8_name = r_error.failed_request_name.utf8();
	const auto it = p_slot.request_name_map.find(utf8_name.get_data());
	if (it == p_slot.request_name_map.end() || it->second.is_null()) {
		return;
	}
	const Ref<DSRequest> &req = it->second;
	// Attribution is assignment, never text rewriting: calling this twice for the
	// same record (the queue-error event thread and the batch claim both can) must
	// produce the same message.
	if (r_error.failed_filename.is_empty()) {
		r_error.resolved_file_path = req->get_file_path();
	}
	String details = " (dest=" + String::num_int64(static_cast<int64_t>(req->get_destination_type())) +
			", offset=" + String::num_uint64(req->get_offset()) +
			", size=" + String::num_uint64(req->get_size()) +
			", uncomp=" + String::num_uint64(req->get_uncompressed_size()) +
			", mip=" + String::num_uint64(req->get_first_subresource()) +
			", count=" + String::num_uint64(req->get_subresource_count());
	if (req->has_texture_row_region()) {
		details += ", region=[" + String::num_uint64(req->get_texture_region_top()) +
				", " + String::num_uint64(req->get_texture_region_bottom()) + ")";
	}
	const String label = req->get_debug_label();
	if (!label.is_empty()) {
		details += ", asset=" + label;
	}
	details += ")";
	r_error.request_details = details;
}

void Win32Backend::enrich_queue_error_from_queue_locked(const QueueState &p_queue, QueueErrorInfo &r_error) {
	if (!r_error.present || r_error.failed_request_name.is_empty()) {
		return;
	}
	for (int slot_index = 0; slot_index < p_queue.batch_slots.size(); slot_index++) {
		const BatchSlot &slot = p_queue.batch_slots[slot_index];
		const CharString utf8_name = r_error.failed_request_name.utf8();
		if (slot.request_name_map.find(utf8_name.get_data()) != slot.request_name_map.end()) {
			enrich_queue_error_from_slot(slot, r_error);
			return;
		}
	}
}

void Win32Backend::report_queue_error(const QueueErrorInfo &p_error) {
	if (!p_error.present) {
		return;
	}
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	std::function<void()> hook;
	{
		std::lock_guard<std::mutex> lock(s_queue_error_report_hook_mutex);
		hook = s_queue_error_report_hook;
	}
	if (hook) {
		hook();
	}
#endif
	const String text = p_error.format_message();
	if (!p_error.fatal) {
		UtilityFunctions::print_verbose("[DirectStorage] Request aborted on queue: " + text);
		return;
	}
	// Engine calls happen after every extension mutex is released (GODOT_QUIRKS.md Q46).
	UtilityFunctions::push_error(text);
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->report_error(text);
	}
}

void Win32Backend::report_batch_outcome(const BatchReport &p_report) {
	for (const String &warning : p_report.warnings) {
		UtilityFunctions::push_warning(warning);
	}
	report_queue_error(p_report.error);
}

void Win32Backend::release_resource_guards(const Vector<Ref<DSRequest>> &p_requests) {
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_null()) {
			continue;
		}
		std::unordered_map<DSRequest *, ID3D12Resource *>::iterator it = resource_guards.find(p_requests[i].ptr());
		if (it != resource_guards.end()) {
			if (it->second) {
				it->second->Release();
			}
			resource_guards.erase(it);
		}
	}
}

void Win32Backend::release_all_resource_guards() {
	for (std::unordered_map<DSRequest *, ID3D12Resource *>::iterator it = resource_guards.begin(); it != resource_guards.end(); ++it) {
		if (it->second) {
			it->second->Release();
		}
	}
	resource_guards.clear();
}

// Render-thread-affine guard re-query used by validate_resource_guard().
// Returns true and fills r_resource when the query runs against a live
// RenderingDevice inline on the render thread. When called off the render
// thread (e.g. completion thread under batch_mutex), immediately returns false
// without blocking, allowing validate_resource_guard() to safely fall back to
// the AddRef'd guard resource without stalling frames or risking deadlocks.
static bool ds_win32_query_guard_resource_on_render_thread(bool p_is_buffer_request, const RID &p_destination_rid, ID3D12Resource *&r_resource) {
	r_resource = nullptr;
	RenderingServer *rs = safe_get_rendering_server();
	if (!rs) {
		return false;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (!rd) {
			return false;
		}
		auto resource_type = p_is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		r_resource = reinterpret_cast<ID3D12Resource *>(rd->get_driver_resource(resource_type, p_destination_rid, 0));
		return true;
	}
	return false;
}


// Render-thread destination query context for enqueue_request().
// Marshals RenderingDevice::get_driver_resource() and ID3D12Resource::GetDesc()
// to the render thread when called from main/worker threads.
// Managed via std::shared_ptr so that if the calling thread times out after 250ms,
// any late-arriving render-thread callback safely references a live heap object
// rather than a destroyed stack frame (HIGH-01).
struct DSGPUDestinationContext {
	std::mutex mutex;
	std::condition_variable cv;
	bool completed = false;
	bool is_buffer_request = false;
	RID destination_rid;
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	uint64_t native_format = 0;
	bool rd_available = false;
};
// One parked context per in-flight query, never a single slot (R1). With one
// slot, two threads resolving different destination RIDs before the render thread
// runs overwrite each other: the callback services the newer context, and the
// older caller spends its whole 250 ms budget waiting for a completion that can
// never arrive, so its perfectly valid request is rejected. A queue parks every
// context, and each posted callback pops exactly one.
// Never destroyed: freeing handoff state from a static destructor would run
// during DLL_PROCESS_DETACH and touch D3D12 under the loader lock.
static DSRenderHandoffQueue<DSGPUDestinationContext> &s_gpu_dest_query_queue = *new DSRenderHandoffQueue<DSGPUDestinationContext>();

static void ds_win32_fetch_destination_resource_on_render_thread() {
	std::shared_ptr<DSGPUDestinationContext> ctx = s_gpu_dest_query_queue.take_front();
	if (!ctx) {
		return;
	}
	RenderingServer *rs = safe_get_rendering_server();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	uint64_t native_format = 0;
	bool rd_available = false;
	if (rd) {
		rd_available = true;
		auto resource_type = ctx->is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		uint64_t handle = rd->get_driver_resource(resource_type, ctx->destination_rid, 0);
		resource = reinterpret_cast<ID3D12Resource *>(handle);
		if (resource) {
			desc = resource->GetDesc();
			if (!ctx->is_buffer_request) {
				native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, ctx->destination_rid, 0);
			}
		}
	}
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		ctx->resource = resource;
		ctx->desc = desc;
		ctx->native_format = native_format;
		ctx->rd_available = rd_available;
		ctx->completed = true;
		ctx->cv.notify_one(); // HIGH-01: Notify while holding mutex
	}
}

static bool ds_win32_query_destination_resource_on_render_thread(
		bool p_is_buffer_request,
		const RID &p_destination_rid,
		ID3D12Resource *&r_resource,
		D3D12_RESOURCE_DESC &r_desc,
		uint64_t &r_native_format,
		bool &r_rd_available) {
	r_resource = nullptr;
	r_desc = {};
	r_native_format = 0;
	r_rd_available = false;

	RenderingServer *rs = safe_get_rendering_server();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return false;
	}
	if (rs->is_on_render_thread()) {
		RenderingDevice *rd = rs->get_rendering_device();
		if (!rd) {
			return false;
		}
		r_rd_available = true;
		auto resource_type = p_is_buffer_request
				? RenderingDevice::DRIVER_RESOURCE_BUFFER
				: RenderingDevice::DRIVER_RESOURCE_TEXTURE;
		uint64_t handle = rd->get_driver_resource(resource_type, p_destination_rid, 0);
		r_resource = reinterpret_cast<ID3D12Resource *>(handle);
		if (r_resource) {
			r_desc = r_resource->GetDesc();
			if (!p_is_buffer_request) {
				r_native_format = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_TEXTURE_DATA_FORMAT, p_destination_rid, 0);
			}
		}
		return true;
	}

	auto ctx = std::make_shared<DSGPUDestinationContext>();
	ctx->is_buffer_request = p_is_buffer_request;
	ctx->destination_rid = p_destination_rid;

	s_gpu_dest_query_queue.push(ctx);
	rs->call_on_render_thread(callable_mp_static(ds_win32_fetch_destination_resource_on_render_thread));

	{
		std::unique_lock<std::mutex> lock(ctx->mutex);
		constexpr std::chrono::milliseconds kDestQueryTimeout(250);
		bool signaled = ctx->cv.wait_for(lock, kDestQueryTimeout, [&ctx] { return ctx->completed; });
		if (!signaled) {
			// HIGH-02: Bounded timeout with no secondary unbounded wait. Drop this
			// query from the handoff so a late callback cannot service a caller that
			// already returned.
			s_gpu_dest_query_queue.prune(ctx);
			return false;
		}
		r_rd_available = ctx->rd_available;
		r_resource = ctx->resource;
		r_desc = ctx->desc;
		r_native_format = ctx->native_format;
		return true;
	}
}

bool Win32Backend::validate_resource_guard(const Ref<DSRequest> &p_request, String &r_error, std::unordered_map<int64_t, ID3D12Resource *> *p_rid_cache) {
	if (p_request.is_null() || (p_request->get_destination_type() != DSRequest::DESTINATION_GPU_BUFFER &&
			p_request->get_destination_type() != DSRequest::DESTINATION_GPU_TEXTURE)) {
		return true;
	}

	std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard = resource_guards.find(p_request.ptr());
	RID destination_rid = p_request->get_destination_resource();
	if (guard == resource_guards.end() || !guard->second || !destination_rid.is_valid() || !p_request->is_gpu_in_flight()) {
		r_error = "DirectStorage GPU resource RID was freed or cancelled before batch completion.";
		return false;
	}

	RenderingServer *rs = safe_get_rendering_server();
	RenderingDevice *rd = rs ? rs->get_rendering_device() : nullptr;
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12" || !rd) {
		r_error = "DirectStorage GPU resource could not be validated after batch completion.";
		return false;
	}

	// Split validation contract (MED-12):
	// - Off-render-thread (completion thread): Calling RenderingDevice queries (texture_is_valid,
	//   get_driver_resource) violates thread affinity and asserts (Q02). Validation is conservative:
	//   the COM ID3D12Resource AddRef retention invariant guarantees the GPU allocation remains
	//   physically valid for DMA write, while is_gpu_in_flight() above confirms the request was not cancelled.
	// - On-render-thread (sync flush / main poll): Queries the live driver resource to confirm
	//   the RID still points to the exact same ID3D12Resource pointer.
	bool is_buffer_request = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	ID3D12Resource *current_resource = nullptr;
	bool has_cached = false;
	int64_t rid_id = destination_rid.get_id();
	if (p_rid_cache) {
		std::unordered_map<int64_t, ID3D12Resource *>::const_iterator cache_it = p_rid_cache->find(rid_id);
		if (cache_it != p_rid_cache->end()) {
			current_resource = cache_it->second;
			has_cached = true;
		}
	}
	if (!has_cached) {
		if (!ds_win32_query_guard_resource_on_render_thread(is_buffer_request, destination_rid, current_resource)) {
			if (p_rid_cache) {
				(*p_rid_cache)[rid_id] = guard->second;
			}
			return true;
		}
		if (p_rid_cache) {
			(*p_rid_cache)[rid_id] = current_resource;
		}
	}
	if (!current_resource || current_resource != guard->second) {
		r_error = "DirectStorage GPU resource RID was freed or replaced before batch completion.";
		return false;
	}
	return true;
}

DSRequest *Win32Backend::get_logical_request(DSRequest *p_request) const {
	std::unordered_map<DSRequest *, DSRequest *>::const_iterator it = chunk_parents.find(p_request);
	return it != chunk_parents.end() ? it->second : p_request;
}

int Win32Backend::count_logical_requests(const Vector<Ref<DSRequest>> &p_requests) const {
	std::unordered_set<DSRequest *> logical_requests;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid()) {
			logical_requests.insert(get_logical_request(p_requests[i].ptr()));
		}
	}
	return static_cast<int>(logical_requests.size());
}

static int count_completed_requests(const Vector<Ref<DSRequest>> &p_requests) {
	int completed_count = 0;
	for (int i = 0; i < p_requests.size(); i++) {
		if (p_requests[i].is_valid() && p_requests[i]->get_status() == DSRequest::STATUS_COMPLETED) {
			completed_count++;
		}
	}
	return completed_count;
}

void Win32Backend::complete_chunked_requests() {
	for (std::unordered_map<DSRequest *, DSChunkedRequestState>::iterator it = chunked_requests.begin(); it != chunked_requests.end();) {
		DSChunkedRequestState &state = it->second;
		bool all_complete = true;
		bool failed = false;
		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_null()) {
				failed = true;
				continue;
			}
			DSRequest::Status status = state.children[i]->get_status();
			if (status == DSRequest::STATUS_FAILED) {
				failed = true;
			} else if (status != DSRequest::STATUS_COMPLETED) {
				all_complete = false;
			}
		}
		if (!all_complete) {
			++it;
			continue;
		}

		if (failed) {
			state.parent->set_status(DSRequest::STATUS_FAILED);
			for (int i = 0; i < state.children.size(); i++) {
				if (state.children[i].is_valid() && !state.children[i]->get_error_message().is_empty()) {
					state.parent->set_error_message(state.children[i]->get_error_message());
					break;
				}
			}
			if (state.parent->get_error_message().is_empty()) {
				state.parent->set_error_message("DirectStorage GDeflate chunk failed.");
			}
		} else {
			if (state.parent->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE ||
					state.parent->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER) {
				state.parent->set_status(DSRequest::STATUS_COMPLETED);
			} else {
				const uint64_t required_size = state.parent->get_uncompressed_size() > 0
						? state.parent->get_uncompressed_size()
						: state.parent->get_size();
				state.parent->ensure_destination_buffer_capacity(required_size);
				PackedByteArray &destination = state.parent->get_destination_buffer_ref();
				bool valid_destination = true;
				for (int i = 0; i < state.children.size(); i++) {
					PackedByteArray child_data = state.children[i]->get_destination_buffer();
					uint64_t destination_offset = state.destination_offsets[i];
					if (destination_offset + static_cast<uint64_t>(child_data.size()) > static_cast<uint64_t>(destination.size())) {
						valid_destination = false;
						break;
					}
					if (child_data.size() > 0) {
						std::memcpy(destination.ptrw() + destination_offset, child_data.ptr(), child_data.size());
					}
				}
				if (valid_destination) {
					state.parent->set_status(DSRequest::STATUS_COMPLETED);
				} else {
					state.parent->set_status(DSRequest::STATUS_FAILED);
					state.parent->set_error_message("DirectStorage GDeflate chunk output exceeded the parent destination buffer.");
				}
			}
		}

		// The parent status above is already final, but the batch may still be
		// writing: cancel_requests_with_tag() marks children FAILED while their
		// gpu_in_flight flag stays set until the fence retires (it keeps them in the
		// slot for exactly that). Erasing the entry now would leave nothing to clear
		// the parent flag, and DSPreloader::is_terminal() would then free the
		// destination RID under live DMA. Wait for retirement instead.
		bool any_child_gpu_in_flight = false;
		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_valid() && state.children[i]->is_gpu_in_flight()) {
				any_child_gpu_in_flight = true;
				break;
			}
		}
		if (any_child_gpu_in_flight) {
			++it;
			continue;
		}

		for (int i = 0; i < state.children.size(); i++) {
			if (state.children[i].is_valid()) {
				chunk_parents.erase(state.children[i].ptr());
			}
		}
		if (state.parent.is_valid()) {
			state.parent->set_gpu_in_flight(false);
		}
		it = chunked_requests.erase(it);
	}
}

// Fence queries are PlatformBackend virtuals now: the caller dispatches through
// the object it holds, so there is no process-global "live instance" to consult.
#endif

#ifdef _WIN32
bool Win32Backend::has_in_flight_batches_locked() const {
	if (!completion_queue.empty()) {
		return true;
	}
	for (const QueueState &queue : priority_queues) {
		for (const BatchSlot &slot : queue.batch_slots) {
			if (slot.submitted) {
				return true;
			}
		}
	}
	return false;
}
#endif

#ifdef _WIN32
// Caller holds batch_mutex.
bool Win32Backend::has_unfinished_batch_on_queue_locked(const QueueState &p_queue) const {
	for (const BatchSlot &slot : p_queue.batch_slots) {
		if (slot.completion_ready) {
			continue;
		}
		// closing covers the window between slot freeze and Submit(): its requests
		// are about to be handed to this queue, so a failure belongs to them.
		if (slot.submitted || slot.closing) {
			return true;
		}
	}
	return false;
}

// Caller holds batch_mutex.
bool Win32Backend::slot_owns_error_name_locked(const BatchSlot &p_slot, const String &p_failed_request_name) const {
	if (p_failed_request_name.is_empty()) {
		return true;
	}
	const std::string key = p_failed_request_name.utf8().get_data();
	return p_slot.request_name_map.find(key) != p_slot.request_name_map.end();
}

// Caller holds batch_mutex.
int Win32Backend::slot_index_for_error_name_locked(const QueueState &p_queue, const String &p_failed_request_name) const {
	if (p_failed_request_name.is_empty()) {
		return -1;
	}
	const std::string key = p_failed_request_name.utf8().get_data();
	for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
		if (p_queue.batch_slots[slot_index].request_name_map.find(key) != p_queue.batch_slots[slot_index].request_name_map.end()) {
			return slot_index;
		}
	}
	return -1;
}

// Caller holds batch_mutex.
bool Win32Backend::queue_has_request_name_locked(const QueueState &p_queue, const String &p_failed_request_name) const {
	return slot_index_for_error_name_locked(p_queue, p_failed_request_name) >= 0;
}

// Caller holds batch_mutex.
void Win32Backend::stash_queue_error_locked(QueueState &p_queue, const QueueErrorInfo &p_error, QueueErrorInfo *r_immediate) {
	if (!p_error.present) {
		return;
	}
	if (has_unfinished_batch_on_queue_locked(p_queue)) {
		// Every staged request has a unique name, so a record belongs to exactly
		// one slot: keep it on that slot's own entry. A record that names no
		// request - or one no slot holds any more - cannot be attributed, so it
		// takes the first free entry and the completion path surfaces it when no
		// slot claims it.
		const int owner_slot = slot_index_for_error_name_locked(p_queue, p_error.failed_request_name);
		if (owner_slot >= 0 && !p_queue.pending_errors[owner_slot].present) {
			p_queue.pending_errors[owner_slot] = p_error;
			return;
		}
		for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
			if (!p_queue.pending_errors[slot_index].present) {
				p_queue.pending_errors[slot_index] = p_error;
				return;
			}
		}
	}
	// Either no batch can carry this failure, or every slot entry is already
	// claimed: report it here rather than dropping the record.
	if (r_immediate && !r_immediate->present) {
		*r_immediate = p_error;
	}
}

void Win32Backend::handle_queue_error_event(int p_queue_index) {
	QueueErrorInfo immediate;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		QueueState &queue = priority_queues[p_queue_index];
		// Bounded re-drain. The SDK signals the event again for failures that land
		// while this thread is reading, so a zero-timeout re-check closes the
		// window between the read and the reset. ResetEvent() is a no-op on an
		// auto-reset event and required for a manual-reset one, so it is correct
		// for either.
		for (int attempt = 0; attempt < 4; attempt++) {
			QueueErrorInfo record = read_queue_error(queue.queue);
			if (!record.present) {
				break;
			}
			// Stash the record un-attributed. The batch that claims it attributes
			// it against its own slot exactly once; a record no batch can carry is
			// attributed here, because this report is its only consumer.
			QueueErrorInfo immediate_record;
			stash_queue_error_locked(queue, record, &immediate_record);
			if (immediate_record.present) {
				enrich_queue_error_from_queue_locked(queue, immediate_record);
				if (!immediate.present) {
					immediate = immediate_record;
				}
			}
			if (!queue.error_event) {
				break;
			}
			ResetEvent(queue.error_event);
			if (WaitForSingleObject(queue.error_event, 0) != WAIT_OBJECT_0) {
				break;
			}
		}
	}
	report_queue_error(immediate);
}
#endif

bool Win32Backend::has_free_batch_slot() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	if (!accepting_requests.load() || sync_batch_in_flight.load()) {
		return false;
	}
	DSContext *ctx = DSContext::get_singleton();
	const uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
	const bool pipelining = can_pipeline(queue_capacity);
	if (!pipelining) {
		// Single-slot serialization: every slot on every queue must be idle, and
		// at least one slot index must have no unharvested results anywhere.
		for (int queue_index = 0; queue_index < QUEUE_COUNT; ++queue_index) {
			for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; ++slot_index) {
				const BatchSlot &slot = priority_queues[queue_index].batch_slots[slot_index];
				if (slot.submitted || slot.closing) {
					return false;
				}
			}
		}
		for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; ++slot_index) {
			bool any_completion_ready = false;
			for (int queue_index = 0; queue_index < QUEUE_COUNT; ++queue_index) {
				any_completion_ready = any_completion_ready || priority_queues[queue_index].batch_slots[slot_index].completion_ready;
			}
			if (!any_completion_ready) {
				return true;
			}
		}
		return false;
	}
	// submit_async() claims a slot index across ALL queues, so the caller gate
	// must use the same quantifier: exists slot index that is free everywhere.
	for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; ++slot_index) {
		bool slot_free_on_every_queue = true;
		bool any_completion_ready = false;
		for (int queue_index = 0; queue_index < QUEUE_COUNT; ++queue_index) {
			const BatchSlot &slot = priority_queues[queue_index].batch_slots[slot_index];
			if (slot.submitted || slot.closing) {
				slot_free_on_every_queue = false;
			}
			if (slot.completion_ready) {
				any_completion_ready = true;
			}
		}
		if (slot_free_on_every_queue && !any_completion_ready) {
			return true;
		}
	}
	return false;
#else
	return true;
#endif
}

uint32_t Win32Backend::get_in_flight_batch_count() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	uint32_t count = 0;
	for (const QueueState &queue : priority_queues) {
		for (const BatchSlot &slot : queue.batch_slots) {
			if (slot.submitted || slot.closing) {
				++count;
			}
		}
	}
	return count;
#else
	return 0;
#endif
}

bool Win32Backend::is_batch_complete() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	return !has_in_flight_batches_locked();
#else
	return true;
#endif
}

uint64_t Win32Backend::get_completion_fence() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	uint64_t latest_fence = 0;
	for (const QueueState &queue : priority_queues) {
		latest_fence = MAX(latest_fence, queue.completion_fence_value);
	}
	return latest_fence;
#else
	return 0;
#endif
}

bool Win32Backend::wait_for_completion(uint64_t p_fence_value, uint32_t p_timeout_msec) {
#ifdef _WIN32
	if (p_fence_value == 0) {
		return true;
	}
	if (!factory) {
		return false;
	}
	uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
	std::vector<std::pair<ID3D12Fence *, uint64_t>> fences_to_wait;
	uint64_t max_completion_fence = 0;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int q = 0; q < QUEUE_COUNT; q++) {
			const QueueState &queue = priority_queues[q];
			max_completion_fence = MAX(max_completion_fence, queue.completion_fence_value);
			// The three priority queues own independent fences and each counter
			// restarts at 0 on its own queue, so one queue's value is NOT comparable
			// to another's. Filtering on `completion_fence_value >= p_fence_value`
			// dropped every queue whose counter happened to be lower and returned
			// "complete" while its DMA was still running.
			// Wait for each queue's own latest value, clamped to the caller's
			// watermark: a queue below the watermark is still drained.
			if (!queue.completion_fence || queue.completion_fence_value == 0) {
				continue;
			}
			uint64_t target_value = MIN(p_fence_value, queue.completion_fence_value);
			if (queue.completed_fence_value >= target_value) {
				continue;
			}
			fences_to_wait.push_back({ queue.completion_fence, target_value });
		}
	}
	if (p_fence_value > max_completion_fence) {
		if (p_timeout_msec > 0) {
			ID3D12Fence *fence = priority_queues[0].completion_fence;
			if (fence) {
				HANDLE wait_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
				if (wait_event) {
					HRESULT hr = fence->SetEventOnCompletion(p_fence_value, wait_event);
					if (SUCCEEDED(hr)) {
						WaitForSingleObject(wait_event, p_timeout_msec);
					}
					CloseHandle(wait_event);
				}
			} else {
				Sleep(p_timeout_msec);
			}
		}
		return false;
	}
	if (fences_to_wait.empty()) {
		return true;
	}

	for (size_t i = 0; i < fences_to_wait.size(); i++) {
		ID3D12Fence *fence = fences_to_wait[i].first;
		uint64_t target_value = fences_to_wait[i].second;
		uint64_t completed_val = fence->GetCompletedValue();
		if (completed_val == UINT64_MAX) {
			return false;
		}
		if (completed_val < target_value) {
			uint64_t now = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
			uint32_t elapsed = (now >= start_time) ? static_cast<uint32_t>(now - start_time) : 0;
			if (elapsed >= p_timeout_msec) {
				return false;
			}
			uint32_t remaining_ms = p_timeout_msec - elapsed;
			HANDLE wait_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			if (!wait_event) {
				return false;
			}
			HRESULT hr = fence->SetEventOnCompletion(target_value, wait_event);
			DWORD wait_result = SUCCEEDED(hr) ? WaitForSingleObject(wait_event, remaining_ms) : WAIT_FAILED;
			CloseHandle(wait_event);
			if (FAILED(hr) || wait_result != WAIT_OBJECT_0) {
				return false;
			}
			completed_val = fence->GetCompletedValue();
		}
		if (completed_val < target_value || completed_val == UINT64_MAX) {
			return false;
		}
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			for (QueueState &queue : priority_queues) {
				if (queue.completion_fence == fence) {
					queue.completed_fence_value = std::max(queue.completed_fence_value, completed_val);
					break;
				}
			}
		}
	}
	return true;
#else
	return p_fence_value == 0;
#endif
}

// Wait for how far the batch on one priority lane has got. The target is that
// lane's own counter, snapshotted once under batch_mutex: a batch that spans
// lanes writes a value on each queue, so a numeric watermark is not comparable
// across queues and the caller has to name the lane it cares about.
bool Win32Backend::wait_for_priority_completion(int p_priority, uint32_t p_timeout_msec) {
#ifdef _WIN32
	if (!factory) {
		return false;
	}
	const int queue_index = priority_to_queue_index(static_cast<DSRequest::DSPriority>(p_priority));
	ID3D12Fence *fence = nullptr;
	uint64_t target_value = 0;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		const QueueState &queue = priority_queues[queue_index];
		if (!queue.completion_fence || queue.completion_fence_value == 0) {
			// Nothing was ever issued on this lane: there is nothing to wait for.
			return true;
		}
		target_value = queue.completion_fence_value;
		if (queue.completed_fence_value >= target_value) {
			return true;
		}
		fence = queue.completion_fence;
	}

	const uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
	for (;;) {
		const uint64_t completed_val = fence->GetCompletedValue();
		if (completed_val == UINT64_MAX) {
			// Device removed: the fence can never reach the target.
			return false;
		}
		if (completed_val >= target_value) {
			break;
		}
		const uint64_t now = Time::get_singleton() ? Time::get_singleton()->get_ticks_msec() : 0;
		const uint64_t elapsed = now >= start_time ? now - start_time : 0;
		if (elapsed >= p_timeout_msec) {
			return false;
		}
		const uint32_t remaining_ms = static_cast<uint32_t>(p_timeout_msec - elapsed);
		HANDLE wait_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (!wait_event) {
			return false;
		}
		const HRESULT hr = fence->SetEventOnCompletion(target_value, wait_event);
		const DWORD wait_result = SUCCEEDED(hr) ? WaitForSingleObject(wait_event, remaining_ms) : WAIT_FAILED;
		CloseHandle(wait_event);
		if (FAILED(hr) || wait_result != WAIT_OBJECT_0) {
			return false;
		}
	}
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		QueueState &queue = priority_queues[queue_index];
		queue.completed_fence_value = std::max(queue.completed_fence_value, target_value);
	}
	return true;
#else
	(void)p_priority;
	(void)p_timeout_msec;
	return false;
#endif
}

void Win32Backend::poll_batch_status() {
#ifdef _WIN32
	std::deque<BatchMetadata> completed_batches;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (std::vector<ZombieBatchSlot>::iterator it = zombie_slots.begin(); it != zombie_slots.end(); ) {
			ID3D12Fence *fence = priority_queues[it->queue_index].completion_fence;
			if (!fence || fence->GetCompletedValue() >= it->fence_value) {
				for (int r = 0; r < it->requests.size(); r++) {
					if (it->requests[r].is_valid()) {
						it->requests[r]->set_gpu_in_flight(false);
					}
				}
				complete_chunked_requests();
				release_resource_guards(it->requests);
				// Drop the zombie's file references through the cache: the handle
				// is shared with every slot/queue that staged the same path, so it
				// must only close when the last reference goes (same rule as
				// drain_all_queues()). Closing here behind the cache's back left
				// the entry pointing at a closed handle with refs still pinned.
				for (std::unordered_map<std::wstring, IDStorageFile *>::iterator f = it->file_cache.begin(); f != it->file_cache.end(); ++f) {
					release_source_file(f->first, f->second);
				}
				it->file_cache.clear();
				it->files.clear();
				it = zombie_slots.erase(it);
			} else {
				++it;
			}
		}
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
				BatchSlot &slot = queue.batch_slots[slot_index];
				if (!slot.submitted || slot.completion_ready || !queue.completion_fence || slot.fence_value == 0 ||
						queue.completion_fence->GetCompletedValue() < slot.fence_value) {
					continue;
				}
				for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ++it) {
					if (it->queue_index == queue_index && it->slot_index == slot_index && it->fence_value == slot.fence_value) {
						completed_batches.push_back(*it);
						completion_queue.erase(it);
						// Match completion_thread_func(): consume the slot event so a
						// stale signal cannot wake the completion thread needlessly.
						if (slot.event) {
							ResetEvent(slot.event);
						}
						break;
					}
				}
			}
		}
	}

	if (!completed_batches.empty()) {
		std::vector<BatchReport> reports;
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			for (const BatchMetadata &batch : completed_batches) {
				BatchReport report;
				complete_batch_slot(priority_queues[batch.queue_index], priority_queues[batch.queue_index].batch_slots[batch.slot_index], batch, report);
				if (report.error.present || !report.warnings.empty()) {
					reports.push_back(report);
				}
			}
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
		}
		// Outside batch_mutex: these calls reach the engine.
		for (const BatchReport &report : reports) {
			report_batch_outcome(report);
		}
	}
#endif
}

#ifdef _WIN32
void Win32Backend::attribute_batch_error_records(Vector<Ref<DSRequest>> &p_requests, const std::unordered_map<std::string, Ref<DSRequest>> &p_name_map, const String &p_failed_request_name, const String &p_queue_error) {
	Ref<DSRequest> failed_root_req;
	if (!p_failed_request_name.is_empty()) {
		std::string key = p_failed_request_name.utf8().get_data();
		auto it = p_name_map.find(key);
		if (it != p_name_map.end()) {
			failed_root_req = it->second;
		}
	}

	for (int i = 0; i < p_requests.size(); i++) {
		Ref<DSRequest> req = p_requests[i];
		if (req.is_valid()) {
			// If request already failed due to resource guard validation (or prior specific failure),
			// preserve its specific guard error message unless it was the root hardware failure.
			if (req->get_status() == DSRequest::STATUS_FAILED && !req->get_error_message().is_empty() && req != failed_root_req) {
				continue;
			}
			req->set_status(DSRequest::STATUS_FAILED);
			if (failed_root_req.is_valid() && req == failed_root_req) {
				req->set_error_message(p_queue_error);
			} else if (!p_failed_request_name.is_empty()) {
				req->set_error_message("DirectStorage batch aborted due to failure in request [" + p_failed_request_name + "]: " + p_queue_error);
			} else {
				req->set_error_message(p_queue_error);
			}
		}
	}
}

Ref<DSRequest> Win32Backend::get_chunked_child_request(DSRequest *p_parent, int p_index) {
#ifdef _WIN32
	if (!p_parent) {
		return Ref<DSRequest>();
	}
	auto it = chunked_requests.find(p_parent);
	if (it != chunked_requests.end()) {
		if (p_index >= 0 && p_index < it->second.children.size()) {
			return it->second.children[p_index];
		}
	}
	return Ref<DSRequest>();
#else
	return Ref<DSRequest>();
#endif
}

void Win32Backend::rollback_staged_requests(Vector<Ref<DSRequest>> &p_slot_requests,
		std::vector<std::string> &p_slot_request_names,
		std::unordered_map<std::string, Ref<DSRequest>> &p_slot_request_name_map,
		const std::vector<Ref<DSRequest>> &p_staged_children,
		size_t p_failed_index,
		Ref<DSRequest> p_parent
#ifdef _WIN32
		, std::vector<DSTORAGE_REQUEST> *p_dstorage_requests
#endif
) {
#ifdef _WIN32
	if (p_parent.is_valid()) {
		p_parent->set_status(DSRequest::STATUS_FAILED);
		if (p_failed_index < p_staged_children.size() && p_staged_children[p_failed_index].is_valid() &&
				!p_staged_children[p_failed_index]->get_error_message().is_empty()) {
			p_parent->set_error_message(p_staged_children[p_failed_index]->get_error_message());
		} else {
			p_parent->set_error_message("DirectStorage batch staging failed.");
		}
	}

	for (size_t r = p_failed_index; r > 0; --r) {
		size_t idx = r - 1;
		if (idx < p_staged_children.size() && p_staged_children[idx].is_valid()) {
			Ref<DSRequest> child = p_staged_children[idx];
			child->set_status(DSRequest::STATUS_FAILED);
			child->set_gpu_in_flight(false);
			chunk_parents.erase(child.ptr());
			if (!p_slot_requests.is_empty() && p_slot_requests[p_slot_requests.size() - 1] == child) {
				p_slot_requests.remove_at(p_slot_requests.size() - 1);
			}
			if (!p_slot_request_names.empty()) {
				std::string req_name = p_slot_request_names.back();
				p_slot_request_names.pop_back();
				p_slot_request_name_map.erase(req_name);
			}
			if (p_dstorage_requests && !p_dstorage_requests->empty()) {
				p_dstorage_requests->pop_back();
			}
			std::unordered_map<DSRequest *, ID3D12Resource *>::iterator g_it = resource_guards.find(child.ptr());
			if (g_it != resource_guards.end()) {
				if (g_it->second) {
					g_it->second->Release();
				}
				resource_guards.erase(g_it);
			}
		}
	}
#endif
}

bool Win32Backend::build_single_mip_child_request(Ref<DSRequest> p_parent, const DSTexMipEntryV2 &p_mip_entry, Ref<DSRequest> &r_child) {
#ifdef _WIN32
	if (p_parent.is_null()) {
		return false;
	}
	r_child.instantiate();
	r_child->set_file_path(p_parent->get_file_path());
	r_child->set_offset(p_mip_entry.offset);
	r_child->set_size(p_mip_entry.compressed_size);
	r_child->set_uncompressed_size(p_mip_entry.uncompressed_size);
	r_child->set_compression_format(p_parent->get_compression_format());
	r_child->set_destination_type(p_parent->get_destination_type());
	r_child->set_priority(p_parent->get_priority());
	r_child->set_cancellation_tag(p_parent->get_cancellation_tag());
	r_child->set_destination_resource(p_parent->get_destination_resource());
	r_child->set_first_subresource(p_parent->get_first_subresource());
	r_child->set_subresource_count(1);
	r_child->set_texture_format(p_parent->get_texture_format());
	r_child->set_texture_layout(p_parent->get_texture_layout());
	if (!p_parent->get_debug_label().is_empty()) {
		r_child->set_debug_label(p_parent->get_debug_label());
	}
	return true;
#else
	return false;
#endif
}

bool Win32Backend::stage_single_mip_request(Ref<DSRequest> p_parent, Ref<DSRequest> &r_child) {
#ifdef _WIN32
	if (p_parent.is_null()) {
		return false;
	}
	Vector<DSTexMipEntryV2> texture_mip_entries;
	if (!get_texture_mip_entries(p_parent->get_file_path(), p_parent->get_offset(), texture_mip_entries)) {
		return false;
	}
	uint32_t first_subresource = p_parent->get_first_subresource();
	if (first_subresource >= static_cast<uint32_t>(texture_mip_entries.size())) {
		return false;
	}
	const DSTexMipEntryV2 &mip_entry = texture_mip_entries[first_subresource];
	return build_single_mip_child_request(p_parent, mip_entry, r_child);
#else
	return false;
#endif
}

bool Win32Backend::poll_batch_status(QueueState &p_queue, BatchSlot &p_slot, int p_slot_index, uint64_t p_fence_value, BatchReport &r_report) {
	if (!p_queue.completion_fence || p_fence_value == 0 ||
			p_queue.completion_fence->GetCompletedValue() < p_fence_value) {
		return false;
	}

	p_queue.completed_fence_value = std::max(p_queue.completed_fence_value, p_queue.completion_fence->GetCompletedValue());
	// Consume every record this batch can claim. Only a record the SDK actually
	// signalled can be stashed here, so a clean batch leaves this empty and still
	// completes. The record names the request that failed and every staged request
	// has a unique name, so it belongs to exactly one slot: a batch that does not
	// own that request must never be failed by it, and must not swallow it either.
	// This slot's own entry is examined first, so the record routed to it decides
	// the failure and the message root.
	QueueErrorInfo queue_error;
	bool claimed_error = false;
	for (int offset = 0; offset < BATCH_SLOT_COUNT; offset++) {
		const int entry = (p_slot_index + offset) % BATCH_SLOT_COUNT;
		if (!p_queue.pending_errors[entry].present) {
			continue;
		}
		const bool owns = slot_owns_error_name_locked(p_slot, p_queue.pending_errors[entry].failed_request_name);
		if (!owns && queue_has_request_name_locked(p_queue, p_queue.pending_errors[entry].failed_request_name)) {
			// Another slot on this queue still holds the named request. Leave the
			// record stashed for that slot's completion; this batch is clean.
			continue;
		}
		// Either this batch staged the named request, or the owning request already
		// retired (so no later batch will claim it: surface it now instead of
		// stashing it forever). A second record for a batch that is already failing
		// is folded into the failure, because a batch fails as a whole
		// (attribute_batch_error_records) - except that a fatal record always wins
		// over a cancellation, so a cancelled request can never mask a hardware
		// failure on the same queue.
		if (!claimed_error || (p_queue.pending_errors[entry].fatal && !queue_error.fatal)) {
			queue_error = p_queue.pending_errors[entry];
			claimed_error = true;
		}
		p_queue.pending_errors[entry] = QueueErrorInfo();
	}
	// The SDK can raise the error event just after the batch's own completion
	// signal. Read it here only when it is already signalled - still the
	// contract's trigger - so a failing batch is not marked completed because the
	// two events raced, and only when no stashed record already made this batch
	// fail. A clean batch never reaches RetrieveErrorRecord.
	if (!claimed_error && p_queue.error_event && WaitForSingleObject(p_queue.error_event, 0) == WAIT_OBJECT_0) {
		QueueErrorInfo raced = read_queue_error(p_queue.queue);
		ResetEvent(p_queue.error_event);
		if (raced.present) {
			if (!slot_owns_error_name_locked(p_slot, raced.failed_request_name) &&
					queue_has_request_name_locked(p_queue, raced.failed_request_name)) {
				// The record belongs to another slot that is still in flight: keep it
				// for that slot's completion instead of failing this clean batch.
				QueueErrorInfo immediate;
				stash_queue_error_locked(p_queue, raced, &immediate);
				if (immediate.present) {
					// Every entry is claimed and this is the only batch that can retire
					// now: the queue is failing, so never drop the record.
					queue_error = immediate;
					claimed_error = true;
				}
			} else {
				queue_error = raced;
				claimed_error = true;
			}
		}
	}
	const bool queue_failed = claimed_error && queue_error.fatal;

	std::unordered_map<int64_t, ID3D12Resource *> rid_cache;
	for (int i = 0; i < p_slot.requests.size(); i++) {
		Ref<DSRequest> req = p_slot.requests[i];
		if (req.is_valid()) {
			String resource_error;
			if (!validate_resource_guard(req, resource_error, &rid_cache)) {
				// Collected, not pushed: push_warning is an engine call and the
				// caller holds batch_mutex here.
				r_report.warnings.push_back(resource_error);
				req->set_status(DSRequest::STATUS_FAILED);
				req->set_error_message(resource_error);
			} else if (!queue_failed && req->get_status() != DSRequest::STATUS_FAILED) {
				req->set_status(DSRequest::STATUS_COMPLETED);
			}
		}
	}
	if (queue_error.present) {
		// This is the single attribution point for a record a batch claimed; the
		// queue-error event thread only attributes records no batch can carry.
		enrich_queue_error_from_slot(p_slot, queue_error);
		r_report.error = queue_error;
	}
	if (queue_failed) {
		attribute_batch_error_records(p_slot.requests, p_slot.request_name_map, queue_error.failed_request_name, queue_error.format_message());
	}
	for (int i = 0; i < p_slot.requests.size(); i++) {
		if (p_slot.requests[i].is_valid()) {
			p_slot.requests[i]->set_gpu_in_flight(false);
		}
	}
	complete_chunked_requests();
	return true;
}
#endif

#ifdef _WIN32
void Win32Backend::update_profiler_queue_depth_locked() {
	int pending_request_count = 0;
	for (const QueueState &queue : priority_queues) {
		for (const BatchSlot &slot : queue.batch_slots) {
			if (!slot.completion_ready) {
				pending_request_count += count_logical_requests(slot.requests);
			}
		}
	}
	DSProfiler *profiler = DSProfiler::get_singleton();
	if (profiler) {
		profiler->set_queue_depth(pending_request_count);
	}
}

void Win32Backend::complete_batch_slot(QueueState &p_queue, BatchSlot &p_slot, const BatchMetadata &p_batch, BatchReport &r_report) {
	if (!poll_batch_status(p_queue, p_slot, p_batch.slot_index, p_slot.fence_value, r_report)) {
		// The batch's fence has not retired (e.g. a stale slot event matched it).
		// Retiring here would release resource guards and close IDStorageFile
		// handles while DMA is still active, and would strand requests at
		// STATUS_SUBMITTED. Leave everything in place for the fence poll.
		r_report.warnings.push_back("Win32Backend: refused to retire a batch whose completion fence has not retired.");
		return;
	}
	p_slot.completed_count = count_completed_requests(p_slot.requests);
	p_slot.error_code = 0;
	p_slot.error_message = String();
	for (int i = 0; i < p_slot.requests.size(); i++) {
		if (p_slot.requests[i].is_valid() && p_slot.requests[i]->get_status() == DSRequest::STATUS_FAILED) {
			p_slot.error_code = -1;
			p_slot.error_message = p_slot.requests[i]->get_error_message();
			if (p_slot.error_message.is_empty()) {
				p_slot.error_message = "DirectStorage GPU batch failed.";
			}
			break;
		}
	}
	for (int i = 0; i < p_slot.requests.size(); i++) {
		if (p_slot.requests[i].is_valid()) {
			p_slot.requests[i]->set_gpu_in_flight(false);
		}
	}
	release_resource_guards(p_slot.requests);
	close_slot_files(p_slot);
	p_slot.submitted = false;
	p_slot.completion_ready = true;
}
#endif

#ifdef _WIN32
// Slot lifetime close: drops the slot's reference to every handle it holds.
// The handle itself is only closed when no other caller, slot or the cache
// still needs it (see release_source_file), because the same file is shared by
// every request in a batch and across queues. Sole closer for BatchSlot files;
// all reset, completion, cancel, and shutdown paths go through here.
void Win32Backend::clear_completed_slot_locked(BatchSlot &r_slot) {
	r_slot.requests.clear();
	r_slot.request_names.clear();
	r_slot.request_name_map.clear();
	r_slot.dstorage_requests.clear();
	r_slot.fence_value = 0;
	r_slot.completed_count = 0;
	r_slot.error_code = 0;
	r_slot.error_message = String();
}

void Win32Backend::close_slot_files(BatchSlot &p_slot) {
	for (std::unordered_map<std::wstring, IDStorageFile *>::iterator it = p_slot.file_cache.begin(); it != p_slot.file_cache.end(); ++it) {
		release_source_file(it->first, it->second);
	}
	p_slot.file_cache.clear();
	p_slot.files.clear();
	p_slot.dstorage_requests.clear();
}

// Releases every backend-level cached file handle. Caller must hold
// batch_mutex or guarantee no concurrent enqueue (init before the
// completion thread starts, shutdown after it joins).
void Win32Backend::clear_persistent_files() {
	std::lock_guard<std::mutex> f_lock(persistent_file_mutex);
	for (std::unordered_map<std::wstring, CachedSourceFile>::iterator it = persistent_file_cache.begin(); it != persistent_file_cache.end(); ++it) {
		if (it->second.refs > 0) {
			UtilityFunctions::push_warning("Win32Backend: Releasing a cached source file that still has ", (int64_t)it->second.refs, " outstanding reference(s): ", String(it->first.c_str()));
		}
		if (it->second.file) {
			it->second.file->Close();
			it->second.file->Release();
			it->second.file = nullptr;
		}
	}
	persistent_file_cache.clear();
	clear_gdeflate_validation_cache();
}

// Returns a handle for p_wpath with one caller reference. The caller must hand
// that reference to a slot (register_slot_source_file) or drop it
// (release_source_file). A path with no cached entry is opened fresh, so the
// bytes match the file on disk at open time; a path with an entry cannot have
// been replaced, because the open handle blocks deletion/rename, so every
// request in the batch can share it.
IDStorageFile *Win32Backend::resolve_source_file(const std::wstring &p_wpath, String &r_error) {
	{
		std::lock_guard<std::mutex> f_lock(persistent_file_mutex);
		std::unordered_map<std::wstring, CachedSourceFile>::iterator it = persistent_file_cache.find(p_wpath);
		if (it != persistent_file_cache.end() && it->second.file) {
			it->second.refs++;
			return it->second.file;
		}
	}

	IDStorageFile *file = nullptr;
	if (!factory || FAILED(factory->OpenFile(p_wpath.c_str(), IID_PPV_ARGS(&file))) || !file) {
		r_error = "Win32Backend: Failed to open file '" + String(p_wpath.c_str()) + "'.";
		return nullptr;
	}

	std::lock_guard<std::mutex> f_lock(persistent_file_mutex);
	std::unordered_map<std::wstring, CachedSourceFile>::iterator it = persistent_file_cache.find(p_wpath);
	if (it != persistent_file_cache.end() && it->second.file) {
		// Another thread opened the same path while we were away: first opener
		// wins, so share its handle and drop ours.
		it->second.refs++;
		file->Close();
		file->Release();
		return it->second.file;
	}
	CachedSourceFile entry;
	entry.file = file;
	entry.refs = 1;
	persistent_file_cache[p_wpath] = entry;
	return file;
}

// Drops one caller/slot reference. The handle is closed and the entry erased
// when the last reference goes away, which is what unblocks replacing the file
// on disk. Nothing is retained between batches, so the next resolve opens the
// file as it is on disk at that point.
void Win32Backend::release_source_file(const std::wstring &p_wpath, IDStorageFile *p_file) {
	if (!p_file) {
		return;
	}
	std::lock_guard<std::mutex> f_lock(persistent_file_mutex);
	std::unordered_map<std::wstring, CachedSourceFile>::iterator it = persistent_file_cache.find(p_wpath);
	if (it == persistent_file_cache.end() || it->second.file != p_file) {
		// Entry already dropped (clear_persistent_files): this handle is no
		// longer reachable from the cache, so close and drop our own reference.
		p_file->Close();
		p_file->Release();
		return;
	}
	if (it->second.refs > 0) {
		it->second.refs--;
	}
	if (it->second.refs == 0) {
		it->second.file->Close();
		it->second.file->Release();
		persistent_file_cache.erase(it);
	}
}

// Transfers the caller's reference into the slot, once per path: every request
// in a batch shares the slot's reference.
void Win32Backend::register_slot_source_file(BatchSlot &r_slot, const std::wstring &p_wpath, IDStorageFile *p_file) {
	if (!p_file) {
		return;
	}
	if (r_slot.file_cache.find(p_wpath) != r_slot.file_cache.end()) {
		release_source_file(p_wpath, p_file);
		return;
	}
	r_slot.file_cache[p_wpath] = p_file;
	r_slot.files.push_back(p_file);
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
// Test-only: live completion threads process-wide. The object a leaked thread
// outlived cannot report it, so the count has to live outside it.
static std::atomic<int> s_live_completion_threads{ 0 };

struct LiveCompletionThreadScope {
	LiveCompletionThreadScope() { s_live_completion_threads.fetch_add(1, std::memory_order_acq_rel); }
	~LiveCompletionThreadScope() { s_live_completion_threads.fetch_sub(1, std::memory_order_acq_rel); }
};
#endif

void Win32Backend::completion_thread_func() {
	constexpr int kSlotHandleCount = QUEUE_COUNT * BATCH_SLOT_COUNT;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	LiveCompletionThreadScope live_scope;
#endif
	HANDLE wait_handles[kSlotHandleCount + QUEUE_COUNT + 1] = { completion_stop_event };
	for (int i = 0; i < QUEUE_COUNT; i++) {
		for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
			wait_handles[1 + i * BATCH_SLOT_COUNT + slot_index] = priority_queues[i].batch_slots[slot_index].event;
		}
		// One handle per queue. RetrieveErrorRecord is only legal once the SDK has
		// signalled this event, so it is the sole drain trigger; without it a
		// failure with no batch retiring would never be read at all.
		wait_handles[1 + kSlotHandleCount + i] = priority_queues[i].error_event;
	}
	auto fail_pending_batches = [this](const String &p_message) {
		for (BatchMetadata &batch : completion_queue) {
			QueueState &queue = priority_queues[batch.queue_index];
			BatchSlot &slot = queue.batch_slots[batch.slot_index];
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message(p_message);
				}
			}
			bool fence_completed = queue.completion_fence &&
					queue.completion_fence->GetCompletedValue() >= slot.fence_value;
			if (fence_completed || !slot.submitted) {
				close_slot_files(slot);
				release_resource_guards(slot.requests);
				for (int i = 0; i < slot.requests.size(); i++) {
					if (slot.requests[i].is_valid()) {
						slot.requests[i]->set_gpu_in_flight(false);
					}
				}
			}
			complete_chunked_requests();
			slot.completed_count = count_completed_requests(slot.requests);
			slot.error_code = -1;
			slot.error_message = p_message;
			slot.submitted = false;
			slot.completion_ready = true;
		}
		completion_queue.clear();
		batch_in_flight.store(false);
	};
	while (true) {
		{
			std::unique_lock<std::mutex> lock(batch_mutex);
			batch_cv.wait(lock, [this]() {
				return completion_thread_stop.load() || !completion_queue.empty();
			});
			if (completion_thread_stop.load()) {
				fail_pending_batches("DirectStorage backend shut down before batch completion.");
				return;
			}
		}

		DWORD wait_result = WaitForMultipleObjects(kSlotHandleCount + QUEUE_COUNT + 1, wait_handles, FALSE, 500);
		if (wait_result == WAIT_TIMEOUT) {
			// Auto-reset slot events can be missed when a stale signal belongs to a
			// previous batch on the same slot. Poll the fences as a liveness backstop.
			poll_batch_status();
			continue;
		}
		if (wait_result == WAIT_OBJECT_0) {
			std::lock_guard<std::mutex> lock(batch_mutex);
			fail_pending_batches("DirectStorage backend shut down before batch completion.");
			return;
		}
		if (wait_result < WAIT_OBJECT_0 + 1 || wait_result >= WAIT_OBJECT_0 + kSlotHandleCount + QUEUE_COUNT + 1) {
			std::lock_guard<std::mutex> lock(batch_mutex);
			fail_pending_batches("DirectStorage batch completion wait failed.");
			return;
		}

		const int signaled_index = static_cast<int>(wait_result - WAIT_OBJECT_0);
		if (signaled_index > kSlotHandleCount) {
			// Queue error event: drain it (the only legal read) and report after the
			// lock this handler takes internally has been released.
			handle_queue_error_event(signaled_index - kSlotHandleCount - 1);
			continue;
		}
		int completed_handle_index = signaled_index - 1;
		int completed_queue_index = completed_handle_index / BATCH_SLOT_COUNT;
		int completed_slot_index = completed_handle_index % BATCH_SLOT_COUNT;
		BatchMetadata batch;
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			std::deque<BatchMetadata>::iterator batch_it = completion_queue.end();
			for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ++it) {
				if (it->queue_index != completed_queue_index || it->slot_index != completed_slot_index) {
					continue;
				}
				const QueueState &queue_state = priority_queues[it->queue_index];
				const BatchSlot &slot_state = queue_state.batch_slots[it->slot_index];
				// A stale slot event may outlive the batch that set it. Only retire a
				// batch whose own fence has actually completed; otherwise leave it in
				// completion_queue for the fence poll / a later signal.
				if (!slot_state.submitted || slot_state.completion_ready || slot_state.fence_value != it->fence_value ||
						!queue_state.completion_fence ||
						queue_state.completion_fence->GetCompletedValue() < it->fence_value) {
					continue;
				}
				batch_it = it;
				break;
			}
			if (batch_it == completion_queue.end()) {
				continue;
			}
			batch = *batch_it;
			completion_queue.erase(batch_it);
			BatchSlot &completed_slot = priority_queues[batch.queue_index].batch_slots[batch.slot_index];
			ResetEvent(completed_slot.event);
		}

		BatchReport report;
		{
			std::lock_guard<std::mutex> lock(batch_mutex);
			complete_batch_slot(priority_queues[batch.queue_index], priority_queues[batch.queue_index].batch_slots[batch.slot_index], batch, report);
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
		}
		report_batch_outcome(report);

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();

		uint64_t end_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
		uint64_t duration_us = (end_time > batch.start_time) ? (end_time - batch.start_time) : 1;
		DSProfiler *profiler = DSProfiler::get_singleton();
		if (profiler) {
			ds_profiler_set_batch_metrics_timed(
					batch.compressed_bytes,
					batch.uncompressed_bytes,
					batch.request_count,
					-1,
					(err_handler && err_handler->get_error_sequence() != batch.error_sequence) ? 1 : 0,
					static_cast<int64_t>(batch.start_time),
					static_cast<int64_t>(end_time));
				profiler->record_workload(batch.compressed_bytes, duration_us);
		}
		batch_cv.notify_all();
	}
}
#endif

#ifdef _WIN32
// The process-wide cache is deliberately never destroyed. A ComPtr destroyed
// during DLL_PROCESS_DETACH calls Release() with the loader lock held; when that
// releases the last reference the D3D12 object is destroyed there, the driver
// blocks on a thread that needs the same lock, and process exit hangs forever
// with zero CPU. The references are dropped explicitly from module deinit by
// ds_win32_release_static_d3d12_state(); these heap objects are then reclaimed
// by process exit.
static Microsoft::WRL::ComPtr<ID3D12Device> &s_d3d12_device = *new Microsoft::WRL::ComPtr<ID3D12Device>();
static Microsoft::WRL::ComPtr<ID3D12CommandQueue> &s_d3d12_command_queue = *new Microsoft::WRL::ComPtr<ID3D12CommandQueue>();
static std::mutex s_device_mutex;

// Process-wide device acquisition rendezvous.
//
// The fetch itself must run on the render thread because
// RenderingDevice::get_driver_resource() is render-thread affine, but callers
// reach it from worker threads. Every off-render-thread caller joins the same
// generation, so a single posted callable serves all of them and no caller can
// steal another caller's hand-off. A give-up deliberately does NOT cancel the
// request: the post stays queued, the next RenderingServer::sync() still runs
// it, and the device is cached for the following call instead of being fetched
// again.
//
// All fields below are guarded by s_device_acquire_mutex. No COM object and no
// Godot Ref is stored here, so this state is not subject to the module-deinit
// release rule that covers the D3D12-owning statics above.
static std::mutex s_device_acquire_mutex;
static std::condition_variable s_device_acquire_cv;
static uint64_t s_device_acquire_generation = 0;
static uint64_t s_device_acquire_completed_generation = 0;
static uint64_t s_device_acquire_logged_generation = 0;
static bool s_device_acquire_posted = false;

// Main-loop liveness probe. Engine::get_process_frames() advances once per
// Main::iteration(), so a frozen value means the loop that drains the
// cross-thread render queue is not running: in the default single-threaded
// renderer that drain happens in RenderingServerDefault::sync() on the main
// thread, and only that thread can perform it.
static uint64_t ds_win32_main_loop_frames() {
	Engine *engine = Engine::get_singleton();
	return engine ? engine->get_process_frames() : 0;
}

// Log-only adapter identity. Compares Godot's D3D12 adapter LUID against
// the default DXGI adapter; a mismatch is informational only (multi-GPU)
// and never fails acquisition.
static void ds_win32_log_d3d12_adapter_identity(ID3D12Device *p_device) {
	if (!p_device) {
		return;
	}
	LUID luid = p_device->GetAdapterLuid();
	char luid_buf[64];
	snprintf(luid_buf, sizeof(luid_buf), "%08X-%08X", (unsigned int)luid.HighPart, (unsigned int)luid.LowPart);
	UtilityFunctions::print_verbose(String("[DirectStorage] D3D12 device adapter LUID: ") + String(luid_buf) + ".");
	IDXGIFactory1 *factory = nullptr;
	if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
		IDXGIAdapter1 *default_adapter = nullptr;
		if (SUCCEEDED(factory->EnumAdapters1(0, &default_adapter)) && default_adapter) {
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(default_adapter->GetDesc1(&desc))) {
				bool match = (desc.AdapterLuid.HighPart == luid.HighPart && desc.AdapterLuid.LowPart == luid.LowPart);
				if (match) {
					UtilityFunctions::print_verbose("[DirectStorage] D3D12 adapter is the default DXGI adapter.");
				} else {
					UtilityFunctions::print_verbose("[DirectStorage] D3D12 adapter is NOT the default DXGI adapter (multi-GPU system; informational only).");
				}
			}
			default_adapter->Release();
		}
		factory->Release();
	}
}

// Direct fetch; the caller must already be on the render thread.
static Microsoft::WRL::ComPtr<ID3D12Device> ds_win32_fetch_d3d12_device_direct() {
	RenderingServer *rs = safe_get_rendering_server();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return nullptr;
	}
	RenderingDevice *rd = rs->get_rendering_device();
	if (!rd) {
		return nullptr;
	}
	uint64_t handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_LOGICAL_DEVICE, RID(), 0);
	if (handle == 0) {
		return nullptr;
	}
	ID3D12Device *device = reinterpret_cast<ID3D12Device *>(handle);
	uint64_t queue_handle = rd->get_driver_resource(RenderingDevice::DRIVER_RESOURCE_COMMAND_QUEUE, RID(), 0);
	ID3D12CommandQueue *queue = reinterpret_cast<ID3D12CommandQueue *>(queue_handle);
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		s_d3d12_device = device;
		s_d3d12_command_queue = queue;
	}
	ds_win32_log_d3d12_adapter_identity(device);
	return s_d3d12_device;
}

// Render-thread worker posted via RenderingServer::call_on_render_thread().
// Must be a file-static function: PlatformBackend is not a Godot Object, so
// callable_mp on `this` is impossible and callable_mp_static is required.
static void ds_win32_fetch_d3d12_device_on_render_thread() {
	ds_win32_fetch_d3d12_device_direct();
	std::lock_guard<std::mutex> lock(s_device_acquire_mutex);
	// s_device_acquire_posted stayed true from the post until here, so no newer
	// generation could have started: completing the current one is exact.
	s_device_acquire_completed_generation = s_device_acquire_generation;
	s_device_acquire_posted = false;
	s_device_acquire_cv.notify_all(); // notify while holding the protecting mutex
}

// D3D12-02 / HIGH-03: release owned COM references and clear the cache. Safe from any
// thread; COM Release is thread-safe. Called on shutdown and on
// device-removed detection so a stale pointer can never be returned.
void ds_win32_invalidate_d3d12_device() {
	std::lock_guard<std::mutex> lock(s_device_mutex);
	s_d3d12_device.Reset();
	s_d3d12_command_queue.Reset();
}

Microsoft::WRL::ComPtr<ID3D12Device> ds_win32_get_d3d12_device() {
	// D3D12-02 / HIGH-03: validate cached device against device-removed (TDR / driver update).
	Microsoft::WRL::ComPtr<ID3D12Device> cached;
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		cached = s_d3d12_device;
	}
	if (cached) {
		HRESULT removed = cached->GetDeviceRemovedReason();
		if (FAILED(removed)) {
			ds_win32_invalidate_d3d12_device();
			cached.Reset();
		} else {
			return cached;
		}
	}

	RenderingServer *rs = safe_get_rendering_server();
	if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
		return nullptr;
	}

	// Fast path: already on the render thread, fetch directly.
	if (rs->is_on_render_thread()) {
		return ds_win32_fetch_d3d12_device_direct();
	}

	// Marshal acquisition to the render thread. A live render pump answers within
	// one frame, so the wait is short; it is also bounded by main-loop liveness,
	// because in single-threaded rendering only the main thread can drain the
	// cross-thread render queue and waiting there is otherwise pure dead time.
	uint64_t my_generation = 0;
	{
		bool post_request = false;
		{
			std::lock_guard<std::mutex> lock(s_device_acquire_mutex);
			if (!s_device_acquire_posted) {
				s_device_acquire_generation++;
				s_device_acquire_posted = true;
				post_request = true;
			}
			my_generation = s_device_acquire_generation;
		}
		if (post_request) {
			// Post outside our mutex: never hand work to the engine while holding one.
			rs->call_on_render_thread(callable_mp_static(ds_win32_fetch_d3d12_device_on_render_thread));
		}
	}

	constexpr std::chrono::milliseconds kSlice(50);
	constexpr std::chrono::milliseconds kFrozenGiveUp(400);
	constexpr std::chrono::milliseconds kCeiling(5000);
	const bool liveness_gate = !Thread::is_main_thread();
	const auto wait_start = std::chrono::steady_clock::now();
	std::chrono::milliseconds waited(0);
	bool gave_up = false;
	bool log_give_up = false;
	{
		std::unique_lock<std::mutex> lock(s_device_acquire_mutex);
		uint64_t last_frames = ds_win32_main_loop_frames();
		auto frozen_since = wait_start;
		for (;;) {
			if (s_device_acquire_completed_generation >= my_generation) {
				break;
			}
			if (s_device_acquire_cv.wait_for(lock, kSlice, [my_generation] {
						return s_device_acquire_completed_generation >= my_generation;
					})) {
				break;
			}
			const auto now = std::chrono::steady_clock::now();
			const uint64_t frames = ds_win32_main_loop_frames();
			if (frames != last_frames) {
				last_frames = frames;
				frozen_since = now;
			} else if (liveness_gate && now - frozen_since >= kFrozenGiveUp) {
				gave_up = true;
				break;
			}
			if (now - wait_start >= kCeiling) {
				gave_up = true;
				break;
			}
		}
		waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - wait_start);
		if (gave_up && s_device_acquire_logged_generation < my_generation) {
			s_device_acquire_logged_generation = my_generation;
			log_give_up = true;
		}
	}
	if (gave_up) {
		if (log_give_up) {
			// The post stays queued: the next sync still caches the device.
			UtilityFunctions::print("[DirectStorage] D3D12 device acquisition gave up after " +
					String::num_uint64(static_cast<uint64_t>(waited.count())) +
					" ms (main loop not advancing); the request stays queued.");
		}
		return nullptr;
	}

	std::lock_guard<std::mutex> lock(s_device_mutex);
	return s_d3d12_device;
}

Microsoft::WRL::ComPtr<ID3D12CommandQueue> ds_win32_get_d3d12_command_queue() {
	{
		std::lock_guard<std::mutex> lock(s_device_mutex);
		if (s_d3d12_command_queue) {
			return s_d3d12_command_queue;
		}
	}

	ds_win32_get_d3d12_device();

	std::lock_guard<std::mutex> lock(s_device_mutex);
	return s_d3d12_command_queue;
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
bool ds_win32_debug_probe_texture_region(const RID &p_texture, uint32_t p_subresource,
		uint32_t p_top, uint32_t p_bottom, const String &p_path, uint64_t p_offset,
		uint32_t p_size, uint32_t p_uncompressed_size, String &r_error) {
	r_error = String();
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	uint64_t native_format = 0;
	bool rd_available = false;
	if (!ds_win32_query_destination_resource_on_render_thread(false, p_texture, resource, desc, native_format, rd_available) ||
			resource == nullptr) {
		r_error = "probe: could not resolve the destination texture to an ID3D12Resource.";
		return false;
	}
	Microsoft::WRL::ComPtr<ID3D12Device> device = ds_win32_get_d3d12_device();
	if (!device) {
		r_error = "probe: no D3D12 device.";
		return false;
	}
	Microsoft::WRL::ComPtr<IDStorageFactory> factory;
	HRESULT hr = DStorageGetFactory(IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory) {
		r_error = "probe: DStorageGetFactory failed 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ".";
		return false;
	}

	std::wstring wide_path(reinterpret_cast<const wchar_t *>(p_path.utf16().get_data()));
	Microsoft::WRL::ComPtr<IDStorageFile> file;
	hr = factory->OpenFile(wide_path.c_str(), IID_PPV_ARGS(&file));
	if (FAILED(hr) || !file) {
		r_error = "probe: OpenFile failed 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ".";
		return false;
	}

	Microsoft::WRL::ComPtr<IDStorageQueue1> queue;
	DSTORAGE_QUEUE_DESC queue_desc = {};
	queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	queue_desc.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
	queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
	queue_desc.Name = "M0RegionProbe";
	queue_desc.Device = device.Get();
	hr = factory->CreateQueue(&queue_desc, IID_PPV_ARGS(&queue));
	if (FAILED(hr) || !queue) {
		file->Close();
		r_error = "probe: CreateQueue failed 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ".";
		return false;
	}

	HANDLE completion_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	HANDLE error_event = queue->GetErrorEvent();
	if (!completion_event || !error_event) {
		if (completion_event) {
			CloseHandle(completion_event);
		}
		queue->Close();
		file->Close();
		r_error = "probe: could not create the completion event.";
		return false;
	}

	DSTORAGE_REQUEST request = {};
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION;
	request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_GDEFLATE;
	request.Source.File.Source = file.Get();
	request.Source.File.Offset = p_offset;
	request.Source.File.Size = p_size;
	request.Destination.Texture.Resource = resource;
	request.Destination.Texture.SubresourceIndex = p_subresource;
	D3D12_BOX region = {};
	region.left = 0;
	region.top = p_top;
	region.front = 0;
	region.right = static_cast<UINT>(desc.Width);
	region.bottom = p_bottom;
	region.back = 1;
	request.Destination.Texture.Region = region;
	request.UncompressedSize = p_uncompressed_size;
	request.Name = "M0RegionProbe";

	queue->EnqueueRequest(&request);
	queue->EnqueueSetEvent(completion_event);
	queue->Submit();

	HANDLE waits[2] = { completion_event, error_event };
	const DWORD wait_result = WaitForMultipleObjects(2, waits, FALSE, 5000);
	bool completed = false;
	if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_OBJECT_0 + 1) {
		// A signalled error event MUST be consumed: the queue stops processing
		// until RetrieveErrorRecord runs, so the completion event would never
		// fire and every failure would look like a timeout. WaitForMultipleObjects
		// already reset whichever auto-reset handle it returned.
		DSTORAGE_ERROR_RECORD record = {};
		if (wait_result == WAIT_OBJECT_0 + 1) {
			queue->RetrieveErrorRecord(&record);
		} else if (WaitForSingleObject(error_event, 0) == WAIT_OBJECT_0) {
			queue->RetrieveErrorRecord(&record);
		}
		if (record.FailureCount > 0 || FAILED(record.FirstFailure.HResult)) {
			r_error = "probe: FailureCount=" + String::num_int64(record.FailureCount) +
					" HResult=0x" + String::num_uint64(static_cast<uint32_t>(record.FirstFailure.HResult), 16) +
					" CommandType=" + String::num_int64(static_cast<int64_t>(record.FirstFailure.CommandType)) +
					" RequestName='" + String(record.FirstFailure.Request.RequestName) + "'.";
			completed = false;
		} else {
			// The record was consumed (or was never pending); the completion event
			// may still be the one that has to fire.
			completed = wait_result == WAIT_OBJECT_0 ||
					WaitForSingleObject(completion_event, 2000) == WAIT_OBJECT_0;
			if (!completed) {
				r_error = "probe: error event consumed with FailureCount=0 but the completion event never fired.";
			}
		}
	} else {
		r_error = "probe: timed out waiting for the queue (wait_result=" + String::num_int64(static_cast<int64_t>(wait_result)) + ").";
	}

	queue->Close();
	file->Close();
	CloseHandle(completion_event);
	return completed;
}
#endif

// Shared DirectStorage runtime probe state. `is_available()` loads the
// runtime DLLs to verify factory/queue creation, then stashes the still-loaded
// modules here instead of unloading them. `initialize()` claims them for the
// same bin directory, so each process performs a single LoadLibrary pair.
// Guarded by s_probe_mutex; `shutdown()` releases any modules the probe
// stashed but `initialize()` never claimed.
static std::mutex s_probe_mutex;
static HMODULE s_probed_core_module = nullptr;
static HMODULE s_probed_dstorage_module = nullptr;
static std::wstring s_probed_bin_dir;

// Returns true and fills r_core/r_dstorage when a previous successful probe
// stashed loaded modules for the same bin directory. Caller takes ownership.
static bool claim_probed_runtime_modules(const String &p_bin_dir, HMODULE &r_core, HMODULE &r_dstorage) {
	std::lock_guard<std::mutex> lock(s_probe_mutex);
	std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data()));
	if (!s_probed_core_module || !s_probed_dstorage_module || s_probed_bin_dir != w_bin_dir) {
		return false;
	}
	r_core = s_probed_core_module;
	r_dstorage = s_probed_dstorage_module;
	s_probed_core_module = nullptr;
	s_probed_dstorage_module = nullptr;
	s_probed_bin_dir.clear();
	return true;
}

// Renderer capability gate shared by the capability probe and backend init.
// DirectStorage queues require the live D3D12 RenderingDevice, and the
// rendering driver is fixed for the process lifetime, so a non-empty
// non-D3D12 driver is a permanent negative: cache it BEFORE paying for
// LoadLibrary of the runtime and DStorageGetFactory (repeated initialize()
// attempts would otherwise re-acquire the whole runtime just to fail the same
// check). An empty driver name means the RenderingServer is not up yet and is
// deliberately NOT cached.
struct DSRendererGate {
	std::mutex mutex;
	bool negative = false;
	std::string driver_utf8;
};

static DSRendererGate &ds_renderer_gate() {
	// Function-local static: constructed on first use, i.e. after the
	// GDExtension interface exists (a namespace-scope godot::String here
	// would run inside DllMain and crash with Error 1114).
	static DSRendererGate gate;
	return gate;
}

static String ds_renderer_gate_reason(const String &p_driver_name) {
	return "Rendering driver is '" + p_driver_name + "' (Direct3D 12 is required for DirectStorage GPU queues).";
}

// Returns true when the live renderer can host DirectStorage GPU queues; on
// failure fills r_reason with the user-facing cause.
static bool ds_renderer_supports_dstorage(String &r_reason) {
	DSRendererGate &gate = ds_renderer_gate();
	{
		std::lock_guard<std::mutex> lock(gate.mutex);
		if (gate.negative) {
			r_reason = ds_renderer_gate_reason(String(gate.driver_utf8.c_str()));
			return false;
		}
	}

	RenderingServer *rs = safe_get_rendering_server();
	const String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : String();
	if (driver_name != "d3d12") {
		r_reason = ds_renderer_gate_reason(driver_name);
		if (!driver_name.is_empty()) {
			std::lock_guard<std::mutex> lock(gate.mutex);
			gate.negative = true;
			gate.driver_utf8 = driver_name.utf8().get_data();
		}
		return false;
	}
	if (!rs->get_rendering_device()) {
		r_reason = "A usable D3D12 RenderingDevice is not active.";
		return false;
	}
	return true;
}

static DSRuntimeProbe probe_runtime(const String &p_bin_dir, uint16_t p_queue_capacity) {
	DSRuntimeProbe result;
	HMODULE core = nullptr;
	HMODULE dstorage = nullptr;

	// Cheapest and most permanent precondition first: without a live D3D12
	// renderer, loading the runtime and creating a factory can only fail.
	String renderer_reason;
	if (!ds_renderer_supports_dstorage(renderer_reason)) {
		result.reason = renderer_reason;
		return result;
	}

	std::wstring w_bin_dir(reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data()));

	core = Win32Backend::load_runtime_module(p_bin_dir, "dstoragecore.dll");
	if (!core) {
		result.reason = "DirectStorage runtime DLL 'dstoragecore.dll' could not be loaded from '" + p_bin_dir + "'.";
		return result;
	}
	dstorage = Win32Backend::load_runtime_module(p_bin_dir, "dstorage.dll");
	if (!dstorage) {
		result.reason = "DirectStorage runtime DLL 'dstorage.dll' could not be loaded from '" + p_bin_dir + "'.";
		unload_runtime_modules(core, nullptr);
		return result;
	}

	Win32Backend::apply_dstorage_configuration();

	IDStorageFactory *probe_factory = nullptr;
	HRESULT hr = DStorageGetFactory(IID_PPV_ARGS(&probe_factory));
	if (FAILED(hr) || !probe_factory) {
		result.reason = "DStorageGetFactory failed (HRESULT: 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ").";
		unload_runtime_modules(core, dstorage);
		return result;
	}

	DSTORAGE_QUEUE_DESC queue_desc = {};
	queue_desc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	queue_desc.Capacity = p_queue_capacity;
	queue_desc.Priority = DSTORAGE_PRIORITY_NORMAL;
	queue_desc.Name = "DirectStorageCapabilityProbe";
	queue_desc.Device = nullptr;
	IDStorageQueue3 *probe_queue = nullptr;
	hr = probe_factory->CreateQueue(&queue_desc, IID_PPV_ARGS(&probe_queue));
	if (FAILED(hr) || !probe_queue) {
		result.reason = "DirectStorage queue creation failed (HRESULT: 0x" + String::num_uint64(static_cast<uint32_t>(hr), 16) + ").";
		probe_factory->Release();
		unload_runtime_modules(core, dstorage);
		return result;
	}

	probe_queue->Close();
	probe_queue->Release();
	probe_factory->Release();
	{
		// Hand the still-loaded modules to initialize() instead of
		// unloading them; a newer probe replaces (and unloads) the old stash.
		std::lock_guard<std::mutex> lock(s_probe_mutex);
		unload_runtime_modules(s_probed_core_module, s_probed_dstorage_module);
		s_probed_core_module = core;
		s_probed_dstorage_module = dstorage;
		s_probed_bin_dir = reinterpret_cast<const wchar_t *>(p_bin_dir.utf16().get_data());
	}
	result.available = true;
	result.reason = "DirectStorage runtime, D3D12 driver, factory, and queue are available.";
	return result;
}
#endif

Win32Backend::StorageMediaClass Win32Backend::classify_storage_media(bool p_is_nvme, bool p_seek_penalty_known,
		bool p_incurs_seek_penalty, bool p_trim_known, bool p_trim_enabled) {
	if (p_is_nvme) {
		return StorageMediaClass::Flash;
	}
	if (p_seek_penalty_known) {
		return p_incurs_seek_penalty ? StorageMediaClass::Rotational : StorageMediaClass::Flash;
	}
	if (p_trim_known && p_trim_enabled) {
		return StorageMediaClass::Flash;
	}
	return StorageMediaClass::Unknown;
}

Dictionary Win32Backend::get_drive_info_for_path(const String &p_path) {
	Dictionary res;
	res["drive"] = "";
	res["bus_type"] = "Unknown";
	res["file_system"] = "Unknown";
	res["bypassio_state"] = "Unsupported";
	res["bypassio_failing_driver"] = "";
	res["bypassio_failure_reason"] = "";
	res["media_type"] = "Unknown";
	res["rotational"] = false;
	res["rotational_known"] = false;
	res["seek_penalty"] = "Unknown";
	res["trim"] = "Unknown";
	res["directstorage_supported"] = false;
	res["bypassio_supported"] = false;
	res["force_file_buffering"] = false;
	res["reason"] = "";

#ifdef _WIN32
	String global_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	bool allow_relaxed_gate = true;
	if (ps && ps->has_setting("dsgd_extension/allow_directstorage_without_bypassio")) {
		allow_relaxed_gate = static_cast<bool>(ps->get_setting("dsgd_extension/allow_directstorage_without_bypassio"));
	}
	if (ps && (p_path.begins_with("res://") || p_path.begins_with("user://"))) {
		global_path = ps->globalize_path(p_path);
	}
	if (global_path.strip_edges().is_empty()) {
		global_path = "C:\\";
	}

	String norm_path = global_path.replace("/", "\\");
	if (norm_path.begins_with("\\\\?\\UNC\\") || norm_path.begins_with("\\\\")) {
		res["drive"] = norm_path;
		res["bus_type"] = "Network";
		res["file_system"] = "SMB";
		res["bypassio_state"] = "Unsupported";

		DSDriveSupportInput unc_input;
		unc_input.bus_type = "Network";
		unc_input.file_system = "SMB";
		unc_input.is_unc = true;
		const DSDriveSupport support = resolve_drive_support(unc_input);
		res["directstorage_supported"] = support.directstorage_supported;
		res["bypassio_supported"] = support.bypassio_supported;
		res["force_file_buffering"] = support.force_file_buffering;
		res["reason"] = support.reason;
		return res;
	}

	std::wstring w_norm_path(reinterpret_cast<const wchar_t *>(norm_path.utf16().get_data()));
	wchar_t volume_path[MAX_PATH] = {0};
	String drive_root;
	if (GetVolumePathNameW(w_norm_path.c_str(), volume_path, MAX_PATH)) {
		drive_root = String(reinterpret_cast<const char16_t *>(volume_path));
	} else {
		drive_root = norm_path.substr(0, 2) + "\\";
		if (drive_root.length() < 3 || drive_root[1] != ':') {
			drive_root = "C:\\";
		}
	}
	if (!drive_root.ends_with("\\")) {
		drive_root += "\\";
	}
	res["drive"] = drive_root;

	// 3. Query FileSystem Name
	wchar_t fs_name[MAX_PATH] = {0};
	wchar_t vol_name[MAX_PATH] = {0};
	std::wstring w_drive_root(reinterpret_cast<const wchar_t *>(drive_root.utf16().get_data()));
	if (GetVolumeInformationW(w_drive_root.c_str(), vol_name, MAX_PATH, nullptr, nullptr, nullptr, fs_name, MAX_PATH)) {
		res["file_system"] = String(reinterpret_cast<const char16_t *>(fs_name));
	}

	// 4. Query Storage BusType & BypassIO State
	String volume_device;
	wchar_t volume_name_guid[MAX_PATH] = {0};
	if (GetVolumeNameForVolumeMountPointW(w_drive_root.c_str(), volume_name_guid, MAX_PATH)) {
		String guid_str = String(reinterpret_cast<const char16_t *>(volume_name_guid));
		if (guid_str.ends_with("\\")) {
			guid_str = guid_str.substr(0, guid_str.length() - 1);
		}
		volume_device = guid_str;
	} else if (drive_root.length() >= 2 && drive_root[1] == ':') {
		volume_device = "\\\\.\\" + drive_root.substr(0, 2);
	} else {
		volume_device = "\\\\.\\C:";
	}
	std::wstring w_volume_device(reinterpret_cast<const wchar_t *>(volume_device.utf16().get_data()));

	HANDLE hDevice = CreateFileW(
			w_volume_device.c_str(),
			0,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_EXISTING,
			0,
			nullptr);

	bool seek_penalty_known = false;
	bool incurs_seek_penalty = false;
	bool trim_known = false;
	bool trim_enabled = false;

	if (hDevice != INVALID_HANDLE_VALUE) {
		STORAGE_PROPERTY_QUERY query = {};
		query.PropertyId = StorageDeviceProperty;
		query.QueryType = PropertyStandardQuery;

		uint8_t dev_buf[512] = {};
		DWORD bytes_returned = 0;

		if (DeviceIoControl(
					hDevice,
					IOCTL_STORAGE_QUERY_PROPERTY,
					&query,
					sizeof(query),
					dev_buf,
					sizeof(dev_buf),
					&bytes_returned,
					nullptr) &&
				bytes_returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
			const auto *dev_desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(dev_buf);
			switch (dev_desc->BusType) {
				case BusTypeNvme:
					res["bus_type"] = "NVMe";
					break;
				case BusTypeSata:
					res["bus_type"] = "SATA";
					break;
				case BusTypeAta:
					res["bus_type"] = "ATA";
					break;
				case BusTypeUsb:
					res["bus_type"] = "USB";
					break;
				case BusTypeSd:
					res["bus_type"] = "SD";
					break;
				case BusTypeMmc:
					res["bus_type"] = "MMC";
					break;
				case BusTypeVirtual:
					res["bus_type"] = "Virtual";
					break;
				default:
					res["bus_type"] = "BusType_" + String::num_int64(dev_desc->BusType);
					break;
			}
		}

		// Media class inputs for the I/O-mode decision. Seek penalty is the
		// authoritative signal; TRIM only corroborates flash when the seek
		// query is unavailable.
		STORAGE_PROPERTY_QUERY media_query = {};
		media_query.QueryType = PropertyStandardQuery;

		DEVICE_SEEK_PENALTY_DESCRIPTOR seek_desc = {};
		DWORD seek_bytes = 0;
		media_query.PropertyId = StorageDeviceSeekPenaltyProperty;
		if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &media_query, sizeof(media_query),
					&seek_desc, sizeof(seek_desc), &seek_bytes, nullptr)) {
			seek_penalty_known = true;
			incurs_seek_penalty = seek_desc.IncursSeekPenalty != FALSE;
		}

		DEVICE_TRIM_DESCRIPTOR trim_desc = {};
		DWORD trim_bytes = 0;
		media_query.PropertyId = StorageDeviceTrimProperty;
		if (DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY, &media_query, sizeof(media_query),
					&trim_desc, sizeof(trim_desc), &trim_bytes, nullptr)) {
			trim_known = true;
			trim_enabled = trim_desc.TrimEnabled != FALSE;
		}

		CloseHandle(hDevice);
	}

	// Query Windows Kernel BypassIO state on the mounted filesystem root handle.
	// FSCTL_MANAGE_BYPASS_IO is handled by the NTFS/minifilter stack, not the raw volume device.
	HANDLE hFs = CreateFileW(
			w_drive_root.c_str(),
			FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_NO_BUFFERING,
			nullptr);
	if (hFs != INVALID_HANDLE_VALUE) {
		DSBpioInput bpio_in = {};
		bpio_in.Operation = static_cast<decltype(bpio_in.Operation)>(DS_BPIO_OP_QUERY);
		DSBpioOutput bpio_out = {};
		DWORD bpio_bytes = 0;
		if (DeviceIoControl(
					hFs,
					FSCTL_MANAGE_BYPASS_IO,
					&bpio_in,
					sizeof(bpio_in),
					&bpio_out,
					sizeof(bpio_out),
					&bpio_bytes,
					nullptr) &&
				bpio_bytes >= 24) {
			const bool stack_paused = (static_cast<uint32_t>(bpio_out.OutFlags) & DS_BPIO_OUTFLAG_VOLUME_STACK_BYPASS_PAUSED) != 0;
			const bool query_ok = bpio_out.Query.OpStatus == 0;
			res["bypassio_state"] = (query_ok && !stack_paused) ? "Enabled" : "Disabled";

			const size_t max_driver_chars = sizeof(bpio_out.Query.FailingDriverName) / sizeof(wchar_t);
			const size_t driver_chars = wcsnlen(bpio_out.Query.FailingDriverName, max_driver_chars);
			if (driver_chars > 0) {
				res["bypassio_failing_driver"] = String::utf16(reinterpret_cast<const char16_t *>(bpio_out.Query.FailingDriverName), static_cast<int>(driver_chars));
			}

			const size_t max_reason_chars = sizeof(bpio_out.Query.FailureReason) / sizeof(wchar_t);
			const size_t reason_chars = wcsnlen(bpio_out.Query.FailureReason, max_reason_chars);
			if (reason_chars > 0) {
				res["bypassio_failure_reason"] = String::utf16(reinterpret_cast<const char16_t *>(bpio_out.Query.FailureReason), static_cast<int>(reason_chars));
			}
		}
		CloseHandle(hFs);
	}

	bool is_nvme = (res["bus_type"] == "NVMe");
	bool is_sata_family = (res["bus_type"] == "SATA" || res["bus_type"] == "ATA");
	bool is_ntfs = (res["file_system"] == "NTFS");

	StorageMediaClass media = classify_storage_media(is_nvme, seek_penalty_known,
			incurs_seek_penalty, trim_known, trim_enabled);
	res["media_type"] = media == StorageMediaClass::Rotational ? "HDD" :
			(media == StorageMediaClass::Flash ? "SSD" : "Unknown");
	res["rotational"] = media == StorageMediaClass::Rotational;
	res["rotational_known"] = media != StorageMediaClass::Unknown;
	res["seek_penalty"] = !seek_penalty_known ? "Unknown" : (incurs_seek_penalty ? "True" : "False");
	res["trim"] = !trim_known ? "Unknown" : (trim_enabled ? "Enabled" : "Disabled");

	DSDriveSupportInput support_input;
	support_input.bus_type = String(res["bus_type"]);
	support_input.file_system = String(res["file_system"]);
	support_input.is_nvme = is_nvme;
	support_input.is_legacy_bus = is_nvme || is_sata_family;
	support_input.is_ntfs = is_ntfs;
	support_input.media = media == StorageMediaClass::Rotational ? DSDriveMedia::Rotational :
			(media == StorageMediaClass::Flash ? DSDriveMedia::Flash : DSDriveMedia::Unknown);
	support_input.allow_relaxed_gate = allow_relaxed_gate;
	const DSDriveSupport support = resolve_drive_support(support_input);
	res["directstorage_supported"] = support.directstorage_supported;
	res["bypassio_supported"] = support.bypassio_supported;
	res["force_file_buffering"] = support.force_file_buffering;
	res["reason"] = support.reason;

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Benchmark seam (never bound, debug/dev only): lets the I/O-mode probe
	// reach media classes the production gate deliberately excludes.
	OS *bench_os = OS::get_singleton();
	if (!res["directstorage_supported"] && bench_os &&
			bench_os->get_environment("DSGD_BENCH_ALLOW_ROTATIONAL") == "1") {
		res["directstorage_supported"] = true;
		res["reason"] = "Benchmark override: bus/media gate bypassed.";
	}
#endif

#else
	res["reason"] = "Platform is non-Windows.";
#endif

	return res;
}

#ifdef _WIN32
void Win32Backend::apply_dstorage_configuration() {
	DSContext *ctx = DSContext::get_singleton();
	DSBackendConfigOptions options;
	options.disable_gpu_decompression = ctx ? ctx->get_disable_gpu_decompression() : false;
	options.disable_gpu_decompression_metacommand = ctx ? ctx->get_disable_gpu_decompression_metacommand() : false;
	options.force_disable_bypass_io = ctx ? ctx->get_force_disable_bypass_io() : false;
	options.bypassio_unavailable = s_bypassio_unavailable.load();
	options.force_file_buffering = s_desired_file_buffering.load();
	// Raw setting, not the resolved budget: 0 must stay 0 so DirectStorage
	// keeps its own best-guess CPU pool size.
	options.cpu_decompression_threads = static_cast<int32_t>(ds_get_max_cpu_threads_setting());
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	// Benchmark seams (never bound, debug/dev only). The production decision
	// comes from the asset volume's media class; the env switches let the probe
	// A/B the I/O modes on the same hardware.
	OS *bench_os = OS::get_singleton();
	if (bench_os) {
		if (bench_os->get_environment("DSGD_BENCH_FILE_BUFFERING") == "1") {
			options.force_file_buffering = true;
		} else if (bench_os->get_environment("DSGD_BENCH_DISABLE_FILE_BUFFERING") == "1") {
			options.force_file_buffering = false;
		}
		if (bench_os->get_environment("DSGD_BENCH_DISABLE_BYPASSIO") == "1") {
			options.force_disable_bypass_io = true;
		}
	}
#endif
	const DSResolvedBackendConfig resolved = resolve_backend_config(options);

	std::lock_guard<std::mutex> lock(s_dstorage_config_mutex);
	// Sampled under the lock: a concurrent load bumps the epoch, so this pair either
	// describes the instance configured below or an older one.
	const HMODULE current_module = GetModuleHandleW(L"dstorage.dll");
	if (current_module == nullptr) {
		return;
	}
	if (has_live_dstorage_configuration_locked()) {
		if (s_applied_file_buffering.load() != resolved.force_file_buffering) {
			UtilityFunctions::push_warning("[DirectStorage] I/O mode changed after the runtime was configured (requested " +
					String(resolved.force_file_buffering ? "buffered" : "unbuffered") +
					"); keeping the already-applied mode. Initialize DirectStorage before any capability probe, or point it at the same volume.");
		}
		return;
	}

	HRESULT hr = E_FAIL;
	if (resolved.force_file_buffering || resolved.disable_bypass_io) {
		DSTORAGE_CONFIGURATION1 config1 = {};
		config1.DisableTelemetry = resolved.disable_telemetry ? TRUE : FALSE;
		config1.DisableGpuDecompression = resolved.disable_gpu_decompression ? TRUE : FALSE;
		config1.DisableGpuDecompressionMetacommand = resolved.disable_gpu_decompression_metacommand ? TRUE : FALSE;
		config1.DisableBypassIO = resolved.disable_bypass_io ? TRUE : FALSE;
		config1.ForceFileBuffering = resolved.force_file_buffering ? TRUE : FALSE;
		config1.NumBuiltInCpuDecompressionThreads = resolved.cpu_decompression_threads;
		hr = DStorageSetConfiguration1(&config1);
		if (SUCCEEDED(hr)) {
			if (resolved.force_file_buffering) {
				UtilityFunctions::print("[DirectStorage] File buffering enabled: rotational media uses ForceFileBuffering (BypassIO disabled).");
			} else if (options.bypassio_unavailable) {
				UtilityFunctions::print("[DirectStorage] BypassIO unavailable on this volume (non-NVMe bus or non-NTFS filesystem); using standard unbuffered I/O with GPU decompression.");
			} else {
				UtilityFunctions::print("[DirectStorage] BypassIO force-disabled via configuration; using standard unbuffered I/O.");
			}
		} else {
			UtilityFunctions::print("[DirectStorage] DStorageSetConfiguration1 failed: 0x" +
					String::num_uint64(static_cast<uint32_t>(hr), 16) + "; continuing with the default I/O path.");
		}
	} else {
		DSTORAGE_CONFIGURATION config = {};
		config.DisableTelemetry = resolved.disable_telemetry ? TRUE : FALSE;
		config.DisableGpuDecompression = resolved.disable_gpu_decompression ? TRUE : FALSE;
		config.DisableGpuDecompressionMetacommand = resolved.disable_gpu_decompression_metacommand ? TRUE : FALSE;
		config.NumBuiltInCpuDecompressionThreads = resolved.cpu_decompression_threads;

		hr = DStorageSetConfiguration(&config);
		if (FAILED(hr)) {
			UtilityFunctions::print_verbose("[DirectStorage] DStorageSetConfiguration returned 0x" +
					String::num_uint64(static_cast<uint32_t>(hr), 16) + " (configuration may already be active).");
		} else {
			UtilityFunctions::print_verbose("[DirectStorage] DStorageSetConfiguration applied successfully.");
		}
	}
	// Recorded either way: once a factory exists this call cannot succeed on the same
	// instance, so retrying it would only repeat the failure log.
	s_dstorage_configured_module = current_module;
	s_dstorage_configured_epoch = s_dstorage_load_epoch;
	s_applied_file_buffering.store(resolved.force_file_buffering);
	s_dstorage_config_apply_count.fetch_add(1);
}

void Win32Backend::release_runtime_module(HMODULE p_module) {
	if (p_module == nullptr) {
		return;
	}
	// Serialised against load_runtime_module() so the count cannot be decremented while a
	// load is deciding whether it starts a new instance cycle. The epoch is *not* bumped
	// here: the next load from a zero count does that.
	std::lock_guard<std::mutex> lock(s_dstorage_config_mutex);
	if (s_dstorage_ref_count > 0) {
		--s_dstorage_ref_count;
	}
	FreeLibrary(p_module);
}

PackedStringArray Win32Backend::get_runtime_search_dirs() {
	PackedStringArray dirs;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps) {
		String addon_bin = ps->globalize_path("res://addons/directstorage_accelerator/bin");
		if (!addon_bin.is_empty()) {
			dirs.push_back(addon_bin);
		}
	}
	OS *os = OS::get_singleton();
	if (os) {
		String exe_dir = os->get_executable_path().get_base_dir();
		if (!exe_dir.is_empty() && !dirs.has(exe_dir)) {
			dirs.push_back(exe_dir);
		}
	}
	return dirs;
}

namespace {

// Reads the VS_FIXEDFILEINFO version resource of a DLL. Returns false when the
// file carries none; a locally rebuilt runtime is still reported by path, just
// without numeric parts.
bool read_dll_file_version(const std::wstring &p_path, uint16_t &r_major, uint16_t &r_minor, uint16_t &r_build, uint16_t &r_revision) {
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(p_path.c_str(), &ignored);
	if (size == 0) {
		return false;
	}
	std::vector<BYTE> buffer(size);
	if (!GetFileVersionInfoW(p_path.c_str(), 0, size, buffer.data())) {
		return false;
	}
	VS_FIXEDFILEINFO *info = nullptr;
	UINT info_size = 0;
	if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID *>(&info), &info_size) || info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
		return false;
	}
	if (info->dwSignature != 0xfeef04bd) {
		return false;
	}
	r_major = static_cast<uint16_t>(HIWORD(info->dwFileVersionMS));
	r_minor = static_cast<uint16_t>(LOWORD(info->dwFileVersionMS));
	r_build = static_cast<uint16_t>(HIWORD(info->dwFileVersionLS));
	r_revision = static_cast<uint16_t>(LOWORD(info->dwFileVersionLS));
	return true;
}

// The dstoragecore.dll this process is using: the loaded module when present,
// otherwise the first search directory that holds the file. Never loads a
// module, so the capability report stays side-effect free.
bool resolve_runtime_core_path(std::wstring &r_path) {
	HMODULE core = GetModuleHandleW(L"dstoragecore.dll");
	if (core != nullptr) {
		wchar_t module_path[MAX_PATH * 4];
		const DWORD capacity = static_cast<DWORD>(sizeof(module_path) / sizeof(module_path[0]));
		const DWORD length = GetModuleFileNameW(core, module_path, capacity);
		if (length > 0 && length < capacity) {
			r_path.assign(module_path, length);
			return true;
		}
	}
	PackedStringArray dirs = Win32Backend::get_runtime_search_dirs();
	for (int i = 0; i < dirs.size(); i++) {
		const String candidate = dirs[i] + "/dstoragecore.dll";
		if (FileAccess::file_exists(candidate)) {
			r_path.assign(reinterpret_cast<const wchar_t *>(candidate.utf16().get_data()));
			return true;
		}
	}
	return false;
}

} // namespace

Dictionary Win32Backend::get_runtime_info() {
	Dictionary info;
	info["version"] = "Unavailable";
	info["version_major"] = static_cast<int64_t>(0);
	info["version_minor"] = static_cast<int64_t>(0);
	info["version_build"] = static_cast<int64_t>(0);
	info["version_revision"] = static_cast<int64_t>(0);
	info["loaded"] = false;

	uint16_t major = 0;
	uint16_t minor = 0;
	uint16_t build = 0;
	uint16_t revision = 0;
	std::wstring core_path;
	bool has_version = false;
	if (resolve_runtime_core_path(core_path)) {
		has_version = read_dll_file_version(core_path, major, minor, build, revision);
	}
	if (has_version) {
		info["version"] = String::num_int64(major) + "." + String::num_int64(minor) + "." + String::num_int64(build) + "." + String::num_int64(revision);
		info["version_major"] = static_cast<int64_t>(major);
		info["version_minor"] = static_cast<int64_t>(minor);
		info["version_build"] = static_cast<int64_t>(build);
		info["version_revision"] = static_cast<int64_t>(revision);
	}

	HMODULE core = GetModuleHandleW(L"dstoragecore.dll");
	HMODULE dstorage = GetModuleHandleW(L"dstorage.dll");
	info["loaded"] = core != nullptr && dstorage != nullptr;

	// 1.3 introduced IDStorageQueue3 (EnqueueRequests); 1.4 introduced Zstd and
	// GACL. The version compare is the fallback; the live probes below are
	// authoritative when the shim is actually loaded.
	const bool at_least_1_3 = has_version && (major > 1 || (major == 1 && minor >= 3));
	const bool at_least_1_4 = has_version && (major > 1 || (major == 1 && minor >= 4));

	bool configuration2 = false;
	if (dstorage != nullptr) {
		configuration2 = GetProcAddress(dstorage, "DStorageSetConfiguration2") != nullptr;
	}

	bool zstd = at_least_1_4;
	bool zstd_probed = false;
	if (dstorage != nullptr && (at_least_1_4 || configuration2)) {
		using CreateCompressionCodecFunction = HRESULT(WINAPI *)(DSTORAGE_COMPRESSION_FORMAT, UINT32, REFIID, void **);
		CreateCompressionCodecFunction create_codec = reinterpret_cast<CreateCompressionCodecFunction>(GetProcAddress(dstorage, "DStorageCreateCompressionCodec"));
		if (create_codec != nullptr) {
			IDStorageCompressionCodec *codec = nullptr;
			// 2 is DSTORAGE_COMPRESSION_FORMAT_ZSTD in the 1.4 header. The
			// literal keeps this translation unit compiling against 1.3.
			const HRESULT hr = create_codec(static_cast<DSTORAGE_COMPRESSION_FORMAT>(2), 0, __uuidof(IDStorageCompressionCodec), reinterpret_cast<void **>(&codec));
			zstd = SUCCEEDED(hr) && codec != nullptr;
			zstd_probed = true;
			if (codec != nullptr) {
				codec->Release();
			}
		}
	}

	Dictionary capabilities;
	capabilities["zstd"] = zstd;
	capabilities["zstd_probed"] = zstd_probed;
	capabilities["gacl"] = zstd && at_least_1_4;
	capabilities["enqueue_requests"] = at_least_1_3;
	capabilities["configuration2"] = configuration2;
	capabilities["creator_id"] = configuration2;
	info["capabilities"] = capabilities;
	return info;
}

bool Win32Backend::probe_system_support() {
	Dictionary drive_info = get_drive_info_for_path("res://");
	// The runtime configuration is one-shot and the probe may be the first
	// caller, so record the media-derived I/O mode before DStorageGetFactory.
	// Never overwrite a mode initialize() already applied: a capability query
	// after startup would otherwise re-evaluate res:// and contradict the active
	// configuration.
	{
		std::lock_guard<std::mutex> config_lock(s_dstorage_config_mutex);
		// Never overwrite a mode initialize() already applied; a marker that no longer
		// names the loaded instance is not a live configuration.
		if (!has_live_dstorage_configuration_locked()) {
			s_desired_file_buffering.store(static_cast<bool>(drive_info.get("rotational", false)));
			s_bypassio_unavailable.store(!static_cast<bool>(drive_info.get("bypassio_supported", true)));
		}
	}
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::print("[DirectStorage] Backend unavailable: " + String(drive_info["reason"]));
		return false;
	}

	DSRuntimeProbe probe;
	PackedStringArray search_dirs = get_runtime_search_dirs();
	for (int i = 0; i < search_dirs.size(); i++) {
		probe = probe_runtime(search_dirs[i], 2000);
		if (probe.available) {
			break;
		}
	}
	if (!probe.available) {
		UtilityFunctions::print("[DirectStorage] Backend unavailable: " + probe.reason);
	}
	return probe.available;
}
#else
void Win32Backend::apply_dstorage_configuration() {}
bool Win32Backend::probe_system_support() { return false; }
PackedStringArray Win32Backend::get_runtime_search_dirs() { return PackedStringArray(); }
Dictionary Win32Backend::get_runtime_info() { return Dictionary(); }
#endif

bool Win32Backend::is_available() const {
	return probe_system_support();
}

String Win32Backend::get_name() const {
	return "Win32DirectStorage";
}

bool Win32Backend::initialize() {
	return initialize("");
}

#ifdef _WIN32
int Win32Backend::priority_to_queue_index(DSRequest::DSPriority p_priority) {
	int priority = static_cast<int>(p_priority);
	return priority >= DSRequest::PRIORITY_LOW && priority <= DSRequest::PRIORITY_HIGH ? priority : DSRequest::PRIORITY_NORMAL;
}
#endif

bool Win32Backend::is_path_valid(const String &p_path) {
#ifdef _WIN32
	if (p_path.strip_edges().is_empty()) {
		return false;
	}

	String global_path = p_path;
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (p_path.begins_with("res://") || p_path.begins_with("user://"))) {
		global_path = ps->globalize_path(p_path);
	}

	String norm_path = global_path.replace("/", "\\");
	if (norm_path.length() < 3 || (norm_path[1] != ':' && !norm_path.begins_with("\\\\"))) {
		return false;
	}

	std::wstring w_path(reinterpret_cast<const wchar_t *>(norm_path.utf16().get_data()));
	DWORD attributes = GetFileAttributesW(w_path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		return true;
	}

	// If specific file does not exist yet, check its parent directory
	String parent_dir = norm_path.get_base_dir();
	if (!parent_dir.is_empty() && parent_dir != norm_path) {
		std::wstring w_parent(reinterpret_cast<const wchar_t *>(parent_dir.utf16().get_data()));
		DWORD parent_attribs = GetFileAttributesW(w_parent.c_str());
		if (parent_attribs != INVALID_FILE_ATTRIBUTES && (parent_attribs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
			return true;
		}
	}

	// Check if the drive root volume itself is valid
	wchar_t volume_path[MAX_PATH] = {0};
	if (GetVolumePathNameW(w_path.c_str(), volume_path, MAX_PATH)) {
		DWORD vol_attribs = GetFileAttributesW(volume_path);
		if (vol_attribs != INVALID_FILE_ATTRIBUTES) {
			return true;
		}
	}

	return false;
#else
	return false;
#endif
}

#ifdef _WIN32
// Drops every process-wide D3D12 reference while the engine is alive and the
// loader lock is NOT held. Diagnostics that led here: the hung process's main
// thread sat in ntdll!LdrUnloadDll -> this module -> D3D12 -> nvwgf2umx ->
// WaitForSingleObject, i.e. ComPtr destructors ran during DLL_PROCESS_DETACH and
// their Release() destroyed D3D12 objects under the loader lock.
//
// Safe without a backend lock: shutdown_ds() has already stopped and joined every
// backend before module deinit calls this, so no completion thread or render-thread
// handoff can be writing these containers any more.
void ds_win32_release_static_d3d12_state() {
	ds_win32_invalidate_d3d12_device();
	s_gpu_dest_query_queue.clear();
}
#endif

bool Win32Backend::initialize(const String &p_path) {
#ifdef _WIN32
	// Renderer gate first: without a live D3D12 RenderingDevice everything
	// below (volume IOCTLs, LoadLibrary of the DirectStorage runtime,
	// DStorageGetFactory) can only fail. A non-D3D12 driver is cached
	// process-wide, so repeated initialize() attempts return immediately.
	String renderer_reason;
	if (!ds_renderer_supports_dstorage(renderer_reason)) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: " + renderer_reason);
		return false;
	}

	active_batch_slot.store(0);
	sync_batch_in_flight.store(false);
	// A fresh initialize() re-arms the one-shot teardown: the object is being
	// brought back up, so the next shutdown() must do the work again.
	shut_down_once.store(false, std::memory_order_release);
	for (QueueState &queue : priority_queues) {
		queue.completion_fence_value = 0;
		queue.completed_fence_value = 0;
		for (BatchSlot &slot : queue.batch_slots) {
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.closing = false;
			slot.submitted = false;
			slot.completion_ready = false;
		}
	}
	DSContext *ctx = DSContext::get_singleton();
	if (!ctx) {
		return false;
	}

	String validation_path = p_path.is_empty() ? "res://" : p_path;
	if (!p_path.is_empty() && !is_path_valid(p_path)) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Path '" + p_path + "' does not exist or is not a valid path.");
		return false;
	}

	Dictionary drive_info = get_drive_info_for_path(validation_path);
	if (!drive_info.get("directstorage_supported", false)) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Drive " + String(drive_info["drive"]) + " is unsupported (" + String(drive_info["reason"]) + ").");
		return false;
	}
	if (!static_cast<bool>(drive_info.get("rotational", false)) &&
			String(drive_info.get("bypassio_state", "")) == "Disabled") {
		const String failing_driver = String(drive_info.get("bypassio_failing_driver", ""));
		const String failure_reason = String(drive_info.get("bypassio_failure_reason", ""));
		String detail;
		if (!failing_driver.is_empty() && !failure_reason.is_empty()) {
			detail = " (" + failing_driver + ": " + failure_reason + ")";
		} else if (!failing_driver.is_empty()) {
			detail = " (" + failing_driver + ")";
		} else if (!failure_reason.is_empty()) {
			detail = " (" + failure_reason + ")";
		}
		UtilityFunctions::print("[DirectStorage] BypassIO disabled on " + String(drive_info.get("drive", "")) + detail + "; using standard unbuffered I/O.");
	}
	// Computed before the runtime is configured (DStorageSetConfiguration is
	// one-shot and must precede DStorageGetFactory). The probe path does the
	// same from res://; this covers a direct initialize_ds_for_path() call.
	s_desired_file_buffering.store(static_cast<bool>(drive_info.get("rotational", false)));
	s_bypassio_unavailable.store(!static_cast<bool>(drive_info.get("bypassio_supported", true)));

	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (!ps) {
		UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: ProjectSettings is not initialized.");
		return false;
	}
	{
		// Try each candidate directory in order (addon bin for editor/dev
		// runs, executable directory for exported games with flat DLLs).
		// First claims modules stashed by is_available()'s probe, if the
		// probe succeeded for that same directory.
		PackedStringArray search_dirs = get_runtime_search_dirs();
		bool modules_acquired = false;
		String last_error_dir;
		for (int i = 0; i < search_dirs.size(); i++) {
			String bin_dir = search_dirs[i];
			HMODULE claimed_core = nullptr;
			HMODULE claimed_dstorage = nullptr;
			if (claim_probed_runtime_modules(bin_dir, claimed_core, claimed_dstorage)) {
				// Reuse the modules is_available() already loaded and verified;
				// this is the single LoadLibrary pair for the process.
				dstoragecore_module = claimed_core;
				dstorage_module = claimed_dstorage;
				modules_acquired = true;
				break;
			}

			HMODULE core = load_runtime_module(bin_dir, "dstoragecore.dll");
			if (!core) {
				last_error_dir = bin_dir;
				continue;
			}
			HMODULE dstorage = load_runtime_module(bin_dir, "dstorage.dll");
			if (!dstorage) {
				unload_runtime_modules(core, nullptr);
				last_error_dir = bin_dir;
				continue;
			}
			dstoragecore_module = core;
			dstorage_module = dstorage;
			modules_acquired = true;
			break;
		}
		if (!modules_acquired) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: DirectStorage runtime DLLs ('dstoragecore.dll', 'dstorage.dll') could not be loaded from any search directory (last tried: '" + last_error_dir + "').");
			dstoragecore_module = nullptr;
			dstorage_module = nullptr;
			return false;
		}
	}

	uint32_t staging_mb = ctx->get_staging_buffer_size_mb();
	bool disable_gpu = ctx->get_disable_gpu_decompression();
	uint16_t capacity = ctx->get_queue_capacity();

	apply_dstorage_configuration();

	String step_name = "DStorageGetFactory";
	HRESULT hr = DStorageGetFactory(IID_PPV_ARGS(&factory));

	if (SUCCEEDED(hr) && factory) {
#ifdef DEBUG_ENABLED
		UINT32 debug_flags = DSTORAGE_DEBUG_SHOW_ERRORS | DSTORAGE_DEBUG_RECORD_OBJECT_NAMES;
		// Opt-in only: DebugBreak() without a debugger attached terminates the
		// process, so the setting must never be on by default.
		if (ProjectSettings *ps = ProjectSettings::get_singleton()) {
			const String break_key = dsgd_settings::key("debug_break_on_error");
			if (ps->has_setting(break_key) && static_cast<bool>(ps->get_setting(break_key))) {
				debug_flags |= static_cast<UINT32>(DSTORAGE_DEBUG_BREAK_ON_ERROR);
			}
		}
		factory->SetDebugFlags(debug_flags);
#else
		factory->SetDebugFlags(DSTORAGE_DEBUG_NONE);
#endif
		uint64_t staging_bytes = static_cast<uint64_t>(staging_mb) * 1024ULL * 1024ULL;
		if (staging_bytes > UINT32_MAX) {
			UtilityFunctions::push_warning("[DirectStorage] Configured staging buffer size (" + String::num_uint64(staging_mb) + " MiB) exceeds DirectStorage 4 GiB maximum; clamping to 4095 MiB.");
			staging_bytes = UINT32_MAX;
		}
		factory->SetStagingBufferSize(static_cast<UINT32>(staging_bytes));
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		s_last_applied_staging_bytes.store(static_cast<int64_t>(staging_bytes));
#endif

		DSTORAGE_QUEUE_DESC queueDesc = {};
		queueDesc.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
		queueDesc.Capacity = capacity;
		Microsoft::WRL::ComPtr<ID3D12Device> d3d12_device = ds_win32_get_d3d12_device();
		if (!d3d12_device) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not acquire a usable D3D12 device.");
			return false;
		}
		queueDesc.Device = d3d12_device.Get();

		step_name = "CreateQueue";
		for (int i = 0; i < QUEUE_COUNT && SUCCEEDED(hr); i++) {
			QueueState &queue = priority_queues[i];
			queueDesc.Priority = i == DSRequest::PRIORITY_LOW ? DSTORAGE_PRIORITY_LOW :
					i == DSRequest::PRIORITY_HIGH ? DSTORAGE_PRIORITY_HIGH : DSTORAGE_PRIORITY_NORMAL;
			queueDesc.Name = i == DSRequest::PRIORITY_LOW ? "DirectStorageLowPriorityQueue" :
					i == DSRequest::PRIORITY_HIGH ? "DirectStorageHighPriorityQueue" : "DirectStorageNormalPriorityQueue";
			if (FAILED(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&queue.completion_fence))) || !queue.completion_fence) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the D3D12 completion fence.");
				hr = E_FAIL;
				break;
			}
			queue.completion_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			for (BatchSlot &slot : queue.batch_slots) {
				slot.event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
			}
			if (!queue.completion_event || !queue.batch_slots[0].event || !queue.batch_slots[1].event) {
				UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create a queue completion event.");
				hr = E_FAIL;
				break;
			}
			hr = factory->CreateQueue(&queueDesc, IID_PPV_ARGS(&queue.queue));
			if (SUCCEEDED(hr) && queue.queue) {
				// Queue-owned: waited on by the completion thread, never CloseHandle()d.
				// RetrieveErrorRecord is only legal once this has signalled, so a null
				// handle would leave failures unobservable - refuse to run that way.
				queue.error_event = queue.queue->GetErrorEvent();
				if (queue.error_event == nullptr) {
					UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: IDStorageQueue::GetErrorEvent() returned no handle for '" + String(queueDesc.Name) + "'.");
					hr = E_FAIL;
				}
			}
		}
	}

	bool queues_ready = SUCCEEDED(hr);
	for (const QueueState &queue : priority_queues) {
		queues_ready = queues_ready && queue.queue != nullptr && queue.completion_event != nullptr && queue.batch_slots[0].event != nullptr && queue.batch_slots[1].event != nullptr && queue.completion_fence != nullptr && queue.error_event != nullptr;
	}
	if (queues_ready) {
		// No eager archive pre-open: a handle cached at init would keep
		// res://textures.dspkg locked with zero references until the first
		// batch that reads it completes, which is exactly the replacement
		// failure issue #0182 fixed. The first request opens the archive
		// lazily; open handles are cached per batch only.
		completion_stop_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
		if (!completion_stop_event) {
			UtilityFunctions::print("[DirectStorage] Win32Backend initialization failed: Could not create the completion thread stop event.");
			clear_persistent_files();
			return false;
		}
			accepting_requests.store(true);
			completion_thread_stop.store(false);
			batch_in_flight.store(false);
			{
				std::lock_guard<std::mutex> lock(batch_mutex);
				completion_queue.clear();
			}
		completion_thread = std::thread(&Win32Backend::completion_thread_func, this);

		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			for (const QueueState &queue : priority_queues) {
				err_handler->register_queue(queue.queue);
			}
		}
		RenderingServer *rs = safe_get_rendering_server();
		String driver_name = rs ? rs->get_current_rendering_driver_name().to_lower() : "unknown";
		int gpu_tier = ctx ? ctx->get_gpu_decompression_support_tier() : 0;
		// Human-readable GPU path: tier 2 = GPU-optimized GDeflate shaders,
		// tier 1 = compute-shader fallback (works, slower, contends with
		// rendering on weak iGPUs), tier 0 = no GPU support (CPU decode).
		String gpu_mode = "unavailable (no GPU support)";
		if (disable_gpu) {
			gpu_mode = "disabled (project setting)";
		} else if (gpu_tier >= 2) {
			gpu_mode = "enabled (GPU-optimized)";
		} else if (gpu_tier == 1) {
			gpu_mode = "enabled (GPU-fallback)";
		}
		String adapter_suffix;
		if (ctx) {
			Dictionary adapter = ctx->get_gpu_adapter_info();
			if (bool(adapter.get("available", false))) {
				String uma_desc;
				if (bool(adapter.get("is_uma", false))) {
					uma_desc = ", UMA" + String(bool(adapter.get("is_cache_coherent_uma", false)) ? " coherent" : " non-coherent");
				} else if (bool(adapter.get("integrated_gpu_guess", false))) {
					uma_desc = ", UMA guess";
				}
				adapter_suffix = ", adapter='" + String(adapter.get("adapter_name", "unknown")) + "' (" +
						String(adapter.get("dedicated_vram_mb", 0)) + " MiB dedicated" +
						uma_desc +
						(bool(adapter.get("software_adapter", false)) ? ", software" : "") + ")";
			}
		}
		UtilityFunctions::print("[DirectStorage] Win32DirectStorage ready: driver=" + driver_name +
				", staging_buffer=" + String::num_uint64(staging_mb) + " MiB, queue_capacity=" +
				String::num_uint64(capacity) + ", gpu_decompression=" + gpu_mode + adapter_suffix);
		return true;
	} else {
		uint32_t u_hr = static_cast<uint32_t>(hr);
		String hr_hex = String::num_uint64(u_hr, 16);
		UtilityFunctions::print("[DirectStorage] Win32Backend DirectStorage " + step_name + " failed (HRESULT: 0x" + hr_hex + ").");
		return false;
	}
#else
	return false;
#endif
}

#ifdef _WIN32
// Samples the fences and applies the result under batch_mutex. Callers invoke this only after the
// drain has decided whether it needs to block: a value read before the wait is what the hardware
// held when the drain started blocking, not what it held when the wait was satisfied.
void Win32Backend::record_drain_watermarks(ID3D12Fence *const *p_fences) {
	uint64_t completed_now[QUEUE_COUNT] = {};
	for (int i = 0; i < QUEUE_COUNT; i++) {
		completed_now[i] = p_fences[i] ? p_fences[i]->GetCompletedValue() : 0;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	for (int i = 0; i < QUEUE_COUNT; i++) {
		// max, not assignment: the completion thread may have recorded a newer value
		// while this drain was waiting, and the counter must never move backwards.
		// UINT64_MAX is "device removed", not a completed value; wait_for_completion()
		// reports it as a failure, so it must never become a watermark.
		if (completed_now[i] == UINT64_MAX) {
			continue;
		}
		priority_queues[i].completed_fence_value = std::max(priority_queues[i].completed_fence_value, completed_now[i]);
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		debug_last_drain_completion[i] = priority_queues[i].completion_fence_value;
		debug_last_drain_completed[i] = priority_queues[i].completed_fence_value;
#endif
	}
}

bool Win32Backend::drain_all_queues(uint32_t p_timeout_ms) {
	last_drain_arm_failed = false;
	// Snapshot the per-queue wait targets under batch_mutex. Every QueueState
	// counter belongs to that mutex: submit_async()/submit_batch() advance
	// completion_fence_value under it, and the completion thread records
	// completed_fence_value under it (complete_batch_slot()). Reading or writing
	// either counter here without the lock raced those writers.
	struct DrainTarget {
		int queue_index = 0;
		ID3D12Fence *fence = nullptr;
		uint64_t target_value = 0;
		HANDLE event = nullptr;
	};
	ID3D12Fence *fences[QUEUE_COUNT] = {};
	DrainTarget targets[QUEUE_COUNT];
	int target_count = 0;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		// Test-only seam: runs inside the counter snapshot, i.e. at the access that
		// must be serialised with every other reader and writer of these counters.
		// A test parks in the hook and asks another thread whether batch_mutex is
		// held here.
		if (drain_counter_access_hook) {
			drain_counter_access_hook();
		}
#endif
		for (int i = 0; i < QUEUE_COUNT; i++) {
			const QueueState &queue = priority_queues[i];
			fences[i] = queue.completion_fence;
			if (queue.completion_fence && queue.completion_fence_value != 0 && queue.completion_event) {
				targets[target_count++] = { i, queue.completion_fence, queue.completion_fence_value, queue.completion_event };
			}
		}
	}

	// The fence queries, SetEventOnCompletion() and the wait stay outside
	// batch_mutex: the wait can park for the whole drain budget, and holding the
	// lock across it would stall every enqueue, submit and batch completion.
	HANDLE wait_events[QUEUE_COUNT];
	int event_count = 0;
	bool arm_failed = false;
	for (int i = 0; i < target_count; i++) {
		uint64_t completed = targets[i].fence->GetCompletedValue();
		if (completed == UINT64_MAX || completed >= targets[i].target_value) {
			continue;
		}
		HRESULT arm_result = S_OK;
#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
		if (drain_arm_hook) {
			arm_result = drain_arm_hook(targets[i].queue_index, targets[i].fence, targets[i].target_value, targets[i].event);
		} else
#endif
		{
			arm_result = targets[i].fence->SetEventOnCompletion(targets[i].target_value, targets[i].event);
		}
		if (FAILED(arm_result)) {
			// A target that could not be armed can never be proven complete. It must
			// not be dropped from the wait set silently: the drain's answer is "every
			// unsatisfied target was waited on", and a subset cannot establish that.
			arm_failed = true;
			UtilityFunctions::push_error("[DirectStorage] Shutdown drain could not arm a completion event on priority queue " +
					String::num_int64(targets[i].queue_index) + " (HRESULT 0x" +
					String::num_uint64(static_cast<uint32_t>(arm_result), 16) +
					"); that queue's target cannot be proven complete.");
			continue;
		}
		wait_events[event_count++] = targets[i].event;
	}

	if (arm_failed) {
		// Fail before either success exit. Waiting on the armed subset cannot prove
		// the failed target, and the only caller takes the deliberate-leak path for a
		// false answer either way, so spending the drain budget buys nothing.
		last_drain_arm_failed = true;
		return false;
	}

	if (event_count == 0) {
		// Nothing to block on: every target was already satisfied when the waits were
		// registered. Those fences were still verified, so the watermarks this drain
		// reports have to be recorded here too - this exit used to return without
		// touching the counters, leaving them behind a fence the drain had just
		// observed as complete.
		record_drain_watermarks(fences);
		return true;
	}

	DWORD wait_result = WaitForMultipleObjects(event_count, wait_events, TRUE, p_timeout_ms);
	if (wait_result >= WAIT_OBJECT_0 && wait_result < WAIT_OBJECT_0 + event_count) {
		// Record AFTER the wait satisfied every target. The snapshot this used to
		// read was taken before the block, i.e. it is what the hardware held when the
		// drain started waiting, not what it held when the wait returned, so it can
		// sit below the target the wait has just proved. wait_for_completion()
		// re-samples for the same reason (:1303).
		record_drain_watermarks(fences);
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (size_t z = 0; z < zombie_slots.size(); z++) {
			for (int r = 0; r < zombie_slots[z].requests.size(); r++) {
				if (zombie_slots[z].requests[r].is_valid()) {
					zombie_slots[z].requests[r]->set_gpu_in_flight(false);
				}
			}
			complete_chunked_requests();
			release_resource_guards(zombie_slots[z].requests);
			for (std::unordered_map<std::wstring, IDStorageFile *>::iterator it = zombie_slots[z].file_cache.begin(); it != zombie_slots[z].file_cache.end(); ++it) {
				release_source_file(it->first, it->second);
			}
			zombie_slots[z].file_cache.clear();
			zombie_slots[z].files.clear();
		}
		zombie_slots.clear();
		return true;
	}

	return false;
}
#endif

bool Win32Backend::was_drain_clean() const {
	return last_shutdown_drained.load();
}

void Win32Backend::shutdown() {
#ifdef _WIN32
	// Idempotent: the first call has either completed the full teardown or taken
	// the drain-timeout branch, which leaves the object deliberately incomplete.
	// Re-entering would repeat a full-budget drain on the main thread (~Win32Backend
	// does exactly that) or tear down handles a detached completion thread may still
	// be waiting on.
	if (shut_down_once.exchange(true, std::memory_order_acq_rel)) {
		return;
	}

	// 1. Immediately refuse any new incoming work.
	accepting_requests.store(false);

	// 2. Drain all GPU DMA work across all priority queues concurrently BEFORE
	// stopping the completion thread or touching any file handles or resource guards.
	uint32_t timeout_ms = 10000;
	if (dsgd_settings::has_setting("shutdown_drain_timeout_ms", "shutdown_drain_timeout_ms")) {
		timeout_ms = static_cast<uint32_t>(MAX(100, int64_t(dsgd_settings::get_setting("shutdown_drain_timeout_ms", "shutdown_drain_timeout_ms"))));
	}
	bool drained = drain_all_queues(timeout_ms);
	last_shutdown_drained.store(drained);

	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (err_handler) {
		err_handler->unregister_queue();
	}

	if (!drained) {
		// Two causes reach this branch: the wait timed out (GPU hardware hang, TDR,
		// driver deadlock) or a queue's completion event could not be armed, in which
		// case the drain failed immediately and never waited at all.
		// Releasing guards, closing in-flight files, or unloading DirectStorage DLLs
		// while hardware or driver DMA is active causes an immediate unhandled Access Violation.
		// Instead: signal stop, detach completion_thread to avoid std::terminate(), leak handles,
		// and skip unloading runtime modules. The OS kernel process teardown reclaims all resources cleanly.
		const String cause = last_drain_arm_failed
				? String("could not arm a queue completion event (device removed or out of resources)")
				: String("timed out after ") + String::num_uint64(timeout_ms) + " ms (GPU/DMA hung)";
		UtilityFunctions::push_error("[DirectStorage] Shutdown drain " + cause +
				"; leaking handles and skipping DLL unload to avoid use-after-free.");
		{
			// Publish the stop flag under batch_mutex: the completion thread
			// checks it in its wait predicate under the same mutex, so a store
			// outside the lock can be missed between predicate check and waiter
			// registration (lost wakeup, then join/detach never returns).
			std::lock_guard<std::mutex> lock(batch_mutex);
			completion_thread_stop.store(true);
			sync_batch_in_flight.store(false);
		}
		if (completion_stop_event) {
			SetEvent(completion_stop_event);
		}
		batch_cv.notify_all();
		if (completion_thread.joinable()) {
			completion_thread.detach();
		}
		return;
	}

	// 3. Normal graceful path: all DMA is drained. Signal and join the completion thread.
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_thread_stop.store(true);
		sync_batch_in_flight.store(false);
	}
	if (completion_stop_event) {
		SetEvent(completion_stop_event);
	}
	batch_cv.notify_all();
	if (completion_thread.joinable()) {
		completion_thread.join();
	}
	batch_in_flight.store(false);

	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		release_all_resource_guards();
	}

	for (QueueState &queue : priority_queues) {
		for (BatchSlot &slot : queue.batch_slots) {
			close_slot_files(slot);
			{
				std::lock_guard<std::mutex> lock(batch_mutex);
				slot.requests.clear();
			}
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = 0;
			slot.error_message = String();
			slot.submitted = false;
			slot.completion_ready = false;
			if (slot.event) {
				CloseHandle(slot.event);
				slot.event = nullptr;
			}
		}
		if (queue.queue) {
			queue.queue->Close();
			queue.queue->Release();
			queue.queue = nullptr;
		}
		// Queue-owned handle: dropping the reference above invalidates it, so it is
		// only forgotten here, never CloseHandle()d.
		queue.error_event = nullptr;
		queue.pending_errors.fill(QueueErrorInfo());
		if (queue.completion_event) {
			CloseHandle(queue.completion_event);
			queue.completion_event = nullptr;
		}
		if (queue.completion_fence) {
			queue.completion_fence->Release();
			queue.completion_fence = nullptr;
		}
		queue.completion_fence_value = 0;
		queue.completed_fence_value = 0;
	}

	// Completion thread is joined above, so no concurrent enqueue can run;
	// take the lock for consistency with the enqueue path.
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		clear_persistent_files();
	}

	if (factory) {
		factory->Release();
		factory = nullptr;
	}

	if (completion_stop_event) {
		CloseHandle(completion_stop_event);
		completion_stop_event = nullptr;
	}

	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		completion_queue.clear();
	}
	{
		ds_win32_invalidate_d3d12_device();
	}
	active_batch_slot.store(0);
	unload_runtime_modules(dstoragecore_module, dstorage_module);
	dstoragecore_module = nullptr;
	dstorage_module = nullptr;
	{
		// Release any probe-stashed modules initialize() never claimed
		// (probe succeeded but init never ran) so they cannot leak.
		std::lock_guard<std::mutex> lock(s_probe_mutex);
		unload_runtime_modules(s_probed_core_module, s_probed_dstorage_module);
		s_probed_core_module = nullptr;
		s_probed_dstorage_module = nullptr;
		s_probed_bin_dir.clear();
	}
#endif
}

bool Win32Backend::enqueue_request(Ref<DSRequest> p_request, PlatformBackend::EnqueueOutcome *r_outcome) {
#ifdef _WIN32
	// Success by default; every refusal overwrites it through reject().
	if (r_outcome != nullptr) {
		*r_outcome = EnqueueOutcome::STAGED;
	}
	// The one place a refusal is expressed: status, operator-facing message and
	// retry classification are set together, so the caller never has to read the
	// message to decide whether the request is retryable.
	auto reject = [r_outcome](const Ref<DSRequest> &p_target, const String &p_message,
			EnqueueOutcome p_outcome) -> bool {
		if (p_target.is_valid()) {
			p_target->set_status(DSRequest::STATUS_FAILED);
			p_target->set_error_message(p_message);
		}
		if (r_outcome != nullptr) {
			*r_outcome = p_outcome;
		}
		return false;
	};
	if (!accepting_requests.load() || p_request.is_null() || !factory) {
		if (p_request.is_valid()) {
			return reject(p_request,
					!accepting_requests.load() ? "Win32Backend is shutting down." : "Win32Backend not initialized.",
					EnqueueOutcome::TERMINAL);
		}
		if (r_outcome != nullptr) {
			*r_outcome = EnqueueOutcome::TERMINAL;
		}
		return false;
	}
	if (sync_batch_in_flight.load()) {
		return reject(p_request, "Win32Backend synchronous batch submission is in progress.", EnqueueOutcome::TRANSIENT);
	}
	if (!p_request->claim_submission()) {
		const String claim_error = "Win32Backend: request was already submitted or is no longer mutable.";
		if (p_request->get_error_message().is_empty()) {
			p_request->set_error_message(claim_error);
		}
		UtilityFunctions::push_error(claim_error);
		if (r_outcome != nullptr) {
			*r_outcome = EnqueueOutcome::TERMINAL;
		}
		return false;
	}
	int queue_index = priority_to_queue_index(p_request->get_priority());
	QueueState &queue = priority_queues[queue_index];
	if (!queue.queue) {
		return reject(p_request, "Win32Backend priority queue is not initialized.", EnqueueOutcome::TERMINAL);
	}

	// Query GPU destination resource info once on the render thread BEFORE taking batch_mutex.
	// This covers single requests and multi-subresource/chunk splits without
	// issuing redundant cross-thread round-trips for each mip/chunk, and prevents
	// holding batch_mutex across cross-thread render-thread waits.
	bool is_gpu_buffer = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool is_gpu_texture = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *cached_dest_resource = nullptr;
	D3D12_RESOURCE_DESC cached_dest_desc = {};
	uint64_t cached_dest_native_format = 0;
	bool cached_rd_available = false;
	bool cached_dest_resolved = false;
	if (is_gpu_buffer || is_gpu_texture) {
		RID dest_rid = p_request->get_destination_resource();
		if (dest_rid.is_valid()) {
			cached_dest_resolved = ds_win32_query_destination_resource_on_render_thread(
					is_gpu_buffer, dest_rid, cached_dest_resource, cached_dest_desc, cached_dest_native_format, cached_rd_available);
		}
	}

	uint32_t src_size = p_request->get_size();
	uint32_t uncompressed_size = p_request->get_uncompressed_size();
	if (p_request->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = src_size;
	}
	if (src_size == 0 || uncompressed_size == 0) {
		return reject(p_request, "Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").", EnqueueOutcome::TERMINAL);
	}
	if (static_cast<uint64_t>(uncompressed_size) > DSTEX_MAX_TOTAL_BYTES) {
		return reject(p_request, "Win32Backend: uncompressed size " + String::num_uint64(uncompressed_size) + " exceeds the 512 MiB limit.", EnqueueOutcome::TERMINAL);
	}

	// Hoisted above batch_mutex: the pre-lock destination pre-size below and the
	// staging-split decision later in this function both need the same values.
	// get_staging_buffer_size_mb() is an atomic load, so this is safe off-lock.
	DSContext *ctx = DSContext::get_singleton();
	const uint64_t staging_size = ctx ? static_cast<uint64_t>(ctx->get_staging_buffer_size_mb()) * 1024ULL * 1024ULL : 0;
	const uint64_t request_size = MAX(static_cast<uint64_t>(src_size), static_cast<uint64_t>(uncompressed_size));

	// A non-zero (top, bottom) pair is the caller opting into the sub-box
	// destination. An empty or inverted band is a caller bug, not "whole
	// subresource": classify it here so the decision does not depend on
	// resolving a D3D12 resource, and so the destination checks below never see
	// a pair they were written to exclude. Both bounds are subresource-relative
	// texel rows.
	const bool has_row_region = p_request->has_texture_row_region();
	if (has_row_region) {
		const uint32_t region_top = p_request->get_texture_region_top();
		const uint32_t region_bottom = p_request->get_texture_region_bottom();
		if (region_top >= region_bottom) {
			return reject(p_request, "Win32Backend: texture row range is empty or inverted (top=" +
					String::num_uint64(region_top) + ", bottom=" + String::num_uint64(region_bottom) +
					"); a non-zero row band must satisfy top < bottom.", EnqueueOutcome::TERMINAL);
		}
		if (p_request->get_destination_type() != DSRequest::DESTINATION_GPU_TEXTURE) {
			return reject(p_request, "Win32Backend: texture row range requires a DESTINATION_GPU_TEXTURE request (destination_type=" +
					String::num_int64(static_cast<int64_t>(p_request->get_destination_type())) + ").", EnqueueOutcome::TERMINAL);
		}
	}

	// GDeflate size contract (issue #0181). The declared uncompressed size is
	// what sizes a memory/buffer destination, but DirectStorage decompresses the
	// tiles the stream header itself declares and reports no error when the two
	// disagree - the write simply runs past the destination. Reject the request
	// before anything is sized from the declaration. Whole-subresource texture
	// uploads are covered by the dstex/dspkg footprint metadata instead; a
	// row-range chunk is not (its sizes come from the caller), so it is checked
	// here like any other declaration.
	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE &&
			(has_row_region || p_request->get_destination_type() == DSRequest::DESTINATION_MEMORY ||
					p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER) &&
			!p_request->is_gdeflate_stream_prevalidated(p_request->get_file_path(), p_request->get_offset(), uncompressed_size)) {
		String stream_error;
		if (!validate_gdeflate_stream_size(p_request->get_file_path(), p_request->get_offset(), uncompressed_size, stream_error)) {
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(stream_error);
			}
			return reject(p_request, stream_error, EnqueueOutcome::TERMINAL);
		}
	}

	Vector<DSTexMipEntryV2> texture_mip_entries;
	// Both codecs upload a GPU texture mip by mip from the container's padded
	// footprints, so the mip table is required for NONE payloads too.
	const bool gpu_texture_destination = p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	// A row-range request is already one chunk of a chunked payload: the caller
	// resolved its sizes from the validated chunk table, so the legacy
	// whole-file staging/rewrite logic below must not run for it.
	const bool is_row_region_request = gpu_texture_destination && has_row_region;
	DSCompressionFormat container_format = DSCompressionFormat::GDEFLATE;
	uint32_t texture_first_subresource = p_request->get_first_subresource();
	uint32_t texture_subresource_count = p_request->get_subresource_count();
	const int64_t single_mip_index = (texture_subresource_count == 1)
			? static_cast<int64_t>(texture_first_subresource)
			: -1;
	bool has_texture_mip_entries = gpu_texture_destination && !is_row_region_request &&
			get_texture_mip_entries(p_request->get_file_path(), p_request->get_offset(), texture_mip_entries, &container_format, single_mip_index);
	uint64_t texture_footprint = 0;
	if (has_texture_mip_entries) {
		if (texture_first_subresource >= static_cast<uint32_t>(texture_mip_entries.size()) ||
				texture_subresource_count == 0 ||
				texture_subresource_count > static_cast<uint32_t>(texture_mip_entries.size()) - texture_first_subresource) {
			has_texture_mip_entries = false;
		} else {
			for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
				const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
				if (mip_entry.compressed_size == 0 || mip_entry.uncompressed_size == 0 ||
						texture_footprint > UINT64_MAX - mip_entry.uncompressed_size) {
					has_texture_mip_entries = false;
					break;
				}
				texture_footprint += mip_entry.uncompressed_size;
			}
		}
	}

	if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE && !has_texture_mip_entries &&
			!is_row_region_request) {
		return reject(p_request, "Win32Backend: GPU texture destinations require v2 footprint-padded formats.", EnqueueOutcome::TERMINAL);
	}

	if (has_texture_mip_entries) {
		// Whole-subresource GPU texture requests must agree with the container's
		// stored codec and, when GDeflate is requested, each target mip must begin
		// with a valid GDeflate tile-stream header whose uncompressed size matches
		// the mip table (#0252 / #0257). Feeding raw NONE bytes to the GPU GDeflate
		// compute shader spins the dispatch past the 2 s TDR threshold.
		const DSCompressionFormat requested_format = (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)
				? DSCompressionFormat::GDEFLATE
				: DSCompressionFormat::NONE;
		if (requested_format != container_format) {
			String codec_error = "Win32Backend: GPU texture request declared compression format " +
					String::num_int64(static_cast<int64_t>(requested_format)) +
					" but container '" + p_request->get_file_path() + "' stores format " +
					String::num_int64(static_cast<int64_t>(container_format)) + ".";
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(codec_error);
			}
			return reject(p_request, codec_error, EnqueueOutcome::TERMINAL);
		}
		if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE) {
			// A whole-texture prewarm request validated all target mips off-thread
			// and carries a range stamp matching its exact request identity.
			const bool whole_range_prevalidated = p_request->is_gdeflate_streams_prevalidated(
					p_request->get_file_path(), p_request->get_offset(), texture_first_subresource, texture_subresource_count);
			for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
				if (whole_range_prevalidated) {
					continue;
				}
				const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
				// A single-stream record (row-range chunk or single-mip request)
				// validates exactly this mip; any other request falls back.
				if (p_request->is_gdeflate_stream_prevalidated(p_request->get_file_path(), mip_entry.offset, mip_entry.uncompressed_size)) {
					continue;
				}
				String stream_error;
				if (!validate_gdeflate_stream_size(p_request->get_file_path(), mip_entry.offset, mip_entry.uncompressed_size, stream_error)) {
					DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
					if (err_handler) {
						err_handler->report_error(stream_error);
					}
					return reject(p_request, stream_error, EnqueueOutcome::TERMINAL);
				}
			}
		}
	}

	String req_raw_path = p_request->get_file_path();
	ProjectSettings *ps = ProjectSettings::get_singleton();
	if (ps && (req_raw_path.begins_with("res://") || req_raw_path.begins_with("user://"))) {
		req_raw_path = ps->globalize_path(req_raw_path);
	}
	String norm_file_path = req_raw_path.replace("/", "\\");
	std::wstring wpath(reinterpret_cast<const wchar_t *>(norm_file_path.utf16().get_data()));

	String resolve_error;
	IDStorageFile *pre_opened_file = resolve_source_file(wpath, resolve_error);
	if (!pre_opened_file) {
		return reject(p_request, resolve_error, EnqueueOutcome::TERMINAL);
	}
	// The reference is dropped again if this request never reaches a slot
	// (validation failure, or a child failing mid-staging); a fully staged
	// request transfers it to the slot (adopt_source_file below).
	bool file_registered = false;
	SourceRefGuard source_ref_guard(this, wpath, pre_opened_file, file_registered);

	// Pre-size a plain memory destination BEFORE taking batch_mutex. In Godot
	// 4.7 PackedByteArray::resize() zero-fills the whole allocation (~220 us/MiB
	// on this host plus soft zero-page faults), so doing it under batch_mutex
	// would make a main-thread has_pending_results()/poll_batch_status() block
	// for the entire resize once flush_stream_async() is offloaded to a worker.
	// Exclude GPU destinations (their resources are render-thread-owned) and
	// staging-split payloads (their children own their own buffers); enqueue_one
	// keeps the idempotent child-side resize as a fallback.
	if (!is_gpu_buffer && !is_gpu_texture &&
			(staging_size == 0 || request_size <= staging_size ||
					p_request->get_compression_format() != DSRequest::COMPRESSION_GDEFLATE)) {
		p_request->ensure_destination_buffer_capacity(uncompressed_size);
	}

	std::lock_guard<std::mutex> lock(batch_mutex);
	if (sync_batch_in_flight.load()) {
		return reject(p_request, "Win32Backend synchronous batch submission is in progress.", EnqueueOutcome::TRANSIENT);
	}
	int current_slot = active_batch_slot.load();
	BatchSlot *slot_ptr = &queue.batch_slots[current_slot];
	if (!slot_ptr->submitted && !slot_ptr->closing && slot_ptr->completion_ready) {
		close_slot_files(*slot_ptr);
		clear_completed_slot_locked(*slot_ptr);
		slot_ptr->completion_ready = false;
	} else if (slot_ptr->submitted || slot_ptr->closing) {
		int alternate_slot = 1 - current_slot;
		BatchSlot &alt_slot = queue.batch_slots[alternate_slot];
		if (!alt_slot.submitted && !alt_slot.closing && alt_slot.completion_ready) {
			close_slot_files(alt_slot);
			clear_completed_slot_locked(alt_slot);
			alt_slot.completion_ready = false;
		}
		if (!alt_slot.submitted && !alt_slot.closing && !alt_slot.completion_ready) {
			active_batch_slot.store(alternate_slot);
			current_slot = alternate_slot;
			slot_ptr = &queue.batch_slots[current_slot];
		}
	}
	BatchSlot &slot = *slot_ptr;
	if (slot.submitted || slot.closing || slot.completion_ready) {
		return reject(p_request, "Win32Backend batch slot is not available; collect completed results before enqueueing more requests.", EnqueueOutcome::TRANSIENT);
	}

	// GPU synchronization contract - in-flight destination scan. A destination
	// ID3D12Resource (AddRef'd in resource_guards at enqueue time, so the pointer
	// is stable while DirectStorage may own it) is in flight while it appears under
	// guard in any slot with submitted && !completion_ready. That is exactly the
	// window where D3D12_RESOURCE_STATE_COMMON cannot be established by anyone but
	// the barrier owner (Godot RD) and where sampling/reuse is undefined, so a new
	// GPU enqueue against it is rejected loudly instead of proceeding silently.
	// Design notes:
	// - DirectStorage requires destination resources to be in D3D12_RESOURCE_STATE_COMMON
	//   during GPU decompression. The extension transitions destination textures to COMMON
	//   at submit time and restores them afterwards.
	// - No silent CPU fallback: re-targeting to a memory destination behind the
	//   caller's back would leave the GPU texture unwritten while the caller samples
	//   it as if DirectStorage had filled it. The request fails with an error that
	//   names the CPU-memory alternative instead.
	// - Duplicates inside the slot currently being filled are exempt by construction
	//   (the filling slot is never submitted - see the guard above): multi-mip
	//   GDeflate splits enqueue several children against one texture into a single
	//   slot that submits/completes atomically. Same-submit aliases in one batch rely
	//   on non-overlapping ranges plus fence gating, unchanged.
	// - Slots with completion_ready (results collected or awaiting collection) are
	//   exempt: DirectStorage has finished writing, so the resource is Godot-owned
	//   again; batch_completed / wait_for_completion remains the sanctioned gate for
	//   sampling. Must be called with batch_mutex held (enqueue_request holds it
	//   here; every site flipping submitted/completion_ready holds it too).
	// - A slot that stopped being submitted but has not completed is NOT exempt:
	//   both the closing window (closing=true, submitted=false, spanning
	//   EnqueueRequests() outside batch_mutex) and
	//   zombie_slots (a synchronous flush that timed out, DMA still running) end in
	//   DMA against the resource. Skipping them would admit a second, barrier-less
	//   write into a destination DirectStorage has not finished with. Only the slot
	//   being filled right now stays exempt, because multi-mip splitting
	//   legitimately puts several children of one texture in it (bullet above).
	auto gpu_destination_in_flight = [&](ID3D12Resource *p_resource) -> bool {
		if (!p_resource) {
			return false;
		}
		for (int check_queue = 0; check_queue < QUEUE_COUNT; check_queue++) {
			const QueueState &check_state = priority_queues[check_queue];
			for (int check_slot = 0; check_slot < BATCH_SLOT_COUNT; check_slot++) {
				const BatchSlot &check = check_state.batch_slots[check_slot];
				if (&check == &slot || check.completion_ready) {
					continue;
				}
				for (int check_request = 0; check_request < check.requests.size(); check_request++) {
					if (check.requests[check_request].is_null()) {
						continue;
					}
					std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard_it = resource_guards.find(check.requests[check_request].ptr());
					if (guard_it != resource_guards.end() && guard_it->second == p_resource) {
						return true;
					}
				}
			}
		}
		// Zombie batches keep their request guards (released only when the fence
		// retires in poll_batch_status), so the same guard lookup applies.
		for (size_t check_zombie = 0; check_zombie < zombie_slots.size(); check_zombie++) {
			const ZombieBatchSlot &zombie = zombie_slots[check_zombie];
			for (int check_request = 0; check_request < zombie.requests.size(); check_request++) {
				if (zombie.requests[check_request].is_null()) {
					continue;
				}
				std::unordered_map<DSRequest *, ID3D12Resource *>::const_iterator guard_it = resource_guards.find(zombie.requests[check_request].ptr());
				if (guard_it != resource_guards.end() && guard_it->second == p_resource) {
					return true;
				}
			}
		}
		return false;
	};

	auto enqueue_one = [&](const Ref<DSRequest> &p_child, uint64_t p_destination_offset) -> EnqueueOutcome {
		IDStorageFile *file = pre_opened_file;
		if (!file) {
			reject(p_child, "Win32Backend: Source file handle unavailable for '" + p_child->get_file_path() + "'.", EnqueueOutcome::TERMINAL);
			return EnqueueOutcome::TERMINAL;
		}

	uint32_t src_size = p_child->get_size();
	uint32_t uncompressed_size = p_child->get_uncompressed_size();
	if (p_child->get_compression_format() == DSRequest::COMPRESSION_NONE && uncompressed_size == 0) {
		uncompressed_size = src_size;
	}

	if (src_size == 0 || uncompressed_size == 0) {
		// Slot-owned handle: close_slot_files() releases it at batch
		// completion; never closed on per-request error paths.
		reject(p_child, "Win32Backend: Invalid size parameters (src_size=" + String::num_uint64(src_size) + ", uncompressed_size=" + String::num_uint64(uncompressed_size) + ").", EnqueueOutcome::TERMINAL);
		return EnqueueOutcome::TERMINAL;
	}

	bool gpu_buffer_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER;
	bool gpu_texture_destination = p_child->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE;
	ID3D12Resource *destination_buffer = nullptr;
	ID3D12Resource *destination_texture = nullptr;

	// Resolve destination resource info via render-thread marshaling.
	ID3D12Resource *dest_res = cached_dest_resource;
	D3D12_RESOURCE_DESC dest_desc = cached_dest_desc;
	uint64_t dest_native_format = cached_dest_native_format;
	bool dest_rd_avail = cached_rd_available;
	bool dest_res_ok = cached_dest_resolved;
	if ((gpu_buffer_destination || gpu_texture_destination) &&
			p_child->get_destination_resource() != p_request->get_destination_resource()) {
		dest_res_ok = ds_win32_query_destination_resource_on_render_thread(
				gpu_buffer_destination, p_child->get_destination_resource(), dest_res, dest_desc, dest_native_format, dest_rd_avail);
	}

	if (gpu_buffer_destination) {
		String destination_error;
		EnqueueOutcome destination_outcome = EnqueueOutcome::TERMINAL;
		RenderingServer *rs = safe_get_rendering_server();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU buffer destination requires the D3D12 rendering driver.";
		} else if (!dest_rd_avail) {
			destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU buffer destination.";
		} else if (!p_child->get_destination_resource().is_valid()) {
			destination_error = "Win32Backend: GPU buffer destination resource RID is invalid.";
		} else if (!dest_res_ok || !dest_res) {
			destination_error = "Win32Backend: GPU buffer RID could not be mapped to an ID3D12Resource.";
		} else {
			destination_buffer = dest_res;
			const D3D12_RESOURCE_DESC &buffer_desc = dest_desc;
			if (buffer_desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
					p_destination_offset > buffer_desc.Width ||
					static_cast<uint64_t>(uncompressed_size) > (buffer_desc.Width - p_destination_offset)) {
				destination_error = "Win32Backend: GPU buffer destination size exceeds the mapped D3D12 buffer resource.";
			} else if (gpu_destination_in_flight(destination_buffer)) {
				// COMMON cannot be established while DirectStorage owns this
				// buffer - reject loudly, do not fall back silently (see above).
				destination_error = "Win32Backend: GPU buffer destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
				destination_outcome = EnqueueOutcome::TRANSIENT;
			}
		}

		if (!destination_error.is_empty()) {
			// Slot-owned handle: close_slot_files() releases it at batch
			// completion; never closed on per-request error paths.
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(destination_error);
			}
			reject(p_child, destination_error, destination_outcome);
			return destination_outcome;
		}
	}
	if (gpu_texture_destination) {
		String destination_error;
		EnqueueOutcome destination_outcome = EnqueueOutcome::TERMINAL;
		RenderingServer *rs = safe_get_rendering_server();
		if (!rs || rs->get_current_rendering_driver_name().to_lower() != "d3d12") {
			destination_error = "Win32Backend: GPU texture destination requires the D3D12 rendering driver.";
		} else if (!dest_rd_avail) {
			destination_error = "Win32Backend: No D3D12 RenderingDevice is active for the GPU texture destination.";
		} else if (!p_child->get_destination_resource().is_valid()) {
			destination_error = "Win32Backend: GPU texture destination resource RID is invalid.";
		} else if (!dest_res_ok || !dest_res) {
			destination_error = "Win32Backend: GPU texture RID could not be mapped to an ID3D12Resource.";
		} else {
			destination_texture = dest_res;
			const D3D12_RESOURCE_DESC &texture_desc = dest_desc;
			if (texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D &&
					texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
					texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) {
				destination_error = "Win32Backend: GPU texture destination RID does not reference a texture resource.";
			} else {
				uint64_t mip_levels = texture_desc.MipLevels > 0 ? texture_desc.MipLevels : 1;
				uint64_t array_size = texture_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : texture_desc.DepthOrArraySize;
				uint64_t subresource_total = mip_levels * array_size;
				uint32_t first_subresource = p_child->get_first_subresource();
				uint32_t subresource_count = p_child->get_subresource_count();
				if (subresource_count == 0 || first_subresource >= subresource_total ||
						subresource_count > subresource_total - first_subresource) {
					destination_error = "Win32Backend: GPU texture subresource range is out of bounds (first=" +
							String::num_uint64(first_subresource) + ", count=" + String::num_uint64(subresource_count) +
							", available=" + String::num_uint64(subresource_total) + ").";
				} else if (p_child->get_texture_layout() != 0 &&
						p_child->get_texture_layout() != static_cast<uint32_t>(texture_desc.Layout)) {
					destination_error = "Win32Backend: GPU texture layout metadata does not match the destination resource.";
				} else if (p_child->get_texture_format() != 0) {
					if (dest_native_format == 0 || p_child->get_texture_format() != dest_native_format) {
						destination_error = "Win32Backend: GPU texture format metadata does not match the destination resource.";
					} else if (gpu_destination_in_flight(destination_texture)) {
						// MULTIPLE_SUBRESOURCES_RANGE destinations must be in COMMON;
						// state is unknowable while DirectStorage owns this texture -
						// reject loudly, do not fall back silently (see above).
						destination_error = "Win32Backend: GPU texture destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before sampling or reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
						destination_outcome = EnqueueOutcome::TRANSIENT;
					}
				} else if (gpu_destination_in_flight(destination_texture)) {
					// Same in-flight rejection when no format metadata was supplied
					// (the format check above is skipped, the contract still applies).
					destination_error = "Win32Backend: GPU texture destination is owned by an in-flight DirectStorage batch; wait for batch_completed / wait_for_completion (or collect_completed_results) before sampling or reusing it. For CPU-side access use a DESTINATION_MEMORY request instead.";
					destination_outcome = EnqueueOutcome::TRANSIENT;
				}
				if (destination_error.is_empty()) {
					// Row-range upload (chunked payloads). Validated here because the
					// box is derived from the mip dimensions, which only this scope has.
					const uint32_t region_top = p_child->get_texture_region_top();
					const uint32_t region_bottom = p_child->get_texture_region_bottom();
					if (p_child->has_texture_row_region()) {
						const uint32_t mip_index = subresource_mip_index(first_subresource, mip_levels);
						const uint32_t mip_height = static_cast<uint32_t>(std::max<uint64_t>(1, texture_desc.Height >> mip_index));
						if (texture_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
							destination_error = "Win32Backend: row-range texture uploads require a 2D texture.";
						} else if (subresource_count != 1) {
							destination_error = "Win32Backend: row-range texture uploads must target exactly one subresource (first=" +
									String::num_uint64(first_subresource) + ", count=" + String::num_uint64(subresource_count) + ").";
						} else if (region_bottom > mip_height) {
							destination_error = "Win32Backend: texture row range is out of bounds (top=" +
									String::num_uint64(region_top) + ", bottom=" + String::num_uint64(region_bottom) +
									", height=" + String::num_uint64(mip_height) + ").";
						} else if (dxgi_format_is_block_compressed(texture_desc.Format)) {
							const uint32_t mip_width = static_cast<uint32_t>(std::max<uint64_t>(1, texture_desc.Width >> mip_index));
							if (mip_width < 4 || mip_height < 4) {
								destination_error = "Win32Backend: block-compressed row-range upload cannot target a < 4x4 tail mip (" +
										String::num_uint64(mip_width) + "x" + String::num_uint64(mip_height) +
										"); use MULTIPLE_SUBRESOURCES_RANGE without a texture row region.";
							} else if (region_top % 4 != 0 || (region_bottom % 4 != 0 && region_bottom != mip_height)) {
								destination_error = "Win32Backend: block-compressed row range must start on a 4-row boundary and end on one unless it reaches the mip edge (top=" +
										String::num_uint64(region_top) + ", bottom=" + String::num_uint64(region_bottom) + ").";
							}
						}
						if (destination_error.is_empty()) {
							// DirectStorage defines a texture destination's UncompressedSize
							// as the box's copyable footprint (GetCopyableFootprints
							// TotalBytes) and does not check it: "for other destination
							// types, the uncompressed size may be greater than the
							// destination size" (dstorage.h:486-492). A wrong extent is
							// therefore written silently - surplus clipped, shortfall
							// zero-filling the band tail - and still reports success.
							// Refuse the mismatch here, where the resource descriptor and
							// the box are both in scope.
							Microsoft::WRL::ComPtr<ID3D12Device> extent_device;
							if (SUCCEEDED(destination_texture->GetDevice(IID_PPV_ARGS(&extent_device))) && extent_device) {
								D3D12_RESOURCE_DESC footprint_desc = texture_desc;
								D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
								UINT footprint_rows = 0;
								UINT64 row_size_in_bytes = 0;
								UINT64 total_bytes = 0;
								extent_device->GetCopyableFootprints(&footprint_desc, static_cast<UINT>(first_subresource), 1, 0,
										&footprint, &footprint_rows, &row_size_in_bytes, &total_bytes);
								// RowPitch is the 256-aligned copyable pitch; pRowSizeInBytes
								// is the natural row (400 for a 100-texel RGBA8 row) and must
								// not be used here.
								const uint64_t copyable_row_bytes = static_cast<uint64_t>(footprint.Footprint.RowPitch);
								const uint32_t block = dxgi_format_is_block_compressed(texture_desc.Format) ? 4u : 1u;
								const uint64_t first_block = region_top / block;
								const uint64_t last_block = (static_cast<uint64_t>(region_bottom) - 1) / block;
								const uint64_t box_block_rows = last_block - first_block + 1;
								// What DirectStorage writes per codec: the source bytes for
								// an uncompressed copy, the declared output for GDeflate.
								const uint64_t declared_bytes = p_child->get_compression_format() == DSRequest::COMPRESSION_NONE
										? static_cast<uint64_t>(src_size)
										: static_cast<uint64_t>(uncompressed_size);
								const uint64_t expected_bytes = copyable_row_bytes * box_block_rows;
								if (declared_bytes != expected_bytes) {
									destination_error = "Win32Backend: row-range payload is " + String::num_uint64(declared_bytes) +
											" bytes but the band [" + String::num_uint64(region_top) + ", " +
											String::num_uint64(region_bottom) + ") holds " + String::num_uint64(expected_bytes) +
											" bytes at the destination's copyable row pitch (" + String::num_uint64(copyable_row_bytes) +
											" x " + String::num_uint64(box_block_rows) + " block rows).";
								}
							} else {
								// Teardown or device-removal race: never invent a rejection.
								static std::atomic<bool> extent_check_skipped{ false };
								if (!extent_check_skipped.exchange(true)) {
									UtilityFunctions::push_warning("Win32Backend: skipped the row-range payload-extent check; the destination device was unavailable.");
								}
							}
						}
					}
				}
			}
		}

		if (!destination_error.is_empty()) {
			// Slot-owned handle: close_slot_files() releases it at batch
			// completion; never closed on per-request error paths.
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				err_handler->report_error(destination_error);
			}
			reject(p_child, destination_error, destination_outcome);
			return destination_outcome;
		}
	}

	DSTORAGE_REQUEST request = {};
	// A chunked upload targets a row range of one subresource; everything else
	// keeps the historic whole-subresource / whole-buffer destinations.
	const bool texture_row_region = gpu_texture_destination && p_child->has_texture_row_region();
	request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
	request.Options.DestinationType = gpu_buffer_destination ? DSTORAGE_REQUEST_DESTINATION_BUFFER :
			texture_row_region ? DSTORAGE_REQUEST_DESTINATION_TEXTURE_REGION :
			gpu_texture_destination ? DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE :
			DSTORAGE_REQUEST_DESTINATION_MEMORY;
	request.Options.CompressionFormat = (p_child->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)
			? DSTORAGE_COMPRESSION_FORMAT_GDEFLATE
			: DSTORAGE_COMPRESSION_FORMAT_NONE;

	request.Source.File.Source = file;
	request.Source.File.Offset = p_child->get_offset();
	request.Source.File.Size = src_size;

	if (gpu_buffer_destination) {
		request.Destination.Buffer.Resource = destination_buffer;
		request.Destination.Buffer.Offset = p_destination_offset;
		request.Destination.Buffer.Size = uncompressed_size;
	} else if (texture_row_region) {
		request.Destination.Texture.Resource = destination_texture;
		request.Destination.Texture.SubresourceIndex = p_child->get_first_subresource();
		// The row band lives inside one subresource, so the horizontal span is
		// that subresource's width, not mip 0's (a mip-0 width here is 2^mip
		// times too wide, and DirectStorage rejects the box with E_INVALIDARG).
		const uint32_t mip_index = subresource_mip_index(
				static_cast<UINT>(p_child->get_first_subresource()), dest_desc.MipLevels);
		D3D12_BOX region = {};
		region.left = 0;
		region.top = p_child->get_texture_region_top();
		region.front = 0;
		region.right = static_cast<UINT>(std::max<uint64_t>(1, dest_desc.Width >> mip_index));
		region.bottom = p_child->get_texture_region_bottom();
		region.back = 1;
		request.Destination.Texture.Region = region;
	} else if (gpu_texture_destination) {
		request.Destination.MultipleSubresourcesRange.Resource = destination_texture;
		request.Destination.MultipleSubresourcesRange.FirstSubresource = p_child->get_first_subresource();
		request.Destination.MultipleSubresourcesRange.NumSubresources = p_child->get_subresource_count();
	} else {
		p_child->ensure_destination_buffer_capacity(uncompressed_size);

		// Children are internal to this call and never exposed to scripts, so this
		// store is completion bookkeeping, not the mutation guard. The caller's parent
		// request was sealed by claim_submission() at enqueue_request() entry, before
		// any payload field was read.
		p_child->set_status(DSRequest::STATUS_SUBMITTED);
		request.Destination.Memory.Buffer = p_child->get_destination_buffer_ptrw();
		request.Destination.Memory.Size = uncompressed_size;
	}
	request.UncompressedSize = uncompressed_size;
	request.CancellationTag = p_child->get_cancellation_tag();

	static std::atomic<uint64_t> s_global_req_counter{ 0 };
	uint64_t req_counter = ++s_global_req_counter;
	std::string req_name = "REQ_" + std::to_string(req_counter) + "_" + std::to_string(reinterpret_cast<uintptr_t>(p_child.ptr()));
	slot.request_names.push_back(req_name);
	request.Name = slot.request_names.back().c_str();
	slot.request_name_map[req_name] = p_child;

	ID3D12Resource *resource_guard = gpu_buffer_destination ? destination_buffer : destination_texture;
	if (resource_guard) {
		resource_guard->AddRef();
		resource_guards[p_child.ptr()] = resource_guard;
	}
	if (gpu_buffer_destination || gpu_texture_destination) {
		p_child->set_gpu_in_flight(true);
		p_child->set_status(DSRequest::STATUS_SUBMITTED);
	}
	slot.requests.push_back(p_child);
	slot.dstorage_requests.push_back(request);
	return EnqueueOutcome::STAGED;
	};

	// Transfers the resolved reference into the slot only after the request's
	// children are all staged. Registering per child leaked the slot's
	// reference when a later child failed mid-loop: rollback_staged_requests
	// pops the requests but cannot know about the file, and the guard below
	// would see file_registered already true. With this ordering a failed
	// staging leaves the guard owning the only reference, so it closes.
	auto adopt_source_file = [&]() {
		if (!file_registered) {
			register_slot_source_file(slot, wpath, pre_opened_file);
			file_registered = true;
		}
	};

	if (gpu_texture_destination && has_texture_mip_entries && texture_subresource_count > 1) {
		// Multi-mip .dstex files store each mip level as an independent GDeflate compressed stream.
		// DirectStorage's MultipleSubresourcesRange destination for multiple subresources expects a single continuous stream,
		// so multi-mip textures MUST be split into per-mip requests using DSTORAGE_REQUEST_DESTINATION_MULTIPLE_SUBRESOURCES_RANGE with NumSubresources = 1.
		// STAGING INVARIANT (multi-mip): textures split per-mip, so the whole-texture
		// footprint MAY exceed staging_size, but EACH mip must independently satisfy
		// MAX(compressed_size, uncompressed_size) <= staging_size (enforced per-mip
		// in the loop below). A mip larger than staging cannot be split further
		// without corrupting its GDeflate tile stream, so it fails loudly here.
		if (staging_size > 0 && texture_footprint > staging_size && staging_size < GDEFLATE_PAGE_SIZE) {
			return reject(p_request, "DirectStorage cannot split a GDeflate texture with the configured staging buffer.", EnqueueOutcome::TERMINAL);
		}

		if (staging_size > 0 && texture_footprint > staging_size) {
			std::string warning_key = p_request->get_file_path().utf8().get_data();
			if (staging_buffer_warnings.insert(warning_key).second) {
				UtilityFunctions::push_warning(
						"[DirectStorage] Texture '", p_request->get_file_path(), "' uncompressed footprint (",
						String::num_uint64(texture_footprint / (1024 * 1024)), " MiB) exceeds DirectStorage staging buffer size (",
						String::num_uint64(staging_size / (1024 * 1024)), " MiB). Splitting into per-mip requests. "
						"To eliminate per-mip request overhead, consider increasing Project Settings > DirectStorage > Staging Buffer Size.");
			}
		}

		DSChunkedRequestState state;
		state.parent = p_request;
		// Pass 1: Pre-validation of all mips against staging size and offsets
		for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
			const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
			if (staging_size > 0 && (mip_entry.compressed_size > staging_size || mip_entry.uncompressed_size > staging_size ||
					mip_entry.offset > UINT64_MAX - mip_entry.compressed_size)) {
				return reject(p_request, "DirectStorage texture payload exceeds staging buffer capacity; increase dsgd_extension/staging_buffer_size_mb", EnqueueOutcome::TERMINAL);
			}
		}

		// Pass 2: Atomic staging and enqueue
		std::vector<Ref<DSRequest>> staged_children;
		staged_children.reserve(texture_subresource_count);
		for (uint32_t mip_index = 0; mip_index < texture_subresource_count; mip_index++) {
			const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource + mip_index];
			Ref<DSRequest> child;
			child.instantiate();
			child->set_file_path(p_request->get_file_path());
			child->set_offset(mip_entry.offset);
			child->set_size(mip_entry.compressed_size);
			child->set_uncompressed_size(mip_entry.uncompressed_size);
			child->set_compression_format(p_request->get_compression_format());
			child->set_destination_type(DSRequest::DESTINATION_GPU_TEXTURE);
			child->set_priority(p_request->get_priority());
			child->set_cancellation_tag(p_request->get_cancellation_tag());
			child->set_destination_resource(p_request->get_destination_resource());
			child->set_first_subresource(texture_first_subresource + mip_index);
			child->set_subresource_count(1);
			child->set_texture_format(p_request->get_texture_format());
			child->set_texture_layout(p_request->get_texture_layout());
			staged_children.push_back(child);
		}

		uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
		const uint32_t slot_budget = per_slot_admission_capacity(queue_capacity);
		if (!admits_requests(static_cast<uint32_t>(slot.requests.size()), static_cast<uint32_t>(staged_children.size()), slot_budget)) {
			s_rejected_admission_count.fetch_add(1, std::memory_order_relaxed);
			return reject(p_request, ds_capacity_reject_message(slot_budget), EnqueueOutcome::TRANSIENT);
		}
		ds_note_staged_ops(static_cast<uint32_t>(slot.requests.size() + staged_children.size()) + 2);

		for (size_t c = 0; c < staged_children.size(); ++c) {
			const EnqueueOutcome child_outcome = enqueue_one(staged_children[c], 0);
			if (child_outcome != EnqueueOutcome::STAGED) {
				rollback_staged_requests(slot.requests, slot.request_names, slot.request_name_map, staged_children, c, p_request, &slot.dstorage_requests);
				if (r_outcome != nullptr) {
					*r_outcome = child_outcome;
				}
				return false;
			}
			state.children.push_back(staged_children[c]);
			state.destination_offsets.push_back(0);
			chunk_parents[staged_children[c].ptr()] = p_request.ptr();
		}
		adopt_source_file();
		chunked_requests[p_request.ptr()] = state;
		// The parent is what callers (and DSPreloader) hold and poll, so it must
		// report the hardware window, not just the children. Without this the
		// preloader sees a cancelled parent as terminal while its per-mip children
		// are still DMA-writing the destination texture.
		p_request->set_gpu_in_flight(true);
		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
	}
	if (gpu_texture_destination && has_texture_mip_entries && texture_subresource_count == 1) {
		// STAGING INVARIANT (single-mip): a single mip is one indivisible GDeflate
		// tile stream and cannot be split across chunks by DirectStorage without
		// corrupting it, so it requires MAX(compressed_size, uncompressed_size)
		// <= staging_size (enforced below). Oversize single-mip payloads fail
		// loudly instead of being chunked.
		const DSTexMipEntryV2 &mip_entry = texture_mip_entries[texture_first_subresource];
		if (staging_size > 0 && (mip_entry.compressed_size > staging_size || mip_entry.uncompressed_size > staging_size)) {
			return reject(p_request, "DirectStorage texture payload exceeds staging buffer capacity; increase dsgd_extension/staging_buffer_size_mb", EnqueueOutcome::TERMINAL);
		}

		uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
		const uint32_t slot_budget = per_slot_admission_capacity(queue_capacity);
		if (!admits_requests(static_cast<uint32_t>(slot.requests.size()), 1, slot_budget)) {
			s_rejected_admission_count.fetch_add(1, std::memory_order_relaxed);
			return reject(p_request, ds_capacity_reject_message(slot_budget), EnqueueOutcome::TRANSIENT);
		}
		ds_note_staged_ops(static_cast<uint32_t>(slot.requests.size()) + 3);

		// Build child request with parsed mip entries to preserve p_request immutability and submit exact offsets/sizes.
		// Note: Uses already-parsed mip_entry from pre-lock phase; does NOT perform disk I/O under batch_mutex.
		Ref<DSRequest> child;
		if (!build_single_mip_child_request(p_request, mip_entry, child)) {
			return reject(p_request, "DirectStorage failed to build single-mip texture child request.", EnqueueOutcome::TERMINAL);
		}

		DSChunkedRequestState state;
		state.parent = p_request;
		const EnqueueOutcome single_child_outcome = enqueue_one(child, 0);
		if (single_child_outcome != EnqueueOutcome::STAGED) {
			// The child already carries the specific message; the parent reports it
			// with the child's retry classification so a busy destination stays
			// retryable through the wrapper.
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message(child->get_error_message().is_empty() ?
					"DirectStorage failed to enqueue single-mip GDeflate texture." :
					child->get_error_message());
			if (r_outcome != nullptr) {
				*r_outcome = single_child_outcome;
			}
			return false;
		}
		state.children.push_back(child);
		state.destination_offsets.push_back(0);
		chunk_parents[child.ptr()] = p_request.ptr();
		adopt_source_file();
		chunked_requests[p_request.ptr()] = state;
		// Same contract as the multi-mip branch above: the parent carries the
		// in-flight flag so cancellation cannot look terminal before the fence
		// retires.
		p_request->set_gpu_in_flight(true);
		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
	}

	if (staging_size > 0 && request_size > staging_size &&
			(has_row_region || p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE)) {
		if (has_row_region) {
			// A row-range chunk cannot be split further without corrupting its
			// tile stream, so an oversized one is rejected before submission.
			return reject(p_request, "DirectStorage row-range chunk (" + String::num_uint64(request_size) +
					" bytes) exceeds the staging buffer (" + String::num_uint64(staging_size) +
					" bytes); increase dsgd_extension/staging_buffer_size_mb", EnqueueOutcome::TERMINAL);
		}
		// Non-texture (or non-GPU-texture) GDeflate payloads larger than staging may
		// be page-chunked below; GPU-texture destinations never reach here chunked —
		// they are governed by the single- vs multi-mip staging invariant above
		// (single-mip no-split, multi-mip per-mip limit) and fail loudly instead.
		if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE || staging_size < GDEFLATE_PAGE_SIZE) {
			return reject(p_request, "DirectStorage cannot split this oversized GDeflate destination with the configured staging buffer.", EnqueueOutcome::TERMINAL);
		}

		uint64_t chunk_capacity = staging_size - (staging_size % GDEFLATE_PAGE_SIZE);
		Vector<DSChunkBoundary> dstex_chunks;
		bool use_dstex_chunks = get_dstex_mip_chunks(p_request->get_file_path(), p_request->get_offset(), src_size, chunk_capacity, dstex_chunks);
		if (!use_dstex_chunks) {
			return reject(p_request, "DirectStorage texture payload exceeds staging buffer capacity; increase dsgd_extension/staging_buffer_size_mb", EnqueueOutcome::TERMINAL);
		}
		uint64_t chunk_count = dstex_chunks.size();

		// Pass 1: Pre-validation of all chunk ranges
		for (uint64_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
			const DSChunkBoundary &boundary = dstex_chunks[chunk_index];
			uint64_t src_begin = boundary.source_offset;
			uint64_t src_end = boundary.source_offset + boundary.source_size;
			uint64_t destination_begin = boundary.destination_offset;
			uint64_t destination_end = boundary.destination_offset + boundary.destination_size;
			if (src_end <= src_begin || destination_end <= destination_begin || src_end - src_begin > chunk_capacity || destination_end - destination_begin > chunk_capacity) {
				return reject(p_request, "DirectStorage texture payload exceeds staging buffer capacity; increase dsgd_extension/staging_buffer_size_mb", EnqueueOutcome::TERMINAL);
			}
		}

		// Pass 2: Atomic staging and enqueue
		DSChunkedRequestState state;
		state.parent = p_request;
		std::vector<Ref<DSRequest>> staged_children;
		std::vector<uint64_t> staged_destination_offsets;
		staged_children.reserve(chunk_count);
		staged_destination_offsets.reserve(chunk_count);
		for (uint64_t chunk_index = 0; chunk_index < chunk_count; chunk_index++) {
			const DSChunkBoundary &boundary = dstex_chunks[chunk_index];
			uint64_t src_begin = boundary.source_offset;
			uint64_t src_end = boundary.source_offset + boundary.source_size;
			uint64_t destination_begin = boundary.destination_offset;
			uint64_t destination_end = boundary.destination_offset + boundary.destination_size;

			Ref<DSRequest> child;
			child.instantiate();
			child->set_file_path(p_request->get_file_path());
			child->set_offset(p_request->get_offset() + src_begin);
			child->set_size(static_cast<uint32_t>(src_end - src_begin));
			child->set_uncompressed_size(static_cast<uint32_t>(destination_end - destination_begin));
			child->set_compression_format(p_request->get_compression_format());
			child->set_destination_type(p_request->get_destination_type());
			child->set_priority(p_request->get_priority());
			child->set_cancellation_tag(p_request->get_cancellation_tag());
			child->set_destination_resource(p_request->get_destination_resource());
			child->set_first_subresource(p_request->get_first_subresource());
			child->set_subresource_count(p_request->get_subresource_count());
			child->set_texture_format(p_request->get_texture_format());
			child->set_texture_layout(p_request->get_texture_layout());
			child->get_destination_buffer_ref().resize(static_cast<int64_t>(destination_end - destination_begin));
			staged_children.push_back(child);
			staged_destination_offsets.push_back(destination_begin);
		}

		uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
		const uint32_t slot_budget = per_slot_admission_capacity(queue_capacity);
		if (!admits_requests(static_cast<uint32_t>(slot.requests.size()), static_cast<uint32_t>(staged_children.size()), slot_budget)) {
			s_rejected_admission_count.fetch_add(1, std::memory_order_relaxed);
			return reject(p_request, ds_capacity_reject_message(slot_budget), EnqueueOutcome::TRANSIENT);
		}
		ds_note_staged_ops(static_cast<uint32_t>(slot.requests.size() + staged_children.size()) + 2);

		for (size_t c = 0; c < staged_children.size(); ++c) {
			const EnqueueOutcome child_outcome = enqueue_one(staged_children[c], staged_destination_offsets[c]);
			if (child_outcome != EnqueueOutcome::STAGED) {
				rollback_staged_requests(slot.requests, slot.request_names, slot.request_name_map, staged_children, c, p_request, &slot.dstorage_requests);
				if (r_outcome != nullptr) {
					*r_outcome = child_outcome;
				}
				return false;
			}
			state.children.push_back(staged_children[c]);
			state.destination_offsets.push_back(staged_destination_offsets[c]);
			chunk_parents[staged_children[c].ptr()] = p_request.ptr();
		}
		adopt_source_file();
		chunked_requests[p_request.ptr()] = state;
		if (p_request->get_destination_type() == DSRequest::DESTINATION_GPU_BUFFER ||
				p_request->get_destination_type() == DSRequest::DESTINATION_GPU_TEXTURE) {
			p_request->set_gpu_in_flight(true);
		}
		p_request->set_status(DSRequest::STATUS_SUBMITTED);
		return true;
	}

	uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
	const uint32_t slot_budget = per_slot_admission_capacity(queue_capacity);
	if (!admits_requests(static_cast<uint32_t>(slot.requests.size()), 1, slot_budget)) {
		s_rejected_admission_count.fetch_add(1, std::memory_order_relaxed);
		return reject(p_request, ds_capacity_reject_message(slot_budget), EnqueueOutcome::TRANSIENT);
	}
	ds_note_staged_ops(static_cast<uint32_t>(slot.requests.size()) + 3);
	const EnqueueOutcome request_outcome = enqueue_one(p_request, 0);
	if (request_outcome != EnqueueOutcome::STAGED) {
		if (r_outcome != nullptr) {
			*r_outcome = request_outcome;
		}
		return false;
	}
	adopt_source_file();
	return true;
#else
	if (r_outcome != nullptr) {
		*r_outcome = EnqueueOutcome::TERMINAL;
	}
	return false;
#endif
}

bool Win32Backend::submit_async() {
#ifdef _WIN32
	if (!accepting_requests.load() || sync_batch_in_flight.load()) {
		return false;
	}
	DSContext *ctx = DSContext::get_singleton();
	const uint32_t queue_capacity = ctx ? ctx->get_queue_capacity() : 512;
	const bool pipelining = can_pipeline(queue_capacity);
	int submit_slot_index = 0;
	// Enqueue payload staged under batch_mutex and consumed after it is released.
	std::vector<DSTORAGE_REQUEST> local_dstorage[QUEUE_COUNT];
	bool enqueue_queue[QUEUE_COUNT] = { false };
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		submit_slot_index = active_batch_slot.load();
		if (pipelining) {
			bool target_busy = false;
			for (const QueueState &queue : priority_queues) {
				const BatchSlot &target = queue.batch_slots[submit_slot_index];
				target_busy = target_busy || target.submitted || target.closing || target.completion_ready;
			}
			if (target_busy) {
				const int alternate = 1 - submit_slot_index;
				bool alternate_free = true;
				for (const QueueState &queue : priority_queues) {
					const BatchSlot &slot = queue.batch_slots[alternate];
					alternate_free = alternate_free && !slot.submitted && !slot.closing && !slot.completion_ready;
				}
				if (alternate_free) {
					submit_slot_index = alternate;
					active_batch_slot.store(alternate);
				}
			}
		}
		// Atomic claim gate: between this claim and the second lock below (which
		// sets `submitted`) a claimed batch is only visible as `closing`, so both
		// states must count on the target slot. Two slots may now be in flight at
		// once because each one's admission budget is halved; tiny queues that
		// cannot be split keep the old all-slot serialization.
		for (const QueueState &queue : priority_queues) {
			const BatchSlot &target = queue.batch_slots[submit_slot_index];
			if (target.submitted || target.closing || target.completion_ready) {
				return false;
			}
			if (!pipelining) {
				for (const BatchSlot &sibling : queue.batch_slots) {
					if (sibling.submitted || sibling.closing) {
						return false;
					}
				}
			}
		}
		bool has_work = false;
		for (const QueueState &queue : priority_queues) {
			const BatchSlot &slot = queue.batch_slots[submit_slot_index];
			if (slot.submitted || slot.closing || slot.completion_ready) {
				return false;
			}
			if (slot.requests.size() > 0) {
				has_work = true;
			}
		}
		if (!has_work) {
			return true;
		}
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			BatchSlot &slot = queue.batch_slots[submit_slot_index];
			slot.closing = true;
			// Take the payload out of the slot while the lock is held. The loop
			// below hands it to DirectStorage with batch_mutex released, so it has
			// to be memory no other thread can reach: cancel_requests_with_tag()
			// walks these same containers under this lock, and reading them while
			// the submitting thread still owns and clears them is a data race.
			// Copy rather than move so the slot keeps its capacity for the next
			// flush.
			enqueue_queue[queue_index] = queue.queue != nullptr && !slot.dstorage_requests.empty();
			if (!enqueue_queue[queue_index]) {
				continue;
			}
			local_dstorage[queue_index] = slot.dstorage_requests;
			slot.dstorage_requests.clear();
			// Re-seat every name while the source strings cannot move: DirectStorage
			// retains Name for the life of the request.
			for (size_t i = 0; i < local_dstorage[queue_index].size() && i < slot.request_names.size(); i++) {
				local_dstorage[queue_index][i].Name = slot.request_names[i].c_str();
			}
		}
		active_batch_slot.store(1 - submit_slot_index);
	}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	if (enqueue_window_hook) {
		enqueue_window_hook();
	}
#endif

	// Dispatch EnqueueRequest to DirectStorage outside batch_mutex to prevent
	// queue back-pressure deadlocks with completion polling. The payload is the
	// stack-local copy taken above, never the slot's own container.
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		if (!enqueue_queue[queue_index] || local_dstorage[queue_index].empty()) {
			continue;
		}
		priority_queues[queue_index].queue->EnqueueRequests(
				local_dstorage[queue_index].data(),
				static_cast<UINT>(local_dstorage[queue_index].size()),
				nullptr,
				0,
				DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE);
	}

	std::deque<BatchMetadata> submitted_batches;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		BatchSlot &slot = queue.batch_slots[submit_slot_index];
		slot.closing = false;
		if (!queue.queue || slot.requests.size() == 0) {
			continue;
		}
		if (slot.submitted || slot.completion_ready) {
			continue;
		}

		BatchMetadata batch;
		batch.queue_index = queue_index;
		batch.slot_index = submit_slot_index;
		batch.start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
		batch.request_count = count_logical_requests(slot.requests);
		// Per-batch tag accounting: group logical requests by
		// cancellation_tag. Chunk children resolve to their logical parent
		// via get_logical_request (same tag) and count once, matching
		// count_logical_requests(). Untagged sync requests land under tag 0.
		{
			std::unordered_map<uint64_t, int> tag_totals;
			std::unordered_set<DSRequest *> seen_logical;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_null()) {
					continue;
				}
				DSRequest *logical = get_logical_request(slot.requests[i].ptr());
				if (!logical || !seen_logical.insert(logical).second) {
					continue;
				}
				tag_totals[slot.requests[i]->get_cancellation_tag()]++;
			}
			for (std::unordered_map<uint64_t, int>::const_iterator tag_it = tag_totals.begin(); tag_it != tag_totals.end(); ++tag_it) {
				BatchMetadata::BatchTagCount tag_count;
				tag_count.tag = tag_it->first;
				tag_count.request_count = tag_it->second;
				batch.tag_counts.push_back(tag_count);
			}
		}
		for (int i = 0; i < slot.requests.size(); i++) {
			if (slot.requests[i].is_valid()) {
				uint32_t request_size = slot.requests[i]->get_size();
				uint32_t uncompressed_size = slot.requests[i]->get_uncompressed_size();
				batch.compressed_bytes += request_size;
				batch.uncompressed_bytes += uncompressed_size > 0 ? uncompressed_size : request_size;
			}
		}

		// Snapshot the error generation instead of clearing the handler: a failure reported by an
		// earlier batch stays readable through has_error()/get_last_error() until a caller clears
		// it, and this batch's own error_state is derived by comparing the sequence at completion.
		DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
		if (err_handler) {
			batch.error_sequence = err_handler->get_error_sequence();
		}
		uint64_t completion_value = queue.completion_fence_value + 1;
		ResetEvent(slot.event);

		queue.completion_fence_value = completion_value;
		batch.fence_value = completion_value;
		slot.fence_value = completion_value;
		slot.submitted = true;
		slot.completion_ready = false;
		slot.error_code = 0;
		slot.error_message = String();
		queue.queue->EnqueueSignal(queue.completion_fence, completion_value);
		queue.queue->EnqueueSetEvent(slot.event);
		queue.queue->Submit();
		submitted_batches.push_back(batch);
	}

	for (BatchMetadata &batch : submitted_batches) {
		completion_queue.push_back(batch);
	}
	batch_in_flight.store(has_in_flight_batches_locked());
	update_profiler_queue_depth_locked();
	batch_cv.notify_one();
	}
	return true;
#else
	return true;
#endif
}

void Win32Backend::collect_completed_results() {
#ifdef _WIN32
	uint64_t fence_value = 0;
	int completed_count = 0;
	int error_code = 0;
	String error_message;
	while (collect_completed_batch(fence_value, completed_count, error_code, error_message)) {
	}
#endif
}

bool Win32Backend::has_pending_results() {
#ifdef _WIN32
	poll_batch_status();
	std::lock_guard<std::mutex> lock(batch_mutex);
	int active_slot_index = active_batch_slot.load();
	for (const QueueState &queue : priority_queues) {
		if (queue.batch_slots[1 - active_slot_index].completion_ready || queue.batch_slots[active_slot_index].completion_ready) {
			return true;
		}
	}
#endif
	return false;
}

bool Win32Backend::collect_completed_batch(uint64_t &r_fence_value, int &r_completed_count, int &r_error_code, String &r_error_message) {
#ifdef _WIN32
	poll_batch_status();
	std::lock_guard<std::mutex> lock(batch_mutex);
	int active_slot_index = active_batch_slot.load();
	for (int pass = 0; pass < 2; pass++) {
		int slot_index = pass == 0 ? 1 - active_slot_index : active_slot_index;
		for (QueueState &queue : priority_queues) {
			BatchSlot &slot = queue.batch_slots[slot_index];
			if (!slot.completion_ready) {
				continue;
			}
			if (slot.is_sync_submission) {
				continue;
			}
			r_fence_value = slot.fence_value;
			r_completed_count = slot.completed_count;
			r_error_code = slot.error_code;
			r_error_message = slot.error_message;
			close_slot_files(slot);
			clear_completed_slot_locked(slot);
			slot.completion_ready = false;
			batch_in_flight.store(has_in_flight_batches_locked());
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return true;
		}
	}
#endif
	return false;
}

void Win32Backend::submit_batch() {
#ifdef _WIN32
	if (!accepting_requests.load()) {
		return;
	}
	int submit_slot_index = -1;
	uint32_t sync_timeout_ms = 3000;
	if (dsgd_settings::has_setting("sync_flush_timeout_ms", "sync_flush_timeout_ms")) {
		sync_timeout_ms = static_cast<uint32_t>(std::clamp<int64_t>(int64_t(dsgd_settings::get_setting("sync_flush_timeout_ms", "sync_flush_timeout_ms")), 500, 10000));
	}
	uint32_t half_timeout_ms = sync_timeout_ms / 2;
	auto sync_start_time = std::chrono::steady_clock::now();
	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		if (!batch_cv.wait_for(lock, std::chrono::milliseconds(half_timeout_ms), [this]() {
				return !has_in_flight_batches_locked() && !sync_batch_in_flight.load();
			})) {
			UtilityFunctions::push_error("[DirectStorage] Timed out waiting for the previous batch to complete.");
			return;
		}
		sync_batch_in_flight.store(true);
		submit_slot_index = active_batch_slot.load();
		auto slot_is_available = [this](int p_slot_index) {
			for (const QueueState &queue : priority_queues) {
				const BatchSlot &slot = queue.batch_slots[p_slot_index];
				if (slot.submitted || slot.completion_ready) {
					return false;
				}
			}
			return true;
		};
		auto reclaim_slot_if_completed = [this](int p_slot_index) {
			for (QueueState &queue : priority_queues) {
				BatchSlot &slot = queue.batch_slots[p_slot_index];
				if (!slot.submitted && slot.completion_ready) {
					close_slot_files(slot);
					slot.requests.clear();
					slot.request_names.clear();
					slot.request_name_map.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = 0;
					slot.error_message = String();
					slot.completion_ready = false;
				}
			}
		};
		reclaim_slot_if_completed(submit_slot_index);
		if (!slot_is_available(submit_slot_index)) {
			reclaim_slot_if_completed(1 - submit_slot_index);
			if (slot_is_available(1 - submit_slot_index)) {
				submit_slot_index = 1 - submit_slot_index;
				active_batch_slot.store(submit_slot_index);
			}
		}
		if (!slot_is_available(submit_slot_index)) {
			sync_batch_in_flight.store(false);
			UtilityFunctions::push_error("[DirectStorage] No batch slot is available for synchronous submission.");
			batch_cv.notify_all();
			return;
		}
		update_profiler_queue_depth_locked();
	}

	// Enqueue payload staged under batch_mutex and consumed after it is released.
	std::vector<DSTORAGE_REQUEST> local_dstorage[QUEUE_COUNT];
	bool enqueue_queue[QUEUE_COUNT] = { false };
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			BatchSlot &slot = queue.batch_slots[submit_slot_index];
			slot.closing = true;
			// Same contract as submit_async(): the payload leaves the slot while the
			// lock is held, so the enqueue below never shares a container with a
			// concurrent cancel_requests_with_tag().
			enqueue_queue[queue_index] = queue.queue != nullptr && !slot.dstorage_requests.empty();
			if (!enqueue_queue[queue_index]) {
				continue;
			}
			local_dstorage[queue_index] = slot.dstorage_requests;
			slot.dstorage_requests.clear();
			for (size_t i = 0; i < local_dstorage[queue_index].size() && i < slot.request_names.size(); i++) {
				local_dstorage[queue_index][i].Name = slot.request_names[i].c_str();
			}
		}
		active_batch_slot.store(1 - submit_slot_index);
	}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
	if (enqueue_window_hook) {
		enqueue_window_hook();
	}
#endif

	// Dispatch EnqueueRequest to DirectStorage outside batch_mutex to prevent
	// queue back-pressure deadlocks with completion polling. The payload is the
	// stack-local copy taken above, never the slot's own container.
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		if (!enqueue_queue[queue_index] || local_dstorage[queue_index].empty()) {
			continue;
		}
		priority_queues[queue_index].queue->EnqueueRequests(
				local_dstorage[queue_index].data(),
				static_cast<UINT>(local_dstorage[queue_index].size()),
				nullptr,
				0,
				DSTORAGE_ENQUEUE_REQUEST_FLAG_NONE);
	}

	Vector<int> submitted_queues;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
			QueueState &queue = priority_queues[queue_index];
			BatchSlot &slot = queue.batch_slots[submit_slot_index];
			slot.closing = false;
			if (!queue.queue || !queue.completion_fence || slot.requests.size() == 0) {
				continue;
			}
			uint64_t start_time = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			int request_count = count_logical_requests(slot.requests);
			uint64_t total_compressed_bytes = 0;
			uint64_t total_uncompressed_bytes = 0;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid()) {
					uint32_t req_size = slot.requests[i]->get_size();
					uint32_t req_unc_size = slot.requests[i]->get_uncompressed_size();
					total_compressed_bytes += req_size;
					total_uncompressed_bytes += req_unc_size > 0 ? req_unc_size : req_size;
				}
			}
			BatchMetadata batch;
			batch.queue_index = queue_index;
			batch.slot_index = submit_slot_index;
			batch.start_time = start_time;
			batch.compressed_bytes = total_compressed_bytes;
			batch.uncompressed_bytes = total_uncompressed_bytes;
			batch.request_count = request_count;
			// Same contract as submit_async(): snapshot the error generation, never clear it.
			DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
			if (err_handler) {
				batch.error_sequence = err_handler->get_error_sequence();
			}
			uint64_t completion_value = queue.completion_fence_value + 1;
			ResetEvent(slot.event);
			queue.completion_fence_value = completion_value;
			batch.fence_value = completion_value;
			slot.fence_value = completion_value;
			slot.submitted = true;
			slot.is_sync_submission = true;
			slot.completion_ready = false;
			slot.error_code = 0;
			slot.error_message = String();
			queue.queue->EnqueueSignal(queue.completion_fence, completion_value);
			queue.queue->EnqueueSetEvent(slot.event);
			queue.queue->Submit();
			completion_queue.push_back(batch);
			submitted_queues.push_back(queue_index);
		}
		if (submitted_queues.is_empty()) {
			sync_batch_in_flight.store(false);
			update_profiler_queue_depth_locked();
			batch_cv.notify_all();
			return;
		}
		batch_in_flight.store(!completion_queue.empty());
		update_profiler_queue_depth_locked();
	}
	batch_cv.notify_one();

	{
		std::unique_lock<std::mutex> lock(batch_mutex);
		bool completed = batch_cv.wait_for(lock, std::chrono::milliseconds(sync_timeout_ms), [this, submit_slot_index, &submitted_queues]() {
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				const BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				if (slot.submitted || !slot.completion_ready) {
					return false;
				}
			}
			return true;
		});

		if (completed) {
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				slot.requests.clear();
				slot.request_names.clear();
				slot.request_name_map.clear();
				slot.fence_value = 0;
				slot.completed_count = 0;
				slot.error_code = 0;
				slot.error_message = String();
				slot.closing = false;
				slot.completion_ready = false;
				slot.is_sync_submission = false;
			}
			auto sync_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_start_time).count();
			if (sync_elapsed_ms > 100) {
				UtilityFunctions::print_verbose("[DirectStorage] Synchronous flush stalled for " + String::num_int64(sync_elapsed_ms) + " ms; avoid calling sync flushes from gameplay loops.");
			}
		} else {
			UtilityFunctions::push_error("[DirectStorage] Win32Backend priority queue submit wait timed out or failed.");
			for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end(); ) {
				if (it->slot_index == submit_slot_index) {
					it = completion_queue.erase(it);
				} else {
					++it;
				}
			}
			for (int i = 0; i < submitted_queues.size(); i++) {
				int q_idx = submitted_queues[i];
				BatchSlot &slot = priority_queues[q_idx].batch_slots[submit_slot_index];
				slot.is_sync_submission = false;
				if (slot.submitted || !slot.completion_ready) {
					std::unordered_set<uint64_t> tags;
					for (int r = 0; r < slot.requests.size(); r++) {
						if (slot.requests[r].is_valid()) {
							tags.insert(slot.requests[r]->get_cancellation_tag());
							slot.requests[r]->set_status(DSRequest::STATUS_FAILED);
							slot.requests[r]->set_error_message("Batch submission timed out.");
						}
					}
					if (priority_queues[q_idx].queue) {
						for (uint64_t tag : tags) {
							priority_queues[q_idx].queue->CancelRequestsWithTag(UINT64_MAX, tag);
						}
					}
					ZombieBatchSlot zombie;
					zombie.queue_index = q_idx;
					zombie.fence_value = slot.fence_value;
					zombie.requests = slot.requests;
					zombie.files = slot.files;
					zombie.file_cache = slot.file_cache;
					zombie_slots.push_back(zombie);

					slot.requests.clear();
					slot.files.clear();
					slot.file_cache.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = -1;
					slot.error_message = "Batch submission timed out.";
					slot.closing = false;
					slot.submitted = false;
					slot.completion_ready = false;
				} else {
					close_slot_files(slot);
					slot.requests.clear();
					slot.fence_value = 0;
					slot.completed_count = 0;
					slot.error_code = 0;
					slot.error_message = String();
					slot.closing = false;
					slot.completion_ready = false;
				}
			}
		}
		sync_batch_in_flight.store(false);
		batch_in_flight.store(has_in_flight_batches_locked());
		update_profiler_queue_depth_locked();
	}
	batch_cv.notify_all();
	return;

#endif
}

void Win32Backend::cancel_requests_with_tag(uint64_t p_tag) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(batch_mutex);
	for (QueueState &queue : priority_queues) {
		if (queue.queue) {
			queue.queue->CancelRequestsWithTag(UINT64_MAX, p_tag);
		}
		for (BatchSlot &slot : queue.batch_slots) {
			Vector<Ref<DSRequest>> remaining;
			Vector<Ref<DSRequest>> dropped;
			std::vector<std::string> remaining_names;
			std::vector<DSTORAGE_REQUEST> remaining_dstorage;
			uint64_t now_usec = Time::get_singleton() ? Time::get_singleton()->get_ticks_usec() : 0;
			for (int i = 0; i < slot.requests.size(); i++) {
				if (slot.requests[i].is_valid() && slot.requests[i]->get_cancellation_tag() == p_tag) {
					slot.requests[i]->set_cancel_timestamp_usec(now_usec);
					slot.requests[i]->set_status(DSRequest::STATUS_FAILED);
					slot.requests[i]->set_error_message("Request cancelled by tag.");
					if (slot.submitted || slot.closing) {
						// In-flight or closing batch: keep the request in the slot so the
						// completion thread tears it down uniformly (guard
						// release, file close, counts). It is already terminal
						// FAILED, so poll_batch_status() leaves it dropped and
						// never promotes it to COMPLETED.
						// gpu_in_flight remains true until the batch fence retires.
						remaining.push_back(slot.requests[i]);
						if (i < static_cast<int>(slot.request_names.size())) {
							remaining_names.push_back(slot.request_names[i]);
						}
						if (i < static_cast<int>(slot.dstorage_requests.size())) {
							remaining_dstorage.push_back(slot.dstorage_requests[i]);
						}
					} else {
						// Staged but unsubmitted and not closing: purge from the slot so a later
						// Submit() cannot pick up a cancelled request. Release the AddRef'd guard
						// (no leak, freed RIDs stay unwritten) and detach chunk tracking.
						slot.requests[i]->set_gpu_in_flight(false);
						dropped.push_back(slot.requests[i]);
						if (i < static_cast<int>(slot.request_names.size())) {
							slot.request_name_map.erase(slot.request_names[i]);
						}
					}
				} else {
					remaining.push_back(slot.requests[i]);
					if (i < static_cast<int>(slot.request_names.size())) {
						remaining_names.push_back(slot.request_names[i]);
					}
					if (i < static_cast<int>(slot.dstorage_requests.size())) {
						remaining_dstorage.push_back(slot.dstorage_requests[i]);
					}
				}
			}
			slot.requests = remaining;
			if (!dropped.is_empty()) {
				slot.request_names = std::move(remaining_names);
				slot.dstorage_requests = std::move(remaining_dstorage);

				release_resource_guards(dropped);
				for (int i = 0; i < dropped.size(); i++) {
					if (dropped[i].is_valid()) {
						chunk_parents.erase(dropped[i].ptr());
					}
				}
			}
			// Only the unsubmitted path empties a slot here (in-flight requests
			// stay for the completion thread), so a slot left with no requests
			// must not keep the file handles those requests pinned open. Guard
			// on submitted/closing anyway: releasing a handle a live DMA still
			// reads would be worse than the leak.
			if (!slot.submitted && !slot.closing && slot.requests.is_empty() && !slot.file_cache.empty()) {
				close_slot_files(slot);
			}
		}
	}
	// Reconcile chunked parents whose children all went terminal via the
	// drops above (parent marked FAILED with the child cancel message; maps
	// released). Touches only Ref statuses and memcpys for memory
	// destinations — no render/scene APIs, safe on the calling thread under
	// batch_mutex (same discipline as every other call site).
	complete_chunked_requests();
#endif
}

bool Win32Backend::is_resource_in_flight(const RID &p_rid) const {
#ifdef _WIN32
	if (!p_rid.is_valid()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	// A slot is done with a destination once completion_ready is set: that flag is
	// only set after the governing fence retired (poll_batch_status() checks it
	// first). Every earlier state still leads to DMA against the same resource, so
	// it must be reported as in flight:
	//   - staged    : enqueue_request() accepted the request, it will be flushed
	//                 later - freeing now lets that flush write into freed VRAM;
	//   - closing   : submit_async()/submit_batch() set closing=true, dropped the
	//                 lock for EnqueueRequests(), and have
	//                 not set submitted=true yet;
	//   - submitted : the historical case, DMA is on the hardware queue.
	// Zombie batches (synchronous flush timed out, hardware still running) are not
	// in any slot at all, so they are scanned separately below.
	for (int q = 0; q < QUEUE_COUNT; q++) {
		const QueueState &queue = priority_queues[q];
		for (int s = 0; s < BATCH_SLOT_COUNT; s++) {
			const BatchSlot &slot = queue.batch_slots[s];
			if (slot.completion_ready) {
				continue;
			}
			for (int r = 0; r < slot.requests.size(); r++) {
				const Ref<DSRequest> &req = slot.requests[r];
				if (req.is_valid() && req->get_destination_resource() == p_rid && req->is_gpu_in_flight()) {
					return true;
				}
			}
		}
	}
	for (size_t z = 0; z < zombie_slots.size(); z++) {
		const ZombieBatchSlot &zombie = zombie_slots[z];
		for (int r = 0; r < zombie.requests.size(); r++) {
			const Ref<DSRequest> &req = zombie.requests[r];
			if (req.is_valid() && req->get_destination_resource() == p_rid && req->is_gpu_in_flight()) {
				return true;
			}
		}
	}
	return false;
#else
	return false;
#endif
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
void Win32Backend::_debug_zombify_active_batch_for_testing() {
#ifdef _WIN32
	// Test-only seam. Reproduces the state submit_batch() enters when its
	// synchronous completion wait times out (see the zombie_slots move in
	// submit_batch()): the active slot's in-flight requests are moved out of the
	// slot into zombie_slots with their fence value intact, and the slot is reset.
	// The real branch needs a DMA >500 ms stall, which
	// dsgd_extension/sync_flush_timeout_ms cannot produce on demand (it is clamped
	// to [500, 10000]). Never bound in _bind_methods().
	std::lock_guard<std::mutex> lock(batch_mutex);
	for (int queue_index = 0; queue_index < QUEUE_COUNT; queue_index++) {
		QueueState &queue = priority_queues[queue_index];
		for (int slot_index = 0; slot_index < BATCH_SLOT_COUNT; slot_index++) {
			BatchSlot &slot = queue.batch_slots[slot_index];
			if (!slot.submitted || slot.requests.is_empty()) {
				continue;
			}
			for (std::deque<BatchMetadata>::iterator it = completion_queue.begin(); it != completion_queue.end();) {
				if (it->slot_index == slot_index && it->queue_index == queue_index) {
					it = completion_queue.erase(it);
				} else {
					++it;
				}
			}
			ZombieBatchSlot zombie;
			zombie.queue_index = queue_index;
			zombie.fence_value = slot.fence_value;
			zombie.requests = slot.requests;
			zombie.files = slot.files;
			zombie.file_cache = slot.file_cache;
			zombie_slots.push_back(zombie);

			slot.is_sync_submission = false;
			slot.requests.clear();
			slot.files.clear();
			slot.file_cache.clear();
			slot.fence_value = 0;
			slot.completed_count = 0;
			slot.error_code = -1;
			slot.error_message = "Batch submission timed out.";
			slot.closing = false;
			slot.submitted = false;
			slot.completion_ready = false;
		}
	}
	batch_in_flight.store(has_in_flight_batches_locked());
	update_profiler_queue_depth_locked();
#endif
}

int Win32Backend::_debug_cached_source_file_count() const {
#ifdef _WIN32
	std::lock_guard<std::mutex> f_lock(persistent_file_mutex);
	return static_cast<int>(persistent_file_cache.size());
#else
	return 0;
#endif
}

void Win32Backend::_debug_set_queue_fence_counters_for_testing(int p_queue_index, uint64_t p_completion_value, uint64_t p_completed_value) {
#ifdef _WIN32
	// Test-only seam. HIGH-02 is about two priority queues holding different
	// counter values while one of them still has DMA in flight; producing that for
	// real would need two live queues whose transfers retire at different speeds.
	// Pinning the counters directly leaves the fence objects alone, so
	// wait_for_completion() still reads a real ID3D12Fence::GetCompletedValue().
	// Callers must reset to (0, 0) before deleting the backend: shutdown() drains
	// every queue whose completion_fence_value is non-zero, and a pinned value the
	// real fence never reaches would burn that drain budget.
	// Never bound in _bind_methods().
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT) {
		return;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	QueueState &queue = priority_queues[p_queue_index];
	queue.completion_fence_value = p_completion_value;
	queue.completed_fence_value = MIN(p_completed_value, p_completion_value);
#endif
}

void Win32Backend::_debug_set_enqueue_window_hook_for_testing(const std::function<void()> &p_hook) {
	// Test-only seam. Installs the callback run inside the batch_mutex-free
	// enqueue window of submit_async()/submit_batch(), or clears it when p_hook is
	// empty. Callers must clear it before the captured state dies and before
	// deleting the backend. Never bound in _bind_methods().
	enqueue_window_hook = p_hook;
}

void Win32Backend::_debug_set_drain_counter_access_hook_for_testing(const std::function<void()> &p_hook) {
	// Test-only seam. Installs the callback run where drain_all_queues() touches
	// the per-queue completion counters, or clears it when p_hook is empty.
	// Callers must clear it before the captured state dies and before deleting the
	// backend. Never bound in _bind_methods().
	drain_counter_access_hook = p_hook;
}

void Win32Backend::_debug_get_last_drain_counters_for_testing(int p_queue_index, uint64_t &r_completion_value, uint64_t &r_completed_value) const {
	// Test-only seam. Per-queue counters as the last successful drain left them;
	// shutdown() zeroes the live counters, so this is the only post-drain observer.
	// Never bound in _bind_methods().
#ifdef _WIN32
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT) {
		return;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	r_completion_value = debug_last_drain_completion[p_queue_index];
	r_completed_value = debug_last_drain_completed[p_queue_index];
#else
	(void)p_queue_index;
	(void)r_completion_value;
	(void)r_completed_value;
#endif
}

void *Win32Backend::_debug_get_queue_completion_fence_for_testing(int p_queue_index) const {
	// Test-only seam. One queue's completion fence, so a test can drive it to a
	// value the GPU has not reached on its own. Never bound in _bind_methods().
#ifdef _WIN32
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	return priority_queues[p_queue_index].completion_fence;
#else
	(void)p_queue_index;
	return nullptr;
#endif
}

int Win32Backend::_debug_live_completion_threads_for_testing() {
#ifdef _WIN32
	return s_live_completion_threads.load(std::memory_order_acquire);
#else
	return 0;
#endif
}

int Win32Backend::_debug_pending_enqueue_count() const {
#ifdef _WIN32
	// Test-only seam. Deliberately reads the slot containers WITHOUT batch_mutex:
	// it is called from the test thread while the submitting thread is parked in
	// the hook, and 0 is exactly the state the fix guarantees - the payload is
	// owned by the submitting stack frame, not by any slot.
	int pending = 0;
	for (int q = 0; q < QUEUE_COUNT; q++) {
		for (int s = 0; s < BATCH_SLOT_COUNT; s++) {
			pending = std::max(pending, static_cast<int>(priority_queues[q].batch_slots[s].dstorage_requests.size()));
		}
	}
	return pending;
#else
	return 0;
#endif
}

bool Win32Backend::_debug_batch_mutex_is_free_for_testing() {
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). Called from a thread that is NOT the one
	// whose lock state is being measured: true means batch_mutex was free, false means something
	// holds it. Measures the submit-path serialisation (drain counters, error reporting), NOT the
	// queue-error drain pair - see _debug_queue_error_read_can_enter_for_testing for that.
	if (batch_mutex.try_lock()) {
		batch_mutex.unlock();
		return true;
	}
	return false;
#else
	// No Win32 queue to serialise against; nothing can hold the lock here.
	return true;
#endif
}

bool Win32Backend::_debug_queue_error_read_can_enter_for_testing() {
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). False means the queue-error drain holds the
	// handler's drain lock, i.e. this backend's own error-record read would block. That is the
	// contract; it is deliberately independent of which mutex implements it.
	DSErrorHandler *err_handler = DSErrorHandler::get_singleton();
	if (!err_handler) {
		return true;
	}
	std::unique_lock<std::mutex> probe;
	return err_handler->try_acquire_queue_drain_lock(probe);
#else
	// No Win32 queue to serialise against; nothing can hold the drain here.
	return true;
#endif
}

int Win32Backend::_debug_dstorage_config_apply_count() {
#ifdef _WIN32
	return s_dstorage_config_apply_count.load();
#else
	return 0;
#endif
}

int64_t Win32Backend::_debug_last_applied_staging_bytes() {
#ifdef _WIN32
	return s_last_applied_staging_bytes.load();
#else
	return 0;
#endif
}

void Win32Backend::_debug_last_runtime_module_load_for_testing(String &r_path, uint32_t &r_flags) {
	r_path = String();
	r_flags = 0;
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(s_runtime_load_record_mutex);
	// wchar_t is 16-bit on Windows, so the recorded path is already UTF-16.
	r_path = String::utf16(reinterpret_cast<const char16_t *>(s_last_runtime_load_path.c_str()));
	r_flags = s_last_runtime_load_flags;
#endif
}

void Win32Backend::_debug_set_queue_error_report_hook_for_testing(const std::function<void()> &p_hook) {
	// Test-only seam. Installs the callback run inside report_queue_error(), i.e.
	// after the caller has released batch_mutex and immediately before the engine
	// push; clears it with an empty callback. Callers must clear it before the
	// captured state dies. Never bound in _bind_methods().
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(s_queue_error_report_hook_mutex);
	s_queue_error_report_hook = p_hook;
#else
	(void)p_hook;
#endif
}

int Win32Backend::_debug_error_record_read_count() const {
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). Counts RetrieveErrorRecord()
	// calls: a batch that reports no failure must contribute 0.
	return s_error_record_read_count.load();
#else
	return 0;
#endif
}

void Win32Backend::_debug_inject_queue_error_for_testing(int p_queue_index, const String &p_message, const String &p_failed_request_name, bool p_fatal) {
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). The only real source of an
	// error record with no batch in flight is a device removal, which cannot be
	// produced on demand. This installs the record the SDK would have returned and
	// runs the same stash-or-report decision the error-event handler uses, so the
	// reporting half is exercised while only the SDK read is bypassed. Naming a
	// staged request routes the record exactly like a real one (by name, to the
	// slot that holds it); without a name it lands on the first completing slot.
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT) {
		return;
	}
	QueueErrorInfo injected;
	injected.present = true;
	injected.fatal = p_fatal;
	injected.message = p_message;
	injected.failed_request_name = p_failed_request_name;
	QueueErrorInfo immediate;
	{
		std::lock_guard<std::mutex> lock(batch_mutex);
		stash_queue_error_locked(priority_queues[p_queue_index], injected, &immediate);
	}
	report_queue_error(immediate);
#else
	(void)p_queue_index;
	(void)p_message;
	(void)p_failed_request_name;
	(void)p_fatal;
#endif
}

String Win32Backend::_debug_render_queue_error_attributed_twice_for_testing(const String &p_failed_request_name,
		const String &p_base_message, const Ref<DSRequest> &p_request, const String &p_failed_filename) {
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). Runs the production
	// attribution and rendering twice over one record, which is what the error
	// event thread and the batch claim used to do; the returned text proves the
	// result does not depend on how often it ran.
	QueueErrorInfo record;
	record.present = true;
	record.message = p_base_message;
	record.failed_request_name = p_failed_request_name;
	record.failed_filename = p_failed_filename;
	BatchSlot slot;
	if (p_request.is_valid()) {
		slot.request_name_map[std::string(p_failed_request_name.utf8().get_data())] = p_request;
	}
	for (int pass = 0; pass < 2; pass++) {
		enrich_queue_error_from_slot(slot, record);
	}
	return record.format_message();
#else
	(void)p_failed_request_name;
	(void)p_base_message;
	(void)p_request;
	(void)p_failed_filename;
	return String();
#endif
}

void Win32Backend::_debug_mark_queue_slots_submitted_for_testing(int p_queue_index) {
#ifdef _WIN32
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT) {
		return;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	for (int i = 0; i < BATCH_SLOT_COUNT; ++i) {
		priority_queues[p_queue_index].batch_slots[i].submitted = true;
	}
#endif
}

PackedStringArray Win32Backend::_debug_staged_request_names_for_testing(int p_queue_index, int p_slot_index) const {
	PackedStringArray names;
#ifdef _WIN32
	// Test-only seam, never bound in _bind_methods(). Reads the slot's staging
	// order so a test can name a real staged request in an injected record
	// without touching the slot's containers.
	if (p_queue_index < 0 || p_queue_index >= QUEUE_COUNT || p_slot_index < 0 || p_slot_index >= BATCH_SLOT_COUNT) {
		return names;
	}
	std::lock_guard<std::mutex> lock(batch_mutex);
	const std::vector<std::string> &staged = priority_queues[p_queue_index].batch_slots[p_slot_index].request_names;
	for (size_t i = 0; i < staged.size(); i++) {
		names.push_back(String(staged[i].c_str()));
	}
#else
	(void)p_queue_index;
	(void)p_slot_index;
#endif
	return names;
}

void Win32Backend::_debug_seed_instance_state_for_testing() {
#ifdef _WIN32
	// Test-only seam. Seeds this instance's containers with one synthetic
	// parent/child chunk pair and one null resource guard (never dereferenced:
	// both release paths test the pointer before Release()). The real setup needs
	// a staged multi-mip GPU request, which needs a device and a crafted .dstex;
	// the invariant under test is only "whose containers are these".
	std::lock_guard<std::mutex> lock(batch_mutex);
	Ref<DSRequest> parent;
	parent.instantiate();
	Ref<DSRequest> child;
	child.instantiate();
	DSChunkedRequestState state;
	state.parent = parent;
	state.children.push_back(child);
	state.destination_offsets.push_back(0);
	chunked_requests[parent.ptr()] = state;
	chunk_parents[child.ptr()] = parent.ptr();
	resource_guards[child.ptr()] = nullptr;
#endif
}

Dictionary Win32Backend::_debug_instance_state_for_testing() const {
	Dictionary state;
#ifdef _WIN32
	state["chunked_requests"] = static_cast<int64_t>(chunked_requests.size());
	state["chunk_parents"] = static_cast<int64_t>(chunk_parents.size());
	state["resource_guards"] = static_cast<int64_t>(resource_guards.size());
	state["staging_buffer_warnings"] = static_cast<int64_t>(staging_buffer_warnings.size());
#endif
	return state;
}

#ifdef _WIN32
void Win32Backend::_debug_set_drain_arm_hook_for_testing(const std::function<HRESULT(int, ID3D12Fence *, uint64_t, HANDLE)> &p_hook) {
	// Test-only seam. Installs the callback that substitutes the per-target
	// SetEventOnCompletion() call inside drain_all_queues(), or clears it when
	// p_hook is empty. Callers must clear it before the captured state dies and
	// before deleting the backend. Never bound in _bind_methods().
	drain_arm_hook = p_hook;
}
#endif
#endif

} // namespace godot
