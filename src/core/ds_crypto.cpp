#include "ds_crypto.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#endif

namespace godot {

void DSCrypto::_bind_methods() {
	ClassDB::bind_static_method("DSCrypto", D_METHOD("sha512_file", "path"), &DSCrypto::sha512_file);
}

PackedByteArray DSCrypto::sha512_file(const String &p_path) {
#ifdef _WIN32
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ);
	if (file.is_null() || !file->is_open()) {
		return PackedByteArray();
	}

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA512_ALGORITHM, nullptr, 0) < 0) {
		return PackedByteArray();
	}

	DWORD object_length = 0;
	DWORD property_length = 0;
	if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &property_length, 0) < 0) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return PackedByteArray();
	}

	std::vector<uint8_t> hash_object(object_length);
	BCRYPT_HASH_HANDLE hash = nullptr;
	if (BCryptCreateHash(algorithm, &hash, hash_object.data(), object_length, nullptr, 0, 0) < 0) {
		BCryptCloseAlgorithmProvider(algorithm, 0);
		return PackedByteArray();
	}

	bool ok = true;
	while (true) {
		PackedByteArray chunk = file->get_buffer(1024 * 1024);
		if (chunk.size() == 0) {
			// get_buffer() returns an empty array both at EOF and on a read
			// error, so only a non-EOF error is a failure here.
			if (!file->eof_reached() && file->get_error() != OK) {
				ok = false;
			}
			break;
		}
		if (BCryptHashData(hash, const_cast<PUCHAR>(chunk.ptr()), static_cast<ULONG>(chunk.size()), 0) < 0) {
			ok = false;
			break;
		}
	}

	PackedByteArray digest;
	if (ok) {
		digest.resize(64);
		if (BCryptFinishHash(hash, digest.ptrw(), 64, 0) < 0) {
			digest.clear();
		}
	}

	BCryptDestroyHash(hash);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return digest;
#else
	(void)p_path;
	return PackedByteArray();
#endif
}

} // namespace godot
