// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/digest.hpp"

#include <cstring>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"

namespace rcb {
namespace {

constexpr std::array<u32, 64> kSha256RoundConstants = {
    0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
    0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
    0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
    0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
    0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
    0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
    0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
    0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
    0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
    0xC67178F2U};

constexpr std::array<u32, 8> kSha256InitialState = {0x6A09E667U, 0xBB67AE85U, 0x3C6EF372U,
                                                    0xA54FF53AU, 0x510E527FU, 0x9B05688CU,
                                                    0x1F83D9ABU, 0x5BE0CD19U};

constexpr u32 RotateRight(u32 value, unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

constexpr u32 LoadBigEndian32(const u8* data) noexcept {
  return (static_cast<u32>(data[0]) << 24U) | (static_cast<u32>(data[1]) << 16U) |
         (static_cast<u32>(data[2]) << 8U) | static_cast<u32>(data[3]);
}

void StoreBigEndian32(u8* out, u32 value) noexcept {
  out[0] = static_cast<u8>((value >> 24U) & 0xFFU);
  out[1] = static_cast<u8>((value >> 16U) & 0xFFU);
  out[2] = static_cast<u8>((value >> 8U) & 0xFFU);
  out[3] = static_cast<u8>(value & 0xFFU);
}

const std::array<u32, 256>& Crc32cTable() noexcept {
  static const std::array<u32, 256> table = [] {
    std::array<u32, 256> entries{};
    for (u32 index = 0; index < 256U; ++index) {
      u32 value = index;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value & 1U) != 0U ? (value >> 1U) ^ 0x82F63B78U : (value >> 1U);
      }
      entries[index] = value;
    }
    return entries;
  }();
  return table;
}

int HexValue(char c) noexcept {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}  // namespace

Digest Digest::FromBytes(const std::array<u8, kSize>& bytes) noexcept {
  Digest digest;
  digest.bytes_ = bytes;
  return digest;
}

Result<Digest> Digest::FromHex(std::string_view text) {
  if (text.size() != kSize * 2U) {
    return Fail<Digest>(ErrorCode::InvalidArgument, "digest hex must be 64 characters");
  }
  Digest digest;
  for (std::size_t i = 0; i < kSize; ++i) {
    const int high = HexValue(text[i * 2U]);
    const int low = HexValue(text[i * 2U + 1U]);
    if (high < 0 || low < 0) {
      return Fail<Digest>(ErrorCode::InvalidArgument, "digest hex contains a non-hex character");
    }
    digest.bytes_[i] = static_cast<u8>((high << 4) | low);
  }
  return digest;
}

std::string Digest::Hex() const { return ShortHex(kSize * 2U); }

std::string Digest::ShortHex(std::size_t count) const {
  static constexpr char kHexDigits[] = "0123456789abcdef";
  const std::size_t available = kSize * 2U;
  const std::size_t limit = count < available ? count : available;
  std::string text;
  text.reserve(limit);
  for (std::size_t i = 0; i < limit; ++i) {
    const u8 byte = bytes_[i / 2U];
    const u8 nibble = (i % 2U == 0U) ? static_cast<u8>(byte >> 4U) : static_cast<u8>(byte & 0x0FU);
    text.push_back(kHexDigits[nibble]);
  }
  return text;
}

Sha256::Sha256() noexcept { Reset(); }

void Sha256::Reset() noexcept {
  state_ = kSha256InitialState;
  buffer_.fill(0);
  buffered_ = 0;
  total_bytes_ = 0;
}

void Sha256::Compress(const u8* block) noexcept {
  u32 schedule[64] = {};
  for (std::size_t i = 0; i < 16; ++i) {
    schedule[i] = LoadBigEndian32(block + i * 4U);
  }
  for (std::size_t i = 16; i < 64; ++i) {
    const u32 s0 = RotateRight(schedule[i - 15], 7U) ^ RotateRight(schedule[i - 15], 18U) ^
                   (schedule[i - 15] >> 3U);
    const u32 s1 = RotateRight(schedule[i - 2], 17U) ^ RotateRight(schedule[i - 2], 19U) ^
                   (schedule[i - 2] >> 10U);
    schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
  }

  u32 a = state_[0];
  u32 b = state_[1];
  u32 c = state_[2];
  u32 d = state_[3];
  u32 e = state_[4];
  u32 f = state_[5];
  u32 g = state_[6];
  u32 h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const u32 s1 = RotateRight(e, 6U) ^ RotateRight(e, 11U) ^ RotateRight(e, 25U);
    const u32 choice = (e & f) ^ ((~e) & g);
    const u32 temp1 = h + s1 + choice + kSha256RoundConstants[i] + schedule[i];
    const u32 s0 = RotateRight(a, 2U) ^ RotateRight(a, 13U) ^ RotateRight(a, 22U);
    const u32 majority = (a & b) ^ (a & c) ^ (b & c);
    const u32 temp2 = s0 + majority;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::Update(std::span<const u8> data) noexcept {
  total_bytes_ += static_cast<u64>(data.size());
  std::size_t offset = 0;
  if (buffered_ != 0) {
    const std::size_t wanted = 64U - buffered_;
    const std::size_t take = data.size() < wanted ? data.size() : wanted;
    std::memcpy(buffer_.data() + buffered_, data.data() + offset, take);
    buffered_ += take;
    offset += take;
    if (buffered_ == 64U) {
      Compress(buffer_.data());
      buffered_ = 0;
    }
  }
  while (data.size() - offset >= 64U) {
    Compress(data.data() + offset);
    offset += 64U;
  }
  if (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    std::memcpy(buffer_.data(), data.data() + offset, remaining);
    buffered_ = remaining;
  }
}

void Sha256::Update(std::string_view text) noexcept {
  Update(std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()));
}

void Sha256::UpdateByte(u8 value) noexcept { Update(std::span<const u8>(&value, 1)); }

Digest Sha256::Finalize() noexcept {
  const u64 bit_length = total_bytes_ * 8U;
  const u8 padding = 0x80U;
  Update(std::span<const u8>(&padding, 1));
  const u8 zero = 0x00U;
  while (buffered_ != 56U) {
    Update(std::span<const u8>(&zero, 1));
  }
  u8 length_bytes[8] = {};
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<u8>((bit_length >> ((7U - i) * 8U)) & 0xFFU);
  }
  Update(std::span<const u8>(length_bytes, 8));

  std::array<u8, Digest::kSize> bytes{};
  for (std::size_t i = 0; i < 8; ++i) {
    StoreBigEndian32(bytes.data() + i * 4U, state_[i]);
  }
  return Digest::FromBytes(bytes);
}

Digest Sha256Of(std::span<const u8> data) noexcept {
  Sha256 hasher;
  hasher.Update(data);
  return hasher.Finalize();
}

Digest Sha256Of(std::string_view text) noexcept {
  Sha256 hasher;
  hasher.Update(text);
  return hasher.Finalize();
}

u32 Crc32c(std::span<const u8> data) noexcept {
  const std::array<u32, 256>& table = Crc32cTable();
  u32 value = 0xFFFFFFFFU;
  for (const u8 byte : data) {
    value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
  }
  return value ^ 0xFFFFFFFFU;
}

void Crc32cStream::Update(std::span<const u8> data) noexcept {
  const std::array<u32, 256>& table = Crc32cTable();
  u32 value = value_;
  for (const u8 byte : data) {
    value = table[(value ^ byte) & 0xFFU] ^ (value >> 8U);
  }
  value_ = value;
}

void AppendBigEndian(std::vector<u8>& out, u64 value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out.push_back(static_cast<u8>((value >> ((7U - i) * 8U)) & 0xFFU));
  }
}

}  // namespace rcb
