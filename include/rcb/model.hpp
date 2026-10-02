// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The broker's domain model: what a site offers, what a caller asks for, what
// the broker decides, and why it refused.
//
// Every structure here is plain data with a canonical encoding. Validation is
// separate from construction so that a structure read from disk, from JSON or
// from a caller is checked at one place before it can influence accounting.

#ifndef RCB_MODEL_HPP
#define RCB_MODEL_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "rcb/digest.hpp"
#include "rcb/identifier.hpp"
#include "rcb/scaled.hpp"
#include "rcb/status.hpp"
#include "rcb/types.hpp"
#include "rcb/units.hpp"

namespace rcb {

// ---- enumerations --------------------------------------------------------

/// Declared risk posture of a site's offer, as published by the site's own
/// capacity authority. The broker never infers it.
enum class RiskTier : u8 { Nominal = 0, Watch = 1, Degraded = 2, Critical = 3 };

std::string_view RiskTierToken(RiskTier tier) noexcept;
Result<RiskTier> ParseRiskTier(std::string_view token) noexcept;
/// Rank for comparisons; higher means worse risk. A ceiling admits every tier
/// whose rank is at or below it.
constexpr u8 RiskRank(RiskTier tier) noexcept { return static_cast<u8>(tier); }

/// Request priority. Ordering is total and is used by every ordering decision
/// the broker makes, including which commitment is evicted when a site shrinks.
enum class PriorityClass : u8 { BestEffort = 0, Low = 1, Normal = 2, High = 3, Critical = 4 };

std::string_view PriorityToken(PriorityClass priority) noexcept;
Result<PriorityClass> ParsePriority(std::string_view token) noexcept;
/// Rank for comparisons; higher means more important.
constexpr u8 PriorityRank(PriorityClass priority) noexcept {
  return static_cast<u8>(priority);
}

/// The explicitly chosen fairness rule. There is no implicit default: a caller
/// that does not name one gets the documented stable order.
enum class FairnessPolicy : u8 {
  /// Candidates in (site, failure domain) order. Deterministic, re-runnable,
  /// and the cheapest to explain.
  StableSiteOrder = 0,
  /// Candidates ordered by unit price, then (site, failure domain).
  LowestCostFirst = 1,
  /// Demand is divided evenly across the sites that can still serve it, with
  /// the remainder redistributed deterministically.
  EqualShareAcrossSites = 2,
  /// Demand is apportioned in proportion to remaining allocatable capacity,
  /// with floor shares and a deterministic residue pass.
  ProportionalToAllocatable = 3,
};

std::string_view FairnessToken(FairnessPolicy policy) noexcept;
Result<FairnessPolicy> ParseFairness(std::string_view token) noexcept;

/// What the broker does when a new offer generation publishes less capacity
/// than the previous generation has already committed.
enum class ShrinkPolicy : u8 {
  /// Refuse the new generation; the previous generation stays authoritative.
  RefuseShrink = 0,
  /// Accept the new generation and revoke commitments, lowest priority first
  /// and, within a priority, most recently created first, until the ledger
  /// closes. Every revocation is reported and carries its reason.
  EvictToFit = 1,
};

std::string_view ShrinkPolicyToken(ShrinkPolicy policy) noexcept;
Result<ShrinkPolicy> ParseShrinkPolicy(std::string_view token) noexcept;

/// The durability boundary that actually applied when a decision was taken.
/// A decision is never labelled durable unless its record reached the platform
/// durability call.
enum class DurabilityClass : u8 { Durable = 0, Buffered = 1, Volatile = 2 };

std::string_view DurabilityToken(DurabilityClass durability) noexcept;

/// Why a commitment was revoked.
enum class RevocationReason : u8 {
  /// A newer generation of the same site no longer covers it.
  SupersededByGeneration = 0,
  /// The site explicitly withdrew the generation.
  OfferRevoked = 1,
  /// The new generation offered less than was committed, and the shrink
  /// policy chose to evict.
  CapacityShrink = 2,
};

std::string_view RevocationReasonToken(RevocationReason reason) noexcept;

// ---- offer ---------------------------------------------------------------

/// Cost, energy and carbon evidence for one service unit of the offer. The
/// broker does not invent prices: it multiplies the site's published evidence
/// by the quantity it commits and checks the caller's ceiling against the
/// exact product.
struct CostEvidence {
  /// Reference to the site's own published evidence; opaque to this boundary.
  EvidenceId reference;
  /// Price of one milli-unit of ServiceCapacity. The dimension is already in
  /// milli-units, so a commitment's cost is an exact product with no division
  /// and therefore no rounding.
  ScaledAmount price_per_milli_unit;
  /// Energy attributable to one milli-unit, in millijoules.
  i64 energy_millijoules_per_milli_unit = 0;
  /// Carbon attributable to one milli-unit, in milligrams.
  i64 carbon_milligrams_per_milli_unit = 0;
};

/// One failure domain's slice of a site's offer. Accounting is per tranche, so
/// capacity in different failure domains can never be silently pooled.
struct CapacityTranche {
  FailureDomainId domain;
  /// Capacity the broker may commit without touching protected reserve.
  CapacityVector allocatable;
  /// Capacity the site protects. It is committed only when the ask carries an
  /// authorization naming this offer's reserve policy and its priority is at
  /// least the offer's reserve minimum.
  CapacityVector protected_reserve;
};

/// An offer published by one site for one service class, in one generation.
struct Offer {
  SiteId site;
  /// Monotonic per site. A generation is immutable once published.
  u64 generation = 0;
  /// Identity of the site capacity snapshot this offer was derived from, as
  /// published by the site's own capacity authority. Evidence, not a handle.
  SnapshotId source_snapshot;
  ServiceClassId service_class;
  RegionId region;
  JurisdictionId jurisdiction;
  RiskTier risk = RiskTier::Nominal;
  /// Validity window in the caller's logical instant domain.
  i64 valid_from = 0;
  i64 valid_until = 0;
  /// Reference to the site's reserve policy that governs protected reserve.
  PolicyId reserve_policy;
  /// Generation of that policy, so a stale authorization can be detected.
  u64 policy_generation = 0;
  /// Lowest ask priority permitted to consume the protected reserve.
  PriorityClass reserve_minimum_priority = PriorityClass::Critical;
  CostEvidence cost;
  /// Sorted by failure domain, at most Limits::kMaxTranchesPerOffer entries.
  std::vector<CapacityTranche> tranches;
};

/// Structural validation of an offer as published. Rejects duplicate failure
/// domains rather than merging them, and bounds every quantity before it can
/// reach the ledger.
Status ValidateOffer(const Offer& offer);

/// Sorts tranches by failure domain. Duplicate domains are an error, not a
/// merge: two tranches claiming the same failure domain would be a
/// double-count waiting to happen.
Result<Offer> NormalizeOffer(Offer offer);

/// Component-wise sum of the tranche allocatable capacity of an offer.
Result<CapacityVector> OfferAllocatableTotal(const Offer& offer);
/// Component-wise sum of the tranche protected reserve of an offer.
Result<CapacityVector> OfferReserveTotal(const Offer& offer);

// ---- ask -----------------------------------------------------------------

/// A caller's expectation about a site's generation. A mismatch is refused
/// with the exact expected and current generations, never adjusted.
struct GenerationPin {
  SiteId site;
  u64 generation = 0;
};

/// A regional demand request.
struct Ask {
  /// Idempotency key. Two submissions with the same key and the same content
  /// are one decision; the same key with different content is a conflict.
  AskKey key;
  RequesterId requester;
  ServiceClassId service_class;
  /// What the caller wants, per dimension. At least one dimension must be
  /// positive.
  CapacityVector requested;
  PriorityClass priority = PriorityClass::Normal;
  FairnessPolicy fairness = FairnessPolicy::StableSiteOrder;
  /// When true, a decision that cannot satisfy every requested dimension in
  /// full is a refusal, with the shortfall named.
  bool all_or_nothing = false;
  /// When true, the whole request must be committed at one site and one
  /// failure domain. This is the strict coherence mode: a caller that needs a
  /// single physical home for the capacity asks for one.
  bool require_single_source = false;
  /// Logical instant at which freshness, validity and expiry are evaluated.
  i64 as_of = 0;

  std::vector<RegionId> allowed_regions;
  std::vector<RegionId> excluded_regions;
  std::vector<JurisdictionId> allowed_jurisdictions;
  std::vector<JurisdictionId> excluded_jurisdictions;
  std::vector<SiteId> allowed_sites;
  std::vector<SiteId> excluded_sites;
  std::vector<FailureDomainId> excluded_failure_domains;

  /// Number of distinct failure domains the commitment must span. 0 and 1 both
  /// mean "no diversity requirement".
  u32 min_distinct_failure_domains = 0;
  /// Upper bound on distinct sites used. 0 means unbounded.
  u32 max_sites = 0;

  /// Risk ceiling; an offer whose tier is worse than this is ineligible.
  RiskTier max_risk = RiskTier::Critical;

  bool has_cost_ceiling = false;
  ScaledAmount max_total_cost;
  bool has_energy_ceiling = false;
  i64 max_total_energy_millijoules = 0;
  bool has_carbon_ceiling = false;
  i64 max_total_carbon_milligrams = 0;

  /// Whether the caller may consume protected reserve at all.
  bool may_consume_protected_reserve = false;
  /// The policy the caller claims authority under; it must equal the offer's
  /// reserve policy reference.
  PolicyId reserve_policy_authorization;
  bool has_policy_generation_pin = false;
  u64 policy_generation_pin = 0;

  /// Generation expectations, sorted by site, at most
  /// Limits::kMaxGenerationPins entries.
  std::vector<GenerationPin> generation_pins;
  /// When true, only the current generation of each site is eligible; an older
  /// but still fresh generation is refused as superseded.
  bool require_current_generation = true;
};

Status ValidateAsk(const Ask& ask);

// ---- refusals ------------------------------------------------------------

/// Why a request, or part of a request, was not committed.
enum class ConstraintKind : u8 {
  NoOffersForServiceClass = 0,
  OfferNotYetValid = 1,
  OfferExpired = 2,
  OfferRevoked = 3,
  OfferSuperseded = 4,
  GenerationPinMismatch = 5,
  RegionNotAllowed = 6,
  RegionExcluded = 7,
  JurisdictionNotAllowed = 8,
  JurisdictionExcluded = 9,
  SiteNotAllowed = 10,
  SiteExcluded = 11,
  FailureDomainExcluded = 12,
  RiskAboveCeiling = 13,
  DiversityNotMet = 14,
  SiteLimitExceeded = 15,
  ReserveNotAuthorized = 16,
  ReservePolicyMismatch = 17,
  PolicyGenerationMismatch = 18,
  InsufficientAllocatableCapacity = 19,
  InsufficientReserveCapacity = 20,
  AllOrNothingNotMet = 21,
  SingleSourceNotMet = 22,
  CostCeilingExceeded = 23,
  EnergyCeilingExceeded = 24,
  CarbonCeilingExceeded = 25,
  CapacityEvictedByShrink = 26,
  LedgerLimitReached = 27,
  /// Nothing was eligible at all: no site publishes this service class, or every
  /// offer failed an eligibility rule.
  NoEligibleOffer = 28,
};

std::string_view ConstraintToken(ConstraintKind kind) noexcept;

/// A refusal with its blocking constraint named. A refusal without one is a
/// defect, not a policy.
struct BlockingConstraint {
  ConstraintKind kind = ConstraintKind::NoOffersForServiceClass;
  /// Empty when the constraint is not site-specific.
  SiteId site;
  /// Empty when the constraint is not failure-domain specific.
  FailureDomainId domain;
  bool has_dimension = false;
  Dimension dimension = Dimension::Power;
  /// Quantities are included only where they are meaningful for the kind.
  i64 required = 0;
  i64 available = 0;
  /// Free text. Not part of the contract.
  std::string detail;

  /// Stable single-line rendering used by the CLI and by the tests:
  /// "insufficient_allocatable_capacity(site=s1,dimension=power,required=10,available=4)".
  [[nodiscard]] std::string ToString() const;
};

// ---- decision ------------------------------------------------------------

enum class DecisionOutcome : u8 {
  Accepted = 0,
  PartiallyAccepted = 1,
  Refused = 2,
};

std::string_view DecisionOutcomeToken(DecisionOutcome outcome) noexcept;

/// One committed quantity against one (site, generation, failure domain) and
/// one capacity pool. A single ask may produce several allocations; each is
/// separately accountable and separately revocable.
struct Allocation {
  CommitmentId id;
  SiteId site;
  u64 generation = 0;
  FailureDomainId domain;
  ServiceClassId service_class;
  /// Committed from the tranche's allocatable pool.
  CapacityVector allocatable_amount;
  /// Committed from the tranche's protected reserve.
  CapacityVector reserve_amount;
  ScaledAmount cost;
  i64 energy_millijoules = 0;
  i64 carbon_milligrams = 0;
  PriorityClass priority = PriorityClass::Normal;
  u64 sequence = 0;
  i64 created_at = 0;

  [[nodiscard]] Result<CapacityVector> Total() const;
};

struct Decision {
  DecisionId id;
  AskKey key;
  RequesterId requester;
  ServiceClassId service_class;
  DecisionOutcome outcome = DecisionOutcome::Refused;
  /// Sorted by (site, failure domain, sequence).
  std::vector<Allocation> allocations;
  /// Why the request was refused, or why it was only partly satisfied. Empty
  /// only for a fully accepted decision.
  std::vector<BlockingConstraint> blocking;
  CapacityVector requested;
  CapacityVector committed;
  CapacityVector unmet;
  ScaledAmount total_cost;
  i64 total_energy_millijoules = 0;
  i64 total_carbon_milligrams = 0;
  u32 distinct_failure_domains = 0;
  u32 distinct_sites = 0;
  /// Monotonic within a broker incarnation; part of the decision identity.
  u64 broker_sequence = 0;
  /// Broker epoch that took the decision. A decision from another epoch is
  /// evidence about the past, never authority for the present.
  u64 broker_epoch = 0;
  i64 decided_at = 0;
  DurabilityClass durability = DurabilityClass::Volatile;
  /// True when this decision was served from the idempotency record rather
  /// than taken again.
  bool replay = false;
  /// Digest of the authoritative accounting state after the decision.
  Digest accounting_digest;
  /// Digest of the canonical request, used to detect key reuse.
  Digest request_digest;

  /// True when the decision committed anything.
  [[nodiscard]] bool committed_anything() const { return !allocations.empty(); }
};

}  // namespace rcb

#endif  // RCB_MODEL_HPP
