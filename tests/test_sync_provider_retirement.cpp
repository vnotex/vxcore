#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

#include "sync/sync_encryption_guard.h"
#include "sync/sync_provider_retirement.h"
#include "sync/webdav/webdav_state.h"
#include "test_utils.h"
#include "utils/file_utils.h"

namespace {
using namespace vxcore;
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr const char *kNotebook = "11111111-1111-4111-8111-111111111111";
constexpr const char *kOperation = "22222222-2222-4222-8222-222222222222";
constexpr const char *kOtherOperation = "33333333-3333-4333-8333-333333333333";
constexpr const char *kSnapshot = "44444444-4444-4444-8444-444444444444";
constexpr const char *kScratch = ".vnote-webdav-tmp-55555555-5555-4555-8555-555555555555";
constexpr const char *kEndpoint = "https://example.invalid/notebook/";

void Put(const fs::path &path, const std::string &bytes) {
  fs::create_directories(path.parent_path());
  if (WriteFileAtomic(path, bytes) != VXCORE_OK) throw std::runtime_error("test write failed");
}
std::string Bytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("test read failed");
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
Json Object(const fs::path &path) { return Json::parse(Bytes(path)); }
Json Baseline(const std::string &user = webdav::HashBytes("published-account")) {
  return {{"version", 1}, {"notebookId", kNotebook}, {"remoteUrl", kEndpoint},
          {"usernameHash", user}, {"entries", Json::object()}, {"conflicts", Json::object()}};
}
std::string SnapshotName() { return std::string("snapshots/") + kOperation + '/' + kSnapshot; }
Json Pending(const std::string &path, const std::string &action,
             const std::string &user = webdav::HashBytes("published-account")) {
  auto pending = Baseline(user);
  pending.erase("entries");
  pending.erase("conflicts");
  pending["operationId"] = kOperation;
  pending["operations"] = Json::array({
      {{"path", path}, {"kind", "file"}, {"action", action},
       {"expectedLocalKind", "absent"}, {"expectedRemoteKind", "absent"},
       {"expectedLocalSha256", nullptr}, {"expectedRemoteEtag", nullptr},
       {"oldSha256", nullptr}, {"newSha256", webdav::HashBytes("snapshot payload")},
       {"sourceSnapshot", SnapshotName()}, {"previousSnapshot", nullptr},
       {"scratchUrl", nullptr}, {"stage", "prepared"}}});
  return pending;
}
Json Command(const std::string &kind, const std::string &operation = kOperation) {
  Json result = {{"version", 1}, {"kind", kind}};
  if (kind != "inspect") result["operationId"] = operation;
  return result;
}
struct Fixture {
  fs::path root = fs::canonical(fs::temp_directory_path()) / PathFromUtf8("vxcore-retirement-" + webdav::NewId());
  fs::path metadata = root / "vx_notebook";
  fs::path provider = metadata / "vx_sync/webdav";
  fs::path archive = provider / "retired" / kOperation;
  SyncProviderRuntimeFacts runtime;
  Fixture() {
    Put(metadata / "config.json", Json({{"id", kNotebook}, {"name", "storage regression"}}).dump());
  }
  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);  // Only this freshly created, test-owned root.
  }
  void State() { Put(provider / "state.json", Baseline().dump(2) + "\n"); }
  void Snapshot() { Put(provider / PathFromUtf8(SnapshotName()), "snapshot payload"); }
  VxCoreError Inspect(Json &facts) { return InspectSyncStorage(PathToUtf8(root), kNotebook, facts); }
  VxCoreError Transaction(const std::string &kind, Json &facts,
                          const std::string &operation = kOperation) {
    return SyncProviderTransaction(PathToUtf8(root), kNotebook, Command(kind, operation), runtime, facts);
  }
  void Phase(const std::string &phase) {
    auto journal = Object(provider / "retirement.json");
    journal["phase"] = phase;
    Put(provider / "retirement.json", journal.dump());
  }
};

int LongPrivateCandidateAndArchiveKeepLogicalOwnership() {
  Fixture owned;
  const auto base_units = owned.root.native().size();
  const auto padding = base_units < 119 ? 119 - base_units : size_t{1};
  const auto root = owned.root / std::string(padding, 'p');
  struct Cleanup {
    fs::path root;
    static void Remove(const fs::path &path) {
      if (!private_io::Exists(path)) return;
      if (private_io::IsDirectory(path))
        private_io::VisitDirectory(path, [](const fs::path &child) { Remove(child); });
      private_io::Remove(path);
    }
    ~Cleanup() { try { Remove(root); } catch (...) {} }
  } cleanup{root};
  const auto metadata = root / "vx_notebook";
  const auto provider = metadata / "vx_sync/webdav";
  Put(metadata / "config.json", Json({{"id", kNotebook}, {"name", "long private regression"}}).dump());
  const auto candidate_snapshot = provider / "credential-validation" / kOperation /
                                  "snapshots" / kOperation / kSnapshot;
#ifdef _WIN32
  // native().size() is wchar units here, not a UTF-8 byte approximation.
  ASSERT_TRUE(candidate_snapshot.native().size() >= 260);
#endif
  std::cout << "private snapshot native units: " << candidate_snapshot.native().size() << '\n';
  const char *phase = "open candidate";
  VxCoreError first_error = VXCORE_OK;
  try {
    webdav::State candidate;
    candidate.OpenCandidate(root, kNotebook, kEndpoint, webdav::HashBytes("candidate-account"),
                            kOperation, true);
    phase = "reserve/write candidate snapshot";
    const auto snapshot = candidate.SnapshotBytes("long candidate bytes");
    ASSERT_TRUE(snapshot.find('\\') == std::string::npos && snapshot.front() != '/');
    phase = "read/hash candidate snapshot";
    candidate.VerifySnapshot(snapshot, webdav::HashBytes("long candidate bytes"));
    ASSERT_EQ(webdav::ReadBytes(candidate.Resolve(snapshot), 16 * 1024 * 1024, FileAccess::kPrivate), "long candidate bytes");
    Json facts;
    phase = "inspect candidate";
    ASSERT_EQ(InspectSyncStorage(PathToUtf8(root), kNotebook, facts), VXCORE_OK);
    ASSERT_TRUE(facts.at("credentialValidation").get<bool>());
    ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(metadata)), VXCORE_OK);
    ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(metadata)), VXCORE_ERR_SYNC_CONFLICT);
    phase = "remove owned candidate";
    candidate.RemoveCandidate();

    webdav::State state;
    state.Open(root, kNotebook, kEndpoint, webdav::HashBytes("published-account"));
    state.Save();
    const auto retained_snapshot = state.SnapshotBytes("long archive bytes");
    const auto baseline = webdav::ReadBytes(state.Resolve("state.json"), 16 * 1024 * 1024, FileAccess::kPrivate);
    SyncProviderRuntimeFacts runtime;
    phase = "archive validated private tree";
    ASSERT_EQ(SyncProviderTransaction(PathToUtf8(root), kNotebook, Command("begin"), runtime, facts), VXCORE_OK);
    const auto archived = provider / "retired" / kOperation / PathFromUtf8(retained_snapshot);
#ifdef _WIN32
    ASSERT_TRUE(archived.native().size() >= 260);
#endif
    phase = "read/hash archived snapshot";
    ASSERT_EQ(webdav::ReadBytes(archived, 16 * 1024 * 1024, FileAccess::kPrivate), "long archive bytes");
    ASSERT_EQ(webdav::HashFile(archived, nullptr, FileAccess::kPrivate), webdav::HashBytes("long archive bytes"));
    ASSERT_EQ(InspectSyncStorage(PathToUtf8(root), kNotebook, facts), VXCORE_OK);
    const auto recovery = facts.at("retirementOperationId").get<std::string>();
    phase = "restore original private tree";
    ASSERT_EQ(SyncProviderTransaction(PathToUtf8(root), kNotebook, Command("recover", recovery), runtime, facts), VXCORE_OK);
    ASSERT_EQ(webdav::ReadBytes(state.Resolve("state.json"), 16 * 1024 * 1024, FileAccess::kPrivate), baseline);
    ASSERT_EQ(webdav::ReadBytes(state.Resolve(retained_snapshot), 16 * 1024 * 1024, FileAccess::kPrivate), "long archive bytes");
    ASSERT_TRUE(facts.at("retirementOperationId").is_null());
  } catch (const webdav::Failure &failure) {
    first_error = failure.error;
  } catch (const fs::filesystem_error &) {
    first_error = VXCORE_ERR_IO;
  }
  if (first_error != VXCORE_OK)
    std::cerr << "first private I/O failure: " << phase << ", code=" << first_error << '\n';
  ASSERT_EQ(first_error, VXCORE_OK);
  return 0;
}

int ReadOnlyAbsentAndUnsupportedRouting() {
  Fixture f;
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_EQ(facts.at("notebookId"), kNotebook);
  ASSERT_TRUE(facts.at("provider").is_null());
  ASSERT_FALSE(facts.at("hasState").get<bool>());
  ASSERT_TRUE(facts.at("retirementOperationId").is_null());
  ASSERT_FALSE(fs::exists(f.metadata / "vx_sync"));
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_FALSE(facts.at("changed").get<bool>());
  ASSERT_FALSE(fs::exists(f.metadata / "vx_sync"));
  auto config = Object(f.metadata / "config.json");
  config["syncBackend"] = "unsupported-provider";
  config["syncEnabled"] = true;
  config["syncRemoteUrl"] = "https://example.invalid/unrelated/";
  Put(f.metadata / "config.json", config.dump());
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_NE(InspectSyncStorage(PathToUtf8(f.root), kOtherOperation, facts), VXCORE_OK);
  Put(f.metadata / "vx_sync/jianguoyun/owned.bin", "do not migrate");
  ASSERT_EQ(f.Inspect(facts), VXCORE_ERR_UNSUPPORTED);
  ASSERT_EQ(Bytes(f.metadata / "vx_sync/jianguoyun/owned.bin"), "do not migrate");
  return 0;
}

int CleanBaselineIsOnlyADiagnostic() {
  Fixture f;
  f.State();
  auto state = Object(f.provider / "state.json");
  state["entries"]["absent-from-source.md"] =
      {{"kind", "file"}, {"sha256", webdav::HashBytes("not in this tree")}, {"etag", "\"version\""}};
  const auto raw = state.dump(2) + "\n";
  Put(f.provider / "state.json", raw);
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("cleanCloneBaseline").get<bool>());
  ASSERT_FALSE(fs::exists(f.root / "absent-from-source.md"));
  // The diagnostic deliberately does not mint a receipt or assert source-tree
  // equivalence. The ordinary importer must still reject this cursor.
  ASSERT_EQ(facts.at("baselineSha256"), webdav::HashBytes(raw));
  ASSERT_EQ(Bytes(f.provider / "state.json"), raw);
  ASSERT_EQ(f.Transaction("inspect", facts), VXCORE_OK);
  ASSERT_TRUE(facts.contains("runtimeRegistered") && facts.contains("applyInProgress") &&
              facts.contains("persistedEnabled"));
  Put(f.provider / "state.json", state.dump());
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_NE(facts.at("baselineSha256"), webdav::HashBytes(raw));
  f.Snapshot();
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_FALSE(facts.at("cleanCloneBaseline").get<bool>());
  return 0;
}

int InspectionRecoversOriginalIdentityWithoutTargetingForeignArchives() {
  Fixture f;
  f.State();
  const auto original = Bytes(f.provider / "state.json");
  const auto foreign = f.provider / "retired" / kOtherOperation / "state.json";
  const auto foreign_bytes = Baseline(webdav::HashBytes("historical-account")).dump();
  Put(foreign, foreign_bytes);
  FileIdentity foreign_identity;
  ASSERT_EQ(GetFileIdentity(foreign, foreign_identity), VXCORE_OK);
  Json facts;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  const auto journal_path = f.provider / "retirement.json";
  const auto journal_bytes = Bytes(journal_path);

  // A restarted caller needs only validated core facts, not a vault record or
  // its own parser of provider files, to recover the retained transaction.
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  const auto retained = facts.at("retirementOperationId").get<std::string>();
  ASSERT_EQ(retained, kOperation);
  ASSERT_EQ(f.Transaction("inspect", facts), VXCORE_OK);
  ASSERT_EQ(facts.at("retirementOperationId"), retained);
  ASSERT_EQ(f.Transaction("recover", facts, kOtherOperation), VXCORE_ERR_INVALID_STATE);
  ASSERT_EQ(Bytes(journal_path), journal_bytes);
  ASSERT_EQ(Bytes(f.archive / "state.json"), original);
  ASSERT_EQ(Bytes(foreign), foreign_bytes);

  // Even a canonical ID read from a journal is not exposed until the selected
  // archive's immutable inventory and binding validate against that journal.
  auto replaced = Json::parse(journal_bytes);
  replaced["operationId"] = kOtherOperation;
  Put(journal_path, replaced.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.is_null());
  ASSERT_EQ(Bytes(foreign), foreign_bytes);
  Put(journal_path, journal_bytes);

  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  const auto recovered_id = facts.at("retirementOperationId").get<std::string>();
  ASSERT_EQ(f.Transaction("recover", facts, recovered_id), VXCORE_OK);
  ASSERT_TRUE(facts.at("retirementOperationId").is_null());
  ASSERT_EQ(Bytes(f.provider / "state.json"), original);
  ASSERT_EQ(Bytes(foreign), foreign_bytes);
  FileIdentity after;
  ASSERT_EQ(GetFileIdentity(foreign, after), VXCORE_OK);
  ASSERT_TRUE(after.SameObject(foreign_identity));
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("retirementOperationId").is_null());
  ASSERT_FALSE(fs::exists(journal_path));
  return 0;
}

int ArchiveRestoreAndCommitPreserveEveryByte() {
  Fixture f;
  f.State();
  f.Snapshot();
  const auto original = Bytes(f.provider / "state.json");
  FileIdentity state_identity, snapshot_identity;
  ASSERT_EQ(GetFileIdentity(f.provider / "state.json", state_identity), VXCORE_OK);
  ASSERT_EQ(GetFileIdentity(f.provider / PathFromUtf8(SnapshotName()), snapshot_identity), VXCORE_OK);
  Json facts;
  f.runtime.runtime_registered = true;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_IN_PROGRESS);
  f.runtime.runtime_registered = false;
  f.runtime.apply_in_progress = true;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_IN_PROGRESS);
  f.runtime.apply_in_progress = false;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_EQ(facts.at("phase"), "archived");
  ASSERT_FALSE(facts.at("hasState").get<bool>());
  ASSERT_TRUE(facts.at("baselineSha256").is_null());
  ASSERT_TRUE(facts.at("remoteUrl").is_null());
  ASSERT_TRUE(facts.at("usernameHash").is_null());
  ASSERT_TRUE(facts.at("retirement").get<bool>());
  ASSERT_FALSE(fs::exists(f.provider / "state.json"));
  ASSERT_EQ(Bytes(f.archive / "state.json"), original);
  ASSERT_EQ(Bytes(f.archive / PathFromUtf8(SnapshotName())), "snapshot payload");
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_OK);
  ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(f.Transaction("recover", facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("phase").is_null());
  ASSERT_EQ(Bytes(f.provider / "state.json"), original);
  FileIdentity actual;
  ASSERT_EQ(GetFileIdentity(f.provider / "state.json", actual), VXCORE_OK);
  ASSERT_TRUE(actual.SameObject(state_identity));
  ASSERT_EQ(GetFileIdentity(f.provider / PathFromUtf8(SnapshotName()), actual), VXCORE_OK);
  ASSERT_TRUE(actual.SameObject(snapshot_identity));
  ASSERT_TRUE(fs::is_directory(f.archive));
  // Empty retired directories are retained too; an operation ID is never reused.
  ASSERT_NE(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_EQ(f.Transaction("begin", facts, kOtherOperation), VXCORE_OK);
  const auto archive2 = f.provider / "retired" / kOtherOperation;
  ASSERT_NE(f.Transaction("commit", facts, kOtherOperation), VXCORE_OK);
  ASSERT_TRUE(fs::exists(f.provider / "retirement.json"));
  f.runtime.persisted_routing_cleared = true;
  f.runtime.persisted_enabled = true;
  ASSERT_NE(f.Transaction("commit", facts, kOtherOperation), VXCORE_OK);
  f.runtime.persisted_enabled = false;
  ASSERT_EQ(f.Transaction("commit", facts, kOtherOperation), VXCORE_OK);
  ASSERT_TRUE(facts.at("phase").is_null());
  ASSERT_FALSE(facts.at("hasState").get<bool>());
  ASSERT_TRUE(facts.at("retired").get<bool>());
  ASSERT_EQ(Bytes(archive2 / "state.json"), original);
  ASSERT_EQ(Bytes(archive2 / PathFromUtf8(SnapshotName())), "snapshot payload");
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  ASSERT_EQ(f.Transaction("commit", facts, kOtherOperation), VXCORE_OK);
  ASSERT_FALSE(facts.at("changed").get<bool>());
  return 0;
}

int PreparedPartialArchiveRestoresAsAWhole() {
  // Reconstruct each crash boundary using a real begin's immutable inventories:
  // before archive creation, between moves, and after the last move.
  for (int moved = 0; moved != 3; ++moved) {
    Fixture f;
    f.State();
    f.Snapshot();
    Json facts;
    ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
    f.Phase("prepared");
    if (moved < 2) fs::rename(f.archive / "state.json", f.provider / "state.json");
    if (moved == 0) {
      fs::rename(f.archive / "snapshots", f.provider / "snapshots");
      ASSERT_TRUE(fs::remove(f.archive));
    }
    ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
    ASSERT_TRUE(facts.at("retirement").get<bool>());
    ASSERT_EQ(f.Transaction("recover", facts), VXCORE_OK);
    ASSERT_EQ(Bytes(f.provider / PathFromUtf8(SnapshotName())), "snapshot payload");
    ASSERT_TRUE(fs::exists(f.provider / "state.json"));
    ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  }
  return 0;
}

int AllEntriesValidateBeforeTheFirstRestoreMove() {
  for (int damage = 0; damage != 4; ++damage) {
    Fixture f;
    f.State();
    f.Snapshot();
    Json facts;
    ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
    f.Phase("prepared");
    const auto original_state = Bytes(f.archive / "state.json");
    if (damage == 0) fs::remove(f.archive / PathFromUtf8(SnapshotName()));
    if (damage == 1) Put(f.provider / "state.json", original_state);  // Both source and archive.
    if (damage == 2) Put(f.archive / "unknown.bin", "foreign archive child");
    if (damage == 3) {
      const auto bytes = Bytes(f.archive / PathFromUtf8(SnapshotName()));
      Put(f.archive / PathFromUtf8(SnapshotName()), bytes);  // Same bytes, replaced object.
    }
    ASSERT_NE(f.Inspect(facts), VXCORE_OK);
    ASSERT_NE(f.Transaction("restore", facts), VXCORE_OK);
    ASSERT_EQ(Bytes(f.archive / "state.json"), original_state);
    ASSERT_TRUE(fs::exists(f.provider / "retirement.json"));
    if (damage != 1) ASSERT_FALSE(fs::exists(f.provider / "state.json"));
  }
  return 0;
}

int ReplacedStateAndMismatchedOperationNeverAuthorizeCleanup() {
  Fixture f;
  f.State();
  Json facts;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  const auto journal = Bytes(f.provider / "retirement.json");
  ASSERT_NE(f.Transaction("restore", facts, kOtherOperation), VXCORE_OK);
  ASSERT_EQ(Bytes(f.provider / "retirement.json"), journal);
  const auto original = Bytes(f.archive / "state.json");
  Put(f.archive / "state.json", original);
  f.runtime.persisted_routing_cleared = true;
  ASSERT_NE(f.Transaction("commit", facts), VXCORE_OK);
  ASSERT_NE(f.Transaction("recover", facts), VXCORE_OK);
  ASSERT_EQ(Bytes(f.provider / "retirement.json"), journal);
  ASSERT_EQ(Bytes(f.archive / "state.json"), original);
  return 0;
}

int DisabledRecoveryRequiresActualClearedRouting() {
  Fixture f;
  f.State();
  Json facts;
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  f.Phase("disabled");
  ASSERT_NE(f.Transaction("recover", facts), VXCORE_OK);
  ASSERT_NE(f.Transaction("restore", facts), VXCORE_OK);
  f.runtime.persisted_routing_cleared = true;
  f.runtime.runtime_registered = true;
  ASSERT_EQ(f.Transaction("recover", facts), VXCORE_ERR_SYNC_IN_PROGRESS);
  f.runtime.runtime_registered = false;
  ASSERT_EQ(f.Transaction("recover", facts), VXCORE_OK);
  ASSERT_TRUE(fs::exists(f.archive / "state.json"));
  ASSERT_FALSE(fs::exists(f.provider / "state.json"));
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  return 0;
}

int LegacyJournalIsReadableButNotOwnershipAuthority() {
  for (const auto *phase : {"prepared", "archived", "disabled"}) {
    Fixture f;
    f.State();
    Json facts;
    ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
    auto legacy = Object(f.provider / "retirement.json");
    legacy["phase"] = phase;
    for (auto &entry : legacy["entries"]) entry.erase("inventory");
    Put(f.provider / "retirement.json", legacy.dump());
    const auto bytes = Bytes(f.provider / "retirement.json");
    ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
    ASSERT_TRUE(facts.at("retirement").get<bool>());
    ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_OK);
    ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
    f.runtime.persisted_routing_cleared = true;
    for (const auto *kind : {"restore", "commit", "recover"})
      ASSERT_EQ(f.Transaction(kind, facts), VXCORE_ERR_INVALID_STATE);
    ASSERT_EQ(Bytes(f.provider / "retirement.json"), bytes);
    ASSERT_TRUE(fs::exists(f.archive / "state.json"));
    legacy["entries"][0]["name"] = "../state.json";
    Put(f.provider / "retirement.json", legacy.dump());
    ASSERT_NE(f.Inspect(facts), VXCORE_OK);
    ASSERT_TRUE(fs::exists(f.archive / "state.json"));
  }
  return 0;
}

int CandidateRecoveryIsNotAnEncryptionKeyConflict() {
  Fixture f;
  f.State();
  const auto published = Bytes(f.provider / "state.json");
  const auto directory = f.provider / "credential-validation" / kOperation;
  const auto user = webdav::HashBytes("candidate-account");
  Put(directory / "state.json", Baseline(user).dump());
  Put(directory / PathFromUtf8(SnapshotName()), "snapshot payload");
  Put(directory / "pending.json", Pending(kScratch, "upsertRemote", user).dump());
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("credentialValidation").get<bool>());
  ASSERT_TRUE(facts.at("pending").get<bool>());
  ASSERT_FALSE(facts.at("cleanCloneBaseline").get<bool>());
  ASSERT_EQ(facts.at("usernameHash"), webdav::HashBytes("published-account"));
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_OK);
  ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(Bytes(f.provider / "state.json"), published);
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  auto malformed = Object(directory / "pending.json");
  malformed["operationId"] = kOtherOperation;
  Put(directory / "pending.json", malformed.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_ERR_ENCRYPTION_SYNC_STATE);
  malformed["operationId"] = kOperation;
  malformed["operations"][0]["path"] = "ordinary.md";
  Put(directory / "pending.json", malformed.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  Put(directory / "pending.json", Pending(kScratch, "upsertRemote", user).dump());
  Put(directory / PathFromUtf8(SnapshotName()), "corrupted scratch snapshot");
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  ASSERT_EQ(Bytes(f.provider / "state.json"), published);
  return 0;
}

int CandidateCrashTailsAndBindingsAreClosed() {
  Fixture f;
  f.State();
  const auto directory = f.provider / "credential-validation" / kOperation;
  fs::create_directories(directory);
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("credentialValidation").get<bool>());
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_OK);
  Put(directory / "state.json", Baseline(webdav::HashBytes("candidate")).dump());
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  // Interrupted readback snapshots need not be referenced, but must be confined.
  Put(directory / PathFromUtf8(SnapshotName()), "unreferenced readback");
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  auto state = Object(directory / "state.json");
  state["remoteUrl"] = "https://example.invalid/different/";
  Put(directory / "state.json", state.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  state["remoteUrl"] = kEndpoint;
  state["notebookId"] = kOtherOperation;
  Put(directory / "state.json", state.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  state["notebookId"] = kNotebook;
  Put(directory / "state.json", state.dump());
  Put(directory / "unknown.json", "{}");
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  return 0;
}

int CandidateOnlyEnableDoesNotPublishAccountFacts() {
  Fixture f;
  const auto directory = f.provider / "credential-validation" / kOperation;
  Put(directory / "state.json", Baseline(webdav::HashBytes("candidate-only")).dump());
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_FALSE(facts.at("hasState").get<bool>());
  ASSERT_TRUE(facts.at("usernameHash").is_null());
  ASSERT_TRUE(facts.at("baselineSha256").is_null());
  ASSERT_TRUE(facts.at("remoteUrl").is_null());
  ASSERT_TRUE(facts.at("credentialValidation").get<bool>());
  ASSERT_TRUE(facts.at("pending").get<bool>());
  ASSERT_FALSE(facts.at("cleanCloneBaseline").get<bool>());
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_OK);
  ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_FALSE(fs::exists(f.provider / "state.json"));
  return 0;
}

int EmptyCandidateParentArchivesWithoutDeletingAnything() {
  Fixture f;
  f.State();
  fs::create_directories(f.provider / "credential-validation");
  FileIdentity original;
  ASSERT_EQ(GetFileIdentity(f.provider / "credential-validation", original), VXCORE_OK);
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_FALSE(facts.at("credentialValidation").get<bool>());
  ASSERT_FALSE(facts.at("pending").get<bool>());
  ASSERT_FALSE(facts.at("cleanCloneBaseline").get<bool>());
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_FALSE(fs::exists(f.provider / "credential-validation"));
  ASSERT_TRUE(fs::is_empty(f.archive / "credential-validation"));
  ASSERT_EQ(f.Transaction("restore", facts), VXCORE_OK);
  FileIdentity restored;
  ASSERT_EQ(GetFileIdentity(f.provider / "credential-validation", restored), VXCORE_OK);
  ASSERT_TRUE(restored.SameObject(original));
  ASSERT_TRUE(fs::is_empty(f.provider / "credential-validation"));
  ASSERT_TRUE(fs::is_directory(f.archive));
  fs::create_directory(f.provider / "credential-validation" / kOperation);
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("credentialValidation").get<bool>());
  ASSERT_EQ(f.Transaction("begin", facts, kOtherOperation), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_TRUE(fs::is_directory(f.provider / "credential-validation" / kOperation));
  return 0;
}

int KeyConflictsPendingAndProbeRemainProtected() {
  Fixture f;
  f.State();
  f.Snapshot();
  auto state = Baseline();
  const auto conflict = Json{{"localSha256", webdav::HashBytes("snapshot payload")},
                             {"remoteSha256", nullptr}, {"remoteEtag", nullptr},
                             {"localSnapshot", SnapshotName()}, {"remoteSnapshot", nullptr},
                             {"isBinary", true}, {"canKeepBoth", false},
                             {"localModifiedUtc", 0}, {"remoteModifiedUtc", 0}};
  state["conflicts"]["vx_notebook/encryption.vne"] = conflict;
  Put(f.provider / "state.json", state.dump());
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("conflicts").get<bool>());
  ASSERT_EQ(CheckNotebookEncryptionSyncState(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_CONFLICT);
  state["conflicts"] = Json::object();
  Put(f.provider / "state.json", state.dump());
  Put(f.provider / "pending.json", Pending("vx_notebook/encryption.vne", "upsertLocal").dump());
  WebDavStorageStatus status;
  ASSERT_EQ(InspectWebDavStorage(f.provider, status), VXCORE_OK);
  ASSERT_TRUE(status.encryption_pending && status.replaces_encryption && status.any_pending);
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_CONFLICT);
  fs::remove(f.provider / "pending.json");
  Put(f.provider / "probe/state.json", Baseline().dump());
  Put(f.provider / "probe" / PathFromUtf8(SnapshotName()), "snapshot payload");
  Put(f.provider / "probe/pending.json", Pending(kScratch, "upsertRemote").dump());
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_TRUE(facts.at("probe").get<bool>());
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_ERR_SYNC_CONFLICT);
  ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_ERR_SYNC_CONFLICT);
  return 0;
}

int DormantLegacyProbeCanRetireWithoutChangingItsAccount() {
  Fixture f;
  f.State();
  const auto probe = Baseline(webdav::HashBytes("old-published-account")).dump();
  Put(f.provider / "probe/state.json", probe);
  fs::create_directories(f.provider / "probe/snapshots" / kOtherOperation);
  Json facts;
  ASSERT_EQ(f.Inspect(facts), VXCORE_OK);
  ASSERT_FALSE(facts.at("probe").get<bool>());
  ASSERT_FALSE(facts.at("pending").get<bool>());
  ASSERT_FALSE(facts.at("cleanCloneBaseline").get<bool>());
  ASSERT_EQ(CheckNotebookSyncReconfiguration(PathToUtf8(f.metadata)), VXCORE_OK);
  ASSERT_EQ(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_EQ(Bytes(f.archive / "probe/state.json"), probe);
  ASSERT_EQ(f.Transaction("restore", facts), VXCORE_OK);
  ASSERT_EQ(Bytes(f.provider / "probe/state.json"), probe);
  // Unreferenced payloads are not a completed clean probe and may not archive.
  Put(f.provider / "probe" / PathFromUtf8(SnapshotName()), "unowned readback");
  ASSERT_NE(f.Transaction("begin", facts, kOtherOperation), VXCORE_OK);
  ASSERT_TRUE(fs::exists(f.provider / "probe" / PathFromUtf8(SnapshotName())));
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  return 0;
}

int UnknownSchemaTraversalAndClosedCommandsAreRejected() {
  Fixture f;
  f.State();
  Json facts;
  auto state = Baseline();
  state["entries"]["../escape"] = {{"kind", "collection"}};
  Put(f.provider / "state.json", state.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  state = Baseline();
  state["unexpected"] = true;
  Put(f.provider / "state.json", state.dump());
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  f.State();
  auto raw = Bytes(f.provider / "state.json");
  raw.insert(1, "\"version\":1,");
  Put(f.provider / "state.json", raw);
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  f.State();
  auto command = Command("begin");
  command["path"] = "retired/attacker";
  ASSERT_NE(SyncProviderTransaction(PathToUtf8(f.root), kNotebook, command, f.runtime, facts), VXCORE_OK);
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  Put(f.provider / "reconfiguration.json", "{}");
  ASSERT_NE(f.Inspect(facts), VXCORE_OK);
  ASSERT_NE(f.Transaction("begin", facts), VXCORE_OK);
  ASSERT_TRUE(fs::exists(f.provider / "state.json"));
  ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
  return 0;
}

int LinksAreNeverFollowedOrArchived() {
  Fixture f;
  f.State();
  f.Snapshot();
  const auto outside = f.root / "outside.bin";
  Put(outside, "outside bytes");
  const auto snapshot = f.provider / PathFromUtf8(SnapshotName());
  fs::remove(snapshot);
  std::error_code error;
  fs::create_hard_link(outside, snapshot, error);
  if (!error) {
    Json facts;
    ASSERT_NE(f.Inspect(facts), VXCORE_OK);
    ASSERT_NE(f.Transaction("begin", facts), VXCORE_OK);
    ASSERT_EQ(Bytes(outside), "outside bytes");
    ASSERT_FALSE(fs::exists(f.provider / "retirement.json"));
    fs::remove(snapshot);
  } else {
    std::cout << "hard-link case unavailable on this filesystem\n";
  }
  error.clear();
  fs::create_symlink(outside, snapshot, error);
  if (!error) {
    Json facts;
    ASSERT_NE(f.Inspect(facts), VXCORE_OK);
    ASSERT_NE(f.Transaction("begin", facts), VXCORE_OK);
    ASSERT_EQ(Bytes(outside), "outside bytes");
    fs::remove(snapshot);
  } else {
    std::cout << "symlink case unavailable on this filesystem\n";
  }
  const auto alias = f.root / "metadata-link";
  error.clear();
  fs::create_directory_symlink(f.metadata, alias, error);
  if (!error) {
    WebDavStorageStatus status;
    ASSERT_NE(InspectWebDavStorage(alias / "vx_sync/webdav", status), VXCORE_OK);
    ASSERT_EQ(Bytes(outside), "outside bytes");
  } else {
    std::cout << "directory-link case unavailable on this filesystem\n";
  }
  return 0;
}
}  // namespace

int main() {
  try {
    RUN_TEST(LongPrivateCandidateAndArchiveKeepLogicalOwnership);
    RUN_TEST(ReadOnlyAbsentAndUnsupportedRouting);
    RUN_TEST(CleanBaselineIsOnlyADiagnostic);
    RUN_TEST(InspectionRecoversOriginalIdentityWithoutTargetingForeignArchives);
    RUN_TEST(ArchiveRestoreAndCommitPreserveEveryByte);
    RUN_TEST(PreparedPartialArchiveRestoresAsAWhole);
    RUN_TEST(AllEntriesValidateBeforeTheFirstRestoreMove);
    RUN_TEST(ReplacedStateAndMismatchedOperationNeverAuthorizeCleanup);
    RUN_TEST(DisabledRecoveryRequiresActualClearedRouting);
    RUN_TEST(LegacyJournalIsReadableButNotOwnershipAuthority);
    RUN_TEST(CandidateRecoveryIsNotAnEncryptionKeyConflict);
    RUN_TEST(CandidateCrashTailsAndBindingsAreClosed);
    RUN_TEST(CandidateOnlyEnableDoesNotPublishAccountFacts);
    RUN_TEST(EmptyCandidateParentArchivesWithoutDeletingAnything);
    RUN_TEST(KeyConflictsPendingAndProbeRemainProtected);
    RUN_TEST(DormantLegacyProbeCanRetireWithoutChangingItsAccount);
    RUN_TEST(UnknownSchemaTraversalAndClosedCommandsAreRejected);
    RUN_TEST(LinksAreNeverFollowedOrArchived);
    std::cout << "provider retirement/storage tests passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "provider storage test exception: " << error.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "provider storage test failed\n";
    return 1;
  }
}
