// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/identifier.hpp"

#include <array>
#include <cctype>

namespace rcb {
namespace {

bool IsAsciiAlphanumeric(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAllowedBodyCharacter(char c) noexcept {
  return IsAsciiAlphanumeric(c) || c == '.' || c == '_' || c == ':' || c == '-';
}

char ToUpperAscii(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

/// True for the classic Windows device names, which are unusable as file names
/// on that platform regardless of extension.
bool IsReservedDeviceName(std::string_view text) noexcept {
  static constexpr std::array<std::string_view, 22> kReserved = {
      "CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
      "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};

  std::string_view stem = text;
  const std::size_t dot = text.find('.');
  if (dot != std::string_view::npos) {
    stem = text.substr(0, dot);
  }
  if (stem.size() > 4) {
    return false;
  }
  std::string upper;
  upper.reserve(stem.size());
  for (const char c : stem) {
    upper.push_back(ToUpperAscii(c));
  }
  for (const std::string_view reserved : kReserved) {
    if (upper == reserved) {
      return true;
    }
  }
  return false;
}

}  // namespace

Result<Identifier> Identifier::Make(std::string_view text) {
  if (text.empty()) {
    return Fail<Identifier>(ErrorCode::InvalidIdentifier, "identifier is empty");
  }
  if (text.size() > kMaxLength) {
    return Fail<Identifier>(ErrorCode::InvalidIdentifier, "identifier is longer than 64 characters");
  }
  if (!IsAsciiAlphanumeric(text.front())) {
    return Fail<Identifier>(ErrorCode::InvalidIdentifier,
                            "identifier must start with an ASCII letter or digit");
  }
  for (const char c : text) {
    if (!IsAllowedBodyCharacter(c)) {
      return Fail<Identifier>(ErrorCode::InvalidIdentifier,
                              "identifier contains a character outside [A-Za-z0-9._:-]");
    }
  }
  if (text == "." || text == "..") {
    return Fail<Identifier>(ErrorCode::InvalidIdentifier, "identifier is a relative path element");
  }
  if (IsReservedDeviceName(text)) {
    return Fail<Identifier>(ErrorCode::InvalidIdentifier, "identifier is a reserved device name");
  }
  Identifier identifier;
  identifier.value_.assign(text);
  return identifier;
}

}  // namespace rcb
