// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Where the game modules (RMGE01_game, RMGE01_home_button, RMGE01_dsp) and
// their sidecar files live.
//
// Windows: <dir>/<stem>.dll, next to NebulaRuntime.exe.
// Apple:   iOS only runs signed code from inside the app bundle, and the
//          sideloading tools re-sign nested frameworks, so the app carries
//          each module as <bundle>/Frameworks/<stem>.framework/<stem>.
//          <dir>/<stem>.dylib is used when there is no framework (macOS
//          command-line runs).
// Linux:   <dir>/<stem>.so.

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace galaxy {

[[nodiscard]] inline std::filesystem::path native_module_path(
    const std::filesystem::path& dir, std::string_view stem) {
    const std::string name(stem);
#if defined(_WIN32)
    return dir / (name + ".dll");
#elif defined(__APPLE__)
    const std::filesystem::path framework =
        dir / "Frameworks" / (name + ".framework") / name;
    std::error_code error;
    if (std::filesystem::is_regular_file(framework, error)) {
        return framework;
    }
    return dir / (name + ".dylib");
#else
    return dir / (name + ".so");
#endif
}

// The directory that holds a module's sidecars (the other modules and the
// boot image): the module's own directory, or for a module inside
// Frameworks/<stem>.framework/, the app bundle.
[[nodiscard]] inline std::filesystem::path native_module_sidecar_dir(
    const std::filesystem::path& module) {
    std::filesystem::path dir = module.parent_path();
#if defined(__APPLE__)
    if (dir.extension() == ".framework" &&
        dir.parent_path().filename() == "Frameworks") {
        return dir.parent_path().parent_path();
    }
#endif
    return dir;
}

}  // namespace galaxy
