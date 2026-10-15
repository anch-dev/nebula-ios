// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// <Windows.h> for the renderer sources on Apple/Linux hosts: the runtime's
// Win32 shims (files, events, threads, modules, the virtual window) plus the
// COM vocabulary the D3D12/DXGI headers need. See docs/APPLE_PORT.md.

#include "galaxy/platform_compat.h"
#include "galaxy/win32_portable.h"
#include "galaxy/win32_fs.h"
#include "galaxy/win32_host_posix.h"

#include <basetsd.h>
#include <unknwn.h>

#include "galaxy/win32_gui_posix.h"
