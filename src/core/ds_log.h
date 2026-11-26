// SPDX-FileCopyrightText: 2026 MediumEffort
// SPDX-License-Identifier: MPL-2.0
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#pragma once

namespace godot {
namespace dsgd_log {

// Log prefix for messages emitted by the dsgd extension itself (as opposed to
// Microsoft DirectStorage / D3D12 runtime messages in Win32Backend).
inline constexpr const char *EXTENSION_PREFIX = "[dsgd Extension] ";

} // namespace dsgd_log
} // namespace godot
