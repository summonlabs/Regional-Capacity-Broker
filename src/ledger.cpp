// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/ledger.hpp"

#include <algorithm>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"

namespace rcb {
namespace {

/// Adds into an accumulator, reporting rather than throwing on overflow: an
/// overflowing ledger is a conservation violation, and the report says so.
bool Accumulate(CapacityVector& accumulator, const CapacityVector& value) {
  Result<CapacityVector> next =
      CapacityVector::Add(accumulator, value, Limits::kMaxLedgerTotal * 4);
  if (!next.ok()) {
    return false;
  }
  accumulator = next.value();
  return true;
}

}  // namespace

Result<CapacityVector> CommitmentRecord::Total() const {
  return CapacityVector::Add(allocatable_amount, reserve_amount, Limits::kMaxLedgerTotal);
}

bool CommitmentRecord::IsEvictedBefore(const CommitmentRecord& other) const {
  if (priority != other.priority) {
    return PriorityRank(priority) < PriorityRank(other.priority);
  }
  if (sequence != other.sequence) {
    return sequence > other.sequence;
  }
  return other.id.value() < id.value();
}

Result<CapacityVector> TrancheLedger::RemainingAllocatable() const {
  Result<CapacityVector> committed_and_withheld =
      CapacityVector::Add(committed_allocatable, withheld, Limits::kMaxLedgerTotal);
  if (!committed_and_withheld.ok()) {
    return committed_and_withheld;
  }
  Result<CapacityVector> remaining = CapacityVector::Sub(allocatable, committed_and_withheld.value());
  if (!remaining.ok()) {
    return remaining;
  }
  if (!remaining.value().IsNonNegative()) {
    return Fail<CapacityVector>(
        ErrorCode::InvariantViolation,
        "the tranche has committed more allocatable capacity than it publishes");
  }
  return remaining;
}

Result<CapacityVector> TrancheLedger::RemainingReserve() const {
  Result<CapacityVector> committed_and_withheld =
      CapacityVector::Add(committed_reserve, reserve_withheld, Limits::kMaxLedgerTotal);
  if (!committed_and_withheld.ok()) {
    return committed_and_withheld;
  }
  Result<CapacityVector> remaining =
      CapacityVector::Sub(protected_reserve, committed_and_withheld.value());
  if (!remaining.ok()) {
    return remaining;
  }
  if (!remaining.value().IsNonNegative()) {
    return Fail<CapacityVector>(
        ErrorCode::InvariantViolation,
        "the tranche has committed more protected reserve than it publishes");
  }
  return remaining;
}

Result<CapacityVector> TrancheLedger::CommittedTotal() const {
  return CapacityVector::Add(committed_allocatable, committed_reserve, Limits::kMaxLedgerTotal);
}

Status TrancheLedger::ApplyCommitment(const CapacityVector& allocatable_amount,
                                      const CapacityVector& reserve_amount) {
  if (!allocatable_amount.IsNonNegative() || !reserve_amount.IsNonNegative()) {
    return Status::Error(ErrorCode::InvalidArgument, "commitment amount must not be negative");
  }
  Result<CapacityVector> next_allocatable =
      CapacityVector::Add(committed_allocatable, allocatable_amount, Limits::kMaxLedgerTotal);
  if (!next_allocatable.ok()) {
    return next_allocatable.status();
  }
  Result<CapacityVector> next_reserve =
      CapacityVector::Add(committed_reserve, reserve_amount, Limits::kMaxLedgerTotal);
  if (!next_reserve.ok()) {
    return next_reserve.status();
  }
  const CapacityVector candidate_allocatable = next_allocatable.value();
  const CapacityVector candidate_reserve = next_reserve.value();

  if (!allocatable.Covers(candidate_allocatable) ||
      !withheld.IsNonNegative()) {
    return Status::Error(ErrorCode::InsufficientCapacity,
                         "commitment would exceed the published allocatable capacity");
  }
  if (!protected_reserve.Covers(candidate_reserve)) {
    return Status::Error(ErrorCode::InsufficientCapacity,
                         "commitment would exceed the protected reserve");
  }
  // The identity must hold after the change, including the withheld term.
  Result<CapacityVector> remaining = CapacityVector::Sub(
      allocatable, CapacityVector::Add(candidate_allocatable, withheld, Limits::kMaxLedgerTotal)
                        .ValueOr(CapacityVector()));
  if (!remaining.ok() || !remaining.value().IsNonNegative()) {
    return Status::Error(ErrorCode::InsufficientCapacity,
                         "commitment would leave a negative residual");
  }
  committed_allocatable = candidate_allocatable;
  committed_reserve = candidate_reserve;
  return Status::Ok();
}

Status TrancheLedger::ReleaseCommitment(const CapacityVector& allocatable_amount,
                                        const CapacityVector& reserve_amount) {
  Result<CapacityVector> next_allocatable =
      CapacityVector::Sub(committed_allocatable, allocatable_amount);
  if (!next_allocatable.ok()) {
    return next_allocatable.status();
  }
  Result<CapacityVector> next_reserve = CapacityVector::Sub(committed_reserve, reserve_amount);
  if (!next_reserve.ok()) {
    return next_reserve.status();
  }
  if (!next_allocatable.value().IsNonNegative() || !next_reserve.value().IsNonNegative()) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "releasing a commitment would make the ledger negative");
  }
  committed_allocatable = next_allocatable.value();
  committed_reserve = next_reserve.value();
  return Status::Ok();
}

Status TrancheLedger::WithholdRemaining() {
  Result<CapacityVector> remaining = RemainingAllocatable();
  if (!remaining.ok()) {
    return remaining.status();
  }
  Result<CapacityVector> remaining_reserve = RemainingReserve();
  if (!remaining_reserve.ok()) {
    return remaining_reserve.status();
  }
  Result<CapacityVector> withheld_next =
      CapacityVector::Add(withheld, remaining.value(), Limits::kMaxLedgerTotal);
  if (!withheld_next.ok()) {
    return withheld_next.status();
  }
  Result<CapacityVector> reserve_withheld_next =
      CapacityVector::Add(reserve_withheld, remaining_reserve.value(), Limits::kMaxLedgerTotal);
  if (!reserve_withheld_next.ok()) {
    return reserve_withheld_next.status();
  }
  withheld = withheld_next.value();
  reserve_withheld = reserve_withheld_next.value();
  return Status::Ok();
}

const TrancheLedger* SiteLedger::FindTranche(const FailureDomainId& domain) const {
  const auto found = std::lower_bound(
      tranches.begin(), tranches.end(), domain,
      [](const TrancheLedger& tranche, const FailureDomainId& value) { return tranche.domain < value; });
  if (found == tranches.end() || found->domain != domain) {
    return nullptr;
  }
  return &(*found);
}

TrancheLedger* SiteLedger::FindTranche(const FailureDomainId& domain) {
  const auto found = std::lower_bound(
      tranches.begin(), tranches.end(), domain,
      [](const TrancheLedger& tranche, const FailureDomainId& value) { return tranche.domain < value; });
  if (found == tranches.end() || found->domain != domain) {
    return nullptr;
  }
  return &(*found);
}

Result<CapacityVector> SiteLedger::RemainingAllocatableTotal() const {
  CapacityVector total;
  for (const TrancheLedger& tranche : tranches) {
    Result<CapacityVector> remaining = tranche.RemainingAllocatable();
    if (!remaining.ok()) {
      return remaining;
    }
    if (!Accumulate(total, remaining.value())) {
      return Fail<CapacityVector>(ErrorCode::Overflow, "site remaining capacity overflow");
    }
  }
  return total;
}

Result<CapacityVector> SiteLedger::RemainingReserveTotal() const {
  CapacityVector total;
  for (const TrancheLedger& tranche : tranches) {
    Result<CapacityVector> remaining = tranche.RemainingReserve();
    if (!remaining.ok()) {
      return remaining;
    }
    if (!Accumulate(total, remaining.value())) {
      return Fail<CapacityVector>(ErrorCode::Overflow, "site remaining reserve overflow");
    }
  }
  return total;
}

Result<CapacityVector> SiteLedger::CommittedTotal() const {
  CapacityVector total;
  for (const TrancheLedger& tranche : tranches) {
    Result<CapacityVector> committed = tranche.CommittedTotal();
    if (!committed.ok()) {
      return committed;
    }
    if (!Accumulate(total, committed.value())) {
      return Fail<CapacityVector>(ErrorCode::Overflow, "site committed total overflow");
    }
  }
  return total;
}

Result<CapacityVector> SiteLedger::WithheldTotal() const {
  CapacityVector total;
  for (const TrancheLedger& tranche : tranches) {
    if (!Accumulate(total, tranche.withheld)) {
      return Fail<CapacityVector>(ErrorCode::Overflow, "site withheld total overflow");
    }
    if (!Accumulate(total, tranche.reserve_withheld)) {
      return Fail<CapacityVector>(ErrorCode::Overflow, "site withheld reserve overflow");
    }
  }
  return total;
}

std::size_t SiteLedger::LiveCommitmentCount() const {
  std::size_t count = 0;
  for (const CommitmentRecord& commitment : commitments) {
    if (commitment.live) {
      ++count;
    }
  }
  return count;
}

Result<CapacityVector> ConservationReport::OfferedTotal() const {
  return CapacityVector::Add(allocatable_total, reserve_total, Limits::kMaxLedgerTotal * 4);
}

Result<CapacityVector> ConservationReport::CommittedIncludingReserve() const {
  return CapacityVector::Add(committed_total, reserve_committed_total, Limits::kMaxLedgerTotal * 4);
}

}  // namespace rcb
