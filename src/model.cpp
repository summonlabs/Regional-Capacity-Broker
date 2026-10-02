// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/model.hpp"

#include <algorithm>

#include "rcb/checked.hpp"

namespace rcb {
namespace {

template <class Tag>
Status RequireId(const TaggedId<Tag>& identifier, const char* what) {
  if (identifier.empty()) {
    return Status::Error(ErrorCode::MissingField, std::string(what) + " must be present");
  }
  return Status::Ok();
}

Status ValidateNonNegative(ScaledAmount amount, const char* what) {
  if (amount.IsNegative()) {
    return Status::Error(ErrorCode::InvalidRequest, std::string(what) + " must not be negative");
  }
  return Status::Ok();
}

Status ValidateListSize(std::size_t size, const char* what) {
  if (size > Limits::kMaxListElements) {
    return Status::Error(ErrorCode::LimitExceeded, std::string(what) + " has too many elements");
  }
  return Status::Ok();
}

template <class Tag>
bool Contains(const std::vector<TaggedId<Tag>>& list, const TaggedId<Tag>& value) {
  return std::find(list.begin(), list.end(), value) != list.end();
}

template <class Tag>
Status ValidateDisjoint(const std::vector<TaggedId<Tag>>& allowed,
                        const std::vector<TaggedId<Tag>>& excluded, const char* what) {
  for (const auto& value : excluded) {
    if (Contains(allowed, value)) {
      return Status::Error(ErrorCode::InvalidRequest,
                           std::string(what) + " is both allowed and excluded");
    }
  }
  return Status::Ok();
}

template <class Tag>
Status ValidateSortedUnique(const std::vector<TaggedId<Tag>>& list, const char* what) {
  for (std::size_t i = 1; i < list.size(); ++i) {
    if (!(list[i - 1] < list[i])) {
      return Status::Error(ErrorCode::DuplicateIdentity,
                           std::string(what) + " must be sorted and unique");
    }
  }
  return Status::Ok();
}

}  // namespace

std::string_view RiskTierToken(RiskTier tier) noexcept {
  switch (tier) {
    case RiskTier::Nominal: return "nominal";
    case RiskTier::Watch: return "watch";
    case RiskTier::Degraded: return "degraded";
    case RiskTier::Critical: return "critical";
  }
  return "unknown";
}

Result<RiskTier> ParseRiskTier(std::string_view token) noexcept {
  if (token == "nominal") return RiskTier::Nominal;
  if (token == "watch") return RiskTier::Watch;
  if (token == "degraded") return RiskTier::Degraded;
  if (token == "critical") return RiskTier::Critical;
  return Fail<RiskTier>(ErrorCode::InvalidArgument, "unknown risk tier token");
}

std::string_view PriorityToken(PriorityClass priority) noexcept {
  switch (priority) {
    case PriorityClass::BestEffort: return "best_effort";
    case PriorityClass::Low: return "low";
    case PriorityClass::Normal: return "normal";
    case PriorityClass::High: return "high";
    case PriorityClass::Critical: return "critical";
  }
  return "unknown";
}

Result<PriorityClass> ParsePriority(std::string_view token) noexcept {
  if (token == "best_effort") return PriorityClass::BestEffort;
  if (token == "low") return PriorityClass::Low;
  if (token == "normal") return PriorityClass::Normal;
  if (token == "high") return PriorityClass::High;
  if (token == "critical") return PriorityClass::Critical;
  return Fail<PriorityClass>(ErrorCode::InvalidArgument, "unknown priority token");
}

std::string_view FairnessToken(FairnessPolicy policy) noexcept {
  switch (policy) {
    case FairnessPolicy::StableSiteOrder: return "stable_site_order";
    case FairnessPolicy::LowestCostFirst: return "lowest_cost_first";
    case FairnessPolicy::EqualShareAcrossSites: return "equal_share_across_sites";
    case FairnessPolicy::ProportionalToAllocatable: return "proportional_to_allocatable";
  }
  return "unknown";
}

Result<FairnessPolicy> ParseFairness(std::string_view token) noexcept {
  if (token == "stable_site_order") return FairnessPolicy::StableSiteOrder;
  if (token == "lowest_cost_first") return FairnessPolicy::LowestCostFirst;
  if (token == "equal_share_across_sites") return FairnessPolicy::EqualShareAcrossSites;
  if (token == "proportional_to_allocatable") return FairnessPolicy::ProportionalToAllocatable;
  return Fail<FairnessPolicy>(ErrorCode::InvalidArgument, "unknown fairness policy token");
}

std::string_view ShrinkPolicyToken(ShrinkPolicy policy) noexcept {
  switch (policy) {
    case ShrinkPolicy::RefuseShrink: return "refuse_shrink";
    case ShrinkPolicy::EvictToFit: return "evict_to_fit";
  }
  return "unknown";
}

Result<ShrinkPolicy> ParseShrinkPolicy(std::string_view token) noexcept {
  if (token == "refuse_shrink") return ShrinkPolicy::RefuseShrink;
  if (token == "evict_to_fit") return ShrinkPolicy::EvictToFit;
  return Fail<ShrinkPolicy>(ErrorCode::InvalidArgument, "unknown shrink policy token");
}

std::string_view DurabilityToken(DurabilityClass durability) noexcept {
  switch (durability) {
    case DurabilityClass::Durable: return "durable";
    case DurabilityClass::Buffered: return "buffered";
    case DurabilityClass::Volatile: return "volatile";
  }
  return "unknown";
}

std::string_view RevocationReasonToken(RevocationReason reason) noexcept {
  switch (reason) {
    case RevocationReason::SupersededByGeneration: return "superseded_by_generation";
    case RevocationReason::OfferRevoked: return "offer_revoked";
    case RevocationReason::CapacityShrink: return "capacity_shrink";
  }
  return "unknown";
}

std::string_view ConstraintToken(ConstraintKind kind) noexcept {
  switch (kind) {
    case ConstraintKind::NoOffersForServiceClass: return "no_offers_for_service_class";
    case ConstraintKind::OfferNotYetValid: return "offer_not_yet_valid";
    case ConstraintKind::OfferExpired: return "offer_expired";
    case ConstraintKind::OfferRevoked: return "offer_revoked";
    case ConstraintKind::OfferSuperseded: return "offer_superseded";
    case ConstraintKind::GenerationPinMismatch: return "generation_pin_mismatch";
    case ConstraintKind::RegionNotAllowed: return "region_not_allowed";
    case ConstraintKind::RegionExcluded: return "region_excluded";
    case ConstraintKind::JurisdictionNotAllowed: return "jurisdiction_not_allowed";
    case ConstraintKind::JurisdictionExcluded: return "jurisdiction_excluded";
    case ConstraintKind::SiteNotAllowed: return "site_not_allowed";
    case ConstraintKind::SiteExcluded: return "site_excluded";
    case ConstraintKind::FailureDomainExcluded: return "failure_domain_excluded";
    case ConstraintKind::RiskAboveCeiling: return "risk_above_ceiling";
    case ConstraintKind::DiversityNotMet: return "failure_domain_diversity_not_met";
    case ConstraintKind::SiteLimitExceeded: return "site_limit_exceeded";
    case ConstraintKind::ReserveNotAuthorized: return "protected_reserve_not_authorized";
    case ConstraintKind::ReservePolicyMismatch: return "reserve_policy_mismatch";
    case ConstraintKind::PolicyGenerationMismatch: return "policy_generation_mismatch";
    case ConstraintKind::InsufficientAllocatableCapacity: return "insufficient_allocatable_capacity";
    case ConstraintKind::InsufficientReserveCapacity: return "insufficient_reserve_capacity";
    case ConstraintKind::AllOrNothingNotMet: return "all_or_nothing_not_met";
    case ConstraintKind::SingleSourceNotMet: return "single_source_not_met";
    case ConstraintKind::CostCeilingExceeded: return "cost_ceiling_exceeded";
    case ConstraintKind::EnergyCeilingExceeded: return "energy_ceiling_exceeded";
    case ConstraintKind::CarbonCeilingExceeded: return "carbon_ceiling_exceeded";
    case ConstraintKind::CapacityEvictedByShrink: return "capacity_evicted_by_shrink";
    case ConstraintKind::LedgerLimitReached: return "ledger_limit_reached";
    case ConstraintKind::NoEligibleOffer: return "no_eligible_offer";
  }
  return "unknown";
}

std::string_view DecisionOutcomeToken(DecisionOutcome outcome) noexcept {
  switch (outcome) {
    case DecisionOutcome::Accepted: return "accepted";
    case DecisionOutcome::PartiallyAccepted: return "partially_accepted";
    case DecisionOutcome::Refused: return "refused";
  }
  return "unknown";
}

std::string BlockingConstraint::ToString() const {
  std::string text(ConstraintToken(kind));
  text.push_back('(');
  bool first = true;
  const auto separator = [&text, &first]() {
    if (!first) {
      text.push_back(',');
    }
    first = false;
  };
  if (!site.empty()) {
    separator();
    text += "site=";
    text += site.value();
  }
  if (!domain.empty()) {
    separator();
    text += "domain=";
    text += domain.value();
  }
  if (has_dimension) {
    separator();
    text += "dimension=";
    text += DimensionToken(dimension);
  }
  if (required != 0 || available != 0) {
    separator();
    text += "required=";
    text += std::to_string(required);
    text.push_back(',');
    text += "available=";
    text += std::to_string(available);
  }
  if (!detail.empty()) {
    separator();
    text += "detail=";
    text += detail;
  }
  text.push_back(')');
  return text;
}

Result<CapacityVector> Allocation::Total() const {
  return CapacityVector::Add(allocatable_amount, reserve_amount, Limits::kMaxLedgerTotal);
}

Status ValidateOffer(const Offer& offer) {
  Status status = RequireId(offer.site, "site");
  if (!status.ok()) return status;
  status = RequireId(offer.source_snapshot, "source_snapshot");
  if (!status.ok()) return status;
  status = RequireId(offer.service_class, "service_class");
  if (!status.ok()) return status;
  status = RequireId(offer.region, "region");
  if (!status.ok()) return status;
  status = RequireId(offer.jurisdiction, "jurisdiction");
  if (!status.ok()) return status;
  status = RequireId(offer.reserve_policy, "reserve_policy");
  if (!status.ok()) return status;
  status = RequireId(offer.cost.reference, "cost.reference");
  if (!status.ok()) return status;

  if (offer.generation == 0) {
    return Status::Error(ErrorCode::InvalidRequest, "offer generation must be positive");
  }
  if (offer.valid_until < offer.valid_from) {
    return Status::Error(ErrorCode::InvalidRequest,
                         "offer validity window ends before it begins");
  }
  if (offer.valid_from < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "offer validity window starts before zero");
  }
  if (offer.tranches.empty()) {
    return Status::Error(ErrorCode::InvalidRequest, "offer has no capacity tranches");
  }
  if (offer.tranches.size() > Limits::kMaxTranchesPerOffer) {
    return Status::Error(ErrorCode::LimitExceeded, "offer has too many capacity tranches");
  }
  status = ValidateNonNegative(offer.cost.price_per_milli_unit, "cost.price_per_milli_unit");
  if (!status.ok()) return status;
  if (offer.cost.energy_millijoules_per_milli_unit < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "cost energy evidence must not be negative");
  }
  if (offer.cost.carbon_milligrams_per_milli_unit < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "cost carbon evidence must not be negative");
  }

  CapacityVector allocatable_total;
  CapacityVector reserve_total;
  for (std::size_t i = 0; i < offer.tranches.size(); ++i) {
    const CapacityTranche& tranche = offer.tranches[i];
    status = RequireId(tranche.domain, "tranche domain");
    if (!status.ok()) return status;
    if (!tranche.allocatable.IsNonNegative() || !tranche.protected_reserve.IsNonNegative()) {
      return Status::Error(ErrorCode::InvalidRequest, "tranche capacity must not be negative");
    }
    if (i > 0 && !(offer.tranches[i - 1].domain < tranche.domain)) {
      return Status::Error(ErrorCode::DuplicateIdentity,
                           "offer tranche failure domains must be sorted and unique");
    }
    Result<CapacityVector> next_allocatable =
        CapacityVector::Add(allocatable_total, tranche.allocatable, Limits::kMaxOfferTotal);
    if (!next_allocatable.ok()) return next_allocatable.status();
    allocatable_total = next_allocatable.value();
    Result<CapacityVector> next_reserve =
        CapacityVector::Add(reserve_total, tranche.protected_reserve, Limits::kMaxOfferTotal);
    if (!next_reserve.ok()) return next_reserve.status();
    reserve_total = next_reserve.value();
  }
  if (!allocatable_total.HasAny() && !reserve_total.HasAny()) {
    return Status::Error(ErrorCode::InvalidRequest,
                         "offer declares neither allocatable nor protected reserve capacity");
  }
  return Status::Ok();
}

Result<Offer> NormalizeOffer(Offer offer) {
  std::sort(offer.tranches.begin(), offer.tranches.end(),
            [](const CapacityTranche& a, const CapacityTranche& b) { return a.domain < b.domain; });
  return offer;
}

Result<CapacityVector> OfferAllocatableTotal(const Offer& offer) {
  CapacityVector total;
  for (const CapacityTranche& tranche : offer.tranches) {
    Result<CapacityVector> next =
        CapacityVector::Add(total, tranche.allocatable, Limits::kMaxOfferTotal);
    if (!next.ok()) {
      return next;
    }
    total = next.value();
  }
  return total;
}

Result<CapacityVector> OfferReserveTotal(const Offer& offer) {
  CapacityVector total;
  for (const CapacityTranche& tranche : offer.tranches) {
    Result<CapacityVector> next =
        CapacityVector::Add(total, tranche.protected_reserve, Limits::kMaxOfferTotal);
    if (!next.ok()) {
      return next;
    }
    total = next.value();
  }
  return total;
}

Status ValidateAsk(const Ask& ask) {
  Status status = RequireId(ask.key, "key");
  if (!status.ok()) return status;
  status = RequireId(ask.requester, "requester");
  if (!status.ok()) return status;
  status = RequireId(ask.service_class, "service_class");
  if (!status.ok()) return status;

  if (!ask.requested.HasAny()) {
    return Status::Error(ErrorCode::InvalidRequest, "ask requests no capacity");
  }
  if (!ask.requested.IsNonNegative()) {
    return Status::Error(ErrorCode::InvalidRequest, "ask requests negative capacity");
  }
  if (ask.as_of < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "ask evaluation instant is before zero");
  }
  if (ask.min_distinct_failure_domains > static_cast<u32>(Limits::kMaxListElements)) {
    return Status::Error(ErrorCode::OutOfRange, "ask requires too many distinct failure domains");
  }
  if (ask.max_sites > static_cast<u32>(Limits::kMaxListElements)) {
    return Status::Error(ErrorCode::OutOfRange, "ask site limit is too large");
  }
  if (ask.require_single_source && ask.max_sites > 1) {
    return Status::Error(ErrorCode::InvalidRequest,
                         "a single-source ask cannot also permit more than one site");
  }

  status = ValidateListSize(ask.allowed_regions.size(), "allowed_regions");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.excluded_regions.size(), "excluded_regions");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.allowed_jurisdictions.size(), "allowed_jurisdictions");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.excluded_jurisdictions.size(), "excluded_jurisdictions");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.allowed_sites.size(), "allowed_sites");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.excluded_sites.size(), "excluded_sites");
  if (!status.ok()) return status;
  status = ValidateListSize(ask.excluded_failure_domains.size(), "excluded_failure_domains");
  if (!status.ok()) return status;
  if (ask.generation_pins.size() > Limits::kMaxGenerationPins) {
    return Status::Error(ErrorCode::LimitExceeded, "ask has too many generation pins");
  }

  status = ValidateDisjoint(ask.allowed_regions, ask.excluded_regions, "region");
  if (!status.ok()) return status;
  status = ValidateDisjoint(ask.allowed_jurisdictions, ask.excluded_jurisdictions, "jurisdiction");
  if (!status.ok()) return status;
  status = ValidateDisjoint(ask.allowed_sites, ask.excluded_sites, "site");
  if (!status.ok()) return status;

  status = ValidateSortedUnique(ask.allowed_regions, "allowed_regions");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.excluded_regions, "excluded_regions");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.allowed_jurisdictions, "allowed_jurisdictions");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.excluded_jurisdictions, "excluded_jurisdictions");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.allowed_sites, "allowed_sites");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.excluded_sites, "excluded_sites");
  if (!status.ok()) return status;
  status = ValidateSortedUnique(ask.excluded_failure_domains, "excluded_failure_domains");
  if (!status.ok()) return status;

  for (std::size_t i = 0; i < ask.generation_pins.size(); ++i) {
    if (ask.generation_pins[i].site.empty()) {
      return Status::Error(ErrorCode::MissingField, "generation pin names no site");
    }
    if (ask.generation_pins[i].generation == 0) {
      return Status::Error(ErrorCode::InvalidRequest, "generation pin must be positive");
    }
    if (i > 0 && !(ask.generation_pins[i - 1].site < ask.generation_pins[i].site)) {
      return Status::Error(ErrorCode::DuplicateIdentity,
                           "generation pins must be sorted and unique by site");
    }
  }

  if (ask.has_cost_ceiling && ask.max_total_cost.IsNegative()) {
    return Status::Error(ErrorCode::InvalidRequest, "cost ceiling must not be negative");
  }
  if (ask.has_energy_ceiling && ask.max_total_energy_millijoules < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "energy ceiling must not be negative");
  }
  if (ask.has_carbon_ceiling && ask.max_total_carbon_milligrams < 0) {
    return Status::Error(ErrorCode::InvalidRequest, "carbon ceiling must not be negative");
  }
  if (ask.may_consume_protected_reserve && ask.reserve_policy_authorization.empty()) {
    return Status::Error(ErrorCode::MissingField,
                         "an ask that may consume protected reserve must name the policy it "
                         "claims authority under");
  }
  if (ask.has_policy_generation_pin && ask.policy_generation_pin == 0) {
    return Status::Error(ErrorCode::InvalidRequest, "policy generation pin must be positive");
  }
  return Status::Ok();
}

}  // namespace rcb
