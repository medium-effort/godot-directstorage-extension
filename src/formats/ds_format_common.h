#ifndef DS_FORMAT_COMMON_H
#define DS_FORMAT_COMMON_H

#include <cstddef>
#include <cstdint>

namespace godot {

// Shared on-disk codec identity across DSTex, DSPkg and DSRequest. This is the
// payload compression format, not an audio/video codec.
enum class DSCompressionFormat : uint8_t {
	NONE = 0,
	GDEFLATE = 1,
	ZSTD = 2,
};

// Game Asset Conditioning Library shuffle transform. Values match
// DSTORAGE_GACL_SHUFFLE_TRANSFORM_TYPE on DirectStorage 1.4. Values above BC5
// are reserved for raw GACL experimental ids and are rejected until the runtime
// exposes a matching reverse transform.
enum class DSGaclTransform : uint8_t {
	NONE = 0,
	BC1 = 1,
	BC3 = 2,
	BC4 = 3,
	BC5 = 4,
};

enum class DSPkgEntryKind : uint8_t {
	TEXTURE = 0,
	MESH = 1,
	BLOB = 2,
	// Reserved by the v5 spec as an additive kind: payload is a standard audio
	// container (Ogg/MP3/WAV), compression_format must be NONE, there is no mip
	// table, and metadata carries the PCM format/loop points. The archive reader
	// validates the container layout; audio playback is a later consumer.
	AUDIO = 3,
};

// DSPkg v5 capabilities. Every bit is a required feature: an older reader that
// does not know it fails closed. Optional metadata belongs in a TLV.
constexpr uint64_t DSPKG_CAP_ZSTD = 1ULL << 0;
constexpr uint64_t DSPKG_CAP_GACL = 1ULL << 1;
constexpr uint64_t DSPKG_CAP_CHUNKED = 1ULL << 2;
constexpr uint64_t DSPKG_CAP_CONTENT_HASH = 1ULL << 3;
constexpr uint64_t DSPKG_CAP_MESH = 1ULL << 4;
constexpr uint64_t DSPKG_CAP_CRYPTO_V2 = 1ULL << 5;
constexpr uint64_t DSPKG_CAP_MULTI_ARCHIVE = 1ULL << 6;
constexpr uint64_t DSPKG_CAP_SHARED_PAYLOAD = 1ULL << 7;
constexpr uint64_t DSPKG_CAP_KNOWN_MASK =
		DSPKG_CAP_ZSTD | DSPKG_CAP_GACL | DSPKG_CAP_CHUNKED | DSPKG_CAP_CONTENT_HASH |
		DSPKG_CAP_MESH | DSPKG_CAP_CRYPTO_V2 | DSPKG_CAP_MULTI_ARCHIVE |
		DSPKG_CAP_SHARED_PAYLOAD;

// DSTex v3 capability word layout. Bits 0-7 and 8-15 carry the codec and GACL
// transform directly; bit 16 is the chunked summary; bits 17-63 are reserved.
constexpr uint64_t DSTEX_CAP_COMPRESSION_MASK = 0xFFULL;
constexpr uint64_t DSTEX_CAP_GACL_SHIFT = 8;
constexpr uint64_t DSTEX_CAP_GACL_MASK = 0xFFULL << DSTEX_CAP_GACL_SHIFT;
constexpr uint64_t DSTEX_CAP_CHUNKED = 1ULL << 16;
constexpr uint64_t DSTEX_CAP_KNOWN_MASK =
		DSTEX_CAP_COMPRESSION_MASK | DSTEX_CAP_GACL_MASK | DSTEX_CAP_CHUNKED;

// TLV tags. Unknown tags are skipped; tag 0 is invalid.
constexpr uint32_t DS_TLV_TAG_CRYPTO = 0x0001;
constexpr uint32_t DS_TLV_TAG_PROVENANCE = 0x0002;
constexpr uint32_t DS_TLV_TAG_ARCHIVE_SET = 0x0003;
constexpr uint32_t DS_TLV_TAG_DELTA = 0x0004;
// NPOT display dimensions for a texture entry (Phase 3 item 2). Payload is a
// DSPkgTextureLogical (8 bytes). Absent = logical size equals storage size.
// Written by the v5 archive writer; v4 archives never carry it.
constexpr uint32_t DS_TLV_TAG_TEXTURE_LOGICAL = 0x0005;
constexpr uint32_t DS_TLV_TAG_EXPERIMENTAL_MIN = 0x8000;
constexpr uint32_t DS_TLV_HEADER_SIZE = 8;
constexpr uint32_t DS_TLV_MAX_PAYLOAD = 1024 * 1024;
constexpr uint64_t DS_ENTRY_TLV_MAX_TOTAL = 64 * 1024;
constexpr uint64_t DS_HEADER_TLV_MAX_TOTAL = 1024 * 1024;

#pragma pack(push, 1)
// Common 16-byte chunk descriptor for both DSTex v3 and DSPkg v5.
struct DSChunkEntry {
	uint64_t offset;            // absolute file offset
	uint32_t compressed_size;
	uint32_t uncompressed_size;
};
#pragma pack(pop)

static_assert(sizeof(DSChunkEntry) == 16, "DSChunkEntry must be 16 bytes");

// v5 entry-TLV payload for DS_TLV_TAG_TEXTURE_LOGICAL: the NPOT display size.
// Both fields are 0 (= storage size) when the texture needs no override, but
// the writer omits the record entirely in that case.
#pragma pack(push, 1)
struct DSPkgTextureLogical {
	uint32_t logical_width;
	uint32_t logical_height;
};
#pragma pack(pop)

static_assert(sizeof(DSPkgTextureLogical) == 8, "DSPkgTextureLogical must be 8 bytes");

inline bool ds_compression_format_is_valid(uint8_t p_format) {
	return p_format <= static_cast<uint8_t>(DSCompressionFormat::ZSTD);
}

inline bool ds_gacl_transform_is_valid(uint8_t p_transform) {
	return p_transform <= static_cast<uint8_t>(DSGaclTransform::BC5);
}

inline bool ds_entry_kind_is_valid(uint8_t p_kind) {
	return p_kind <= static_cast<uint8_t>(DSPkgEntryKind::AUDIO);
}

} // namespace godot

#endif // DS_FORMAT_COMMON_H
