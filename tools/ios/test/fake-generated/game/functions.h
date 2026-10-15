#pragma once

#include "galaxy/native_api.h"
#include "galaxy/ppc_float.h"

namespace rmge01 {
void fake_function(galaxy::PpcContext* context, galaxy::GuestMemoryV1* memory,
                   const galaxy::NativeServicesV1* services);
}  // namespace rmge01
