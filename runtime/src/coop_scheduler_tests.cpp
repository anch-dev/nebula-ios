// SPDX-License-Identifier: GPL-3.0-only
// Cooperative scheduler on host fibers: a higher-priority guest thread
// preempts the boot thread on SelectThread, runs on its own fiber, blocks,
// and is later resumed on that same fiber with its registers intact.

#include "galaxy/coop_scheduler.h"
#include "galaxy/host_fiber.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

using namespace galaxy::coop;
using galaxy::PpcContext;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

constexpr std::uint32_t kBootThread = 0x80100000u;
constexpr std::uint32_t kWorkerThread = 0x80101000u;
constexpr std::uint32_t kBootPriority = 16u;
constexpr std::uint32_t kWorkerPriority = 10u;

std::byte* map_guest_view() {
    constexpr std::size_t kBytes = std::size_t{1} << 32;
#if defined(_WIN32)
    return static_cast<std::byte*>(
        VirtualAlloc(nullptr, kBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* const base = mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, flags, -1, 0);
    return base == MAP_FAILED ? nullptr : static_cast<std::byte*>(base);
#endif
}

struct Harness {
    GuestRam ram;
    PpcContext context{};
    std::unique_ptr<Scheduler> scheduler;
    std::vector<int> events;
    std::uint32_t exchanges = 0u;
    int idle_calls = 0;
    int worker_runs = 0;
};

Harness* g_harness = nullptr;

void make_ready(GuestRam ram, std::uint32_t thread, std::uint32_t priority) {
    // SDK SetRun on an empty queue.
    const std::uint32_t queue = os::kRunQueue + priority * 8u;
    ram.store32(thread + os::kThreadPriority, priority);
    ram.store16(thread + os::kThreadState, os::kStateReady);
    ram.store32(thread + os::kThreadQueue, queue);
    ram.store32(thread + os::kThreadLinkNext, 0u);
    ram.store32(thread + os::kThreadLinkPrev, 0u);
    ram.store32(queue, thread);
    ram.store32(queue + 4u, thread);
    ram.store32(os::kRunQueueBits, ram.load32(os::kRunQueueBits) | (1u << (31u - priority)));
}

void run_new_thread(void* user, std::uint32_t thread, PpcContext* context) {
    auto* harness = static_cast<Harness*>(user);
    check(thread == kWorkerThread, "new fiber runs the worker thread");
    ++harness->worker_runs;
    harness->events.push_back(1);
    // A callee-saved-heavy local value must survive the block below.
    volatile std::uint64_t marker = 0xC0FFEE0012345678ull;
    context->gpr[31] = 0xAAAA5555u;

    // Block (OSSleepThread): leave RUNNING, then SelectThread.
    harness->ram.store16(kWorkerThread + os::kThreadState, os::kStateWaiting);
    harness->scheduler->select_thread(false);

    // Resumed on the same fiber with this thread's registers restored.
    harness->events.push_back(3);
    check(marker == 0xC0FFEE0012345678ull, "worker stack survives the switch");
    check(context->gpr[31] == 0xAAAA5555u, "worker registers restored on resume");
    harness->ram.store16(kWorkerThread + os::kThreadState, os::kStateWaiting);
    harness->scheduler->select_thread(false);
    std::fputs("worker resumed after its final block\n", stderr);
    std::abort();
}

void idle(void* user, PpcContext*) {
    ++static_cast<Harness*>(user)->idle_calls;
    std::fputs("scheduler idled unexpectedly\n", stderr);
    std::abort();
}

void call_guest(void*, std::uint32_t, PpcContext*) {}

void exchange_chain(void* user, ChainState& parked, const ChainState& resumed) {
    auto* harness = static_cast<Harness*>(user);
    ++harness->exchanges;
    parked.words[0] = harness->exchanges;
    (void)resumed;
}

}  // namespace

int main() {
    std::byte* const base = map_guest_view();
    if (base == nullptr) {
        std::fputs("could not map a 4 GiB guest view\n", stderr);
        return 1;
    }
    Harness harness;
    g_harness = &harness;
    harness.ram.base = base;
    Scheduler::Host host;
    host.user = &harness;
    host.run_new_thread = &run_new_thread;
    host.idle = &idle;
    host.call_guest = &call_guest;
    host.exchange_chain = &exchange_chain;
    harness.scheduler = std::make_unique<Scheduler>(host, harness.ram, &harness.context);
    harness.scheduler->attach_boot_fiber();

    // Boot thread is current and running at priority 16.
    GuestRam ram = harness.ram;
    ram.store32(os::kCurrentThread, kBootThread);
    ram.store32(os::kCurrentContext, kBootThread);
    ram.store32(kBootThread + os::kThreadPriority, kBootPriority);
    ram.store16(kBootThread + os::kThreadState, os::kStateRunning);
    harness.context.gpr[31] = 0x0B007B00u;

    // A higher-priority worker becomes ready: SelectThread preempts boot.
    make_ready(ram, kWorkerThread, kWorkerPriority);
    harness.scheduler->select_thread(false);
    harness.events.push_back(2);
    check(harness.worker_runs == 1, "worker ran once");
    check(ram.load32(os::kCurrentThread) == kBootThread, "boot thread current again");
    check(harness.context.gpr[31] == 0x0B007B00u, "boot registers restored");
    check(harness.scheduler->fibers_created() == 1u, "one fiber created");
    check(galaxy::host::fiber_current() != nullptr, "still on a fiber");

    // Wake the worker again: it resumes on its existing fiber.
    make_ready(ram, kWorkerThread, kWorkerPriority);
    harness.scheduler->select_thread(false);
    harness.events.push_back(4);
    check(harness.worker_runs == 1, "worker resumed, not restarted");
    check(harness.scheduler->fibers_created() == 1u, "no second fiber");
    check(harness.scheduler->switches() == 4u, "four fiber switches");
    check(harness.context.gpr[31] == 0x0B007B00u, "boot registers restored again");
    check((harness.events == std::vector<int>{1, 2, 3, 4}), "event order");
    check(harness.idle_calls == 0, "never idled");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d scheduler check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("coop scheduler tests passed");
    // The worker fiber is parked inside its final block; skip teardown
    // ordering issues by exiting directly, as the runtime does.
    std::fflush(stdout);
    std::_Exit(0);
}
