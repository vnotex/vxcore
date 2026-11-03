#include <atomic>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/context.h"
#include "test_utils.h"
#include "utils/utils.h"
#include "vxcore/vxcore.h"

namespace {
struct Context {
  VxCoreContextHandle value = nullptr;
  ~Context() { vxcore_context_destroy(value); }
};

int test_snapshot_survives_other_thread_writes_and_getters() {
  Context context;
  ASSERT_EQ(vxcore_context_create(nullptr, &context.value), VXCORE_OK);
  auto *internal = reinterpret_cast<vxcore::VxCoreContext *>(context.value);
  const std::string original(32768, 'A');
  internal->last_error = original;
  const char *snapshot = nullptr;
  ASSERT_EQ(vxcore_context_get_last_error(context.value, &snapshot), VXCORE_OK);
  ASSERT_EQ(std::string(snapshot), original);
  std::atomic<bool> valid{true};
  std::thread writer([&] {
    for (size_t i = 1; i < 2048; ++i) {
      internal->last_error = std::string(i * 3, 'B');
      const char *other = nullptr;
      if (vxcore_context_get_last_error(context.value, &other) != VXCORE_OK ||
          std::string(other) != std::string(i * 3, 'B')) valid = false;
      internal->last_error.clear();
    }
  });
  writer.join();
  ASSERT_TRUE(valid.load());
  ASSERT_EQ(std::string(snapshot), original);
  ASSERT_EQ(vxcore_context_get_last_error(context.value, &snapshot), VXCORE_OK);
  ASSERT_EQ(std::string(snapshot), "No error");
  return 0;
}

int test_shared_error_assign_clear_copy_is_atomic() {
  Context context;
  ASSERT_EQ(vxcore_context_create(nullptr, &context.value), VXCORE_OK);
  auto *internal = reinterpret_cast<vxcore::VxCoreContext *>(context.value);
  std::atomic<bool> start{false}, valid{true};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 8; ++worker) {
    threads.emplace_back([&, worker] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (int i = 0; i < 2000; ++i) {
        if (worker < 4) {
          internal->last_error = std::string(static_cast<size_t>(1 + i % 1024), 'a' + worker);
          if (i % 7 == 0) internal->last_error.clear();
        } else {
          const char *snapshot = nullptr;
          if (vxcore_context_get_last_error(context.value, &snapshot) != VXCORE_OK || !snapshot) {
            valid = false;
            continue;
          }
          const std::string value(snapshot);
          if (value == "No error") continue;
          if (value.empty() || value.front() < 'a' || value.front() > 'd' ||
              value.find_first_not_of(value.front()) != std::string::npos) valid = false;
        }
      }
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) thread.join();
  ASSERT_TRUE(valid.load());
  return 0;
}

int test_concurrent_uuid_generation_preserves_format_and_uniqueness() {
  constexpr size_t count = 2048;
  std::vector<std::vector<std::string>> values(8);
  std::vector<std::thread> threads;
  std::atomic<bool> start{false};
  for (size_t worker = 0; worker < values.size(); ++worker) {
    threads.emplace_back([&, worker] {
      values[worker].reserve(count);
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      for (size_t i = 0; i < count; ++i) values[worker].push_back(vxcore::GenerateUUID());
    });
  }
  start.store(true, std::memory_order_release);
  for (auto &thread : threads) thread.join();
  std::set<std::string> unique;
  for (const auto &batch : values) {
    for (const auto &id : batch) {
      ASSERT_EQ(id.size(), size_t{36});
      ASSERT_EQ(id[14], '4');
      ASSERT_TRUE(std::string("89ab").find(id[19]) != std::string::npos);
      for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) ASSERT_EQ(id[i], '-');
        else ASSERT_TRUE((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'));
      }
      ASSERT_TRUE(unique.insert(id).second);
    }
  }
  ASSERT_EQ(unique.size(), count * values.size());
  return 0;
}
}  // namespace

int main() {
  vxcore_set_test_mode(1);
  RUN_TEST(test_snapshot_survives_other_thread_writes_and_getters);
  RUN_TEST(test_shared_error_assign_clear_copy_is_atomic);
  RUN_TEST(test_concurrent_uuid_generation_preserves_format_and_uniqueness);
  return 0;
}
