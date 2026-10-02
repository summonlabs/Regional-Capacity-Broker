// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Content identity: SHA-256 for authoritative digests and identity derivation,
// CRC-32C for per-record integrity in the journal. Both are first-party: the
// runtime links no cryptographic or compression library.

#ifndef RCB_DIGEST_HPP
#define RCB_DIGEST_HPP

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

/// A 256-bit digest.
class Digest {
 public:
  static constexpr std::size_t kSize = 32;

  Digest() noexcept = default;

  static Digest FromBytes(const std::array<u8, kSize>& bytes) noexcept;

  /// Parses exactly kSize*2 lowercase or uppercase hex digits.
  static Result<Digest> FromHex(std::string_view text);

  [[nodiscard]] const std::array<u8, kSize>& bytes() const noexcept { return bytes_; }

  /// 64 lowercase hex characters.
  [[nodiscard]] std::string Hex() const;

  /// The first \p count hex characters, for compact derived identities.
  [[nodiscard]] std::string ShortHex(std::size_t count) const;

  friend bool operator==(const Digest& a, const Digest& b) noexcept { return a.bytes_ == b.bytes_; }
  friend bool operator!=(const Digest& a, const Digest& b) noexcept { return !(a == b); }
  friend bool operator<(const Digest& a, const Digest& b) noexcept { return a.bytes_ < b.bytes_; }

 private:
  std::array<u8, kSize> bytes_{};
};

/// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256() noexcept;

  void Update(std::span<const u8> data) noexcept;
  void Update(std::string_view text) noexcept;
  void UpdateByte(u8 value) noexcept;

  /// Finalises; the hasher must not be updated afterwards without Reset().
  [[nodiscard]] Digest Finalize() noexcept;
  void Reset() noexcept;

 private:
  void Compress(const u8* block) noexcept;

  std::array<u32, 8> state_{};
  std::array<u8, 64> buffer_{};
  std::size_t buffered_ = 0;
  u64 total_bytes_ = 0;
};

/// Convenience: SHA-256 over a byte span.
Digest Sha256Of(std::span<const u8> data) noexcept;
/// Convenience: SHA-256 over text.
Digest Sha256Of(std::string_view text) noexcept;

/// CRC-32C (Castagnoli), used as the per-record integrity check in the journal.
u32 Crc32c(std::span<const u8> data) noexcept;

/// Incremental CRC-32C. value() applies the final complement, so it agrees
/// with the one-shot Crc32c() for the same byte sequence.
class Crc32cStream {
 public:
  void Update(std::span<const u8> data) noexcept;
  [[nodiscard]] u32 value() const noexcept { return value_ ^ 0xFFFFFFFFU; }

 private:
  u32 value_ = 0xFFFFFFFFU;
};

/// Big-endian minimal encoding of an unsigned integer, used by the canonical
/// encoder so that a digest never depends on host endianness.
void AppendBigEndian(std::vector<u8>& out, u64 value);

}  // namespace rcb

#endif  // RCB_DIGEST_HPP
