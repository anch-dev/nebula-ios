// SPDX-License-Identifier: GPL-3.0-only
// POSIX implementations behind galaxy/win32_host_posix.h and the input state
// an app shell publishes through galaxy/host_platform.h.

#if !defined(_WIN32)

#include "galaxy/win32_host_posix.h"
#include "galaxy/win32_gui_posix.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
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

// One thread's message queue (GetMessageW/PeekMessageW).
struct SentMessage;
struct ThreadQueue {
    std::deque<MSG> messages;
    std::condition_variable changed;
};

// A message sent to the window from another thread (SendMessageTimeoutW),
// run by the window thread's next GetMessageW/PeekMessageW.
struct SentMessage {
    UINT message = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
    LRESULT result = 0;
    bool done = false;
};

struct HostState {
    std::mutex mutex;
    // The app shell's view.
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
    // The screen (monitor).
    int screen_width = 0;
    int screen_height = 0;
    unsigned refresh_hz = 60u;
    float scale = 1.0f;

    // The window created by CreateWindowExW.
    bool created = false;
    std::unordered_map<std::wstring, WNDCLASSEXW> classes;
    WNDPROC procedure = nullptr;
    std::intptr_t user_data = 0;
    DWORD style = 0;
    DWORD ex_style = 0;
    int headless_x = 0;
    int headless_y = 0;
    int headless_width = 0;
    int headless_height = 0;
    bool shown = false;
    bool headless_focus = false;
    DWORD window_thread = 0;
    HWND capture = nullptr;
    bool tracking_leave = false;
    std::unordered_map<DWORD, std::shared_ptr<ThreadQueue>> queues;
    std::condition_variable sent_done;
};

HostState& state() {
    static HostState* host = new HostState;  // used during static destruction
    return *host;
}

std::array<std::atomic<bool>, 256>& keys() {
    static std::array<std::atomic<bool>, 256> down{};
    return down;
}

bool window_exists_locked() noexcept { return state().view != nullptr || state().created; }

bool is_window(HWND window) noexcept {
    if (window != window_handle()) return false;
    const std::lock_guard<std::mutex> lock(state().mutex);
    return window_exists_locked();
}

// Client size: the shell's view, else the headless window's.
void client_size_locked(int& width, int& height) noexcept {
    if (state().view != nullptr) {
        width = state().width;
        height = state().height;
    } else {
        width = state().headless_width;
        height = state().headless_height;
    }
}

std::shared_ptr<ThreadQueue> queue_locked(DWORD thread) {
    auto& queue = state().queues[thread];
    if (queue == nullptr) queue = std::make_shared<ThreadQueue>();
    return queue;
}

// Posts to the window thread's queue; false without a window thread.
bool post_to_window_locked(UINT message, WPARAM wparam, LPARAM lparam) {
    if (!state().created || state().window_thread == 0u) return false;
    const std::shared_ptr<ThreadQueue> queue = queue_locked(state().window_thread);
    MSG msg{};
    msg.hwnd = window_handle();
    msg.message = message;
    msg.wParam = wparam;
    msg.lParam = lparam;
    msg.time = static_cast<DWORD>(GetTickCount64());
    msg.pt = POINT{state().pointer_x, state().pointer_y};
    queue->messages.push_back(msg);
    queue->changed.notify_all();
    return true;
}

WPARAM mouse_key_state() noexcept {
    WPARAM state_bits = 0;
    if (keys()[0x01].load(std::memory_order_acquire)) state_bits |= MK_LBUTTON;
    if (keys()[0x02].load(std::memory_order_acquire)) state_bits |= MK_RBUTTON;
    if (keys()[0x04].load(std::memory_order_acquire)) state_bits |= MK_MBUTTON;
    if (keys()[0x05].load(std::memory_order_acquire)) state_bits |= MK_XBUTTON1;
    if (keys()[0x06].load(std::memory_order_acquire)) state_bits |= MK_XBUTTON2;
    return state_bits;
}

FILETIME filetime_from_ns(std::uint64_t nanoseconds) noexcept {
    const std::uint64_t ticks = nanoseconds / 100u;
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32u)};
}

}  // namespace

namespace galaxy::host_platform {

void set_window(void* view, int width, int height, bool visible, bool focused) {
    const std::lock_guard<std::mutex> lock(state().mutex);
    const bool resized = width != state().width || height != state().height;
    const bool focus_changed = focused != state().focused;
    state().view = view;
    state().width = width;
    state().height = height;
    state().visible = visible;
    state().focused = focused;
    if (resized) {
        (void)post_to_window_locked(WM_SIZE, SIZE_RESTORED, MAKELPARAM(width, height));
    }
    if (focus_changed) {
        (void)post_to_window_locked(WM_ACTIVATEAPP, focused ? TRUE : FALSE, 0);
        (void)post_to_window_locked(focused ? WM_SETFOCUS : WM_KILLFOCUS, 0, 0);
    }
}

void set_screen(int width, int height, unsigned refresh_hz, float scale) {
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().screen_width = width;
    state().screen_height = height;
    state().refresh_hz = refresh_hz != 0u ? refresh_hz : 60u;
    state().scale = scale > 0.0f ? scale : 1.0f;
}

void set_pointer(bool inside, int x, int y) {
    const std::lock_guard<std::mutex> lock(state().mutex);
    const bool left = state().pointer_inside && !inside;
    const bool moved = inside && (!state().pointer_inside || x != state().pointer_x ||
                                  y != state().pointer_y);
    state().pointer_inside = inside;
    state().pointer_x = x;
    state().pointer_y = y;
    if (moved) (void)post_to_window_locked(WM_MOUSEMOVE, mouse_key_state(), MAKELPARAM(x, y));
    if (left && state().tracking_leave) {
        state().tracking_leave = false;
        (void)post_to_window_locked(WM_MOUSELEAVE, 0, 0);
    }
}

void set_key(int virtual_key, bool down) {
    if (virtual_key <= 0 || virtual_key >= 256) return;
    const bool was_down =
        keys()[static_cast<std::size_t>(virtual_key)].exchange(down, std::memory_order_acq_rel);
    if (was_down == down) return;
    const std::lock_guard<std::mutex> lock(state().mutex);
    const LPARAM position = MAKELPARAM(state().pointer_x, state().pointer_y);
    switch (virtual_key) {
    case 0x01:
        (void)post_to_window_locked(down ? WM_LBUTTONDOWN : WM_LBUTTONUP, mouse_key_state(), position);
        break;
    case 0x02:
        (void)post_to_window_locked(down ? WM_RBUTTONDOWN : WM_RBUTTONUP, mouse_key_state(), position);
        break;
    case 0x04:
        (void)post_to_window_locked(down ? WM_MBUTTONDOWN : WM_MBUTTONUP, mouse_key_state(), position);
        break;
    case 0x05:
    case 0x06:
        (void)post_to_window_locked(down ? WM_XBUTTONDOWN : WM_XBUTTONUP,
                                    mouse_key_state() |
                                        (static_cast<WPARAM>(virtual_key == 0x05 ? XBUTTON1 : XBUTTON2) << 16),
                                    position);
        break;
    default: {
        // lParam: repeat count 1; bit 30 previous state; bit 31 transition.
        const LPARAM flags = 1 | (down ? 0 : (LPARAM{1} << 30) | (LPARAM{1} << 31));
        (void)post_to_window_locked(down ? WM_KEYDOWN : WM_KEYUP, static_cast<WPARAM>(virtual_key),
                                    flags);
        break;
    }
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
    if (state().view != nullptr) return state().focused ? window_handle() : nullptr;
    return state().created && state().headless_focus ? window_handle() : nullptr;
}

BOOL IsWindow(HWND window) noexcept { return is_window(window) ? TRUE : FALSE; }

BOOL IsWindowVisible(HWND window) noexcept {
    if (window != window_handle()) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    if (state().view != nullptr) return state().visible ? TRUE : FALSE;
    return state().created && state().shown ? TRUE : FALSE;
}

DWORD GetWindowThreadProcessId(HWND window, DWORD* process_id) noexcept {
    if (!is_window(window)) {
        if (process_id != nullptr) *process_id = 0;
        return 0;
    }
    if (process_id != nullptr) *process_id = GetCurrentProcessId();
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().window_thread != 0u ? state().window_thread : GetCurrentThreadId();
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
    if (!window_exists_locked() || !state().pointer_inside) return nullptr;
    int width = 0, height = 0;
    client_size_locked(width, height);
    if (point.x < 0 || point.y < 0 || point.x >= width || point.y >= height) return nullptr;
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
    if (!window_exists_locked()) return FALSE;
    int width = 0, height = 0;
    client_size_locked(width, height);
    *rect = RECT{0, 0, width, height};
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
// Window classes, creation, messages (galaxy/win32_gui_posix.h)
// ---------------------------------------------------------------------------

namespace {

thread_local LONG t_message_time = 0;

LRESULT call_procedure(UINT message, WPARAM wparam, LPARAM lparam) {
    WNDPROC procedure = nullptr;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        procedure = state().procedure;
    }
    return procedure != nullptr ? procedure(window_handle(), message, wparam, lparam)
                                : DefWindowProcW(window_handle(), message, wparam, lparam);
}

constexpr UINT kSentMessage = 0x7FFF0001u;  // internal: a SentMessage in lParam

// Takes the next message for this thread, running sent messages on the way.
bool next_message(MSG& out, UINT filter_min, UINT filter_max, bool remove, bool wait) {
    const DWORD thread = GetCurrentThreadId();
    std::unique_lock<std::mutex> lock(state().mutex);
    const std::shared_ptr<ThreadQueue> queue = queue_locked(thread);
    for (;;) {
        for (auto it = queue->messages.begin(); it != queue->messages.end(); ++it) {
            if (it->message == kSentMessage) {
                auto* sent = reinterpret_cast<std::shared_ptr<SentMessage>*>(it->lParam);
                std::shared_ptr<SentMessage> message = std::move(*sent);
                delete sent;
                queue->messages.erase(it);
                lock.unlock();
                const LRESULT result = call_procedure(message->message, message->wparam,
                                                      message->lparam);
                lock.lock();
                message->result = result;
                message->done = true;
                state().sent_done.notify_all();
                goto rescan;
            }
            const bool in_range = (filter_min == 0u && filter_max == 0u) ||
                                  (it->message >= filter_min && it->message <= filter_max) ||
                                  it->message == WM_QUIT;
            if (!in_range) continue;
            out = *it;
            if (remove) queue->messages.erase(it);
            return true;
        }
        if (!wait) return false;
        queue->changed.wait(lock);
    rescan:;
    }
}

}  // namespace

ATOM RegisterClassExW(const WNDCLASSEXW* window_class) noexcept {
    if (window_class == nullptr || window_class->lpszClassName == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().classes[window_class->lpszClassName] = *window_class;
    return static_cast<ATOM>(0xC000u + state().classes.size());
}

ATOM RegisterClassW(const WNDCLASSW* window_class) noexcept {
    if (window_class == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    WNDCLASSEXW extended{};
    extended.cbSize = sizeof(extended);
    extended.style = window_class->style;
    extended.lpfnWndProc = window_class->lpfnWndProc;
    extended.hInstance = window_class->hInstance;
    extended.lpszClassName = window_class->lpszClassName;
    return RegisterClassExW(&extended);
}

BOOL UnregisterClassW(LPCWSTR class_name, HINSTANCE) noexcept {
    if (class_name == nullptr) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().classes.erase(class_name) != 0u ? TRUE : FALSE;
}

HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR, DWORD style, int x, int y,
                     int width, int height, HWND, HMENU, HINSTANCE, LPVOID) noexcept {
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        if (state().created || class_name == nullptr) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
        const auto found = state().classes.find(class_name);
        if (found == state().classes.end()) {
            SetLastError(ERROR_FILE_NOT_FOUND);  // ERROR_CANNOT_FIND_WND_CLASS
            return nullptr;
        }
        state().created = true;
        state().procedure = found->second.lpfnWndProc;
        state().style = style;
        state().ex_style = ex_style;
        state().headless_x = x == CW_USEDEFAULT ? 0 : x;
        state().headless_y = y == CW_USEDEFAULT ? 0 : y;
        state().headless_width = width == CW_USEDEFAULT || width <= 0 ? 1280 : width;
        state().headless_height = height == CW_USEDEFAULT || height <= 0 ? 720 : height;
        state().shown = (style & WS_VISIBLE) != 0;
        state().user_data = 0;
        state().window_thread = GetCurrentThreadId();
        (void)queue_locked(state().window_thread);
    }
    (void)call_procedure(WM_CREATE, 0, 0);
    return window_handle();
}

BOOL DestroyWindow(HWND window) noexcept {
    if (!is_window(window)) return FALSE;
    (void)call_procedure(WM_DESTROY, 0, 0);
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().created = false;
    state().procedure = nullptr;
    state().shown = false;
    state().headless_focus = false;
    state().capture = nullptr;
    return TRUE;
}

BOOL ShowWindow(HWND window, int command) noexcept {
    if (!is_window(window)) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    const BOOL was_visible = state().shown ? TRUE : FALSE;
    state().shown = command != SW_HIDE && command != SW_MINIMIZE;
    return was_visible;
}

BOOL ShowWindowAsync(HWND window, int command) noexcept { return ShowWindow(window, command); }
BOOL UpdateWindow(HWND window) noexcept { return is_window(window) ? TRUE : FALSE; }

BOOL SetForegroundWindow(HWND window) noexcept {
    if (!is_window(window)) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().headless_focus = true;
    return TRUE;
}

HWND SetActiveWindow(HWND window) noexcept { return is_window(window) ? window : nullptr; }

HWND SetFocus(HWND window) noexcept {
    if (!is_window(window)) return nullptr;
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().headless_focus = true;
    return window;
}

LRESULT DefWindowProcW(HWND window, UINT message, WPARAM, LPARAM) noexcept {
    if (message == WM_CLOSE) (void)DestroyWindow(window);
    return 0;
}

std::intptr_t SetWindowLongPtrW(HWND window, int index, std::intptr_t value) noexcept {
    if (!is_window(window)) return 0;
    const std::lock_guard<std::mutex> lock(state().mutex);
    std::intptr_t previous = 0;
    switch (index) {
    case GWLP_USERDATA: previous = state().user_data; state().user_data = value; break;
    case GWL_STYLE: previous = state().style; state().style = static_cast<DWORD>(value); break;
    case GWL_EXSTYLE: previous = state().ex_style; state().ex_style = static_cast<DWORD>(value); break;
    case GWLP_WNDPROC:
        previous = reinterpret_cast<std::intptr_t>(state().procedure);
        state().procedure = reinterpret_cast<WNDPROC>(value);
        break;
    default: break;
    }
    return previous;
}

std::intptr_t GetWindowLongPtrW(HWND window, int index) noexcept {
    if (!is_window(window)) return 0;
    const std::lock_guard<std::mutex> lock(state().mutex);
    switch (index) {
    case GWLP_USERDATA: return state().user_data;
    case GWL_STYLE: return static_cast<std::intptr_t>(state().style);
    case GWL_EXSTYLE: return static_cast<std::intptr_t>(state().ex_style);
    case GWLP_WNDPROC: return reinterpret_cast<std::intptr_t>(state().procedure);
    default: return 0;
    }
}

BOOL SetWindowPos(HWND window, HWND, int x, int y, int cx, int cy, UINT flags) noexcept {
    if (!is_window(window)) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    if ((flags & SWP_SHOWWINDOW) != 0u) state().shown = true;
    if ((flags & SWP_HIDEWINDOW) != 0u) state().shown = false;
    // The shell owns its view's geometry; only a headless window moves.
    if (state().view != nullptr) return TRUE;
    if ((flags & SWP_NOMOVE) == 0u) {
        state().headless_x = x;
        state().headless_y = y;
    }
    if ((flags & SWP_NOSIZE) == 0u && cx > 0 && cy > 0 &&
        (cx != state().headless_width || cy != state().headless_height)) {
        state().headless_width = cx;
        state().headless_height = cy;
        (void)post_to_window_locked(WM_SIZE, SIZE_RESTORED, MAKELPARAM(cx, cy));
    }
    return TRUE;
}

BOOL GetWindowRect(HWND window, LPRECT rect) noexcept {
    if (rect == nullptr || !is_window(window)) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    int width = 0, height = 0;
    client_size_locked(width, height);
    const int x = state().view != nullptr ? 0 : state().headless_x;
    const int y = state().view != nullptr ? 0 : state().headless_y;
    *rect = RECT{x, y, x + width, y + height};
    return TRUE;
}

BOOL AdjustWindowRectExForDpi(LPRECT rect, DWORD, BOOL, DWORD, UINT) noexcept {
    return rect != nullptr ? TRUE : FALSE;  // no frame: client == window
}

BOOL GetMessageW(LPMSG message, HWND, UINT filter_min, UINT filter_max) noexcept {
    if (message == nullptr) return -1;
    MSG next{};
    (void)next_message(next, filter_min, filter_max, true, true);
    *message = next;
    t_message_time = static_cast<LONG>(next.time);
    return next.message == WM_QUIT ? FALSE : TRUE;
}

BOOL PeekMessageW(LPMSG message, HWND, UINT filter_min, UINT filter_max, UINT remove) noexcept {
    if (message == nullptr) return FALSE;
    MSG next{};
    if (!next_message(next, filter_min, filter_max, (remove & PM_REMOVE) != 0u, false)) {
        return FALSE;
    }
    *message = next;
    t_message_time = static_cast<LONG>(next.time);
    return TRUE;
}

BOOL TranslateMessage(const MSG*) noexcept { return FALSE; }

LRESULT DispatchMessageW(const MSG* message) noexcept {
    if (message == nullptr || message->hwnd != window_handle() || !is_window(message->hwnd)) {
        return 0;
    }
    return call_procedure(message->message, message->wParam, message->lParam);
}

BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    if (window != window_handle()) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    return post_to_window_locked(message, wparam, lparam) ? TRUE : FALSE;
}

BOOL PostThreadMessageW(DWORD thread, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    const std::shared_ptr<ThreadQueue> queue = queue_locked(thread);
    MSG msg{};
    msg.message = message;
    msg.wParam = wparam;
    msg.lParam = lparam;
    msg.time = static_cast<DWORD>(GetTickCount64());
    queue->messages.push_back(msg);
    queue->changed.notify_all();
    return TRUE;
}

void PostQuitMessage(int exit_code) noexcept {
    (void)PostThreadMessageW(GetCurrentThreadId(), WM_QUIT, static_cast<WPARAM>(exit_code), 0);
}

LRESULT SendMessageTimeoutW(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT,
                            UINT timeout, DWORD_PTR* result) noexcept {
    if (!is_window(window)) return 0;
    DWORD window_thread = 0;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        window_thread = state().window_thread;
    }
    if (window_thread == 0u || window_thread == GetCurrentThreadId()) {
        const LRESULT value = call_procedure(message, wparam, lparam);
        if (result != nullptr) *result = static_cast<DWORD_PTR>(value);
        return 1;
    }
    auto sent = std::make_shared<SentMessage>();
    sent->message = message;
    sent->wparam = wparam;
    sent->lparam = lparam;
    std::unique_lock<std::mutex> lock(state().mutex);
    if (!post_to_window_locked(kSentMessage, 0,
                               reinterpret_cast<LPARAM>(new std::shared_ptr<SentMessage>(sent)))) {
        return 0;
    }
    const bool done = state().sent_done.wait_for(
        lock, std::chrono::milliseconds(timeout), [&] { return sent->done; });
    if (!done) {
        SetLastError(ERROR_TIMEOUT);
        return 0;
    }
    if (result != nullptr) *result = static_cast<DWORD_PTR>(sent->result);
    return 1;
}

LONG GetMessageTime() noexcept { return t_message_time; }

BOOL GetCurrentInputMessageSource(INPUT_MESSAGE_SOURCE* source) noexcept {
    if (source == nullptr) return FALSE;
    source->deviceType = IMDT_MOUSE;
    source->originId = IMO_HARDWARE;
    return TRUE;
}

// ---------------------------------------------------------------------------
// Cursor, capture, keys, resources
// ---------------------------------------------------------------------------

namespace {
int g_cursor_identity = 0;
int g_icon_identity = 0;
int g_brush_identity = 0;
}  // namespace

HCURSOR LoadCursorA(HINSTANCE, const char*) noexcept { return &g_cursor_identity; }
HCURSOR SetCursor(HCURSOR cursor) noexcept { return cursor; }

HWND SetCapture(HWND window) noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    const HWND previous = state().capture;
    state().capture = window;
    return previous;
}

BOOL ReleaseCapture() noexcept {
    bool had = false;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        had = state().capture != nullptr;
        state().capture = nullptr;
    }
    if (had) (void)call_procedure(WM_CAPTURECHANGED, 0, 0);
    return TRUE;
}

HWND GetCapture() noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    return state().capture;
}

BOOL TrackMouseEvent(TRACKMOUSEEVENT* event) noexcept {
    if (event == nullptr) return FALSE;
    const std::lock_guard<std::mutex> lock(state().mutex);
    state().tracking_leave = (event->dwFlags & TME_LEAVE) != 0u;
    return TRUE;
}

UINT MapVirtualKeyA(UINT, UINT) noexcept { return 0u; }  // no scan codes on these hosts

HANDLE LoadImageW(HINSTANCE, LPCWSTR, UINT, int, int, UINT) noexcept { return &g_icon_identity; }
HGDIOBJ GetStockObject(int) noexcept { return &g_brush_identity; }
int FillRect(void*, const RECT*, HBRUSH) noexcept { return 1; }

// ---------------------------------------------------------------------------
// Monitors, display settings, DPI, system
// ---------------------------------------------------------------------------

namespace {
int g_monitor_identity = 0;

void screen_locked(int& width, int& height) noexcept {
    width = state().screen_width;
    height = state().screen_height;
    if (width <= 0 || height <= 0) client_size_locked(width, height);
    if (width <= 0 || height <= 0) {
        width = 1920;
        height = 1080;
    }
}
}  // namespace

int GetSystemMetrics(int index) noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    int width = 0, height = 0;
    screen_locked(width, height);
    switch (index) {
    case SM_CXSCREEN: return width;
    case SM_CYSCREEN: return height;
    case SM_CXICON:
    case SM_CYICON: return 32;
    case SM_CXSMICON:
    case SM_CYSMICON: return 16;
    default: return 0;
    }
}

HMONITOR MonitorFromWindow(HWND, DWORD) noexcept { return &g_monitor_identity; }
HMONITOR MonitorFromPoint(POINT, DWORD) noexcept { return &g_monitor_identity; }

BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO* info) noexcept {
    if (monitor != &g_monitor_identity || info == nullptr) return FALSE;
    int width = 0, height = 0;
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        screen_locked(width, height);
    }
    info->rcMonitor = RECT{0, 0, width, height};
    info->rcWork = info->rcMonitor;
    info->dwFlags = MONITORINFOF_PRIMARY;
    if (info->cbSize >= sizeof(MONITORINFOEXW)) {
        const wchar_t kDevice[] = L"\\\\.\\DISPLAY1";
        std::wmemcpy(static_cast<MONITORINFOEXW*>(info)->szDevice, kDevice,
                     sizeof(kDevice) / sizeof(wchar_t));
    }
    return TRUE;
}

BOOL EnumDisplayMonitors(void*, const RECT*, MONITORENUMPROC callback, LPARAM data) noexcept {
    if (callback == nullptr) return FALSE;
    RECT bounds{};
    {
        const std::lock_guard<std::mutex> lock(state().mutex);
        int width = 0, height = 0;
        screen_locked(width, height);
        bounds = RECT{0, 0, width, height};
    }
    (void)callback(&g_monitor_identity, nullptr, &bounds, data);
    return TRUE;
}

BOOL EnumDisplaySettingsExW(LPCWSTR, DWORD mode, DEVMODEW* devmode, DWORD) noexcept {
    if (devmode == nullptr) return FALSE;
    // One mode: the screen's own.
    if (mode != ENUM_CURRENT_SETTINGS && mode != ENUM_REGISTRY_SETTINGS && mode != 0u) {
        return FALSE;
    }
    const std::lock_guard<std::mutex> lock(state().mutex);
    int width = 0, height = 0;
    screen_locked(width, height);
    devmode->dmBitsPerPel = 32u;
    devmode->dmPelsWidth = static_cast<DWORD>(width);
    devmode->dmPelsHeight = static_cast<DWORD>(height);
    devmode->dmDisplayFlags = 0u;
    devmode->dmDisplayFrequency = state().refresh_hz;
    devmode->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return TRUE;
}

BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* devmode) noexcept {
    return EnumDisplaySettingsExW(device, mode, devmode, 0u);
}

UINT GetDpiForSystem() noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    return static_cast<UINT>(USER_DEFAULT_SCREEN_DPI * state().scale + 0.5f);
}

UINT GetDpiForWindow(HWND window) noexcept { return is_window(window) ? GetDpiForSystem() : 0u; }

BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status) noexcept {
    if (status == nullptr) return FALSE;
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page_size = sysconf(_SC_PAGESIZE);
    const std::uint64_t total = pages > 0 && page_size > 0
        ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size)
        : 0u;
    status->dwMemoryLoad = 0u;
    status->ullTotalPhys = total;
    status->ullAvailPhys = total;
    status->ullTotalPageFile = total;
    status->ullAvailPageFile = total;
    status->ullTotalVirtual = std::uint64_t{1} << 47u;
    status->ullAvailVirtual = std::uint64_t{1} << 47u;
    status->ullAvailExtendedVirtual = 0u;
    return total != 0u ? TRUE : FALSE;
}

// The app shell keeps the display awake (UIApplication.idleTimerDisabled);
// these succeed without effect.
HANDLE PowerCreateRequest(REASON_CONTEXT*) noexcept { return &g_icon_identity; }
BOOL PowerSetRequest(HANDLE, POWER_REQUEST_TYPE) noexcept { return TRUE; }
BOOL PowerClearRequest(HANDLE, POWER_REQUEST_TYPE) noexcept { return TRUE; }

// ---------------------------------------------------------------------------
// Strings and waits
// ---------------------------------------------------------------------------

int WideCharToMultiByte(UINT, DWORD, const wchar_t* wide, int wide_length, char* narrow,
                        int narrow_size, const char*, BOOL* used_default) noexcept {
    if (used_default != nullptr) *used_default = FALSE;
    if (wide == nullptr) return 0;
    try {
        const std::wstring text = wide_length < 0 ? std::wstring(wide)
                                                   : std::wstring(wide, static_cast<std::size_t>(wide_length));
        std::string converted = galaxy::win32::narrow(text.c_str());
        if (wide_length < 0) converted.push_back('\0');
        if (narrow_size == 0 || narrow == nullptr) return static_cast<int>(converted.size());
        if (static_cast<int>(converted.size()) > narrow_size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        std::memcpy(narrow, converted.data(), converted.size());
        return static_cast<int>(converted.size());
    } catch (...) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
}

int MultiByteToWideChar(UINT, DWORD, const char* narrow, int narrow_length, wchar_t* wide,
                        int wide_size) noexcept {
    if (narrow == nullptr) return 0;
    try {
        const std::string text = narrow_length < 0 ? std::string(narrow)
                                                   : std::string(narrow, static_cast<std::size_t>(narrow_length));
        std::wstring converted = galaxy::win32::widen(text.c_str());
        if (narrow_length < 0) converted.push_back(L'\0');
        if (wide_size == 0 || wide == nullptr) return static_cast<int>(converted.size());
        if (static_cast<int>(converted.size()) > wide_size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        std::wmemcpy(wide, converted.data(), converted.size());
        return static_cast<int>(converted.size());
    } catch (...) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
}

DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL) noexcept {
    return WaitForSingleObject(handle, milliseconds);
}

namespace galaxy::win32_gui {
bool post_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    const std::lock_guard<std::mutex> lock(state().mutex);
    return post_to_window_locked(message, wparam, lparam);
}
}  // namespace galaxy::win32_gui

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
