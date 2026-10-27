#include "UC2Version.h"

#ifndef UC2_CAM_VERSION
#error "UC2_CAM_VERSION is not defined - core/CMakeLists.txt passes it to this file only"
#endif

namespace hik {

const char* version()
{
    return UC2_CAM_VERSION;
}

} // namespace hik
