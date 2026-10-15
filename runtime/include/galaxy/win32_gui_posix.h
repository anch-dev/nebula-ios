// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Win32 window-system subset the renderer uses (window class and
// creation, the message loop, cursor capture and focus, monitors, display
// settings, DPI, power requests), for Apple/Linux hosts.
//
// There is one window: the app shell's view (galaxy/host_platform.h), or,
// without a shell (tests), a headless window of the size it was created
// with. What the shell reports arrives as window messages (WM_SIZE,
// WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_KEYDOWN, WM_SETFOCUS, ...) posted to the
// window's thread and delivered to the registered window procedure by the
// same GetMessageW/DispatchMessageW loop the renderer's window thread runs on
// Windows. There is one monitor: the shell's screen.

#if !defined(_WIN32)

#include <cstdint>

#include "galaxy/win32_host_posix.h"

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------
typedef void* HMONITOR;
typedef void* HCURSOR;
typedef void* HICON;
typedef void* HBRUSH;
typedef void* HINSTANCE;
typedef void* HMENU;
typedef void* HGDIOBJ;
typedef std::intptr_t LRESULT;
typedef RECT* LPRECT;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (*MONITORENUMPROC)(HMONITOR, void* hdc, LPRECT, LPARAM);
typedef unsigned short ATOM;

struct MSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
};
typedef MSG* LPMSG;

struct WNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
};

struct WNDCLASSW {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
};

struct MONITORINFO {
    DWORD cbSize;
    RECT rcMonitor;
    RECT rcWork;
    DWORD dwFlags;
};
struct MONITORINFOEXW : MONITORINFO {
    WCHAR szDevice[32];
};

struct DEVMODEW {
    WCHAR dmDeviceName[32];
    WORD dmSpecVersion;
    WORD dmDriverVersion;
    WORD dmSize;
    WORD dmDriverExtra;
    DWORD dmFields;
    POINT dmPosition;
    DWORD dmDisplayOrientation;
    DWORD dmDisplayFixedOutput;
    DWORD dmBitsPerPel;
    DWORD dmPelsWidth;
    DWORD dmPelsHeight;
    DWORD dmDisplayFlags;
    DWORD dmDisplayFrequency;
};

struct TRACKMOUSEEVENT {
    DWORD cbSize;
    DWORD dwFlags;
    HWND hwndTrack;
    DWORD dwHoverTime;
};

struct MEMORYSTATUSEX {
    DWORD dwLength;
    DWORD dwMemoryLoad;
    std::uint64_t ullTotalPhys;
    std::uint64_t ullAvailPhys;
    std::uint64_t ullTotalPageFile;
    std::uint64_t ullAvailPageFile;
    std::uint64_t ullTotalVirtual;
    std::uint64_t ullAvailVirtual;
    std::uint64_t ullAvailExtendedVirtual;
};

enum INPUT_MESSAGE_DEVICE_TYPE {
    IMDT_UNAVAILABLE = 0, IMDT_KEYBOARD = 1, IMDT_MOUSE = 2, IMDT_TOUCH = 4, IMDT_PEN = 8
};
enum INPUT_MESSAGE_ORIGIN_ID {
    IMO_UNAVAILABLE = 0, IMO_HARDWARE = 1, IMO_INJECTED = 2, IMO_SYSTEM = 4
};
struct INPUT_MESSAGE_SOURCE {
    INPUT_MESSAGE_DEVICE_TYPE deviceType;
    INPUT_MESSAGE_ORIGIN_ID originId;
};

struct REASON_CONTEXT {
    ULONG Version;
    DWORD Flags;
    union {
        struct {
            HMODULE LocalizedReasonModule;
            ULONG LocalizedReasonId;
            ULONG ReasonStringCount;
            LPWSTR* ReasonStrings;
        } Detailed;
        LPWSTR SimpleReasonString;
    } Reason;
};
enum POWER_REQUEST_TYPE {
    PowerRequestDisplayRequired = 0,
    PowerRequestSystemRequired = 1,
    PowerRequestAwayModeRequired = 2,
    PowerRequestExecutionRequired = 3
};

// Only the fields the renderer reads; DwmGetCompositionTimingInfo itself does
// not exist on these hosts (LoadLibraryW("dwmapi.dll") fails).
struct DWM_TIMING_INFO {
    std::uint32_t cbSize;
    std::uint64_t qpcRefreshPeriod;
    std::uint64_t qpcVBlank;
};

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define LOWORD(l) (static_cast<WORD>(static_cast<std::uintptr_t>(l) & 0xffff))
#define HIWORD(l) (static_cast<WORD>((static_cast<std::uintptr_t>(l) >> 16) & 0xffff))
#define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))
#define GET_XBUTTON_WPARAM(wp) (HIWORD(wp))
#define MAKELPARAM(l, h) (static_cast<LPARAM>(static_cast<DWORD>(static_cast<WORD>(l)) | \
                          (static_cast<DWORD>(static_cast<WORD>(h)) << 16)))
#define MAKEINTRESOURCEW(i) (reinterpret_cast<LPWSTR>(static_cast<std::uintptr_t>(static_cast<WORD>(i))))
#define MAKEINTRESOURCEA(i) (reinterpret_cast<char*>(static_cast<std::uintptr_t>(static_cast<WORD>(i))))
#define CW_USEDEFAULT (static_cast<int>(0x80000000))

#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_ACTIVATEAPP 0x001C
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCOMMAND 0x0112
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_CAPTURECHANGED 0x0215
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_MOUSELEAVE 0x02A3
#define WM_DPICHANGED 0x02E0
#define WM_USER 0x0400
#define WM_APP 0x8000

#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define MK_XBUTTON1 0x0020
#define MK_XBUTTON2 0x0040
#define XBUTTON1 0x0001
#define XBUTTON2 0x0002

#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2

#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | \
                             WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_NOACTIVATE 0x08000000L

#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_OWNDC 0x0020

#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define GWLP_WNDPROC (-4)

#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOOWNERZORDER 0x0200
#define HWND_TOP (reinterpret_cast<HWND>(static_cast<std::intptr_t>(0)))
#define HWND_TOPMOST (reinterpret_cast<HWND>(static_cast<std::intptr_t>(-1)))
#define HWND_NOTOPMOST (reinterpret_cast<HWND>(static_cast<std::intptr_t>(-2)))

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_RESTORE 9

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_REMOTESESSION 0x1000

#define MONITOR_DEFAULTTONULL 0x00000000
#define MONITOR_DEFAULTTOPRIMARY 0x00000001
#define MONITOR_DEFAULTTONEAREST 0x00000002
#define MONITORINFOF_PRIMARY 0x00000001
#define ENUM_CURRENT_SETTINGS (static_cast<DWORD>(-1))
#define ENUM_REGISTRY_SETTINGS (static_cast<DWORD>(-2))
#define DM_INTERLACED 0x00000002
#define DM_BITSPERPEL 0x00040000L
#define DM_PELSWIDTH 0x00080000L
#define DM_PELSHEIGHT 0x00100000L
#define DM_DISPLAYFREQUENCY 0x00400000L

#define TME_LEAVE 0x00000002
#define HTCLIENT 1
#define IDC_ARROW (MAKEINTRESOURCEA(32512))
#define IMAGE_ICON 1
#define LR_DEFAULTCOLOR 0x00000000
#define LR_SHARED 0x00008000
#define BLACK_BRUSH 4
#define SMTO_NORMAL 0x0000
#define SMTO_BLOCK 0x0001
#define SMTO_ABORTIFHUNG 0x0002
#define MAPVK_VK_TO_VSC 0
#define MAPVK_VSC_TO_VK 1
#define MAPVK_VK_TO_CHAR 2
#define MAPVK_VSC_TO_VK_EX 3
#define USER_DEFAULT_SCREEN_DPI 96
#define POWER_REQUEST_CONTEXT_VERSION 0
#define POWER_REQUEST_CONTEXT_SIMPLE_STRING 0x00000001
#define CP_ACP 0
#define CP_UTF8 65001

// ---------------------------------------------------------------------------
// Window classes, windows and messages
// ---------------------------------------------------------------------------
ATOM RegisterClassExW(const WNDCLASSEXW* window_class) noexcept;
ATOM RegisterClassW(const WNDCLASSW* window_class) noexcept;
BOOL UnregisterClassW(LPCWSTR class_name, HINSTANCE instance) noexcept;
// Creates the one window (the shell's view, or a headless one) on the
// calling thread, which becomes the window's thread, and sends WM_CREATE.
HWND CreateWindowExW(DWORD ex_style, LPCWSTR class_name, LPCWSTR title, DWORD style, int x,
                     int y, int width, int height, HWND parent, HMENU menu,
                     HINSTANCE instance, LPVOID param) noexcept;
BOOL DestroyWindow(HWND window) noexcept;
BOOL ShowWindow(HWND window, int command) noexcept;
BOOL ShowWindowAsync(HWND window, int command) noexcept;
BOOL UpdateWindow(HWND window) noexcept;
BOOL SetForegroundWindow(HWND window) noexcept;
HWND SetActiveWindow(HWND window) noexcept;
HWND SetFocus(HWND window) noexcept;
LRESULT DefWindowProcW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
std::intptr_t SetWindowLongPtrW(HWND window, int index, std::intptr_t value) noexcept;
std::intptr_t GetWindowLongPtrW(HWND window, int index) noexcept;
BOOL SetWindowPos(HWND window, HWND insert_after, int x, int y, int cx, int cy,
                  UINT flags) noexcept;
BOOL GetWindowRect(HWND window, LPRECT rect) noexcept;
BOOL AdjustWindowRectExForDpi(LPRECT rect, DWORD style, BOOL menu, DWORD ex_style,
                              UINT dpi) noexcept;

inline constexpr UINT PM_NOREMOVE = 0x0000;
inline constexpr UINT PM_REMOVE = 0x0001;
// Messages for the calling thread: its window's queue, plus thread messages.
BOOL GetMessageW(LPMSG message, HWND window, UINT filter_min, UINT filter_max) noexcept;
BOOL PeekMessageW(LPMSG message, HWND window, UINT filter_min, UINT filter_max,
                  UINT remove) noexcept;
BOOL TranslateMessage(const MSG* message) noexcept;
LRESULT DispatchMessageW(const MSG* message) noexcept;
BOOL PostMessageW(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
BOOL PostThreadMessageW(DWORD thread_id, UINT message, WPARAM wparam, LPARAM lparam) noexcept;
void PostQuitMessage(int exit_code) noexcept;
// Runs the window procedure on the window's thread (directly when called
// there); waits up to `timeout` milliseconds.
LRESULT SendMessageTimeoutW(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                            UINT flags, UINT timeout, DWORD_PTR* result) noexcept;
LONG GetMessageTime() noexcept;
BOOL GetCurrentInputMessageSource(INPUT_MESSAGE_SOURCE* source) noexcept;

// ---------------------------------------------------------------------------
// Cursor, capture, keys, resources
// ---------------------------------------------------------------------------
HCURSOR LoadCursorA(HINSTANCE instance, const char* name) noexcept;
HCURSOR SetCursor(HCURSOR cursor) noexcept;
HWND SetCapture(HWND window) noexcept;
BOOL ReleaseCapture() noexcept;
HWND GetCapture() noexcept;
BOOL TrackMouseEvent(TRACKMOUSEEVENT* event) noexcept;
UINT MapVirtualKeyA(UINT code, UINT map_type) noexcept;
HANDLE LoadImageW(HINSTANCE instance, LPCWSTR name, UINT type, int cx, int cy,
                  UINT load) noexcept;
HGDIOBJ GetStockObject(int object) noexcept;
int FillRect(void* hdc, const RECT* rect, HBRUSH brush) noexcept;

// ---------------------------------------------------------------------------
// Monitors, display settings, DPI, system
// ---------------------------------------------------------------------------
int GetSystemMetrics(int index) noexcept;
HMONITOR MonitorFromWindow(HWND window, DWORD flags) noexcept;
HMONITOR MonitorFromPoint(POINT point, DWORD flags) noexcept;
BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO* info) noexcept;
BOOL EnumDisplayMonitors(void* hdc, const RECT* clip, MONITORENUMPROC callback,
                         LPARAM data) noexcept;
BOOL EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* devmode) noexcept;
BOOL EnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW* devmode,
                            DWORD flags) noexcept;
UINT GetDpiForWindow(HWND window) noexcept;
UINT GetDpiForSystem() noexcept;
BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* status) noexcept;
HANDLE PowerCreateRequest(REASON_CONTEXT* context) noexcept;
BOOL PowerSetRequest(HANDLE request, POWER_REQUEST_TYPE type) noexcept;
BOOL PowerClearRequest(HANDLE request, POWER_REQUEST_TYPE type) noexcept;

// ---------------------------------------------------------------------------
// Strings and waits
// ---------------------------------------------------------------------------
int WideCharToMultiByte(UINT code_page, DWORD flags, const wchar_t* wide, int wide_length,
                        char* narrow, int narrow_size, const char* default_char,
                        BOOL* used_default) noexcept;
int MultiByteToWideChar(UINT code_page, DWORD flags, const char* narrow, int narrow_length,
                        wchar_t* wide, int wide_size) noexcept;
DWORD WaitForSingleObjectEx(HANDLE handle, DWORD milliseconds, BOOL alertable) noexcept;

// ---------------------------------------------------------------------------
// The app shell (through galaxy/host_platform.h) and tests post input here,
// as the system posts it to a Windows window.
// ---------------------------------------------------------------------------
namespace galaxy::win32_gui {
// False when no window exists yet.
bool post_window_message(UINT message, WPARAM wparam, LPARAM lparam) noexcept;
}  // namespace galaxy::win32_gui

#endif
