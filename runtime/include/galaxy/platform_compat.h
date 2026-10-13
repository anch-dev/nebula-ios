#pragma once

// Small shims for MSVC CRT functions the runtime and generated game code use,
// so the same sources build with libc/libc++ on Linux, macOS and iOS. On
// Windows this header adds nothing.

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#if !defined(_WIN32)

// errno_t getenv_s(size_t* required, char* buffer, size_t size, const char* name)
// Matches the MSVC contract: `*required` is the value's length including the
// terminator (0 when unset), and a too-small buffer fails with ERANGE.
inline int getenv_s(std::size_t* required, char* buffer, std::size_t size,
                    const char* name) noexcept {
    if (required == nullptr || name == nullptr || (buffer == nullptr && size != 0u)) {
        return EINVAL;
    }
    const char* const value = std::getenv(name);
    if (value == nullptr) {
        *required = 0u;
        if (buffer != nullptr && size != 0u) buffer[0] = '\0';
        return 0;
    }
    const std::size_t needed = std::strlen(value) + 1u;
    *required = needed;
    if (buffer == nullptr && size == 0u) return 0;
    if (needed > size) {
        if (size != 0u) buffer[0] = '\0';
        return ERANGE;
    }
    std::memcpy(buffer, value, needed);
    return 0;
}

#endif
