#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "test_reparse_utils.h"
#include "test_utils.h"

namespace file_utils_test {
enum class Fault { None, UnsupportedId, ZeroVolume, ZeroFileId, ZeroLinks, BasicInfoFailure };
Fault fault = Fault::None;

BOOL WINAPI FileInformation(HANDLE handle, BY_HANDLE_FILE_INFORMATION *info) {
  const BOOL ok = ::GetFileInformationByHandle(handle, info);
  if (!ok) return ok;
  if (fault == Fault::BasicInfoFailure) {
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
  }
  if (fault == Fault::ZeroLinks) info->nNumberOfLinks = 0;
  return ok;
}

BOOL WINAPI FileInformationEx(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, void *data,
                              DWORD size) {
  if (kind == FileIdInfo && fault == Fault::UnsupportedId) {
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
  }
  const BOOL ok = ::GetFileInformationByHandleEx(handle, kind, data, size);
  if (ok && kind == FileIdInfo) {
    auto *id = static_cast<FILE_ID_INFO *>(data);
    if (fault == Fault::ZeroVolume) id->VolumeSerialNumber = 0;
    if (fault == Fault::ZeroFileId)
      std::memset(id->FileId.Identifier, 0, sizeof(id->FileId.Identifier));
  }
  return ok;
}

struct ScopedFault {
  explicit ScopedFault(Fault value) { fault = value; }
  ~ScopedFault() { fault = Fault::None; }
};
}  // namespace file_utils_test

// Compile the production implementation once with test-local Win32 capability faults.
// Real opens, file writes, flushes, renames and handle closes remain unmodified.
#define GetFileInformationByHandle file_utils_test::FileInformation
#define GetFileInformationByHandleEx file_utils_test::FileInformationEx
#include "utils/file_utils.cpp"
#undef GetFileInformationByHandleEx
#undef GetFileInformationByHandle

namespace {
namespace fs = std::filesystem;
using file_utils_test::Fault;
using file_utils_test::ScopedFault;

struct Fixture {
  fs::path directory = vxcore::PathFromUtf8(
      get_test_path("file_utils_windows_" + std::to_string(GetCurrentProcessId())));
  Fixture() { fs::create_directories(directory); }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(directory, ec);
  }
};

std::string ReadBytes(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int test_publication_without_file_identity() {
  Fixture fixture;
  const std::string bytes("image\0\xff\r\n", 9);
  const std::string replacement = "must not overwrite";
  for (const auto fault :
       {Fault::UnsupportedId, Fault::ZeroVolume, Fault::ZeroFileId, Fault::ZeroLinks}) {
    const auto directory = fixture.directory / std::to_string(static_cast<int>(fault));
    ASSERT_TRUE(fs::create_directory(directory));
    ScopedFault injected(fault);
    // These controls prove the fault is active and strict identity remains fail-closed.
    vxcore::FileIdentity identity;
    ASSERT_NE(vxcore::GetFileIdentity(directory, identity), VXCORE_OK);
    ASSERT_NE(vxcore::FlushDirectory(directory, vxcore::FileAccess::kPrivate), VXCORE_OK);

    const auto asset = directory / "image.jpg";
    {
      vxcore::AtomicFileWriter writer(asset);
      ASSERT_EQ(writer.Open(), VXCORE_OK);
      ASSERT_EQ(writer.Write(bytes.data(), bytes.size()), VXCORE_OK);
      ASSERT_EQ(writer.CommitNoReplace(), VXCORE_OK);
    }
    ASSERT_EQ(ReadBytes(asset), bytes);
    ASSERT_EQ(vxcore::FlushDirectory(directory / ""), VXCORE_OK);
    {
      vxcore::AtomicFileWriter loser(asset);
      ASSERT_EQ(loser.Open(), VXCORE_OK);
      ASSERT_EQ(loser.Write(replacement.data(), replacement.size()), VXCORE_OK);
      ASSERT_NE(loser.CommitNoReplace(), VXCORE_OK);
    }
    ASSERT_EQ(ReadBytes(asset), bytes);
    ASSERT_EQ(std::distance(fs::directory_iterator(directory), fs::directory_iterator()), 1);
  }
  return 0;
}

int test_directory_validation_failures() {
  Fixture fixture;
  const auto file = fixture.directory / "ordinary.bin";
  const std::string bytes = "keep published bytes";
  ASSERT_EQ(vxcore::WriteFileAtomic(file, bytes), VXCORE_OK);
  ASSERT_EQ(vxcore::FlushDirectory({}), VXCORE_ERR_INVALID_PARAM);
  auto invalid = fixture.directory.native();
  invalid.push_back(L'\0');
  invalid += L"hidden-suffix";
  ASSERT_EQ(vxcore::FlushDirectory(fs::path(invalid)), VXCORE_ERR_INVALID_PARAM);
  ASSERT_NE(vxcore::FlushDirectory(file), VXCORE_OK);
  ASSERT_NE(vxcore::FlushDirectory(fixture.directory / "missing"), VXCORE_OK);
  ASSERT_EQ(vxcore::FlushDirectory(fixture.directory.root_path()), VXCORE_OK);

  const auto junction = fixture.directory / "junction";
  ASSERT_TRUE(vxcore_test::create_junction(vxcore::PathToUtf8(fixture.directory),
                                           vxcore::PathToUtf8(junction)));
  const auto junction_result = vxcore::FlushDirectory(junction);
  const auto trailing_result = vxcore::FlushDirectory(junction / "");
  vxcore_test::remove_reparse_dir(vxcore::PathToUtf8(junction));
  ASSERT_NE(junction_result, VXCORE_OK);
  ASSERT_NE(trailing_result, VXCORE_OK);

  ScopedFault injected(Fault::BasicInfoFailure);
  ASSERT_NE(vxcore::FlushDirectory(fixture.directory), VXCORE_OK);
  const auto published = fixture.directory / "published.bin";
  {
    vxcore::AtomicFileWriter writer(published);
    ASSERT_EQ(writer.Open(), VXCORE_OK);
    ASSERT_EQ(writer.Write(bytes.data(), bytes.size()), VXCORE_OK);
    ASSERT_NE(writer.CommitNoReplace(), VXCORE_OK);
  }
  ASSERT_EQ(ReadBytes(published), bytes);
  return 0;
}
}  // namespace

int main() {
  RUN_TEST(test_publication_without_file_identity);
  RUN_TEST(test_directory_validation_failures);
  std::cout << "Windows file publication capability tests passed\n";
  return 0;
}
