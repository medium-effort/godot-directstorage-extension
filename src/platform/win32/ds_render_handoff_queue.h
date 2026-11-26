// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef DS_RENDER_HANDOFF_QUEUE_H
#define DS_RENDER_HANDOFF_QUEUE_H

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

namespace godot {

// FIFO handoff of per-request work from a submitting thread to the render thread.
//
// Deliberately platform-neutral: no Windows, D3D12 or godot-cpp headers, only the
// standard library. That keeps the take/prune contract unit-testable without a
// live RenderingDevice, which matters here because the defect this type exists to
// prevent only shows up when two GPU destinations are submitted concurrently.
//
// A single-slot handoff loses the first caller: the second submit overwrites the
// parked context, the render thread services only the newer one, and the first
// caller finds out when its bounded wait expires. A queue parks every context and
// each posted render-thread callback pops exactly one, so N concurrent submits
// produce N serviced contexts regardless of interleaving.
//
// Threading: every method takes the lock, so all of them are safe from any thread.
// Ownership: contexts are shared_ptr, so a submitter that times out and prunes its
// own entry cannot free memory a render-thread callback is already reading.

template <typename T>
class DSRenderHandoffQueue {
public:
	// Submitter side, before posting the render-thread callback.
	void push(const std::shared_ptr<T> &p_context) {
		std::lock_guard<std::mutex> lock(mutex);
		contexts.push_back(p_context);
	}

	// Render-thread side: takes the oldest parked context, or null when the queue
	// is empty. A callback posted for a context that has since been pruned still
	// fires, so an empty queue must be a no-op rather than an error.
	std::shared_ptr<T> take_front() {
		std::lock_guard<std::mutex> lock(mutex);
		if (contexts.empty()) {
			return std::shared_ptr<T>();
		}
		std::shared_ptr<T> ctx = contexts.front();
		contexts.pop_front();
		return ctx;
	}

	// Submitter side, after its bounded wait expired: drops its own context
	// wherever it now sits, so it is never serviced after the caller gave up on it
	// and no ghost entry shifts the FIFO for later submitter. Returns false when a
	// callback already took it (the caller still returns its timeout, which is the
	// correct outcome).
	bool prune(const std::shared_ptr<T> &p_context) {
		std::lock_guard<std::mutex> lock(mutex);
		for (auto it = contexts.begin(); it != contexts.end(); ++it) {
			if (*it == p_context) {
				contexts.erase(it);
				return true;
			}
		}
		return false;
	}

	// Module teardown, called while the engine is alive and the loader lock is NOT
	// held (see ds_win32_release_static_d3d12_state()).
	void clear() {
		std::lock_guard<std::mutex> lock(mutex);
		contexts.clear();
	}

	size_t size() const {
		std::lock_guard<std::mutex> lock(mutex);
		return contexts.size();
	}

private:
	mutable std::mutex mutex;
	// std::deque, not std::vector: the render thread pops from the front on every
	// handoff, and erase(begin()) would shift the whole queue each time. prune()
	// stays a linear scan, which is what identity matching needs anyway.
	std::deque<std::shared_ptr<T>> contexts;
};

} // namespace godot

#endif // DS_RENDER_HANDOFF_QUEUE_H
