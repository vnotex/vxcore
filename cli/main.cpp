#include "vxcore/vxcore.h"
#include <iostream>

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;

  std::cout << "VxCore CLI v" << vxcore_get_version_string() << std::endl;

  VxCoreVersion version = vxcore_get_version();
  std::cout << "Version: " << version.major << "." << version.minor << "." << version.patch
            << std::endl;

  std::cout << "Error message test: " << vxcore_error_message(VXCORE_OK) << std::endl;

  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  if (err == VXCORE_OK) {
    std::cout << "Context created successfully" << std::endl;
    vxcore_context_destroy(ctx);
  } else {
    std::cout << "Failed to create context: " << vxcore_error_message(err) << std::endl;
  }

  return 0;
}
