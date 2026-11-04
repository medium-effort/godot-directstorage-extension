// tests/ds_crypto_tests.cpp
//
// DSCrypto is the installer's integrity primitive: NuGet publishes a SHA-512
// packageHash that Godot's HashingContext cannot compute. The known vector
// pins the backend (bcrypt on Windows) and the file path, so a broken chunk
// loop or a wrong algorithm is caught without a network download.

#include "gdextest/assert.h"
#include "gdextest/registry.h"
#include "gdextest/strings.h"

#include "core/ds_crypto.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>

namespace {

#ifdef _WIN32
// SHA-512("abc"), FIPS 180-4 test vector.
const char *kAbcSha512Hex =
		"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
		"2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f";
#endif

} // namespace

GDEX_TEST(crypto, sha512_file_matches_fips_vector) {
#ifdef _WIN32
	(void)ctx;
	const char *path = "user://gdextest_sha512_abc.bin";
	{
		godot::Ref<godot::FileAccess> file = godot::FileAccess::open(path, godot::FileAccess::WRITE);
		GDEX_ASSERT_TRUE(file.is_valid());
		file->store_string("abc");
	}

	const godot::PackedByteArray digest = godot::DSCrypto::sha512_file(path);
	GDEX_EXPECT_EQ(static_cast<int64_t>(digest.size()), static_cast<int64_t>(64));
	GDEX_EXPECT_EQ(::gdextest::to_std_string(digest.hex_encode()), std::string(kAbcSha512Hex));

	godot::ProjectSettings *settings = godot::ProjectSettings::get_singleton();
	if (settings != nullptr) {
		godot::DirAccess::remove_absolute(settings->globalize_path(path));
	}
#else
	(void)ctx;
	GDEX_SKIP("SHA-512 file hashing is Windows-only (bcrypt backend).");
#endif
}
