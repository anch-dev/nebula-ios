#include "galaxy/native_api.h"
#include "galaxy/crcp.h"

extern "C" GALAXY_MODULE_EXPORT std::uint32_t galaxy_fake_home_probe() { return 2u; }
