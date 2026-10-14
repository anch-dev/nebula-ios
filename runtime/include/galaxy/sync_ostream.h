// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// galaxy::osyncstream: std::osyncstream where the standard library has it,
// otherwise a buffer that writes its whole contents to the target stream
// under one process-wide lock on destruction (Apple's libc++ ships
// <syncstream> only behind -fexperimental-library).

#include <version>

#if defined(__cpp_lib_syncbuf)
#include <syncstream>
namespace galaxy {
using osyncstream = std::osyncstream;
}
#else
#include <mutex>
#include <ostream>
#include <sstream>
namespace galaxy {
class osyncstream : public std::ostringstream {
public:
    explicit osyncstream(std::ostream& target) : target_(target) {}
    osyncstream(const osyncstream&) = delete;
    osyncstream& operator=(const osyncstream&) = delete;
    ~osyncstream() override { emit(); }
    void emit() {
        const std::string text = str();
        if (text.empty()) return;
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        target_ << text;
        target_.flush();
        str({});
    }
    std::ostream* get_wrapped() const noexcept { return &target_; }

private:
    std::ostream& target_;
};
}  // namespace galaxy
#endif
