#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dspkg_format.h"
#include "formats/dstex_format.h"
#include "monocypher.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
#include "platform/win32/ds_win32_backend.h"
#endif

// Step 3 format-guard suite. Synthetic v5/v3 files are built byte-by-byte so
// every reserved/limit/error-code branch can be forged without a writer.
// Positive cases prove the readers accept the minimal legal shape; the rest
// assert the exact Godot Error from the audited format spec.

namespace {

constexpr uint64_t ALIGN_512 = 512;
constexpr uint32_t MIP_ROW_PITCH = 256;
constexpr uint32_t TEXTURE_HEIGHT_UNIT = 1; // 4xN RGBA8 => 256 bytes per row
constexpr uint32_t PAYLOAD_ROW_BYTES = 256;
constexpr uint32_t MESH_PAYLOAD_BYTES = 1024;
constexpr uint16_t V5_HEADER_MIN = 96;
constexpr uint16_t V5_ENTRY_MIN = 66;
// TLV cost fixtures: one 8-byte zero-payload record per distinct tag, with tags
// above every known tag so no summary flag or capability check is involved.
constexpr uint32_t TLV_TAG_BASE = 0x10000;
constexpr uint32_t HEADER_TLV_RECORD_COUNT = 131072;     // 1 MiB / 8-byte empty record
constexpr uint32_t HEADER_TLV_SMALL_RECORD_COUNT = 8192; // 64 KiB, 16x fewer records
constexpr uint32_t ENTRY_TLV_RECORD_COUNT = 8192;        // 64 KiB / 8-byte empty record
constexpr int64_t HEADER_TLV_TABLE_BUDGET_US = 150000;   // absolute sanity bound
constexpr int64_t HEADER_TLV_TABLE_RATIO = 60;           // quadratic ~250x here, linear ~16x
constexpr int64_t HEADER_TLV_TIMING_FLOOR_US = 1000;     // keeps the ratio finite

uint64_t align_up_test(uint64_t p_value, uint64_t p_alignment) {
	return (p_value + p_alignment - 1) / p_alignment * p_alignment;
}

bool write_bytes(const godot::String &p_path, const godot::PackedByteArray &p_bytes) {
	godot::Ref<godot::FileAccess> file = godot::FileAccess::open(p_path, godot::FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}
	file->store_buffer(p_bytes);
	file->close();
	return true;
}

void remove_file(const godot::String &p_path) {
	if (godot::FileAccess::file_exists(p_path)) {
		godot::DirAccess::remove_absolute(p_path);
	}
}

void w16(godot::PackedByteArray &r_buf, uint64_t p_offset, uint16_t p_value) {
	std::memcpy(r_buf.ptrw() + p_offset, &p_value, sizeof(p_value));
}
void w32(godot::PackedByteArray &r_buf, uint64_t p_offset, uint32_t p_value) {
	std::memcpy(r_buf.ptrw() + p_offset, &p_value, sizeof(p_value));
}
void w64(godot::PackedByteArray &r_buf, uint64_t p_offset, uint64_t p_value) {
	std::memcpy(r_buf.ptrw() + p_offset, &p_value, sizeof(p_value));
}

struct V5Spec {
	godot::String path = "res://t.dstex";
	uint16_t header_size = V5_HEADER_MIN;
	uint16_t toc_entry_size = V5_ENTRY_MIN;
	uint16_t header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA;
	uint32_t entry_flags = 0;
	uint8_t entry_kind = 0;
	uint8_t compression_format = 1; // GDEFLATE
	uint8_t gacl_transform = 0;
	uint8_t descriptor_flags = 0;
	uint32_t min_reader_version = 5;
	uint64_t capabilities = 0;
	uint32_t chunk_count = 1;
	uint32_t metadata_size = 0;
	godot::PackedByteArray entry_tlv;
	godot::PackedByteArray header_tlv;
};

struct V5Built {
	godot::PackedByteArray bytes;
	uint64_t entries_start = 0;
	uint64_t mip_table_offset = 0;
	uint64_t chunk_table_offset = 0;
	uint64_t extension_offset = 0;
	uint64_t data_offset = 0;
	uint64_t entry_fixed_offset = 0;
};

V5Built build_v5(const V5Spec &p_spec) {
	V5Built built;
	const bool is_texture = p_spec.entry_kind == 0;
	const uint32_t payload_total = is_texture ? PAYLOAD_ROW_BYTES * p_spec.chunk_count : MESH_PAYLOAD_BYTES;
	const uint32_t metadata_size = is_texture ? p_spec.metadata_size :
			(p_spec.metadata_size == 0 ? 1024 : p_spec.metadata_size);

	const godot::PackedByteArray path_bytes = p_spec.path.to_utf8_buffer();
	const uint64_t entry_fixed_bytes = 10 + path_bytes.size() + p_spec.toc_entry_size;
	const uint64_t mip_table_size = is_texture ? sizeof(godot::DSPkgMipEntry) : 0;
	const uint64_t chunk_table_size = (is_texture && p_spec.chunk_count > 1) ?
			static_cast<uint64_t>(p_spec.chunk_count) * sizeof(godot::DSChunkEntry) : 0;
	const uint64_t entry_size = entry_fixed_bytes + metadata_size + p_spec.entry_tlv.size() +
			mip_table_size + chunk_table_size;

	built.entries_start = p_spec.header_size;
	const uint64_t entries_end = built.entries_start + entry_size;
	const uint64_t extension_size = p_spec.header_tlv.size();
	uint64_t toc_end = entries_end;
	if (extension_size > 0) {
		built.extension_offset = align_up_test(entries_end, 8);
		toc_end = built.extension_offset + extension_size;
	}
	built.data_offset = align_up_test(toc_end, ALIGN_512);
	const uint64_t file_size = built.data_offset + payload_total;
	built.bytes.resize(static_cast<int64_t>(file_size));
	std::memset(built.bytes.ptrw(), 0, static_cast<size_t>(file_size));

	// Header (first 64 bytes keep the v4 layout, then the v5 tail).
	w32(built.bytes, 0, 0x474B5044u);
	w16(built.bytes, 4, 5);
	w16(built.bytes, 6, p_spec.header_flags);
	w32(built.bytes, 8, 1);
	w64(built.bytes, 12, built.data_offset);
	w64(built.bytes, 20, 0);
	w32(built.bytes, 28, 0);
	w32(built.bytes, 48, p_spec.min_reader_version);
	w64(built.bytes, 52, 0);
	w32(built.bytes, 60, 0);
	w16(built.bytes, 64, p_spec.header_size);
	w16(built.bytes, 66, p_spec.toc_entry_size);
	w64(built.bytes, 68, p_spec.capabilities);
	w64(built.bytes, 76, built.extension_offset);
	w32(built.bytes, 84, extension_size);
	w64(built.bytes, 88, 0);

	if (extension_size > 0) {
		std::memcpy(built.bytes.ptrw() + built.extension_offset, p_spec.header_tlv.ptr(), extension_size);
	}

	uint64_t cursor = built.entries_start;
	w64(built.bytes, cursor, static_cast<uint64_t>(p_spec.path.hash()));
	cursor += 8;
	w16(built.bytes, cursor, static_cast<uint16_t>(path_bytes.size()));
	cursor += 2;
	std::memcpy(built.bytes.ptrw() + cursor, path_bytes.ptr(), path_bytes.size());
	cursor += path_bytes.size();
	built.entry_fixed_offset = cursor;

	w64(built.bytes, cursor, 0); // relative data_offset
	cursor += 8;
	w64(built.bytes, cursor, payload_total);
	cursor += 8;
	w64(built.bytes, cursor, payload_total);
	cursor += 8;
	w32(built.bytes, cursor, is_texture ? 4u : 0u);
	cursor += 4;
	w32(built.bytes, cursor, is_texture ? p_spec.chunk_count : 0u);
	cursor += 4;
	w32(built.bytes, cursor, is_texture ? static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM) : 0u);
	cursor += 4;
	w32(built.bytes, cursor, is_texture ? 1u : 0u);
	cursor += 4;
	w32(built.bytes, cursor, p_spec.entry_flags);
	cursor += 4;
	built.bytes.set(cursor++, p_spec.entry_kind);
	built.bytes.set(cursor++, p_spec.compression_format);
	built.bytes.set(cursor++, p_spec.gacl_transform);
	built.bytes.set(cursor++, p_spec.descriptor_flags);
	w32(built.bytes, cursor, metadata_size);
	cursor += 4;
	w32(built.bytes, cursor, static_cast<uint32_t>(p_spec.entry_tlv.size()));
	cursor += 4;

	cursor += static_cast<uint64_t>(p_spec.toc_entry_size) - V5_ENTRY_MIN; // trailing fixed fields
	cursor += metadata_size;
	if (p_spec.entry_tlv.size() > 0) {
		std::memcpy(built.bytes.ptrw() + cursor, p_spec.entry_tlv.ptr(), p_spec.entry_tlv.size());
		cursor += p_spec.entry_tlv.size();
	}

	if (is_texture) {
		built.mip_table_offset = cursor;
		w64(built.bytes, cursor, built.data_offset);
		w32(built.bytes, cursor + 8, payload_total);
		w32(built.bytes, cursor + 12, payload_total);
		w32(built.bytes, cursor + 16, MIP_ROW_PITCH);
		w32(built.bytes, cursor + 20, p_spec.chunk_count);
		cursor += sizeof(godot::DSPkgMipEntry);
		if (p_spec.chunk_count > 1) {
			built.chunk_table_offset = cursor;
			for (uint32_t i = 0; i < p_spec.chunk_count; ++i) {
				const uint64_t chunk_offset = built.data_offset + static_cast<uint64_t>(i) * PAYLOAD_ROW_BYTES;
				w64(built.bytes, cursor, chunk_offset);
				w32(built.bytes, cursor + 8, PAYLOAD_ROW_BYTES);
				w32(built.bytes, cursor + 12, PAYLOAD_ROW_BYTES);
				cursor += sizeof(godot::DSChunkEntry);
			}
		}
	}

	// Payload bytes are opaque to open(); use a stable pattern.
	for (uint64_t i = 0; i < payload_total; ++i) {
		built.bytes.set(static_cast<int64_t>(built.data_offset + i), static_cast<uint8_t>((i * 31 + 7) & 0xFF));
	}
	return built;
}

struct EncryptedV5Built {
	godot::PackedByteArray bytes;
	uint64_t data_offset = 0;
	uint64_t header_size = 0;
};

// Builds a v5 archive whose TOC is AEAD-encrypted. The plaintext TOC comes from
// build_v5() and the mip offset inside it is patched to the new payload start
// before encryption, so the AAD (the full header) and the TOC agree.
godot::PackedByteArray build_encrypted_v5(const godot::String &p_key_hex, bool p_v4_separator) {
	static const uint8_t kDomainV4[8] = { 'D', 'S', 'P', 'K', 'G', 'v', '4', 0x01 };
	static const uint8_t kDomainV5[8] = { 'D', 'S', 'P', 'K', 'G', 'v', '5', 0x01 };
	V5Spec spec;
	spec.header_size = 104;
	V5Built plain = build_v5(spec);
	const uint64_t header_size = spec.header_size;
	const uint64_t old_data_offset = plain.data_offset;
	const uint64_t old_toc_len = old_data_offset - header_size;
	const uint64_t mac_offset = header_size;
	const uint64_t cipher_offset = header_size + godot::DSPKG_ENCRYPTION_MAC_SIZE;
	const uint64_t new_data_offset = align_up_test(cipher_offset + old_toc_len, ALIGN_512);
	const uint64_t new_toc_len = new_data_offset - cipher_offset;
	const uint64_t payload_size = plain.bytes.size() - old_data_offset;

	const godot::PackedByteArray key_bytes = p_key_hex.hex_decode();
	godot::PackedByteArray toc_plain;
	toc_plain.resize(static_cast<int64_t>(new_toc_len));
	std::memset(toc_plain.ptrw(), 0, static_cast<size_t>(new_toc_len));
	std::memcpy(toc_plain.ptrw(), plain.bytes.ptr() + header_size, old_toc_len);
	// The mip offset lives in the TOC, not the header; patch the copy too.
	w64(toc_plain, plain.mip_table_offset - header_size, new_data_offset);

	// Patch the header data_offset and the mip offset before they are hashed.
	w64(plain.bytes, 12, new_data_offset);
	w64(plain.bytes, plain.mip_table_offset, new_data_offset);
	w16(plain.bytes, 6, godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_ENCRYPTED_TOC);
	for (int i = 0; i < 16; ++i) {
		plain.bytes.set(32 + i, static_cast<uint8_t>(0x20 + i));
	}
	uint8_t key_id_bytes[8] = { 0 };
	crypto_blake2b(key_id_bytes, 8, key_bytes.ptr(), 32);
	uint64_t key_id = 0;
	std::memcpy(&key_id, key_id_bytes, 8);
	w64(plain.bytes, 52, key_id);

	godot::PackedByteArray out;
	out.resize(static_cast<int64_t>(new_data_offset + payload_size));
	std::memset(out.ptrw(), 0, static_cast<size_t>(new_data_offset + payload_size));
	std::memcpy(out.ptrw(), plain.bytes.ptr(), header_size);

	uint8_t nonce[24] = { 0 };
	std::memcpy(nonce, plain.bytes.ptr() + 32, 16);
	std::memcpy(nonce + 16, p_v4_separator ? kDomainV4 : kDomainV5, 8);
	uint8_t mac[16] = { 0 };
	crypto_aead_lock(out.ptrw() + cipher_offset, mac, key_bytes.ptr(), nonce,
			out.ptr(), header_size, toc_plain.ptr(), new_toc_len);
	std::memcpy(out.ptrw() + mac_offset, mac, 16);
	std::memcpy(out.ptrw() + new_data_offset, plain.bytes.ptr() + old_data_offset, payload_size);
	return out;
}

struct GlobalKeyResetGuard {
	~GlobalKeyResetGuard() {
		godot::DSPkg::set_global_encryption_key(godot::String());
	}
};

struct V3Spec {
	uint8_t compression_format = 1; // GDEFLATE
	uint8_t gacl_transform = 0;
	uint32_t chunk_count = 1;
	bool set_chunked_capability = true;
	bool use_capability_override = false;
	uint64_t capability_override = 0;
	uint32_t pixel_format = static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	uint32_t logical_width = 0;
	uint32_t logical_height = 0;
	uint32_t depth_or_layers = 0;
	uint32_t reserved0 = 0;
};

godot::PackedByteArray build_v2(uint64_t p_header_reserved = 0, uint32_t p_mip_reserved = 0, uint16_t p_flags = 0) {
	const uint32_t uncompressed = PAYLOAD_ROW_BYTES;
	const uint64_t mip_table_end = sizeof(godot::DSTexHeader) + sizeof(godot::DSTexMipEntryV2);
	const uint64_t data_offset = align_up_test(mip_table_end, ALIGN_512);
	const uint64_t file_size = data_offset + uncompressed;

	godot::PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(file_size));
	std::memset(bytes.ptrw(), 0, static_cast<size_t>(file_size));

	w32(bytes, 0, 0x58455444u);
	w16(bytes, 4, 2);
	w16(bytes, 6, p_flags);
	w32(bytes, 8, 4);
	w32(bytes, 12, 1);
	w32(bytes, 16, static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM));
	w32(bytes, 20, 1);
	w64(bytes, 24, uncompressed);
	w64(bytes, 32, uncompressed);
	w64(bytes, 40, p_header_reserved);

	uint64_t cursor = sizeof(godot::DSTexHeader);
	w64(bytes, cursor, data_offset);
	w32(bytes, cursor + 8, uncompressed);
	w32(bytes, cursor + 12, uncompressed);
	w32(bytes, cursor + 16, MIP_ROW_PITCH);
	w32(bytes, cursor + 20, p_mip_reserved);

	for (uint64_t i = 0; i < uncompressed; ++i) {
		bytes.set(static_cast<int64_t>(data_offset + i), static_cast<uint8_t>((i * 17 + 3) & 0xFF));
	}
	return bytes;
}

godot::PackedByteArray build_v3(const V3Spec &p_spec) {
	const uint32_t uncompressed = PAYLOAD_ROW_BYTES * p_spec.chunk_count;
	uint64_t capabilities = static_cast<uint64_t>(p_spec.compression_format) |
			(static_cast<uint64_t>(p_spec.gacl_transform) << 8);
	if (p_spec.chunk_count > 1 && p_spec.set_chunked_capability) {
		capabilities |= godot::DSTEX_CAP_CHUNKED;
	}
	if (p_spec.use_capability_override) {
		capabilities = p_spec.capability_override;
	}

	const bool chunked = p_spec.chunk_count > 1;
	const uint64_t mip_table_end = sizeof(godot::DSTexHeaderV3) + sizeof(godot::DSTexMipEntryV2);
	const uint64_t chunk_table_end = mip_table_end +
			(chunked ? static_cast<uint64_t>(p_spec.chunk_count) * sizeof(godot::DSChunkEntry) : 0);
	const uint64_t data_offset = align_up_test(chunk_table_end, ALIGN_512);
	const uint64_t file_size = data_offset + uncompressed;

	godot::PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(file_size));
	std::memset(bytes.ptrw(), 0, static_cast<size_t>(file_size));

	w32(bytes, 0, 0x58455444u);
	w16(bytes, 4, 3);
	w16(bytes, 6, 0);
	w32(bytes, 8, 4);
	w32(bytes, 12, p_spec.chunk_count);
	w32(bytes, 16, p_spec.pixel_format);
	w32(bytes, 20, 1);
	w64(bytes, 24, uncompressed);
	w64(bytes, 32, uncompressed);
	w64(bytes, 40, capabilities);
	w32(bytes, 48, p_spec.logical_width);
	w32(bytes, 52, p_spec.logical_height);
	w32(bytes, 56, p_spec.depth_or_layers);
	w32(bytes, 60, p_spec.reserved0);

	uint64_t cursor = sizeof(godot::DSTexHeaderV3);
	w64(bytes, cursor, data_offset);
	w32(bytes, cursor + 8, uncompressed);
	w32(bytes, cursor + 12, uncompressed);
	w32(bytes, cursor + 16, MIP_ROW_PITCH);
	w32(bytes, cursor + 20, p_spec.chunk_count);
	cursor += sizeof(godot::DSTexMipEntryV2);

	if (chunked) {
		for (uint32_t i = 0; i < p_spec.chunk_count; ++i) {
			const uint64_t chunk_offset = data_offset + static_cast<uint64_t>(i) * PAYLOAD_ROW_BYTES;
			w64(bytes, cursor, chunk_offset);
			w32(bytes, cursor + 8, PAYLOAD_ROW_BYTES);
			w32(bytes, cursor + 12, PAYLOAD_ROW_BYTES);
			cursor += sizeof(godot::DSChunkEntry);
		}
	}

	for (uint64_t i = 0; i < uncompressed; ++i) {
		bytes.set(static_cast<int64_t>(data_offset + i), static_cast<uint8_t>((i * 17 + 3) & 0xFF));
	}
	return bytes;
}

// Two-mip v3 texture: mip 0 is chunked (4x2 = two rows), mip 1 is inline
// (4x1-ish after halving = one row). Used to prove the mixed chunked/inline
// ordering check, which the single-mip builder cannot reach.
godot::PackedByteArray build_v3_two_mip_mixed(bool p_overlap) {
	const uint64_t data_offset = ALIGN_512;
	const uint32_t total_payload = 768;
	godot::PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(data_offset + total_payload));
	std::memset(bytes.ptrw(), 0, static_cast<size_t>(data_offset + total_payload));

	w32(bytes, 0, 0x58455444u);
	w16(bytes, 4, 3);
	w16(bytes, 6, 0);
	w32(bytes, 8, 4);
	w32(bytes, 12, 2);
	w32(bytes, 16, static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM));
	w32(bytes, 20, 2);
	w64(bytes, 24, total_payload);
	w64(bytes, 32, total_payload);
	w64(bytes, 40, static_cast<uint64_t>(godot::DSCompressionFormat::GDEFLATE) | godot::DSTEX_CAP_CHUNKED);
	w32(bytes, 48, 0); // logical_width
	w32(bytes, 52, 0); // logical_height
	w32(bytes, 56, 0); // depth_or_layers
	w32(bytes, 60, 0); // reserved0

	uint64_t cursor = sizeof(godot::DSTexHeaderV3);
	// mip 0: chunked, 4x2 -> 512 bytes.
	w64(bytes, cursor, data_offset);
	w32(bytes, cursor + 8, 512);
	w32(bytes, cursor + 12, 512);
	w32(bytes, cursor + 16, MIP_ROW_PITCH);
	w32(bytes, cursor + 20, 2);
	cursor += sizeof(godot::DSTexMipEntryV2);
	// mip 1: inline, 2x1 -> 256 bytes. Overlap it with chunk 1 when requested.
	w64(bytes, cursor, p_overlap ? 768 : 1024);
	w32(bytes, cursor + 8, 256);
	w32(bytes, cursor + 12, 256);
	w32(bytes, cursor + 16, MIP_ROW_PITCH);
	w32(bytes, cursor + 20, 1);
	cursor += sizeof(godot::DSTexMipEntryV2);

	// Chunk table for mip 0.
	w64(bytes, cursor, data_offset);
	w32(bytes, cursor + 8, PAYLOAD_ROW_BYTES);
	w32(bytes, cursor + 12, PAYLOAD_ROW_BYTES);
	cursor += sizeof(godot::DSChunkEntry);
	w64(bytes, cursor, data_offset + PAYLOAD_ROW_BYTES);
	w32(bytes, cursor + 8, PAYLOAD_ROW_BYTES);
	w32(bytes, cursor + 12, PAYLOAD_ROW_BYTES);

	for (uint64_t i = 0; i < total_payload; ++i) {
		bytes.set(static_cast<int64_t>(data_offset + i), static_cast<uint8_t>((i * 17 + 3) & 0xFF));
	}
	return bytes;
}

godot::Ref<godot::DSPkg> open_v5(const godot::String &p_path) {
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	if (pkg.is_valid()) {
		pkg->open(p_path, 0);
	}
	return pkg;
}

godot::Ref<godot::DSTex> make_dstex_reader() {
	godot::Ref<godot::DSTex> dstex;
	dstex.instantiate();
	return dstex;
}

godot::PackedByteArray make_tlv(uint32_t p_tag, const godot::PackedByteArray &p_payload) {
	godot::PackedByteArray tlv;
	const uint64_t record = 8 + p_payload.size();
	const uint64_t padded = align_up_test(record, 8);
	tlv.resize(static_cast<int64_t>(padded));
	std::memset(tlv.ptrw(), 0, static_cast<size_t>(padded));
	w32(tlv, 0, p_tag);
	w32(tlv, 4, static_cast<uint32_t>(p_payload.size()));
	if (p_payload.size() > 0) {
		std::memcpy(tlv.ptrw() + 8, p_payload.ptr(), p_payload.size());
	}
	return tlv;
}

// The largest legal table of the duplicating shape: one zero-payload record per
// distinct tag. A per-record scan over the tags already seen costs the
// triangular number of comparisons on exactly this input.
godot::PackedByteArray make_distinct_tag_table(uint32_t p_count) {
	godot::PackedByteArray table;
	const uint64_t bytes = static_cast<uint64_t>(p_count) * godot::DS_TLV_HEADER_SIZE;
	table.resize(static_cast<int64_t>(bytes));
	std::memset(table.ptrw(), 0, static_cast<size_t>(bytes));
	for (uint32_t i = 0; i < p_count; ++i) {
		w32(table, static_cast<uint64_t>(i) * godot::DS_TLV_HEADER_SIZE, TLV_TAG_BASE + i);
	}
	return table;
}

godot::String scratch_path(const char *p_name) {
	return godot::String("user://v5v3_guard_") + p_name;
}
struct ProtectedSettingGuard {
	godot::ProjectSettings *settings = nullptr;
	godot::String name = "dsgd_extension/protected";
	bool had = false;
	godot::Variant previous;

	ProtectedSettingGuard() {
		settings = godot::ProjectSettings::get_singleton();
		if (settings != nullptr) {
			had = settings->has_setting(name);
			if (had) {
				previous = settings->get_setting(name);
			}
		}
	}
	~ProtectedSettingGuard() {
		if (settings == nullptr) {
			return;
		}
		if (had) {
			settings->set_setting(name, previous);
		} else {
			settings->clear(name);
		}
	}
};

} // namespace

// --- Layout / ABI -----------------------------------------------------------

GDEX_TEST(dspkg_v5, header_layout_is_96_bytes) {
	GDEX_EXPECT_EQ(sizeof(godot::DSPkgHeaderV5), 96u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, header_size), 64u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, toc_entry_size), 66u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, capabilities), 68u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, extension_offset), 76u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, extension_size), 84u);
	GDEX_EXPECT_EQ(offsetof(godot::DSPkgHeaderV5, reserved2), 88u);
}

GDEX_TEST(dspkg_v5, entry_and_chunk_sizes) {
	GDEX_EXPECT_EQ(sizeof(godot::DSPkgMipEntry), 24u);
	GDEX_EXPECT_EQ(sizeof(godot::DSChunkEntry), 16u);
	GDEX_EXPECT_EQ(godot::DSPKG_FIXED_ENTRY_SIZE_V5, 66u);
	GDEX_EXPECT_EQ(godot::DSPKG_ENTRY_FIELDS_SIZE_V5, 56u);
}

GDEX_TEST(dstex_v3, capability_word_layout) {
	GDEX_EXPECT_EQ(sizeof(godot::DSTexHeader), 48u);
	GDEX_EXPECT_EQ(sizeof(godot::DSTexMipEntryV2), 24u);
	const uint64_t caps = static_cast<uint64_t>(godot::DSCompressionFormat::ZSTD) |
			(static_cast<uint64_t>(godot::DSGaclTransform::BC5) << 8) | godot::DSTEX_CAP_CHUNKED;
	GDEX_EXPECT_EQ(static_cast<int>(godot::DSTex::get_compression_format(caps)), static_cast<int>(godot::DSCompressionFormat::ZSTD));
	GDEX_EXPECT_EQ(static_cast<int>(godot::DSTex::get_gacl_transform(caps)), static_cast<int>(godot::DSGaclTransform::BC5));
	GDEX_EXPECT_TRUE(godot::DSTex::is_chunked(caps));
}

// --- DSPkg v5 positive cases ------------------------------------------------

GDEX_TEST_T(dspkg_v5, minimal_none_entry_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("min_none.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	GDEX_EXPECT_TRUE(pkg->has_texture(spec.path));
	godot::Dictionary info = pkg->get_entry_info(spec.path);
	GDEX_EXPECT_EQ((int)info["format_version"], 5);
	GDEX_EXPECT_EQ((int)info["compression_format"], (int)godot::DSCompressionFormat::NONE);
	GDEX_EXPECT_EQ((int)info["entry_kind"], 0);
	GDEX_EXPECT_EQ((int64_t)info["data_offset"], (int64_t)built.data_offset);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, gdeflate_entry_opens_and_lists_mip, TAG_INTEGRATION) {
	const godot::String path = scratch_path("min_gdeflate.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	godot::Vector<godot::DSPkgMipEntry> mips;
	GDEX_EXPECT_TRUE(pkg->get_mip_entries_for_path(spec.path, mips));
	GDEX_ASSERT_EQ(mips.size(), 1);
	GDEX_EXPECT_EQ(mips[0].offset, built.data_offset);
	GDEX_EXPECT_EQ(mips[0].chunk_count, 1u);
	godot::Dictionary payload = pkg->read_texture(spec.path);
	GDEX_ASSERT_FALSE(payload.is_empty());
	GDEX_EXPECT_EQ((int64_t)payload["compressed_size"], 256);
	godot::Array mip_data = payload["mip_data"];
	GDEX_ASSERT_EQ(mip_data.size(), 1);
	godot::PackedByteArray mip0 = mip_data[0];
	GDEX_EXPECT_EQ(mip0.size(), 256);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, header_size_104_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("header_104.dspkg");
	V5Spec spec;
	spec.header_size = 104;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, valid_header_tlv_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("header_tlv.dspkg");
	V5Spec spec;
	const godot::PackedByteArray payload;
	spec.header_tlv = make_tlv(godot::DS_TLV_TAG_PROVENANCE, payload);
	spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	remove_file(path);
}

// A table's validation cost must track its byte size. Two tables of the same
// shape, 16x apart in records: a per-record scan over the tags already seen
// costs roughly n(n-1)/2 comparisons, so the large table costs about 250x the
// small one, while linear validation costs about the 16x byte ratio.
GDEX_TEST_T(dspkg_v5, header_tlv_cost_stays_linear_at_table_cap, TAG_INTEGRATION) {
	const godot::String big_path = scratch_path("header_tlv_1mib.dspkg");
	const godot::String small_path = scratch_path("header_tlv_64kib.dspkg");

	V5Spec big_spec;
	big_spec.header_tlv = make_distinct_tag_table(HEADER_TLV_RECORD_COUNT);
	big_spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built big = build_v5(big_spec);

	V5Spec small_spec;
	small_spec.header_tlv = make_distinct_tag_table(HEADER_TLV_SMALL_RECORD_COUNT);
	small_spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built small_archive = build_v5(small_spec);

	GDEX_ASSERT_TRUE(write_bytes(big_path, big.bytes));
	GDEX_ASSERT_TRUE(write_bytes(small_path, small_archive.bytes));

	auto measure_open_us = [](const godot::String &p_path, godot::Error &r_err, int32_t &r_entry_count) -> int64_t {
		godot::Ref<godot::DSPkg> pkg;
		pkg.instantiate();
		const int64_t start = godot::Time::get_singleton()->get_ticks_usec();
		r_err = pkg->open(p_path, 0);
		const int64_t elapsed = godot::Time::get_singleton()->get_ticks_usec() - start;
		r_entry_count = (r_err == godot::OK) ? pkg->get_entry_count() : 0;
		return elapsed;
	};

	// Warm OS file cache and take median of 5 runs so CI VM I/O jitter doesn't
	// trip the linear-vs-quadratic ratio guard.
	godot::Error big_err = godot::OK;
	godot::Error small_err = godot::OK;
	int32_t big_count = 0;
	int32_t small_count = 0;
	measure_open_us(big_path, big_err, big_count);
	measure_open_us(small_path, small_err, small_count);

	std::array<int64_t, 5> big_samples{};
	std::array<int64_t, 5> small_samples{};
	for (size_t i = 0; i < big_samples.size(); ++i) {
		big_samples[i] = measure_open_us(big_path, big_err, big_count);
		small_samples[i] = measure_open_us(small_path, small_err, small_count);
	}
	std::sort(big_samples.begin(), big_samples.end());
	std::sort(small_samples.begin(), small_samples.end());
	const int64_t big_us = big_samples[big_samples.size() / 2];
	const int64_t small_us = small_samples[small_samples.size() / 2];

	remove_file(big_path);
	remove_file(small_path);

	GDEX_ASSERT_EQ(static_cast<int64_t>(big_err), static_cast<int64_t>(godot::OK));
	GDEX_ASSERT_EQ(static_cast<int64_t>(small_err), static_cast<int64_t>(godot::OK));
	GDEX_EXPECT_EQ(big_count, 1);
	GDEX_EXPECT_LT(big_us, static_cast<int64_t>(HEADER_TLV_TABLE_BUDGET_US));
	const int64_t small_floor_us = small_us > HEADER_TLV_TIMING_FLOOR_US ? small_us : HEADER_TLV_TIMING_FLOOR_US;
	GDEX_EXPECT_LT(big_us, static_cast<int64_t>(HEADER_TLV_TABLE_RATIO) * small_floor_us);
}

// Guard, not closure: a full 64 KiB entry table costs milliseconds, too little
// to bound reliably, but it pins the entry call site and its accepted set.
GDEX_TEST_T(dspkg_v5, entry_tlv_full_table_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("entry_tlv_64kib.dspkg");
	V5Spec spec;
	spec.entry_tlv = make_distinct_tag_table(ENTRY_TLV_RECORD_COUNT);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));

	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	const godot::Error err = pkg->open(path, 0);
	remove_file(path);

	GDEX_ASSERT_EQ(static_cast<int64_t>(err), static_cast<int64_t>(godot::OK));
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
}

GDEX_TEST_T(dspkg_v5, zstd_archive_opens_and_read_texture_fails_closed, TAG_INTEGRATION) {
	const godot::String path = scratch_path("zstd.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::ZSTD);
	spec.capabilities = godot::DSPKG_CAP_ZSTD;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	GDEX_EXPECT_EQ((int)pkg->get_entry_info(spec.path)["compression_format"],
			(int)godot::DSCompressionFormat::ZSTD);
	GDEX_EXPECT_TRUE(pkg->read_texture(spec.path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunked_archive_reports_chunks_and_cpu_path_fails_closed, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunked.dspkg");
	V5Spec spec;
	spec.chunk_count = 4;
	spec.capabilities = godot::DSPKG_CAP_CHUNKED;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	godot::Dictionary info = pkg->get_entry_info(spec.path);
	GDEX_EXPECT_TRUE((bool)info["chunked"]);
	GDEX_EXPECT_EQ((int64_t)info["total_chunk_count"], 4);
	godot::PackedInt32Array chunk_counts = info["mip_chunk_counts"];
	GDEX_EXPECT_EQ(chunk_counts.size(), 1);
	GDEX_EXPECT_TRUE(pkg->read_texture(spec.path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, mesh_entry_with_capability_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mesh.dspkg");
	V5Spec spec;
	spec.path = "res://m.mesh";
	spec.entry_kind = static_cast<uint8_t>(godot::DSPkgEntryKind::MESH);
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	spec.capabilities = godot::DSPKG_CAP_MESH;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	GDEX_EXPECT_EQ((int)pkg->get_entry_info(spec.path)["entry_kind"],
			(int)godot::DSPkgEntryKind::MESH);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, audio_entry_opens_and_is_not_a_texture, TAG_INTEGRATION) {
	const godot::String path = scratch_path("audio.dspkg");
	V5Spec spec;
	spec.path = "res://a.ogg";
	spec.entry_kind = static_cast<uint8_t>(godot::DSPkgEntryKind::AUDIO);
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg = open_v5(path);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	GDEX_EXPECT_EQ((int)pkg->get_entry_info(spec.path)["entry_kind"],
			(int)godot::DSPkgEntryKind::AUDIO);
	GDEX_EXPECT_TRUE(pkg->read_texture(spec.path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, audio_entry_requires_none_compression, TAG_INTEGRATION) {
	const godot::String path = scratch_path("audio_codec.dspkg");
	V5Spec spec;
	spec.path = "res://a.ogg";
	spec.entry_kind = static_cast<uint8_t>(godot::DSPkgEntryKind::AUDIO);
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::GDEFLATE);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

// --- DSPkg v5 forged headers ------------------------------------------------

GDEX_TEST_T(dspkg_v5, unknown_capability_bit_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_cap.dspkg");
	V5Spec spec;
	spec.capabilities = 1ULL << 63;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, unknown_entry_kind_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_kind.dspkg");
	V5Spec spec;
	spec.entry_kind = 4;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, unknown_compression_format_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_codec.dspkg");
	V5Spec spec;
	spec.compression_format = 3;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, unknown_gacl_transform_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_gacl.dspkg");
	V5Spec spec;
	spec.gacl_transform = 5;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, descriptor_reserved_bits_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_desc.dspkg");
	V5Spec spec;
	spec.descriptor_flags = 0x02;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, min_reader_below_5_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("min_reader_low.dspkg");
	V5Spec spec;
	spec.min_reader_version = 4;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, min_reader_above_5_is_unrecognized, TAG_INTEGRATION) {
	const godot::String path = scratch_path("min_reader_high.dspkg");
	V5Spec spec;
	spec.min_reader_version = 6;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, header_size_misaligned_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("header_97.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w16(built.bytes, 64, 97);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, toc_entry_size_too_small_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("toc_entry_60.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w16(built.bytes, 66, 60);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, data_offset_not_512_aligned_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("data_offset_513.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w64(built.bytes, 12, built.data_offset + 1);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, unencrypted_nonzero_salt_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("salt.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	built.bytes.set(32, 0x7F);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, extension_flag_without_table_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("ext_flag.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w16(built.bytes, 6, godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, extension_offset_inside_entries_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("ext_inside.dspkg");
	V5Spec spec;
	spec.header_tlv = make_tlv(godot::DS_TLV_TAG_PROVENANCE, godot::PackedByteArray());
	spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built built = build_v5(spec);
	w64(built.bytes, 76, built.extension_offset - 16);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, tlv_tag_zero_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("tlv_zero.dspkg");
	V5Spec spec;
	spec.header_tlv = make_tlv(0, godot::PackedByteArray());
	spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, duplicate_tlv_tag_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("tlv_dup.dspkg");
	V5Spec spec;
	godot::PackedByteArray tlv = make_tlv(godot::DS_TLV_TAG_PROVENANCE, godot::PackedByteArray());
	tlv.append_array(make_tlv(godot::DS_TLV_TAG_PROVENANCE, godot::PackedByteArray()));
	spec.header_tlv = tlv;
	spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, nonzero_tlv_padding_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("tlv_pad.dspkg");
	V5Spec spec;
	godot::PackedByteArray payload;
	payload.resize(1);
	payload.set(0, 0x11);
	godot::PackedByteArray tlv = make_tlv(godot::DS_TLV_TAG_PROVENANCE, payload);
	tlv.set(tlv.size() - 1, 0x01); // last padding byte
	spec.header_tlv = tlv;
	spec.header_flags = godot::DSPKG_FLAG_GPU_MIP_METADATA | godot::DSPKG_FLAG_EXTENSION_TABLE;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, capability_zstd_without_zstd_entry_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("cap_zstd.dspkg");
	V5Spec spec;
	spec.capabilities = godot::DSPKG_CAP_ZSTD;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, zstd_entry_without_capability_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("zstd_nocap.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::ZSTD);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunked_without_capability_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_nocap.dspkg");
	V5Spec spec;
	spec.chunk_count = 2;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, none_entry_size_mismatch_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("none_size.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	V5Built built = build_v5(spec);
	// entry compressed_size is the third fixed u64 after the prefix/path.
	w64(built.bytes, built.entry_fixed_offset + 8, 512);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, texture_reserved_flags_are_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("entry_flags.dspkg");
	V5Spec spec;
	spec.entry_flags = 4;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunk_count_zero_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_zero.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w32(built.bytes, built.mip_table_offset + 20, 0);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunk_sum_mismatch_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_sum.dspkg");
	V5Spec spec;
	spec.chunk_count = 2;
	spec.capabilities = godot::DSPKG_CAP_CHUNKED;
	V5Built built = build_v5(spec);
	w32(built.bytes, built.chunk_table_offset + 16 + 8, 128); // second chunk compressed
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunk_row_misalignment_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_row.dspkg");
	V5Spec spec;
	spec.chunk_count = 2;
	spec.capabilities = godot::DSPKG_CAP_CHUNKED;
	V5Built built = build_v5(spec);
	w32(built.bytes, built.chunk_table_offset + 16 + 12, 300);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, chunk_overlap_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_overlap.dspkg");
	V5Spec spec;
	spec.chunk_count = 2;
	spec.capabilities = godot::DSPKG_CAP_CHUNKED;
	V5Built built = build_v5(spec);
	w64(built.bytes, built.chunk_table_offset + 16, built.data_offset);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, mip_offset_must_be_512_aligned, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mip_align.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	w64(built.bytes, built.mip_table_offset, built.data_offset + 8);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, mesh_without_capability_is_corrupt, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mesh_nocap.dspkg");
	V5Spec spec;
	spec.path = "res://m.mesh";
	spec.entry_kind = static_cast<uint8_t>(godot::DSPkgEntryKind::MESH);
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	spec.capabilities = 0;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, toc_plausible_bound_rejects_before_allocating, TAG_INTEGRATION) {
	const godot::String path = scratch_path("toc_bound.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	const uint64_t forged_data_offset = 3ULL * 1024ULL * 1024ULL;
	built.bytes.resize(static_cast<int64_t>(forged_data_offset + 512));
	std::memset(built.bytes.ptrw() + forged_data_offset, 0, 512);
	w64(built.bytes, 12, forged_data_offset);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_CORRUPT);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, protected_build_rejects_plaintext_v5, TAG_INTEGRATION) {
	const godot::String path = scratch_path("protected_v5.dspkg");
	V5Spec spec;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	ProtectedSettingGuard guard;
	GDEX_ASSERT_TRUE(guard.settings != nullptr);
	guard.settings->set_setting(guard.name, true);
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_UNAUTHORIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, protected_build_rejects_pre_v4_as_unrecognized, TAG_INTEGRATION) {
	const godot::String path = scratch_path("protected_v3.dspkg");
	V5Built built = build_v5(V5Spec());
	w16(built.bytes, 4, 3);
	// v3 has a 28-byte header, so truncate to the real header + payload.
	godot::PackedByteArray legacy;
	const uint64_t data_offset = built.data_offset;
	const uint64_t payload_size = 256;
	legacy.resize(static_cast<int64_t>(data_offset + payload_size));
	std::memset(legacy.ptrw(), 0, static_cast<size_t>(legacy.size()));
	w32(legacy, 0, 0x474B5044u);
	w16(legacy, 4, 3);
	w16(legacy, 6, godot::DSPKG_FLAG_GPU_MIP_METADATA);
	w32(legacy, 8, 0);
	w64(legacy, 12, data_offset);
	GDEX_ASSERT_TRUE(write_bytes(path, legacy));
	ProtectedSettingGuard guard;
	GDEX_ASSERT_TRUE(guard.settings != nullptr);
	guard.settings->set_setting(guard.name, true);
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_FILE_UNRECOGNIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, encrypted_v5_opens, TAG_INTEGRATION) {
	const godot::String path = scratch_path("encrypted_ok.dspkg");
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	GlobalKeyResetGuard key_guard;
	GDEX_ASSERT_TRUE(godot::DSPkg::set_global_encryption_key(key_hex) == godot::OK);
	GDEX_ASSERT_TRUE(write_bytes(path, build_encrypted_v5(key_hex, false)));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::OK);
	GDEX_EXPECT_EQ(pkg->get_entry_count(), 1);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, encrypted_v5_extended_aad_tamper_is_unauthorized, TAG_INTEGRATION) {
	const godot::String path = scratch_path("encrypted_aad.dspkg");
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	GlobalKeyResetGuard key_guard;
	GDEX_ASSERT_TRUE(godot::DSPkg::set_global_encryption_key(key_hex) == godot::OK);
	godot::PackedByteArray bytes = build_encrypted_v5(key_hex, false);
	// Byte 100 is inside header_size = 104 but outside the 96-byte struct: it is
	// AAD-only, so only the MAC can reject it.
	bytes.set(100, static_cast<uint8_t>(bytes[100] ^ 0xFF));
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_UNAUTHORIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, encrypted_v5_semantic_field_tamper_is_unauthorized, TAG_INTEGRATION) {
	const godot::String path = scratch_path("encrypted_semantic.dspkg");
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	GlobalKeyResetGuard key_guard;
	GDEX_ASSERT_TRUE(godot::DSPkg::set_global_encryption_key(key_hex) == godot::OK);
	godot::PackedByteArray bytes = build_encrypted_v5(key_hex, false);
	// Flip a capabilities bit. If capabilities were validated before the MAC,
	// the old code returned ERR_FILE_UNRECOGNIZED here instead of unauthorized.
	bytes.set(68, static_cast<uint8_t>(bytes[68] ^ 0x01));
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_UNAUTHORIZED);
	remove_file(path);
}

GDEX_TEST_T(dspkg_v5, encrypted_v5_with_v4_separator_is_unauthorized, TAG_INTEGRATION) {
	const godot::String path = scratch_path("encrypted_v4sep.dspkg");
	const godot::String key_hex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	GlobalKeyResetGuard key_guard;
	GDEX_ASSERT_TRUE(godot::DSPkg::set_global_encryption_key(key_hex) == godot::OK);
	GDEX_ASSERT_TRUE(write_bytes(path, build_encrypted_v5(key_hex, true)));
	godot::Ref<godot::DSPkg> pkg;
	pkg.instantiate();
	GDEX_EXPECT_EQ(pkg->open(path, 0), godot::ERR_UNAUTHORIZED);
	remove_file(path);
}

// --- DSTex v3 positive cases -----------------------------------------------

GDEX_TEST_T(dstex_v3, minimal_gdeflate_header_accepts, TAG_INTEGRATION) {
	const godot::String path = scratch_path("min_v3.dstex");
	V3Spec spec;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	godot::Dictionary header = dstex->read_header(path);
	GDEX_ASSERT_FALSE(header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int>(header["version"]), 3);
	GDEX_EXPECT_EQ(static_cast<int>(header["compression_format"]), static_cast<int>(godot::DSCompressionFormat::GDEFLATE));
	GDEX_EXPECT_EQ(static_cast<godot::PackedInt32Array>(header["mip_chunk_counts"]).size(), 1);
	GDEX_EXPECT_FALSE(dstex->read_from_file(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, chunked_64_rows_header_accepts, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunked_v3.dstex");
	V3Spec spec;
	spec.chunk_count = 64;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	godot::Dictionary header = dstex->read_header(path);
	GDEX_ASSERT_FALSE(header.is_empty());
	GDEX_EXPECT_TRUE((bool)header["chunked"]);
	godot::PackedInt32Array counts = header["mip_chunk_counts"];
	GDEX_ASSERT_EQ(counts.size(), 1);
	GDEX_EXPECT_EQ(counts[0], 64);
	GDEX_EXPECT_TRUE(dstex->read_from_file(path).is_empty()); // CPU path fails closed
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, zstd_header_accepts_but_cpu_read_fails_closed, TAG_INTEGRATION) {
	const godot::String path = scratch_path("zstd_v3.dstex");
	V3Spec spec;
	spec.use_capability_override = true;
	spec.capability_override = static_cast<uint64_t>(godot::DSCompressionFormat::ZSTD);
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	godot::Dictionary header = dstex->read_header(path);
	GDEX_EXPECT_FALSE(header.is_empty());
	GDEX_EXPECT_EQ((int)header["compression_format"], (int)godot::DSCompressionFormat::ZSTD);
	GDEX_EXPECT_TRUE(dstex->read_from_file(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, unknown_capability_bit_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_cap_v3.dstex");
	V3Spec spec;
	spec.use_capability_override = true;
	spec.capability_override = 1ULL << 40;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, unknown_compression_format_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_codec_v3.dstex");
	V3Spec spec;
	spec.use_capability_override = true;
	spec.capability_override = 3;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, unknown_gacl_transform_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_gacl_v3.dstex");
	V3Spec spec;
	spec.use_capability_override = true;
	spec.capability_override = 1 | (5ULL << 8);
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, gacl_family_mismatch_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("gacl_family_v3.dstex");
	V3Spec spec;
	// BC3 transform on an RGBA8 texture.
	spec.use_capability_override = true;
	spec.capability_override = 1 | (static_cast<uint64_t>(godot::DSGaclTransform::BC3) << 8);
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, cap_chunked_without_chunked_mip_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("cap_chunk_v3.dstex");
	V3Spec spec;
	spec.chunk_count = 1;
	spec.use_capability_override = true;
	spec.capability_override = 1 | godot::DSTEX_CAP_CHUNKED;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, chunked_mip_without_capability_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("no_chunk_cap_v3.dstex");
	V3Spec spec;
	spec.chunk_count = 2;
	spec.set_chunked_capability = false;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, v2_nonzero_header_reserved_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("v2_reserved_header.dstex");
	godot::PackedByteArray bytes = build_v2(1, 0); // v2 reserved header word = 1
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, v2_nonzero_mip_reserved_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("v2_reserved_mip.dstex");
	godot::PackedByteArray bytes = build_v2(0, 1); // v2 mip reserved = 1
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, chunk_row_alignment_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_row_v3.dstex");
	V3Spec spec;
	spec.chunk_count = 2;
	godot::PackedByteArray bytes = build_v3(spec);
	const uint64_t chunk_table = sizeof(godot::DSTexHeaderV3) + sizeof(godot::DSTexMipEntryV2);
	w32(bytes, chunk_table + 16 + 12, 300);
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, chunk_sum_mismatch_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("chunk_sum_v3.dstex");
	V3Spec spec;
	spec.chunk_count = 2;
	godot::PackedByteArray bytes = build_v3(spec);
	const uint64_t chunk_table = sizeof(godot::DSTexHeaderV3) + sizeof(godot::DSTexMipEntryV2);
	w32(bytes, chunk_table + 16 + 8, 128);
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, mip_offset_not_512_aligned_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mip_align_v3.dstex");
	godot::PackedByteArray bytes = build_v3(V3Spec());
	uint64_t original_offset = 0;
	std::memcpy(&original_offset, bytes.ptr() + sizeof(godot::DSTexHeaderV3), sizeof(original_offset));
	w64(bytes, sizeof(godot::DSTexHeaderV3), original_offset + 8);
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, mip_offset_inside_tables_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mip_inside_tables.dstex");
	godot::PackedByteArray bytes = build_v3(V3Spec());
	w64(bytes, sizeof(godot::DSTexHeaderV3), 0);
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, mixed_chunked_unchunked_payload_accepted, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mixed_ok.dstex");
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3_two_mip_mixed(false)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_FALSE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, mixed_chunked_unchunked_overlap_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("mixed_overlap.dstex");
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3_two_mip_mixed(true)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

// --- Win32 backend container parser guards --------------------------------

GDEX_TEST_T(win32_dstex_mip, v3_chunked_row_pitch_zero_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_row_zero.dstex");
	V3Spec spec;
	spec.chunk_count = 2;
	godot::PackedByteArray bytes = build_v3(spec);
	w32(bytes, sizeof(godot::DSTexHeaderV3) + 16, 0); // mip[0].row_pitch
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v3_unchunked_row_pitch_zero_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_row_zero_unchunked.dstex");
	godot::PackedByteArray bytes = build_v3(V3Spec());
	w32(bytes, sizeof(godot::DSTexHeaderV3) + 16, 0);
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v3_gdeflate_accepted, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v3_ok.dstex");
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(V3Spec())));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_TRUE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	GDEX_EXPECT_EQ(entries.size(), 1);
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, container_codec_matches_header_reader, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	// #0257 added a second codec derivation: the backend's container parser now
	// reports the stored format so enqueue_request() can refuse a caller/codec
	// disagreement. If that derivation drifts from read_header(), the guard
	// either rejects valid assets or lets an invalid request through (the #0252
	// TDR). Pin both directions here, headlessly.
	godot::Ref<godot::DSTex> reader = make_dstex_reader();
	godot::Win32Backend backend;

	V3Spec packed;
	const godot::String packed_path = scratch_path("be_codec_packed.dstex");
	GDEX_ASSERT_TRUE(write_bytes(packed_path, build_v3(packed)));
	godot::Vector<godot::DSTexMipEntryV2> entries;
	godot::DSCompressionFormat backend_format = godot::DSCompressionFormat::NONE;
	GDEX_EXPECT_TRUE(backend._debug_get_texture_mip_entries_for_testing(packed_path, 0, entries, &backend_format));
	GDEX_EXPECT_EQ(static_cast<int>(backend_format), static_cast<int>(godot::DSCompressionFormat::GDEFLATE));
	GDEX_EXPECT_EQ(static_cast<int>(backend_format), (int)reader->read_header(packed_path)["compression_format"]);

	V3Spec raw;
	raw.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	const godot::String raw_path = scratch_path("be_codec_none.dstex");
	GDEX_ASSERT_TRUE(write_bytes(raw_path, build_v3(raw)));
	entries.clear();
	backend_format = godot::DSCompressionFormat::GDEFLATE;
	GDEX_EXPECT_TRUE(backend._debug_get_texture_mip_entries_for_testing(raw_path, 0, entries, &backend_format));
	GDEX_EXPECT_EQ(static_cast<int>(backend_format), static_cast<int>(godot::DSCompressionFormat::NONE));
	GDEX_EXPECT_EQ(static_cast<int>(backend_format), (int)reader->read_header(raw_path)["compression_format"]);

	remove_file(packed_path);
	remove_file(raw_path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v3_chunked_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v3_chunked.dstex");
	V3Spec spec;
	spec.chunk_count = 2;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v3_zstd_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v3_zstd.dstex");
	V3Spec spec;
	spec.use_capability_override = true;
	spec.capability_override = static_cast<uint64_t>(godot::DSCompressionFormat::ZSTD);
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v3_gacl_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v3_gacl.dstex");
	V3Spec spec;
	spec.pixel_format = static_cast<uint32_t>(godot::RenderingDevice::DATA_FORMAT_BC3_UNORM_BLOCK);
	spec.use_capability_override = true;
	spec.capability_override = static_cast<uint64_t>(godot::DSCompressionFormat::GDEFLATE) |
			(static_cast<uint64_t>(godot::DSGaclTransform::BC3) << 8);
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v2_reserved_flags_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v2_flags.dstex");
	godot::PackedByteArray bytes = build_v2(0, 0, 4); // flag 4 = reserved
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v2_reserved_mip_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v2_mip_reserved.dstex");
	godot::PackedByteArray bytes = build_v2(0, 1, 0); // mip reserved = 1
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dstex_mip, v2_footprint_mismatch_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v2_footprint.dstex");
	godot::PackedByteArray bytes = build_v2(0, 0, 0);
	w64(bytes, 24, 512); // header original_size
	w32(bytes, sizeof(godot::DSTexHeader) + 12, 512); // mip uncompressed_size
	GDEX_ASSERT_TRUE(write_bytes(path, bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(dstex_v3, logical_dimensions_parsed, TAG_INTEGRATION) {
	const godot::String path = scratch_path("logical_dims_v3.dstex");
	V3Spec spec;
	spec.logical_width = 3;
	spec.logical_height = 1;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	godot::Dictionary header = dstex->read_header(path);
	GDEX_ASSERT_FALSE(header.is_empty());
	GDEX_EXPECT_EQ(static_cast<int>(header["width"]), 4);
	GDEX_EXPECT_EQ(static_cast<int>(header["height"]), 1);
	GDEX_EXPECT_EQ(static_cast<int>(header["logical_width"]), 3);
	GDEX_EXPECT_EQ(static_cast<int>(header["logical_height"]), 1);
	GDEX_EXPECT_EQ(static_cast<int>(header["depth_or_layers"]), 1);
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, logical_width_exceeds_storage_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_log_w_v3.dstex");
	V3Spec spec;
	spec.logical_width = 5; // storage width is 4
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, logical_height_exceeds_storage_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_log_h_v3.dstex");
	V3Spec spec;
	spec.logical_height = 2; // storage height is 1
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, reserved0_nonzero_rejected, TAG_INTEGRATION) {
	const godot::String path = scratch_path("bad_res0_v3.dstex");
	V3Spec spec;
	spec.reserved0 = 1;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Ref<godot::DSTex> dstex = make_dstex_reader();
	GDEX_EXPECT_TRUE(dstex->read_header(path).is_empty());
	remove_file(path);
}

GDEX_TEST_T(dstex_v3, depth_or_layers_greater_than_1_fails_closed, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("layers_v3.dstex");
	V3Spec spec;
	spec.depth_or_layers = 4;
	GDEX_ASSERT_TRUE(write_bytes(path, build_v3(spec)));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, 0, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dspkg_mip, gdeflate_accepted, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v5_ok.dspkg");
	V5Built built = build_v5(V5Spec());
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_TRUE(backend._debug_get_texture_mip_entries_for_testing(path, built.data_offset, entries));
	GDEX_EXPECT_EQ(entries.size(), 1);
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dspkg_mip, zstd_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v5_zstd.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::ZSTD);
	spec.capabilities = godot::DSPKG_CAP_ZSTD;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, built.data_offset, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dspkg_mip, chunked_rejected, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v5_chunked.dspkg");
	V5Spec spec;
	spec.chunk_count = 2;
	spec.capabilities = godot::DSPKG_CAP_CHUNKED;
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	GDEX_EXPECT_FALSE(backend._debug_get_texture_mip_entries_for_testing(path, built.data_offset, entries));
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}

GDEX_TEST_T(win32_dspkg_mip, none_accepted, TAG_INTEGRATION) {
#if defined(_WIN32) && (defined(DEBUG_ENABLED) || defined(DEV_ENABLED))
	const godot::String path = scratch_path("be_v5_none.dspkg");
	V5Spec spec;
	spec.compression_format = static_cast<uint8_t>(godot::DSCompressionFormat::NONE);
	V5Built built = build_v5(spec);
	GDEX_ASSERT_TRUE(write_bytes(path, built.bytes));
	godot::Win32Backend backend;
	godot::Vector<godot::DSTexMipEntryV2> entries;
	// A raw NONE entry is uploadable: the staging path copies the bytes and the
	// request carries COMPRESSION_NONE. Only Zstd/GACL/chunked fail closed.
	GDEX_EXPECT_TRUE(backend._debug_get_texture_mip_entries_for_testing(path, built.data_offset, entries));
	GDEX_EXPECT_TRUE(!entries.is_empty());
	remove_file(path);
#else
	GDEX_SKIP("Windows-only backend parser");
#endif
}
