#include "vxcore/vxcore.h"
#include <iostream>

int main() {
  std::cout << "Running vxcore tests..." << std::endl;

  VxCoreVersion version = vxcore_get_version();
  if (version.major != 0 || version.minor != 1 || version.patch != 0) {
    std::cerr << "Version test failed!" << std::endl;
    return 1;
  }

  const char *msg = vxcore_error_message(VXCORE_OK);
  if (msg == nullptr) {
    std::cerr << "Error message test failed!" << std::endl;
    return 1;
  }

  std::cout << "All tests passed!" << std::endl;
  return 0;
}
