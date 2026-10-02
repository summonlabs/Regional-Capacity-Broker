// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Checked arithmetic.
//
// Every capacity, count, cost, duration, sequence number and externally
// supplied size that can overflow is computed through this header. A wrapped
// capacity is worse than an error: the caller is told, and never handed a
// plausible-looking wrong number.

#ifndef RCB_CHECKED_HPP
#define RCB_CHECKED_HPP

#include <cstdint>
#include <limits>
#include <type_traits>

#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

/// Non-throwing checked add. Returns false and leaves \p out untouched on
/// overflow. Defined for the signed and unsigned integer types used here.
template <class T>
constexpr bool TryAdd(T a, T b, T* out) noexcept {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "integer types only");
  if constexpr (std::is_unsigned_v<T>) {
    if (a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return false;
    }
    *out = static_cast<T>(a + b);
    return true;
  } else {
    if (b > 0 && a > static_cast<T>(std::numeric_limits<T>::max() - b)) {
      return false;
    }
    if (b < 0 && a < static_cast<T>(std::numeric_limits<T>::min() - b)) {
      return false;
    }
    *out = static_cast<T>(a + b);
    return true;
  }
}

/// Non-throwing checked subtract.
template <class T>
constexpr bool TrySub(T a, T b, T* out) noexcept {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "integer types only");
  if constexpr (std::is_unsigned_v<T>) {
    if (a < b) {
      return false;
    }
    *out = static_cast<T>(a - b);
    return true;
  } else {
    if (b < 0 && a > static_cast<T>(std::numeric_limits<T>::max() + b)) {
      return false;
    }
    if (b > 0 && a < static_cast<T>(std::numeric_limits<T>::min() + b)) {
      return false;
    }
    *out = static_cast<T>(a - b);
    return true;
  }
}

/// Non-throwing checked multiply.
template <class T>
constexpr bool TryMul(T a, T b, T* out) noexcept {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "integer types only");
  using U = std::make_unsigned_t<T>;
  if constexpr (std::is_unsigned_v<T>) {
    if (b != 0 && a > static_cast<T>(std::numeric_limits<T>::max() / b)) {
      return false;
    }
    *out = static_cast<T>(a * b);
    return true;
  } else {
    if (a == 0 || b == 0) {
      *out = 0;
      return true;
    }
    const bool negative = (a < 0) != (b < 0);
    const U ua = (a < 0) ? static_cast<U>(U{0} - static_cast<U>(a)) : static_cast<U>(a);
    const U ub = (b < 0) ? static_cast<U>(U{0} - static_cast<U>(b)) : static_cast<U>(b);
    const U limit = negative ? static_cast<U>(U{0} - static_cast<U>(std::numeric_limits<T>::min()))
                             : static_cast<U>(std::numeric_limits<T>::max());
    if (ua > limit / ub) {
      return false;
    }
    const U product = static_cast<U>(ua * ub);
    *out = negative ? static_cast<T>(U{0} - product) : static_cast<T>(product);
    return true;
  }
}

/// An unsigned 128-bit magnitude, split for portability. The wide helpers exist
/// because proportional apportionment multiplies two bounded 40-bit quantities
/// and divides by a third, and because rational-free cost arithmetic needs the
/// exact product rather than a rounded one.
struct Wide {
  u64 hi = 0;
  u64 lo = 0;

  bool IsZero() const noexcept { return hi == 0 && lo == 0; }
  bool FitsU64() const noexcept { return hi == 0; }
};

/// Exact 64x64 -> 128 bit product.
Wide MultiplyWide(u64 a, u64 b) noexcept;

/// Three-way unsigned comparison of two wide magnitudes.
int CompareWide(const Wide a, const Wide b) noexcept;

/// Exact floor(a * b / c). p remainder, when supplied, receives a*b mod c.
/// Fails with ErrorCode::OutOfRange when c is zero, and with
/// ErrorCode::Overflow when the quotient does not fit in a u64.
Result<u64> MulDivU64(u64 a, u64 b, u64 c, u64* remainder = nullptr);

/// Exact floor(a * b / c) for non-negative a and b, positive c.
/// Fails with ErrorCode::InvalidArgument for a negative operand or a
/// non-positive c, and ErrorCode::Overflow when the quotient exceeds i64.
Result<i64> MulDivI64(i64 a, i64 b, i64 c, i64* remainder = nullptr);

// ---- Result-returning wrappers with stable detail text -------------------

Result<i64> AddChecked(i64 a, i64 b);
Result<i64> SubChecked(i64 a, i64 b);
Result<i64> MulChecked(i64 a, i64 b);
Result<u64> AddCheckedU64(u64 a, u64 b);

/// Narrowing cast that refuses to truncate.
template <class To, class From>
constexpr Result<To> CheckedCast(From value) noexcept {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>, "integer types only");
  if constexpr (std::is_signed_v<From> == std::is_signed_v<To>) {
    if (value < static_cast<From>(std::numeric_limits<To>::min()) ||
        value > static_cast<From>(std::numeric_limits<To>::max())) {
      return Fail<To>(ErrorCode::OutOfRange, "value does not fit the target type");
    }
    return static_cast<To>(value);
  } else if constexpr (std::is_signed_v<From>) {
    if (value < 0 || static_cast<std::make_unsigned_t<From>>(value) >
                         static_cast<std::make_unsigned_t<From>>(std::numeric_limits<To>::max())) {
      return Fail<To>(ErrorCode::OutOfRange, "value does not fit the target type");
    }
    return static_cast<To>(value);
  } else {
    if (value > static_cast<std::make_unsigned_t<From>>(std::numeric_limits<To>::max())) {
      return Fail<To>(ErrorCode::OutOfRange, "value does not fit the target type");
    }
    return static_cast<To>(value);
  }
}

/// Sum of a range of i64 with overflow detection and an explicit bound.
Result<i64> SumChecked(const i64* values, std::size_t count, i64 bound);

}  // namespace rcb

#endif  // RCB_CHECKED_HPP
