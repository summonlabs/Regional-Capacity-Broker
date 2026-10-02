// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Capacity dimensions and their exact units.
//
// A commitment is never a single flattened scalar. Facility power, cooling,
// rack space and service-class-qualified capacity are separate dimensions with
// separate units, and every dimension is accounted for independently. A number
// without its dimension is not a capacity.

#ifndef RCB_UNITS_HPP
#define RCB_UNITS_HPP

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

/// The dimensions this broker commits. Four independent dimensions, each with
/// its own unit; nothing in the runtime converts between them.
enum class Dimension : u8 {
  /// Facility power headroom, in watts.
  Power = 0,
  /// Heat rejection headroom, in watts of cooling.
  Cooling = 1,
  /// Physical rack slots, in whole racks.
  RackSpace = 2,
  /// Service-class-qualified capacity, in milli-units (10^-3 units) so that a
  /// fractional accelerator or VM can be committed exactly.
  ServiceCapacity = 3,
};

inline constexpr std::size_t kDimensionCount = 4;

/// Dimension order is part of the canonical encoding; never reorder it.
inline constexpr std::array<Dimension, kDimensionCount> kDimensions = {
    Dimension::Power, Dimension::Cooling, Dimension::RackSpace, Dimension::ServiceCapacity};

/// Stable token used in JSON and in canonical encodings.
std::string_view DimensionToken(Dimension dimension) noexcept;
/// Unit of the dimension, for rendering and for refusal evidence.
std::string_view DimensionUnit(Dimension dimension) noexcept;
Result<Dimension> ParseDimension(std::string_view token) noexcept;

/// Hard bounds. Every externally supplied size and every capacity is validated
/// against these before anything is allocated, summed or committed.
struct Limits {
  /// Largest value of a single dimension on a single tranche (about 1.1e12).
  static constexpr i64 kMaxDimensionValue = 1LL << 40;
  /// Largest sum of one dimension across all tranches of one offer.
  static constexpr i64 kMaxOfferTotal = 1LL << 44;
  /// Largest sum of one dimension across the whole ledger.
  static constexpr i64 kMaxLedgerTotal = 1LL << 46;
  /// Tranches in one offer.
  static constexpr std::size_t kMaxTranchesPerOffer = 1024;
  /// Live site ledgers.
  static constexpr std::size_t kMaxSites = 65536;
  /// Allocations produced by one decision.
  static constexpr std::size_t kMaxAllocationsPerDecision = 4096;
  /// Live commitments tracked across all sites and generations.
  static constexpr std::size_t kMaxLiveCommitments = 1 << 20;
  /// Identifier elements in one ask's locality lists.
  static constexpr std::size_t kMaxListElements = 256;
  /// Generation pins in one ask.
  static constexpr std::size_t kMaxGenerationPins = 256;
  /// Retained decisions for idempotent retries.
  static constexpr std::size_t kMaxRetainedDecisions = 1 << 20;
  /// Journal record payload.
  static constexpr std::size_t kMaxRecordPayloadBytes = 1 << 22;
  /// Snapshot file.
  static constexpr std::size_t kMaxSnapshotBytes = 1 << 28;
  /// Directory or file text read from the operator.
  static constexpr std::size_t kMaxDocumentBytes = 1 << 24;
  /// Depth of the JSON parser's nesting. This is the documented bound and the
  /// parser's default; the two are asserted to agree at compile time in
  /// src/json.cpp so that they can never drift apart.
  static constexpr std::size_t kMaxJsonDepth = 64;
};

/// A vector of exact integer quantities, one per dimension.
class CapacityVector {
 public:
  constexpr CapacityVector() noexcept = default;

  static Result<CapacityVector> Make(std::array<i64, kDimensionCount> values) noexcept;

  /// A vector with a single non-zero dimension.
  static Result<CapacityVector> FromDimension(Dimension dimension, i64 amount) noexcept;

  [[nodiscard]] constexpr i64 Get(Dimension dimension) const noexcept {
    return values_[static_cast<std::size_t>(dimension)];
  }
  constexpr void Set(Dimension dimension, i64 amount) noexcept {
    values_[static_cast<std::size_t>(dimension)] = amount;
  }

  [[nodiscard]] constexpr bool IsZero() const noexcept {
    for (const i64 value : values_) {
      if (value != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] constexpr bool IsNonNegative() const noexcept {
    for (const i64 value : values_) {
      if (value < 0) {
        return false;
      }
    }
    return true;
  }

  /// True when this vector is greater than or equal to \p other in every
  /// dimension.
  [[nodiscard]] constexpr bool Covers(const CapacityVector& other) const noexcept {
    for (std::size_t i = 0; i < kDimensionCount; ++i) {
      if (values_[i] < other.values_[i]) {
        return false;
      }
    }
    return true;
  }

  /// True when any dimension is strictly positive.
  [[nodiscard]] constexpr bool HasAny() const noexcept {
    for (const i64 value : values_) {
      if (value > 0) {
        return true;
      }
    }
    return false;
  }

  /// Exact checked sum. Both operands must be non-negative.
  static Result<CapacityVector> Add(const CapacityVector& a, const CapacityVector& b,
                                    i64 bound = Limits::kMaxLedgerTotal) noexcept;
  /// Exact checked difference. The result may be negative: refusing to compute
  /// an over-commitment would hide it.
  static Result<CapacityVector> Sub(const CapacityVector& a, const CapacityVector& b) noexcept;
  /// Exact checked difference that asserts the residual cannot be negative.
  /// A negative residual here is an invariant violation, not an input error.
  static Result<CapacityVector> SubNonNegative(const CapacityVector& a,
                                               const CapacityVector& b) noexcept;

  [[nodiscard]] constexpr CapacityVector Min(const CapacityVector& other) const noexcept {
    CapacityVector result;
    for (std::size_t i = 0; i < kDimensionCount; ++i) {
      result.values_[i] = values_[i] < other.values_[i] ? values_[i] : other.values_[i];
    }
    return result;
  }

  [[nodiscard]] constexpr CapacityVector Max(const CapacityVector& other) const noexcept {
    CapacityVector result;
    for (std::size_t i = 0; i < kDimensionCount; ++i) {
      result.values_[i] = values_[i] > other.values_[i] ? values_[i] : other.values_[i];
    }
    return result;
  }

  /// Total across all dimensions. Used only for ordering and reporting; it is
  /// never an accounting quantity.
  Result<i64> ScalarTotal() const noexcept;

  [[nodiscard]] std::array<i64, kDimensionCount> values() const noexcept { return values_; }

  /// "power=1,cooling=2,rack_space=3,service_capacity=4"; deterministic order.
  [[nodiscard]] std::string ToString() const;

  friend bool operator==(const CapacityVector& a, const CapacityVector& b) noexcept {
    return a.values_ == b.values_;
  }
  friend bool operator!=(const CapacityVector& a, const CapacityVector& b) noexcept {
    return !(a == b);
  }

 private:
  std::array<i64, kDimensionCount> values_{0, 0, 0, 0};
};

}  // namespace rcb

#endif  // RCB_UNITS_HPP
