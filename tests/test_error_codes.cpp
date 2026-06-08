#include <cstring>
#include <iostream>

#include "test_utils.h"
#include "vxcore/vxcore.h"

int test_error_message_readonly() {
  std::cout << "  Running test_error_message_readonly..." << std::endl;

  // Test that vxcore_error_message returns a non-empty string for
  // VXCORE_ERR_READ_ONLY containing "read-only" or "read only"
  const char *msg = vxcore_error_message(VXCORE_ERR_READ_ONLY);
  ASSERT_NOT_NULL(msg);

  std::string msg_str(msg);
  ASSERT(msg_str.length() > 0);

  // Convert to lowercase for case-insensitive search
  std::string msg_lower = msg_str;
  std::transform(msg_lower.begin(), msg_lower.end(), msg_lower.begin(), ::tolower);

  // Check for "read-only" or "read only" (hyphenated or spaced)
  bool contains_readonly =
      msg_lower.find("read-only") != std::string::npos || msg_lower.find("read only") != std::string::npos;
  ASSERT(contains_readonly);

  std::cout << "    ✓ Error message: \"" << msg << "\"" << std::endl;
  return 0;
}

int test_error_message_other_codes() {
  std::cout << "  Running test_error_message_other_codes..." << std::endl;

  // Regression: ensure other error codes still return messages
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_OK));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_INVALID_PARAM));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_NOT_FOUND));
  ASSERT_NOT_NULL(vxcore_error_message(VXCORE_ERR_UNKNOWN));

  std::cout << "    ✓ Other error codes still have messages" << std::endl;
  return 0;
}

int main() {
  vxcore_set_test_mode(1);

  std::cout << "Running error code tests..." << std::endl;
  RUN_TEST(test_error_message_readonly);
  RUN_TEST(test_error_message_other_codes);

  std::cout << "✓ All error code tests passed" << std::endl;
  return 0;
}
