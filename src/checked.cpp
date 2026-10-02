// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/checked.hpp"

namespace rcb {
namespace {

#if defined(__SIZEOF_INT128__)
// GCC and Clang spell the 128-bit unsigned integer __int128, which ISO C++
// does not have; -Wpedantic therefore fires on the spelling itself. This is the
// only diagnostic suppression in the library, it is scoped to this alias
// declaration, and every use site below writes Wide128 instead. Toolchains
// without the extension use the portable implementation that follows the
// #else, so no behaviour depends on the suppression.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpedantic"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
using Wide128 = unsigned __int128;
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#else
/// Portable 64x64 -> 128 product built from 32-bit limbs. The compiler
/// intrinsic path is used where it exists; this path is exact everywhere else.
Wide MultiplyWidePortable(u64 a, u64 b) noexcept {
  const u64 a0 = a & 0xFFFFFFFFULL;
  const u64 a1 = a >> 32U;
  const u64 b0 = b & 0xFFFFFFFFULL;
  const u64 b1 = b >> 32U;

  const u64 p00 = a0 * b0;
  const u64 p01 = a0 * b1;
  const u64 p10 = a1 * b0;
  const u64 p11 = a1 * b1;

  const u64 mid = (p00 >> 32U) + (p01 & 0xFFFFFFFFULL) + (p10 & 0xFFFFFFFFULL);

  Wide result;
  result.lo = (p00 & 0xFFFFFFFFULL) | (mid << 32U);
  result.hi = p11 + (p01 >> 32U) + (p10 >> 32U) + (mid >> 32U);
  return result;
}

/// Restoring division of a 128-bit magnitude by a 64-bit divisor. Exact for all
/// inputs; the quotient is reported as not fitting when it needs more than 64
/// bits.
struct WideDivResult {
  bool quotient_fits = true;
  u64 quotient = 0;
  u64 remainder = 0;
};

WideDivResult DivideWide(const Wide value, u64 divisor) noexcept {
  WideDivResult result;
  u64 remainder = 0;
  u64 quotient = 0;
  const u64 words[2] = {value.hi, value.lo};
  for (std::size_t w = 0; w < 2; ++w) {
    const u64 word = words[w];
    for (int bit = 63; bit >= 0; --bit) {
      const u64 next_bit = (word >> static_cast<unsigned>(bit)) & 1ULL;
      const bool overflow = (remainder >> 63U) != 0;
      remainder = (remainder << 1U) | next_bit;
      if (overflow || remainder >= divisor) {
        // When the doubling overflowed, the true remainder is 2^64 + remainder;
        // subtracting the divisor wraps to exactly the true difference.
        remainder -= divisor;
        if (w == 0) {
          result.quotient_fits = false;  // a quotient bit above 2^64 was needed
        } else {
          quotient |= (1ULL << static_cast<unsigned>(bit));
        }
      }
    }
  }
  result.quotient = quotient;
  result.remainder = remainder;
  return result;
}
#endif  // !__SIZEOF_INT128__

}  // namespace

Wide MultiplyWide(u64 a, u64 b) noexcept {
#if defined(__SIZEOF_INT128__)
  const Wide128 product = static_cast<Wide128>(a) * static_cast<Wide128>(b);
  Wide result;
  result.lo = static_cast<u64>(product);
  result.hi = static_cast<u64>(product >> 64U);
  return result;
#else
  return MultiplyWidePortable(a, b);
#endif
}

int CompareWide(const Wide a, const Wide b) noexcept {
  if (a.hi != b.hi) {
    return a.hi < b.hi ? -1 : 1;
  }
  if (a.lo != b.lo) {
    return a.lo < b.lo ? -1 : 1;
  }
  return 0;
}

Result<u64> MulDivU64(u64 a, u64 b, u64 c, u64* remainder) {
  if (c == 0) {
    return Fail<u64>(ErrorCode::OutOfRange, "division by zero in mul-div");
  }
  if (a == 0 || b == 0) {
    if (remainder != nullptr) {
      *remainder = 0;
    }
    return u64{0};
  }
#if defined(__SIZEOF_INT128__)
  const Wide128 product = static_cast<Wide128>(a) * static_cast<Wide128>(b);
  const Wide128 quotient = product / static_cast<Wide128>(c);
  if (quotient > static_cast<Wide128>(std::numeric_limits<u64>::max())) {
    return Fail<u64>(ErrorCode::Overflow, "mul-div quotient does not fit in 64 bits");
  }
  if (remainder != nullptr) {
    *remainder = static_cast<u64>(product - (quotient * static_cast<Wide128>(c)));
  }
  return static_cast<u64>(quotient);
#else
  const Wide product = MultiplyWide(a, b);
  const WideDivResult divided = DivideWide(product, c);
  if (!divided.quotient_fits) {
    return Fail<u64>(ErrorCode::Overflow, "mul-div quotient does not fit in 64 bits");
  }
  if (remainder != nullptr) {
    *remainder = divided.remainder;
  }
  return divided.quotient;
#endif
}

Result<i64> MulDivI64(i64 a, i64 b, i64 c, i64* remainder) {
  if (a < 0 || b < 0) {
    return Fail<i64>(ErrorCode::InvalidArgument, "mul-div requires non-negative multiplicands");
  }
  if (c <= 0) {
    return Fail<i64>(ErrorCode::InvalidArgument, "mul-div requires a positive divisor");
  }
  u64 wide_remainder = 0;
  Result<u64> quotient =
      MulDivU64(static_cast<u64>(a), static_cast<u64>(b), static_cast<u64>(c), &wide_remainder);
  if (!quotient.ok()) {
    return Result<i64>(quotient.status());
  }
  if (quotient.value() > static_cast<u64>(std::numeric_limits<i64>::max())) {
    return Fail<i64>(ErrorCode::Overflow, "mul-div quotient does not fit in 64 bits");
  }
  if (remainder != nullptr) {
    *remainder = static_cast<i64>(wide_remainder);
  }
  return static_cast<i64>(quotient.value());
}

Result<i64> AddChecked(i64 a, i64 b) {
  i64 result = 0;
  if (!TryAdd(a, b, &result)) {
    return Fail<i64>(ErrorCode::Overflow, "signed 64-bit addition overflow");
  }
  return result;
}

Result<i64> SubChecked(i64 a, i64 b) {
  i64 result = 0;
  if (!TrySub(a, b, &result)) {
    return Fail<i64>(ErrorCode::Overflow, "signed 64-bit subtraction overflow");
  }
  return result;
}

Result<i64> MulChecked(i64 a, i64 b) {
  i64 result = 0;
  if (!TryMul(a, b, &result)) {
    return Fail<i64>(ErrorCode::Overflow, "signed 64-bit multiplication overflow");
  }
  return result;
}

Result<u64> AddCheckedU64(u64 a, u64 b) {
  u64 result = 0;
  if (!TryAdd(a, b, &result)) {
    return Fail<u64>(ErrorCode::Overflow, "unsigned 64-bit addition overflow");
  }
  return result;
}

Result<i64> SumChecked(const i64* values, std::size_t count, i64 bound) {
  i64 total = 0;
  for (std::size_t i = 0; i < count; ++i) {
    if (values[i] < 0) {
      return Fail<i64>(ErrorCode::InvalidArgument, "negative value in a non-negative sum");
    }
    i64 next = 0;
    if (!TryAdd(total, values[i], &next)) {
      return Fail<i64>(ErrorCode::Overflow, "sum overflow");
    }
    if (next > bound) {
      return Fail<i64>(ErrorCode::LimitExceeded, "sum exceeds the bound for this quantity");
    }
    total = next;
  }
  return total;
}

}  // namespace rcb
