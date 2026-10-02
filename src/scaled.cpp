// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/scaled.hpp"

#include <cstddef>
#include <limits>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"

namespace rcb {
namespace {

bool IsDigit(char c) noexcept { return c >= '0' && c <= '9'; }

}  // namespace

Result<ScaledAmount> ScaledAmount::FromMicros(i64 micros) noexcept {
  if (micros > kMaxMicros || micros < -kMaxMicros) {
    return Fail<ScaledAmount>(ErrorCode::OutOfRange,
                              "fixed-point amount exceeds the representable range");
  }
  ScaledAmount amount;
  amount.micros_ = micros;
  return amount;
}

Result<ScaledAmount> ScaledAmount::FromUnits(i64 units, i64 micros) noexcept {
  Result<i64> scaled = MulChecked(units, kMicrosPerUnit);
  if (!scaled.ok()) {
    return Result<ScaledAmount>(scaled.status());
  }
  Result<i64> total = AddChecked(scaled.value(), micros);
  if (!total.ok()) {
    return Result<ScaledAmount>(total.status());
  }
  return FromMicros(total.value());
}

Result<ScaledAmount> ScaledAmount::Parse(std::string_view text) noexcept {
  if (text.empty()) {
    return Fail<ScaledAmount>(ErrorCode::MalformedInput, "empty fixed-point amount");
  }
  std::size_t index = 0;
  bool negative = false;
  if (text[index] == '+' || text[index] == '-') {
    negative = text[index] == '-';
    ++index;
  }
  if (index >= text.size()) {
    return Fail<ScaledAmount>(ErrorCode::MalformedInput, "fixed-point amount has no digits");
  }

  i64 whole = 0;
  std::size_t whole_digits = 0;
  while (index < text.size() && IsDigit(text[index])) {
    const i64 digit = static_cast<i64>(text[index] - '0');
    Result<i64> next = MulChecked(whole, 10);
    if (!next.ok()) {
      return Fail<ScaledAmount>(ErrorCode::Overflow, "fixed-point whole part overflow");
    }
    Result<i64> shifted = AddChecked(next.value(), digit);
    if (!shifted.ok()) {
      return Fail<ScaledAmount>(ErrorCode::Overflow, "fixed-point whole part overflow");
    }
    whole = shifted.value();
    ++whole_digits;
    ++index;
    if (whole_digits > 15) {
      return Fail<ScaledAmount>(ErrorCode::OutOfRange, "fixed-point whole part has too many digits");
    }
  }
  if (whole_digits == 0) {
    return Fail<ScaledAmount>(ErrorCode::MalformedInput, "fixed-point amount has no whole digits");
  }

  i64 micros = 0;
  if (index < text.size() && text[index] == '.') {
    ++index;
    int digits = 0;
    while (index < text.size() && IsDigit(text[index])) {
      if (digits >= kFractionDigits) {
        return Fail<ScaledAmount>(ErrorCode::OutOfRange,
                                  "fixed-point amount has more precision than micro-units carry");
      }
      micros = micros * 10 + static_cast<i64>(text[index] - '0');
      ++digits;
      ++index;
    }
    if (digits == 0) {
      return Fail<ScaledAmount>(ErrorCode::MalformedInput, "fixed-point fraction has no digits");
    }
    while (digits < kFractionDigits) {
      micros *= 10;
      ++digits;
    }
  }
  if (index != text.size()) {
    return Fail<ScaledAmount>(ErrorCode::MalformedInput,
                              "fixed-point amount has trailing characters");
  }

  Result<ScaledAmount> magnitude = FromUnits(whole, micros);
  if (!magnitude.ok()) {
    return magnitude;
  }
  if (!negative) {
    return magnitude;
  }
  return FromMicros(-magnitude.value().micros());
}

Result<ScaledAmount> ScaledAmount::ScaleBy(i64 units) const noexcept {
  if (units < 0) {
    return Fail<ScaledAmount>(ErrorCode::InvalidArgument, "scaling by a negative quantity");
  }
  Result<i64> product = MulChecked(micros_, units);
  if (!product.ok()) {
    return Fail<ScaledAmount>(ErrorCode::Overflow,
                              "fixed-point product exceeds the representable range");
  }
  return FromMicros(product.value());
}

Result<ScaledAmount> ScaledAmount::Add(const ScaledAmount& a, const ScaledAmount& b) noexcept {
  Result<i64> total = AddChecked(a.micros_, b.micros_);
  if (!total.ok()) {
    return Result<ScaledAmount>(total.status());
  }
  return FromMicros(total.value());
}

Result<ScaledAmount> ScaledAmount::Sub(const ScaledAmount& a, const ScaledAmount& b) noexcept {
  Result<i64> total = SubChecked(a.micros_, b.micros_);
  if (!total.ok()) {
    return Result<ScaledAmount>(total.status());
  }
  return FromMicros(total.value());
}

int ScaledAmount::Compare(const ScaledAmount& a, const ScaledAmount& b) noexcept {
  if (a.micros_ == b.micros_) {
    return 0;
  }
  return a.micros_ < b.micros_ ? -1 : 1;
}

std::string ScaledAmount::ToString() const {
  const bool negative = micros_ < 0;
  // The magnitude of kMaxMicros always fits without overflowing when negated.
  const i64 magnitude = negative ? -micros_ : micros_;
  const i64 whole = magnitude / kMicrosPerUnit;
  const i64 fraction = magnitude % kMicrosPerUnit;

  std::string text;
  if (negative) {
    text.push_back('-');
  }
  text += std::to_string(whole);
  if (fraction != 0) {
    std::string digits = std::to_string(fraction);
    digits.insert(digits.begin(), static_cast<std::size_t>(kFractionDigits) - digits.size(), '0');
    while (!digits.empty() && digits.back() == '0') {
      digits.pop_back();
    }
    text.push_back('.');
    text += digits;
  }
  return text;
}

}  // namespace rcb
