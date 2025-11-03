#include "test_utils.h"

#include "vxcore/vxcore.h"

#include <iostream>

int test_version() {
  std::cout << "  Running test_version..." << std::endl;
  VxCoreVersion version = vxcore_get_version();
  ASSERT_EQ(version.major, 0);
  ASSERT_EQ(version.minor, 1);
  ASSERT_EQ(version.patch, 0);
  std::cout << "  ✓ test_version passed" << std::endl;
  return 0;
}

int test_error_message() {
  std::cout << "  Running test_error_message..." << std::endl;
  const char *msg = vxcore_error_message(VXCORE_OK);
  ASSERT_NOT_NULL(msg);
  ASSERT_NE(std::string(msg), "");
  std::cout << "  ✓ test_error_message passed" << std::endl;
  return 0;
}

int test_context_create_destroy() {
  std::cout << "  Running test_context_create_destroy..." << std::endl;
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);
  vxcore_context_destroy(ctx);
  std::cout << "  ✓ test_context_create_destroy passed" << std::endl;
  return 0;
}

int main() {
  std::cout << "Running core tests..." << std::endl;
  int result = 0;
  
  result |= test_version();
  result |= test_error_message();
  result |= test_context_create_destroy();
  
  if (result == 0) {
    std::cout << "✓ All core tests passed" << std::endl;
  } else {
    std::cerr << "✗ Some core tests failed" << std::endl;
  }
  
  return result;
}
