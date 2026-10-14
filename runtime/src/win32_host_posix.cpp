// SPDX-License-Identifier: GPL-3.0-only
// POSIX implementations behind galaxy/win32_host_posix.h and the input state
// an app shell publishes through galaxy/host_platform.h.

#if !defined(_WIN32)

#include "galaxy/win32_host_posix.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>

#include <cstdio>
#include <unordered_map>

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pthread/qos.h>
#endif

namespace {

// The single virtual window is identified by this address.
int g_window_identity = 0;
HWND window_handle() noexcept { return &g_window_identity; }

struct HostState {
    std::mutex mutex;
    void* view = nullptr;
    int width = 0;
    int height = 0;
    bool visible = false;
    bool focused = false;
    bool pointer_inside = false;
    int pointer_x = 0;
    int pointer_y = 0;
    std::array<galaxy::host_platform::PadState, 4> pads{};
    std::array<DWORD, 4> packets{};
    std::array<galaxy::host_platform::PadRumble, 4> rumble{};
};

HostState& state() {
    static HostState host;
    return host;
}

std::array<std::atomic<bool>, 256>& keys() {
    static std::array<std::atomic<bool>, 256> down{};
    return down;
}

bool is_window(HWND window) noexcept {
    if (window != window_handle()) return false;
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().view != nullptr;
}

FILETIME filetime_from_ns(std::uint64_t nanoseconds) noexcept {
    const std::uint64_t ticks = nanoseconds / 100u;
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32u)};
}

}  // namespace

namespace galaxy::host_platform {

void set_window(void* view, int width, int height, bool visible, bool focused) {
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().view = view;
    state().width = width;
    state().height = height;
    state().visible = visible;
    state().focused = focused;
}

void set_pointer(bool inside, int x, int y) {
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().pointer_inside = inside;
    state().pointer_x = x;
    state().pointer_y = y;
}

void set_key(int virtual_key, bool down) {
    if (virtual_key > 0 && virtual_key < 256) {
        keys()[static_cast<std::size_t>(virtual_key)].store(down, std::memory_order_release);
    }
}

void set_pad(unsigned slot, const PadState& pad) {
    if (slot >= 4u) return;
    const std::lock_guard<std::mutex> lock(state().mutex);
    PadState& current = state().pads[slot];
    const bool changed = std::memcmp(&current, &pad, sizeof(PadState)) != 0;
    current = pad;
    if (changed) ++state().packets[slot];
}

PadRumble pad_rumble(unsigned slot) {
    if (slot >= 4u) return {};
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().rumble[slot];
}

}  // namespace galaxy::host_platform

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------

namespace {
thread_local int t_priority = THREAD_PRIORITY_NORMAL;
}

BOOL SetThreadPriority(HANDLE thread, int priority) noexcept {
    if (thread != GetCurrentThread()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    t_priority = priority;
#if defined(__APPLE__)
    qos_class_t qos = QOS_CLASS_DEFAULT;
    if (priority >= THREAD_PRIORITY_ABOVE_NORMAL) qos = QOS_CLASS_USER_INTERACTIVE;
    else if (priority <= THREAD_PRIORITY_LOWEST) qos = QOS_CLASS_UTILITY;
    else if (priority < THREAD_PRIORITY_NORMAL) qos = QOS_CLASS_USER_INITIATED;
    (void)pthread_set_qos_class_self_np(qos, 0);
#endif
    return TRUE;
}

int GetThreadPriority(HANDLE thread) noexcept {
    if (thread != GetCurrentThread()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return THREAD_PRIORITY_ERROR_RETURN;
    }
    return t_priority;
}

BOOL GetThreadTimes(HANDLE thread, FILETIME* creation, FILETIME* exit, FILETIME* kernel,
                    FILETIME* user) noexcept {
    if (thread != GetCurrentThread()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    timespec cpu{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0) {
        SetLastError(win32_error_from_errno(errno));
        return FALSE;
    }
    const std::uint64_t ns = static_cast<std::uint64_t>(cpu.tv_sec) * 1'000'000'000ull +
                             static_cast<std::uint64_t>(cpu.tv_nsec);
    if (creation != nullptr) *creation = {};
    if (exit != nullptr) *exit = {};
    if (kernel != nullptr) *kernel = {};  // not separated on POSIX
    if (user != nullptr) *user = filetime_from_ns(ns);
    return TRUE;
}

BOOL GetProcessTimes(HANDLE process, FILETIME* creation, FILETIME* exit, FILETIME* kernel,
                     FILETIME* user) noexcept {
    if (process != GetCurrentProcess()) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        SetLastError(win32_error_from_errno(errno));
        return FALSE;
    }
    const auto ns = [](const timeval& value) {
        return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000ull +
               static_cast<std::uint64_t>(value.tv_usec) * 1'000ull;
    };
    if (creation != nullptr) *creation = {};
    if (exit != nullptr) *exit = {};
    if (kernel != nullptr) *kernel = filetime_from_ns(ns(usage.ru_stime));
    if (user != nullptr) *user = filetime_from_ns(ns(usage.ru_utime));
    return TRUE;
}

void ExitProcess(UINT exit_code) noexcept {
    std::fflush(nullptr);
    _exit(static_cast<int>(exit_code));
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

namespace {

std::string platform_library_name(std::string name) {
    // "RMGE01.dll" -> "libRMGE01.dylib" / "libRMGE01.so"; a name with a
    // directory or another suffix is used as given.
    const std::size_t slash = name.find_last_of('/');
    const std::size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        std::string suffix = name.substr(dot);
        for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (suffix != ".dll") return name;
        name.resize(dot);
    }
    const std::string directory = slash == std::string::npos ? "" : name.substr(0, slash + 1u);
    const std::string base = slash == std::string::npos ? name : name.substr(slash + 1u);
#if defined(__APPLE__)
    return directory + "lib" + base + ".dylib";
#else
    return directory + "lib" + base + ".so";
#endif
}

// dlopen handles are opaque (they are not image bases), so the file each was
// opened from is recorded for GetModuleFileNameW.
std::mutex& module_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::unordered_map<void*, std::string>& module_paths() {
    static auto* paths = new std::unordered_map<void*, std::string>;
    return *paths;
}
void remember_module_path(void* module, const std::string& file) {
    char resolved[PATH_MAX];
    const std::string path = realpath(file.c_str(), resolved) != nullptr ? resolved : file;
    const std::lock_guard<std::mutex> lock(module_mutex());
    module_paths()[module] = path;
}

std::string executable_path() {
#if defined(__APPLE__)
    std::uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) return {};
    path.resize(std::strlen(path.c_str()));
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) != nullptr ? std::string(resolved) : path;
#else
    char buffer[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1u);
    return length > 0 ? std::string(buffer, static_cast<std::size_t>(length)) : std::string{};
#endif
}

}  // namespace

HMODULE LoadLibraryExW(const wchar_t* name, HANDLE, DWORD) noexcept {
    return LoadLibraryW(name);
}

HMODULE GetModuleHandleW(const wchar_t* name) noexcept {
    try {
        void* module = name == nullptr
            ? dlopen(nullptr, RTLD_NOW)
            : dlopen(platform_library_name(galaxy::win32::narrow(name)).c_str(),
                     RTLD_NOW | RTLD_NOLOAD);
        // GetModuleHandle does not take a reference; dlopen did.
        if (module != nullptr && name != nullptr) (void)dlclose(module);
        SetLastError(module != nullptr ? ERROR_SUCCESS : ERROR_MOD_NOT_FOUND);
        return module;
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

BOOL GetModuleHandleExW(DWORD flags, const wchar_t* name_or_address, HMODULE* module) noexcept {
    if (module == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *module = nullptr;
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) == 0u) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }
    if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0u) {
        *module = GetModuleHandleW(name_or_address);
        return *module != nullptr ? TRUE : FALSE;
    }
    Dl_info info{};
    if (dladdr(name_or_address, &info) == 0 || info.dli_fbase == nullptr) {
        SetLastError(ERROR_MOD_NOT_FOUND);
        return FALSE;
    }
    *module = info.dli_fbase;
    return TRUE;
}

DWORD GetModuleFileNameW(HMODULE module, wchar_t* path, DWORD size) noexcept {
    if (path == nullptr || size == 0u) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0u;
    }
    try {
        std::string file;
        if (module == nullptr) {
            file = executable_path();
        } else {
            {
                const std::lock_guard<std::mutex> lock(module_mutex());
                const auto found = module_paths().find(module);
                if (found != module_paths().end()) file = found->second;
            }
            Dl_info info{};
            if (file.empty() && dladdr(module, &info) != 0 && info.dli_fbase == module &&
                info.dli_fname != nullptr) {
                file = info.dli_fname;
            }
        }
        if (file.empty()) {
            path[0] = L'\0';
            SetLastError(ERROR_MOD_NOT_FOUND);
            return 0u;
        }
        const std::wstring wide = galaxy::win32::widen(file.c_str());
        if (wide.size() >= size) {
            // Truncated and terminated, as Windows does.
            std::wmemcpy(path, wide.data(), size - 1u);
            path[size - 1u] = L'\0';
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return size;
        }
        std::wmemcpy(path, wide.data(), wide.size());
        path[wide.size()] = L'\0';
        SetLastError(ERROR_SUCCESS);
        return static_cast<DWORD>(wide.size());
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0u;
    }
}

HMODULE LoadLibraryA(const char* name) noexcept {
    if (name == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    try {
        const std::string file = platform_library_name(name);
        void* const module = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (module == nullptr) {
            if (const char* reason = dlerror()) std::fprintf(stderr, "[module] dlopen: %s\n", reason);
            SetLastError(ERROR_FILE_NOT_FOUND);
            return nullptr;
        }
        remember_module_path(module, file);
        SetLastError(ERROR_SUCCESS);
        return module;
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

HMODULE LoadLibraryW(const wchar_t* name) noexcept {
    try {
        return LoadLibraryA(galaxy::win32::narrow(name).c_str());
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

FARPROC GetProcAddress(HMODULE module, const char* name) noexcept {
    if (module == nullptr || name == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    void* const symbol = dlsym(module, name);
    SetLastError(symbol != nullptr ? ERROR_SUCCESS : ERROR_PROC_NOT_FOUND);
    return reinterpret_cast<FARPROC>(symbol);
}

BOOL FreeLibrary(HMODULE module) noexcept {
    if (module == nullptr) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (dlclose(module) != 0) return FALSE;
    // dlopen reference counts; the recorded path is kept while any reference
    // may remain (re-recorded identically on the next load).
    return TRUE;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

HWND GetForegroundWindow() noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().view != nullptr && state().focused ? window_handle() : nullptr;
}

BOOL IsWindow(HWND window) noexcept { return is_window(window) ? TRUE : FALSE; }

BOOL IsWindowVisible(HWND window) noexcept {
    if (window != window_handle()) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().view != nullptr && state().visible ? TRUE : FALSE;
}

DWORD GetWindowThreadProcessId(HWND window, DWORD* process_id) noexcept {
    if (!is_window(window)) {
        if (process_id != nullptr) *process_id = 0;
        return 0;
    }
    if (process_id != nullptr) *process_id = GetCurrentProcessId();
    return GetCurrentThreadId();
}

BOOL GetCursorPos(POINT* point) noexcept {
    if (point == nullptr) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    point->x = state().pointer_x;
    point->y = state().pointer_y;
    return TRUE;
}

HWND WindowFromPoint(POINT point) noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    if (state().view == nullptr || !state().visible || !state().pointer_inside) return nullptr;
    if (point.x < 0 || point.y < 0 || point.x >= state().width || point.y >= state().height) {
        return nullptr;
    }
    return window_handle();
}

HWND GetAncestor(HWND window, unsigned) noexcept { return is_window(window) ? window : nullptr; }

BOOL EnumWindows(WNDENUMPROC callback, LPARAM user) noexcept {
    if (callback == nullptr) return FALSE;
    if (is_window(window_handle())) (void)callback(window_handle(), user);
    return TRUE;
}

BOOL GetClientRect(HWND window, RECT* rect) noexcept {
    if (rect == nullptr || window != window_handle()) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    if (state().view == nullptr) return FALSE;
    *rect = RECT{0, 0, state().width, state().height};
    return TRUE;
}

BOOL ScreenToClient(HWND window, POINT*) noexcept { return is_window(window) ? TRUE : FALSE; }

int GetClassNameW(HWND window, wchar_t* name, int length) noexcept {
    if (!is_window(window) || name == nullptr || length <= 0) return 0;
    const wchar_t kClass[] = L"NebulaRuntimeWndR";
    const int count = std::min<int>(length - 1, static_cast<int>(std::wcslen(kClass)));
    std::wmemcpy(name, kClass, static_cast<std::size_t>(count));
    name[count] = L'\0';
    return count;
}

int lstrcmpW(const wchar_t* left, const wchar_t* right) noexcept {
    return std::wcscmp(left != nullptr ? left : L"", right != nullptr ? right : L"");
}

short GetAsyncKeyState(int virtual_key) noexcept {
    if (virtual_key <= 0 || virtual_key >= 256) return 0;
    return keys()[static_cast<std::size_t>(virtual_key)].load(std::memory_order_acquire)
               ? static_cast<short>(0x8000)
               : static_cast<short>(0);
}

// ---------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------

DWORD XInputGetState(DWORD user_index, XINPUT_STATE* out) noexcept {
    if (user_index >= XUSER_MAX_COUNT || out == nullptr) return ERROR_BAD_ARGUMENTS;
    const std::lock_guard<std::mutex> lock(state().mutex);
    const auto& pad = state().pads[user_index];
    if (!pad.connected) return ERROR_DEVICE_NOT_CONNECTED;
    out->dwPacketNumber = state().packets[user_index];
    out->Gamepad.wButtons = pad.buttons;
    out->Gamepad.bLeftTrigger = pad.left_trigger;
    out->Gamepad.bRightTrigger = pad.right_trigger;
    out->Gamepad.sThumbLX = pad.left_x;
    out->Gamepad.sThumbLY = pad.left_y;
    out->Gamepad.sThumbRX = pad.right_x;
    out->Gamepad.sThumbRY = pad.right_y;
    return ERROR_SUCCESS;
}

DWORD XInputSetState(DWORD user_index, XINPUT_VIBRATION* vibration) noexcept {
    if (user_index >= XUSER_MAX_COUNT || vibration == nullptr) return ERROR_BAD_ARGUMENTS;
    const std::lock_guard<std::mutex> lock(state().mutex);
    if (!state().pads[user_index].connected) return ERROR_DEVICE_NOT_CONNECTED;
    state().rumble[user_index] = {vibration->wLeftMotorSpeed, vibration->wRightMotorSpeed};
    return ERROR_SUCCESS;
}

#endif  // !_WIN32
