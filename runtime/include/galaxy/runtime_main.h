// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// The native runtime's entry point on non-Windows hosts. Windows enters
// through wmain in native_runtime.cpp; elsewhere an executable's main
// (runtime/src/runtime_main_posix.cpp) or an app shell's game thread calls
// this with UTF-8 arguments, the same ones NebulaRuntime.exe takes:
//   [game-pak-dir] [native-module] [--max-calls N] ...
// Configuration otherwise comes from GALAXY_* environment variables, which
// the caller sets beforehand. Returns the process exit code; most runs end
// the process from inside instead (as ExitProcess does on Windows).

#if !defined(_WIN32)
extern "C" int galaxy_runtime_main(int argc, char** argv);
#endif
