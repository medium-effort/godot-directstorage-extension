// tests/dspkg_hostile_toc_tests.cpp
//
// Pre-authentication footprint of the encrypted DSPkg TOC read. A hostile
// archive that fails its MAC must cost the reader one envelope-sized buffer,
// not two. The companion pure-C++ case pins the Monocypher aliasing contract
// that the single-buffer read relies on.
//
// Archive shape (v5, encrypted, bad MAC): entry_count = 32 so the plausible
// bound admits the full 64 MiB envelope, header_size = 96, and a file padded to
// data_offset. Every pre-MAC guard passes; only the AEAD fails.

#include "gdextest/assert.h"
#include "gdextest/registry.h"

#include "formats/dspkg_format.h"
#include "monocypher.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstring>

namespace {

constexpr const char *kKeyHex = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char *kHostilePath = "user://gdextest_dspkg_preauth_hostile.dspkg";
// The v5 envelope cap; entry_count = 32 makes the plausible bound admit it.
constexpr uint64_t kTocLen = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t kChunk = 1024ULL * 1024ULL;
constexpr uint32_t kEntryCount = 32;
constexpr uint64_t kHeaderSize = 96;
constexpr uint64_t kMacSize = 16;

void remove_user_file(const char *p_path) {
	godot::ProjectSettings *settings = godot::ProjectSettings::get_singleton();
	if (settings == nullptr) {
		return;
	}
	godot::DirAccess::remove_absolute(settings->globalize_path(godot::String(p_path)));
}

// Writes the hostile fixture described at the top of the file.
bool write_hostile_archive() {
	using namespace godot;

	DSPkgHeaderV5 header{};
	header.magic = 0x474B5044;
	header.version = DSPKG_VERSION_V5;
	header.flags = DSPKG_FLAG_ENCRYPTED_TOC;
	header.entry_count = kEntryCount;
	header.data_offset = kHeaderSize + kMacSize + kTocLen;
	header.archive_fingerprint = 0;
	header.compression_codec = 0;
	for (int i = 0; i < 16; ++i) {
		header.archive_salt[i] = static_cast<uint8_t>(0x11 + i * 7);
	}
	header.min_reader_version = DSPKG_VERSION_V5;
	PackedByteArray key_id = DSPkg::compute_key_id(String(kKeyHex)).hex_decode();
	if (key_id.size() != 8) {
		return false;
	}
	std::memcpy(&header.key_id, key_id.ptr(), 8);
	header.header_size = static_cast<uint16_t>(kHeaderSize);
	header.toc_entry_size = static_cast<uint16_t>(DSPKG_FIXED_ENTRY_SIZE_V5);

	Ref<FileAccess> file = FileAccess::open(String(kHostilePath), FileAccess::WRITE);
	if (file.is_null() || !file->is_open()) {
		return false;
	}

	PackedByteArray header_bytes;
	header_bytes.resize(static_cast<int64_t>(sizeof(DSPkgHeaderV5)));
	std::memcpy(header_bytes.ptrw(), &header, sizeof(DSPkgHeaderV5));
	file->store_buffer(header_bytes);

	PackedByteArray mac;
	mac.resize(static_cast<int64_t>(kMacSize));
	mac.fill(0xAB);
	file->store_buffer(mac);

	PackedByteArray zeros;
	zeros.resize(static_cast<int64_t>(kChunk));
	uint64_t remaining = kTocLen;
	while (remaining > 0) {
		const uint64_t take = remaining < kChunk ? remaining : kChunk;
		file->store_buffer(take == kChunk ? zeros : zeros.slice(0, static_cast<int64_t>(take)));
		remaining -= take;
	}

	const Error write_error = file->get_error();
	file->close();
	return write_error == OK;
}

} // namespace

// Pins the vendored contract the reader now depends on: crypto_aead_unlock()
// may write into the buffer it reads, and it authenticates before it writes.
GDEX_TEST(dspkg_toc_crypto, inplace_unlock_preserves_plaintext) {
	(void)ctx;

	uint8_t pt[256];
	uint8_t ct[256];
	uint8_t key[32];
	uint8_t nonce[24];
	uint8_t aad[64];
	uint8_t mac[16];
	for (int i = 0; i < 256; ++i) {
		pt[i] = static_cast<uint8_t>(i * 7 + 3);
	}
	for (int i = 0; i < 32; ++i) {
		key[i] = static_cast<uint8_t>(i * 11 + 1);
	}
	for (int i = 0; i < 24; ++i) {
		nonce[i] = static_cast<uint8_t>(i * 5 + 9);
	}
	for (int i = 0; i < 64; ++i) {
		aad[i] = static_cast<uint8_t>(i * 3 + 2);
	}

	crypto_aead_lock(ct, mac, key, nonce, aad, sizeof(aad), pt, sizeof(pt));
	uint8_t expected_ct[256];
	std::memcpy(expected_ct, ct, sizeof(ct));

	const int ok = crypto_aead_unlock(ct, mac, key, nonce, aad, sizeof(aad), ct, sizeof(ct));
	GDEX_EXPECT_EQ(ok, 0);
	GDEX_EXPECT_EQ(std::memcmp(ct, pt, sizeof(pt)), 0);

	uint8_t tampered_mac[16];
	std::memcpy(tampered_mac, mac, sizeof(mac));
	tampered_mac[0] ^= 0xFF;
	std::memcpy(ct, expected_ct, sizeof(ct));
	const int bad = crypto_aead_unlock(ct, tampered_mac, key, nonce, aad, sizeof(aad), ct, sizeof(ct));
	GDEX_EXPECT_TRUE(bad != 0);
	GDEX_EXPECT_EQ(std::memcmp(ct, expected_ct, sizeof(expected_ct)), 0);
}

#if defined(DEBUG_ENABLED) || defined(DEV_ENABLED)
GDEX_TEST_T(dspkg_hostile_toc, preauth_toc_single_buffer, TAG_INTEGRATION) {
	remove_user_file(kHostilePath);
	ctx.add_teardown([]() {
		remove_user_file(kHostilePath);
		godot::DSPkg::set_global_encryption_key("");
	});

	GDEX_ASSERT_TRUE(godot::DSPkg::set_global_encryption_key(godot::String(kKeyHex)) == godot::OK);
	const godot::String key_id_before = godot::DSPkg::get_global_encryption_key_id();

	GDEX_ASSERT_TRUE(write_hostile_archive());

	godot::Ref<godot::DSPkg> reader;
	reader.instantiate();
	GDEX_ASSERT_TRUE(reader.is_valid());

	godot::DSPkg::_reset_preauth_toc_probe_for_testing();
	const godot::Error open_error = reader->open(godot::String(kHostilePath), 0);
	const uint64_t peak = godot::DSPkg::_get_preauth_toc_peak_bytes_for_testing();

	// 1. The hostile file passed the framing guards and failed authentication.
	//    Anything else means the pre-MAC read path was never reached.
	GDEX_EXPECT_EQ(static_cast<int64_t>(open_error), static_cast<int64_t>(godot::ERR_UNAUTHORIZED));
	// 2. Exactly one envelope-sized buffer may be held before the MAC verifies.
	GDEX_EXPECT_EQ(peak, kTocLen);
	// 3. A refused open leaves no reader state behind.
	GDEX_EXPECT_EQ(reader->get_entry_count(), 0);
	GDEX_EXPECT_FALSE(reader->has_texture(godot::String("res://textures/absent.dstex")));
	// 4. A refused open neither installs nor clears the key.
	GDEX_EXPECT_STR_EQ(godot::DSPkg::get_global_encryption_key_id(), key_id_before);
}
#else
GDEX_TEST_T(dspkg_hostile_toc, preauth_toc_single_buffer, TAG_INTEGRATION) {
	(void)ctx;
	GDEX_SKIP("the pre-auth allocation probe is compiled out in release builds");
}
#endif
