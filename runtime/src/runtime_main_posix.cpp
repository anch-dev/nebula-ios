// SPDX-License-Identifier: GPL-3.0-only
// NebulaRuntime's main on Linux and macOS. The iOS app calls
// galaxy_runtime_main from its own game thread instead.

#include "galaxy/runtime_main.h"

int main(int argc, char** argv) {
    return galaxy_runtime_main(argc, argv);
}
