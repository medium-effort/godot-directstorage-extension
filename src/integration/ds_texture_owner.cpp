// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include "ds_texture_owner.h"

#include "ds_texture_cache.h"

#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace godot {

namespace {

// Metadata key on the texture. Deliberately a plain literal: a namespace-scope
// StringName would be constructed before engine init.
const char *const OWNER_META_KEY = "_ds_texture_owner";

// Armed RIDs keyed by texture instance id. See the header for why the state
// lives here instead of inside the attached object.
std::mutex s_owner_mutex;
std::unordered_map<uint64_t, RID> s_owned_rids;

// False after module teardown: destructors then skip the engine entirely.
// Atomic: destructors read it from whatever thread drops the last Ref while
// shutdown() writes it from the main thread.
std::atomic<bool> s_release_allowed{ true };

} // namespace

void DSTextureOwner::attach(const Ref<Texture2D> &p_texture, const RID &p_rid) {
	if (p_texture.is_null() || !p_texture.is_valid() || !p_rid.is_valid()) {
		return;
	}

	const uint64_t texture_id = p_texture->get_instance_id();
	RID replaced;
	{
		std::lock_guard<std::mutex> lock(s_owner_mutex);
		std::unordered_map<uint64_t, RID>::iterator it = s_owned_rids.find(texture_id);
		if (it != s_owned_rids.end()) {
			if (it->second == p_rid) {
				return; // Already armed with this RID: nothing to do.
			}
			replaced = it->second;
			it->second = p_rid;
		} else {
			s_owned_rids[texture_id] = p_rid;
		}
	}

	// Defensive: a re-arm replaces the RID, so the superseded one has no owner
	// left. Released outside the lock (it may post render-thread work).
	if (replaced.is_valid()) {
		UtilityFunctions::push_warning("DSTextureOwner: re-arming a texture replaced RID ", replaced, "; releasing it.");
		if (s_release_allowed.load()) {
			DSTextureCache::release_owned_rid(replaced);
		}
	}

	if (!p_texture->has_meta(OWNER_META_KEY)) {
		Ref<DSTextureOwner> owner;
		owner.instantiate();
		owner->texture_id = texture_id;
		// The metadata Variant holds the only Ref: the owner dies with the
		// texture's Object (its metadata is destroyed inside ~Object).
		p_texture->set_meta(OWNER_META_KEY, owner.ptr());
	}
}

void DSTextureOwner::disarm(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null() || !p_texture.is_valid()) {
		return;
	}
	const uint64_t texture_id = p_texture->get_instance_id();
	{
		std::lock_guard<std::mutex> lock(s_owner_mutex);
		s_owned_rids.erase(texture_id);
	}
	if (p_texture->has_meta(OWNER_META_KEY)) {
		// Dropping the Variant releases the owner; its destructor now finds no
		// table entry and frees nothing.
		p_texture->remove_meta(OWNER_META_KEY);
	}
}

bool DSTextureOwner::has_owner(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null() || !p_texture.is_valid()) {
		return false;
	}
	std::lock_guard<std::mutex> lock(s_owner_mutex);
	return s_owned_rids.find(p_texture->get_instance_id()) != s_owned_rids.end();
}

void DSTextureOwner::initialize() {
	// Re-arm releases after a module init -> uninit -> init cycle (editor
	// reload, or a harness that drives the module lifecycle). shutdown() is a
	// one-way latch otherwise, and every later owner release would be a no-op.
	s_release_allowed.store(true);
}

void DSTextureOwner::shutdown() {
	std::unordered_map<uint64_t, RID> dropped;
	{
		std::lock_guard<std::mutex> lock(s_owner_mutex);
		s_release_allowed.store(false);
		dropped.swap(s_owned_rids);
	}
	// RIDs are intentionally not freed: the RenderingDevice is being torn down
	// and they die with it. Keeping the table would only invite late frees.
	dropped.clear();
}

DSTextureOwner::~DSTextureOwner() {
	if (texture_id == 0) {
		return;
	}

	RID rid;
	{
		std::lock_guard<std::mutex> lock(s_owner_mutex);
		std::unordered_map<uint64_t, RID>::iterator it = s_owned_rids.find(texture_id);
		if (it == s_owned_rids.end()) {
			return; // Disarmed, or a cache entry took ownership.
		}
		rid = it->second;
		s_owned_rids.erase(it);
	}

	if (s_release_allowed.load() && rid.is_valid()) {
		// Reaches the render thread through DSTextureCache's existing deferred
		// free queue (no-op when the device/singletons are already gone).
		DSTextureCache::release_owned_rid(rid);
	}
}

} // namespace godot
