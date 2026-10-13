// SPDX-License-Identifier: GPL-3.0-only
#include "galaxy/host_event.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <stdexcept>
#include <system_error>

namespace galaxy::host {

Event event_create() noexcept { return CreateEventW(nullptr, FALSE, FALSE, nullptr); }

void event_signal(Event event) noexcept {
    if (event != nullptr) (void)SetEvent(static_cast<HANDLE>(event));
}

void event_close(Event event) noexcept {
    if (event != nullptr) (void)CloseHandle(static_cast<HANDLE>(event));
}

bool event_wait(Event event, std::optional<std::chrono::nanoseconds> timeout) {
    if (event == nullptr) throw std::runtime_error("waiting on a null event");
    DWORD milliseconds = INFINITE;
    if (timeout.has_value()) {
        const auto rounded = std::chrono::ceil<std::chrono::milliseconds>(*timeout).count();
        milliseconds = rounded <= 0 ? 0u : static_cast<DWORD>(rounded);
    }
    const DWORD result = WaitForSingleObject(static_cast<HANDLE>(event), milliseconds);
    if (result == WAIT_OBJECT_0) return true;
    if (result == WAIT_TIMEOUT) return false;
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                            "event wait failed");
}

}  // namespace galaxy::host

#else

#include <condition_variable>
#include <mutex>
#include <new>
#include <stdexcept>

namespace galaxy::host {
namespace {

struct PosixEvent {
    std::mutex mutex;
    std::condition_variable condition;
    bool signaled = false;
};

}  // namespace

Event event_create() noexcept { return new (std::nothrow) PosixEvent{}; }

void event_signal(Event event) noexcept {
    auto* const target = static_cast<PosixEvent*>(event);
    if (target == nullptr) return;
    {
        const std::lock_guard<std::mutex> lock(target->mutex);
        target->signaled = true;
    }
    target->condition.notify_one();
}

void event_close(Event event) noexcept { delete static_cast<PosixEvent*>(event); }

bool event_wait(Event event, std::optional<std::chrono::nanoseconds> timeout) {
    auto* const target = static_cast<PosixEvent*>(event);
    if (target == nullptr) throw std::runtime_error("waiting on a null event");
    std::unique_lock<std::mutex> lock(target->mutex);
    if (timeout.has_value()) {
        // steady_clock deadline: immune to wall-clock changes.
        const auto deadline = std::chrono::steady_clock::now() + *timeout;
        if (!target->condition.wait_until(lock, deadline, [&] { return target->signaled; })) {
            return false;
        }
    } else {
        target->condition.wait(lock, [&] { return target->signaled; });
    }
    target->signaled = false;  // auto-reset
    return true;
}

}  // namespace galaxy::host

#endif
