// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/store.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace rcb {
namespace {

Status PlatformError(const char* what, const std::string& path) {
  std::string detail = what;
  detail += " failed for '";
  detail += path;
  detail += "'";
#if defined(_WIN32)
  detail += " (win32 error ";
  detail += std::to_string(static_cast<unsigned long>(GetLastError()));
  detail += ")";
#else
  detail += " (errno ";
  detail += std::to_string(errno);
  detail += ")";
#endif
  return Status::Error(ErrorCode::PersistenceIoError, std::move(detail));
}

#if defined(_WIN32)

std::wstring WidenPath(const std::string& path) {
  if (path.empty()) {
    return std::wstring();
  }
  const int needed = MultiByteToWideChar(CP_UTF8, 0, path.c_str(),
                                         static_cast<int>(path.size()), nullptr, 0);
  if (needed <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), wide.data(), needed);
  return wide;
}

Status WriteAllAt(HANDLE handle, u64 offset, const std::span<const std::uint8_t> bytes) {
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "SetFilePointerEx failed");
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const std::size_t chunk = bytes.size() - written;
    const DWORD request = static_cast<DWORD>(chunk > 0x10000000U ? 0x10000000U : chunk);
    DWORD done = 0;
    if (WriteFile(handle, bytes.data() + written, request, &done, nullptr) == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "WriteFile failed");
    }
    if (done == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "WriteFile wrote nothing");
    }
    written += done;
  }
  return Status::Ok();
}

Status ReadAllAt(HANDLE handle, u64 offset, u8* out, std::size_t length) {
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "SetFilePointerEx failed");
  }
  std::size_t read = 0;
  while (read < length) {
    const std::size_t chunk = length - read;
    const DWORD request = static_cast<DWORD>(chunk > 0x10000000U ? 0x10000000U : chunk);
    DWORD done = 0;
    if (ReadFile(handle, out + read, request, &done, nullptr) == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "ReadFile failed");
    }
    if (done == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "unexpected end of file");
    }
    read += done;
  }
  return Status::Ok();
}

Status SetEndOfFileAt(HANDLE handle, u64 size) {
  LARGE_INTEGER position;
  position.QuadPart = static_cast<LONGLONG>(size);
  if (SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) == 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "SetFilePointerEx failed");
  }
  if (SetEndOfFile(handle) == 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "SetEndOfFile failed");
  }
  return Status::Ok();
}

// Windows has no directory fsync; MoveFileEx with MOVEFILE_WRITE_THROUGH is the
// durability boundary for the rename itself, so this is only used on POSIX.
[[maybe_unused]] Status FlushDirectory(const std::string& /*path*/) { return Status::Ok(); }

bool FileExistsImpl(const std::string& path) {
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return false;
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#else  // POSIX

Status WriteAllAt(const int fd, const u64 offset, const std::span<const std::uint8_t> bytes) {
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t done = ::pwrite(fd, bytes.data() + written, bytes.size() - written,
                                  static_cast<off_t>(offset + written));
    if (done < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::Error(ErrorCode::PersistenceIoError, "pwrite failed");
    }
    if (done == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "pwrite wrote nothing");
    }
    written += static_cast<std::size_t>(done);
  }
  return Status::Ok();
}

Status ReadAllAt(const int fd, const u64 offset, u8* out, const std::size_t length) {
  std::size_t read = 0;
  while (read < length) {
    const ssize_t done = ::pread(fd, out + read, length - read,
                                 static_cast<off_t>(offset + read));
    if (done < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status::Error(ErrorCode::PersistenceIoError, "pread failed");
    }
    if (done == 0) {
      return Status::Error(ErrorCode::PersistenceIoError, "unexpected end of file");
    }
    read += static_cast<std::size_t>(done);
  }
  return Status::Ok();
}

Status FlushDirectory(const std::string& path) {
  const std::string directory = path.empty() ? std::string(".") : path;
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "opening the directory for fsync failed");
  }
  const int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) {
    return Status::Error(ErrorCode::PersistenceIoError, "directory fsync failed");
  }
  return Status::Ok();
}

bool FileExistsImpl(const std::string& path) {
  struct stat info {};
  if (::stat(path.c_str(), &info) != 0) {
    return false;
  }
  return S_ISREG(info.st_mode);
}

#endif

std::string ParentDirectory(const std::string& path) {
  const std::size_t separator = path.find_last_of("/\\");
  if (separator == std::string::npos) {
    return std::string(".");
  }
  if (separator == 0) {
    return path.substr(0, 1);
  }
  return path.substr(0, separator);
}

}  // namespace

// ---- memory byte log -----------------------------------------------------

Status MemoryByteLog::Append(const std::span<const std::uint8_t> bytes) {
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the log is closed");
  }
  if (fail_point_ == FailPoint::Append) {
    fail_point_ = FailPoint::None;
    return Status::Error(ErrorCode::PersistenceIoError, "injected append failure");
  }
  if (fail_point_ == FailPoint::PartialAppend) {
    fail_point_ = FailPoint::None;
    const std::size_t half = bytes.size() / 2U;
    buffer_.insert(buffer_.end(), bytes.begin(),
                   bytes.begin() + static_cast<std::ptrdiff_t>(half));
    size_ = buffer_.size();
    return Status::Error(ErrorCode::PersistenceIoError, "injected partial append failure");
  }
  buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  size_ = buffer_.size();
  return Status::Ok();
}

Status MemoryByteLog::Sync() {
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the log is closed");
  }
  if (fail_point_ == FailPoint::Sync) {
    fail_point_ = FailPoint::None;
    return Status::Error(ErrorCode::PersistenceIoError, "injected sync failure");
  }
  if (sync_is_durable_) {
    synced_ = size_;
  }
  return Status::Ok();
}

Result<Bytes> MemoryByteLog::ReadRange(const u64 offset, const std::size_t length) {
  if (fail_point_ == FailPoint::Read) {
    fail_point_ = FailPoint::None;
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "injected read failure");
  }
  if (offset > size_ || length > size_ - offset) {
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "read past the end of the log");
  }
  return Bytes(buffer_.begin() + static_cast<std::ptrdiff_t>(offset),
               buffer_.begin() + static_cast<std::ptrdiff_t>(offset + length));
}

Result<Bytes> MemoryByteLog::ReadAll(const std::size_t max_bytes) {
  if (fail_point_ == FailPoint::Read) {
    fail_point_ = FailPoint::None;
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "injected read failure");
  }
  if (size_ > max_bytes) {
    return Fail<Bytes>(ErrorCode::LimitExceeded, "the log is larger than the read limit");
  }
  return Bytes(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size_));
}

Status MemoryByteLog::Truncate(const u64 size) {
  if (fail_point_ == FailPoint::Truncate) {
    fail_point_ = FailPoint::None;
    return Status::Error(ErrorCode::PersistenceIoError, "injected truncate failure");
  }
  if (size > size_) {
    return Status::Error(ErrorCode::InvalidArgument, "cannot extend a log by truncation");
  }
  buffer_.resize(static_cast<std::size_t>(size));
  size_ = size;
  if (synced_ > size) {
    synced_ = size;
  }
  return Status::Ok();
}

Status MemoryByteLog::Close() {
  closed_ = true;
  return Status::Ok();
}

// ---- memory snapshot store ----------------------------------------------

Status MemorySnapshotStore::Publish(const std::span<const std::uint8_t> bytes) {
  if (fail_point_ == FailPoint::Publish || persistent_failure_ == FailPoint::Publish) {
    fail_point_ = FailPoint::None;
    return Status::Error(ErrorCode::PersistenceIoError, "injected snapshot publish failure");
  }
  if (fail_point_ == FailPoint::PartialPublish || persistent_failure_ == FailPoint::PartialPublish) {
    fail_point_ = FailPoint::None;
    content_.assign(bytes.begin(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2U));
    has_content_ = true;
    return Status::Error(ErrorCode::PersistenceIoError, "injected partial publish failure");
  }
  content_.assign(bytes.begin(), bytes.end());
  has_content_ = true;
  ++publish_count_;
  return Status::Ok();
}

Result<std::optional<Bytes>> MemorySnapshotStore::Load(const std::size_t max_bytes) {
  if (fail_point_ == FailPoint::Load || persistent_failure_ == FailPoint::Load) {
    fail_point_ = FailPoint::None;
    return Fail<std::optional<Bytes>>(ErrorCode::PersistenceIoError, "injected load failure");
  }
  if (!has_content_) {
    return std::optional<Bytes>();
  }
  if (content_.size() > max_bytes) {
    return Fail<std::optional<Bytes>>(ErrorCode::LimitExceeded,
                                      "the snapshot is larger than the read limit");
  }
  return std::optional<Bytes>(content_);
}

Status MemorySnapshotStore::Remove() {
  if (fail_point_ == FailPoint::Remove || persistent_failure_ == FailPoint::Remove) {
    fail_point_ = FailPoint::None;
    return Status::Error(ErrorCode::PersistenceIoError, "injected snapshot remove failure");
  }
  content_.clear();
  has_content_ = false;
  return Status::Ok();
}

// ---- file byte log -------------------------------------------------------

Result<std::unique_ptr<FileByteLog>> FileByteLog::Open(const std::string& path, const bool create) {
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Fail<std::unique_ptr<FileByteLog>>(ErrorCode::InvalidArgument,
                                              "the log path is not valid UTF-8");
  }
  const DWORD disposition = create ? OPEN_ALWAYS : OPEN_EXISTING;
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, disposition,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Fail<std::unique_ptr<FileByteLog>>(
        ErrorCode::PersistenceIoError,
        "CreateFile failed for '" + path + "' (win32 error " +
            std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
  }
  LARGE_INTEGER size;
  if (GetFileSizeEx(handle, &size) == 0) {
    CloseHandle(handle);
    return Fail<std::unique_ptr<FileByteLog>>(ErrorCode::PersistenceIoError, "GetFileSizeEx failed");
  }
  auto log = std::unique_ptr<FileByteLog>(new FileByteLog());
  log->handle_ = handle;
  log->size_ = static_cast<u64>(size.QuadPart);
  log->path_ = path;
  return log;
#else
  const int flags = O_RDWR | (create ? O_CREAT : 0);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    return Fail<std::unique_ptr<FileByteLog>>(ErrorCode::PersistenceIoError,
                                              "open failed for '" + path + "'");
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    ::close(fd);
    return Fail<std::unique_ptr<FileByteLog>>(ErrorCode::PersistenceIoError, "fstat failed");
  }
  auto log = std::unique_ptr<FileByteLog>(new FileByteLog());
  log->handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
  log->size_ = static_cast<u64>(info.st_size);
  log->path_ = path;
  return log;
#endif
}

FileByteLog::~FileByteLog() { (void)Close(); }

Status FileByteLog::Append(const std::span<const std::uint8_t> bytes) {
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the log is closed");
  }
#if defined(_WIN32)
  const Status written = WriteAllAt(static_cast<HANDLE>(handle_), size_, bytes);
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  const Status written = WriteAllAt(fd, size_, bytes);
#endif
  if (!written.ok()) {
    return written;
  }
  size_ += bytes.size();
  return Status::Ok();
}

Status FileByteLog::Sync() {
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the log is closed");
  }
#if defined(_WIN32)
  if (FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return PlatformError("FlushFileBuffers", path_);
  }
  return Status::Ok();
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  if (::fsync(fd) != 0) {
    return PlatformError("fsync", path_);
  }
  return Status::Ok();
#endif
}

Result<Bytes> FileByteLog::ReadRange(const u64 offset, const std::size_t length) {
  if (offset > size_ || length > size_ - offset) {
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "read past the end of the log");
  }
  Bytes buffer(length);
  if (length == 0) {
    return buffer;
  }
#if defined(_WIN32)
  const Status read = ReadAllAt(static_cast<HANDLE>(handle_), offset, buffer.data(), length);
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  const Status read = ReadAllAt(fd, offset, buffer.data(), length);
#endif
  if (!read.ok()) {
    return Result<Bytes>(read);
  }
  return buffer;
}

Result<Bytes> FileByteLog::ReadAll(const std::size_t max_bytes) {
  if (size_ > max_bytes) {
    return Fail<Bytes>(ErrorCode::LimitExceeded, "the log is larger than the read limit");
  }
  return ReadRange(0, static_cast<std::size_t>(size_));
}

Status FileByteLog::Truncate(const u64 size) {
  if (size > size_) {
    return Status::Error(ErrorCode::InvalidArgument, "cannot extend a log by truncation");
  }
#if defined(_WIN32)
  const Status truncated = SetEndOfFileAt(static_cast<HANDLE>(handle_), size);
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  Status truncated = ::ftruncate(fd, static_cast<off_t>(size)) == 0
                         ? Status::Ok()
                         : PlatformError("ftruncate", path_);
#endif
  if (!truncated.ok()) {
    return truncated;
  }
  size_ = size;
  return Sync();
}

Status FileByteLog::Close() {
  if (closed_) {
    return Status::Ok();
  }
  closed_ = true;
#if defined(_WIN32)
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
#else
  if (handle_ != nullptr) {
    const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
    ::close(fd);
    handle_ = nullptr;
  }
#endif
  return Status::Ok();
}

// ---- file snapshot store -------------------------------------------------

Status FileSnapshotStore::Publish(const std::span<const std::uint8_t> bytes) {
  const std::string temporary = path_ + ".tmp";
  const Status written = WriteFileAtomically(temporary, bytes);
  if (!written.ok()) {
    return written;
  }
  const Result<Bytes> verified = ReadWholeFile(temporary, bytes.size() + 1U);
  if (!verified.ok()) {
    (void)RemoveFile(temporary);
    return Status::Error(ErrorCode::PersistenceIoError,
                         "the snapshot could not be read back for verification");
  }
  if (verified.value().size() != bytes.size() ||
      !std::equal(bytes.begin(), bytes.end(), verified.value().begin())) {
    (void)RemoveFile(temporary);
    return Status::Error(ErrorCode::PersistenceIoError,
                         "the snapshot does not match what was written; refusing to publish it");
  }
#if defined(_WIN32)
  const std::wstring from = WidenPath(temporary);
  const std::wstring to = WidenPath(path_);
  if (from.empty() || to.empty() ||
      MoveFileExW(from.c_str(), to.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    (void)RemoveFile(temporary);
    return PlatformError("MoveFileEx", path_);
  }
  return Status::Ok();
#else
  if (::rename(temporary.c_str(), path_.c_str()) != 0) {
    (void)RemoveFile(temporary);
    return PlatformError("rename", path_);
  }
  return FlushDirectory(ParentDirectory(path_));
#endif
}

Result<std::optional<Bytes>> FileSnapshotStore::Load(const std::size_t max_bytes) {
  if (!FileExistsImpl(path_)) {
    return std::optional<Bytes>();
  }
  Result<Bytes> bytes = ReadWholeFile(path_, max_bytes);
  if (!bytes.ok()) {
    return Result<std::optional<Bytes>>(bytes.status());
  }
  return std::optional<Bytes>(std::move(bytes.value()));
}

Status FileSnapshotStore::Remove() { return RemoveFile(path_); }

bool FileSnapshotStore::exists() { return FileExistsImpl(path_); }

// ---- lock ----------------------------------------------------------------

Result<std::unique_ptr<StoreLock>> StoreLock::Acquire(const std::string& path) {
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Fail<std::unique_ptr<StoreLock>>(ErrorCode::InvalidArgument,
                                            "the lock path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Fail<std::unique_ptr<StoreLock>>(
        ErrorCode::PersistenceIoError,
        "another process holds the store lock on '" + path + "' (win32 error " +
            std::to_string(static_cast<unsigned long>(GetLastError())) + ")");
  }
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd < 0) {
    return Fail<std::unique_ptr<StoreLock>>(ErrorCode::PersistenceIoError,
                                            "open failed for the store lock '" + path + "'");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    return Fail<std::unique_ptr<StoreLock>>(ErrorCode::PersistenceIoError,
                                            "another process holds the store lock on '" + path +
                                                "'");
  }
  // Stored as an opaque pointer so that the same member serves both platforms;
  // HANDLE is a Windows type and must not appear on this path.
  void* handle = reinterpret_cast<void*>(static_cast<std::intptr_t>(fd));
#endif
  auto lock = std::unique_ptr<StoreLock>(new StoreLock());
  lock->handle_ = handle;
  lock->path_ = path;
  return lock;
}

StoreLock::~StoreLock() {
  if (handle_ == nullptr) {
    return;
  }
#if defined(_WIN32)
  CloseHandle(static_cast<HANDLE>(handle_));
#else
  const int fd = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_));
  (void)::flock(fd, LOCK_UN);
  ::close(fd);
#endif
  handle_ = nullptr;
}

// ---- filesystem helpers --------------------------------------------------

Result<Bytes> ReadWholeFile(const std::string& path, const std::size_t max_bytes) {
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Fail<Bytes>(ErrorCode::InvalidArgument, "the path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Fail<Bytes>(ErrorCode::NotFound, "no such file: '" + path + "'");
    }
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "CreateFile failed for '" + path + "'");
  }
  LARGE_INTEGER size;
  if (GetFileSizeEx(handle, &size) == 0) {
    CloseHandle(handle);
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "GetFileSizeEx failed");
  }
  if (static_cast<u64>(size.QuadPart) > max_bytes) {
    CloseHandle(handle);
    return Fail<Bytes>(ErrorCode::LimitExceeded, "the file is larger than the read limit");
  }
  Bytes buffer(static_cast<std::size_t>(size.QuadPart));
  if (!buffer.empty()) {
    const Status read = ReadAllAt(handle, 0, buffer.data(), buffer.size());
    if (!read.ok()) {
      CloseHandle(handle);
      return Result<Bytes>(read);
    }
  }
  CloseHandle(handle);
  return buffer;
#else
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errno == ENOENT) {
      return Fail<Bytes>(ErrorCode::NotFound, "no such file: '" + path + "'");
    }
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "open failed for '" + path + "'");
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    ::close(fd);
    return Fail<Bytes>(ErrorCode::PersistenceIoError, "fstat failed");
  }
  if (static_cast<u64>(info.st_size) > max_bytes) {
    ::close(fd);
    return Fail<Bytes>(ErrorCode::LimitExceeded, "the file is larger than the read limit");
  }
  Bytes buffer(static_cast<std::size_t>(info.st_size));
  if (!buffer.empty()) {
    const Status read = ReadAllAt(fd, 0, buffer.data(), buffer.size());
    if (!read.ok()) {
      ::close(fd);
      return Result<Bytes>(read);
    }
  }
  ::close(fd);
  return buffer;
#endif
}

Status WriteFileAtomically(const std::string& path, const std::span<const std::uint8_t> bytes) {
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "the path is not valid UTF-8");
  }
  HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return PlatformError("CreateFile", path);
  }
  const Status written = WriteAllAt(handle, 0, bytes);
  if (!written.ok()) {
    CloseHandle(handle);
    return written;
  }
  if (FlushFileBuffers(handle) == 0) {
    CloseHandle(handle);
    return PlatformError("FlushFileBuffers", path);
  }
  CloseHandle(handle);
  return Status::Ok();
#else
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    return PlatformError("open", path);
  }
  const Status written = WriteAllAt(fd, 0, bytes);
  if (!written.ok()) {
    ::close(fd);
    return written;
  }
  if (::fsync(fd) != 0) {
    ::close(fd);
    return PlatformError("fsync", path);
  }
  ::close(fd);
  return Status::Ok();
#endif
}

Status RemoveFile(const std::string& path) {
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "the path is not valid UTF-8");
  }
  if (DeleteFileW(wide.c_str()) == 0) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return Status::Ok();
    }
    return PlatformError("DeleteFile", path);
  }
  return Status::Ok();
#else
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return PlatformError("unlink", path);
  }
  return Status::Ok();
#endif
}

bool FileExists(const std::string& path) { return FileExistsImpl(path); }

Status CreateDirectories(const std::string& path) {
  if (path.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "empty directory path");
  }
#if defined(_WIN32)
  const std::wstring wide = WidenPath(path);
  if (wide.empty()) {
    return Status::Error(ErrorCode::InvalidArgument, "the path is not valid UTF-8");
  }
  const DWORD attributes = GetFileAttributesW(wide.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return Status::Ok();
    }
    return Status::Error(ErrorCode::PersistenceIoError,
                         "a file already exists where a directory is required: '" + path + "'");
  }
  if (CreateDirectoryW(wide.c_str(), nullptr) == 0) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) {
      // Create the parent chain, then retry once.
      const std::string parent = ParentDirectory(path);
      if (parent != path) {
        const Status created = CreateDirectories(parent);
        if (!created.ok()) {
          return created;
        }
      }
      if (CreateDirectoryW(wide.c_str(), nullptr) == 0 && GetLastError() != ERROR_ALREADY_EXISTS) {
        return PlatformError("CreateDirectory", path);
      }
    }
  }
  return Status::Ok();
#else
  if (::mkdir(path.c_str(), 0755) == 0) {
    return Status::Ok();
  }
  if (errno == EEXIST) {
    struct stat info {};
    if (::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
      return Status::Ok();
    }
    return Status::Error(ErrorCode::PersistenceIoError,
                         "a file already exists where a directory is required: '" + path + "'");
  }
  const std::string parent = ParentDirectory(path);
  if (parent != path) {
    const Status created = CreateDirectories(parent);
    if (!created.ok()) {
      return created;
    }
  }
  if (::mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
    return PlatformError("mkdir", path);
  }
  return Status::Ok();
#endif
}

}  // namespace rcb
