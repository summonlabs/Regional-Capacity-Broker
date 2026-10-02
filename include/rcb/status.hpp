// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Status and error identity.
//
// The error code and its stable token are part of the public contract; the
// human-readable detail string is not. Codes are deliberately fine-grained:
// collapsing invalid, unsupported, stale, unknown, conflicting and
// indeterminate into one bucket would lose semantics that callers act on.

#ifndef RCB_STATUS_HPP
#define RCB_STATUS_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "rcb/assert.hpp"

namespace rcb {

/// The error taxonomy. Format: X(Enumerator, stable_token, category).
#define RCB_ERROR_CODE_TABLE(X)                                            \
  X(Ok, ok, Ok)                                                            \
  X(InvalidArgument, invalid_argument, Invalid)                            \
  X(InvalidIdentifier, invalid_identifier, Invalid)                        \
  X(InvalidRequest, invalid_request, Invalid)                              \
  X(MalformedInput, malformed_input, Invalid)                              \
  X(UnknownField, unknown_field, Invalid)                                  \
  X(MissingField, missing_field, Invalid)                                  \
  X(DuplicateField, duplicate_field, Invalid)                              \
  X(InvalidUnicode, invalid_unicode, Invalid)                              \
  X(Overflow, overflow, Invalid)                                           \
  X(OutOfRange, out_of_range, Invalid)                                     \
  X(LimitExceeded, limit_exceeded, Exhausted)                              \
  X(TooLarge, too_large, Exhausted)                                        \
  X(NotFound, not_found, NotFound)                                         \
  X(AlreadyExists, already_exists, Conflict)                               \
  X(DuplicateIdentity, duplicate_identity, Conflict)                       \
  X(Conflict, conflict, Conflict)                                          \
  X(IdempotencyConflict, idempotency_conflict, Conflict)                   \
  X(UnknownSite, unknown_site, NotFound)                                   \
  X(UnknownOffer, unknown_offer, NotFound)                                 \
  X(UnknownServiceClass, unknown_service_class, NotFound)                  \
  X(StaleGeneration, stale_generation, Stale)                              \
  X(StaleAuthority, stale_authority, Stale)                                \
  X(StaleOffer, stale_offer, Stale)                                        \
  X(ExpiredOffer, expired_offer, Stale)                                    \
  X(RevokedOffer, revoked_offer, Stale)                                    \
  X(SupersededOffer, superseded_offer, Stale)                              \
  X(Fenced, fenced, Stale)                                                 \
  X(Unsupported, unsupported, Unsupported)                                 \
  X(UnsupportedVersion, unsupported_version, Unsupported)                  \
  X(LocalityUnsatisfied, locality_unsatisfied, Unmet)                      \
  X(JurisdictionExcluded, jurisdiction_excluded, Unmet)                    \
  X(SiteExcluded, site_excluded, Unmet)                                    \
  X(DiversityUnsatisfied, diversity_unsatisfied, Unmet)                    \
  X(RiskCeilingExceeded, risk_ceiling_exceeded, Unmet)                     \
  X(CostCeilingExceeded, cost_ceiling_exceeded, Unmet)                     \
  X(EnergyCeilingExceeded, energy_ceiling_exceeded, Unmet)                 \
  X(ReserveNotAuthorized, reserve_not_authorized, Unmet)                   \
  X(ReservePolicyMismatch, reserve_policy_mismatch, Unmet)                 \
  X(PolicyGenerationMismatch, policy_generation_mismatch, Unmet)           \
  X(RiskUnsatisfied, risk_unsatisfied, Unmet)                              \
  X(InsufficientCapacity, insufficient_capacity, Unmet)                    \
  X(AllOrNothingUnsatisfiable, all_or_nothing_unsatisfiable, Unmet)        \
  X(SingleSourceUnsatisfiable, single_source_unsatisfiable, Unmet)         \
  X(NoEligibleOffer, no_eligible_offer, Unmet)                             \
  X(SiteLimitUnsatisfied, site_limit_unsatisfied, Unmet)                   \
  X(CapacityShrinkEviction, capacity_shrink_eviction, Unmet)               \
  X(PersistenceCorrupt, persistence_corrupt, Persistence)                  \
  X(PersistenceInteriorCorruption, persistence_interior_corruption, Persistence) \
  X(PersistenceTornTail, persistence_torn_tail, Persistence)               \
  X(PersistenceIoError, persistence_io_error, Persistence)                 \
  X(PersistenceFenced, persistence_fenced, Persistence)                    \
  X(PersistenceVersionMismatch, persistence_version_mismatch, Persistence) \
  X(ShuttingDown, shutting_down, Lifecycle)                                \
  X(QueueFull, queue_full, Lifecycle)                                      \
  X(Cancelled, cancelled, Lifecycle)                                       \
  X(CancelledAfterCommit, cancelled_after_commit, Lifecycle)               \
  X(InvariantViolation, invariant_violation, Invariant)                    \
  X(Indeterminate, indeterminate, Indeterminate)                           \
  X(Internal, internal, Internal)

enum class ErrorCode : std::uint16_t {
#define RCB_ERROR_ENUM(name, token, category) name,
  RCB_ERROR_CODE_TABLE(RCB_ERROR_ENUM)
#undef RCB_ERROR_ENUM
};

/// Coarse class of an error, for callers that branch on the kind of failure
/// rather than the exact code.
enum class ErrorCategory : std::uint8_t {
  Ok,
  Invalid,
  Unmet,
  Conflict,
  Stale,
  NotFound,
  Unsupported,
  Exhausted,
  Persistence,
  Lifecycle,
  Invariant,
  Indeterminate,
  Internal,
};

/// The stable machine token of an error code, e.g. "stale_generation". Tokens
/// are public contract: they appear in CLI output and are asserted by tests.
constexpr std::string_view ErrorToken(ErrorCode code) noexcept {
  switch (code) {
#define RCB_ERROR_TOKEN(name, token, category) \
  case ErrorCode::name:                        \
    return #token;
    RCB_ERROR_CODE_TABLE(RCB_ERROR_TOKEN)
#undef RCB_ERROR_TOKEN
  }
  return "internal";
}

constexpr ErrorCategory ErrorCategoryOf(ErrorCode code) noexcept {
  switch (code) {
#define RCB_ERROR_CATEGORY(name, token, category) \
  case ErrorCode::name:                           \
    return ErrorCategory::category;
    RCB_ERROR_CODE_TABLE(RCB_ERROR_CATEGORY)
#undef RCB_ERROR_CATEGORY
  }
  return ErrorCategory::Internal;
}

constexpr std::string_view ErrorCategoryToken(ErrorCategory category) noexcept {
  switch (category) {
    case ErrorCategory::Ok: return "ok";
    case ErrorCategory::Invalid: return "invalid";
    case ErrorCategory::Unmet: return "unmet";
    case ErrorCategory::Conflict: return "conflict";
    case ErrorCategory::Stale: return "stale";
    case ErrorCategory::NotFound: return "not_found";
    case ErrorCategory::Unsupported: return "unsupported";
    case ErrorCategory::Exhausted: return "exhausted";
    case ErrorCategory::Persistence: return "persistence";
    case ErrorCategory::Lifecycle: return "lifecycle";
    case ErrorCategory::Invariant: return "invariant";
    case ErrorCategory::Indeterminate: return "indeterminate";
    case ErrorCategory::Internal: return "internal";
  }
  return "internal";
}

/// True when repeating the identical operation could plausibly succeed later
/// without changing any input: a full queue, a shutting-down service, a fenced
/// generation that a newer publication may resolve.
constexpr bool IsRetryable(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::QueueFull:
    case ErrorCode::ShuttingDown:
    case ErrorCode::StaleGeneration:
    case ErrorCode::StaleOffer:
    case ErrorCode::SupersededOffer:
    case ErrorCode::Fenced:
    case ErrorCode::Cancelled:
      return true;
    default:
      return false;
  }
}

/// True when the error reports a constraint that refused a request, as opposed
/// to a malformed request or a runtime failure.
constexpr bool IsConstraintFailure(ErrorCode code) noexcept {
  return ErrorCategoryOf(code) == ErrorCategory::Unmet;
}

/// A success marker or a failure code plus non-contractual prose.
class Status {
 public:
  Status() noexcept = default;

  static Status Ok() noexcept { return Status(); }

  static Status Error(ErrorCode code, std::string detail) {
    RCB_ASSERT(code != ErrorCode::Ok);
    Status status;
    status.code_ = code;
    status.detail_ = std::move(detail);
    return status;
  }

  static Status Error(ErrorCode code) { return Error(code, std::string()); }

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] ErrorCode code() const noexcept { return code_; }
  [[nodiscard]] ErrorCategory category() const noexcept { return ErrorCategoryOf(code_); }
  [[nodiscard]] std::string_view token() const noexcept { return ErrorToken(code_); }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  /// "token: detail" (or just "token" when there is no detail).
  [[nodiscard]] std::string ToString() const;

  void SetDetail(std::string detail) { detail_ = std::move(detail); }

  friend bool operator==(const Status& lhs, const Status& rhs) {
    return lhs.code_ == rhs.code_ && lhs.detail_ == rhs.detail_;
  }
  friend bool operator!=(const Status& lhs, const Status& rhs) { return !(lhs == rhs); }

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string detail_;
};

/// A value or a failure. A Result never holds Ok without a value.
template <class T>
class Result {
 public:
  static_assert(!std::is_reference_v<T>, "Result<T> cannot hold a reference");

  Result(T value) : value_(std::move(value)) {}          // NOLINT(google-explicit-constructor)
  Result(Status status) : status_(std::move(status)) {   // NOLINT(google-explicit-constructor)
    RCB_ASSERT(!status_.ok());
  }

  [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] const Status& status() const noexcept { return status_; }

  T& value() {
    RCB_ASSERT(ok());
    return *value_;
  }
  [[nodiscard]] const T& value() const {
    RCB_ASSERT(ok());
    return *value_;
  }
  T& operator*() { return value(); }
  [[nodiscard]] const T& operator*() const { return value(); }
  T* operator->() { return &value(); }
  [[nodiscard]] const T* operator->() const { return &value(); }

  [[nodiscard]] T ValueOr(T fallback) const {
    return ok() ? *value_ : std::move(fallback);
  }

  /// Propagates failure unchanged; applies p fn to the value on success.
  template <class Fn>
  auto Map(Fn&& fn) const -> Result<std::invoke_result_t<Fn, const T&>> {
    using U = std::invoke_result_t<Fn, const T&>;
    if (!ok()) {
      return Result<U>(status_);
    }
    return Result<U>(fn(*value_));
  }

 private:
  std::optional<T> value_;
  Status status_;
};

/// Convenience constructor so call sites can write c Fail<X>(code, detail).
template <class T>
Result<T> Fail(ErrorCode code, std::string detail) {
  return Result<T>(Status::Error(code, std::move(detail)));
}

template <class T>
Result<T> Fail(const Status& status) {
  return Result<T>(status);
}

}  // namespace rcb

#endif  // RCB_STATUS_HPP
