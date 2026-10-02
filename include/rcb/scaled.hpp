// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Exact fixed-point cost and energy evidence.
//
// Money, energy and carbon evidence in this runtime is exact fixed point, not
// floating point: a value is a signed integer count of micro-units (10^-6) of
// the declared unit, and it serialises as an exact decimal string. Every
// operation is checked; an operation whose exact result cannot be represented
// fails with ErrorCode::Overflow rather than rounding. There is no rounding
// mode anywhere in this header, and no conversion to or from a floating-point
// type.

#ifndef RCB_SCALED_HPP
#define RCB_SCALED_HPP

#include <string>
#include <string_view>

#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

/// A signed exact fixed-point amount in 10^-6 units.
class ScaledAmount {
 public:
  /// One whole unit, expressed in micro-units.
  static constexpr i64 kMicrosPerUnit = 1000000;
  /// Decimal digits carried after the point.
  static constexpr int kFractionDigits = 6;
  /// Largest magnitude accepted, so that any product with a bounded capacity
  /// stays inside checked 64-bit arithmetic and beyond it fails loudly.
  static constexpr i64 kMaxMicros = 1LL << 40;

  constexpr ScaledAmount() noexcept = default;

  /// Exact construction from micro-units. Fails when the magnitude exceeds
  /// kMaxMicros.
  static Result<ScaledAmount> FromMicros(i64 micros) noexcept;

  /// Exact construction from whole units and a micro-unit remainder, both
  /// constrained so that the sum cannot overflow.
  static Result<ScaledAmount> FromUnits(i64 units, i64 micros = 0) noexcept;

  /// Exact decimal parse: an optional sign, digits, and an optional fraction of
  /// at most kFractionDigits digits. No exponent, no locale, no float. A value
  /// with more precision than can be represented is rejected, never rounded.
  static Result<ScaledAmount> Parse(std::string_view text) noexcept;

  [[nodiscard]] constexpr i64 micros() const noexcept { return micros_; }
  [[nodiscard]] constexpr bool IsZero() const noexcept { return micros_ == 0; }
  [[nodiscard]] constexpr bool IsNegative() const noexcept { return micros_ < 0; }
  [[nodiscard]] constexpr bool IsPositive() const noexcept { return micros_ > 0; }

  /// Exact product with a non-negative integer count of units. This is the
  /// price-times-quantity operation used to price a commitment; it fails rather
  /// than rounds.
  [[nodiscard]] Result<ScaledAmount> ScaleBy(i64 units) const noexcept;

  static Result<ScaledAmount> Add(const ScaledAmount& a, const ScaledAmount& b) noexcept;
  static Result<ScaledAmount> Sub(const ScaledAmount& a, const ScaledAmount& b) noexcept;

  /// -1, 0 or 1.
  static int Compare(const ScaledAmount& a, const ScaledAmount& b) noexcept;

  /// Canonical exact decimal, e.g. "12.5", "-0.000001", "0".
  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const ScaledAmount& a, const ScaledAmount& b) noexcept {
    return a.micros_ == b.micros_;
  }
  friend bool operator!=(const ScaledAmount& a, const ScaledAmount& b) noexcept { return !(a == b); }
  friend bool operator<(const ScaledAmount& a, const ScaledAmount& b) noexcept {
    return a.micros_ < b.micros_;
  }
  friend bool operator>(const ScaledAmount& a, const ScaledAmount& b) noexcept { return b < a; }
  friend bool operator<=(const ScaledAmount& a, const ScaledAmount& b) noexcept { return !(b < a); }
  friend bool operator>=(const ScaledAmount& a, const ScaledAmount& b) noexcept { return !(a < b); }

 private:
  i64 micros_ = 0;
};

}  // namespace rcb

#endif  // RCB_SCALED_HPP
