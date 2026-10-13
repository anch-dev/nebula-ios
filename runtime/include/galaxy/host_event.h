// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Auto-reset events with the semantics of CreateEventW(nullptr, FALSE, FALSE,
// nullptr): a signal releases exactly one wait, and a signal with no waiter is
// kept until the next wait consumes it. Handles are opaque pointers so they
// fit the runtime's existing `void*` event slots (for example
// PendingEventLatch::set_idle_wake_event). On Windows they are Win32 event
// HANDLEs; elsewhere a mutex/condition-variable pair.

#include <chrono>
#include <optional>

namespace galaxy::host {

using Event = void*;

// Returns nullptr on failure.
[[nodiscard]] Event event_create() noexcept;
void event_signal(Event event) noexcept;
void event_close(Event event) noexcept;

// Waits for a signal, at most `timeout` when one is given. Returns true when
// the event was signaled (and consumes the signal), false on timeout.
bool event_wait(Event event, std::optional<std::chrono::nanoseconds> timeout = std::nullopt);

}  // namespace galaxy::host
