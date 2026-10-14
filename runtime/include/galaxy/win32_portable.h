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

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <thread>

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <sys/syscall.h>
#endif

// ---------------------------------------------------------------------------
// Base types with their Win32 widths (DWORD and ULONG are 32-bit on Windows).
// ---------------------------------------------------------------------------
using BYTE = std::uint8_t;
using UCHAR = std::uint8_t;
using WORD = std::uint16_t;
using USHORT = std::uint16_t;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using UINT = unsigned int;
using LONG = std::int32_t;
using LONGLONG = std::int64_t;
using ULONGLONG = std::uint64_t;
using DWORD64 = std::uint64_t;
using ULONG64 = std::uint64_t;
using DWORD_PTR = std::uintptr_t;
using ULONG_PTR = std::uintptr_t;
using BOOL = int;
using HRESULT = std::int32_t;
using WCHAR = wchar_t;
using PUCHAR = UCHAR*;
using LPBYTE = BYTE*;
using PVOID = void*;
using LPVOID = void*;
using LPCWSTR = const wchar_t*;
using LPWSTR = wchar_t*;
using HANDLE = void*;
using HMODULE = void*;
using HWND = void*;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0  // Apple's mach headers may already define it
#endif
#ifndef WINAPI
#define WINAPI
#endif
#ifndef CALLBACK
#define CALLBACK
#endif
#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-1)))
inline constexpr DWORD INFINITE = 0xFFFFFFFFu;
inline constexpr DWORD MAX_PATH = 260u;

union LARGE_INTEGER {
    struct {
        DWORD LowPart;
        LONG HighPart;
    };
    LONGLONG QuadPart;
};
union ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    ULONGLONG QuadPart;
};
struct FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
};
struct POINT {
    LONG x;
    LONG y;
};
struct RECT {
    LONG left;
    LONG top;
    LONG right;
    LONG bottom;
};

// ---------------------------------------------------------------------------
// Error codes (winerror.h values) and the thread's last error.
// ---------------------------------------------------------------------------
inline constexpr DWORD ERROR_SUCCESS = 0;
inline constexpr DWORD ERROR_INVALID_FUNCTION = 1;
inline constexpr DWORD ERROR_FILE_NOT_FOUND = 2;
inline constexpr DWORD ERROR_PATH_NOT_FOUND = 3;
inline constexpr DWORD ERROR_TOO_MANY_OPEN_FILES = 4;
inline constexpr DWORD ERROR_ACCESS_DENIED = 5;
inline constexpr DWORD ERROR_INVALID_HANDLE = 6;
inline constexpr DWORD ERROR_NOT_ENOUGH_MEMORY = 8;
inline constexpr DWORD ERROR_INVALID_DATA = 13;
inline constexpr DWORD ERROR_OUTOFMEMORY = 14;
inline constexpr DWORD ERROR_NOT_SAME_DEVICE = 17;
inline constexpr DWORD ERROR_NOT_READY = 21;
inline constexpr DWORD ERROR_CRC = 23;
inline constexpr DWORD ERROR_WRITE_FAULT = 29;
inline constexpr DWORD ERROR_READ_FAULT = 30;
inline constexpr DWORD ERROR_GEN_FAILURE = 31;
inline constexpr DWORD ERROR_SHARING_VIOLATION = 32;
inline constexpr DWORD ERROR_LOCK_VIOLATION = 33;
inline constexpr DWORD ERROR_HANDLE_EOF = 38;
inline constexpr DWORD ERROR_HANDLE_DISK_FULL = 39;
inline constexpr DWORD ERROR_NOT_SUPPORTED = 50;
inline constexpr DWORD ERROR_FILE_EXISTS = 80;
inline constexpr DWORD ERROR_INVALID_PARAMETER = 87;
inline constexpr DWORD ERROR_BROKEN_PIPE = 109;
inline constexpr DWORD ERROR_PROC_NOT_FOUND = 127;
inline constexpr DWORD ERROR_BAD_ARGUMENTS = 160;
inline constexpr DWORD ERROR_DISK_FULL = 112;
inline constexpr DWORD ERROR_INSUFFICIENT_BUFFER = 122;
inline constexpr DWORD ERROR_MOD_NOT_FOUND = 126;
inline constexpr DWORD ERROR_INVALID_NAME = 123;
inline constexpr DWORD ERROR_DIR_NOT_EMPTY = 145;
inline constexpr DWORD ERROR_BUSY = 170;
inline constexpr DWORD ERROR_BAD_PATHNAME = 161;
inline constexpr DWORD ERROR_ALREADY_EXISTS = 183;
inline constexpr DWORD ERROR_ENVVAR_NOT_FOUND = 203;
inline constexpr DWORD ERROR_FILENAME_EXCED_RANGE = 206;
inline constexpr DWORD ERROR_NO_MORE_ITEMS = 259;
inline constexpr DWORD ERROR_DIRECTORY = 267;
inline constexpr DWORD ERROR_OPERATION_ABORTED = 995;
inline constexpr DWORD ERROR_IO_PENDING = 997;
inline constexpr DWORD ERROR_DEVICE_NOT_CONNECTED = 1167;
inline constexpr DWORD ERROR_INVALID_STATE = 5023;
inline constexpr DWORD ERROR_INVALID_ADDRESS = 487;
inline constexpr DWORD ERROR_PRIVILEGE_NOT_HELD = 1314;
inline constexpr DWORD ERROR_FILE_INVALID = 1006;
inline constexpr DWORD ERROR_TIMEOUT = 1460;
inline constexpr DWORD ERROR_STACK_BUFFER_OVERRUN = 1282;
inline constexpr DWORD ERROR_CANT_RESOLVE_FILENAME = 1921;
inline constexpr DWORD WAIT_OBJECT_0 = 0;
inline constexpr DWORD WAIT_TIMEOUT = 258;
inline constexpr DWORD WAIT_FAILED = 0xFFFFFFFFu;

namespace galaxy::win32_detail {
inline thread_local DWORD t_last_error = 0;
}
inline DWORD GetLastError() noexcept { return galaxy::win32_detail::t_last_error; }
inline void SetLastError(DWORD error) noexcept { galaxy::win32_detail::t_last_error = error; }

// errno → the closest Win32 error, as the CRT's mapping does in reverse.
inline DWORD win32_error_from_errno(int error) noexcept {
    switch (error) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: case EROFS: case EISDIR: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    case ENOTEMPTY: return ERROR_DIR_NOT_EMPTY;
    case EMFILE: case ENFILE: return ERROR_TOO_MANY_OPEN_FILES;
    case ENOSPC: return ERROR_DISK_FULL;
#if defined(EDQUOT)
    case EDQUOT: return ERROR_DISK_FULL;
#endif
    case ENAMETOOLONG: return ERROR_FILENAME_EXCED_RANGE;
    case EINVAL: return ERROR_INVALID_PARAMETER;
    case EBADF: return ERROR_INVALID_HANDLE;
    case ENOMEM: return ERROR_NOT_ENOUGH_MEMORY;
    case EBUSY: case ETXTBSY: return ERROR_SHARING_VIOLATION;
    case ELOOP: return ERROR_CANT_RESOLVE_FILENAME;
    case ENOTSUP: return ERROR_NOT_SUPPORTED;
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    case EOPNOTSUPP: return ERROR_NOT_SUPPORTED;
#endif
    case EIO: return ERROR_GEN_FAILURE;
    case EXDEV: return ERROR_NOT_SUPPORTED;
    case ETIMEDOUT: return ERROR_TIMEOUT;
    case EPIPE: return ERROR_BROKEN_PIPE;
    default: return ERROR_GEN_FAILURE;
    }
}

// ---------------------------------------------------------------------------
// Process, thread and time basics.
// ---------------------------------------------------------------------------
inline DWORD GetCurrentProcessId() noexcept { return static_cast<DWORD>(getpid()); }
inline DWORD GetCurrentThreadId() noexcept {
#if defined(__APPLE__)
    std::uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<DWORD>(id);
#else
    return static_cast<DWORD>(syscall(SYS_gettid));
#endif
}
inline ULONGLONG GetTickCount64() noexcept {
    return static_cast<ULONGLONG>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline void Sleep(DWORD milliseconds) noexcept {
    if (milliseconds == 0u) {
        sched_yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
inline BOOL SwitchToThread() noexcept { return sched_yield() == 0 ? TRUE : FALSE; }
inline void YieldProcessor() noexcept {
#if defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__)
    __builtin_ia32_pause();
#endif
}
// QueryPerformanceCounter on the same monotonic clock the runtime timeline uses.
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept {
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || timebase.numer == 0u) return FALSE;
    frequency->QuadPart = static_cast<LONGLONG>(1'000'000'000ull * timebase.denom / timebase.numer);
#else
    frequency->QuadPart = 1'000'000'000;
#endif
    return TRUE;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept {
#if defined(__APPLE__)
    counter->QuadPart = static_cast<LONGLONG>(mach_absolute_time());
#else
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    counter->QuadPart = static_cast<LONGLONG>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
#endif
    return TRUE;
}

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
