#ifndef VXCORE_CORE_UTILS_H_
#define VXCORE_CORE_UTILS_H_

#include <cstdint>
#include <string>
#include <type_traits>

namespace vxcore {

std::string GenerateUUID();

int64_t GetCurrentTimestampMillis();

template <typename EnumT>
bool HasFlag(EnumT flags, EnumT flag) {
  static_assert(std::is_enum<EnumT>::value, "HasFlag requires enum type");
  using UnderlyingT = typename std::underlying_type<EnumT>::type;
  return (static_cast<UnderlyingT>(flags) & static_cast<UnderlyingT>(flag)) ==
         static_cast<UnderlyingT>(flag);
}

}  // namespace vxcore

#endif
