#pragma once

// Small shims for MSVC CRT functions the runtime and generated game code use,
// so the same sources build with libc/libc++ on Linux, macOS and iOS. On
// Windows this header adds nothing.

#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <sys/types.h>
#if !defined(_WIN32)
#include <strings.h>
#include <unistd.h>
#include <wctype.h>
#endif

#if !defined(_WIN32)

// __rdtsc: the runtime's cycle-count profiling. x86 hosts have the
// intrinsic; AArch64 reads the virtual counter (a fixed frequency, not CPU
// cycles). (_mm_pause comes from galaxy/simd_compat.h; this header must not
// pull in arm_neon.h, whose float32_t clashes with SoftFloat's.)
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#elif defined(__aarch64__)
inline std::uint64_t __rdtsc() noexcept {
    std::uint64_t value;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
    return value;
}
#endif

#include <clocale>
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

// MSVC per-call locale functions, on POSIX locale_t. Conversions switch the
// calling thread's locale for the duration of the call (uselocale).
using _locale_t = locale_t;
#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif

inline _locale_t _create_locale(int category, const char* name) noexcept {
    int mask = LC_ALL_MASK;
    if (category == LC_NUMERIC) mask = LC_NUMERIC_MASK;
    else if (category == LC_CTYPE) mask = LC_CTYPE_MASK;
    const locale_t created = newlocale(mask, name, static_cast<locale_t>(0));
    return created;
}

inline void _free_locale(_locale_t locale) noexcept {
    if (locale != static_cast<locale_t>(0)) freelocale(locale);
}

inline float _strtof_l(const char* text, char** end, _locale_t locale) noexcept {
    const locale_t previous = uselocale(locale);
    const float value = std::strtof(text, end);
    uselocale(previous);
    return value;
}

inline double _strtod_l(const char* text, char** end, _locale_t locale) noexcept {
    const locale_t previous = uselocale(locale);
    const double value = std::strtod(text, end);
    uselocale(previous);
    return value;
}

// int _snprintf_s_l(char* buffer, size_t size, size_t count, const char* format,
//                   _locale_t locale, ...): with _TRUNCATE, truncates and
// returns -1 when the output did not fit.
inline int _snprintf_s_l(char* buffer, std::size_t size, std::size_t count,
                         const char* format, _locale_t locale, ...) noexcept {
    if (buffer == nullptr || size == 0u) return -1;
    const std::size_t limit = count == _TRUNCATE || count + 1u > size ? size : count + 1u;
    va_list arguments;
    va_start(arguments, locale);
    const locale_t previous = uselocale(locale);
    const int written = std::vsnprintf(buffer, limit, format, arguments);
    uselocale(previous);
    va_end(arguments);
    if (written < 0) {
        buffer[0] = '\0';
        return -1;
    }
    return static_cast<std::size_t>(written) >= limit ? -1 : written;
}

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

// errno_t _putenv_s(const char* name, const char* value): an empty value
// removes the variable, as on Windows.
inline int _putenv_s(const char* name, const char* value) noexcept {
    if (name == nullptr || value == nullptr) return EINVAL;
    const int result = value[0] == '\0' ? unsetenv(name) : setenv(name, value, 1);
    return result == 0 ? 0 : errno;
}

// errno_t _dupenv_s(char** buffer, size_t* length, const char* name): a
// malloc'd copy the caller frees with free(), or null when unset.
inline int _dupenv_s(char** buffer, std::size_t* length, const char* name) noexcept {
    if (buffer == nullptr || name == nullptr) return EINVAL;
    *buffer = nullptr;
    if (length != nullptr) *length = 0u;
    const char* const value = std::getenv(name);
    if (value == nullptr) return 0;
    const std::size_t bytes = std::strlen(value) + 1u;
    char* const copy = static_cast<char*>(std::malloc(bytes));
    if (copy == nullptr) return ENOMEM;
    std::memcpy(copy, value, bytes);
    *buffer = copy;
    if (length != nullptr) *length = bytes;
    return 0;
}

// MSVC 128-bit multiply/divide intrinsics, through unsigned __int128.
inline std::uint64_t _umul128(std::uint64_t a, std::uint64_t b,
                                   std::uint64_t* high) noexcept {
    const unsigned __int128 product = static_cast<unsigned __int128>(a) * b;
    *high = static_cast<std::uint64_t>(product >> 64);
    return static_cast<std::uint64_t>(product);
}

// Divides high:low by divisor; the quotient must fit in 64 bits (high < divisor).
inline std::uint64_t _udiv128(std::uint64_t high, std::uint64_t low,
                                   std::uint64_t divisor,
                                   std::uint64_t* remainder) noexcept {
    const unsigned __int128 dividend = (static_cast<unsigned __int128>(high) << 64) | low;
    *remainder = static_cast<std::uint64_t>(dividend % divisor);
    return static_cast<std::uint64_t>(dividend / divisor);
}

// MSVC bounded string functions.
using errno_t = int;
inline std::size_t strnlen_s(const char* text, std::size_t maximum) noexcept {
    return text == nullptr ? 0u : ::strnlen(text, maximum);
}
#ifndef _TRUNCATE
#define _TRUNCATE (static_cast<std::size_t>(-1))
#endif
// strncpy_s(dest, size, src, count): copies at most `count` characters (all
// that fit, with _TRUNCATE) and always terminates; STRUNCATE (80) when cut.
inline errno_t strncpy_s(char* destination, std::size_t size, const char* source,
                         std::size_t count) noexcept {
    if (destination == nullptr || size == 0u) return EINVAL;
    if (source == nullptr) {
        destination[0] = '\0';
        return EINVAL;
    }
    const std::size_t available = ::strnlen(source, count == _TRUNCATE ? size : count);
    if (available >= size) {
        if (count != _TRUNCATE) {
            destination[0] = '\0';
            return ERANGE;
        }
        std::memcpy(destination, source, size - 1u);
        destination[size - 1u] = '\0';
        return 80;  // STRUNCATE
    }
    std::memcpy(destination, source, available);
    destination[available] = '\0';
    return 0;
}

inline errno_t fopen_s(std::FILE** file, const char* path, const char* mode) noexcept {
    if (file == nullptr) return EINVAL;
    *file = std::fopen(path, mode);
    return *file != nullptr ? 0 : errno;
}

// _wfopen_s with a std::filesystem::path::c_str() (narrow on POSIX) and an
// ASCII wide mode string.
inline errno_t _wfopen_s(std::FILE** file, const char* path, const wchar_t* mode) noexcept {
    char narrow_mode[8]{};
    for (std::size_t i = 0; i + 1u < sizeof(narrow_mode) && mode[i] != L'\0'; ++i) {
        narrow_mode[i] = static_cast<char>(mode[i]);
    }
    return fopen_s(file, path, narrow_mode);
}

// 64-bit file positions (off_t is 64-bit on every supported host).
#ifndef __int64
#define __int64 long long
#endif
inline long long _ftelli64(std::FILE* file) noexcept {
    return static_cast<long long>(::ftello(file));
}
inline int _fseeki64(std::FILE* file, long long offset, int origin) noexcept {
    return ::fseeko(file, static_cast<off_t>(offset), origin);
}
// _wfsopen: share flags are a Windows notion; the path is narrow on POSIX.
inline std::FILE* _wfsopen(const char* path, const wchar_t* mode, int) noexcept {
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path, mode) == 0 ? file : nullptr;
}
// swprintf_s(buffer, format, ...) for fixed arrays; formats follow the C
// library's rules (%s takes a narrow string on POSIX: use %ls).
template <std::size_t N, typename... Args>
inline int swprintf_s(wchar_t (&buffer)[N], const wchar_t* format, Args... args) noexcept {
    const int written = std::swprintf(buffer, N, format, args...);
    if (written < 0 && N > 0u) buffer[0] = L'\0';
    return written;
}
template <typename... Args>
inline int swprintf_s(wchar_t* buffer, std::size_t size, const wchar_t* format,
                      Args... args) noexcept {
    const int written = std::swprintf(buffer, size, format, args...);
    if (written < 0 && size > 0u) buffer[0] = L'\0';
    return written;
}

inline int _getpid() noexcept { return static_cast<int>(::getpid()); }

inline int _stricmp(const char* left, const char* right) noexcept {
    return ::strcasecmp(left, right);
}
inline int _strnicmp(const char* left, const char* right, std::size_t count) noexcept {
    return ::strncasecmp(left, right, count);
}
inline int _wcsicmp(const wchar_t* left, const wchar_t* right) noexcept {
    return ::wcscasecmp(left, right);
}

// sscanf_s differs from sscanf only for %s/%c/%[ (which take a size); the
// runtime uses it for numeric conversions alone.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
#endif
template <typename... Args>
inline int sscanf_s(const char* text, const char* format, Args... args) noexcept {
    return std::sscanf(text, format, args...);
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#endif
