#include "ds_fallback_backend.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "GDeflate.h"

namespace godot {

FallbackBackend::FallbackBackend() {
	OS *os = OS::get_singleton();
	if (os) {
		int cpus = os->get_processor_count();
		num_workers = static_cast<uint32_t>(Math::clamp(cpus, 1, 8));
	} else {
		num_workers = 4;
	}
}

bool FallbackBackend::is_available() const {
	return true;
}

String FallbackBackend::get_name() const {
	return "SoftwareFallback";
}

bool FallbackBackend::initialize() {
	pending_requests.clear();
	UtilityFunctions::print("[DirectStorage] FallbackBackend initialized (CPU Workers: " + String::num_int64(num_workers) + ").");
	return true;
}

void FallbackBackend::shutdown() {
	pending_requests.clear();
}

bool FallbackBackend::enqueue_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}

	p_request->set_status(DSRequest::STATUS_SUBMITTED);
	pending_requests.push_back(p_request);
	return true;
}

bool FallbackBackend::process_request(Ref<DSRequest> p_request) {
	if (p_request.is_null()) {
		return false;
	}

	String path = p_request->get_file_path();
	Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
	if (file.is_null()) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("FallbackBackend: Failed to open file '" + path + "'.");
		return false;
	}

	uint64_t offset = p_request->get_offset();
	uint32_t read_size = p_request->get_size();

	if (offset > 0) {
		file->seek(offset);
	}

	PackedByteArray raw_bytes = file->get_buffer(read_size);
	if (raw_bytes.size() < read_size) {
		p_request->set_status(DSRequest::STATUS_FAILED);
		p_request->set_error_message("FallbackBackend: Failed to read complete buffer from file.");
		return false;
	}

	if (p_request->get_compression_format() == DSRequest::COMPRESSION_GDEFLATE) {
		PackedByteArray dest = p_request->get_destination_buffer();
		if (dest.size() == 0) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: Destination buffer unallocated for GDeflate decompression.");
			return false;
		}

		bool ok = GDeflate::Decompress(
				dest.ptrw(),
				dest.size(),
				raw_bytes.ptr(),
				raw_bytes.size(),
				num_workers);

		if (!ok) {
			p_request->set_status(DSRequest::STATUS_FAILED);
			p_request->set_error_message("FallbackBackend: CPU GDeflate decompression failed.");
			return false;
		}
	} else {
		p_request->set_destination_buffer(raw_bytes);
	}

	p_request->set_status(DSRequest::STATUS_COMPLETED);
	return true;
}

void FallbackBackend::submit_batch() {
	for (int i = 0; i < pending_requests.size(); i++) {
		process_request(pending_requests[i]);
	}
	pending_requests.clear();
}

void FallbackBackend::cancel_requests_with_tag(uint64_t p_tag) {
	Vector<Ref<DSRequest>> remaining;
	for (int i = 0; i < pending_requests.size(); i++) {
		if (pending_requests[i].is_valid() && (pending_requests[i]->get_cancellation_tag() & UINT64_MAX) == p_tag) {
			pending_requests[i]->set_status(DSRequest::STATUS_FAILED);
			pending_requests[i]->set_error_message("Request cancelled by tag.");
		} else {
			remaining.push_back(pending_requests[i]);
		}
	}
	pending_requests = remaining;
}

} // namespace godot
