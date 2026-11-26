// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef DS_TEXTURE_OWNER_H
#define DS_TEXTURE_OWNER_H

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>

namespace godot {

// Lifetime owner for a RenderingDevice texture RID that DSTextureCache will
// NOT take - i.e. an uncached load (CACHE_MODE_IGNORE / CACHE_MODE_IGNORE_DEEP)
// or a disabled cache (#0126).
//
// Texture2DRD is a non-owning view: dropping its last Ref frees the wrapper but
// never the device RID (GODOT_QUIRKS.md section 27, godotengine/godot#116815).
// The cache is the normal owner of those RIDs; when the cache refuses, the
// texture carries one of these instead, attached via Object metadata, so the
// RID is released exactly when the last Ref drops.
//
// Ownership bookkeeping lives in a side table keyed by the texture's instance
// id, NOT in the attached object: disarm() must work without casting the
// metadata value back to this type, because Object::cast_to() on Godot 4.7
// relies on ClassDB and therefore fails for an unregistered build (gdextest
// links these sources without running module registration).
class DSTextureOwner : public RefCounted {
	GDCLASS(DSTextureOwner, RefCounted);

	// Instance id of the texture this owner was attached to (0 = unarmed).
	uint64_t texture_id = 0;

protected:
	static void _bind_methods() {}

public:
	// Arms p_texture so its RID is released when the texture dies. Call at most
	// once per texture; re-arming replaces the RID that will be released.
	static void attach(const Ref<Texture2D> &p_texture, const RID &p_rid);
	// Transfers ownership away from the owner WITHOUT freeing the RID (used
	// when DSTextureCache::store() accepts the texture). Idempotent; safe on
	// textures that never had an owner.
	static void disarm(const Ref<Texture2D> &p_texture);
	// Whether p_texture currently has an armed owner (tests/diagnostics).
	static bool has_owner(const Ref<Texture2D> &p_texture);
	// Re-arms releases after a module init -> uninit -> init cycle. Called from
	// module initialization; without it shutdown()'s latch would keep every
	// later owner release a silent no-op for the rest of the process.
	static void initialize();
	// Drops all armed RIDs without touching the engine. Called from module
	// teardown: from that point on a late texture destructor must not post
	// render-thread work (the device is going away, its RIDs go with it).
	static void shutdown();

	DSTextureOwner() = default;
	~DSTextureOwner() override;
};

} // namespace godot

#endif // DS_TEXTURE_OWNER_H
