// Copyright (c) 2025 VNote
#include "mobile/file_writer_mobile.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <unistd.h>

#include "utils/file_utils.h"

namespace vxcore {

namespace {

VxCoreError MobileWriteError() {
  return errno == ENOMEM ? VXCORE_ERR_OUT_OF_MEMORY : VXCORE_ERR_IO;
}

// Match the bounded sharing-violation retry policy used by folder-config publication.
constexpr int kMobileRenameRetries = 5;
constexpr int kMobileRenameRetryDelayMs = 40;
constexpr int kMobileTempAttempts = 64;

}  // namespace

MobileFileWriter::MobileFileWriter(const std::filesystem::path &path) {
  try {
    path_ = path;
  } catch (const std::bad_alloc &) {
    error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    error_ = VXCORE_ERR_IO;
  }
}


MobileFileWriter::~MobileFileWriter() {
  if (parent_fd_ >= 0) ::close(parent_fd_);
  if (file_) {
    std::fclose(file_);
  }
  if (owns_temp_) ::unlink(temp_path_.c_str());
}

VxCoreError MobileFileWriter::InspectDestination(bool initial) {
  bool exists = true;
  uint64_t device = 0;
  uint64_t id = 0;
  unsigned int mode = 0;
  struct stat info{};
  if (::lstat(path_.c_str(), &info) != 0) {
    if (errno != ENOENT) return MobileWriteError();
    exists = false;
  } else {
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        !(info.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH))) return VXCORE_ERR_IO;
    // Opening without truncation checks ACL/effective write access. O_NOFOLLOW
    // and fstat also reject a link/type swap between lstat and open.
    const int fd = ::open(path_.c_str(), O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return MobileWriteError();
    struct stat opened{};
    const bool valid = ::fstat(fd, &opened) == 0 && S_ISREG(opened.st_mode) &&
        opened.st_nlink == 1 && opened.st_dev == info.st_dev && opened.st_ino == info.st_ino &&
        opened.st_mode == info.st_mode;
    const int close_result = ::close(fd);
    if (!valid || close_result != 0) return VXCORE_ERR_IO;
    device = static_cast<uint64_t>(info.st_dev);
    id = static_cast<uint64_t>(info.st_ino);
    mode = info.st_mode & 07777;
  }
  if (initial) {
    destination_exists_ = exists;
    destination_device_ = device;
    destination_id_ = id;
    destination_mode_ = mode;
  } else if (exists != destination_exists_ || device != destination_device_ ||
             id != destination_id_ || mode != destination_mode_) {
    return VXCORE_ERR_IO;
  }
  return VXCORE_OK;
}

VxCoreError MobileFileWriter::Open() {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (opened_) {
    return VXCORE_ERR_INVALID_STATE;
  }
  opened_ = true;

  try {
    if (path_.empty() || !path_.has_filename() ||
        path_.native().find(std::filesystem::path::value_type{}) !=
            std::filesystem::path::string_type::npos) {
      return error_ = VXCORE_ERR_INVALID_PARAM;
    }

    error_ = InspectDestination(true);
    if (error_ != VXCORE_OK) return error_;
    const auto parent = path_.has_parent_path() ? path_.parent_path() : std::filesystem::path(".");
    parent_fd_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd_ < 0) return error_ = MobileWriteError();

    // Exclusivity, not name unpredictability, prevents reuse of another writer's
    // file or a pre-existing symlink. No UUID/crypto dependency is needed here.
    static std::atomic<uint64_t> sequence{0};
    const auto process_id = ::getpid();
    for (int attempt = 0; attempt < kMobileTempAttempts; ++attempt) {
      temp_path_ = path_;
      temp_path_ += PathFromUtf8(".tmp-" + std::to_string(process_id) + "-" +
                                std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
      const int fd = ::open(temp_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                            S_IRUSR | S_IWUSR);
      if (fd < 0) {
        if (errno == EEXIST) {
          continue;
        }
        return error_ = MobileWriteError();
      }

      owns_temp_ = true;
      file_ = ::fdopen(fd, "wb");
      if (!file_) {
        error_ = MobileWriteError();
        ::close(fd);
        return error_;
      }
      return VXCORE_OK;
    }
    return error_ = VXCORE_ERR_IO;
  } catch (const std::bad_alloc &) {
    return error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return error_ = VXCORE_ERR_IO;
  }
}

VxCoreError MobileFileWriter::Write(const void *data, size_t size) {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (!file_) {
    return VXCORE_ERR_INVALID_STATE;
  }
  if (!data && size != 0) {
    return error_ = VXCORE_ERR_NULL_POINTER;
  }
  errno = 0;
  if (size != 0 && std::fwrite(data, 1, size, file_) != size) {
    return error_ = MobileWriteError();
  }
  return VXCORE_OK;
}

VxCoreError MobileFileWriter::Commit() {
  if (error_ != VXCORE_OK) {
    return error_;
  }
  if (!file_) {
    return VXCORE_ERR_INVALID_STATE;
  }

  errno = 0;
  if (std::fflush(file_) != 0) {
    error_ = MobileWriteError();
  }
  if (error_ == VXCORE_OK && destination_exists_ &&
      ::fchmod(fileno(file_), static_cast<mode_t>(destination_mode_)) != 0) {
    error_ = MobileWriteError();
  }
  if (error_ == VXCORE_OK) {
    errno = 0;
    if (::fsync(fileno(file_)) != 0) {
      error_ = MobileWriteError();
    }
  }
  errno = 0;
  const int close_result = std::fclose(file_);
  file_ = nullptr;
  if (close_result != 0 && error_ == VXCORE_OK) {
    error_ = MobileWriteError();
  }
  if (error_ != VXCORE_OK) {
    return error_;
  }

  try {
    for (int attempt = 0; attempt < kMobileRenameRetries; ++attempt) {
      std::error_code ec;
      error_ = InspectDestination(false);
      if (error_ != VXCORE_OK) return error_;
      // Same-directory rename replaces the destination atomically, without an
      // intermediate removal that could lose the previous live file on failure.
      std::filesystem::rename(temp_path_, path_, ec);
      if (!ec) {
        owns_temp_ = false;
        if (::fsync(parent_fd_) != 0) {
          // Publication already happened. Report the durability failure honestly.
          return error_ = MobileWriteError();
        }
        return VXCORE_OK;
      }
      if (ec == std::errc::not_enough_memory) {
        return error_ = VXCORE_ERR_OUT_OF_MEMORY;
      }
      if (attempt + 1 < kMobileRenameRetries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kMobileRenameRetryDelayMs));
      }
    }
    return error_ = VXCORE_ERR_IO;
  } catch (const std::bad_alloc &) {
    return error_ = VXCORE_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return error_ = VXCORE_ERR_IO;
  }
}

}  // namespace vxcore
