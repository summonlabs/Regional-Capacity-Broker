// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/units.hpp"

#include "rcb/checked.hpp"

namespace rcb {

std::string_view DimensionToken(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Power: return "power";
    case Dimension::Cooling: return "cooling";
    case Dimension::RackSpace: return "rack_space";
    case Dimension::ServiceCapacity: return "service_capacity";
  }
  return "unknown";
}

std::string_view DimensionUnit(Dimension dimension) noexcept {
  switch (dimension) {
    case Dimension::Power: return "W";
    case Dimension::Cooling: return "W";
    case Dimension::RackSpace: return "rack";
    case Dimension::ServiceCapacity: return "milli_unit";
  }
  return "unknown";
}

Result<Dimension> ParseDimension(std::string_view token) noexcept {
  for (const Dimension dimension : kDimensions) {
    if (DimensionToken(dimension) == token) {
      return dimension;
    }
  }
  return Fail<Dimension>(ErrorCode::InvalidArgument, "unknown capacity dimension token");
}

Result<CapacityVector> CapacityVector::Make(std::array<i64, kDimensionCount> values) noexcept {
  CapacityVector vector;
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    if (values[i] < 0) {
      return Fail<CapacityVector>(ErrorCode::InvalidArgument,
                                  "negative value in a capacity vector");
    }
    if (values[i] > Limits::kMaxDimensionValue) {
      return Fail<CapacityVector>(ErrorCode::OutOfRange,
                                  "capacity value exceeds the per-dimension bound");
    }
    vector.values_[i] = values[i];
  }
  return vector;
}

Result<CapacityVector> CapacityVector::FromDimension(Dimension dimension, i64 amount) noexcept {
  if (amount < 0) {
    return Fail<CapacityVector>(ErrorCode::InvalidArgument, "negative capacity value");
  }
  if (amount > Limits::kMaxDimensionValue) {
    return Fail<CapacityVector>(ErrorCode::OutOfRange,
                                "capacity value exceeds the per-dimension bound");
  }
  CapacityVector vector;
  vector.Set(dimension, amount);
  return vector;
}

Result<CapacityVector> CapacityVector::Add(const CapacityVector& a, const CapacityVector& b,
                                           i64 bound) noexcept {
  CapacityVector result;
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    Result<i64> sum = AddChecked(a.values_[i], b.values_[i]);
    if (!sum.ok()) {
      return Result<CapacityVector>(sum.status());
    }
    if (sum.value() > bound) {
      return Fail<CapacityVector>(ErrorCode::LimitExceeded,
                                  "capacity total exceeds the bound for this quantity");
    }
    result.values_[i] = sum.value();
  }
  return result;
}

Result<CapacityVector> CapacityVector::Sub(const CapacityVector& a, const CapacityVector& b) noexcept {
  CapacityVector result;
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    Result<i64> difference = SubChecked(a.values_[i], b.values_[i]);
    if (!difference.ok()) {
      return Result<CapacityVector>(difference.status());
    }
    result.values_[i] = difference.value();
  }
  return result;
}

Result<CapacityVector> CapacityVector::SubNonNegative(const CapacityVector& a,
                                                      const CapacityVector& b) noexcept {
  Result<CapacityVector> difference = Sub(a, b);
  if (!difference.ok()) {
    return difference;
  }
  if (!difference.value().IsNonNegative()) {
    return Fail<CapacityVector>(
        ErrorCode::InvariantViolation,
        "residual capacity would be negative; the ledger and the request disagree");
  }
  return difference;
}

Result<i64> CapacityVector::ScalarTotal() const noexcept {
  i64 total = 0;
  for (const i64 value : values_) {
    Result<i64> sum = AddChecked(total, value);
    if (!sum.ok()) {
      return Result<i64>(sum.status());
    }
    total = sum.value();
  }
  return total;
}

std::string CapacityVector::ToString() const {
  std::string text;
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    if (i != 0) {
      text.push_back(',');
    }
    text += DimensionToken(kDimensions[i]);
    text.push_back('=');
    text += std::to_string(values_[i]);
  }
  return text;
}

}  // namespace rcb
