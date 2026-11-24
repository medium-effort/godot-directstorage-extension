#pragma once

namespace godot {
namespace dsgd_log {

// Log prefix for messages emitted by the dsgd extension itself (as opposed to
// Microsoft DirectStorage / D3D12 runtime messages in Win32Backend).
inline constexpr const char *EXTENSION_PREFIX = "[dsgd Extension] ";

} // namespace dsgd_log
} // namespace godot
