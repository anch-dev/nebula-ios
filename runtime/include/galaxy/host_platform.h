// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// What an app shell (the iOS/macOS app, a Linux test harness) tells the
// runtime about its window and input devices on non-Windows hosts.
//
// On Windows the runtime reads these from Win32 (its own window, XInput,
// GetAsyncKeyState). Elsewhere the runtime's Win32-shaped input code reads
// the state published here instead: the shell maps UIKit/AppKit views,
// touches and GameController devices onto one virtual window, virtual-key
// states and up to four XInput-format pads. Every function is thread-safe.

#include <cstdint>

namespace galaxy::host_platform {

// The game's window/view. `view` is an opaque native handle (CAMetalLayer*
// for the Metal renderer) the renderer also receives; null means no window.
// Size and focus changes reach the renderer's window procedure as WM_SIZE,
// WM_SETFOCUS/WM_KILLFOCUS and WM_ACTIVATEAPP, as on Windows.
void set_window(void* view, int width, int height, bool visible, bool focused);

// The view last passed to set_window (null when none), for the renderer.
void* current_view();

// The screen the view is on: size in pixels, refresh rate, and points to
// pixels scale (DPI = 96 x scale). Unset: the view's size, 60 Hz, 1x.
void set_screen(int width, int height, unsigned refresh_hz, float scale);

// Pointer position in window client coordinates (touch or mouse), and
// whether it is over the window. Delivered as WM_MOUSEMOVE (and
// WM_MOUSELEAVE once tracked).
void set_pointer(bool inside, int x, int y);

// Virtual-key state (VK_* codes from galaxy/win32_portable.h, including
// VK_LBUTTON/VK_RBUTTON/VK_MBUTTON for pointer buttons). Changes are
// delivered as WM_KEYDOWN/WM_KEYUP or the button messages.
void set_key(int virtual_key, bool down);

// One pad in XInput's format (XINPUT_GAMEPAD_* buttons, sticks -32768..32767,
// triggers 0..255). Slots 0..3.
struct PadState {
    bool connected = false;
    std::uint16_t buttons = 0;
    std::uint8_t left_trigger = 0;
    std::uint8_t right_trigger = 0;
    std::int16_t left_x = 0;
    std::int16_t left_y = 0;
    std::int16_t right_x = 0;
    std::int16_t right_y = 0;
};
void set_pad(unsigned slot, const PadState& state);

// The vibration the runtime last requested for a pad (0..65535 per motor),
// for the shell to apply (e.g. through GCDeviceHaptics).
struct PadRumble {
    std::uint16_t left_motor = 0;
    std::uint16_t right_motor = 0;
};
PadRumble pad_rumble(unsigned slot);

}  // namespace galaxy::host_platform
