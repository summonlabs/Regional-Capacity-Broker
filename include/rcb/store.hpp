// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Platform adapters for durability.
//
// Everything in this header is an adapter: the broker kernel above it never
// sees a file, a handle or a system call. Two capabilities matter here and
// nowhere else:
//
//   * a byte log with an explicit durability barrier. Success from Append means
//     the bytes were handed to the platform; success from Sync means the
//     platform reports them durable. The two are never conflated;
//   * a snapshot store that publishes a whole file atomically: write a
//     temporary file, read it back and verify it, rename it over the target,
//     and make the rename itself durable.
//
// A memory implementation of each exists so that crash boundaries, torn writes
// and I/O failures can be injected deterministically in tests.

#ifndef RCB_STORE_HPP
#define RCB_STORE_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "rcb/digest.hpp"
#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

/// An append-only byte log with an explicit durability barrier.
class ByteLog {
 public:
  ByteLog() = default;
  virtual ~ByteLog() = default;
  ByteLog(const ByteLog&) = delete;
  ByteLog& operator=(const ByteLog&) = delete;

  /// Appends bytes. May be buffered by the platform; see Sync().
  virtual Status Append(std::span<const std::uint8_t> bytes) = 0;
  /// Durability barrier: returns only once the platform reports the bytes
  /// durable.
  virtual Status Sync() = 0;
  /// Reads a byte range. Used to verify what was appended.
  virtual Result<Bytes> ReadRange(u64 offset, std::size_t length) = 0;
  /// Reads the whole log, bounded by \p max_bytes.
  virtual Result<Bytes> ReadAll(std::size_t max_bytes) = 0;
  /// Shortens the log. Only ever used to drop a torn tail or a superseded log.
  virtual Status Truncate(u64 size) = 0;
  virtual Status Close() = 0;
  [[nodiscard]] virtual u64 size() const = 0;
  /// True when Sync() reaches real platform durability for this log.
  [[nodiscard]] virtual bool durable() const = 0;
};

/// Publishes whole files atomically.
class SnapshotStore {
 public:
  SnapshotStore() = default;
  virtual ~SnapshotStore() = default;
  SnapshotStore(const SnapshotStore&) = delete;
  SnapshotStore& operator=(const SnapshotStore&) = delete;

  /// Writes, verifies and atomically publishes the bytes.
  virtual Status Publish(std::span<const std::uint8_t> bytes) = 0;
  /// Loads the published bytes, or nullopt when nothing has been published.
  virtual Result<std::optional<Bytes>> Load(std::size_t max_bytes) = 0;
  virtual Status Remove() = 0;
  [[nodiscard]] virtual bool exists() = 0;
};

// ---- in-memory adapters --------------------------------------------------

/// A byte log in memory, with a switch for every failure the platform can make.
class MemoryByteLog final : public ByteLog {
 public:
  enum class FailPoint {
    None,
    Append,     ///< the next Append fails without writing
    PartialAppend,  ///< the next Append writes half of the bytes, then fails
    Sync,       ///< the next Sync fails after the bytes are visible
    Truncate,   ///< the next Truncate fails
    Read,       ///< the next ReadRange fails
  };

  MemoryByteLog() = default;

  /// Arms a one-shot failure. The next matching call fails once.
  void FailOnce(FailPoint point) { fail_point_ = point; }
  /// When true, Sync() reports success without making anything durable.
  void SetSyncIsDurable(bool durable) { sync_is_durable_ = durable; }
  /// Bytes appended but not yet synced (the "not durable" window).
  [[nodiscard]] std::size_t unsynced_bytes() const { return size_ - synced_; }
  /// Simulates a crash: everything appended but not synced is discarded.
  void SimulateCrash() { buffer_.resize(synced_); size_ = synced_; }
  [[nodiscard]] const Bytes& buffer() const { return buffer_; }
  /// Replaces the log's content with arbitrary bytes (corruption injection).
  void SetContent(Bytes bytes) {
    buffer_ = std::move(bytes);
    size_ = buffer_.size();
    synced_ = size_;
  }
  void Reserve(std::size_t capacity) { buffer_.reserve(capacity); }
  [[nodiscard]] bool closed() const { return closed_; }

  Status Append(std::span<const std::uint8_t> bytes) override;
  Status Sync() override;
  Result<Bytes> ReadRange(u64 offset, std::size_t length) override;
  Result<Bytes> ReadAll(std::size_t max_bytes) override;
  Status Truncate(u64 size) override;
  Status Close() override;
  [[nodiscard]] u64 size() const override { return size_; }
  [[nodiscard]] bool durable() const override { return sync_is_durable_; }

 private:
  Bytes buffer_;
  u64 size_ = 0;
  u64 synced_ = 0;
  bool closed_ = false;
  bool sync_is_durable_ = true;
  FailPoint fail_point_ = FailPoint::None;
};

/// A snapshot store in memory, with the same failure injection.
class MemorySnapshotStore final : public SnapshotStore {
 public:
  enum class FailPoint { None, Publish, PartialPublish, Load, Remove };

  MemorySnapshotStore() = default;

  void FailOnce(FailPoint point) { fail_point_ = point; }
  /// Arms a failure that persists for every subsequent call.
  void FailAlways(FailPoint point) { persistent_failure_ = point; }
  void SetContent(Bytes bytes) { content_ = std::move(bytes); has_content_ = true; }
  /// The published snapshot bytes, empty when nothing has been published.
  [[nodiscard]] Bytes content() const { return has_content_ ? content_ : Bytes(); }
  [[nodiscard]] std::size_t publish_count() const { return publish_count_; }

  Status Publish(std::span<const std::uint8_t> bytes) override;
  Result<std::optional<Bytes>> Load(std::size_t max_bytes) override;
  Status Remove() override;
  [[nodiscard]] bool exists() override { return has_content_; }

 private:
  Bytes content_;
  bool has_content_ = false;
  std::size_t publish_count_ = 0;
  FailPoint fail_point_ = FailPoint::None;
  FailPoint persistent_failure_ = FailPoint::None;
};

// ---- platform adapters ---------------------------------------------------

/// An append-only file. On Windows durability goes through FlushFileBuffers; on
/// POSIX through fsync.
class FileByteLog final : public ByteLog {
 public:
  /// Opens (creating when \p create) the file for appending.
  static Result<std::unique_ptr<FileByteLog>> Open(const std::string& path, bool create);

  ~FileByteLog() override;
  FileByteLog(const FileByteLog&) = delete;
  FileByteLog& operator=(const FileByteLog&) = delete;

  Status Append(std::span<const std::uint8_t> bytes) override;
  Status Sync() override;
  Result<Bytes> ReadRange(u64 offset, std::size_t length) override;
  Result<Bytes> ReadAll(std::size_t max_bytes) override;
  Status Truncate(u64 size) override;
  Status Close() override;
  [[nodiscard]] u64 size() const override { return size_; }
  [[nodiscard]] bool durable() const override { return true; }

 private:
  FileByteLog() = default;

  void* handle_ = nullptr;
  u64 size_ = 0;
  std::string path_;
  bool closed_ = false;
};

/// A snapshot file published by write-verify-rename.
class FileSnapshotStore final : public SnapshotStore {
 public:
  explicit FileSnapshotStore(std::string path) : path_(std::move(path)) {}

  ~FileSnapshotStore() override = default;
  FileSnapshotStore(const FileSnapshotStore&) = delete;
  FileSnapshotStore& operator=(const FileSnapshotStore&) = delete;

  Status Publish(std::span<const std::uint8_t> bytes) override;
  Result<std::optional<Bytes>> Load(std::size_t max_bytes) override;
  Status Remove() override;
  [[nodiscard]] bool exists() override;

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_;
};

/// An exclusive advisory lock on a store directory, so that two processes
/// cannot both believe they are the writer. Released when the object dies.
class StoreLock {
 public:
  static Result<std::unique_ptr<StoreLock>> Acquire(const std::string& path);

  ~StoreLock();
  StoreLock(const StoreLock&) = delete;
  StoreLock& operator=(const StoreLock&) = delete;

 private:
  StoreLock() = default;
  void* handle_ = nullptr;
  std::string path_;
};

// ---- filesystem helpers --------------------------------------------------

/// Reads a whole file, bounded. Reports ErrorCode::NotFound when it is absent.
Result<Bytes> ReadWholeFile(const std::string& path, std::size_t max_bytes);
/// Atomically replaces a file's content, verifying what was written.
Status WriteFileAtomically(const std::string& path, std::span<const std::uint8_t> bytes);
/// Removes a file. Succeeds when it was already absent.
Status RemoveFile(const std::string& path);
/// True when the path names an existing file.
bool FileExists(const std::string& path);
/// Creates a directory and its parents. Succeeds when it already exists.
Status CreateDirectories(const std::string& path);

}  // namespace rcb

#endif  // RCB_STORE_HPP
