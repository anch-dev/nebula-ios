#include "functions.h"

#include <cstring>

namespace rmge01 {
void fake_function(galaxy::PpcContext* context, galaxy::GuestMemoryV1*, const galaxy::NativeServicesV1*) {
    context->pc += 4u;
}
}  // namespace rmge01

extern "C" GALAXY_MODULE_EXPORT std::uint32_t galaxy_fake_module_probe() {
    galaxy::PpcContext context{};
    rmge01::fake_function(&context, nullptr, nullptr);
    return context.pc;
}
