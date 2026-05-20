// Task 4.2 (sync-backend-phase4 F1.1): SyncBackendRegistry unit tests.
//
// These tests exercise the registry against local lambda factories only —
// they deliberately do NOT instantiate GitSyncBackend, so the binary can be
// built via the add_vxcore_test() helper and link only vxcore.dll (which
// re-exports the registry's public surface via VXCORE_API).
//
// NOTE: Because the live vxcore.dll loads GitSyncBackend's static-init
// BackendRegistration token, the "git" entry MAY already be present in the
// global registry by the time main() runs. Tests therefore use unique
// throwaway names ("__test_foo_42", etc.) so they never collide with the
// real "git" registration and never accidentally pass/fail because of it.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "sync/credential_provider.h"
#include "sync/sync_backend.h"
#include "sync/sync_backend_registry.h"
#include "sync/sync_types.h"
#include "test_utils.h"
#include "vxcore/vxcore.h"
#include "vxcore/vxcore_types.h"

namespace {

using vxcore::BackendRegistration;
using vxcore::ISyncBackend;
using vxcore::SyncBackendFactory;
using vxcore::SyncBackendRegistry;
using vxcore::SyncCapabilities;
using vxcore::SyncConfig;
using vxcore::SyncConflictInfo;
using vxcore::SyncConflictResolution;
using vxcore::SyncCredentials;
using vxcore::SyncFileInfo;
using vxcore::SyncProgressCallback;

// Minimal stub ISyncBackend used by the lambda factories below. It returns
// configurable values from GetName so tests can assert factory identity.
class StubBackend : public ISyncBackend {
 public:
  explicit StubBackend(std::string name) : name_(std::move(name)) {}

  std::string GetName() const override { return name_; }
  SyncCapabilities GetCapabilities() const override { return 0; }
  bool IsInitialized() const override { return false; }
  VxCoreError Initialize(const std::string &, const SyncConfig &) override {
    return VXCORE_OK;
  }
  VxCoreError Sync(SyncProgressCallback, void *) override { return VXCORE_OK; }
  VxCoreError GetStatus(std::vector<SyncFileInfo> &) override {
    return VXCORE_OK;
  }
  VxCoreError GetConflicts(std::vector<SyncConflictInfo> &) override {
    return VXCORE_OK;
  }
  VxCoreError ResolveConflict(const std::string &,
                              SyncConflictResolution) override {
    return VXCORE_OK;
  }

 private:
  std::string name_;
};

SyncBackendFactory make_stub_factory(const std::string &name) {
  return [name](const SyncConfig &,
                std::shared_ptr<vxcore::ICredentialProvider>) -> std::unique_ptr<ISyncBackend> {
    return std::make_unique<StubBackend>(name);
  };
}

int register_returns_true_for_new_name() {
  std::cout << "  Running register_returns_true_for_new_name..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  ASSERT_TRUE(r.Register("__test_rt_new", make_stub_factory("__test_rt_new")));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int register_returns_false_for_empty_name() {
  std::cout << "  Running register_returns_false_for_empty_name..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  ASSERT_FALSE(r.Register("", make_stub_factory("anything")));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int register_returns_false_for_null_factory() {
  std::cout << "  Running register_returns_false_for_null_factory..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  SyncBackendFactory null_factory;  // default-constructed -> operator bool == false
  ASSERT_FALSE(r.Register("__test_null_factory", null_factory));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int register_returns_false_for_duplicate_name() {
  std::cout << "  Running register_returns_false_for_duplicate_name..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  const std::string n = "__test_dup_first_wins";
  ASSERT_TRUE(r.Register(n, make_stub_factory("first")));
  // Second registration must fail — first-wins semantics.
  ASSERT_FALSE(r.Register(n, make_stub_factory("second")));
  // And the still-resolvable factory must be "first" (sentinel via GetName).
  auto inst = r.Create(n, SyncConfig{}, nullptr);
  ASSERT_NOT_NULL(inst.get());
  ASSERT_EQ(inst->GetName(), std::string("first"));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int create_returns_factory_result_for_known_name() {
  std::cout << "  Running create_returns_factory_result_for_known_name..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  const std::string n = "__test_create_known";
  ASSERT_TRUE(r.Register(n, make_stub_factory("hello")));
  auto inst = r.Create(n, SyncConfig{}, nullptr);
  ASSERT_NOT_NULL(inst.get());
  ASSERT_EQ(inst->GetName(), std::string("hello"));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int create_returns_nullptr_for_unknown_name() {
  std::cout << "  Running create_returns_nullptr_for_unknown_name..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  auto inst = r.Create("__definitely_not_a_backend_name_xyz", SyncConfig{}, nullptr);
  ASSERT_NULL(inst.get());
  std::cout << "  PASS" << std::endl;
  return 0;
}

int names_returns_sorted_list() {
  std::cout << "  Running names_returns_sorted_list..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  // Register names in non-sorted order — registry must still return sorted.
  r.Register("__test_sorted_zeta", make_stub_factory("z"));
  r.Register("__test_sorted_alpha", make_stub_factory("a"));
  r.Register("__test_sorted_mike", make_stub_factory("m"));
  auto names = r.Names();
  // Confirm globally sorted (not just our subset).
  for (size_t i = 1; i < names.size(); ++i) {
    ASSERT_TRUE(names[i - 1] <= names[i]);
  }
  // Confirm our subset is present.
  auto find = [&](const std::string &needle) {
    return std::find(names.begin(), names.end(), needle) != names.end();
  };
  ASSERT_TRUE(find("__test_sorted_alpha"));
  ASSERT_TRUE(find("__test_sorted_mike"));
  ASSERT_TRUE(find("__test_sorted_zeta"));
  std::cout << "  PASS" << std::endl;
  return 0;
}

int concurrent_register_and_create_is_safe() {
  std::cout << "  Running concurrent_register_and_create_is_safe..." << std::endl;
  auto &r = SyncBackendRegistry::Instance();
  constexpr int kThreads = 4;
  constexpr int kIterations = 100;
  std::atomic<int> crashes{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &r, &crashes]() {
      try {
        for (int i = 0; i < 100; ++i) {
          const std::string n =
              "__test_concurrent_" + std::to_string(t) + "_" + std::to_string(i);
          r.Register(n, make_stub_factory(n));
          (void)r.Create(n, SyncConfig{}, nullptr);
        }
      } catch (...) {
        crashes.fetch_add(1);
      }
    });
  }
  for (auto &th : threads) th.join();
  ASSERT_EQ(crashes.load(), 0);
  // Final sanity: a sampling of expected names is present in Names().
  auto names = r.Names();
  for (int t = 0; t < kThreads; ++t) {
    const std::string n = "__test_concurrent_" + std::to_string(t) + "_0";
    ASSERT_TRUE(std::find(names.begin(), names.end(), n) != names.end());
  }
  std::cout << "  PASS" << std::endl;
  return 0;
}

int backend_registration_token_does_not_throw() {
  std::cout << "  Running backend_registration_token_does_not_throw..." << std::endl;
  // Construct tokens with adversarial inputs — empty name, null factory.
  // Neither must throw (static-init exception safety).
  try {
    BackendRegistration empty_name("", make_stub_factory("x"));
    BackendRegistration null_factory("__test_brt_null", SyncBackendFactory{});
    (void)empty_name;
    (void)null_factory;
  } catch (...) {
    std::cerr << "BackendRegistration ctor threw — must not happen" << std::endl;
    return 1;
  }
  std::cout << "  PASS" << std::endl;
  return 0;
}

}  // namespace

int main() {
  vxcore_set_test_mode(1);
  std::cout << "Running SyncBackendRegistry tests..." << std::endl;
  RUN_TEST(register_returns_true_for_new_name);
  RUN_TEST(register_returns_false_for_empty_name);
  RUN_TEST(register_returns_false_for_null_factory);
  RUN_TEST(register_returns_false_for_duplicate_name);
  RUN_TEST(create_returns_factory_result_for_known_name);
  RUN_TEST(create_returns_nullptr_for_unknown_name);
  RUN_TEST(names_returns_sorted_list);
  RUN_TEST(concurrent_register_and_create_is_safe);
  RUN_TEST(backend_registration_token_does_not_throw);
  std::cout << "All SyncBackendRegistry tests passed." << std::endl;
  return 0;
}
