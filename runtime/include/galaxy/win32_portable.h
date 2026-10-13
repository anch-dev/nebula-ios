// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The small Win32 vocabulary shared runtime code relies on, for non-Windows
// hosts: slim reader/writer locks and the virtual-key codes the input layer
// uses as its key namespace (an iOS/macOS host maps hardware key codes onto
// these values). Include it in place of <Windows.h> where nothing else from
// Windows is needed. On Windows it simply includes <Windows.h>.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#else

#include <pthread.h>

// SRWLOCK: a pthread rwlock, statically initializable like SRWLOCK_INIT.
using SRWLOCK = pthread_rwlock_t;
#define SRWLOCK_INIT PTHREAD_RWLOCK_INITIALIZER
inline void AcquireSRWLockShared(SRWLOCK* lock) noexcept { pthread_rwlock_rdlock(lock); }
inline void ReleaseSRWLockShared(SRWLOCK* lock) noexcept { pthread_rwlock_unlock(lock); }
inline void AcquireSRWLockExclusive(SRWLOCK* lock) noexcept { pthread_rwlock_wrlock(lock); }
inline void ReleaseSRWLockExclusive(SRWLOCK* lock) noexcept { pthread_rwlock_unlock(lock); }

// Virtual-key codes (values from WinUser.h).
inline constexpr int VK_LBUTTON = 0x01;
inline constexpr int VK_RBUTTON = 0x02;
inline constexpr int VK_MBUTTON = 0x04;
inline constexpr int VK_XBUTTON1 = 0x05;
inline constexpr int VK_XBUTTON2 = 0x06;
inline constexpr int VK_BACK = 0x08;
inline constexpr int VK_TAB = 0x09;
inline constexpr int VK_RETURN = 0x0D;
inline constexpr int VK_SHIFT = 0x10;
inline constexpr int VK_CONTROL = 0x11;
inline constexpr int VK_MENU = 0x12;
inline constexpr int VK_CAPITAL = 0x14;
inline constexpr int VK_ESCAPE = 0x1B;
inline constexpr int VK_SPACE = 0x20;
inline constexpr int VK_PRIOR = 0x21;
inline constexpr int VK_NEXT = 0x22;
inline constexpr int VK_END = 0x23;
inline constexpr int VK_HOME = 0x24;
inline constexpr int VK_LEFT = 0x25;
inline constexpr int VK_UP = 0x26;
inline constexpr int VK_RIGHT = 0x27;
inline constexpr int VK_DOWN = 0x28;
inline constexpr int VK_INSERT = 0x2D;
inline constexpr int VK_DELETE = 0x2E;
inline constexpr int VK_NUMPAD0 = 0x60;
inline constexpr int VK_NUMPAD1 = 0x61;
inline constexpr int VK_NUMPAD2 = 0x62;
inline constexpr int VK_NUMPAD3 = 0x63;
inline constexpr int VK_NUMPAD4 = 0x64;
inline constexpr int VK_NUMPAD5 = 0x65;
inline constexpr int VK_NUMPAD6 = 0x66;
inline constexpr int VK_NUMPAD7 = 0x67;
inline constexpr int VK_NUMPAD8 = 0x68;
inline constexpr int VK_NUMPAD9 = 0x69;
inline constexpr int VK_MULTIPLY = 0x6A;
inline constexpr int VK_ADD = 0x6B;
inline constexpr int VK_SUBTRACT = 0x6D;
inline constexpr int VK_DECIMAL = 0x6E;
inline constexpr int VK_DIVIDE = 0x6F;
inline constexpr int VK_F1 = 0x70;
inline constexpr int VK_F24 = 0x87;
inline constexpr int VK_LSHIFT = 0xA0;
inline constexpr int VK_RSHIFT = 0xA1;
inline constexpr int VK_LCONTROL = 0xA2;
inline constexpr int VK_RCONTROL = 0xA3;
inline constexpr int VK_LMENU = 0xA4;
inline constexpr int VK_RMENU = 0xA5;
inline constexpr int VK_OEM_1 = 0xBA;
inline constexpr int VK_OEM_PLUS = 0xBB;
inline constexpr int VK_OEM_COMMA = 0xBC;
inline constexpr int VK_OEM_MINUS = 0xBD;
inline constexpr int VK_OEM_PERIOD = 0xBE;
inline constexpr int VK_OEM_2 = 0xBF;
inline constexpr int VK_OEM_3 = 0xC0;
inline constexpr int VK_OEM_4 = 0xDB;
inline constexpr int VK_OEM_5 = 0xDC;
inline constexpr int VK_OEM_6 = 0xDD;
inline constexpr int VK_OEM_7 = 0xDE;

// XInput gamepad button word and deadzones (values from Xinput.h): the
// runtime's controller sample format on every platform; GameController input
// on Apple hosts is translated into it.
inline constexpr unsigned short XINPUT_GAMEPAD_DPAD_UP = 0x0001;
inline constexpr unsigned short XINPUT_GAMEPAD_DPAD_DOWN = 0x0002;
inline constexpr unsigned short XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
inline constexpr unsigned short XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008;
inline constexpr unsigned short XINPUT_GAMEPAD_START = 0x0010;
inline constexpr unsigned short XINPUT_GAMEPAD_BACK = 0x0020;
inline constexpr unsigned short XINPUT_GAMEPAD_LEFT_THUMB = 0x0040;
inline constexpr unsigned short XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
inline constexpr unsigned short XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100;
inline constexpr unsigned short XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
inline constexpr unsigned short XINPUT_GAMEPAD_A = 0x1000;
inline constexpr unsigned short XINPUT_GAMEPAD_B = 0x2000;
inline constexpr unsigned short XINPUT_GAMEPAD_X = 0x4000;
inline constexpr unsigned short XINPUT_GAMEPAD_Y = 0x8000;
inline constexpr int XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE = 7849;
inline constexpr int XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE = 8689;
inline constexpr int XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;

#endif
