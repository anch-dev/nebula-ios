// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The Win32 file API subset the runtime's disc and save (NAND) layers use,
// emulated on POSIX hosts (Linux, macOS, iOS) with the Win32 semantics those
// layers depend on. On Windows this header only includes <Windows.h>.
//
// Emulated semantics (POSIX):
// - Paths: `const char*` (std::filesystem::path::c_str() on POSIX, UTF-8) or
//   `const wchar_t*` (converted to UTF-8).
// - NTFS named streams: "file:stream" (a ':' in the final path component;
//   guest file names cannot contain ':') names a stream stored as an extended
//   attribute of `file`, so streams follow the file through rename and
//   disappear with it, as on NTFS. Opening a stream for writing buffers the
//   bytes and commits them on FlushFileBuffers/CloseHandle. Stream data is
//   capped at kWin32MaxStreamBytes (the save layer's streams are < 256 bytes).
// - Share modes: enforced between handles of this process (no other process
//   participates), including DELETE sharing for MoveFileExW, DeleteFileW and
//   delete-on-close, so in-process "lease" handles keep working.
// - MoveFileExW without MOVEFILE_REPLACE_EXISTING never overwrites
//   (renameat2 RENAME_NOREPLACE / renamex_np RENAME_EXCL).
// - Identity: dwVolumeSerialNumber is a stable 32-bit hash of the volume's
//   persistent identifier (APFS volume UUID; Linux f_fsid), file index is the
//   inode, directories report one link, and symlinks are reparse points.
// - FileCaseSensitiveInfo is not supported (ERROR_NOT_SUPPORTED).

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#else

#include <cstddef>
#include <cstdint>
#include <string>

#include "galaxy/win32_portable.h"

inline constexpr std::size_t kWin32MaxStreamBytes = 64u * 1024u;

// Access rights, share modes, dispositions and flags (WinNT.h / WinBase.h).
inline constexpr DWORD GENERIC_READ = 0x80000000u;
inline constexpr DWORD GENERIC_WRITE = 0x40000000u;
inline constexpr DWORD GENERIC_EXECUTE = 0x20000000u;
inline constexpr DWORD GENERIC_ALL = 0x10000000u;
inline constexpr DWORD DELETE = 0x00010000u;
inline constexpr DWORD SYNCHRONIZE = 0x00100000u;
inline constexpr DWORD FILE_READ_DATA = 0x0001u;
inline constexpr DWORD FILE_WRITE_DATA = 0x0002u;
inline constexpr DWORD FILE_APPEND_DATA = 0x0004u;
inline constexpr DWORD FILE_READ_ATTRIBUTES = 0x0080u;
inline constexpr DWORD FILE_WRITE_ATTRIBUTES = 0x0100u;

inline constexpr DWORD FILE_SHARE_READ = 0x1u;
inline constexpr DWORD FILE_SHARE_WRITE = 0x2u;
inline constexpr DWORD FILE_SHARE_DELETE = 0x4u;

inline constexpr DWORD CREATE_NEW = 1u;
inline constexpr DWORD CREATE_ALWAYS = 2u;
inline constexpr DWORD OPEN_EXISTING = 3u;
inline constexpr DWORD OPEN_ALWAYS = 4u;
inline constexpr DWORD TRUNCATE_EXISTING = 5u;

inline constexpr DWORD FILE_ATTRIBUTE_READONLY = 0x1u;
inline constexpr DWORD FILE_ATTRIBUTE_HIDDEN = 0x2u;
inline constexpr DWORD FILE_ATTRIBUTE_DIRECTORY = 0x10u;
inline constexpr DWORD FILE_ATTRIBUTE_ARCHIVE = 0x20u;
inline constexpr DWORD FILE_ATTRIBUTE_NORMAL = 0x80u;
inline constexpr DWORD FILE_ATTRIBUTE_TEMPORARY = 0x100u;
inline constexpr DWORD FILE_ATTRIBUTE_REPARSE_POINT = 0x400u;
inline constexpr DWORD INVALID_FILE_ATTRIBUTES = 0xFFFFFFFFu;

inline constexpr DWORD FILE_FLAG_WRITE_THROUGH = 0x80000000u;
inline constexpr DWORD FILE_FLAG_OVERLAPPED = 0x40000000u;
inline constexpr DWORD FILE_FLAG_NO_BUFFERING = 0x20000000u;
inline constexpr DWORD FILE_FLAG_RANDOM_ACCESS = 0x10000000u;
inline constexpr DWORD FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000u;
inline constexpr DWORD FILE_FLAG_DELETE_ON_CLOSE = 0x04000000u;
inline constexpr DWORD FILE_FLAG_BACKUP_SEMANTICS = 0x02000000u;
inline constexpr DWORD FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000u;

inline constexpr DWORD MOVEFILE_REPLACE_EXISTING = 0x1u;
inline constexpr DWORD MOVEFILE_COPY_ALLOWED = 0x2u;
inline constexpr DWORD MOVEFILE_WRITE_THROUGH = 0x8u;

inline constexpr DWORD FILE_NAMED_STREAMS = 0x00040000u;
inline constexpr DWORD FILE_CASE_SENSITIVE_SEARCH = 0x1u;
inline constexpr DWORD FILE_CASE_PRESERVED_NAMES = 0x2u;
inline constexpr DWORD FILE_CS_FLAG_CASE_SENSITIVE_DIR = 0x1u;

inline constexpr DWORD FILE_BEGIN = 0u;
inline constexpr DWORD FILE_CURRENT = 1u;
inline constexpr DWORD FILE_END = 2u;

struct SECURITY_ATTRIBUTES;
struct OVERLAPPED {
    ULONG_PTR Internal;
    ULONG_PTR InternalHigh;
    union {
        struct {
            DWORD Offset;
            DWORD OffsetHigh;
        };
        PVOID Pointer;
    };
    HANDLE hEvent;
};

struct BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD dwVolumeSerialNumber;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD nNumberOfLinks;
    DWORD nFileIndexHigh;
    DWORD nFileIndexLow;
};

struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
};

enum GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard };

enum FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo = 0,
    FileStandardInfo = 1,
    FileDispositionInfo = 4,
    FileCaseSensitiveInfo = 23,
};

struct FILE_DISPOSITION_INFO {
    BOOL DeleteFile;
};

struct FILE_CASE_SENSITIVE_INFO {
    ULONG Flags;
};

enum STREAM_INFO_LEVELS { FindStreamInfoStandard };

struct WIN32_FIND_STREAM_DATA {
    LARGE_INTEGER StreamSize;
    WCHAR cStreamName[MAX_PATH + 36];
};

// Paths: UTF-8 (`char`) or wide.
HANDLE CreateFileW(const char* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* security,
                   DWORD disposition, DWORD flags, HANDLE template_file) noexcept;
HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* security,
                   DWORD disposition, DWORD flags, HANDLE template_file) noexcept;
BOOL ReadFile(HANDLE file, void* buffer, DWORD bytes, DWORD* read, OVERLAPPED* overlapped) noexcept;
BOOL WriteFile(HANDLE file, const void* buffer, DWORD bytes, DWORD* written,
               OVERLAPPED* overlapped) noexcept;
BOOL FlushFileBuffers(HANDLE file) noexcept;
BOOL CloseHandle(HANDLE handle) noexcept;
BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER* size) noexcept;
BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, LARGE_INTEGER* new_position,
                      DWORD method) noexcept;
BOOL GetFileInformationByHandle(HANDLE file, BY_HANDLE_FILE_INFORMATION* information) noexcept;
BOOL GetFileInformationByHandleEx(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
                                  void* information, DWORD size) noexcept;
BOOL SetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
                                void* information, DWORD size) noexcept;

DWORD GetFileAttributesW(const char* path) noexcept;
DWORD GetFileAttributesW(const wchar_t* path) noexcept;
BOOL GetFileAttributesExW(const char* path, GET_FILEEX_INFO_LEVELS level, void* information) noexcept;
BOOL GetFileAttributesExW(const wchar_t* path, GET_FILEEX_INFO_LEVELS level,
                          void* information) noexcept;
BOOL DeleteFileW(const char* path) noexcept;
BOOL DeleteFileW(const wchar_t* path) noexcept;
BOOL MoveFileExW(const char* from, const char* to, DWORD flags) noexcept;
BOOL MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD flags) noexcept;
BOOL CreateDirectoryW(const char* path, SECURITY_ATTRIBUTES* security) noexcept;
BOOL CreateDirectoryW(const wchar_t* path, SECURITY_ATTRIBUTES* security) noexcept;
BOOL RemoveDirectoryW(const char* path) noexcept;
BOOL RemoveDirectoryW(const wchar_t* path) noexcept;
BOOL CreateHardLinkW(const char* new_link, const char* existing, SECURITY_ATTRIBUTES* security) noexcept;
BOOL CreateHardLinkW(const wchar_t* new_link, const wchar_t* existing,
                     SECURITY_ATTRIBUTES* security) noexcept;

// Volume queries: GetVolumePathNameW returns the path itself (every POSIX
// query below works on any path inside the volume).
BOOL GetVolumePathNameW(const char* path, wchar_t* volume_root, DWORD length) noexcept;
BOOL GetVolumePathNameW(const wchar_t* path, wchar_t* volume_root, DWORD length) noexcept;
BOOL GetVolumeInformationW(const wchar_t* root, wchar_t* name, DWORD name_length, DWORD* serial,
                           DWORD* maximum_component_length, DWORD* flags,
                           wchar_t* file_system_name, DWORD file_system_name_length) noexcept;

// Stream enumeration: "::$DATA" for a regular file's unnamed stream, then
// ":<name>:$DATA" for each named stream.
HANDLE FindFirstStreamW(const char* path, STREAM_INFO_LEVELS level, void* data, DWORD flags) noexcept;
HANDLE FindFirstStreamW(const wchar_t* path, STREAM_INFO_LEVELS level, void* data,
                        DWORD flags) noexcept;
BOOL FindNextStreamW(HANDLE search, void* data) noexcept;
BOOL FindClose(HANDLE search) noexcept;

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) noexcept;
// Wide CRT environment functions (empty value removes the variable).
int _wputenv_s(const wchar_t* name, const wchar_t* value) noexcept;
int _wdupenv_s(wchar_t** buffer, std::size_t* length, const wchar_t* name) noexcept;
DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size) noexcept;

inline HANDLE CreateFileA(const char* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* security,
                          DWORD disposition, DWORD flags, HANDLE template_file) noexcept {
    return CreateFileW(path, access, share, security, disposition, flags, template_file);
}

// Read-only file mappings (the disc image view).
using SIZE_T = std::size_t;
inline constexpr DWORD PAGE_READONLY = 0x02u;
inline constexpr DWORD FILE_MAP_READ = 0x0004u;
HANDLE CreateFileMappingW(HANDLE file, SECURITY_ATTRIBUTES* security, DWORD protect,
                          DWORD maximum_size_high, DWORD maximum_size_low,
                          const wchar_t* name) noexcept;
void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low,
                    SIZE_T bytes) noexcept;
BOOL UnmapViewOfFile(const void* view) noexcept;
struct WIN32_MEMORY_RANGE_ENTRY {
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
};
BOOL PrefetchVirtualMemory(HANDLE process, ULONG_PTR count, WIN32_MEMORY_RANGE_ENTRY* ranges,
                           ULONG flags) noexcept;

// Events (auto- or manual-reset) and waits on them.
HANDLE CreateEventW(SECURITY_ATTRIBUTES* security, BOOL manual_reset, BOOL initial_state,
                    const wchar_t* name) noexcept;
BOOL SetEvent(HANDLE event) noexcept;
BOOL ResetEvent(HANDLE event) noexcept;
DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds) noexcept;
DWORD WaitForMultipleObjects(DWORD count, const HANDLE* objects, BOOL wait_all,
                             DWORD milliseconds) noexcept;

namespace galaxy::win32 {
// UTF-8 ↔ wide conversions used by the shim (wchar_t is UTF-32 on POSIX).
std::string narrow(const wchar_t* text);
std::wstring widen(const char* text);
// True when `handle` was produced by this shim (any kind).
bool is_shim_handle(HANDLE handle) noexcept;
// The POSIX descriptor behind a file handle, or -1 (stream and lease handles).
int native_descriptor(HANDLE handle) noexcept;
}  // namespace galaxy::win32

#endif
