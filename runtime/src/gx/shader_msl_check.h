// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Test helper: compiles translated MSL with Metal where the host has it
// (macOS), so the shader tests check what the Metal renderer will load.
// Elsewhere it accepts the source unchecked.

#include <string>

namespace galaxy_test {

// False with `error` set when Metal rejects the source.
bool msl_compiles(const std::string& msl, const char* label, std::string& error);

}  // namespace galaxy_test
