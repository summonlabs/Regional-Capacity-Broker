// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A strict, first-party JSON subset used for operator input, CLI output, state
// snapshots and canonical encodings.
//
// Strict means: no floating point (a number with a fraction or an exponent is
// refused, never rounded), no duplicate object keys, no trailing content, no
// unpaired surrogates, no invalid UTF-8, no control characters in strings, and
// hard limits on depth, size and element count that are checked before anything
// is allocated. Object members are held in a sorted map, so the canonical
// rendering of a value does not depend on input order.

#ifndef RCB_JSON_HPP
#define RCB_JSON_HPP

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

class JsonValue {
 public:
  enum class Kind : u8 { Null = 0, Bool = 1, Int = 2, Uint = 3, String = 4, Array = 5, Object = 6 };

  JsonValue() = default;

  static JsonValue MakeBool(bool value);
  static JsonValue MakeInt(i64 value);
  static JsonValue MakeUint(u64 value);
  static JsonValue MakeString(std::string value);
  static JsonValue MakeArray();
  static JsonValue MakeObject();

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] bool IsNull() const noexcept { return kind_ == Kind::Null; }
  [[nodiscard]] bool IsBool() const noexcept { return kind_ == Kind::Bool; }
  [[nodiscard]] bool IsInt() const noexcept { return kind_ == Kind::Int; }
  [[nodiscard]] bool IsUint() const noexcept { return kind_ == Kind::Uint; }
  [[nodiscard]] bool IsNumber() const noexcept { return IsInt() || IsUint(); }
  [[nodiscard]] bool IsString() const noexcept { return kind_ == Kind::String; }
  [[nodiscard]] bool IsArray() const noexcept { return kind_ == Kind::Array; }
  [[nodiscard]] bool IsObject() const noexcept { return kind_ == Kind::Object; }

  [[nodiscard]] bool AsBool() const noexcept { return boolean_; }
  [[nodiscard]] i64 AsInt() const noexcept { return integer_; }
  [[nodiscard]] u64 AsUint() const noexcept { return unsigned_; }
  [[nodiscard]] const std::string& AsString() const noexcept { return string_; }

  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
  [[nodiscard]] std::vector<JsonValue>& items() noexcept { return items_; }
  [[nodiscard]] const std::map<std::string, JsonValue>& members() const noexcept { return members_; }

  /// Member lookup; nullptr when the key is absent or this is not an object.
  [[nodiscard]] const JsonValue* Find(std::string_view key) const;

  /// Object insertion. Returns false when the key already exists, so a
  /// duplicate is always the caller's explicit decision.
  bool Set(std::string key, JsonValue value);
  /// Array append.
  void Push(JsonValue value);

  /// Deterministic rendering: object keys in byte order, no insignificant
  /// whitespace unless \p pretty is requested.
  [[nodiscard]] std::string ToText(bool pretty = false) const;

 private:
  Kind kind_ = Kind::Null;
  bool boolean_ = false;
  i64 integer_ = 0;
  u64 unsigned_ = 0;
  std::string string_;
  std::vector<JsonValue> items_;
  std::map<std::string, JsonValue> members_;
};

/// Bounds applied before anything is allocated or recursed into. The depth
/// default is the documented bound `rcb::Limits::kMaxJsonDepth`; a static
/// assertion in src/json.cpp keeps the two from drifting apart.
struct JsonLimits {
  std::size_t max_bytes = 16U * 1024U * 1024U;
  std::size_t max_depth = 64;
  std::size_t max_elements = 1U << 20;
  std::size_t max_string_bytes = 1U << 20;
  std::size_t max_key_bytes = 256;
};

/// Parses one complete JSON value. Fails with ErrorCode::MalformedInput for
/// anything outside the accepted subset, and ErrorCode::LimitExceeded when a
/// bound is exceeded.
Result<JsonValue> ParseJson(std::string_view text, const JsonLimits& limits = JsonLimits());

/// Validates that a byte string is well-formed UTF-8.
bool IsValidUtf8(std::string_view text) noexcept;

}  // namespace rcb

#endif  // RCB_JSON_HPP
