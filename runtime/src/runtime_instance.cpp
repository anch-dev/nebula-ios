#include "galaxy/runtime_instance.h"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <string>

namespace galaxy {

RuntimeInstanceLease::~RuntimeInstanceLease() noexcept {
    if (mutex_ != nullptr) {
        (void)ReleaseMutex(static_cast<HANDLE>(mutex_));
        (void)CloseHandle(static_cast<HANDLE>(mutex_));
    }
}

RuntimeInstanceLease::RuntimeInstanceLease(
    RuntimeInstanceLease&& other) noexcept
    : mutex_(other.mutex_) {
    other.mutex_ = nullptr;
}

RuntimeInstanceLease& RuntimeInstanceLease::operator=(
    RuntimeInstanceLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (mutex_ != nullptr) {
        (void)ReleaseMutex(static_cast<HANDLE>(mutex_));
        (void)CloseHandle(static_cast<HANDLE>(mutex_));
    }
    mutex_ = other.mutex_;
    other.mutex_ = nullptr;
    return *this;
}

bool RuntimeInstanceLease::acquire(
    std::wstring_view product_identity,
    bool& already_running) noexcept {
    already_running = false;
    if (mutex_ != nullptr || product_identity.empty() ||
        product_identity.find(L'\\') != std::wstring_view::npos) {
        return false;
    }

    try {
        const std::wstring name =
            std::wstring(L"Local\\") + std::wstring(product_identity);
        SetLastError(ERROR_SUCCESS);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
        if (mutex == nullptr) {
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            already_running = true;
            (void)CloseHandle(mutex);
            return false;
        }
        mutex_ = mutex;
        return true;
    } catch (...) {
        return false;
    }
}

bool RuntimeInstanceLease::owns_lease() const noexcept {
    return mutex_ != nullptr;
}

}  // namespace galaxy

#else  // POSIX

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

// POSIX: an exclusive flock on a per-product lock file in the temporary
// directory. flock conflicts between separate opens even in one process,
// matching a second CreateMutexW reporting ERROR_ALREADY_EXISTS, and the lock
// disappears with the process.

namespace galaxy {
namespace {

// mutex_ holds the descriptor + 1 so that descriptor 0 is still non-null.
void* encode(int fd) noexcept {
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(fd) + 1);
}
int decode(void* handle) noexcept {
    return static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
}
void release(void* handle) noexcept {
    if (handle != nullptr) {
        const int fd = decode(handle);
        (void)flock(fd, LOCK_UN);
        (void)close(fd);
    }
}

}  // namespace

RuntimeInstanceLease::~RuntimeInstanceLease() noexcept { release(mutex_); }

RuntimeInstanceLease::RuntimeInstanceLease(RuntimeInstanceLease&& other) noexcept
    : mutex_(other.mutex_) {
    other.mutex_ = nullptr;
}

RuntimeInstanceLease& RuntimeInstanceLease::operator=(
    RuntimeInstanceLease&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    release(mutex_);
    mutex_ = other.mutex_;
    other.mutex_ = nullptr;
    return *this;
}

bool RuntimeInstanceLease::acquire(
    std::wstring_view product_identity,
    bool& already_running) noexcept {
    already_running = false;
    if (mutex_ != nullptr || product_identity.empty()) {
        return false;
    }
    try {
        std::string name;
        for (const wchar_t c : product_identity) {
            // ASCII only, and nothing that leaves the directory.
            if (c <= 0x20 || c >= 0x7F || c == L'\\' || c == L'/') return false;
            name.push_back(static_cast<char>(c));
        }
        const char* directory = std::getenv("TMPDIR");
        std::string path = directory != nullptr && directory[0] != '\0' ? directory : "/tmp";
        if (path.back() != '/') path.push_back('/');
        path += name + ".lock";
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
            already_running = errno == EWOULDBLOCK;
            (void)close(fd);
            return false;
        }
        mutex_ = encode(fd);
        return true;
    } catch (...) {
        return false;
    }
}

bool RuntimeInstanceLease::owns_lease() const noexcept {
    return mutex_ != nullptr;
}

}  // namespace galaxy

#endif
