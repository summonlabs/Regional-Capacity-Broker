// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/json.hpp"

#include <limits>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"
#include "rcb/units.hpp"

// The documented nesting bound and the parser's default must be the same
// number: a caller that validates against Limits and a parser that enforces
// something else would disagree about what is acceptable input.
static_assert(rcb::JsonLimits{}.max_depth == rcb::Limits::kMaxJsonDepth,
              "JsonLimits::max_depth and Limits::kMaxJsonDepth must agree");

namespace rcb {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void AppendCodePointUtf8(std::string& out, const u32 code_point) {
  if (code_point <= 0x7FU) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FFU) {
    out.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else if (code_point <= 0xFFFFU) {
    out.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
  }
}

struct DecodedCodePoint {
  bool ok = false;
  u32 value = 0;
  std::size_t width = 0;
  bool overlong = false;
  bool surrogate = false;
  bool too_large = false;
};

DecodedCodePoint DecodeUtf8At(const std::string_view text, const std::size_t offset) {
  DecodedCodePoint decoded;
  if (offset >= text.size()) {
    return decoded;
  }
  const u8 first = static_cast<u8>(text[offset]);
  if (first < 0x80U) {
    decoded.ok = true;
    decoded.value = first;
    decoded.width = 1;
    return decoded;
  }
  std::size_t width = 0;
  u32 value = 0;
  u32 minimum = 0;
  if ((first & 0xE0U) == 0xC0U) {
    width = 2;
    value = first & 0x1FU;
    minimum = 0x80U;
  } else if ((first & 0xF0U) == 0xE0U) {
    width = 3;
    value = first & 0x0FU;
    minimum = 0x800U;
  } else if ((first & 0xF8U) == 0xF0U) {
    width = 4;
    value = first & 0x07U;
    minimum = 0x10000U;
  } else {
    return decoded;
  }
  if (offset + width > text.size()) {
    return decoded;
  }
  for (std::size_t i = 1; i < width; ++i) {
    const u8 continuation = static_cast<u8>(text[offset + i]);
    if ((continuation & 0xC0U) != 0x80U) {
      return decoded;
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  decoded.ok = true;
  decoded.value = value;
  decoded.width = width;
  decoded.overlong = value < minimum;
  decoded.surrogate = value >= 0xD800U && value <= 0xDFFFU;
  decoded.too_large = value > 0x10FFFFU;
  return decoded;
}

class Parser {
 public:
  Parser(const std::string_view text, const JsonLimits& limits) : text_(text), limits_(limits) {}

  Result<JsonValue> Parse() {
    if (text_.size() > limits_.max_bytes) {
      return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON document is larger than the limit");
    }
    SkipWhitespace();
    Result<JsonValue> value = ParseValue(0);
    if (!value.ok()) {
      return value;
    }
    SkipWhitespace();
    if (offset_ != text_.size()) {
      return Fail<JsonValue>(ErrorCode::MalformedInput,
                             "trailing content after the JSON value at offset " +
                                 std::to_string(offset_));
    }
    return value;
  }

 private:
  void SkipWhitespace() {
    while (offset_ < text_.size()) {
      const char c = text_[offset_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++offset_;
      } else {
        break;
      }
    }
  }

  Result<JsonValue> ParseValue(const std::size_t depth) {
    if (depth > limits_.max_depth) {
      return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON nesting is deeper than the limit");
    }
    if (offset_ >= text_.size()) {
      return Fail<JsonValue>(ErrorCode::MalformedInput, "unexpected end of JSON document");
    }
    const char c = text_[offset_];
    switch (c) {
      case '{':
        return ParseObject(depth);
      case '[':
        return ParseArray(depth);
      case '"':
        return ParseString();
      case 't':
        return ParseLiteral("true", JsonValue::MakeBool(true));
      case 'f':
        return ParseLiteral("false", JsonValue::MakeBool(false));
      case 'n':
        return ParseLiteral("null", JsonValue());
      default:
        break;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return ParseNumber();
    }
    return Fail<JsonValue>(ErrorCode::MalformedInput,
                           std::string("unexpected character '") + c + "' in JSON document");
  }

  Result<JsonValue> ParseLiteral(const std::string_view literal, JsonValue value) {
    if (text_.compare(offset_, literal.size(), literal) != 0) {
      return Fail<JsonValue>(ErrorCode::MalformedInput, "invalid JSON literal");
    }
    offset_ += literal.size();
    return value;
  }

  Result<JsonValue> ParseNumber() {
    const std::size_t start = offset_;
    if (offset_ < text_.size() && text_[offset_] == '-') {
      ++offset_;
    }
    if (offset_ >= text_.size() || text_[offset_] < '0' || text_[offset_] > '9') {
      return Fail<JsonValue>(ErrorCode::MalformedInput, "JSON number has no digits");
    }
    if (text_[offset_] == '0' && offset_ + 1 < text_.size() && text_[offset_ + 1] >= '0' &&
        text_[offset_ + 1] <= '9') {
      return Fail<JsonValue>(ErrorCode::MalformedInput, "JSON number has a leading zero");
    }
    while (offset_ < text_.size() && text_[offset_] >= '0' && text_[offset_] <= '9') {
      ++offset_;
    }
    if (offset_ < text_.size() && (text_[offset_] == '.' || text_[offset_] == 'e' ||
                                   text_[offset_] == 'E')) {
      // Floating point carries no authority in this runtime, so it is refused
      // rather than parsed and rounded. Fixed-point evidence is carried as an
      // exact decimal string.
      return Fail<JsonValue>(ErrorCode::MalformedInput,
                             "floating-point JSON numbers are not accepted; use an exact integer "
                             "or an exact decimal string");
    }
    const std::string_view digits = text_.substr(start, offset_ - start);
    const bool negative = !digits.empty() && digits.front() == '-';
    const std::string_view magnitude = negative ? digits.substr(1) : digits;
    u64 value = 0;
    for (const char digit : magnitude) {
      u64 scaled = 0;
      if (!TryMul(value, static_cast<u64>(10), &scaled)) {
        return Fail<JsonValue>(ErrorCode::OutOfRange, "JSON integer is out of range");
      }
      u64 next = 0;
      if (!TryAdd(scaled, static_cast<u64>(digit - '0'), &next)) {
        return Fail<JsonValue>(ErrorCode::OutOfRange, "JSON integer is out of range");
      }
      value = next;
    }
    if (negative) {
      const u64 limit = static_cast<u64>(std::numeric_limits<i64>::max()) + 1ULL;
      if (value > limit) {
        return Fail<JsonValue>(ErrorCode::OutOfRange, "JSON integer is out of range");
      }
      if (value == limit) {
        return JsonValue::MakeInt(std::numeric_limits<i64>::min());
      }
      return JsonValue::MakeInt(-static_cast<i64>(value));
    }
    if (value > static_cast<u64>(std::numeric_limits<i64>::max())) {
      return JsonValue::MakeUint(value);
    }
    return JsonValue::MakeInt(static_cast<i64>(value));
  }

  Result<JsonValue> ParseString() {
    if (text_[offset_] != '"') {
      return Fail<JsonValue>(ErrorCode::MalformedInput, "expected a JSON string");
    }
    ++offset_;
    std::string out;
    while (true) {
      if (offset_ >= text_.size()) {
        return Fail<JsonValue>(ErrorCode::MalformedInput, "unterminated JSON string");
      }
      const unsigned char c = static_cast<unsigned char>(text_[offset_]);
      if (c == '"') {
        ++offset_;
        break;
      }
      if (c < 0x20U) {
        return Fail<JsonValue>(ErrorCode::MalformedInput,
                               "raw control character inside a JSON string");
      }
      if (c == '\\') {
        ++offset_;
        if (offset_ >= text_.size()) {
          return Fail<JsonValue>(ErrorCode::MalformedInput, "unterminated JSON escape");
        }
        const char escape = text_[offset_];
        switch (escape) {
          case '"': out.push_back('"'); ++offset_; break;
          case '\\': out.push_back('\\'); ++offset_; break;
          case '/': out.push_back('/'); ++offset_; break;
          case 'b': out.push_back('\b'); ++offset_; break;
          case 'f': out.push_back('\f'); ++offset_; break;
          case 'n': out.push_back('\n'); ++offset_; break;
          case 'r': out.push_back('\r'); ++offset_; break;
          case 't': out.push_back('\t'); ++offset_; break;
          case 'u': {
            ++offset_;
            Result<u32> first = ParseHex4();
            if (!first.ok()) {
              return Result<JsonValue>(first.status());
            }
            u32 code_point = first.value();
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if (offset_ + 1 >= text_.size() || text_[offset_] != '\\' ||
                  text_[offset_ + 1] != 'u') {
                return Fail<JsonValue>(ErrorCode::InvalidUnicode,
                                       "high surrogate is not followed by a low surrogate");
              }
              offset_ += 2;
              Result<u32> second = ParseHex4();
              if (!second.ok()) {
                return Result<JsonValue>(second.status());
              }
              if (second.value() < 0xDC00U || second.value() > 0xDFFFU) {
                return Fail<JsonValue>(ErrorCode::InvalidUnicode,
                                       "high surrogate is not followed by a low surrogate");
              }
              code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                           (second.value() - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              return Fail<JsonValue>(ErrorCode::InvalidUnicode, "unpaired low surrogate");
            }
            AppendCodePointUtf8(out, code_point);
            break;
          }
          default:
            return Fail<JsonValue>(ErrorCode::MalformedInput, "unknown JSON escape sequence");
        }
      } else if (c < 0x80U) {
        out.push_back(static_cast<char>(c));
        ++offset_;
      } else {
        const DecodedCodePoint decoded = DecodeUtf8At(text_, offset_);
        if (!decoded.ok || decoded.overlong || decoded.surrogate || decoded.too_large) {
          return Fail<JsonValue>(ErrorCode::InvalidUnicode,
                                 "JSON string contains invalid UTF-8");
        }
        out.append(text_.substr(offset_, decoded.width));
        offset_ += decoded.width;
      }
      if (out.size() > limits_.max_string_bytes) {
        return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON string is larger than the limit");
      }
    }
    return JsonValue::MakeString(std::move(out));
  }

  Result<u32> ParseHex4() {
    if (offset_ + 4 > text_.size()) {
      return Fail<u32>(ErrorCode::MalformedInput, "truncated \\u escape");
    }
    u32 value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
      const char c = text_[offset_ + i];
      u32 digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<u32>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<u32>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        digit = static_cast<u32>(c - 'A' + 10);
      } else {
        return Fail<u32>(ErrorCode::MalformedInput, "invalid hex digit in a \\u escape");
      }
      value = (value << 4U) | digit;
    }
    offset_ += 4;
    return value;
  }

  Result<JsonValue> ParseArray(const std::size_t depth) {
    ++offset_;  // '['
    JsonValue array = JsonValue::MakeArray();
    SkipWhitespace();
    if (offset_ < text_.size() && text_[offset_] == ']') {
      ++offset_;
      return array;
    }
    while (true) {
      SkipWhitespace();
      Result<JsonValue> item = ParseValue(depth + 1);
      if (!item.ok()) {
        return item;
      }
      array.Push(std::move(item.value()));
      if (array.items().size() > limits_.max_elements) {
        return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON array is larger than the limit");
      }
      SkipWhitespace();
      if (offset_ >= text_.size()) {
        return Fail<JsonValue>(ErrorCode::MalformedInput, "unterminated JSON array");
      }
      if (text_[offset_] == ',') {
        ++offset_;
        continue;
      }
      if (text_[offset_] == ']') {
        ++offset_;
        return array;
      }
      return Fail<JsonValue>(ErrorCode::MalformedInput, "expected ',' or ']' in a JSON array");
    }
  }

  Result<JsonValue> ParseObject(const std::size_t depth) {
    ++offset_;  // '{'
    JsonValue object = JsonValue::MakeObject();
    SkipWhitespace();
    if (offset_ < text_.size() && text_[offset_] == '}') {
      ++offset_;
      return object;
    }
    while (true) {
      SkipWhitespace();
      if (offset_ >= text_.size() || text_[offset_] != '"') {
        return Fail<JsonValue>(ErrorCode::MalformedInput, "expected a JSON object key");
      }
      Result<JsonValue> key = ParseString();
      if (!key.ok()) {
        return key;
      }
      if (key.value().AsString().size() > limits_.max_key_bytes) {
        return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON object key is longer than the "
                                                        "limit");
      }
      SkipWhitespace();
      if (offset_ >= text_.size() || text_[offset_] != ':') {
        return Fail<JsonValue>(ErrorCode::MalformedInput, "expected ':' after a JSON object key");
      }
      ++offset_;
      SkipWhitespace();
      Result<JsonValue> value = ParseValue(depth + 1);
      if (!value.ok()) {
        return value;
      }
      if (!object.Set(key.value().AsString(), std::move(value.value()))) {
        return Fail<JsonValue>(ErrorCode::DuplicateField,
                               "duplicate JSON object key '" + key.value().AsString() + "'");
      }
      if (object.members().size() > limits_.max_elements) {
        return Fail<JsonValue>(ErrorCode::LimitExceeded, "JSON object is larger than the limit");
      }
      SkipWhitespace();
      if (offset_ >= text_.size()) {
        return Fail<JsonValue>(ErrorCode::MalformedInput, "unterminated JSON object");
      }
      if (text_[offset_] == ',') {
        ++offset_;
        continue;
      }
      if (text_[offset_] == '}') {
        ++offset_;
        return object;
      }
      return Fail<JsonValue>(ErrorCode::MalformedInput, "expected ',' or '}' in a JSON object");
    }
  }

  std::string_view text_;
  JsonLimits limits_;
  std::size_t offset_ = 0;
};

void WriteEscaped(std::string& out, const std::string_view text) {
  out.push_back('"');
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20U) {
          out += "\\u00";
          out.push_back(kHexDigits[(c >> 4U) & 0x0FU]);
          out.push_back(kHexDigits[c & 0x0FU]);
        } else {
          out.push_back(raw);
        }
        break;
    }
  }
  out.push_back('"');
}

void WriteValue(std::string& out, const JsonValue& value, const bool pretty, const std::size_t depth) {
  const auto indent = [&out, pretty, depth]() {
    if (pretty) {
      out.push_back('\n');
      out.append(depth * 2U, ' ');
    }
  };
  switch (value.kind()) {
    case JsonValue::Kind::Null:
      out += "null";
      return;
    case JsonValue::Kind::Bool:
      out += value.AsBool() ? "true" : "false";
      return;
    case JsonValue::Kind::Int:
      out += std::to_string(value.AsInt());
      return;
    case JsonValue::Kind::Uint:
      out += std::to_string(value.AsUint());
      return;
    case JsonValue::Kind::String:
      WriteEscaped(out, value.AsString());
      return;
    case JsonValue::Kind::Array: {
      if (value.items().empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      bool first = true;
      for (const JsonValue& item : value.items()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        indent();
        WriteValue(out, item, pretty, depth + 1);
      }
      indent();
      out.push_back(']');
      return;
    }
    case JsonValue::Kind::Object: {
      if (value.members().empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      bool first = true;
      for (const auto& member : value.members()) {
        if (!first) {
          out.push_back(',');
        }
        first = false;
        indent();
        WriteEscaped(out, member.first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        WriteValue(out, member.second, pretty, depth + 1);
      }
      indent();
      out.push_back('}');
      return;
    }
  }
}

}  // namespace

JsonValue JsonValue::MakeBool(const bool value) {
  JsonValue result;
  result.kind_ = Kind::Bool;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::MakeInt(const i64 value) {
  JsonValue result;
  result.kind_ = Kind::Int;
  result.integer_ = value;
  return result;
}

JsonValue JsonValue::MakeUint(const u64 value) {
  JsonValue result;
  result.kind_ = Kind::Uint;
  result.unsigned_ = value;
  return result;
}

JsonValue JsonValue::MakeString(std::string value) {
  JsonValue result;
  result.kind_ = Kind::String;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::MakeArray() {
  JsonValue result;
  result.kind_ = Kind::Array;
  return result;
}

JsonValue JsonValue::MakeObject() {
  JsonValue result;
  result.kind_ = Kind::Object;
  return result;
}

const JsonValue* JsonValue::Find(const std::string_view key) const {
  if (kind_ != Kind::Object) {
    return nullptr;
  }
  const auto found = members_.find(std::string(key));
  return found == members_.end() ? nullptr : &found->second;
}

bool JsonValue::Set(std::string key, JsonValue value) {
  RCB_ASSERT(kind_ == Kind::Object);
  return members_.emplace(std::move(key), std::move(value)).second;
}

void JsonValue::Push(JsonValue value) {
  RCB_ASSERT(kind_ == Kind::Array);
  items_.push_back(std::move(value));
}

std::string JsonValue::ToText(const bool pretty) const {
  std::string out;
  WriteValue(out, *this, pretty, 0);
  if (pretty) {
    out.push_back('\n');
  }
  return out;
}

Result<JsonValue> ParseJson(const std::string_view text, const JsonLimits& limits) {
  Parser parser(text, limits);
  return parser.Parse();
}

bool IsValidUtf8(const std::string_view text) noexcept {
  std::size_t offset = 0;
  while (offset < text.size()) {
    const DecodedCodePoint decoded = DecodeUtf8At(text, offset);
    if (!decoded.ok || decoded.overlong || decoded.surrogate || decoded.too_large) {
      return false;
    }
    offset += decoded.width;
  }
  return true;
}

}  // namespace rcb
