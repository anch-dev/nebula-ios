// SPDX-License-Identifier: GPL-3.0-only
// Every runtime header the generated game, Home Menu and DSP modules include.
// package-module-sdk.sh records which iOS SDK headers this needs, so the
// module SDK carries them; CI then compiles this against the SDK alone.
#include "galaxy/native_api.h"
#include "galaxy/crcp.h"
#include "galaxy/ppc_float.h"
#include "galaxy/ppc_paired_float.h"
#include "galaxy/dsp_context.h"
#include "galaxy/dsp_instruction_provenance.h"

#include <cstring>

extern "C" int galaxy_module_sdk_probe() { return 1; }
