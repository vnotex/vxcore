#ifndef VXCORE_CORE_UTILS_H_
#define VXCORE_CORE_UTILS_H_

#include <cstdint>
#include <string>

namespace vxcore {

std::string GenerateUUID();

int64_t GetCurrentTimestampMillis();

}  // namespace vxcore

#endif
