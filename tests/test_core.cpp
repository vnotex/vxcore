#include "test_utils.h"

#include "vxcore/vxcore.h"

TEST(test_version) {
  VxCoreVersion version = vxcore_get_version();
  ASSERT_EQ(version.major, 0);
  ASSERT_EQ(version.minor, 1);
  ASSERT_EQ(version.patch, 0);
}

TEST(test_error_message) {
  const char *msg = vxcore_error_message(VXCORE_OK);
  ASSERT_NOT_NULL(msg);
  ASSERT_NE(std::string(msg), "");
}

TEST(test_context_create_destroy) {
  VxCoreContextHandle ctx = nullptr;
  VxCoreError err = vxcore_context_create(nullptr, &ctx);
  ASSERT_EQ(err, VXCORE_OK);
  ASSERT_NOT_NULL(ctx);
  vxcore_context_destroy(ctx);
}
