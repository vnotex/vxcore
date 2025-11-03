#ifndef VXCORE_TEST_UTILS_H_
#define VXCORE_TEST_UTILS_H_

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

static int test_count = 0;
static int passed_count = 0;

#define TEST(name)                                                                                 \
  void name();                                                                                     \
  struct name##_runner {                                                                           \
    name##_runner() {                                                                              \
      std::cout << "Running " << #name << "..." << std::endl;                                     \
      test_count++;                                                                                \
      name();                                                                                      \
      passed_count++;                                                                              \
      std::cout << "  ✓ " << #name << " passed" << std::endl;                                     \
    }                                                                                              \
  } name##_instance;                                                                               \
  void name()

#define ASSERT(condition)                                                                          \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      std::cerr << "  ✗ Assertion failed: " << #condition << " at " << __FILE__ << ":"           \
                << __LINE__ << std::endl;                                                          \
      throw std::runtime_error("Test failed");                                                    \
    }                                                                                              \
  } while (0)

#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_NULL(ptr) ASSERT((ptr) == nullptr)
#define ASSERT_NOT_NULL(ptr) ASSERT((ptr) != nullptr)

static void cleanup_test_dir(const std::string &path) {
  if (std::filesystem::exists(path)) {
    std::filesystem::remove_all(path);
  }
}

#endif
