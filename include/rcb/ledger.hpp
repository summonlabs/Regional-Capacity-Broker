// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The brokerage ledger: per-site, per-failure-domain exact accounting of what
// was offered, what is committed, what is still allocatable, and what is
// explicitly withheld.
//
// Conservation identity, per tranche and per dimension:
//
//   allocatable       = committed + remaining_allocatable + withheld
//   protected_reserve = reserve_committed + remaining_reserve + reserve_withheld
//
// Every term is a non-negative exact integer. A site's capacity is never
// pooled with another site's, and two failure domains are never merged.

#ifndef RCB_LEDGER_HPP
#define RCB_LEDGER_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "rcb/digest.hpp"
#include "rcb/identifier.hpp"
#include "rcb/model.hpp"
#include "rcb/scaled.hpp"
#include "rcb/status.hpp"
#include "rcb/types.hpp"
#include "rcb/units.hpp"

namespace rcb {

/// A commitment as it is held in the ledger, with its provenance and, when it
/// has been revoked, the reason and the sequence that revoked it.
struct CommitmentRecord {
  CommitmentId id;
  AskKey ask_key;
  DecisionId decision_id;
  RequesterId requester;
  SiteId site;
  /// Generation under which the commitment was created. Immutable provenance.
  u64 created_generation = 0;
  /// Generation the commitment is currently accounted against. A carried-over
  /// commitment is re-bound when a newer generation covers it, because the
  /// authority for its capacity is the generation that published that
  /// capacity. Never decreases.
  u64 accounted_generation = 0;
  FailureDomainId domain;
  ServiceClassId service_class;
  CapacityVector allocatable_amount;
  CapacityVector reserve_amount;
  ScaledAmount cost;
  i64 energy_millijoules = 0;
  i64 carbon_milligrams = 0;
  PriorityClass priority = PriorityClass::Normal;
  /// Globally monotonic creation sequence within the store's history.
  u64 sequence = 0;
  i64 created_at = 0;
  bool live = true;
  bool has_revocation = false;
  RevocationReason revocation_reason = RevocationReason::SupersededByGeneration;
  u64 revoked_sequence = 0;
  i64 revoked_at = 0;

  [[nodiscard]] Result<CapacityVector> Total() const;
  /// Eviction order: lowest priority first, then newest commitment first, then
  /// descending commitment identity for a total order.
  [[nodiscard]] bool IsEvictedBefore(const CommitmentRecord& other) const;
};

/// One failure domain's slice of the ledger.
struct TrancheLedger {
  FailureDomainId domain;
  CapacityVector allocatable;
  CapacityVector protected_reserve;
  CapacityVector committed_allocatable;
  CapacityVector committed_reserve;
  /// Capacity explicitly withdrawn from brokerage (a revoked generation).
  CapacityVector withheld;
  CapacityVector reserve_withheld;

  /// The uncommitted, unwithdrawn pool. A negative residual is an invariant
  /// violation and is reported as one, never returned as a quantity.
  [[nodiscard]] Result<CapacityVector> RemainingAllocatable() const;
  /// The protected reserve this tranche can still commit, under the same rule.
  [[nodiscard]] Result<CapacityVector> RemainingReserve() const;
  [[nodiscard]] Result<CapacityVector> CommittedTotal() const;

  /// Adds a commitment's amounts. Fails when either pool would be exceeded;
  /// over-commitment is an error, never a clamp.
  Status ApplyCommitment(const CapacityVector& allocatable_amount,
                         const CapacityVector& reserve_amount);
  /// Returns amounts to the pools. Fails when it would make a term negative.
  Status ReleaseCommitment(const CapacityVector& allocatable_amount,
                           const CapacityVector& reserve_amount);
  /// Moves the uncommitted pools into the withheld pools (revocation).
  Status WithholdRemaining();
};

/// One site's ledger, for one generation of that site's offer.
struct SiteLedger {
  SiteId site;
  u64 generation = 0;
  SnapshotId source_snapshot;
  ServiceClassId service_class;
  RegionId region;
  JurisdictionId jurisdiction;
  RiskTier risk = RiskTier::Nominal;
  i64 valid_from = 0;
  i64 valid_until = 0;
  PolicyId reserve_policy;
  u64 policy_generation = 0;
  PriorityClass reserve_minimum_priority = PriorityClass::Critical;
  CostEvidence cost;
  bool revoked = false;
  RevocationReason revocation_reason = RevocationReason::OfferRevoked;
  u64 revoked_sequence = 0;
  i64 revoked_at = 0;
  /// Sequence of the publication that established this generation.
  u64 published_sequence = 0;
  /// Sorted by failure domain.
  std::vector<TrancheLedger> tranches;
  /// Sorted by creation sequence.
  std::vector<CommitmentRecord> commitments;

  [[nodiscard]] const TrancheLedger* FindTranche(const FailureDomainId& domain) const;
  [[nodiscard]] TrancheLedger* FindTranche(const FailureDomainId& domain);
  [[nodiscard]] bool IsValidAt(i64 instant) const {
    return !revoked && instant >= valid_from && instant <= valid_until;
  }
  [[nodiscard]] Result<CapacityVector> RemainingAllocatableTotal() const;
  [[nodiscard]] Result<CapacityVector> RemainingReserveTotal() const;
  [[nodiscard]] Result<CapacityVector> CommittedTotal() const;
  [[nodiscard]] Result<CapacityVector> WithheldTotal() const;
  [[nodiscard]] std::size_t LiveCommitmentCount() const;
};

/// A single conservation failure, named by scope so that the report points at
/// the site, tranche and dimension that disagree.
struct ConservationViolation {
  std::string scope;
  std::string detail;
  Dimension dimension = Dimension::Power;
  bool has_dimension = false;
};

/// The result of re-deriving the conservation identity from the stored terms.
struct ConservationReport {
  bool closed = true;
  std::size_t sites_checked = 0;
  std::size_t tranches_checked = 0;
  std::size_t commitments_checked = 0;
  std::size_t live_commitments = 0;
  std::size_t revoked_commitments = 0;
  std::vector<ConservationViolation> violations;
  CapacityVector allocatable_total;
  CapacityVector committed_total;
  CapacityVector remaining_total;
  CapacityVector withheld_total;
  CapacityVector reserve_total;
  CapacityVector reserve_committed_total;
  CapacityVector reserve_remaining_total;
  CapacityVector reserve_withheld_total;

  /// offered = allocatable + protected reserve, both committed and not.
  [[nodiscard]] Result<CapacityVector> OfferedTotal() const;
  [[nodiscard]] Result<CapacityVector> CommittedIncludingReserve() const;
};

/// A cheap summary of the ledger for operators and smoke checks.
struct AccountingSummary {
  std::size_t sites = 0;
  std::size_t tranches = 0;
  std::size_t live_commitments = 0;
  std::size_t revoked_commitments = 0;
  std::size_t decisions = 0;
  u64 sequence = 0;
  u64 epoch = 0;
  CapacityVector committed_total;
  CapacityVector remaining_allocatable_total;
  CapacityVector withheld_total;
  CapacityVector reserve_remaining_total;
  Digest state_digest;
};

}  // namespace rcb

#endif  // RCB_LEDGER_HPP
