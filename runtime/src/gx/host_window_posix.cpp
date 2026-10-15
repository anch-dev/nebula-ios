// SPDX-License-Identifier: GPL-3.0-only
// Window queries the host shares with the renderer, on non-Windows hosts:
// the window is the app shell's view published through
// galaxy/host_platform.h (the Windows renderer reads its HWND instead).

#if !defined(_WIN32)

#include "galaxy/gx_d3d12.h"
#include "galaxy/win32_host_posix.h"

namespace galaxy::gx {

// Weak: when the renderer is linked, its definition (renderer_d3d12.cpp,
// over the same virtual window) is the one used.
__attribute__((weak)) HostClientExtent current_host_client_extent() noexcept {
    HWND window = nullptr;
    if (EnumWindows(
            [](HWND candidate, LPARAM user) -> BOOL {
                *reinterpret_cast<HWND*>(user) = candidate;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&window)) == FALSE ||
        window == nullptr) {
        return {};
    }
    RECT client{};
    if (!GetClientRect(window, &client)) return {};
    return {client.right - client.left, client.bottom - client.top};
}

}  // namespace galaxy::gx

#endif
