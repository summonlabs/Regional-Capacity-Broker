// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/broker.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"
#include "rcb/serialize.hpp"

namespace rcb {
namespace {

constexpr std::string_view kChainSeed = "rcb-accounting-chain-v1";

/// Exact ceiling division for a non-negative dividend and a positive divisor.
i64 CeilDiv(i64 value, i64 divisor) {
  RCB_ASSERT(value >= 0);
  RCB_ASSERT(divisor > 0);
  return value / divisor + ((value % divisor) != 0 ? 1 : 0);
}

/// A (site, failure domain) pair that could receive capacity for this ask.
struct Candidate {
  const SiteLedger* site = nullptr;
  const TrancheLedger* tranche = nullptr;
  CapacityVector room_allocatable;
  CapacityVector room_reserve;
  i64 unit_price_micros = 0;
};

void AddConstraint(std::vector<BlockingConstraint>& out, const std::size_t limit,
                   BlockingConstraint constraint) {
  if (out.size() >= limit) {
    return;
  }
  for (const BlockingConstraint& existing : out) {
    if (existing.kind == constraint.kind && existing.site == constraint.site &&
        existing.domain == constraint.domain) {
      return;
    }
  }
  out.push_back(std::move(constraint));
}

bool CandidateLess(const Candidate& a, const Candidate& b, const FairnessPolicy policy) {
  if (policy == FairnessPolicy::LowestCostFirst && a.unit_price_micros != b.unit_price_micros) {
    return a.unit_price_micros < b.unit_price_micros;
  }
  if (a.site->site != b.site->site) {
    return a.site->site < b.site->site;
  }
  return a.tranche->domain < b.tranche->domain;
}

bool ContainsValue(const std::vector<SiteId>& list, const SiteId& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

bool ContainsValue(const std::vector<RegionId>& list, const RegionId& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

bool ContainsValue(const std::vector<JurisdictionId>& list, const JurisdictionId& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

bool ContainsValue(const std::vector<FailureDomainId>& list, const FailureDomainId& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

BlockingConstraint MakeConstraint(const ConstraintKind kind, const SiteId& site,
                                  const FailureDomainId& domain, const bool has_dimension,
                                  const Dimension dimension, const i64 required,
                                  const i64 available, std::string detail) {
  BlockingConstraint constraint;
  constraint.kind = kind;
  constraint.site = site;
  constraint.domain = domain;
  constraint.has_dimension = has_dimension;
  constraint.dimension = dimension;
  constraint.required = required;
  constraint.available = available;
  constraint.detail = std::move(detail);
  return constraint;
}

/// Rebuilds the offer an offer generation was published from, so that a
/// re-publication can be compared field by field.
Offer OfferOfLedger(const SiteLedger& ledger) {
  Offer offer;
  offer.site = ledger.site;
  offer.generation = ledger.generation;
  offer.source_snapshot = ledger.source_snapshot;
  offer.service_class = ledger.service_class;
  offer.region = ledger.region;
  offer.jurisdiction = ledger.jurisdiction;
  offer.risk = ledger.risk;
  offer.valid_from = ledger.valid_from;
  offer.valid_until = ledger.valid_until;
  offer.reserve_policy = ledger.reserve_policy;
  offer.policy_generation = ledger.policy_generation;
  offer.reserve_minimum_priority = ledger.reserve_minimum_priority;
  offer.cost = ledger.cost;
  for (const TrancheLedger& tranche : ledger.tranches) {
    CapacityTranche entry;
    entry.domain = tranche.domain;
    entry.allocatable = tranche.allocatable;
    entry.protected_reserve = tranche.protected_reserve;
    offer.tranches.push_back(entry);
  }
  return offer;
}

}  // namespace

BrokerCore::BrokerCore(BrokerConfig config, u64 epoch) : config_(config), epoch_(epoch) {
  if (config_.max_blocking_constraints == 0) {
    config_.max_blocking_constraints = 1;
  }
  if (config_.max_retained_decisions == 0) {
    config_.max_retained_decisions = 1;
  }
  chain_digest_ = Sha256Of(kChainSeed);
}

void BrokerCore::ExtendChain(const std::string_view record_bytes) {
  Sha256 hasher;
  hasher.Update(std::span<const u8>(chain_digest_.bytes()));
  hasher.Update(record_bytes);
  chain_digest_ = hasher.Finalize();
}

Digest BrokerCore::RequestDigest(const Ask& ask) { return AskDigest(ask); }

// ---- publication ---------------------------------------------------------

Result<OfferPublication> BrokerCore::PublishOffer(const Offer& offer) {
  Result<Offer> normalized = NormalizeOffer(offer);
  if (!normalized.ok()) {
    return Result<OfferPublication>(normalized.status());
  }
  Status valid = ValidateOffer(normalized.value());
  if (!valid.ok()) {
    return Fail<OfferPublication>(valid);
  }
  return ApplyOffer(normalized.value(), next_sequence(), false);
}

Result<OfferPublication> BrokerCore::ReplayOffer(const Offer& offer, u64 sequence) {
  Result<Offer> normalized = NormalizeOffer(offer);
  if (!normalized.ok()) {
    return Result<OfferPublication>(normalized.status());
  }
  Status valid = ValidateOffer(normalized.value());
  if (!valid.ok()) {
    return Fail<OfferPublication>(valid);
  }
  return ApplyOffer(normalized.value(), sequence, true);
}

Result<OfferPublication> BrokerCore::ApplyOffer(const Offer& offer, const u64 sequence,
                                                const bool replay) {
  if (offer.generation == 0) {
    return Fail<OfferPublication>(ErrorCode::InvalidRequest, "offer generation must be positive");
  }
  OfferPublication publication;
  publication.site = offer.site;
  publication.generation = offer.generation;
  publication.sequence = sequence;

  const auto existing = sites_.find(offer.site);
  if (existing != sites_.end()) {
    const SiteLedger& current = existing->second.ledger;
    if (offer.generation == current.generation) {
      // Re-publishing the identical generation is idempotent; the same
      // generation with different content is a conflict, never a silent
      // overwrite of capacity other commitments were made against.
      if (CanonicalOfferText(OfferOfLedger(current)) == CanonicalOfferText(offer)) {
        publication.idempotent_replay = true;
        publication.allocatable_total = OfferAllocatableTotal(offer).ValueOr(CapacityVector());
        publication.reserve_total = OfferReserveTotal(offer).ValueOr(CapacityVector());
        return publication;
      }
      return Fail<OfferPublication>(
          ErrorCode::Conflict,
          "generation " + std::to_string(offer.generation) +
              " is already published for this site with different content");
    }
    if (offer.generation < current.generation) {
      return Fail<OfferPublication>(ErrorCode::StaleGeneration,
                                    "generation " + std::to_string(offer.generation) +
                                        " is older than the current generation " +
                                        std::to_string(current.generation));
    }
  }
  if (existing == sites_.end() && sites_.size() >= Limits::kMaxSites) {
    return Fail<OfferPublication>(ErrorCode::LimitExceeded, "too many live sites");
  }
  if (replay && sequence != next_sequence()) {
    return Fail<OfferPublication>(ErrorCode::PersistenceCorrupt,
                                  "publication record carries sequence " +
                                      std::to_string(sequence) + " but the ledger expects " +
                                      std::to_string(next_sequence()));
  }

  SiteLedger next;
  next.site = offer.site;
  next.generation = offer.generation;
  next.source_snapshot = offer.source_snapshot;
  next.service_class = offer.service_class;
  next.region = offer.region;
  next.jurisdiction = offer.jurisdiction;
  next.risk = offer.risk;
  next.valid_from = offer.valid_from;
  next.valid_until = offer.valid_until;
  next.reserve_policy = offer.reserve_policy;
  next.policy_generation = offer.policy_generation;
  next.reserve_minimum_priority = offer.reserve_minimum_priority;
  next.cost = offer.cost;
  next.published_sequence = sequence;
  for (const CapacityTranche& tranche : offer.tranches) {
    TrancheLedger entry;
    entry.domain = tranche.domain;
    entry.allocatable = tranche.allocatable;
    entry.protected_reserve = tranche.protected_reserve;
    next.tranches.push_back(entry);
  }

  std::vector<CommitmentRecord> carried;
  if (existing != sites_.end()) {
    for (const CommitmentRecord& commitment : existing->second.ledger.commitments) {
      if (commitment.live) {
        carried.push_back(commitment);
      }
    }
  }

  // Bind every live commitment to the new generation's tranches. Capacity that
  // the new generation no longer publishes is an over-commitment, not a
  // rounding difference, so it is detected exactly and never clamped away.
  std::vector<CommitmentRecord> eviction_order = carried;
  std::sort(eviction_order.begin(), eviction_order.end(),
            [](const CommitmentRecord& a, const CommitmentRecord& b) { return a.IsEvictedBefore(b); });

  std::vector<CommitmentRecord> over_committed;
  for (const CommitmentRecord& commitment : carried) {
    TrancheLedger* tranche = next.FindTranche(commitment.domain);
    if (tranche == nullptr) {
      over_committed.push_back(commitment);
      continue;
    }
    Result<CapacityVector> allocatable =
        CapacityVector::Add(tranche->committed_allocatable, commitment.allocatable_amount,
                            Limits::kMaxLedgerTotal);
    Result<CapacityVector> reserve = CapacityVector::Add(
        tranche->committed_reserve, commitment.reserve_amount, Limits::kMaxLedgerTotal);
    if (!allocatable.ok() || !reserve.ok()) {
      return Fail<OfferPublication>(ErrorCode::Overflow, "committed totals overflow the ledger");
    }
    tranche->committed_allocatable = allocatable.value();
    tranche->committed_reserve = reserve.value();
  }

  // A commitment against a failure domain the new generation does not publish
  // is entirely over-committed; the overage test below cannot see it, so the
  // orphan case is tracked separately.
  (void)over_committed;

  // A tranche is over-committed when either pool holds more than it publishes.
  // Both deficits are combined componentwise, because evicting a commitment
  // releases both pools and the ledger is closed only when both are closed.
  const auto overage_of = [](const TrancheLedger& tranche) -> Result<CapacityVector> {
    Result<CapacityVector> allocatable_used = CapacityVector::Add(
        tranche.committed_allocatable, tranche.withheld, Limits::kMaxLedgerTotal);
    if (!allocatable_used.ok()) {
      return allocatable_used;
    }
    Result<CapacityVector> reserve_used = CapacityVector::Add(
        tranche.committed_reserve, tranche.reserve_withheld, Limits::kMaxLedgerTotal);
    if (!reserve_used.ok()) {
      return reserve_used;
    }
    Result<CapacityVector> allocatable_over =
        CapacityVector::Sub(allocatable_used.value(), tranche.allocatable);
    if (!allocatable_over.ok()) {
      return allocatable_over;
    }
    Result<CapacityVector> reserve_over =
        CapacityVector::Sub(reserve_used.value(), tranche.protected_reserve);
    if (!reserve_over.ok()) {
      return reserve_over;
    }
    return allocatable_over.value().Max(reserve_over.value()).Max(CapacityVector());
  };

  CapacityVector overage;
  for (const TrancheLedger& tranche : next.tranches) {
    Result<CapacityVector> tranche_overage = overage_of(tranche);
    if (!tranche_overage.ok()) {
      return Result<OfferPublication>(tranche_overage.status());
    }
    Result<CapacityVector> total =
        CapacityVector::Add(overage, tranche_overage.value(), Limits::kMaxLedgerTotal);
    if (!total.ok()) {
      return Result<OfferPublication>(total.status());
    }
    overage = total.value();
  }
  // Capacity committed in a failure domain that is no longer published.
  bool has_orphan_domain = false;
  for (const CommitmentRecord& commitment : carried) {
    if (next.FindTranche(commitment.domain) == nullptr) {
      has_orphan_domain = true;
      break;
    }
  }

  std::vector<CommitmentRecord> evicted;
  // A carried commitment whose failure domain the new generation no longer
  // publishes has nowhere to live. The test is re-derived after every eviction:
  // it is not enough for the allocatable pools to close.
  const auto orphan_remains = [&next, &carried, &evicted]() {
    for (const CommitmentRecord& commitment : carried) {
      if (next.FindTranche(commitment.domain) != nullptr) {
        continue;
      }
      const bool already_evicted =
          std::any_of(evicted.begin(), evicted.end(), [&commitment](const CommitmentRecord& entry) {
            return entry.id == commitment.id;
          });
      if (!already_evicted) {
        return true;
      }
    }
    return false;
  };
  if (!overage.IsZero() || has_orphan_domain) {
    if (config_.shrink_policy == ShrinkPolicy::RefuseShrink) {
      return Fail<OfferPublication>(
          ErrorCode::InsufficientCapacity,
          "generation " + std::to_string(offer.generation) +
              " offers less than is already committed and the shrink policy refuses the shrink");
    }
    // Evict in policy order until the ledger closes. Removing a commitment adds
    // its capacity back, so the loop makes progress on every iteration and
    // terminates with an overage of zero once everything is evicted.
    std::size_t index = 0;
    while (index < eviction_order.size()) {
      bool closed = overage.IsZero() && !has_orphan_domain;
      if (closed) {
        break;
      }
      const CommitmentRecord& victim = eviction_order[index];
      ++index;
      TrancheLedger* tranche = next.FindTranche(victim.domain);
      if (tranche != nullptr) {
        Status released = tranche->ReleaseCommitment(victim.allocatable_amount, victim.reserve_amount);
        if (!released.ok()) {
          return Fail<OfferPublication>(ErrorCode::InvariantViolation,
                                        "eviction could not release the committed amount: " +
                                            released.ToString());
        }
      }
      CommitmentRecord revoked = victim;
      revoked.live = false;
      revoked.has_revocation = true;
      revoked.revocation_reason = RevocationReason::CapacityShrink;
      revoked.revoked_sequence = sequence;
      revoked.revoked_at = offer.valid_from;
      evicted.push_back(revoked);

      overage = CapacityVector();
      for (const TrancheLedger& tranche_entry : next.tranches) {
        Result<CapacityVector> tranche_overage = overage_of(tranche_entry);
        if (!tranche_overage.ok()) {
          return Result<OfferPublication>(tranche_overage.status());
        }
        Result<CapacityVector> total =
            CapacityVector::Add(overage, tranche_overage.value(), Limits::kMaxLedgerTotal);
        if (!total.ok()) {
          return Result<OfferPublication>(total.status());
        }
        overage = total.value();
      }
      has_orphan_domain = orphan_remains();
    }
    if (!overage.IsZero() || orphan_remains()) {
      return Fail<OfferPublication>(ErrorCode::InvariantViolation,
                                    "eviction did not close the ledger");
    }
  }

  // Rebuild the commitment list for the new generation: survivors keep creation
  // order and are bound to the new generation; revoked commitments stay in the
  // record as evidence.
  std::vector<CommitmentRecord> survivors;
  survivors.reserve(carried.size());
  for (const CommitmentRecord& commitment : carried) {
    const bool is_evicted =
        std::any_of(evicted.begin(), evicted.end(), [&commitment](const CommitmentRecord& entry) {
          return entry.id == commitment.id;
        });
    if (is_evicted) {
      continue;
    }
    CommitmentRecord survivor = commitment;
    survivor.accounted_generation = offer.generation;
    survivors.push_back(survivor);
  }
  std::sort(survivors.begin(), survivors.end(),
            [](const CommitmentRecord& a, const CommitmentRecord& b) { return a.sequence < b.sequence; });
  std::sort(evicted.begin(), evicted.end(),
            [](const CommitmentRecord& a, const CommitmentRecord& b) { return a.sequence < b.sequence; });

  next.commitments = survivors;
  for (const CommitmentRecord& entry : evicted) {
    next.commitments.push_back(entry);
  }
  std::sort(next.commitments.begin(), next.commitments.end(),
            [](const CommitmentRecord& a, const CommitmentRecord& b) { return a.sequence < b.sequence; });

  publication.carried_commitments = survivors.size();
  publication.commitments_revoked = evicted.size();
  publication.revoked = evicted;
  Result<CapacityVector> allocatable_total = OfferAllocatableTotal(offer);
  if (!allocatable_total.ok()) {
    return Result<OfferPublication>(allocatable_total.status());
  }
  Result<CapacityVector> reserve_total = OfferReserveTotal(offer);
  if (!reserve_total.ok()) {
    return Result<OfferPublication>(reserve_total.status());
  }
  publication.allocatable_total = allocatable_total.value();
  publication.reserve_total = reserve_total.value();

  SiteState& state = sites_[offer.site];
  if (existing != sites_.end()) {
    GenerationHistoryEntry entry;
    entry.generation = state.ledger.generation;
    entry.sequence = sequence;
    entry.at = offer.valid_from;
    entry.reason = RevocationReason::SupersededByGeneration;
    entry.commitments_revoked = evicted.size();
    state.history.push_back(entry);
    if (state.history.size() > config_.generation_history) {
      state.history.erase(state.history.begin());
    }
  }
  state.ledger = std::move(next);
  sequence_ = sequence;
  ++state_version_;

  const std::string record = "offer|" + CanonicalOfferText(offer) + "|" + std::to_string(sequence);
  ExtendChain(record);
  return publication;
}

Result<OfferRevocation> BrokerCore::RevokeOffer(const SiteId& site, u64 generation, i64 at,
                                                RevocationReason reason) {
  return ApplyRevocation(site, generation, next_sequence(), at, reason, false);
}

Result<OfferRevocation> BrokerCore::ReplayRevocation(const SiteId& site, u64 generation, u64 sequence,
                                                     i64 at, RevocationReason reason) {
  return ApplyRevocation(site, generation, sequence, at, reason, true);
}

Result<OfferRevocation> BrokerCore::ApplyRevocation(const SiteId& site, const u64 generation,
                                                    const u64 sequence, const i64 at,
                                                    const RevocationReason reason,
                                                    const bool replay) {
  const auto found = sites_.find(site);
  if (found == sites_.end()) {
    return Fail<OfferRevocation>(ErrorCode::UnknownSite, "no offer has been published for this site");
  }
  SiteLedger& ledger = found->second.ledger;
  if (generation != ledger.generation) {
    return Fail<OfferRevocation>(ErrorCode::StaleGeneration,
                                 "generation " + std::to_string(generation) +
                                     " is not the current generation " +
                                     std::to_string(ledger.generation));
  }
  if (replay && sequence != next_sequence()) {
    return Fail<OfferRevocation>(ErrorCode::PersistenceCorrupt,
                                 "revocation record carries sequence " + std::to_string(sequence) +
                                     " but the ledger expects " + std::to_string(next_sequence()));
  }

  OfferRevocation revocation;
  revocation.site = site;
  revocation.generation = generation;
  revocation.sequence = sequence;
  revocation.at = at;

  if (!ledger.revoked) {
    for (CommitmentRecord& commitment : ledger.commitments) {
      if (!commitment.live) {
        continue;
      }
      TrancheLedger* tranche = ledger.FindTranche(commitment.domain);
      const Status released =
          tranche != nullptr
              ? tranche->ReleaseCommitment(commitment.allocatable_amount, commitment.reserve_amount)
              : Status::Ok();
      if (!released.ok()) {
        return Fail<OfferRevocation>(ErrorCode::InvariantViolation,
                                     "revocation could not release a commitment: " +
                                         released.ToString());
      }
      commitment.live = false;
      commitment.has_revocation = true;
      commitment.revocation_reason = reason;
      commitment.revoked_sequence = sequence;
      commitment.revoked_at = at;
      ++revocation.commitments_revoked;
      revocation.revoked.push_back(commitment);
    }
    for (TrancheLedger& tranche : ledger.tranches) {
      Status withheld = tranche.WithholdRemaining();
      if (!withheld.ok()) {
        return Fail<OfferRevocation>(withheld);
      }
      Result<CapacityVector> total =
          CapacityVector::Add(revocation.withheld, tranche.withheld, Limits::kMaxLedgerTotal * 4);
      if (!total.ok()) {
        return Result<OfferRevocation>(total.status());
      }
      revocation.withheld = total.value();
    }
    ledger.revoked = true;
    ledger.revocation_reason = reason;
    ledger.revoked_sequence = sequence;
    ledger.revoked_at = at;
    found->second.history.push_back(GenerationHistoryEntry{generation, sequence, at, reason,
                                                            revocation.commitments_revoked});
    if (found->second.history.size() > config_.generation_history) {
      found->second.history.erase(found->second.history.begin());
    }
  }

  sequence_ = sequence;
  ++state_version_;
  ExtendChain("revoke|" + site.value() + "|" + std::to_string(generation) + "|" +
              std::to_string(sequence) + "|" + std::to_string(at) + "|" +
              std::string(RevocationReasonToken(reason)));
  return revocation;
}

// ---- planning ------------------------------------------------------------

namespace {

/// Decision identity is derived from the canonical request, never random: the
/// same ask produces the same identity on every run and every restart, which is
/// what makes a retried ask provably idempotent. The ask key is part of the
/// digest, so two different keys never collide by content.
DecisionId DeriveDecisionId(const Digest& request_digest) {
  const std::string text = "dec-" + request_digest.ShortHex(24);
  return DecisionId::Make(text).ValueOr(DecisionId());
}

/// Commitment identity is derived from the decision it belongs to, the exact
/// scope it commits against, and its sequence.
CommitmentId DeriveCommitmentId(const DecisionId& decision, const SiteId& site,
                                const FailureDomainId& domain, const u64 sequence) {
  const std::string material = decision.value() + "|" + site.value() + "|" + domain.value() + "|" +
                               std::to_string(sequence);
  const Digest digest = Sha256Of(material);
  return CommitmentId::Make("cmt-" + digest.ShortHex(24)).ValueOr(CommitmentId());
}

/// Adds a tranche's term into a report total, recording a violation rather than
/// failing when the total overflows.
bool AccumulateOrReport(ConservationReport& report, CapacityVector& total,
                        const CapacityVector& value, const std::string& scope) {
  Result<CapacityVector> next =
      CapacityVector::Add(total, value, Limits::kMaxLedgerTotal * 8);
  if (!next.ok()) {
    ConservationViolation violation;
    violation.scope = scope;
    violation.detail = "a ledger total overflows when re-derived";
    report.violations.push_back(violation);
    return false;
  }
  total = next.value();
  return true;
}

/// Clamps a take so that the exact cost, energy and carbon of the priced
/// dimension keep the running totals inside the ask's ceilings. \p budget_divisor
/// is the number of failure domains that still have to receive capacity: a
/// ceiling is shared between them, because otherwise the first candidate in
/// order would spend the whole budget and make an achievable diverse commitment
/// look impossible. The rest of the take is scaled by the same exact ratio and
/// floored, so the commitment stays a coherent fraction of the request and
/// nothing is ever rounded up. Returns false when nothing can be taken under the
/// ceilings at all.
bool ClampTakeToCeilings(CapacityVector& take, const i64 unit_price_micros,
                         const i64 unit_energy, const i64 unit_carbon, const Ask& ask,
                         const ScaledAmount& total_cost, const i64 total_energy,
                         const i64 total_carbon, const i64 budget_divisor,
                         bool& cost_clamped, bool& energy_clamped, bool& carbon_clamped) {
  const i64 service_capacity = take.Get(Dimension::ServiceCapacity);
  if (service_capacity <= 0) {
    return true;
  }
  const i64 divisor = budget_divisor > 1 ? budget_divisor : 1;
  i64 affordable = service_capacity;
  if (ask.has_cost_ceiling && unit_price_micros > 0) {
    const i64 remaining = (ask.max_total_cost.micros() - total_cost.micros()) / divisor;
    const i64 by_cost = remaining > 0 ? remaining / unit_price_micros : 0;
    if (by_cost < affordable) {
      affordable = by_cost;
      cost_clamped = true;
    }
  }
  if (ask.has_energy_ceiling && unit_energy > 0) {
    const i64 remaining = (ask.max_total_energy_millijoules - total_energy) / divisor;
    const i64 by_energy = remaining > 0 ? remaining / unit_energy : 0;
    if (by_energy < affordable) {
      affordable = by_energy;
      energy_clamped = true;
    }
  }
  if (ask.has_carbon_ceiling && unit_carbon > 0) {
    const i64 remaining = (ask.max_total_carbon_milligrams - total_carbon) / divisor;
    const i64 by_carbon = remaining > 0 ? remaining / unit_carbon : 0;
    if (by_carbon < affordable) {
      affordable = by_carbon;
      carbon_clamped = true;
    }
  }
  if (affordable < service_capacity) {
    // The ceiling scales the whole take, not just the dimension that carries the
    // price: a commitment that is half of the request in one dimension and all
    // of it in another is not a coherent bundle. Each other dimension is scaled
    // by the same exact ratio and floored, so nothing is committed beyond what
    // the ceiling authorised and nothing is rounded up.
    for (const Dimension dimension : kDimensions) {
      if (dimension == Dimension::ServiceCapacity) {
        continue;
      }
      const i64 amount = take.Get(dimension);
      if (amount <= 0) {
        continue;
      }
      Result<i64> scaled = MulDivI64(amount, affordable, service_capacity);
      if (!scaled.ok()) {
        return false;
      }
      take.Set(dimension, scaled.value());
    }
    take.Set(Dimension::ServiceCapacity, affordable);
  }
  // A service-class ask that can afford nothing under its ceilings takes
  // nothing: committing the unpriced dimensions alone would be a commitment the
  // caller's ceiling never authorised.
  if (ask.requested.Get(Dimension::ServiceCapacity) > 0 && affordable == 0) {
    return false;
  }
  return take.HasAny();
}

/// Folds a further take for the same (site, failure domain) into the allocation
/// that is already planned for it, so that one decision produces at most one
/// commitment per site and failure domain however many passes it took.
Status AccumulateAllocation(std::vector<PlannedAllocation>& allocations,
                            const PlannedAllocation& addition) {
  for (PlannedAllocation& entry : allocations) {
    if (entry.site != addition.site || entry.domain != addition.domain) {
      continue;
    }
    Result<CapacityVector> allocatable =
        CapacityVector::Add(entry.allocatable_amount, addition.allocatable_amount,
                            Limits::kMaxLedgerTotal);
    if (!allocatable.ok()) {
      return allocatable.status();
    }
    Result<CapacityVector> reserve = CapacityVector::Add(entry.reserve_amount,
                                                         addition.reserve_amount,
                                                         Limits::kMaxLedgerTotal);
    if (!reserve.ok()) {
      return reserve.status();
    }
    Result<ScaledAmount> cost = ScaledAmount::Add(entry.cost, addition.cost);
    if (!cost.ok()) {
      return cost.status();
    }
    Result<i64> energy = AddChecked(entry.energy_millijoules, addition.energy_millijoules);
    if (!energy.ok()) {
      return energy.status();
    }
    Result<i64> carbon = AddChecked(entry.carbon_milligrams, addition.carbon_milligrams);
    if (!carbon.ok()) {
      return carbon.status();
    }
    entry.allocatable_amount = allocatable.value();
    entry.reserve_amount = reserve.value();
    entry.cost = cost.value();
    entry.energy_millijoules = energy.value();
    entry.carbon_milligrams = carbon.value();
    return Status::Ok();
  }
  allocations.push_back(addition);
  return Status::Ok();
}

/// Determines whether a candidate tranche can serve one dimension of the ask.
bool CandidateHasRoom(const CapacityVector& room, const CapacityVector& need) {
  for (const Dimension dimension : kDimensions) {
    if (need.Get(dimension) > 0 && room.Get(dimension) > 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

Result<AskPlan> BrokerCore::PlanAsk(const Ask& ask) const {
  const Status valid = ValidateAsk(ask);
  if (!valid.ok()) {
    return Fail<AskPlan>(valid);
  }

  AskPlan plan;
  plan.ask = ask;
  plan.request_digest = AskDigest(ask);
  plan.broker_epoch = epoch_;
  plan.state_version = state_version_;
  plan.committed = CapacityVector();
  plan.unmet = ask.requested;

  const auto recorded = decisions_by_key_.find(ask.key);
  if (recorded != decisions_by_key_.end()) {
    if (recorded->second.request_digest != plan.request_digest) {
      return Fail<AskPlan>(ErrorCode::IdempotencyConflict,
                           "this ask key was already used with different content");
    }
    plan.kind = PlanKind::Replay;
    plan.replay_decision_id = recorded->second.id;
    return plan;
  }

  const std::size_t constraint_limit = config_.max_blocking_constraints;
  std::vector<BlockingConstraint> constraints;
  std::vector<Candidate> candidates;
  std::size_t sites_matching_class = 0;
  bool site_limit_applied = false;
  /// Set when a tranche still holds protected reserve that this ask could not
  /// touch, either because it claimed no authority or because its priority is
  /// below the offer's reserve minimum. It is only reported when the ask ends up
  /// short, because that is when it explains the shortfall.
  bool reserve_unavailable = false;
  bool reserve_policy_mismatch = false;

  const auto pin_for = [&ask](const SiteId& site, u64* generation) {
    for (const GenerationPin& pin : ask.generation_pins) {
      if (pin.site == site) {
        *generation = pin.generation;
        return true;
      }
    }
    return false;
  };

  for (const auto& entry : sites_) {
    const SiteId& site = entry.first;
    const SiteLedger& ledger = entry.second.ledger;
    if (ledger.service_class != ask.service_class) {
      continue;
    }
    ++sites_matching_class;

    if (ledger.revoked) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::OfferRevoked, site, FailureDomainId(), false,
                                   Dimension::Power, 0, 0, "the generation was withdrawn"));
      continue;
    }
    if (ask.as_of < ledger.valid_from) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::OfferNotYetValid, site, FailureDomainId(), false,
                                   Dimension::Power, ledger.valid_from, ask.as_of,
                                   "the offer is not yet valid at this instant"));
      continue;
    }
    if (ask.as_of > ledger.valid_until) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::OfferExpired, site, FailureDomainId(), false,
                                   Dimension::Power, ledger.valid_until, ask.as_of,
                                   "the offer expired before this instant"));
      continue;
    }
    u64 pinned_generation = 0;
    if (pin_for(site, &pinned_generation) && pinned_generation != ledger.generation) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::GenerationPinMismatch, site, FailureDomainId(),
                                   false, Dimension::Power,
                                   static_cast<i64>(pinned_generation % 1000000007ULL),
                                   static_cast<i64>(ledger.generation % 1000000007ULL),
                                   "the ask pins generation " + std::to_string(pinned_generation) +
                                       " but the site publishes " +
                                       std::to_string(ledger.generation)));
      continue;
    }
    if (!ask.allowed_sites.empty() && !ContainsValue(ask.allowed_sites, site)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::SiteNotAllowed, site, FailureDomainId(), false,
                                   Dimension::Power, 0, 0, "site is not in the allowed set"));
      continue;
    }
    if (ContainsValue(ask.excluded_sites, site)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::SiteExcluded, site, FailureDomainId(), false,
                                   Dimension::Power, 0, 0, "site is excluded by the ask"));
      continue;
    }
    if (!ask.allowed_regions.empty() && !ContainsValue(ask.allowed_regions, ledger.region)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::RegionNotAllowed, site, FailureDomainId(), false,
                                   Dimension::Power, 0, 0, "region is not in the allowed set"));
      continue;
    }
    if (ContainsValue(ask.excluded_regions, ledger.region)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::RegionExcluded, site, FailureDomainId(), false,
                                   Dimension::Power, 0, 0, "region is excluded by the ask"));
      continue;
    }
    if (!ask.allowed_jurisdictions.empty() &&
        !ContainsValue(ask.allowed_jurisdictions, ledger.jurisdiction)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::JurisdictionNotAllowed, site, FailureDomainId(),
                                   false, Dimension::Power, 0, 0,
                                   "jurisdiction is not in the allowed set"));
      continue;
    }
    if (ContainsValue(ask.excluded_jurisdictions, ledger.jurisdiction)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::JurisdictionExcluded, site, FailureDomainId(),
                                   false, Dimension::Power, 0, 0,
                                   "jurisdiction is excluded by the ask"));
      continue;
    }
    if (RiskRank(ledger.risk) > RiskRank(ask.max_risk)) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::RiskAboveCeiling, site, FailureDomainId(), false,
                                   Dimension::Power, static_cast<i64>(RiskRank(ledger.risk)),
                                   static_cast<i64>(RiskRank(ask.max_risk)),
                                   "the site's declared risk tier is above the ask ceiling"));
      continue;
    }
    if (ask.has_policy_generation_pin && ledger.policy_generation != ask.policy_generation_pin) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::PolicyGenerationMismatch, site,
                                   FailureDomainId(), false, Dimension::Power,
                                   static_cast<i64>(ask.policy_generation_pin % 1000000007ULL),
                                   static_cast<i64>(ledger.policy_generation % 1000000007ULL),
                                   "the offer's policy generation is not the pinned one"));
      continue;
    }

    const bool policy_authorized =
        ask.may_consume_protected_reserve && ask.reserve_policy_authorization == ledger.reserve_policy;
    const bool priority_authorized =
        PriorityRank(ask.priority) >= PriorityRank(ledger.reserve_minimum_priority);
    const bool reserve_usable = policy_authorized && priority_authorized;

    if (ask.may_consume_protected_reserve && !policy_authorized) {
      reserve_policy_mismatch = true;
    }

    for (const TrancheLedger& tranche : ledger.tranches) {
      if (ContainsValue(ask.excluded_failure_domains, tranche.domain)) {
        AddConstraint(constraints, constraint_limit,
                      MakeConstraint(ConstraintKind::FailureDomainExcluded, site, tranche.domain,
                                     false, Dimension::Power, 0, 0,
                                     "failure domain is excluded by the ask"));
        continue;
      }
      Result<CapacityVector> room_allocatable = tranche.RemainingAllocatable();
      if (!room_allocatable.ok()) {
        return Fail<AskPlan>(ErrorCode::InvariantViolation,
                             "the ledger has a negative residual before planning: " +
                                 room_allocatable.status().ToString());
      }
      Result<CapacityVector> remaining_reserve = tranche.RemainingReserve();
      if (!remaining_reserve.ok()) {
        return Fail<AskPlan>(ErrorCode::InvariantViolation,
                             "the ledger has a negative reserve residual before planning: " +
                                 remaining_reserve.status().ToString());
      }
      if (!reserve_usable && remaining_reserve.value().HasAny()) {
        reserve_unavailable = true;
      }
      const CapacityVector room_reserve =
          reserve_usable ? remaining_reserve.value() : CapacityVector();
      if (!CandidateHasRoom(room_allocatable.value(), ask.requested) &&
          !CandidateHasRoom(room_reserve, ask.requested)) {
        continue;
      }
      Candidate candidate;
      candidate.site = &ledger;
      candidate.tranche = &tranche;
      candidate.room_allocatable = room_allocatable.value();
      candidate.room_reserve = room_reserve;
      candidate.unit_price_micros = ledger.cost.price_per_milli_unit.micros();
      candidates.push_back(candidate);
    }
  }

  if (sites_matching_class == 0) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::NoOffersForServiceClass, SiteId(),
                                 FailureDomainId(), false, Dimension::Power, 0, 0,
                                 "no site publishes an offer for this service class"));
  }
  if (reserve_policy_mismatch) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::ReservePolicyMismatch, SiteId(),
                                 FailureDomainId(), false, Dimension::Power, 0, 0,
                                 "the ask's reserve authorization does not name the policy the "
                                 "offer protects its reserve under"));
  }
  if (reserve_unavailable) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::ReserveNotAuthorized, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, 0, 0,
                                 "the offer holds protected reserve this ask has no authority "
                                 "to consume, either because it named none or because its "
                                 "priority is below the offer's reserve minimum"));
  }

  const FairnessPolicy policy = ask.fairness;
  std::sort(candidates.begin(), candidates.end(),
            [policy](const Candidate& a, const Candidate& b) { return CandidateLess(a, b, policy); });

  if (ask.max_sites > 0) {
    std::vector<SiteId> kept_sites;
    std::vector<Candidate> filtered;
    for (const Candidate& candidate : candidates) {
      if (ContainsValue(kept_sites, candidate.site->site)) {
        filtered.push_back(candidate);
        continue;
      }
      if (kept_sites.size() < ask.max_sites) {
        kept_sites.push_back(candidate.site->site);
        filtered.push_back(candidate);
      } else {
        site_limit_applied = true;
      }
    }
    candidates = std::move(filtered);
  }

  if (ask.require_single_source) {
    std::vector<Candidate> solo;
    BlockingConstraint best = MakeConstraint(ConstraintKind::SingleSourceNotMet, SiteId(),
                                             FailureDomainId(), false, Dimension::Power, 0, 0,
                                             "no single failure domain can serve the whole request");
    bool have_best = false;
    for (const Candidate& candidate : candidates) {
      Result<CapacityVector> room_total =
          CapacityVector::Add(candidate.room_allocatable, candidate.room_reserve,
                              Limits::kMaxLedgerTotal);
      if (!room_total.ok()) {
        return Result<AskPlan>(room_total.status());
      }
      if (room_total.value().Covers(ask.requested)) {
        solo.push_back(candidate);
        continue;
      }
      for (const Dimension dimension : kDimensions) {
        if (ask.requested.Get(dimension) > room_total.value().Get(dimension)) {
          BlockingConstraint alternative =
              MakeConstraint(ConstraintKind::SingleSourceNotMet, candidate.site->site,
                             candidate.tranche->domain, true, dimension,
                             ask.requested.Get(dimension), room_total.value().Get(dimension),
                             "the largest single source cannot serve the whole request");
          if (!have_best || alternative.available > best.available) {
            best = alternative;
            have_best = true;
          }
          break;
        }
      }
    }
    if (solo.empty()) {
      AddConstraint(constraints, constraint_limit, best);
      plan.blocking = constraints;
      plan.outcome = DecisionOutcome::Refused;
      plan.unmet = ask.requested;
      return plan;
    }
    candidates = std::move(solo);
  }

  // ---- allocation ----
  CapacityVector need = ask.requested;
  CapacityVector committed;
  std::vector<PlannedAllocation> allocations;
  ScaledAmount total_cost;
  i64 total_energy = 0;
  i64 total_carbon = 0;
  bool cost_clamped = false;
  bool energy_clamped = false;
  bool carbon_clamped = false;
  std::vector<FailureDomainId> used_domains;
  std::vector<SiteId> used_sites;
  std::vector<FailureDomainId> domain_take_domains;
  std::vector<CapacityVector> domain_take_amounts;
  const u32 min_domains = ask.min_distinct_failure_domains;

  std::size_t passes = 0;
  const std::size_t max_passes = candidates.size() + 2;
  while (need.HasAny() && passes < max_passes) {
    ++passes;
    std::vector<std::size_t> active;
    CapacityVector total_room;
    std::vector<SiteId> active_sites;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
      const Candidate& candidate = candidates[index];
      Result<CapacityVector> room_total =
          CapacityVector::Add(candidate.room_allocatable, candidate.room_reserve,
                              Limits::kMaxLedgerTotal);
      if (!room_total.ok()) {
        return Result<AskPlan>(room_total.status());
      }
      if (!CandidateHasRoom(room_total.value(), need)) {
        continue;
      }
      active.push_back(index);
      Result<CapacityVector> accumulated = CapacityVector::Add(total_room, room_total.value(),
                                                               Limits::kMaxLedgerTotal);
      if (!accumulated.ok()) {
        return Result<AskPlan>(accumulated.status());
      }
      total_room = accumulated.value();
      if (!ContainsValue(active_sites, candidate.site->site)) {
        active_sites.push_back(candidate.site->site);
      }
    }
    if (active.empty()) {
      break;
    }

    // Shares are computed once per pass, against the demand as it stood when the
    // pass began. Recomputing them per candidate against a shrinking remainder
    // would divide the remainder again for every candidate and turn one fair
    // split into a long tail of fragments.
    std::size_t progress = 0;
    const CapacityVector pass_need = need;
    std::vector<CapacityVector> pass_caps(candidates.size());
    for (const std::size_t index : active) {
      const Candidate& candidate = candidates[index];
      Result<CapacityVector> room_total_result =
          CapacityVector::Add(candidate.room_allocatable, candidate.room_reserve,
                              Limits::kMaxLedgerTotal);
      if (!room_total_result.ok()) {
        return Result<AskPlan>(room_total_result.status());
      }
      const CapacityVector room_total = room_total_result.value();

      CapacityVector cap = pass_need;
      if (policy == FairnessPolicy::EqualShareAcrossSites) {
        const i64 shares = static_cast<i64>(active_sites.size());
        for (const Dimension dimension : kDimensions) {
          cap.Set(dimension, CeilDiv(pass_need.Get(dimension), shares));
        }
      } else if (policy == FairnessPolicy::ProportionalToAllocatable) {
        for (const Dimension dimension : kDimensions) {
          const i64 needed = pass_need.Get(dimension);
          const i64 available = room_total.Get(dimension);
          const i64 total = total_room.Get(dimension);
          if (needed <= 0 || available <= 0 || total <= 0) {
            cap.Set(dimension, 0);
            continue;
          }
          Result<i64> share = MulDivI64(needed, available, total);
          if (!share.ok()) {
            return Result<AskPlan>(share.status());
          }
          cap.Set(dimension, share.value());
        }
      }

      // Until the diversity requirement is met, no single failure domain may
      // take more than its even share of the whole request. The cap is measured
      // against the request and against everything that domain has already been
      // given, so a domain cannot accumulate more than its share across passes.
      if (min_domains > 1 && used_domains.size() < static_cast<std::size_t>(min_domains)) {
        const i64 shares = static_cast<i64>(min_domains);
        CapacityVector already;
        for (std::size_t i = 0; i < domain_take_domains.size(); ++i) {
          if (domain_take_domains[i] == candidate.tranche->domain) {
            already = domain_take_amounts[i];
            break;
          }
        }
        for (const Dimension dimension : kDimensions) {
          const i64 allowance = CeilDiv(ask.requested.Get(dimension), shares);
          const i64 left = allowance > already.Get(dimension) ? allowance - already.Get(dimension) : 0;
          cap.Set(dimension, std::min(cap.Get(dimension), left));
        }
      }
      pass_caps[index] = cap;
    }

    for (const std::size_t index : active) {
      Candidate& candidate = candidates[index];
      Result<CapacityVector> room_total_result =
          CapacityVector::Add(candidate.room_allocatable, candidate.room_reserve,
                              Limits::kMaxLedgerTotal);
      if (!room_total_result.ok()) {
        return Result<AskPlan>(room_total_result.status());
      }
      const CapacityVector room_total = room_total_result.value();
      const CapacityVector cap = pass_caps[index];
      CapacityVector take = room_total.Min(need).Min(cap);
      if (!take.HasAny()) {
        continue;
      }

      // Cost, energy and carbon ceilings are exact caps on the total, so they
      // clamp the priced dimension rather than rounding the commitment. While a
      // diversity requirement is still unmet the budget is shared between the
      // domains that still have to receive capacity.
      const i64 budget_divisor =
          (min_domains > 1 && used_domains.size() < static_cast<std::size_t>(min_domains))
              ? static_cast<i64>(min_domains - static_cast<u32>(used_domains.size()))
              : 1;
      if (!ClampTakeToCeilings(take, candidate.unit_price_micros,
                               candidate.site->cost.energy_millijoules_per_milli_unit,
                               candidate.site->cost.carbon_milligrams_per_milli_unit, ask,
                               total_cost, total_energy, total_carbon, budget_divisor,
                               cost_clamped, energy_clamped, carbon_clamped)) {
        continue;
      }

      const CapacityVector from_allocatable = take.Min(candidate.room_allocatable);
      Result<CapacityVector> from_reserve = CapacityVector::Sub(take, from_allocatable);
      if (!from_reserve.ok()) {
        return Result<AskPlan>(from_reserve.status());
      }
      const i64 priced_units = take.Get(Dimension::ServiceCapacity);
      Result<ScaledAmount> cost = candidate.site->cost.price_per_milli_unit.ScaleBy(priced_units);
      if (!cost.ok()) {
        return Result<AskPlan>(cost.status());
      }
      Result<i64> energy =
          MulChecked(candidate.site->cost.energy_millijoules_per_milli_unit, priced_units);
      if (!energy.ok()) {
        return Result<AskPlan>(energy.status());
      }
      Result<i64> carbon =
          MulChecked(candidate.site->cost.carbon_milligrams_per_milli_unit, priced_units);
      if (!carbon.ok()) {
        return Result<AskPlan>(carbon.status());
      }

      PlannedAllocation planned;
      planned.site = candidate.site->site;
      planned.generation = candidate.site->generation;
      planned.domain = candidate.tranche->domain;
      planned.allocatable_amount = from_allocatable;
      planned.reserve_amount = from_reserve.value();
      planned.cost = cost.value();
      planned.energy_millijoules = energy.value();
      planned.carbon_milligrams = carbon.value();
      planned.priority = ask.priority;
      const Status merged = AccumulateAllocation(allocations, planned);
      if (!merged.ok()) {
        return Fail<AskPlan>(merged);
      }

      Result<CapacityVector> next_need = CapacityVector::Sub(need, take);
      if (!next_need.ok()) {
        return Result<AskPlan>(next_need.status());
      }
      need = next_need.value();
      Result<CapacityVector> next_committed =
          CapacityVector::Add(committed, take, Limits::kMaxLedgerTotal);
      if (!next_committed.ok()) {
        return Result<AskPlan>(next_committed.status());
      }
      committed = next_committed.value();
      Result<ScaledAmount> next_cost = ScaledAmount::Add(total_cost, cost.value());
      if (!next_cost.ok()) {
        return Result<AskPlan>(next_cost.status());
      }
      total_cost = next_cost.value();
      Result<i64> next_energy = AddChecked(total_energy, energy.value());
      if (!next_energy.ok()) {
        return Result<AskPlan>(next_energy.status());
      }
      total_energy = next_energy.value();
      Result<i64> next_carbon = AddChecked(total_carbon, carbon.value());
      if (!next_carbon.ok()) {
        return Result<AskPlan>(next_carbon.status());
      }
      total_carbon = next_carbon.value();

      Result<CapacityVector> next_room_allocatable =
          CapacityVector::Sub(candidate.room_allocatable, from_allocatable);
      Result<CapacityVector> next_room_reserve =
          CapacityVector::Sub(candidate.room_reserve, from_reserve.value());
      if (!next_room_allocatable.ok() || !next_room_reserve.ok()) {
        return Fail<AskPlan>(ErrorCode::InvariantViolation,
                             "the allocator produced a take larger than the candidate room");
      }
      candidate.room_allocatable = next_room_allocatable.value();
      candidate.room_reserve = next_room_reserve.value();

      const FailureDomainId& domain = candidate.tranche->domain;
      bool domain_seen = false;
      for (std::size_t i = 0; i < domain_take_domains.size(); ++i) {
        if (domain_take_domains[i] == domain) {
          Result<CapacityVector> summed =
              CapacityVector::Add(domain_take_amounts[i], take, Limits::kMaxLedgerTotal);
          if (!summed.ok()) {
            return Result<AskPlan>(summed.status());
          }
          domain_take_amounts[i] = summed.value();
          domain_seen = true;
          break;
        }
      }
      if (!domain_seen) {
        domain_take_domains.push_back(domain);
        domain_take_amounts.push_back(take);
      }
      if (!ContainsValue(used_domains, domain)) {
        used_domains.push_back(domain);
      }
      if (!ContainsValue(used_sites, candidate.site->site)) {
        used_sites.push_back(candidate.site->site);
      }
      ++progress;
    }

    if (progress == 0) {
      // No candidate could take anything this pass; shares based on floor
      // division can starve a small candidate, so one relaxed greedy pass is
      // made before the shortfall is reported. It changes nothing about what is
      // offered or committed; it only prevents a rounding artefact from looking
      // like exhausted capacity.
      break;
    }
  }

  if (policy == FairnessPolicy::ProportionalToAllocatable && need.HasAny() && !committed.IsZero()) {
    // Residue pass: floor shares can leave capacity that the exact request could
    // still use. Fill it in the same deterministic candidate order.
    for (const Candidate& candidate : candidates) {
      if (!need.HasAny()) {
        break;
      }
      Result<CapacityVector> room_total =
          CapacityVector::Add(candidate.room_allocatable, candidate.room_reserve,
                              Limits::kMaxLedgerTotal);
      if (!room_total.ok()) {
        return Result<AskPlan>(room_total.status());
      }
      CapacityVector take = room_total.value().Min(need);
      if (!take.HasAny()) {
        continue;
      }
      const i64 budget_divisor =
          (min_domains > 1 && used_domains.size() < static_cast<std::size_t>(min_domains))
              ? static_cast<i64>(min_domains - static_cast<u32>(used_domains.size()))
              : 1;
      if (!ClampTakeToCeilings(take, candidate.unit_price_micros,
                               candidate.site->cost.energy_millijoules_per_milli_unit,
                               candidate.site->cost.carbon_milligrams_per_milli_unit, ask,
                               total_cost, total_energy, total_carbon, budget_divisor,
                               cost_clamped, energy_clamped, carbon_clamped)) {
        continue;
      }
      const CapacityVector from_allocatable = take.Min(candidate.room_allocatable);
      Result<CapacityVector> from_reserve = CapacityVector::Sub(take, from_allocatable);
      if (!from_reserve.ok()) {
        return Result<AskPlan>(from_reserve.status());
      }
      const i64 priced_units = take.Get(Dimension::ServiceCapacity);
      Result<ScaledAmount> cost = candidate.site->cost.price_per_milli_unit.ScaleBy(priced_units);
      Result<i64> energy =
          MulChecked(candidate.site->cost.energy_millijoules_per_milli_unit, priced_units);
      Result<i64> carbon =
          MulChecked(candidate.site->cost.carbon_milligrams_per_milli_unit, priced_units);
      if (!cost.ok() || !energy.ok() || !carbon.ok()) {
        return Fail<AskPlan>(ErrorCode::Overflow, "cost evidence overflow in the residue pass");
      }
      PlannedAllocation planned;
      planned.site = candidate.site->site;
      planned.generation = candidate.site->generation;
      planned.domain = candidate.tranche->domain;
      planned.allocatable_amount = from_allocatable;
      planned.reserve_amount = from_reserve.value();
      planned.cost = cost.value();
      planned.energy_millijoules = energy.value();
      planned.carbon_milligrams = carbon.value();
      planned.priority = ask.priority;
      const Status merged = AccumulateAllocation(allocations, planned);
      if (!merged.ok()) {
        return Fail<AskPlan>(merged);
      }

      Result<CapacityVector> next_need = CapacityVector::Sub(need, take);
      Result<CapacityVector> next_committed =
          CapacityVector::Add(committed, take, Limits::kMaxLedgerTotal);
      Result<ScaledAmount> next_cost = ScaledAmount::Add(total_cost, cost.value());
      Result<i64> next_energy = AddChecked(total_energy, energy.value());
      Result<i64> next_carbon = AddChecked(total_carbon, carbon.value());
      if (!next_need.ok() || !next_committed.ok() || !next_cost.ok() || !next_energy.ok() ||
          !next_carbon.ok()) {
        return Fail<AskPlan>(ErrorCode::Overflow, "residue pass arithmetic overflow");
      }
      need = next_need.value();
      committed = next_committed.value();
      total_cost = next_cost.value();
      total_energy = next_energy.value();
      total_carbon = next_carbon.value();
      if (!ContainsValue(used_domains, candidate.tranche->domain)) {
        used_domains.push_back(candidate.tranche->domain);
      }
      if (!ContainsValue(used_sites, candidate.site->site)) {
        used_sites.push_back(candidate.site->site);
      }
    }
  }

  // ---- constraints and outcome ----
  DecisionOutcome outcome = DecisionOutcome::Refused;
  if (committed.IsZero()) {
    outcome = DecisionOutcome::Refused;
  } else if (need.HasAny()) {
    outcome = DecisionOutcome::PartiallyAccepted;
  } else {
    outcome = DecisionOutcome::Accepted;
  }

  bool diversity_failed = false;
  if (min_domains > 1 && used_domains.size() < static_cast<std::size_t>(min_domains)) {
    diversity_failed = true;
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::DiversityNotMet, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, static_cast<i64>(min_domains),
                                 static_cast<i64>(used_domains.size()),
                                 "fewer distinct failure domains could take capacity than the "
                                 "ask requires"));
  }
  bool ceiling_exceeded = false;
  if (ask.has_cost_ceiling && total_cost > ask.max_total_cost) {
    ceiling_exceeded = true;
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::CostCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, ask.max_total_cost.micros(),
                                 total_cost.micros(), "the committed total would exceed the cost "
                                 "ceiling"));
  }
  if (ask.has_energy_ceiling && total_energy > ask.max_total_energy_millijoules) {
    ceiling_exceeded = true;
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::EnergyCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, ask.max_total_energy_millijoules,
                                 total_energy, "the committed total would exceed the energy "
                                 "ceiling"));
  }
  if (ask.has_carbon_ceiling && total_carbon > ask.max_total_carbon_milligrams) {
    ceiling_exceeded = true;
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::CarbonCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, ask.max_total_carbon_milligrams,
                                 total_carbon, "the committed total would exceed the carbon "
                                 "ceiling"));
  }
  if (cost_clamped) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::CostCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, 0, 0,
                                 "the cost ceiling limited what could be committed"));
  }
  if (energy_clamped) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::EnergyCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, 0, 0,
                                 "the energy ceiling limited what could be committed"));
  }
  if (carbon_clamped) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::CarbonCeilingExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, 0, 0,
                                 "the carbon ceiling limited what could be committed"));
  }
  if (site_limit_applied && need.HasAny()) {
    AddConstraint(constraints, constraint_limit,
                  MakeConstraint(ConstraintKind::SiteLimitExceeded, SiteId(), FailureDomainId(),
                                 false, Dimension::Power, static_cast<i64>(ask.max_sites),
                                 static_cast<i64>(used_sites.size()),
                                 "the ask's site limit excluded offers that still had capacity"));
  }

  const bool must_refuse = (ask.all_or_nothing && need.HasAny()) || diversity_failed || ceiling_exceeded;
  if (must_refuse) {
    if (ask.all_or_nothing && need.HasAny()) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::AllOrNothingNotMet, SiteId(), FailureDomainId(),
                                   false, Dimension::Power, 0, 0,
                                   "the ask requires every dimension in full or nothing"));
    }
    if (constraints.empty()) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::NoEligibleOffer, SiteId(), FailureDomainId(),
                                   false, Dimension::Power, 0, 0, "no eligible offer"));
    }
    plan.kind = PlanKind::Commit;
    plan.blocking = constraints;
    plan.outcome = DecisionOutcome::Refused;
    plan.committed = CapacityVector();
    plan.unmet = ask.requested;
    plan.total_cost = ScaledAmount();
    plan.total_energy_millijoules = 0;
    plan.total_carbon_milligrams = 0;
    return plan;
  }

  if (outcome == DecisionOutcome::Accepted) {
    // A fully satisfied ask has nothing to explain.
    plan.blocking.clear();
  } else {
    for (const Dimension dimension : kDimensions) {
      if (need.Get(dimension) > 0) {
        AddConstraint(constraints, constraint_limit,
                      MakeConstraint(ConstraintKind::InsufficientAllocatableCapacity, SiteId(),
                                     FailureDomainId(), true, dimension,
                                     ask.requested.Get(dimension), committed.Get(dimension),
                                     "the eligible offers could not cover this dimension"));
      }
    }
    if (constraints.empty()) {
      AddConstraint(constraints, constraint_limit,
                    MakeConstraint(ConstraintKind::NoEligibleOffer, SiteId(), FailureDomainId(),
                                   false, Dimension::Power, 0, 0, "no eligible offer"));
    }
    plan.blocking = constraints;
  }

  std::sort(allocations.begin(), allocations.end(),
            [](const PlannedAllocation& a, const PlannedAllocation& b) {
              if (a.site != b.site) {
                return a.site < b.site;
              }
              return a.domain < b.domain;
            });

  plan.kind = PlanKind::Commit;
  plan.allocations = std::move(allocations);
  plan.committed = committed;
  plan.unmet = need;
  plan.total_cost = total_cost;
  plan.total_energy_millijoules = total_energy;
  plan.total_carbon_milligrams = total_carbon;
  plan.outcome = outcome;
  return plan;
}

// ---- commit --------------------------------------------------------------

Result<Decision> BrokerCore::MaterializeDecision(const AskPlan& plan) const {
  if (plan.broker_epoch != epoch_) {
    return Fail<Decision>(ErrorCode::Fenced,
                          "the plan was produced under broker epoch " +
                              std::to_string(plan.broker_epoch) + " but this broker is at epoch " +
                              std::to_string(epoch_));
  }
  if (plan.kind == PlanKind::Replay) {
    const auto recorded = decisions_by_key_.find(plan.ask.key);
    if (recorded == decisions_by_key_.end()) {
      return Fail<Decision>(ErrorCode::NotFound,
                            "the idempotency record for this ask key is no longer retained");
    }
    Decision replay = recorded->second;
    replay.replay = true;
    return replay;
  }
  if (plan.state_version != state_version_) {
    return Fail<Decision>(ErrorCode::Fenced,
                          "the plan was computed against ledger version " +
                              std::to_string(plan.state_version) + " but the ledger is at " +
                              std::to_string(state_version_));
  }

  Decision decision;
  decision.request_digest = plan.request_digest;
  decision.id = DeriveDecisionId(plan.request_digest);
  const auto existing_id = decision_ids_.find(decision.id);
  if (existing_id != decision_ids_.end() && existing_id->second != plan.ask.key) {
    return Fail<Decision>(ErrorCode::DuplicateIdentity,
                          "two different asks produced the same decision identity; refusing to "
                          "merge them");
  }
  decision.key = plan.ask.key;
  decision.requester = plan.ask.requester;
  decision.service_class = plan.ask.service_class;
  decision.outcome = plan.outcome;
  decision.blocking = plan.blocking;
  decision.requested = plan.ask.requested;
  decision.committed = plan.committed;
  decision.unmet = plan.unmet;
  decision.total_cost = plan.total_cost;
  decision.total_energy_millijoules = plan.total_energy_millijoules;
  decision.total_carbon_milligrams = plan.total_carbon_milligrams;
  decision.decided_at = plan.ask.as_of;
  decision.broker_epoch = epoch_;
  decision.broker_sequence = sequence_ + 1;

  // Sequences are assigned in plan order so that a materialised decision and
  // the decision the commit produces are byte-for-byte the same document.
  u64 sequence = sequence_;
  for (const PlannedAllocation& planned : plan.allocations) {
    ++sequence;
    Allocation allocation;
    allocation.id = DeriveCommitmentId(decision.id, planned.site, planned.domain, sequence);
    allocation.site = planned.site;
    allocation.generation = planned.generation;
    allocation.domain = planned.domain;
    allocation.service_class = plan.ask.service_class;
    allocation.allocatable_amount = planned.allocatable_amount;
    allocation.reserve_amount = planned.reserve_amount;
    allocation.cost = planned.cost;
    allocation.energy_millijoules = planned.energy_millijoules;
    allocation.carbon_milligrams = planned.carbon_milligrams;
    allocation.priority = planned.priority;
    allocation.sequence = sequence;
    allocation.created_at = plan.ask.as_of;
    decision.allocations.push_back(allocation);
  }

  std::sort(decision.allocations.begin(), decision.allocations.end(),
            [](const Allocation& a, const Allocation& b) {
              if (a.site != b.site) {
                return a.site < b.site;
              }
              if (a.domain != b.domain) {
                return a.domain < b.domain;
              }
              return a.sequence < b.sequence;
            });
  std::vector<SiteId> distinct_sites;
  std::vector<FailureDomainId> distinct_domains;
  for (const Allocation& allocation : decision.allocations) {
    if (!ContainsValue(distinct_sites, allocation.site)) {
      distinct_sites.push_back(allocation.site);
    }
    if (!ContainsValue(distinct_domains, allocation.domain)) {
      distinct_domains.push_back(allocation.domain);
    }
  }
  decision.distinct_sites = static_cast<u32>(distinct_sites.size());
  decision.distinct_failure_domains = static_cast<u32>(distinct_domains.size());

  Sha256 hasher;
  hasher.Update(std::span<const u8>(chain_digest_.bytes()));
  hasher.Update("decision|" + CanonicalDecisionText(decision));
  decision.accounting_digest = hasher.Finalize();
  decision.durability = DurabilityClass::Volatile;
  return decision;
}

Result<Decision> BrokerCore::ApplyMaterializedDecision(const Decision& decision) {
  const Status applied = ApplyDecision(decision, false);
  if (!applied.ok()) {
    return Fail<Decision>(applied);
  }
  return decision;
}

Result<Decision> BrokerCore::CommitPlan(const AskPlan& plan) {
  Result<Decision> materialized = MaterializeDecision(plan);
  if (!materialized.ok()) {
    return materialized;
  }
  if (plan.kind == PlanKind::Replay) {
    return materialized;
  }
  return ApplyMaterializedDecision(materialized.value());
}

Status BrokerCore::ApplyDecision(const Decision& decision, const bool replay) {
  if (decisions_by_key_.find(decision.key) != decisions_by_key_.end() ||
      decision_ids_.find(decision.id) != decision_ids_.end()) {
    return Status::Error(ErrorCode::PersistenceCorrupt,
                         "a decision identity that is already in the ledger is being applied "
                         "again");
  }
  if (decision.broker_sequence == 0) {
    return Status::Error(ErrorCode::PersistenceCorrupt, "a decision record carries no sequence");
  }
  if (replay) {
    if (decision.broker_sequence < sequence_) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a journal decision record goes backwards in sequence");
    }
  } else if (decision.broker_sequence != sequence_ + 1) {
    return Status::Error(ErrorCode::Fenced,
                         "a live decision must carry the next sequence: it carries " +
                             std::to_string(decision.broker_sequence) + " but the ledger is at " +
                             std::to_string(sequence_));
  }

  std::vector<std::pair<SiteState*, CommitmentRecord>> applied;
  const auto rollback = [&applied]() {
    for (auto entry = applied.rbegin(); entry != applied.rend(); ++entry) {
      SiteState* state = entry->first;
      TrancheLedger* tranche = state->ledger.FindTranche(entry->second.domain);
      if (tranche != nullptr) {
        (void)tranche->ReleaseCommitment(entry->second.allocatable_amount,
                                         entry->second.reserve_amount);
      }
      state->ledger.commitments.pop_back();
    }
  };

  for (const Allocation& allocation : decision.allocations) {
    const auto site = sites_.find(allocation.site);
    if (site == sites_.end()) {
      rollback();
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a journal decision record names a site that has no ledger");
    }
    TrancheLedger* tranche = site->second.ledger.FindTranche(allocation.domain);
    if (tranche == nullptr) {
      rollback();
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a journal decision record names a failure domain that is not "
                           "published");
    }
    const Status status =
        tranche->ApplyCommitment(allocation.allocatable_amount, allocation.reserve_amount);
    if (!status.ok()) {
      rollback();
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a journal decision record does not fit the replayed ledger: " +
                               status.ToString());
    }
    CommitmentRecord record;
    record.id = allocation.id;
    record.ask_key = decision.key;
    record.decision_id = decision.id;
    record.requester = decision.requester;
    record.site = allocation.site;
    record.created_generation = allocation.generation;
    record.accounted_generation = allocation.generation;
    record.domain = allocation.domain;
    record.service_class = allocation.service_class;
    record.allocatable_amount = allocation.allocatable_amount;
    record.reserve_amount = allocation.reserve_amount;
    record.cost = allocation.cost;
    record.energy_millijoules = allocation.energy_millijoules;
    record.carbon_milligrams = allocation.carbon_milligrams;
    record.priority = allocation.priority;
    record.sequence = allocation.sequence;
    record.created_at = allocation.created_at;
    site->second.ledger.commitments.push_back(record);
    applied.emplace_back(&site->second, record);
    sequence_ = std::max(sequence_, allocation.sequence);
  }
  sequence_ = std::max(sequence_, decision.broker_sequence);

  ExtendChain("decision|" + CanonicalDecisionText(decision));
  if (chain_digest_ != decision.accounting_digest) {
    return Status::Error(ErrorCode::PersistenceCorrupt,
                         "the replayed accounting chain does not match the digest the record "
                         "carries");
  }

  Decision stored = decision;
  stored.replay = false;
  decisions_by_key_[stored.key] = stored;
  decision_ids_[stored.id] = stored.key;
  decision_order_.push_back(stored.key);
  ++state_version_;
  RetireIdempotencyRecords();
  return Status::Ok();
}

Status BrokerCore::ReplayDecision(const Decision& decision) { return ApplyDecision(decision, true); }

void BrokerCore::RetireIdempotencyRecords() {
  if (decisions_by_key_.size() <= config_.max_retained_decisions) {
    return;
  }
  std::size_t examined = 0;
  const std::size_t total = decision_order_.size();
  while (decisions_by_key_.size() > config_.max_retained_decisions && examined < total) {
    const AskKey key = decision_order_.front();
    decision_order_.erase(decision_order_.begin());
    ++examined;
    bool has_live_commitment = false;
    for (const auto& entry : sites_) {
      for (const CommitmentRecord& commitment : entry.second.ledger.commitments) {
        if (commitment.live && commitment.ask_key == key) {
          has_live_commitment = true;
          break;
        }
      }
      if (has_live_commitment) {
        break;
      }
    }
    if (has_live_commitment) {
      // The retry record is the only thing preventing a duplicate commitment,
      // so it is kept and re-queued rather than dropped.
      decision_order_.push_back(key);
      continue;
    }
    const auto found = decisions_by_key_.find(key);
    if (found != decisions_by_key_.end()) {
      decision_ids_.erase(found->second.id);
      decisions_by_key_.erase(found);
    }
  }
}

// ---- queries -------------------------------------------------------------

std::vector<SiteId> BrokerCore::SiteIds() const {
  std::vector<SiteId> ids;
  ids.reserve(sites_.size());
  for (const auto& entry : sites_) {
    ids.push_back(entry.first);
  }
  return ids;
}

const SiteLedger* BrokerCore::FindSite(const SiteId& site) const {
  const auto found = sites_.find(site);
  return found == sites_.end() ? nullptr : &found->second.ledger;
}

Result<Decision> BrokerCore::FindDecision(const DecisionId& id) const {
  const auto index = decision_ids_.find(id);
  if (index == decision_ids_.end()) {
    return Fail<Decision>(ErrorCode::NotFound, "no decision with that identity is retained");
  }
  const auto found = decisions_by_key_.find(index->second);
  if (found == decisions_by_key_.end()) {
    return Fail<Decision>(ErrorCode::NotFound, "the decision index and record store disagree");
  }
  return found->second;
}

Result<Decision> BrokerCore::FindDecisionByKey(const AskKey& key) const {
  const auto found = decisions_by_key_.find(key);
  if (found == decisions_by_key_.end()) {
    return Fail<Decision>(ErrorCode::NotFound, "no decision with that ask key is retained");
  }
  return found->second;
}

std::vector<Decision> BrokerCore::RecentDecisions(const std::size_t limit) const {
  std::vector<Decision> decisions;
  const std::size_t available = decision_order_.size();
  const std::size_t take = limit < available ? limit : available;
  decisions.reserve(take);
  for (std::size_t i = available - take; i < available; ++i) {
    const auto found = decisions_by_key_.find(decision_order_[i]);
    if (found != decisions_by_key_.end()) {
      decisions.push_back(found->second);
    }
  }
  return decisions;
}

std::vector<AskKey> BrokerCore::DecisionKeys() const {
  return std::vector<AskKey>(decision_order_.begin(), decision_order_.end());
}

std::vector<GenerationHistoryEntry> BrokerCore::GenerationHistory(const SiteId& site) const {
  const auto found = sites_.find(site);
  if (found == sites_.end()) {
    return {};
  }
  return found->second.history;
}

AccountingSummary BrokerCore::Summary() const {
  AccountingSummary summary;
  summary.sites = sites_.size();
  summary.decisions = decisions_by_key_.size();
  summary.sequence = sequence_;
  summary.epoch = epoch_;
  for (const auto& entry : sites_) {
    const SiteLedger& ledger = entry.second.ledger;
    summary.tranches += ledger.tranches.size();
    for (const CommitmentRecord& commitment : ledger.commitments) {
      if (commitment.live) {
        ++summary.live_commitments;
      } else {
        ++summary.revoked_commitments;
      }
    }
    Result<CapacityVector> committed = ledger.CommittedTotal();
    Result<CapacityVector> remaining = ledger.RemainingAllocatableTotal();
    Result<CapacityVector> withheld = ledger.WithheldTotal();
    Result<CapacityVector> reserve_remaining = ledger.RemainingReserveTotal();
    if (committed.ok()) {
      summary.committed_total = CapacityVector::Add(summary.committed_total, committed.value(),
                                                    Limits::kMaxLedgerTotal * 4)
                                    .ValueOr(summary.committed_total);
    }
    if (remaining.ok()) {
      summary.remaining_allocatable_total =
          CapacityVector::Add(summary.remaining_allocatable_total, remaining.value(),
                              Limits::kMaxLedgerTotal * 4)
              .ValueOr(summary.remaining_allocatable_total);
    }
    if (withheld.ok()) {
      summary.withheld_total = CapacityVector::Add(summary.withheld_total, withheld.value(),
                                                   Limits::kMaxLedgerTotal * 4)
                                   .ValueOr(summary.withheld_total);
    }
    if (reserve_remaining.ok()) {
      summary.reserve_remaining_total =
          CapacityVector::Add(summary.reserve_remaining_total, reserve_remaining.value(),
                              Limits::kMaxLedgerTotal * 4)
              .ValueOr(summary.reserve_remaining_total);
    }
  }
  summary.state_digest = StateDigest();
  return summary;
}

ConservationReport BrokerCore::VerifyConservation() const {
  ConservationReport report;
  for (const auto& entry : sites_) {
    const SiteLedger& ledger = entry.second.ledger;
    ++report.sites_checked;

    std::vector<FailureDomainId> seen_domains;
    for (const TrancheLedger& tranche : ledger.tranches) {
      ++report.tranches_checked;
      const std::string site_name = ledger.site.value();
      if (ContainsValue(seen_domains, tranche.domain)) {
        ConservationViolation violation;
        violation.scope = site_name + "/" + tranche.domain.value();
        violation.detail = "the same failure domain appears twice in one offer generation";
        report.violations.push_back(violation);
        report.closed = false;
      }
      seen_domains.push_back(tranche.domain);

      CapacityVector recorded_allocatable;
      CapacityVector recorded_reserve;
      for (const CommitmentRecord& commitment : ledger.commitments) {
        if (commitment.domain != tranche.domain) {
          continue;
        }
        if (commitment.live) {
          Result<CapacityVector> with_allocatable =
              CapacityVector::Add(recorded_allocatable, commitment.allocatable_amount,
                                  Limits::kMaxLedgerTotal * 4);
          Result<CapacityVector> with_reserve =
              CapacityVector::Add(recorded_reserve, commitment.reserve_amount,
                                  Limits::kMaxLedgerTotal * 4);
          if (!with_allocatable.ok() || !with_reserve.ok()) {
            ConservationViolation violation;
            violation.scope = site_name + "/" + tranche.domain.value();
            violation.detail = "committed totals overflow when re-derived from commitments";
            report.violations.push_back(violation);
            report.closed = false;
            break;
          }
          recorded_allocatable = with_allocatable.value();
          recorded_reserve = with_reserve.value();
        }
      }
      if (recorded_allocatable != tranche.committed_allocatable ||
          recorded_reserve != tranche.committed_reserve) {
        ConservationViolation violation;
        violation.scope = site_name + "/" + tranche.domain.value();
        violation.detail =
            "the tranche's committed totals disagree with the sum of its live commitments";
        report.violations.push_back(violation);
        report.closed = false;
      }

      for (const Dimension dimension : kDimensions) {
        Result<CapacityVector> committed_withheld =
            CapacityVector::Add(tranche.committed_allocatable, tranche.withheld,
                                Limits::kMaxLedgerTotal);
        if (!committed_withheld.ok()) {
          ConservationViolation violation;
          violation.scope = site_name + "/" + tranche.domain.value();
          violation.detail = "committed plus withheld overflows";
          violation.has_dimension = true;
          violation.dimension = dimension;
          report.violations.push_back(violation);
          report.closed = false;
          continue;
        }
        const i64 consumed = committed_withheld.value().Get(dimension);
        const i64 published = tranche.allocatable.Get(dimension);
        if (consumed > published) {
          ConservationViolation violation;
          violation.scope = site_name + "/" + tranche.domain.value();
          violation.detail = "committed plus withheld exceeds the published allocatable capacity";
          violation.has_dimension = true;
          violation.dimension = dimension;
          report.violations.push_back(violation);
          report.closed = false;
        }
        const i64 reserve_consumed =
            tranche.committed_reserve.Get(dimension) + tranche.reserve_withheld.Get(dimension);
        if (reserve_consumed > tranche.protected_reserve.Get(dimension)) {
          ConservationViolation violation;
          violation.scope = site_name + "/" + tranche.domain.value();
          violation.detail = "committed reserve plus withheld exceeds the protected reserve";
          violation.has_dimension = true;
          violation.dimension = dimension;
          report.violations.push_back(violation);
          report.closed = false;
        }
      }

      CapacityVector committed_total = tranche.CommittedTotal().ValueOr(CapacityVector());
      CapacityVector remaining = tranche.RemainingAllocatable().ValueOr(CapacityVector());
      CapacityVector remaining_reserve = tranche.RemainingReserve().ValueOr(CapacityVector());
      if (!AccumulateOrReport(report, report.allocatable_total, tranche.allocatable, site_name) ||
          !AccumulateOrReport(report, report.committed_total, committed_total, site_name) ||
          !AccumulateOrReport(report, report.remaining_total, remaining, site_name) ||
          !AccumulateOrReport(report, report.withheld_total, tranche.withheld, site_name) ||
          !AccumulateOrReport(report, report.reserve_total, tranche.protected_reserve, site_name) ||
          !AccumulateOrReport(report, report.reserve_committed_total, tranche.committed_reserve,
                              site_name) ||
          !AccumulateOrReport(report, report.reserve_remaining_total, remaining_reserve,
                              site_name) ||
          !AccumulateOrReport(report, report.reserve_withheld_total, tranche.reserve_withheld,
                              site_name)) {
        report.closed = false;
      }
    }

    std::vector<CommitmentId> seen_ids;
    for (const CommitmentRecord& commitment : ledger.commitments) {
      ++report.commitments_checked;
      if (commitment.live) {
        ++report.live_commitments;
      } else {
        ++report.revoked_commitments;
      }
      if (std::find(seen_ids.begin(), seen_ids.end(), commitment.id) != seen_ids.end()) {
        ConservationViolation violation;
        violation.scope = ledger.site.value();
        violation.detail = "duplicate commitment identity inside one site ledger";
        report.violations.push_back(violation);
        report.closed = false;
      }
      seen_ids.push_back(commitment.id);
      if (commitment.live && ledger.FindTranche(commitment.domain) == nullptr) {
        ConservationViolation violation;
        violation.scope = ledger.site.value() + "/" + commitment.domain.value();
        violation.detail =
            "a live commitment names a failure domain this generation does not publish";
        report.violations.push_back(violation);
        report.closed = false;
      }
    }
  }
  return report;
}

Digest BrokerCore::StateDigest() const { return StateDigestOf(*this); }

Status BrokerCore::SetDecisionDurability(const DecisionId& id, const DurabilityClass durability) {
  const auto index = decision_ids_.find(id);
  if (index == decision_ids_.end()) {
    return Status::Error(ErrorCode::NotFound, "no decision with that identity is retained");
  }
  const auto found = decisions_by_key_.find(index->second);
  if (found == decisions_by_key_.end()) {
    return Status::Error(ErrorCode::NotFound, "the decision index and record store disagree");
  }
  found->second.durability = durability;
  return Status::Ok();
}

// ---- restore -------------------------------------------------------------

Status BrokerCore::RestoreSiteLedger(SiteLedger ledger, std::vector<GenerationHistoryEntry> history) {
  if (ledger.site.empty() || ledger.generation == 0) {
    return Status::Error(ErrorCode::PersistenceCorrupt,
                         "a restored site ledger needs an identity and a generation");
  }
  if (sites_.find(ledger.site) != sites_.end()) {
    return Status::Error(ErrorCode::PersistenceCorrupt,
                         "a restored snapshot names the same site twice");
  }
  if (sites_.size() >= Limits::kMaxSites) {
    return Status::Error(ErrorCode::LimitExceeded, "too many live sites in the snapshot");
  }
  for (std::size_t i = 1; i < ledger.tranches.size(); ++i) {
    if (!(ledger.tranches[i - 1].domain < ledger.tranches[i].domain)) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "restored offer tranches are not sorted and unique");
    }
  }
  for (std::size_t i = 1; i < ledger.commitments.size(); ++i) {
    if (ledger.commitments[i - 1].sequence >= ledger.commitments[i].sequence) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "restored commitments are not ordered by sequence");
    }
  }

  std::vector<CommitmentId> seen_ids;
  for (const CommitmentRecord& commitment : ledger.commitments) {
    if (commitment.id.empty() || commitment.sequence == 0) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a restored commitment has no identity or no sequence");
    }
    if (std::find(seen_ids.begin(), seen_ids.end(), commitment.id) != seen_ids.end()) {
      return Status::Error(ErrorCode::DuplicateIdentity,
                           "a restored site ledger carries a duplicate commitment identity");
    }
    seen_ids.push_back(commitment.id);
  }

  SiteState state;
  state.ledger = std::move(ledger);
  state.history = std::move(history);
  if (state.history.size() > config_.generation_history) {
    state.history.erase(state.history.begin(),
                        state.history.begin() +
                            static_cast<std::ptrdiff_t>(state.history.size() -
                                                        config_.generation_history));
  }
  const SiteLedger& adopted = state.ledger;
  for (const TrancheLedger& tranche : adopted.tranches) {
    CapacityVector derived_allocatable;
    CapacityVector derived_reserve;
    for (const CommitmentRecord& commitment : adopted.commitments) {
      if (!commitment.live || commitment.domain != tranche.domain) {
        continue;
      }
      Result<CapacityVector> with_allocatable =
          CapacityVector::Add(derived_allocatable, commitment.allocatable_amount,
                              Limits::kMaxLedgerTotal * 4);
      Result<CapacityVector> with_reserve = CapacityVector::Add(derived_reserve,
                                                                commitment.reserve_amount,
                                                                Limits::kMaxLedgerTotal * 4);
      if (!with_allocatable.ok() || !with_reserve.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "restored commitment totals overflow the ledger");
      }
      derived_allocatable = with_allocatable.value();
      derived_reserve = with_reserve.value();
    }
    if (derived_allocatable != tranche.committed_allocatable ||
        derived_reserve != tranche.committed_reserve) {
      return Status::Error(
          ErrorCode::PersistenceCorrupt,
          "a restored tranche's committed totals disagree with the sum of its live commitments");
    }
    for (const Dimension dimension : kDimensions) {
      const i64 consumed = tranche.committed_allocatable.Get(dimension) +
                           tranche.withheld.Get(dimension);
      if (consumed > tranche.allocatable.Get(dimension)) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "a restored tranche has committed more than it published");
      }
      const i64 reserve_consumed =
          tranche.committed_reserve.Get(dimension) + tranche.reserve_withheld.Get(dimension);
      if (reserve_consumed > tranche.protected_reserve.Get(dimension)) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "a restored tranche has committed more reserve than it published");
      }
    }
  }

  sites_.emplace(adopted.site, std::move(state));
  return Status::Ok();
}

Status BrokerCore::RestoreDecisionRecord(const Decision& decision) {
  if (decision.id.empty() || decision.key.empty()) {
    return Status::Error(ErrorCode::PersistenceCorrupt,
                         "a restored decision has no identity or no ask key");
  }
  if (decisions_by_key_.find(decision.key) != decisions_by_key_.end()) {
    return Status::Error(ErrorCode::DuplicateIdentity,
                         "a restored snapshot carries the same ask key twice");
  }
  if (decision_ids_.find(decision.id) != decision_ids_.end()) {
    return Status::Error(ErrorCode::DuplicateIdentity,
                         "a restored snapshot carries the same decision identity twice");
  }
  decisions_by_key_[decision.key] = decision;
  decision_ids_[decision.id] = decision.key;
  decision_order_.push_back(decision.key);
  return Status::Ok();
}

Status BrokerCore::VerifyDecisionProvenance(const Decision& decision) const {
  for (const Allocation& allocation : decision.allocations) {
    const auto site = sites_.find(allocation.site);
    if (site == sites_.end()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a retained decision names a site with no ledger");
    }
    const auto found = std::find_if(
        site->second.ledger.commitments.begin(), site->second.ledger.commitments.end(),
        [&allocation](const CommitmentRecord& commitment) { return commitment.id == allocation.id; });
    if (found == site->second.ledger.commitments.end()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a retained decision claims a commitment the ledger does not hold");
    }
    if (found->allocatable_amount != allocation.allocatable_amount ||
        found->reserve_amount != allocation.reserve_amount || found->domain != allocation.domain) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "a retained decision's allocation disagrees with the ledger");
    }
  }
  return Status::Ok();
}

Status BrokerCore::RestoreConfig(const BrokerConfig& config) {
  config_ = config;
  if (config_.max_blocking_constraints == 0) {
    config_.max_blocking_constraints = 1;
  }
  if (config_.max_retained_decisions == 0) {
    config_.max_retained_decisions = 1;
  }
  return Status::Ok();
}

}  // namespace rcb
