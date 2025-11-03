#include "test_utils.h"

#include <exception>
#include <iostream>

int main() {
  std::cout << "\n=== Running vxcore tests ===\n" << std::endl;

  try {
  } catch (const std::exception &e) {
    std::cerr << "\nTest execution failed: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "\n=== Test Summary ===" << std::endl;
  std::cout << "Total tests: " << test_count << std::endl;
  std::cout << "Passed: " << passed_count << std::endl;
  std::cout << "Failed: " << (test_count - passed_count) << std::endl;

  if (test_count == passed_count) {
    std::cout << "\n✓ All tests passed!" << std::endl;
    return 0;
  } else {
    std::cout << "\n✗ Some tests failed!" << std::endl;
    return 1;
  }
}
