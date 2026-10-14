// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Win32 thread, module, window and XInput vocabulary native_host.cpp and
// native_runtime.cpp use, for POSIX hosts:
// - threads: pseudo-handles, CPU time, priorities (advisory);
// - modules: LoadLibraryW/GetProcAddress/FreeLibrary over dlopen, with a
//   "name.dll" mapped to the platform's library name;
// - one virtual window and the input state published through
//   galaxy/host_platform.h (cursor, keys, XInput pads).

#if !defined(_WIN32)

#include <cstdint>

#include "galaxy/host_platform.h"
#include "galaxy/win32_fs.h"
#include "galaxy/win32_portable.h"

// ---------------------------------------------------------------------------
// Threads and processes
// ---------------------------------------------------------------------------
inline HANDLE GetCurrentThread() noexcept {
    return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-2));
}
inline HANDLE GetCurrentProcess() noexcept {
    return reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1));
}
inline constexpr int THREAD_PRIORITY_IDLE = -15;
inline constexpr int THREAD_PRIORITY_LOWEST = -2;
inline constexpr int THREAD_PRIORITY_BELOW_NORMAL = -1;
inline constexpr int THREAD_PRIORITY_NORMAL = 0;
inline constexpr int THREAD_PRIORITY_ABOVE_NORMAL = 1;
inline constexpr int THREAD_PRIORITY_HIGHEST = 2;
inline constexpr int THREAD_PRIORITY_TIME_CRITICAL = 15;
inline constexpr int THREAD_PRIORITY_ERROR_RETURN = 0x7FFFFFFF;

// Advisory: maps onto QoS classes on Apple; a no-op elsewhere.
BOOL SetThreadPriority(HANDLE thread, int priority) noexcept;
int GetThreadPriority(HANDLE thread) noexcept;
// CPU time of the calling thread (only GetCurrentThread() is supported).
BOOL GetThreadTimes(HANDLE thread, FILETIME* creation, FILETIME* exit, FILETIME* kernel,
                    FILETIME* user) noexcept;
inline BOOL QueryThreadCycleTime(HANDLE, ULONG64* cycles) noexcept {
    if (cycles != nullptr) *cycles = 0;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
using FARPROC = void (*)();
HMODULE LoadLibraryW(const wchar_t* name) noexcept;
HMODULE LoadLibraryA(const char* name) noexcept;
FARPROC GetProcAddress(HMODULE module, const char* name) noexcept;
BOOL FreeLibrary(HMODULE module) noexcept;

// ---------------------------------------------------------------------------
// The virtual window
// ---------------------------------------------------------------------------
using LPARAM = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPDWORD = DWORD*;
using WNDENUMPROC = BOOL (*)(HWND, LPARAM);
inline constexpr unsigned GA_PARENT = 1u;
inline constexpr unsigned GA_ROOT = 2u;
inline constexpr unsigned GA_ROOTOWNER = 3u;

HWND GetForegroundWindow() noexcept;
BOOL IsWindow(HWND window) noexcept;
BOOL IsWindowVisible(HWND window) noexcept;
DWORD GetWindowThreadProcessId(HWND window, DWORD* process_id) noexcept;
BOOL GetCursorPos(POINT* point) noexcept;  // screen == client coordinates
HWND WindowFromPoint(POINT point) noexcept;
HWND GetAncestor(HWND window, unsigned flags) noexcept;
BOOL EnumWindows(WNDENUMPROC callback, LPARAM user) noexcept;
BOOL GetClientRect(HWND window, RECT* rect) noexcept;
BOOL ScreenToClient(HWND window, POINT* point) noexcept;
// Reports the runtime window's Win32 class name, "NebulaRuntimeWndR".
int GetClassNameW(HWND window, wchar_t* name, int length) noexcept;
int lstrcmpW(const wchar_t* left, const wchar_t* right) noexcept;
short GetAsyncKeyState(int virtual_key) noexcept;

// ---------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------
inline constexpr DWORD XUSER_MAX_COUNT = 4u;
struct XINPUT_GAMEPAD {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    std::int16_t sThumbLX;
    std::int16_t sThumbLY;
    std::int16_t sThumbRX;
    std::int16_t sThumbRY;
};
struct XINPUT_STATE {
    DWORD dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_VIBRATION {
    WORD wLeftMotorSpeed;
    WORD wRightMotorSpeed;
};
DWORD XInputGetState(DWORD user_index, XINPUT_STATE* state) noexcept;
DWORD XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) noexcept;

#endif
