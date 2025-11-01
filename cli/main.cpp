#include "vxcore/vxcore.h"
#include <iostream>

int main(int argc, char *argv[]) {
  std::cout << "VxCore CLI v" << vxcore_get_version_string() << std::endl;

  VxCoreVersion version = vxcore_get_version();
  std::cout << "Version: " << version.major << "." << version.minor << "." << version.patch
            << std::endl;

  std::cout << "Error message test: " << vxcore_error_message(VXCORE_OK) << std::endl;

  return 0;
}
