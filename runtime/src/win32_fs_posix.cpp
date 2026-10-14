// SPDX-License-Identifier: GPL-3.0-only
// POSIX emulation of the Win32 file API subset in galaxy/win32_fs.h. See the
// header for the semantics this preserves and how.

#if !defined(_WIN32)

#include "galaxy/win32_fs.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <climits>
#include <sys/statfs.h>
#include <sys/syscall.h>
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1  // linux/fs.h, which clashes with libc headers
#endif
#endif

#if !defined(ENOATTR)
#define ENOATTR ENODATA
#endif

namespace galaxy::win32 {

// ---------------------------------------------------------------------------
// UTF-8 / UTF-32
// ---------------------------------------------------------------------------

std::string narrow(const wchar_t* text) {
    std::string out;
    if (text == nullptr) return out;
    for (; *text != L'\0'; ++text) {
        auto code = static_cast<std::uint32_t>(*text);
        if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) code = 0xFFFDu;
        if (code < 0x80u) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (code >> 6u)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else if (code < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (code >> 12u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (code >> 18u)));
            out.push_back(static_cast<char>(0x80u | ((code >> 12u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((code >> 6u) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
        }
    }
    return out;
}

std::wstring widen(const char* text) {
    std::wstring out;
    if (text == nullptr) return out;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    while (*bytes != 0u) {
        std::uint32_t code = *bytes;
        int extra = 0;
        if (code >= 0xF0u) { code &= 0x07u; extra = 3; }
        else if (code >= 0xE0u) { code &= 0x0Fu; extra = 2; }
        else if (code >= 0xC0u) { code &= 0x1Fu; extra = 1; }
        else if (code >= 0x80u) { code = 0xFFFDu; }
        ++bytes;
        for (int i = 0; i < extra; ++i) {
            if ((*bytes & 0xC0u) != 0x80u) { code = 0xFFFDu; break; }
            code = (code << 6u) | (*bytes & 0x3Fu);
            ++bytes;
        }
        out.push_back(static_cast<wchar_t>(code));
    }
    return out;
}

namespace {

constexpr std::uint32_t kHandleMagic = 0x57334653u;  // "W3FS"
constexpr const char* kStreamXattrPrefix = "user.galaxy.stream.";
constexpr std::size_t kStreamXattrPrefixLength = 19u;

enum class Kind : std::uint8_t { File, Stream, StreamSearch, Event, Mapping, Timer };

struct ObjectKey {
    dev_t device;
    ino_t inode;
    bool operator<(const ObjectKey& other) const noexcept {
        return std::tie(device, inode) < std::tie(other.device, other.inode);
    }
};

struct Handle {
    std::uint32_t magic = kHandleMagic;
    Kind kind = Kind::File;
    // File / Stream
    int fd = -1;
    std::string path;           // file path (stream: base path)
    std::string stream;         // stream name ("" for the unnamed stream)
    struct stat info {};        // identity at open (fd-less handles)
    ObjectKey key{};
    DWORD access = 0;
    DWORD share = 0;
    DWORD flags = 0;
    bool registered = false;
    bool delete_on_close = false;
    // Stream data
    std::vector<unsigned char> data;
    std::size_t position = 0;
    bool dirty = false;
    // Stream search
    std::vector<std::pair<std::wstring, std::int64_t>> streams;
    std::size_t next = 0;
    // Event and timer (state guarded by event_mutex())
    bool manual_reset = false;
    bool signaled = false;
    bool armed = false;
    std::chrono::steady_clock::time_point due{};
    // Mapping (fd is a duplicate of the file's descriptor)
    std::uint64_t mapping_size = 0;
};

Handle* as_handle(HANDLE handle) noexcept {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) return nullptr;
    auto* const object = static_cast<Handle*>(handle);
    return object->magic == kHandleMagic ? object : nullptr;
}

bool fail(DWORD error) noexcept {
    SetLastError(error);
    return false;
}

BOOL fail_bool(DWORD error) noexcept {
    SetLastError(error);
    return FALSE;
}

HANDLE fail_handle(DWORD error) noexcept {
    SetLastError(error);
    return INVALID_HANDLE_VALUE;
}

// ---------------------------------------------------------------------------
// Events: one mutex/condition pair for every event keeps multi-object waits
// simple; event traffic in the runtime is low.
// ---------------------------------------------------------------------------

std::mutex& event_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::condition_variable& event_condition() {
    static std::condition_variable condition;
    return condition;
}

// View base → mapped length, for UnmapViewOfFile.
std::mutex& view_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<const void*, std::size_t>& views() {
    static std::unordered_map<const void*, std::size_t> mapped;
    return mapped;
}

// ---------------------------------------------------------------------------
// Share-mode registry (in-process)
// ---------------------------------------------------------------------------

constexpr DWORD kReadAccess = GENERIC_READ | FILE_READ_DATA | GENERIC_ALL;
constexpr DWORD kWriteAccess = GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_ALL;
constexpr DWORD kDeleteAccess = DELETE | GENERIC_ALL;

struct ShareEntry {
    const Handle* owner;
    std::string stream;
    DWORD access;
    DWORD share;
};

std::mutex& registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<ObjectKey, std::vector<ShareEntry>>& registry() {
    static std::map<ObjectKey, std::vector<ShareEntry>> entries;
    return entries;
}

// NTFS keeps share access per stream: handles of different streams of one
// file never conflict on open. Deleting or renaming the file as a whole
// (delete_permitted) consults every stream's handles.
bool access_conflicts(DWORD new_access, DWORD new_share, DWORD old_access, DWORD old_share,
                      bool same_stream) noexcept {
    if (!same_stream) return false;
    const auto needs = [](DWORD access, DWORD bits) { return (access & bits) != 0u; };
    if (needs(new_access, kReadAccess) && (old_share & FILE_SHARE_READ) == 0u) return true;
    if (needs(new_access, kWriteAccess) && (old_share & FILE_SHARE_WRITE) == 0u) return true;
    if (needs(new_access, kDeleteAccess) && (old_share & FILE_SHARE_DELETE) == 0u) return true;
    if (needs(old_access, kReadAccess) && (new_share & FILE_SHARE_READ) == 0u) return true;
    if (needs(old_access, kWriteAccess) && (new_share & FILE_SHARE_WRITE) == 0u) return true;
    if (needs(old_access, kDeleteAccess) && (new_share & FILE_SHARE_DELETE) == 0u) return true;
    return false;
}

// Registers `handle` unless an existing handle forbids it.
bool register_share(Handle& handle) {
    const std::lock_guard<std::mutex> lock(registry_mutex());
    auto& entries = registry()[handle.key];
    for (const ShareEntry& entry : entries) {
        if (access_conflicts(handle.access, handle.share, entry.access, entry.share,
                             entry.stream == handle.stream)) {
            return fail(ERROR_SHARING_VIOLATION);
        }
    }
    entries.push_back({&handle, handle.stream, handle.access, handle.share});
    handle.registered = true;
    return true;
}

void unregister_share(Handle& handle) {
    if (!handle.registered) return;
    const std::lock_guard<std::mutex> lock(registry_mutex());
    auto it = registry().find(handle.key);
    if (it != registry().end()) {
        auto& entries = it->second;
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const ShareEntry& e) { return e.owner == &handle; }),
                      entries.end());
        if (entries.empty()) registry().erase(it);
    }
    handle.registered = false;
}

// A delete or rename of the object behind `key` (every stream's handles must
// share delete), or of one named stream (only that stream's handles).
bool delete_permitted(const ObjectKey& key, const std::string* stream = nullptr) {
    const std::lock_guard<std::mutex> lock(registry_mutex());
    const auto it = registry().find(key);
    if (it == registry().end()) return true;
    for (const ShareEntry& entry : it->second) {
        if (stream != nullptr && entry.stream != *stream) continue;
        if ((entry.share & FILE_SHARE_DELETE) == 0u) return fail(ERROR_SHARING_VIOLATION);
    }
    return true;
}

// Objects marked delete-on-close refuse new opens, as a delete-pending file does.
bool delete_pending(const ObjectKey& key) {
    const std::lock_guard<std::mutex> lock(registry_mutex());
    const auto it = registry().find(key);
    if (it == registry().end()) return false;
    for (const ShareEntry& entry : it->second) {
        if (entry.owner->delete_on_close) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string parent_of(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0u) return "/";
    return path.substr(0, slash);
}

// Splits "dir/name:stream[:$DATA]" into base path and stream name. Returns
// false for a plain path. "name::$DATA" names the unnamed stream (stream "").
bool split_stream(const std::string& path, std::string& base, std::string& stream) {
    const std::size_t slash = path.find_last_of('/');
    const std::size_t start = slash == std::string::npos ? 0u : slash + 1u;
    const std::size_t colon = path.find(':', start);
    if (colon == std::string::npos) return false;
    base = path.substr(0, colon);
    std::string rest = path.substr(colon + 1u);
    const std::size_t type = rest.find(':');
    if (type != std::string::npos) {
        if (rest.substr(type + 1u) != "$DATA") return false;
        rest.resize(type);
    }
    stream = rest;
    return true;
}

// ERROR_FILE_NOT_FOUND when the parent directory exists, else ERROR_PATH_NOT_FOUND.
DWORD missing_error(const std::string& path) {
    struct stat parent {};
    if (::stat(parent_of(path).c_str(), &parent) == 0 && S_ISDIR(parent.st_mode)) {
        return ERROR_FILE_NOT_FOUND;
    }
    return ERROR_PATH_NOT_FOUND;
}

DWORD error_for(int error, const std::string& path) {
    if (error == ENOENT) return missing_error(path);
    return win32_error_from_errno(error);
}

DWORD attributes_from_stat(const struct stat& info, bool follows_to_directory) {
    if (S_ISLNK(info.st_mode)) {
        return FILE_ATTRIBUTE_REPARSE_POINT |
               (follows_to_directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE);
    }
    if (S_ISDIR(info.st_mode)) return FILE_ATTRIBUTE_DIRECTORY;
    return FILE_ATTRIBUTE_ARCHIVE;
}

DWORD path_attributes(const std::string& path, const struct stat& info) {
    bool to_directory = false;
    if (S_ISLNK(info.st_mode)) {
        struct stat target {};
        to_directory = ::stat(path.c_str(), &target) == 0 && S_ISDIR(target.st_mode);
    }
    return attributes_from_stat(info, to_directory);
}

FILETIME filetime_from(const struct timespec& time) {
    constexpr std::uint64_t kUnixToFiletimeSeconds = 11644473600ull;
    const std::uint64_t ticks =
        (static_cast<std::uint64_t>(time.tv_sec) + kUnixToFiletimeSeconds) * 10'000'000ull +
        static_cast<std::uint64_t>(time.tv_nsec) / 100u;
    return FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32u)};
}

#if defined(__APPLE__)
const struct timespec& modified_time(const struct stat& info) { return info.st_mtimespec; }
const struct timespec& accessed_time(const struct stat& info) { return info.st_atimespec; }
const struct timespec& created_time(const struct stat& info) { return info.st_birthtimespec; }
#else
const struct timespec& modified_time(const struct stat& info) { return info.st_mtim; }
const struct timespec& accessed_time(const struct stat& info) { return info.st_atim; }
const struct timespec& created_time(const struct stat& info) { return info.st_ctim; }
#endif

// ---------------------------------------------------------------------------
// Extended attributes (named streams)
// ---------------------------------------------------------------------------

std::string stream_attribute(const std::string& stream) { return kStreamXattrPrefix + stream; }

ssize_t xattr_get(const std::string& path, const std::string& name, void* value, size_t size) {
#if defined(__APPLE__)
    return ::getxattr(path.c_str(), name.c_str(), value, size, 0, XATTR_NOFOLLOW);
#else
    return ::lgetxattr(path.c_str(), name.c_str(), value, size);
#endif
}

int xattr_set(const std::string& path, const std::string& name, const void* value, size_t size,
              int flags) {
#if defined(__APPLE__)
    return ::setxattr(path.c_str(), name.c_str(), value, size, 0, flags | XATTR_NOFOLLOW);
#else
    return ::lsetxattr(path.c_str(), name.c_str(), value, size, flags);
#endif
}

int xattr_remove(const std::string& path, const std::string& name) {
#if defined(__APPLE__)
    return ::removexattr(path.c_str(), name.c_str(), XATTR_NOFOLLOW);
#else
    return ::lremovexattr(path.c_str(), name.c_str());
#endif
}

ssize_t xattr_list(const std::string& path, char* names, size_t size) {
#if defined(__APPLE__)
    return ::listxattr(path.c_str(), names, size, XATTR_NOFOLLOW);
#else
    return ::llistxattr(path.c_str(), names, size);
#endif
}

bool is_missing_attribute(int error) noexcept { return error == ENOATTR || error == ENODATA; }

// Reads a whole stream; false with last error on failure, ERROR_FILE_NOT_FOUND if absent.
bool read_stream(const std::string& base, const std::string& stream,
                 std::vector<unsigned char>& out) {
    const std::string name = stream_attribute(stream);
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ssize_t size = xattr_get(base, name, nullptr, 0);
        if (size < 0) {
            return fail(is_missing_attribute(errno) ? ERROR_FILE_NOT_FOUND : error_for(errno, base));
        }
        out.resize(static_cast<std::size_t>(size));
        const ssize_t read = xattr_get(base, name, out.data(), out.size());
        if (read >= 0) {
            out.resize(static_cast<std::size_t>(read));
            return true;
        }
        if (errno != ERANGE) return fail(error_for(errno, base));
    }
    return fail(ERROR_READ_FAULT);
}

bool stream_exists(const std::string& base, const std::string& stream) {
    return xattr_get(base, stream_attribute(stream), nullptr, 0) >= 0;
}

bool full_fsync(int fd) noexcept {
#if defined(__APPLE__)
    if (::fcntl(fd, F_FULLFSYNC) == 0) return true;
#endif
    return ::fsync(fd) == 0;
}

bool sync_path(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return fail(error_for(errno, path));
    const bool synced = full_fsync(fd);
    const int error = errno;
    ::close(fd);
    return synced ? true : fail(win32_error_from_errno(error));
}

bool commit_stream(Handle& handle) {
    if (!handle.dirty) return true;
    const std::string name = stream_attribute(handle.stream);
    if (xattr_set(handle.path, name, handle.data.data(), handle.data.size(), 0) != 0) {
        return fail(errno == ENOSPC || errno == E2BIG ? ERROR_DISK_FULL
                                                     : error_for(errno, handle.path));
    }
    handle.dirty = false;
    if ((handle.flags & FILE_FLAG_WRITE_THROUGH) != 0u) return sync_path(handle.path);
    return true;
}

// ---------------------------------------------------------------------------
// Volume identity
// ---------------------------------------------------------------------------

std::uint32_t fnv1a(const void* bytes, std::size_t size) {
    std::uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<const unsigned char*>(bytes)[i];
        hash *= 16777619u;
    }
    return hash;
}

std::mutex& serial_mutex() {
    static std::mutex mutex;
    return mutex;
}

DWORD volume_serial_for(dev_t device, const std::string& path_on_volume) {
    static std::unordered_map<std::uint64_t, DWORD> cache;
    const std::lock_guard<std::mutex> lock(serial_mutex());
    const auto cached = cache.find(static_cast<std::uint64_t>(device));
    if (cached != cache.end()) return cached->second;
    DWORD serial = 0;
#if defined(__APPLE__)
    struct statfs fs {};
    if (::statfs(path_on_volume.c_str(), &fs) == 0) {
        struct attrlist request {};
        request.bitmapcount = ATTR_BIT_MAP_COUNT;
        request.volattr = ATTR_VOL_INFO | ATTR_VOL_UUID;
        struct {
            std::uint32_t length;
            uuid_t uuid;
        } __attribute__((packed)) reply {};
        if (::getattrlist(fs.f_mntonname, &request, &reply, sizeof(reply), 0) == 0) {
            serial = fnv1a(reply.uuid, sizeof(reply.uuid));
        }
    }
#else
    struct statfs fs {};
    if (::statfs(path_on_volume.c_str(), &fs) == 0) {
        int fsid[2];
        std::memcpy(fsid, &fs.f_fsid, sizeof(fsid));
        if (fsid[0] != 0 || fsid[1] != 0) serial = fnv1a(fsid, sizeof(fsid));
    }
#endif
    if (serial == 0) {
        const auto value = static_cast<std::uint64_t>(device);
        serial = fnv1a(&value, sizeof(value));
    }
    if (serial == 0) serial = 1;
    cache.emplace(static_cast<std::uint64_t>(device), serial);
    return serial;
}

std::string mount_root(const std::string& path) {
#if defined(__APPLE__)
    struct statfs fs {};
    if (::statfs(path.c_str(), &fs) == 0) return fs.f_mntonname;
    return {};
#else
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) return {};
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr) return {};
    std::string current = resolved;
    if (!S_ISDIR(info.st_mode)) current = parent_of(current);
    while (current != "/") {
        const std::string parent = parent_of(current);
        struct stat parent_info {};
        if (::stat(parent.c_str(), &parent_info) != 0 || parent_info.st_dev != info.st_dev) break;
        current = parent;
    }
    return current;
#endif
}

// NTFS file IDs carry a sequence number and never repeat. ext4 and other
// Linux filesystems reuse a freed inode number at once, so the inode's
// generation (FS_IOC_GETVERSION), which changes on reuse, is folded into the
// file index there. APFS object IDs are not reused.
std::uint64_t file_index_for(const struct stat& info, int fd, const std::string& path) {
    auto index = static_cast<std::uint64_t>(info.st_ino);
#if !defined(__APPLE__)
    constexpr unsigned long kGetVersion = 0x80087601ul;  // _IOR('v', 1, long)
    int descriptor = fd;
    bool owned = false;
    if (descriptor < 0 && !S_ISLNK(info.st_mode)) {
        descriptor = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        owned = descriptor >= 0;
    }
    if (descriptor >= 0) {
        long generation = 0;
        struct stat opened {};
        if (::ioctl(descriptor, kGetVersion, &generation) == 0 && ::fstat(descriptor, &opened) == 0 &&
            opened.st_dev == info.st_dev && opened.st_ino == info.st_ino) {
            index ^= static_cast<std::uint64_t>(static_cast<std::uint32_t>(generation)) << 32u;
        }
        if (owned) ::close(descriptor);
    }
#else
    (void)fd;
    (void)path;
#endif
    return index;
}

void fill_information(const struct stat& info, const std::string& path,
                      BY_HANDLE_FILE_INFORMATION& out, DWORD attributes, int fd = -1) {
    out = {};
    out.dwFileAttributes = attributes;
    out.ftCreationTime = filetime_from(created_time(info));
    out.ftLastAccessTime = filetime_from(accessed_time(info));
    out.ftLastWriteTime = filetime_from(modified_time(info));
    out.dwVolumeSerialNumber = volume_serial_for(info.st_dev, path);
    const auto size = S_ISREG(info.st_mode) ? static_cast<std::uint64_t>(info.st_size) : 0u;
    out.nFileSizeHigh = static_cast<DWORD>(size >> 32u);
    out.nFileSizeLow = static_cast<DWORD>(size);
    // NTFS reports one link for a directory; POSIX counts "." and children.
    out.nNumberOfLinks = S_ISDIR(info.st_mode) ? 1u : static_cast<DWORD>(info.st_nlink);
    const std::uint64_t index = file_index_for(info, fd, path);
    out.nFileIndexHigh = static_cast<DWORD>(index >> 32u);
    out.nFileIndexLow = static_cast<DWORD>(index);
}

bool rename_noreplace(const std::string& from, const std::string& to) {
#if defined(__APPLE__)
    return ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0;
#else
#if defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(),
                  RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) return false;
#endif
    // Filesystems without RENAME_NOREPLACE: link + unlink for files.
    struct stat info {};
    if (::lstat(to.c_str(), &info) == 0) {
        errno = EEXIST;
        return false;
    }
    if (::lstat(from.c_str(), &info) == 0 && !S_ISDIR(info.st_mode)) {
        if (::link(from.c_str(), to.c_str()) != 0) return false;
        return ::unlink(from.c_str()) == 0;
    }
    return ::rename(from.c_str(), to.c_str()) == 0;
#endif
}

// ---------------------------------------------------------------------------
// Opening
// ---------------------------------------------------------------------------

HANDLE open_stream(const std::string& base, const std::string& stream, DWORD access, DWORD share,
                   DWORD disposition, DWORD flags) {
    struct stat info {};
    bool created_base = false;
    if (::lstat(base.c_str(), &info) != 0) {
        if (errno != ENOENT) return fail_handle(error_for(errno, base));
        if (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING) {
            return fail_handle(missing_error(base));
        }
        // Creating a stream of a missing file creates the file, as on NTFS.
        const int fd = ::open(base.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd < 0) return fail_handle(error_for(errno, base));
        ::close(fd);
        created_base = true;
        if (::lstat(base.c_str(), &info) != 0) return fail_handle(error_for(errno, base));
    }
    if (S_ISLNK(info.st_mode)) return fail_handle(ERROR_ACCESS_DENIED);

    auto handle = std::make_unique<Handle>();
    handle->kind = Kind::Stream;
    handle->path = base;
    handle->stream = stream;
    handle->info = info;
    handle->key = {info.st_dev, info.st_ino};
    handle->access = access;
    handle->share = share;
    handle->flags = flags;

    if (delete_pending(handle->key)) return fail_handle(ERROR_ACCESS_DENIED);
    if (!register_share(*handle)) return INVALID_HANDLE_VALUE;

    const auto rollback = [&](DWORD error) {
        unregister_share(*handle);
        if (created_base) ::unlink(base.c_str());
        return fail_handle(error);
    };

    const bool exists = stream_exists(base, stream);
    switch (disposition) {
    case CREATE_NEW:
        if (exists) return rollback(ERROR_FILE_EXISTS);
        handle->dirty = true;
        break;
    case CREATE_ALWAYS:
        handle->dirty = true;
        break;
    case OPEN_EXISTING:
        if (!exists) return rollback(ERROR_FILE_NOT_FOUND);
        break;
    case OPEN_ALWAYS:
        if (!exists) handle->dirty = true;
        break;
    case TRUNCATE_EXISTING:
        if (!exists) return rollback(ERROR_FILE_NOT_FOUND);
        handle->dirty = true;
        break;
    default:
        return rollback(ERROR_INVALID_PARAMETER);
    }
    if (exists && !handle->dirty) {
        if (!read_stream(base, stream, handle->data)) return rollback(GetLastError());
    }
    if (handle->dirty) {
        // The stream exists from now on, as on NTFS.
        const std::string name = stream_attribute(stream);
        const int create_flag = disposition == CREATE_NEW ? XATTR_CREATE : 0;
        if (xattr_set(base, name, "", 0, create_flag) != 0) {
            return rollback(errno == EEXIST ? ERROR_FILE_EXISTS : error_for(errno, base));
        }
        handle->dirty = false;
    }
    SetLastError(exists && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
                     ? ERROR_ALREADY_EXISTS
                     : ERROR_SUCCESS);
    return handle.release();
}

HANDLE open_file(const std::string& path, DWORD access, DWORD share, DWORD disposition,
                 DWORD flags) {
    if (disposition < CREATE_NEW || disposition > TRUNCATE_EXISTING) {
        return fail_handle(ERROR_INVALID_PARAMETER);
    }
    const bool wants_read = (access & kReadAccess) != 0u;
    const bool wants_write = (access & kWriteAccess) != 0u;
    const bool reparse = (flags & FILE_FLAG_OPEN_REPARSE_POINT) != 0u;

    struct stat link_info {};
    const bool present = ::lstat(path.c_str(), &link_info) == 0;
    if (!present && errno != ENOENT) return fail_handle(error_for(errno, path));
    if (present && S_ISLNK(link_info.st_mode) && !reparse) {
        struct stat target {};
        if (::stat(path.c_str(), &target) != 0) return fail_handle(error_for(errno, path));
        link_info = target;
    }

    auto handle = std::make_unique<Handle>();
    handle->path = path;
    handle->access = access;
    handle->share = share;
    handle->flags = flags;
    handle->delete_on_close = (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0u;

    if (present && S_ISLNK(link_info.st_mode)) {
        // The reparse point itself: identity and attributes only.
        if (disposition == CREATE_NEW) return fail_handle(ERROR_FILE_EXISTS);
        handle->info = link_info;
        handle->key = {link_info.st_dev, link_info.st_ino};
        if (delete_pending(handle->key)) return fail_handle(ERROR_ACCESS_DENIED);
        if (!register_share(*handle)) return INVALID_HANDLE_VALUE;
        SetLastError(ERROR_SUCCESS);
        return handle.release();
    }

    if (present && S_ISDIR(link_info.st_mode)) {
        if ((flags & FILE_FLAG_BACKUP_SEMANTICS) == 0u || wants_write ||
            disposition == CREATE_NEW || disposition == CREATE_ALWAYS ||
            disposition == TRUNCATE_EXISTING) {
            return fail_handle(disposition == CREATE_NEW ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
        }
    }

    if (!present) {
        if (disposition == OPEN_EXISTING || disposition == TRUNCATE_EXISTING) {
            return fail_handle(missing_error(path));
        }
    } else if (disposition == CREATE_NEW) {
        return fail_handle(ERROR_FILE_EXISTS);
    }

    int open_flags = O_CLOEXEC | O_NOFOLLOW;
    if (wants_write) open_flags |= wants_read ? O_RDWR : O_WRONLY;
    else open_flags |= O_RDONLY;
    if (!present) open_flags |= O_CREAT | O_EXCL;
    if (present && S_ISDIR(link_info.st_mode)) open_flags |= O_DIRECTORY;

    int fd = ::open(path.c_str(), open_flags, 0644);
    if (fd < 0 && !wants_read && !wants_write && present && errno == EACCES) {
        // Attribute/delete-only access needs no data access.
        handle->info = link_info;
        handle->key = {link_info.st_dev, link_info.st_ino};
        if (delete_pending(handle->key)) return fail_handle(ERROR_ACCESS_DENIED);
        if (!register_share(*handle)) return INVALID_HANDLE_VALUE;
        SetLastError(ERROR_SUCCESS);
        return handle.release();
    }
    if (fd < 0) {
        return fail_handle(errno == EEXIST ? ERROR_FILE_EXISTS : error_for(errno, path));
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        return fail_handle(win32_error_from_errno(error));
    }
    handle->fd = fd;
    handle->info = info;
    handle->key = {info.st_dev, info.st_ino};
    if (delete_pending(handle->key) || !register_share(*handle)) {
        const DWORD error = delete_pending(handle->key) ? ERROR_ACCESS_DENIED : GetLastError();
        ::close(fd);
        if (!present) ::unlink(path.c_str());
        return fail_handle(error);
    }
    // Truncation only after the share check passed, as Windows orders it.
    if (present && (disposition == CREATE_ALWAYS || disposition == TRUNCATE_EXISTING) &&
        S_ISREG(info.st_mode) && ::ftruncate(fd, 0) != 0) {
        const DWORD error = win32_error_from_errno(errno);
        unregister_share(*handle);
        ::close(fd);
        return fail_handle(error);
    }
    SetLastError(present && (disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS)
                     ? ERROR_ALREADY_EXISTS
                     : ERROR_SUCCESS);
    return handle.release();
}

HANDLE create_file(const std::string& path, DWORD access, DWORD share, DWORD disposition,
                   DWORD flags) {
    if (path.empty()) return fail_handle(ERROR_PATH_NOT_FOUND);
    std::string base;
    std::string stream;
    if (split_stream(path, base, stream)) {
        if (base.empty()) return fail_handle(ERROR_INVALID_NAME);
        if (stream.empty()) return open_file(base, access, share, disposition, flags);
        return open_stream(base, stream, access, share, disposition, flags);
    }
    return open_file(path, access, share, disposition, flags);
}

bool same_object(const std::string& path, const ObjectKey& key) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0 && info.st_dev == key.device && info.st_ino == key.inode;
}

// ---------------------------------------------------------------------------
// Path operations
// ---------------------------------------------------------------------------

DWORD get_attributes(const std::string& path) {
    if (path.empty()) {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_FILE_ATTRIBUTES;
    }
    std::string base;
    std::string stream;
    if (split_stream(path, base, stream)) {
        struct stat info {};
        if (::lstat(base.c_str(), &info) != 0) {
            SetLastError(error_for(errno, base));
            return INVALID_FILE_ATTRIBUTES;
        }
        if (!stream.empty() && !stream_exists(base, stream)) {
            SetLastError(ERROR_FILE_NOT_FOUND);
            return INVALID_FILE_ATTRIBUTES;
        }
        SetLastError(ERROR_SUCCESS);
        return path_attributes(base, info);
    }
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) {
        SetLastError(error_for(errno, path));
        return INVALID_FILE_ATTRIBUTES;
    }
    SetLastError(ERROR_SUCCESS);
    return path_attributes(path, info);
}

BOOL delete_file(const std::string& path) {
    std::string base;
    std::string stream;
    if (split_stream(path, base, stream) && !stream.empty()) {
        struct stat info {};
        if (::lstat(base.c_str(), &info) != 0) return fail_bool(error_for(errno, base));
        if (!delete_permitted({info.st_dev, info.st_ino}, &stream)) return FALSE;
        if (xattr_remove(base, stream_attribute(stream)) != 0) {
            return fail_bool(is_missing_attribute(errno) ? ERROR_FILE_NOT_FOUND
                                                         : error_for(errno, base));
        }
        return TRUE;
    }
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) return fail_bool(error_for(errno, path));
    if (S_ISDIR(info.st_mode)) return fail_bool(ERROR_ACCESS_DENIED);
    const ObjectKey key{info.st_dev, info.st_ino};
    if (delete_pending(key)) return fail_bool(ERROR_ACCESS_DENIED);
    if (!delete_permitted(key)) return FALSE;
    if (::unlink(path.c_str()) != 0) return fail_bool(error_for(errno, path));
    return TRUE;
}

BOOL move_file(const std::string& from, const std::string& to, DWORD flags) {
    struct stat source {};
    if (::lstat(from.c_str(), &source) != 0) return fail_bool(error_for(errno, from));
    const ObjectKey source_key{source.st_dev, source.st_ino};
    if (delete_pending(source_key)) return fail_bool(ERROR_ACCESS_DENIED);
    if (!delete_permitted(source_key)) return FALSE;
    struct stat target {};
    const bool target_exists = ::lstat(to.c_str(), &target) == 0;
    if (target_exists) {
        const bool same = target.st_dev == source.st_dev && target.st_ino == source.st_ino;
        if ((flags & MOVEFILE_REPLACE_EXISTING) == 0u && !same) {
            return fail_bool(ERROR_ALREADY_EXISTS);
        }
        if (!same) {
            if (S_ISDIR(target.st_mode)) return fail_bool(ERROR_ACCESS_DENIED);
            const ObjectKey target_key{target.st_dev, target.st_ino};
            if (delete_pending(target_key)) return fail_bool(ERROR_ACCESS_DENIED);
            if (!delete_permitted(target_key)) return FALSE;
        }
    }
    const bool moved = (flags & MOVEFILE_REPLACE_EXISTING) != 0u
                           ? ::rename(from.c_str(), to.c_str()) == 0
                           : rename_noreplace(from, to);
    if (!moved) {
        if (errno == EEXIST || errno == ENOTEMPTY) return fail_bool(ERROR_ALREADY_EXISTS);
        if (errno == EXDEV) return fail_bool(ERROR_NOT_SAME_DEVICE);
        return fail_bool(error_for(errno, errno == ENOENT ? to : from));
    }
    if ((flags & MOVEFILE_WRITE_THROUGH) != 0u) {
        // Durable rename: both directory entries reach the disk.
        if (!sync_path(parent_of(to))) return FALSE;
        if (parent_of(to) != parent_of(from) && !sync_path(parent_of(from))) return FALSE;
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

}  // namespace

bool is_shim_handle(HANDLE handle) noexcept { return as_handle(handle) != nullptr; }

int native_descriptor(HANDLE handle) noexcept {
    const Handle* const object = as_handle(handle);
    return object != nullptr && object->kind == Kind::File ? object->fd : -1;
}

}  // namespace galaxy::win32

using galaxy::win32::narrow;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace {
using galaxy::win32::as_handle;
using galaxy::win32::fail_bool;
using galaxy::win32::Handle;
using galaxy::win32::Kind;
}  // namespace

HANDLE CreateFileW(const char* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES*,
                   DWORD disposition, DWORD flags, HANDLE) noexcept {
    try {
        return galaxy::win32::create_file(path != nullptr ? path : "", access, share, disposition,
                                          flags);
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
}

HANDLE CreateFileW(const wchar_t* path, DWORD access, DWORD share, SECURITY_ATTRIBUTES* security,
                   DWORD disposition, DWORD flags, HANDLE template_file) noexcept {
    try {
        return CreateFileW(narrow(path).c_str(), access, share, security, disposition, flags,
                           template_file);
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
}

BOOL ReadFile(HANDLE file, void* buffer, DWORD bytes, DWORD* read, OVERLAPPED* overlapped) noexcept {
    Handle* const handle = as_handle(file);
    if (read != nullptr) *read = 0;
    if (handle == nullptr || handle->kind == Kind::StreamSearch) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    if ((handle->access & galaxy::win32::kReadAccess) == 0u) return fail_bool(ERROR_ACCESS_DENIED);
    if (handle->kind == Kind::Stream) {
        const std::size_t available =
            handle->position < handle->data.size() ? handle->data.size() - handle->position : 0u;
        const std::size_t count = std::min<std::size_t>(available, bytes);
        if (count != 0u) std::memcpy(buffer, handle->data.data() + handle->position, count);
        handle->position += count;
        if (read != nullptr) *read = static_cast<DWORD>(count);
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    if (handle->fd < 0) return fail_bool(ERROR_ACCESS_DENIED);
    std::size_t total = 0;
    auto* out = static_cast<unsigned char*>(buffer);
    std::uint64_t offset = 0;
    if (overlapped != nullptr) {
        offset = (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset;
    }
    while (total < bytes) {
        const ssize_t count =
            overlapped != nullptr
                ? ::pread(handle->fd, out + total, bytes - total, static_cast<off_t>(offset + total))
                : ::read(handle->fd, out + total, bytes - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (read != nullptr) *read = static_cast<DWORD>(total);
            return fail_bool(errno == EISDIR ? ERROR_ACCESS_DENIED : win32_error_from_errno(errno));
        }
        if (count == 0) break;
        total += static_cast<std::size_t>(count);
    }
    if (read != nullptr) *read = static_cast<DWORD>(total);
    if (overlapped != nullptr) overlapped->InternalHigh = total;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL WriteFile(HANDLE file, const void* buffer, DWORD bytes, DWORD* written,
               OVERLAPPED* overlapped) noexcept {
    Handle* const handle = as_handle(file);
    if (written != nullptr) *written = 0;
    if (handle == nullptr || handle->kind == Kind::StreamSearch) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    if ((handle->access & galaxy::win32::kWriteAccess) == 0u) return fail_bool(ERROR_ACCESS_DENIED);
    if (handle->kind == Kind::Stream) {
        const std::size_t end = handle->position + bytes;
        if (end > kWin32MaxStreamBytes) return fail_bool(ERROR_DISK_FULL);
        try {
            if (handle->data.size() < end) handle->data.resize(end);
        } catch (...) {
            return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
        }
        if (bytes != 0u) std::memcpy(handle->data.data() + handle->position, buffer, bytes);
        handle->position = end;
        handle->dirty = true;
        if (written != nullptr) *written = bytes;
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    if (handle->fd < 0) return fail_bool(ERROR_ACCESS_DENIED);
    std::size_t total = 0;
    const auto* in = static_cast<const unsigned char*>(buffer);
    std::uint64_t offset = 0;
    if (overlapped != nullptr) {
        offset = (static_cast<std::uint64_t>(overlapped->OffsetHigh) << 32u) | overlapped->Offset;
    }
    while (total < bytes) {
        const ssize_t count =
            overlapped != nullptr
                ? ::pwrite(handle->fd, in + total, bytes - total, static_cast<off_t>(offset + total))
                : ::write(handle->fd, in + total, bytes - total);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (written != nullptr) *written = static_cast<DWORD>(total);
            return fail_bool(errno == ENOSPC ? ERROR_DISK_FULL : win32_error_from_errno(errno));
        }
        total += static_cast<std::size_t>(count);
    }
    if (written != nullptr) *written = static_cast<DWORD>(total);
    if ((handle->flags & FILE_FLAG_WRITE_THROUGH) != 0u) {
#if defined(__APPLE__)
        const bool synced = ::fsync(handle->fd) == 0;
#else
        const bool synced = ::fdatasync(handle->fd) == 0;
#endif
        if (!synced) return fail_bool(ERROR_WRITE_FAULT);
    }
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE file) noexcept {
    Handle* const handle = as_handle(file);
    if (handle == nullptr || handle->kind == Kind::StreamSearch) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    if ((handle->access & galaxy::win32::kWriteAccess) == 0u) return fail_bool(ERROR_ACCESS_DENIED);
    if (handle->kind == Kind::Stream) {
        const bool through = (handle->flags & FILE_FLAG_WRITE_THROUGH) != 0u;
        if (!galaxy::win32::commit_stream(*handle)) return FALSE;
        if (!through && !galaxy::win32::sync_path(handle->path)) return FALSE;
        return TRUE;
    }
    if (handle->fd < 0) return fail_bool(ERROR_ACCESS_DENIED);
    if (!galaxy::win32::full_fsync(handle->fd)) return fail_bool(ERROR_WRITE_FAULT);
    return TRUE;
}

BOOL CloseHandle(HANDLE object) noexcept {
    Handle* const handle = as_handle(object);
    if (handle == nullptr) return fail_bool(ERROR_INVALID_HANDLE);
    std::unique_ptr<Handle> owned(handle);
    BOOL result = TRUE;
    DWORD error = ERROR_SUCCESS;
    if (handle->kind == Kind::Event || handle->kind == Kind::Timer) {
        // Waiters hold no reference; closing while waited on is a caller bug
        // on Windows too.
        const std::lock_guard<std::mutex> lock(galaxy::win32::event_mutex());
        handle->magic = 0;
        owned.reset();
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    }
    if (handle->kind == Kind::Stream && handle->dirty && !galaxy::win32::commit_stream(*handle)) {
        result = FALSE;
        error = GetLastError();
    }
    if (handle->fd >= 0 && ::close(handle->fd) != 0 && result != FALSE) {
        result = FALSE;
        error = win32_error_from_errno(errno);
    }
    handle->fd = -1;
    if (handle->delete_on_close) {
        // The object, if its path still names it (it may have been renamed).
        if (galaxy::win32::same_object(handle->path, handle->key)) {
            const int removed = S_ISDIR(handle->info.st_mode) ? ::rmdir(handle->path.c_str())
                                                               : ::unlink(handle->path.c_str());
            if (removed != 0 && result != FALSE) {
                result = FALSE;
                error = win32_error_from_errno(errno);
            }
        }
    }
    galaxy::win32::unregister_share(*handle);
    handle->magic = 0;
    SetLastError(error);
    return result;
}

BOOL GetFileSizeEx(HANDLE file, LARGE_INTEGER* size) noexcept {
    Handle* const handle = as_handle(file);
    if (handle == nullptr || size == nullptr) return fail_bool(ERROR_INVALID_HANDLE);
    if (handle->kind == Kind::Stream) {
        size->QuadPart = static_cast<LONGLONG>(handle->data.size());
        return TRUE;
    }
    struct stat info {};
    if (handle->fd >= 0 ? ::fstat(handle->fd, &info) != 0 : ::lstat(handle->path.c_str(), &info) != 0) {
        return fail_bool(win32_error_from_errno(errno));
    }
    size->QuadPart = static_cast<LONGLONG>(info.st_size);
    return TRUE;
}

BOOL SetFilePointerEx(HANDLE file, LARGE_INTEGER distance, LARGE_INTEGER* new_position,
                      DWORD method) noexcept {
    Handle* const handle = as_handle(file);
    if (handle == nullptr) return fail_bool(ERROR_INVALID_HANDLE);
    if (handle->kind == Kind::Stream) {
        LONGLONG base = 0;
        if (method == FILE_CURRENT) base = static_cast<LONGLONG>(handle->position);
        else if (method == FILE_END) base = static_cast<LONGLONG>(handle->data.size());
        const LONGLONG target = base + distance.QuadPart;
        if (target < 0) return fail_bool(ERROR_INVALID_PARAMETER);
        handle->position = static_cast<std::size_t>(target);
        if (new_position != nullptr) new_position->QuadPart = target;
        return TRUE;
    }
    if (handle->fd < 0) return fail_bool(ERROR_INVALID_HANDLE);
    const int whence = method == FILE_CURRENT ? SEEK_CUR : method == FILE_END ? SEEK_END : SEEK_SET;
    const off_t position = ::lseek(handle->fd, static_cast<off_t>(distance.QuadPart), whence);
    if (position < 0) return fail_bool(win32_error_from_errno(errno));
    if (new_position != nullptr) new_position->QuadPart = static_cast<LONGLONG>(position);
    return TRUE;
}

BOOL GetFileInformationByHandle(HANDLE file, BY_HANDLE_FILE_INFORMATION* information) noexcept {
    Handle* const handle = as_handle(file);
    if (handle == nullptr || information == nullptr || handle->kind == Kind::StreamSearch) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    struct stat info = handle->info;
    if (handle->fd >= 0) {
        if (::fstat(handle->fd, &info) != 0) return fail_bool(win32_error_from_errno(errno));
    } else {
        // Refresh while the path still names the opened object.
        struct stat current {};
        if (::lstat(handle->path.c_str(), &current) == 0 && current.st_dev == info.st_dev &&
            current.st_ino == info.st_ino) {
            info = current;
        }
    }
    galaxy::win32::fill_information(info, handle->path, *information,
                                    galaxy::win32::attributes_from_stat(info, false), handle->fd);
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetFileInformationByHandleEx(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
                                  void*, DWORD) noexcept {
    if (as_handle(file) == nullptr) return fail_bool(ERROR_INVALID_HANDLE);
    if (information_class == FileCaseSensitiveInfo) return fail_bool(ERROR_NOT_SUPPORTED);
    return fail_bool(ERROR_INVALID_PARAMETER);
}

BOOL SetFileInformationByHandle(HANDLE file, FILE_INFO_BY_HANDLE_CLASS information_class,
                                void* information, DWORD size) noexcept {
    Handle* const handle = as_handle(file);
    if (handle == nullptr || handle->kind != Kind::File) return fail_bool(ERROR_INVALID_HANDLE);
    if (information_class != FileDispositionInfo || information == nullptr ||
        size < sizeof(FILE_DISPOSITION_INFO)) {
        return fail_bool(ERROR_INVALID_PARAMETER);
    }
    if ((handle->access & galaxy::win32::kDeleteAccess) == 0u) return fail_bool(ERROR_ACCESS_DENIED);
    const bool remove = static_cast<FILE_DISPOSITION_INFO*>(information)->DeleteFile != FALSE;
    if (remove && S_ISDIR(handle->info.st_mode)) {
        // A non-empty directory cannot be marked for deletion.
        bool empty = true;
        if (DIR* const directory = ::opendir(handle->path.c_str())) {
            while (const dirent* const entry = ::readdir(directory)) {
                const std::string_view name = entry->d_name;
                if (name != "." && name != "..") {
                    empty = false;
                    break;
                }
            }
            ::closedir(directory);
        }
        if (!empty) return fail_bool(ERROR_DIR_NOT_EMPTY);
    }
    handle->delete_on_close = remove;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

DWORD GetFileAttributesW(const char* path) noexcept {
    try {
        return galaxy::win32::get_attributes(path != nullptr ? path : "");
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_FILE_ATTRIBUTES;
    }
}

DWORD GetFileAttributesW(const wchar_t* path) noexcept {
    try {
        return GetFileAttributesW(narrow(path).c_str());
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_FILE_ATTRIBUTES;
    }
}

BOOL GetFileAttributesExW(const char* path, GET_FILEEX_INFO_LEVELS, void* information) noexcept {
    if (path == nullptr || information == nullptr) return fail_bool(ERROR_INVALID_PARAMETER);
    struct stat info {};
    if (::lstat(path, &info) != 0) return fail_bool(galaxy::win32::error_for(errno, path));
    BY_HANDLE_FILE_INFORMATION full{};
    galaxy::win32::fill_information(info, path, full, galaxy::win32::path_attributes(path, info));
    auto* const out = static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(information);
    out->dwFileAttributes = full.dwFileAttributes;
    out->ftCreationTime = full.ftCreationTime;
    out->ftLastAccessTime = full.ftLastAccessTime;
    out->ftLastWriteTime = full.ftLastWriteTime;
    out->nFileSizeHigh = full.nFileSizeHigh;
    out->nFileSizeLow = full.nFileSizeLow;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

BOOL GetFileAttributesExW(const wchar_t* path, GET_FILEEX_INFO_LEVELS level,
                          void* information) noexcept {
    try {
        return GetFileAttributesExW(narrow(path).c_str(), level, information);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL DeleteFileW(const char* path) noexcept {
    try {
        return galaxy::win32::delete_file(path != nullptr ? path : "");
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL DeleteFileW(const wchar_t* path) noexcept {
    try {
        return DeleteFileW(narrow(path).c_str());
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL MoveFileExW(const char* from, const char* to, DWORD flags) noexcept {
    if (from == nullptr || to == nullptr) return fail_bool(ERROR_INVALID_PARAMETER);
    try {
        return galaxy::win32::move_file(from, to, flags);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL MoveFileExW(const wchar_t* from, const wchar_t* to, DWORD flags) noexcept {
    try {
        return MoveFileExW(narrow(from).c_str(), narrow(to).c_str(), flags);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL CreateDirectoryW(const char* path, SECURITY_ATTRIBUTES*) noexcept {
    if (path == nullptr) return fail_bool(ERROR_INVALID_PARAMETER);
    if (::mkdir(path, 0755) != 0) {
        return fail_bool(errno == EEXIST ? ERROR_ALREADY_EXISTS
                                         : galaxy::win32::error_for(errno, path));
    }
    return TRUE;
}

BOOL CreateDirectoryW(const wchar_t* path, SECURITY_ATTRIBUTES* security) noexcept {
    try {
        return CreateDirectoryW(narrow(path).c_str(), security);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL RemoveDirectoryW(const char* path) noexcept {
    if (path == nullptr) return fail_bool(ERROR_INVALID_PARAMETER);
    struct stat info {};
    if (::lstat(path, &info) != 0) return fail_bool(galaxy::win32::error_for(errno, path));
    if (!S_ISDIR(info.st_mode)) return fail_bool(ERROR_DIRECTORY);
    const galaxy::win32::ObjectKey key{info.st_dev, info.st_ino};
    if (!galaxy::win32::delete_permitted(key)) return FALSE;
    if (::rmdir(path) != 0) {
        return fail_bool(errno == ENOTEMPTY || errno == EEXIST
                             ? ERROR_DIR_NOT_EMPTY
                             : galaxy::win32::error_for(errno, path));
    }
    return TRUE;
}

BOOL RemoveDirectoryW(const wchar_t* path) noexcept {
    try {
        return RemoveDirectoryW(narrow(path).c_str());
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL CreateHardLinkW(const char* new_link, const char* existing, SECURITY_ATTRIBUTES*) noexcept {
    if (new_link == nullptr || existing == nullptr) return fail_bool(ERROR_INVALID_PARAMETER);
    if (::link(existing, new_link) != 0) {
        return fail_bool(errno == EEXIST ? ERROR_ALREADY_EXISTS
                                         : galaxy::win32::error_for(errno, errno == ENOENT ? existing : new_link));
    }
    return TRUE;
}

BOOL CreateHardLinkW(const wchar_t* new_link, const wchar_t* existing,
                     SECURITY_ATTRIBUTES* security) noexcept {
    try {
        return CreateHardLinkW(narrow(new_link).c_str(), narrow(existing).c_str(), security);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL GetVolumePathNameW(const char* path, wchar_t* volume_root, DWORD length) noexcept {
    if (path == nullptr || volume_root == nullptr || length == 0u) {
        return fail_bool(ERROR_INVALID_PARAMETER);
    }
    try {
        const std::string root = galaxy::win32::mount_root(path);
        if (root.empty()) return fail_bool(galaxy::win32::error_for(errno, path));
        std::wstring wide = galaxy::win32::widen(root.c_str());
        if (wide.empty() || wide.back() != L'/') wide.push_back(L'/');
        if (wide.size() + 1u > length) return fail_bool(ERROR_FILENAME_EXCED_RANGE);
        std::wmemcpy(volume_root, wide.c_str(), wide.size() + 1u);
        return TRUE;
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL GetVolumePathNameW(const wchar_t* path, wchar_t* volume_root, DWORD length) noexcept {
    try {
        return GetVolumePathNameW(narrow(path).c_str(), volume_root, length);
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

BOOL GetVolumeInformationW(const wchar_t* root, wchar_t* name, DWORD name_length, DWORD* serial,
                           DWORD* maximum_component_length, DWORD* flags,
                           wchar_t* file_system_name, DWORD file_system_name_length) noexcept {
    try {
        const std::string path = narrow(root);
        struct stat info {};
        if (::stat(path.c_str(), &info) != 0) return fail_bool(galaxy::win32::error_for(errno, path));
        if (name != nullptr && name_length != 0u) name[0] = L'\0';
        if (serial != nullptr) *serial = galaxy::win32::volume_serial_for(info.st_dev, path);
        if (maximum_component_length != nullptr) *maximum_component_length = 255u;
        if (flags != nullptr) {
            DWORD value = FILE_CASE_PRESERVED_NAMES;
            errno = 0;
            const ssize_t probe =
                galaxy::win32::xattr_get(path, "user.galaxy.probe", nullptr, 0);
            if (probe >= 0 || galaxy::win32::is_missing_attribute(errno) || errno == ERANGE) {
                value |= FILE_NAMED_STREAMS;
            }
#if defined(__APPLE__) && defined(_PC_CASE_SENSITIVE)
            if (::pathconf(path.c_str(), _PC_CASE_SENSITIVE) == 1) value |= FILE_CASE_SENSITIVE_SEARCH;
#else
            value |= FILE_CASE_SENSITIVE_SEARCH;
#endif
            *flags = value;
        }
        if (file_system_name != nullptr && file_system_name_length != 0u) {
            const wchar_t kName[] = L"POSIX";
            const std::size_t count = std::min<std::size_t>(file_system_name_length - 1u, 5u);
            std::wmemcpy(file_system_name, kName, count);
            file_system_name[count] = L'\0';
        }
        SetLastError(ERROR_SUCCESS);
        return TRUE;
    } catch (...) {
        return fail_bool(ERROR_NOT_ENOUGH_MEMORY);
    }
}

namespace {

BOOL fill_stream_data(Handle& search, void* data) {
    if (search.next >= search.streams.size()) return fail_bool(ERROR_HANDLE_EOF);
    auto* const out = static_cast<WIN32_FIND_STREAM_DATA*>(data);
    const auto& [name, size] = search.streams[search.next++];
    out->StreamSize.QuadPart = size;
    const std::size_t count = std::min<std::size_t>(name.size(), MAX_PATH + 35u);
    std::wmemcpy(out->cStreamName, name.c_str(), count);
    out->cStreamName[count] = L'\0';
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

}  // namespace

HANDLE FindFirstStreamW(const char* path, STREAM_INFO_LEVELS, void* data, DWORD) noexcept {
    if (path == nullptr || data == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    try {
        struct stat info {};
        if (::lstat(path, &info) != 0) {
            SetLastError(galaxy::win32::error_for(errno, path));
            return INVALID_HANDLE_VALUE;
        }
        auto search = std::make_unique<Handle>();
        search->kind = Kind::StreamSearch;
        if (S_ISREG(info.st_mode)) {
            search->streams.emplace_back(L"::$DATA", static_cast<std::int64_t>(info.st_size));
        }
        if (!S_ISLNK(info.st_mode)) {
            std::vector<char> names;
            for (int attempt = 0; attempt < 4; ++attempt) {
                const ssize_t size = galaxy::win32::xattr_list(path, nullptr, 0);
                if (size < 0) {
                    if (errno == ENOTSUP) break;
                    SetLastError(win32_error_from_errno(errno));
                    return INVALID_HANDLE_VALUE;
                }
                names.resize(static_cast<std::size_t>(size));
                const ssize_t listed = galaxy::win32::xattr_list(path, names.data(), names.size());
                if (listed >= 0) {
                    names.resize(static_cast<std::size_t>(listed));
                    break;
                }
                if (errno != ERANGE) {
                    SetLastError(win32_error_from_errno(errno));
                    return INVALID_HANDLE_VALUE;
                }
            }
            for (std::size_t at = 0; at < names.size();) {
                const std::string attribute(names.data() + at);
                at += attribute.size() + 1u;
                if (attribute.compare(0, galaxy::win32::kStreamXattrPrefixLength,
                                      galaxy::win32::kStreamXattrPrefix) != 0) {
                    continue;  // system and foreign attributes are not streams
                }
                const std::string stream = attribute.substr(galaxy::win32::kStreamXattrPrefixLength);
                const ssize_t size = galaxy::win32::xattr_get(path, attribute, nullptr, 0);
                search->streams.emplace_back(
                    L":" + galaxy::win32::widen(stream.c_str()) + L":$DATA",
                    size < 0 ? 0 : static_cast<std::int64_t>(size));
            }
            // Named streams in a stable order after the unnamed one.
            std::sort(search->streams.begin() + (S_ISREG(info.st_mode) ? 1 : 0),
                      search->streams.end());
        }
        if (search->streams.empty()) {
            SetLastError(ERROR_HANDLE_EOF);
            return INVALID_HANDLE_VALUE;
        }
        if (fill_stream_data(*search, data) == FALSE) return INVALID_HANDLE_VALUE;
        return search.release();
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
}

HANDLE FindFirstStreamW(const wchar_t* path, STREAM_INFO_LEVELS level, void* data,
                        DWORD flags) noexcept {
    try {
        return FindFirstStreamW(narrow(path).c_str(), level, data, flags);
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
}

BOOL FindNextStreamW(HANDLE search, void* data) noexcept {
    Handle* const handle = as_handle(search);
    if (handle == nullptr || handle->kind != Kind::StreamSearch || data == nullptr) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    return fill_stream_data(*handle, data);
}

BOOL FindClose(HANDLE search) noexcept {
    Handle* const handle = as_handle(search);
    if (handle == nullptr || handle->kind != Kind::StreamSearch) return fail_bool(ERROR_INVALID_HANDLE);
    return CloseHandle(search);
}

HANDLE CreateFileMappingW(HANDLE file, SECURITY_ATTRIBUTES*, DWORD protect, DWORD maximum_size_high,
                          DWORD maximum_size_low, const wchar_t* name) noexcept {
    Handle* const source = as_handle(file);
    if (source == nullptr || source->kind != Kind::File || source->fd < 0) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    if (protect != PAGE_READONLY || name != nullptr) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    struct stat info {};
    if (::fstat(source->fd, &info) != 0) {
        SetLastError(win32_error_from_errno(errno));
        return nullptr;
    }
    std::uint64_t size = (static_cast<std::uint64_t>(maximum_size_high) << 32u) | maximum_size_low;
    if (size == 0u) size = static_cast<std::uint64_t>(info.st_size);
    if (size == 0u) {
        SetLastError(ERROR_FILE_INVALID);  // as Windows refuses an empty mapping
        return nullptr;
    }
    const int fd = ::fcntl(source->fd, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        SetLastError(win32_error_from_errno(errno));
        return nullptr;
    }
    try {
        auto mapping = std::make_unique<Handle>();
        mapping->kind = Kind::Mapping;
        mapping->fd = fd;
        mapping->mapping_size = size;
        SetLastError(ERROR_SUCCESS);
        return mapping.release();
    } catch (...) {
        ::close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
}

void* MapViewOfFile(HANDLE mapping, DWORD access, DWORD offset_high, DWORD offset_low,
                    SIZE_T bytes) noexcept {
    Handle* const handle = as_handle(mapping);
    if (handle == nullptr || handle->kind != Kind::Mapping) {
        SetLastError(ERROR_INVALID_HANDLE);
        return nullptr;
    }
    if (access != FILE_MAP_READ) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    const std::uint64_t offset = (static_cast<std::uint64_t>(offset_high) << 32u) | offset_low;
    if (offset >= handle->mapping_size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const std::uint64_t length = bytes == 0u ? handle->mapping_size - offset : bytes;
    void* const view = ::mmap(nullptr, static_cast<std::size_t>(length), PROT_READ, MAP_SHARED,
                              handle->fd, static_cast<off_t>(offset));
    if (view == MAP_FAILED) {
        SetLastError(win32_error_from_errno(errno));
        return nullptr;
    }
    try {
        const std::lock_guard<std::mutex> lock(galaxy::win32::view_mutex());
        galaxy::win32::views()[view] = static_cast<std::size_t>(length);
    } catch (...) {
        ::munmap(view, static_cast<std::size_t>(length));
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    SetLastError(ERROR_SUCCESS);
    return view;
}

BOOL UnmapViewOfFile(const void* view) noexcept {
    std::size_t length = 0;
    {
        const std::lock_guard<std::mutex> lock(galaxy::win32::view_mutex());
        auto& mapped = galaxy::win32::views();
        const auto it = mapped.find(view);
        if (it == mapped.end()) return fail_bool(ERROR_INVALID_ADDRESS);
        length = it->second;
        mapped.erase(it);
    }
    if (::munmap(const_cast<void*>(view), length) != 0) {
        return fail_bool(win32_error_from_errno(errno));
    }
    return TRUE;
}

BOOL PrefetchVirtualMemory(HANDLE, ULONG_PTR count, WIN32_MEMORY_RANGE_ENTRY* ranges,
                           ULONG) noexcept {
    static const std::uintptr_t page = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto start = reinterpret_cast<std::uintptr_t>(ranges[i].VirtualAddress);
        const std::uintptr_t aligned = start & ~(page - 1u);
        (void)::madvise(reinterpret_cast<void*>(aligned), ranges[i].NumberOfBytes + (start - aligned),
                        MADV_WILLNEED);
    }
    return TRUE;
}

HANDLE CreateEventW(SECURITY_ATTRIBUTES*, BOOL manual_reset, BOOL initial_state,
                    const wchar_t* name) noexcept {
    if (name != nullptr) {
        SetLastError(ERROR_NOT_SUPPORTED);  // named events are cross-process
        return nullptr;
    }
    auto* const event = new (std::nothrow) Handle{};
    if (event == nullptr) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    event->kind = Kind::Event;
    event->manual_reset = manual_reset != FALSE;
    event->signaled = initial_state != FALSE;
    SetLastError(ERROR_SUCCESS);
    return event;
}

BOOL SetEvent(HANDLE object) noexcept {
    Handle* const event = as_handle(object);
    if (event == nullptr || event->kind != Kind::Event) return fail_bool(ERROR_INVALID_HANDLE);
    {
        const std::lock_guard<std::mutex> lock(galaxy::win32::event_mutex());
        event->signaled = true;
    }
    galaxy::win32::event_condition().notify_all();
    return TRUE;
}

BOOL ResetEvent(HANDLE object) noexcept {
    Handle* const event = as_handle(object);
    if (event == nullptr || event->kind != Kind::Event) return fail_bool(ERROR_INVALID_HANDLE);
    const std::lock_guard<std::mutex> lock(galaxy::win32::event_mutex());
    event->signaled = false;
    return TRUE;
}

DWORD WaitForMultipleObjects(DWORD count, const HANDLE* objects, BOOL wait_all,
                             DWORD milliseconds) noexcept {
    if (count == 0u || objects == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return WAIT_FAILED;
    }
    std::vector<Handle*> waitables;
    try {
        waitables.reserve(count);
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return WAIT_FAILED;
    }
    for (DWORD i = 0; i < count; ++i) {
        Handle* const object = as_handle(objects[i]);
        if (object == nullptr || (object->kind != Kind::Event && object->kind != Kind::Timer)) {
            SetLastError(ERROR_INVALID_HANDLE);
            return WAIT_FAILED;
        }
        waitables.push_back(object);
    }
    using Clock = std::chrono::steady_clock;
    const Clock::time_point started = Clock::now();
    const bool forever = milliseconds == INFINITE;
    const Clock::time_point deadline = started + std::chrono::milliseconds(forever ? 0u : milliseconds);
    std::unique_lock<std::mutex> lock(galaxy::win32::event_mutex());
    // Fires due timers; returns the satisfied index (consuming auto-reset
    // signals) or -1.
    const auto ready = [&](Clock::time_point now) -> long {
        for (Handle* object : waitables) {
            if (object->kind == Kind::Timer && object->armed && now >= object->due) {
                object->armed = false;
                object->signaled = true;
            }
        }
        if (wait_all != FALSE) {
            for (Handle* object : waitables) {
                if (!object->signaled) return -1;
            }
            for (Handle* object : waitables) {
                if (!object->manual_reset) object->signaled = false;
            }
            return 0;
        }
        for (DWORD i = 0; i < count; ++i) {
            if (waitables[i]->signaled) {
                if (!waitables[i]->manual_reset) waitables[i]->signaled = false;
                return static_cast<long>(i);
            }
        }
        return -1;
    };
    for (;;) {
        const Clock::time_point now = Clock::now();
        const long index = ready(now);
        if (index >= 0) {
            SetLastError(ERROR_SUCCESS);
            return WAIT_OBJECT_0 + static_cast<DWORD>(index);
        }
        if (!forever && now >= deadline) {
            SetLastError(ERROR_SUCCESS);
            return WAIT_TIMEOUT;
        }
        // Sleep until a signal, the next timer, or the wait's deadline.
        Clock::time_point wake = forever ? Clock::time_point::max() : deadline;
        for (Handle* object : waitables) {
            if (object->kind == Kind::Timer && object->armed && object->due < wake) wake = object->due;
        }
        if (wake == Clock::time_point::max()) {
            galaxy::win32::event_condition().wait(lock);
        } else {
            galaxy::win32::event_condition().wait_until(lock, wake);
        }
    }
}

HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES*, const wchar_t* name, DWORD flags,
                              DWORD) noexcept {
    if (name != nullptr) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }
    auto* const timer = new (std::nothrow) Handle{};
    if (timer == nullptr) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    timer->kind = Kind::Timer;
    timer->manual_reset = (flags & CREATE_WAITABLE_TIMER_MANUAL_RESET) != 0u;
    SetLastError(ERROR_SUCCESS);
    return timer;
}

BOOL SetWaitableTimer(HANDLE object, const LARGE_INTEGER* due, LONG period, PTIMERAPCROUTINE routine,
                      void*, BOOL) noexcept {
    Handle* const timer = as_handle(object);
    if (timer == nullptr || timer->kind != Kind::Timer || due == nullptr) {
        return fail_bool(ERROR_INVALID_HANDLE);
    }
    if (period != 0 || routine != nullptr) return fail_bool(ERROR_NOT_SUPPORTED);
    using Clock = std::chrono::steady_clock;
    Clock::time_point when;
    if (due->QuadPart < 0) {
        when = Clock::now() + std::chrono::nanoseconds(-due->QuadPart * 100);
    } else {
        // Absolute FILETIME (100 ns since 1601) mapped onto the steady clock.
        constexpr long long kUnixEpochTicks = 116444736000000000ll;
        const auto unix_ns = std::chrono::nanoseconds((due->QuadPart - kUnixEpochTicks) * 100);
        const auto system_now = std::chrono::system_clock::now().time_since_epoch();
        when = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                  unix_ns - std::chrono::duration_cast<std::chrono::nanoseconds>(system_now));
    }
    {
        const std::lock_guard<std::mutex> lock(galaxy::win32::event_mutex());
        timer->signaled = false;
        timer->armed = true;
        timer->due = when;
    }
    galaxy::win32::event_condition().notify_all();  // waiters recompute their wake time
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE object) noexcept {
    Handle* const timer = as_handle(object);
    if (timer == nullptr || timer->kind != Kind::Timer) return fail_bool(ERROR_INVALID_HANDLE);
    const std::lock_guard<std::mutex> lock(galaxy::win32::event_mutex());
    timer->armed = false;
    return TRUE;
}

DWORD WaitForSingleObject(HANDLE object, DWORD milliseconds) noexcept {
    return WaitForMultipleObjects(1u, &object, FALSE, milliseconds);
}

DWORD GetEnvironmentVariableA(const char* name, char* buffer, DWORD size) noexcept {
    const char* const value = name != nullptr ? std::getenv(name) : nullptr;
    if (value == nullptr) {
        SetLastError(ERROR_ENVVAR_NOT_FOUND);
        return 0u;
    }
    const std::size_t length = std::strlen(value);
    if (buffer == nullptr || size <= length) return static_cast<DWORD>(length + 1u);
    std::memcpy(buffer, value, length + 1u);
    SetLastError(ERROR_SUCCESS);
    return static_cast<DWORD>(length);
}

int _wputenv_s(const wchar_t* name, const wchar_t* value) noexcept {
    if (name == nullptr || value == nullptr) return EINVAL;
    try {
        const std::string narrow_name = narrow(name);
        const std::string narrow_value = narrow(value);
        const int result = narrow_value.empty() ? ::unsetenv(narrow_name.c_str())
                                                : ::setenv(narrow_name.c_str(), narrow_value.c_str(), 1);
        return result == 0 ? 0 : errno;
    } catch (...) {
        return ENOMEM;
    }
}

int _wdupenv_s(wchar_t** buffer, std::size_t* length, const wchar_t* name) noexcept {
    if (buffer == nullptr || name == nullptr) return EINVAL;
    *buffer = nullptr;
    if (length != nullptr) *length = 0u;
    try {
        const char* const value = std::getenv(narrow(name).c_str());
        if (value == nullptr) return 0;
        const std::wstring wide = galaxy::win32::widen(value);
        auto* const copy = static_cast<wchar_t*>(std::malloc((wide.size() + 1u) * sizeof(wchar_t)));
        if (copy == nullptr) return ENOMEM;
        std::wmemcpy(copy, wide.c_str(), wide.size() + 1u);
        *buffer = copy;
        if (length != nullptr) *length = wide.size() + 1u;
        return 0;
    } catch (...) {
        return ENOMEM;
    }
}

DWORD GetEnvironmentVariableW(const wchar_t* name, wchar_t* buffer, DWORD size) noexcept {
    try {
        const std::string narrow_name = narrow(name);
        const char* const value = std::getenv(narrow_name.c_str());
        if (value == nullptr) {
            SetLastError(ERROR_ENVVAR_NOT_FOUND);
            return 0u;
        }
        const std::wstring wide = galaxy::win32::widen(value);
        if (buffer == nullptr || size <= wide.size()) return static_cast<DWORD>(wide.size() + 1u);
        std::wmemcpy(buffer, wide.c_str(), wide.size() + 1u);
        SetLastError(ERROR_SUCCESS);
        return static_cast<DWORD>(wide.size());
    } catch (...) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return 0u;
    }
}

#endif  // !_WIN32
