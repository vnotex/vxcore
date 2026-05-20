#include "test_internals/mock_sync_backend.h"

#include <atomic>
#include <sstream>

#include "sync/sync_backend_registry.h"

namespace vxcore {

namespace {
// Task 6.2 (F4.4) capability override plumbing: registry-built mocks pick up
// the next override on construction. A simple atomic uint32 holds the bits,
// with kNoOverride = UINT32_MAX as the sentinel (real capability sets never
// reach that value because SyncCapability is a small bitfield).
constexpr uint32_t kNoCapabilityOverride = 0xFFFFFFFFu;
std::atomic<uint32_t> g_next_caps_override{kNoCapabilityOverride};
}  // namespace

// Task 5.2 (sync-backend-phase4 F4.1): self-registration of the mock backend
// into SyncBackendRegistry. The token's constructor runs at static-init time
// and calls Registry::Register("mock", factory). BackendRegistration swallows
// any exception so static-init can never crash the program.
//
// Task 6.2 (F4.4): factory signature now takes a credential provider, and the
// lambda consumes any pending capability override so AuthRequired tests can
// build a mock with that capability through the registry path.
namespace {
const BackendRegistration kMockRegistration{
    "mock",
    [](const SyncConfig & /*cfg*/, std::shared_ptr<ICredentialProvider> provider)
        -> std::unique_ptr<ISyncBackend> {
      auto backend = std::make_unique<MockSyncBackend>(std::move(provider));
      if (MockSyncBackend::HasNextCapabilitiesOverride()) {
        backend->SetCapabilities(MockSyncBackend::ConsumeNextCapabilitiesOverride());
      }
      return backend;
    }};
}  // namespace

MockSyncBackend::MockSyncBackend() = default;

MockSyncBackend::MockSyncBackend(std::shared_ptr<ICredentialProvider> creds_provider)
    : creds_provider_(std::move(creds_provider)) {}

MockSyncBackend::~MockSyncBackend() {
  // Wave 4.5 (F1.3 of sync-backend-phase4) removed ISyncBackend::Shutdown.
  // Mock teardown is now just clearing the initialized_ flag inline.
  initialized_ = false;
}

std::string MockSyncBackend::GetName() const { return name_; }

SyncCapabilities MockSyncBackend::GetCapabilities() const {
  return capabilities_;
}

bool MockSyncBackend::IsInitialized() const { return initialized_override_; }

VxCoreError MockSyncBackend::Initialize(const std::string &root_folder,
                                        const SyncConfig &config) {
  RecordCall("Initialize");
  root_folder_ = root_folder;
  config_ = config;
  initialized_ = true;
  return GetReturnCode("Initialize");
}

void MockSyncBackend::ReplaceCredsProvider(std::shared_ptr<ICredentialProvider> provider) {
  RecordCall("ReplaceCredsProvider");
  std::lock_guard<std::mutex> lk(creds_provider_mu_);
  creds_provider_ = std::move(provider);
  ++replace_creds_provider_call_count;
}

std::shared_ptr<ICredentialProvider> MockSyncBackend::GetCredsProviderSnapshot() const {
  std::lock_guard<std::mutex> lk(creds_provider_mu_);
  return creds_provider_;
}

VxCoreError MockSyncBackend::Sync(SyncProgressCallback callback,
                                  void *userdata) {
  RecordCall("Sync");
  (void)callback;
  (void)userdata;

  if (fake_conflicts_enabled_) {
    return VXCORE_ERR_SYNC_CONFLICT;
  }

  return GetReturnCode("Sync");
}

VxCoreError MockSyncBackend::GetStatus(std::vector<SyncFileInfo> &out_files) {
  RecordCall("GetStatus");
  out_files.clear();
  return GetReturnCode("GetStatus");
}

VxCoreError MockSyncBackend::GetConflicts(
    std::vector<SyncConflictInfo> &out_conflicts) {
  RecordCall("GetConflicts");
  out_conflicts.clear();
  return GetReturnCode("GetConflicts");
}

VxCoreError MockSyncBackend::ResolveConflict(
    const std::string &path, SyncConflictResolution resolution) {
  RecordCall("ResolveConflict");
  (void)path;
  (void)resolution;
  return GetReturnCode("ResolveConflict");
}

void MockSyncBackend::SetReturnCode(const std::string &method_name,
                                    VxCoreError code) {
  return_codes_[method_name] = code;
}

void MockSyncBackend::EnableFakeConflicts(bool enable) {
  fake_conflicts_enabled_ = enable;
}

void MockSyncBackend::SetName(std::string name) { name_ = std::move(name); }

void MockSyncBackend::SetCapabilities(SyncCapabilities caps) {
  capabilities_ = caps;
}

void MockSyncBackend::SetInitialized(bool initialized) {
  initialized_override_ = initialized;
}

size_t MockSyncBackend::GetCallCount() const {
  return call_records_.size();
}

const std::vector<std::string> &MockSyncBackend::GetCallRecords() const {
  return call_records_;
}

void MockSyncBackend::ClearCallRecords() {
  call_records_.clear();
}

VxCoreError MockSyncBackend::GetReturnCode(const std::string &method_name) {
  auto it = return_codes_.find(method_name);
  if (it != return_codes_.end()) {
    return it->second;
  }
  return VXCORE_OK;
}

void MockSyncBackend::RecordCall(const std::string &method_name) {
  call_records_.push_back(method_name);
}

std::shared_ptr<ICredentialProvider> MockSyncBackend::GetCredsProvider() const {
  std::lock_guard<std::mutex> lk(creds_provider_mu_);
  return creds_provider_;
}

void MockSyncBackend::SetNextCapabilitiesOverride(SyncCapabilities caps) {
  g_next_caps_override.store(static_cast<uint32_t>(caps), std::memory_order_release);
}

void MockSyncBackend::ClearNextCapabilitiesOverride() {
  g_next_caps_override.store(kNoCapabilityOverride, std::memory_order_release);
}

bool MockSyncBackend::HasNextCapabilitiesOverride() {
  return g_next_caps_override.load(std::memory_order_acquire) != kNoCapabilityOverride;
}

SyncCapabilities MockSyncBackend::ConsumeNextCapabilitiesOverride() {
  // exchange so a single registry call consumes the override; subsequent
  // calls go back to the default capability set.
  uint32_t prev = g_next_caps_override.exchange(kNoCapabilityOverride,
                                                std::memory_order_acq_rel);
  return static_cast<SyncCapabilities>(prev);
}

MockSyncBackend::ScopedCapabilityOverride::ScopedCapabilityOverride(SyncCapabilities caps) {
  MockSyncBackend::SetNextCapabilitiesOverride(caps);
}

MockSyncBackend::ScopedCapabilityOverride::~ScopedCapabilityOverride() {
  MockSyncBackend::ClearNextCapabilitiesOverride();
}

}  // namespace vxcore
