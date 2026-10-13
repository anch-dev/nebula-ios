#include "galaxy/runtime_instance.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
    }
    return condition;
}

}  // namespace

int main() {
    bool passed = true;
    const std::wstring identity =
        L"Nebula.RuntimeInstanceTests." +
#if defined(_WIN32)
        std::to_wstring(GetCurrentProcessId());
#else
        std::to_wstring(getpid());
#endif

    galaxy::RuntimeInstanceLease first;
    bool already_running = false;
    passed &= expect(
        first.acquire(identity, already_running) &&
            !already_running && first.owns_lease(),
        "first product runtime acquires its owned lease");

    galaxy::RuntimeInstanceLease duplicate;
    passed &= expect(
        !duplicate.acquire(identity, already_running) && already_running &&
            !duplicate.owns_lease(),
        "duplicate product runtime fails closed without process mutation");

    galaxy::RuntimeInstanceLease unrelated;
    passed &= expect(
        unrelated.acquire(identity + L".Other", already_running) &&
            !already_running,
        "unrelated product identity is not inspected or blocked");

    galaxy::RuntimeInstanceLease invalid;
    passed &= expect(
        !invalid.acquire(L"invalid\\identity", already_running) &&
            !already_running,
        "invalid namespace identity fails closed");

    first = galaxy::RuntimeInstanceLease{};
    galaxy::RuntimeInstanceLease restarted;
    passed &= expect(
        restarted.acquire(identity, already_running) && !already_running,
        "normal runtime teardown releases the product lease for restart");

    if (!passed) {
        return 1;
    }
    std::cout << "Runtime instance lease tests passed\n";
    return 0;
}
