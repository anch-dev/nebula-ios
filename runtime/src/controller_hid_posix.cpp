// SPDX-License-Identifier: GPL-3.0-only
// galaxy/controller_hid.h on non-Windows hosts.
//
// Windows reads Switch controllers directly over HID/WinUSB. On Apple hosts
// the GameController framework owns those devices; the app shell publishes
// every pad (Switch included) as an XInput-format pad through
// galaxy/host_platform.h, so no Switch HID pad is reported here.
//
// Vibration keeps the Windows behaviour for XInput targets: the game's
// published motor state passes the same gate (rumble::motor_should_run) and
// becomes XInputSetState calls, which the shell reads back with
// host_platform::pad_rumble. A worker re-evaluates the gate every 25 ms
// while a motor runs, as the Windows worker's timer does.

#if !defined(_WIN32)

#include "galaxy/controller_hid.h"
#include "galaxy/rumble.h"
#include "galaxy/win32_host_posix.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>

namespace galaxy::host {
namespace {

std::uint64_t steady_us() noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool env_flag(const char* name) noexcept {
    const char* const value = std::getenv(name);
    if (value == nullptr || *value == '\0') return false;
    std::string lowered;
    for (const char* c = value; *c != '\0' && lowered.size() < 16u; ++c) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*c))));
    }
    return !(lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no");
}

RumbleTarget test_rumble_target() noexcept {
    static const RumbleTarget target = [] {
        RumbleTarget parsed{};
        const char* const value = std::getenv("GALAXY_TEST_RUMBLE_TARGET");
        if (value == nullptr || !rumble_output_disabled()) return parsed;
        const std::string_view text(value);
        const std::size_t colon = text.find(':');
        if (colon == std::string_view::npos || colon + 2u != text.size()) return parsed;
        const char digit = text[colon + 1u];
        if (digit < '0' || digit > '3') return parsed;
        const std::string_view kind = text.substr(0, colon);
        if (kind == "xinput") parsed.kind = RumbleTarget::Kind::XInput;
        else if (kind == "switch") parsed.kind = RumbleTarget::Kind::Switch;
        parsed.slot = static_cast<std::uint8_t>(digit - '0');
        return parsed;
    }();
    return target;
}

struct Hub {
    std::mutex lifecycle;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    bool changed = false;
    std::atomic<bool> started{false};
    RumbleRequest request{};
    std::uint64_t published_us = 0;
    std::atomic<bool> rumble_shutdown{false};
    std::atomic<std::uint8_t> xinput_rumbling{0};
};

Hub& hub() {
    static Hub* instance = new Hub;  // never destroyed, as on Windows
    return *instance;
}

class Worker {
public:
    void run() {
        Hub& h = hub();
        std::unique_lock<std::mutex> lock(h.mutex);
        for (;;) {
            if (h.stopping) break;
            const RumbleRequest request = h.request;
            const std::uint64_t published = h.published_us;
            h.changed = false;
            lock.unlock();
            const bool running = service(request, published);
            lock.lock();
            if (h.stopping) break;
            if (running) {
                h.wake.wait_for(lock, std::chrono::milliseconds(25), [&] { return h.stopping || h.changed; });
            } else {
                h.wake.wait(lock, [&] { return h.stopping || h.changed; });
            }
        }
        lock.unlock();
        stop_motors();
    }

private:
    // True while a motor runs (the gate must be re-evaluated over time).
    bool service(const RumbleRequest& request, std::uint64_t published) {
        Hub& h = hub();
        input::rumble::GateInputs gate{};
        gate.game_motor_on = request.level != 0u;
        gate.now_us = steady_us();
        gate.published_us = published;
        gate.focused = request.focused;
        gate.paused = request.paused;
        gate.strength = request.strength;
        const bool run = !rumble_output_disabled() &&
                         !h.rumble_shutdown.load(std::memory_order_acquire) &&
                         input::rumble::motor_should_run(gate);
        const std::uint8_t level = run ? request.level : 0u;
        const bool xinput = level != 0u && request.target.kind == RumbleTarget::Kind::XInput &&
                            request.target.slot < XUSER_MAX_COUNT;
        const std::uint8_t want_mask = xinput ? static_cast<std::uint8_t>(1u << request.target.slot) : 0u;
        const std::uint8_t want_level = xinput ? level : 0u;
        const std::uint8_t want_strength = xinput ? request.strength : 0u;
        if (want_mask != want_mask_ || want_level != want_level_ || want_strength != want_strength_) {
            want_mask_ = want_mask;
            want_level_ = want_level;
            want_strength_ = want_strength;
            for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
                const auto bit = static_cast<std::uint8_t>(1u << slot);
                if ((want_mask & bit) != 0u) {
                    const input::rumble::XInputMotors speeds =
                        input::rumble::xinput_motor_speeds(want_level, want_strength);
                    XINPUT_VIBRATION vibration{speeds.left, speeds.right};
                    h.xinput_rumbling.fetch_or(bit, std::memory_order_acq_rel);
                    if (XInputSetState(slot, &vibration) == ERROR_SUCCESS) {
                        on_ |= bit;
                    } else {
                        h.xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
                    }
                } else if ((on_ & bit) != 0u) {
                    XINPUT_VIBRATION off{};
                    (void)XInputSetState(slot, &off);
                    on_ = static_cast<std::uint8_t>(on_ & ~bit);
                    h.xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
                }
            }
        }
        return on_ != 0u;
    }

    void stop_motors() {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
            const auto bit = static_cast<std::uint8_t>(1u << slot);
            if ((on_ & bit) == 0u) continue;
            XINPUT_VIBRATION off{};
            (void)XInputSetState(slot, &off);
            hub().xinput_rumbling.fetch_and(static_cast<std::uint8_t>(~bit), std::memory_order_acq_rel);
        }
        on_ = 0u;
    }

    std::uint8_t on_ = 0u;
    std::uint8_t want_mask_ = 0u;
    std::uint8_t want_level_ = 0u;
    std::uint8_t want_strength_ = 0u;
};

}  // namespace

void ensure_controller_hid_started() noexcept {
    Hub& h = hub();
    if (h.started.load(std::memory_order_acquire)) return;
    const std::lock_guard<std::mutex> lock(h.lifecycle);
    if (h.started.load(std::memory_order_relaxed)) return;
    try {
        static const bool exit_hook = [] {
            std::atexit([] { stop_all_rumble(); });
            return true;
        }();
        (void)exit_hook;
        {
            const std::lock_guard<std::mutex> state(h.mutex);
            h.stopping = false;
        }
        h.worker = std::thread([] {
            Worker worker;
            worker.run();
        });
        h.started.store(true, std::memory_order_release);
    } catch (...) {
    }
}

void stop_controller_hid() noexcept {
    Hub& h = hub();
    const std::lock_guard<std::mutex> lock(h.lifecycle);
    if (!h.started.load(std::memory_order_acquire)) return;
    {
        const std::lock_guard<std::mutex> state(h.mutex);
        h.stopping = true;
    }
    h.wake.notify_all();
    if (h.worker.joinable()) h.worker.join();
    h.started.store(false, std::memory_order_release);
}

std::size_t read_switch_pads(std::span<SwitchPadState>, bool) noexcept { return 0u; }

std::uint64_t controller_hid_now_us() noexcept { return steady_us(); }

bool rumble_output_disabled() noexcept {
    static const bool disabled = env_flag("GALAXY_HIDE_WINDOW") || env_flag("GALAXY_RUMBLE_TRACE");
    return disabled;
}

void publish_rumble_request(const RumbleRequest& published) noexcept {
    RumbleRequest request = published;
    const RumbleTarget test_target = test_rumble_target();
    if (test_target.kind != RumbleTarget::Kind::None) {
        request.target = test_target;
        request.focused = true;
    }
    Hub& h = hub();
    bool changed = false;
    {
        const std::lock_guard<std::mutex> lock(h.mutex);
        const RumbleRequest& previous = h.request;
        changed = previous.level != request.level || previous.strength != request.strength ||
                  !(previous.target == request.target) || previous.focused != request.focused ||
                  previous.paused != request.paused;
        h.request = request;
        h.published_us = steady_us();
        if (changed) h.changed = true;
    }
    if (changed && h.started.load(std::memory_order_acquire)) h.wake.notify_all();
}

void stop_all_rumble() noexcept {
    Hub& h = hub();
    h.rumble_shutdown.store(true, std::memory_order_release);
    const std::uint8_t xinput = h.xinput_rumbling.exchange(0u, std::memory_order_acq_rel);
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        if ((xinput & (1u << slot)) == 0u) continue;
        XINPUT_VIBRATION off{};
        (void)XInputSetState(slot, &off);
    }
}

ControllerHidStatus controller_hid_status() noexcept { return {}; }

bool switch_controller_present_now() noexcept { return false; }

}  // namespace galaxy::host

#endif  // !_WIN32
