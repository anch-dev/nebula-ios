// SPDX-License-Identifier: GPL-3.0-only
// The POSIX Win32 file shim must behave like Win32 wherever the disc and save
// (NAND) layers depend on it: dispositions, error codes, named streams that
// follow their file, non-overwriting renames, in-process share modes,
// delete-on-close, file identity and volume queries.

#include "galaxy/win32_fs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s (last error %u)\n", what, static_cast<unsigned>(GetLastError()));
        ++g_failures;
    }
}

void check_error(DWORD expected, const char* what) {
    const DWORD actual = GetLastError();
    if (actual != expected) {
        std::fprintf(stderr, "FAIL: %s: error %u, expected %u\n", what, static_cast<unsigned>(actual),
                     static_cast<unsigned>(expected));
        ++g_failures;
    }
}

namespace fs = std::filesystem;
fs::path g_root;

std::string p(const char* name) { return (g_root / name).string(); }

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool write_stream(const std::string& path, const std::string& bytes, DWORD disposition) {
    HANDLE stream = CreateFileW(path.c_str(), GENERIC_WRITE, 0u, nullptr, disposition,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(stream, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    const BOOL flushed = FlushFileBuffers(stream);
    const BOOL closed = CloseHandle(stream);
    return ok != FALSE && written == bytes.size() && flushed != FALSE && closed != FALSE;
}

std::string read_stream(const std::string& path) {
    HANDLE stream = CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return "<missing>";
    std::string out(256, '\0');
    DWORD count = 0;
    ReadFile(stream, out.data(), static_cast<DWORD>(out.size()), &count, nullptr);
    out.resize(count);
    char extra = 0;
    DWORD extra_count = 1;
    ReadFile(stream, &extra, 1u, &extra_count, nullptr);
    CloseHandle(stream);
    return extra_count == 0 ? out : "<too long>";
}

std::vector<std::wstring> streams_of(const std::string& path) {
    std::vector<std::wstring> names;
    WIN32_FIND_STREAM_DATA data{};
    HANDLE search = FindFirstStreamW(path.c_str(), FindStreamInfoStandard, &data, 0u);
    if (search == INVALID_HANDLE_VALUE) return names;
    do {
        names.emplace_back(data.cStreamName);
    } while (FindNextStreamW(search, &data) != FALSE);
    check(GetLastError() == ERROR_HANDLE_EOF, "stream enumeration ends with ERROR_HANDLE_EOF");
    FindClose(search);
    return names;
}

void test_dispositions_and_errors() {
    const std::string file = p("a.bin");
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    check(h != INVALID_HANDLE_VALUE, "CREATE_NEW creates");
    DWORD written = 0;
    check(WriteFile(h, "hello", 5, &written, nullptr) && written == 5, "WriteFile");
    check(FlushFileBuffers(h) != FALSE, "FlushFileBuffers");
    check(CloseHandle(h) != FALSE, "CloseHandle");
    check(read_text(file) == "hello", "file contents");

    check(CreateFileW(file.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW, 0, nullptr) ==
              INVALID_HANDLE_VALUE, "CREATE_NEW on an existing file fails");
    check_error(ERROR_FILE_EXISTS, "CREATE_NEW error");

    check(GetFileAttributesW(p("missing").c_str()) == INVALID_FILE_ATTRIBUTES, "missing leaf");
    check_error(ERROR_FILE_NOT_FOUND, "missing leaf error");
    check(GetFileAttributesW(p("nodir/missing").c_str()) == INVALID_FILE_ATTRIBUTES, "missing parent");
    check_error(ERROR_PATH_NOT_FOUND, "missing parent error");
    check(CreateFileW(p("nodir/x").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) ==
              INVALID_HANDLE_VALUE, "open under a missing parent");
    check_error(ERROR_PATH_NOT_FOUND, "open under a missing parent error");

    check(CreateDirectoryW(p("dir").c_str(), nullptr) != FALSE, "CreateDirectoryW");
    check(CreateDirectoryW(p("dir").c_str(), nullptr) == FALSE, "CreateDirectoryW twice");
    check_error(ERROR_ALREADY_EXISTS, "CreateDirectoryW twice error");
    check((GetFileAttributesW(p("dir").c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0u, "directory attribute");
    check(CreateFileW(p("dir").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                      nullptr) == INVALID_HANDLE_VALUE, "directory needs backup semantics");
    check(DeleteFileW(p("dir").c_str()) == FALSE, "DeleteFileW on a directory");
    check_error(ERROR_ACCESS_DENIED, "DeleteFileW on a directory error");

    check(::symlink(file.c_str(), p("link").c_str()) == 0, "symlink fixture");
    const DWORD link_attributes = GetFileAttributesW(p("link").c_str());
    check(link_attributes != INVALID_FILE_ATTRIBUTES &&
              (link_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u, "symlink is a reparse point");
    HANDLE link = CreateFileW(p("link").c_str(), FILE_READ_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr);
    BY_HANDLE_FILE_INFORMATION info{};
    check(link != INVALID_HANDLE_VALUE && GetFileInformationByHandle(link, &info) &&
              (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u,
          "reparse point opens as itself");
    CloseHandle(link);

    h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char buffer[16];
    DWORD count = 99;
    check(ReadFile(h, buffer, sizeof(buffer), &count, nullptr) && count == 5, "short read");
    check(ReadFile(h, buffer, sizeof(buffer), &count, nullptr) && count == 0, "EOF reads zero");
    check(WriteFile(h, "x", 1, &count, nullptr) == FALSE, "write on a read handle");
    check_error(ERROR_ACCESS_DENIED, "write on a read handle error");
    CloseHandle(h);
}

void test_named_streams() {
    const std::string file = p("save.bin");
    write_text(file, "payload");
    const std::string meta = file + ":galaxy.isfs.meta";
    check(read_stream(meta) == "<missing>", "absent stream");
    check_error(ERROR_FILE_NOT_FOUND, "absent stream error");
    check(GetFileAttributesW(meta.c_str()) == INVALID_FILE_ATTRIBUTES, "absent stream attributes");
    check_error(ERROR_FILE_NOT_FOUND, "absent stream attributes error");

    check(write_stream(meta, std::string("GIS3\x01\x02", 6), CREATE_NEW), "stream CREATE_NEW");
    check(read_stream(meta) == std::string("GIS3\x01\x02", 6), "stream round trip");
    check(read_text(file) == "payload", "unnamed stream untouched");
    check(GetFileAttributesW(meta.c_str()) != INVALID_FILE_ATTRIBUTES, "stream attributes");
    check(!write_stream(meta, "x", CREATE_NEW), "stream CREATE_NEW twice");
    check_error(ERROR_FILE_EXISTS, "stream CREATE_NEW twice error");
    check(write_stream(meta, "replaced", CREATE_ALWAYS), "stream CREATE_ALWAYS");
    check(read_stream(meta) == "replaced", "stream replaced");
    check(read_stream(file + ":galaxy.isfs.meta:$DATA") == "replaced", ":$DATA suffix");

    // A stream opened for writing exists before its first write, as on NTFS.
    HANDLE pending = CreateFileW((file + ":galaxy.isfs.meta.pending").c_str(), GENERIC_WRITE, 0u,
                                 nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(pending != INVALID_HANDLE_VALUE, "pending stream opens");
    check(GetFileAttributesW((file + ":galaxy.isfs.meta.pending").c_str()) != INVALID_FILE_ATTRIBUTES,
          "pending stream exists before writes");
    CloseHandle(pending);

    const auto names = streams_of(file);
    check(names.size() == 3u && names[0] == L"::$DATA" &&
              names[1] == L":galaxy.isfs.meta.pending:$DATA" &&
              names[2] == L":galaxy.isfs.meta:$DATA",
          "stream enumeration lists the unnamed and named streams");
    check(DeleteFileW((file + ":galaxy.isfs.meta.pending").c_str()) != FALSE, "delete a stream");
    check(streams_of(file).size() == 2u, "deleted stream is gone");

    // Streams follow their file through rename and disappear with it.
    const std::string moved = p("moved.bin");
    check(MoveFileExW(file.c_str(), moved.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE, "rename");
    check(read_stream(moved + ":galaxy.isfs.meta") == "replaced", "stream follows rename");
    check(DeleteFileW(moved.c_str()) != FALSE, "delete file");
    write_text(moved, "new");
    check(read_stream(moved + ":galaxy.isfs.meta") == "<missing>", "stream gone with its file");

    // Streams on a directory; a directory has no unnamed stream.
    CreateDirectoryW(p("sdir").c_str(), nullptr);
    check(streams_of(p("sdir")).empty(), "empty directory has no streams");
    check(GetLastError() == ERROR_HANDLE_EOF, "no streams: ERROR_HANDLE_EOF");
    check(write_stream(p("sdir") + ":galaxy.isfs.meta", "dirmeta", CREATE_NEW), "directory stream");
    const auto dir_names = streams_of(p("sdir"));
    check(dir_names.size() == 1u && dir_names[0] == L":galaxy.isfs.meta:$DATA", "directory stream listed");

    // Creating a stream of a missing file creates the file.
    check(write_stream(p("fresh.bin") + ":galaxy.isfs.provisional", "P", CREATE_NEW),
          "stream creates its file");
    check(GetFileAttributesW(p("fresh.bin").c_str()) != INVALID_FILE_ATTRIBUTES, "file created");
}

void test_rename_semantics() {
    write_text(p("src"), "source");
    write_text(p("dst"), "target");
    check(MoveFileExW(p("src").c_str(), p("dst").c_str(), MOVEFILE_WRITE_THROUGH) == FALSE,
          "rename never overwrites without REPLACE_EXISTING");
    check_error(ERROR_ALREADY_EXISTS, "rename onto an existing file error");
    check(read_text(p("src")) == "source" && read_text(p("dst")) == "target", "both files intact");
    check(MoveFileExW(p("src").c_str(), p("dst").c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE,
          "REPLACE_EXISTING replaces");
    check(read_text(p("dst")) == "source" && GetFileAttributesW(p("src").c_str()) == INVALID_FILE_ATTRIBUTES,
          "replaced");
    check(MoveFileExW(p("nothing").c_str(), p("x").c_str(), 0) == FALSE, "rename a missing file");
    check_error(ERROR_FILE_NOT_FOUND, "rename a missing file error");
    CreateDirectoryW(p("d1").c_str(), nullptr);
    check(MoveFileExW(p("d1").c_str(), p("d2").c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
          "directories rename");
    check(RemoveDirectoryW(p("d2").c_str()) != FALSE, "RemoveDirectoryW");
    CreateDirectoryW(p("full").c_str(), nullptr);
    write_text(p("full/f"), "x");
    check(RemoveDirectoryW(p("full").c_str()) == FALSE, "non-empty directory");
    check_error(ERROR_DIR_NOT_EMPTY, "non-empty directory error");
}

void test_share_modes() {
    write_text(p("leased"), "data");
    // A lease without FILE_SHARE_DELETE pins the file in place.
    HANDLE lease = CreateFileW(p("leased").c_str(), 0u, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr);
    check(lease != INVALID_HANDLE_VALUE, "lease opens");
    check(MoveFileExW(p("leased").c_str(), p("elsewhere").c_str(), 0) == FALSE, "lease blocks rename");
    check_error(ERROR_SHARING_VIOLATION, "lease blocks rename error");
    check(DeleteFileW(p("leased").c_str()) == FALSE, "lease blocks delete");
    check_error(ERROR_SHARING_VIOLATION, "lease blocks delete error");
    HANDLE reader = CreateFileW(p("leased").c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, 0, nullptr);
    check(reader != INVALID_HANDLE_VALUE, "readers still open beside a lease");
    CloseHandle(reader);
    write_text(p("victim"), "v");
    check(MoveFileExW(p("victim").c_str(), p("leased").c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE,
          "lease blocks being replaced");
    // Share access is per stream: a lease on the file does not stop work on
    // its named streams, but blocks deleting the file as a whole.
    check(write_stream(p("leased") + ":galaxy.isfs.meta.pending", "pending", CREATE_NEW),
          "write a stream beside a lease");
    check(DeleteFileW((p("leased") + ":galaxy.isfs.meta.pending").c_str()) != FALSE,
          "delete a stream beside a lease");
    HANDLE stream_lease = CreateFileW((p("leased") + ":galaxy.isfs.meta").c_str(), GENERIC_WRITE, 0u,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(stream_lease != INVALID_HANDLE_VALUE, "exclusive stream handle beside a lease");
    CloseHandle(stream_lease);
    CloseHandle(lease);
    HANDLE open_stream = CreateFileW((p("leased") + ":galaxy.isfs.meta").c_str(), GENERIC_READ,
                                     FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    check(DeleteFileW(p("leased").c_str()) == FALSE, "an open stream without delete sharing pins its file");
    check_error(ERROR_SHARING_VIOLATION, "open stream pins its file error");
    CloseHandle(open_stream);
    check(MoveFileExW(p("leased").c_str(), p("elsewhere").c_str(), 0) != FALSE, "rename after the lease");

    // Exclusive writer: share 0 refuses every other open.
    HANDLE writer = CreateFileW(p("exclusive").c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    check(writer != INVALID_HANDLE_VALUE, "exclusive writer");
    check(CreateFileW(p("exclusive").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                      OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE, "exclusive refuses readers");
    check_error(ERROR_SHARING_VIOLATION, "exclusive refuses readers error");
    // CREATE_ALWAYS must not truncate a file it may not open.
    DWORD written = 0;
    WriteFile(writer, "keep", 4, &written, nullptr);
    FlushFileBuffers(writer);
    check(CreateFileW(p("exclusive").c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS, 0, nullptr) ==
              INVALID_HANDLE_VALUE && read_text(p("exclusive")) == "keep",
          "a refused CREATE_ALWAYS leaves the file intact");
    CloseHandle(writer);
}

void test_delete_on_close_and_identity() {
    write_text(p("doomed"), "x");
    HANDLE object = CreateFileW(p("doomed").c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    check(object != INVALID_HANDLE_VALUE, "open for delete");
    BY_HANDLE_FILE_INFORMATION before{};
    check(GetFileInformationByHandle(object, &before) && before.nNumberOfLinks == 1u, "identity");
    FILE_CASE_SENSITIVE_INFO case_info{};
    check(GetFileInformationByHandleEx(object, FileCaseSensitiveInfo, &case_info, sizeof(case_info)) ==
              FALSE, "case-sensitivity query unsupported");
    check_error(ERROR_NOT_SUPPORTED, "case-sensitivity query error");
    FILE_DISPOSITION_INFO disposition{TRUE};
    check(SetFileInformationByHandle(object, FileDispositionInfo, &disposition, sizeof(disposition)),
          "mark delete-on-close");
    check(CreateFileW(p("doomed").c_str(), GENERIC_READ,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                      0, nullptr) == INVALID_HANDLE_VALUE, "delete-pending refuses opens");
    check_error(ERROR_ACCESS_DENIED, "delete-pending error");
    check(CloseHandle(object) != FALSE, "close deletes");
    check(GetFileAttributesW(p("doomed").c_str()) == INVALID_FILE_ATTRIBUTES &&
              GetLastError() == ERROR_FILE_NOT_FOUND, "deleted on close");

    write_text(p("ident"), "i");
    const auto identity = [](const std::string& path, BY_HANDLE_FILE_INFORMATION& out) {
        HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                               nullptr);
        const bool ok = h != INVALID_HANDLE_VALUE && GetFileInformationByHandle(h, &out) != FALSE;
        CloseHandle(h);
        return ok;
    };
    BY_HANDLE_FILE_INFORMATION a{}, b{}, dir{};
    check(identity(p("ident"), a), "identity a");
    MoveFileExW(p("ident").c_str(), p("ident2").c_str(), 0);
    check(identity(p("ident2"), b), "identity b");
    check(a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexLow == b.nFileIndexLow &&
              a.nFileIndexHigh == b.nFileIndexHigh && a.dwVolumeSerialNumber != 0u,
          "identity survives rename");
    // A recreated object never takes over the old identity (NTFS file IDs do
    // not repeat), even where the filesystem reuses inode numbers.
    BY_HANDLE_FILE_INFORMATION first{}, second{};
    CreateDirectoryW(p("reused").c_str(), nullptr);
    check(identity(p("reused"), first), "identity of the first object");
    RemoveDirectoryW(p("reused").c_str());
    CreateDirectoryW(p("reused").c_str(), nullptr);
    check(identity(p("reused"), second), "identity of the recreated object");
    check(first.nFileIndexLow != second.nFileIndexLow || first.nFileIndexHigh != second.nFileIndexHigh,
          "a recreated object has a new identity");
    CreateDirectoryW(p("idir").c_str(), nullptr);
    CreateDirectoryW(p("idir/child").c_str(), nullptr);
    check(identity(p("idir"), dir) && dir.nNumberOfLinks == 1u &&
              (dir.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u,
          "directories report one link");
    check(CreateHardLinkW(p("ident3").c_str(), p("ident2").c_str(), nullptr) != FALSE, "hard link");
    check(identity(p("ident2"), b) && b.nNumberOfLinks == 2u, "link count 2");

    wchar_t volume[MAX_PATH]{};
    DWORD serial = 0;
    DWORD flags = 0;
    check(GetVolumePathNameW(p("ident2").c_str(), volume, MAX_PATH) != FALSE, "volume path");
    check(GetVolumeInformationW(volume, nullptr, 0, &serial, nullptr, &flags, nullptr, 0) != FALSE,
          "volume information");
    check(serial == b.dwVolumeSerialNumber, "volume serial matches the file's");
    check((flags & FILE_NAMED_STREAMS) != 0u, "named streams supported");
}

void test_events_and_mappings() {
    HANDLE automatic = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE manual = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    check(automatic != nullptr && manual != nullptr, "events created");
    check(WaitForSingleObject(automatic, 0) == WAIT_TIMEOUT, "unsignaled event times out");
    SetEvent(automatic);
    check(WaitForSingleObject(automatic, 0) == WAIT_OBJECT_0, "signaled event");
    check(WaitForSingleObject(automatic, 0) == WAIT_TIMEOUT, "auto-reset consumed the signal");
    SetEvent(manual);
    check(WaitForSingleObject(manual, 0) == WAIT_OBJECT_0 &&
              WaitForSingleObject(manual, 0) == WAIT_OBJECT_0, "manual-reset stays signaled");
    ResetEvent(manual);
    check(WaitForSingleObject(manual, 0) == WAIT_TIMEOUT, "ResetEvent");
    const HANDLE both[] = {automatic, manual};
    std::thread setter([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        SetEvent(manual);
    });
    check(WaitForMultipleObjects(2, both, FALSE, 2000) == WAIT_OBJECT_0 + 1, "wait-any wakes on the second");
    setter.join();
    check(WaitForSingleObject(automatic, 30) == WAIT_TIMEOUT, "timed wait expires");
    check(CloseHandle(automatic) && CloseHandle(manual), "events close");

    // Waitable timers: relative due times, precise wakes, cancel and re-arm.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_MODIFY_STATE | SYNCHRONIZE);
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    check(timer != nullptr && wake != nullptr, "timer created");
    LARGE_INTEGER due{};
    due.QuadPart = -30'000;  // 3 ms
    const auto begin = std::chrono::steady_clock::now();
    check(SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE, "timer armed");
    const HANDLE pair[] = {wake, timer};
    check(WaitForMultipleObjects(2, pair, FALSE, INFINITE) == WAIT_OBJECT_0 + 1, "timer fires");
    const auto waited = std::chrono::steady_clock::now() - begin;
    check(waited >= std::chrono::microseconds(2900) && waited < std::chrono::milliseconds(200),
          "timer fires at its due time");
    check(WaitForSingleObject(timer, 0) == WAIT_TIMEOUT, "synchronization timer resets");
    due.QuadPart = -50'000'000;  // 5 s
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    SetEvent(wake);
    check(WaitForMultipleObjects(2, pair, FALSE, INFINITE) == WAIT_OBJECT_0, "event wins before the timer");
    check(CancelWaitableTimer(timer) != FALSE, "cancel");
    check(WaitForSingleObject(timer, 20) == WAIT_TIMEOUT, "cancelled timer never fires");
    due.QuadPart = -10'000;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    check(WaitForSingleObject(timer, 1000) == WAIT_OBJECT_0, "re-armed timer fires");
    CloseHandle(timer);
    CloseHandle(wake);

    write_text(p("pak"), "0123456789abcdef");
    HANDLE file = CreateFileW(p("pak").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size{};
    check(GetFileSizeEx(file, &size) && size.QuadPart == 16, "GetFileSizeEx");
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    check(mapping != nullptr, "file mapping");
    const auto* view = static_cast<const char*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    check(view != nullptr && std::memcmp(view, "0123456789abcdef", 16) == 0, "mapped view");
    WIN32_MEMORY_RANGE_ENTRY range{const_cast<char*>(view) + 3, 8};
    check(PrefetchVirtualMemory(nullptr, 1, &range, 0) != FALSE, "prefetch");
    check(UnmapViewOfFile(view) != FALSE, "unmap");
    check(UnmapViewOfFile(view) == FALSE, "double unmap refused");
    check(CloseHandle(mapping) && CloseHandle(file), "mapping and file close");
}

void test_environment() {
    ::setenv("GALAXY_SHIM_TEST", "valueé", 1);
    wchar_t buffer[32]{};
    const DWORD length = GetEnvironmentVariableW(L"GALAXY_SHIM_TEST", buffer, 32u);
    check(length == 6u && std::wstring(buffer) == L"valueé", "wide environment lookup");
    check(GetEnvironmentVariableW(L"GALAXY_SHIM_TEST", buffer, 3u) == 7u, "size query");
    check(GetEnvironmentVariableW(L"GALAXY_SHIM_MISSING", buffer, 32u) == 0u, "missing variable");
    check_error(ERROR_ENVVAR_NOT_FOUND, "missing variable error");
}

}  // namespace

int main() {
    const char* const base = std::getenv("TMPDIR");
    g_root = fs::path(base != nullptr && *base != '\0' ? base : "/tmp") /
             ("galaxy_win32_fs_tests_" + std::to_string(::getpid()));
    fs::remove_all(g_root);
    fs::create_directories(g_root);
    test_dispositions_and_errors();
    test_named_streams();
    test_rename_semantics();
    test_share_modes();
    test_delete_on_close_and_identity();
    test_events_and_mappings();
    test_environment();
    fs::remove_all(g_root);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d win32 fs check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("win32 fs shim tests passed");
    return 0;
}
