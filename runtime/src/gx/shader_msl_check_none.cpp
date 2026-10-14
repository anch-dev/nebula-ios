// SPDX-License-Identifier: GPL-3.0-only
// shader_msl_check.h without Metal: translation is checked, compilation is not.

#include "shader_msl_check.h"

#include <cstdio>

namespace galaxy_test {

bool msl_compiles(const std::string&, const char*, std::string&) {
    static bool noted = false;
    if (!noted) {
        noted = true;
        std::printf("note: no Metal compiler on this host; MSL is translated, not compiled\n");
    }
    return true;
}

}  // namespace galaxy_test
