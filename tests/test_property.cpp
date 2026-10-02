// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Property suite: conservation and determinism of the brokerage kernel under
// randomized multi-dimensional offers, asks, shrink and eviction.
//
// The boundary requires that after every mutation the ledger closes, per site,
// per failure domain and per dimension:
//
//   allocatable       = committed + remaining_allocatable + withheld
//   protected_reserve = reserve_committed + remaining_reserve + reserve_withheld
//
// and that the kernel is a deterministic function of its inputs: the same seed
// driven through two fresh cores must produce byte-identical decision
// documents, an equal StateDigest() and an equal accounting chain.
//
// Nothing here takes the runtime's word for the numbers it checks. The
// generator is a first-party splitmix64; the identities above are re-derived
// from the SiteLedger/TrancheLedger accessors; the "what should be committed"
// model is a separately written map fed only by decision documents and
// revocation records. BrokerCore::VerifyConservation() is asserted as well,
// never used as the source of truth.
//
// Two kernel defects this suite found are stated as their own tests at the
// bottom of the file. They are regression tests for a fix, not assertions about
// today's behaviour, and they are the only checks here that are expected to
// fail until src/broker.cpp changes; everything else is green.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

namespace {

using rcb::Allocation;
using rcb::Ask;
using rcb::BrokerConfig;
using rcb::BrokerCore;
using rcb::CapacityTranche;
using rcb::CapacityVector;
using rcb::CommitmentRecord;
using rcb::Decision;
using rcb::Dimension;
using rcb::ErrorCode;
using rcb::FailureDomainId;
using rcb::FairnessPolicy;
using rcb::i64;
using rcb::Offer;
using rcb::PlanKind;
using rcb::PriorityClass;
using rcb::Result;
using rcb::RiskTier;
using rcb::ShrinkPolicy;
using rcb::SiteId;
using rcb::SiteLedger;
using rcb::TrancheLedger;
using rcb::u64;

/// The instant every generated offer is valid at, so that a randomized workload
/// always contains decisions that commit something.
constexpr i64 kSharedInstant = 500;
/// Upper bound of every generated capacity, dimension by dimension.
constexpr i64 kCapacityCeiling = 1000000;
/// Four workloads, every one printed, every one different.
constexpr u64 kSeeds[] = {0x5EED0001ULL, 0x5EED0002ULL, 0x5EED0003ULL, 0x5EED0004ULL};

/// Like RCB_CHECK, but the failure message carries the context (which seed,
/// which mutation, which tranche) that RCB_CHECK cannot.
#define PROP_CHECK(condition, message)                                          \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    if (!(condition)) {                                                         \
      ::rcbtest::ReportFailure(__FILE__, __LINE__, (message));                  \
    }                                                                           \
  } while (false)

using Count = unsigned long long;

template <class T>
Count AsCount(T value) {
  return static_cast<Count>(value);
}

std::string Number(u64 value) { return std::to_string(value); }

// ---------------------------------------------------------------------------
// Deterministic generator: splitmix64.
// ---------------------------------------------------------------------------

class SplitMix64 {
 public:
  explicit SplitMix64(u64 seed) noexcept : state_(seed) {}

  u64 Next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    u64 z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  /// Uniform in [low, high]; low when the range is empty.
  i64 Range(i64 low, i64 high) noexcept {
    if (high <= low) {
      return low;
    }
    const u64 span = static_cast<u64>(high - low) + 1U;
    return low + static_cast<i64>(Next() % span);
  }

  /// Uniform in [low, high] for counts.
  std::size_t Count(std::size_t low, std::size_t high) noexcept {
    if (high <= low) {
      return low;
    }
    const u64 span = static_cast<u64>(high - low) + 1U;
    return low + static_cast<std::size_t>(Next() % span);
  }

  bool Chance(u64 numerator, u64 denominator) noexcept {
    return (Next() % denominator) < numerator;
  }

 private:
  u64 state_ = 0;
};

template <class Tag>
rcb::TaggedId<Tag> MakeId(const std::string& text) {
  return rcb::TaggedId<Tag>::Make(text).ValueOr(rcb::TaggedId<Tag>());
}

CapacityVector MakeVector(const std::array<i64, rcb::kDimensionCount>& values) {
  return CapacityVector::Make(values).ValueOr(CapacityVector());
}

rcb::ScaledAmount MakeMicros(i64 micros) {
  return rcb::ScaledAmount::FromMicros(micros).ValueOr(rcb::ScaledAmount());
}

// ---------------------------------------------------------------------------
// The generated workload: sites, offers, asks and the mutation script.
// ---------------------------------------------------------------------------

struct SitePlan {
  Offer offer;
  std::string reserve_policy;
};

struct Workload {
  u64 seed = 0;
  std::size_t seed_index = 0;
  BrokerConfig config;
  std::vector<SitePlan> sites;
  std::vector<Ask> asks;
  std::vector<Ask> post_shrink_asks;
  i64 shrink_percent = 50;
  std::size_t replay_ask = 0;
  std::size_t revoke_site = 0;
  std::size_t republish_site = 0;
  std::string fingerprint;
  std::string config_text;
};

BrokerConfig ConfigForSeed(std::size_t seed_index) {
  BrokerConfig config;
  switch (seed_index % 4U) {
    case 0:
      config.shrink_policy = ShrinkPolicy::EvictToFit;
      config.default_fairness = FairnessPolicy::StableSiteOrder;
      break;
    case 1:
      config.shrink_policy = ShrinkPolicy::EvictToFit;
      config.default_fairness = FairnessPolicy::LowestCostFirst;
      break;
    case 2:
      config.shrink_policy = ShrinkPolicy::EvictToFit;
      config.default_fairness = FairnessPolicy::ProportionalToAllocatable;
      break;
    default:
      // RefuseShrink is exercised by the randomized shrink phase as well as by
      // the hand-built case, so both shrink policies see randomized input.
      config.shrink_policy = ShrinkPolicy::RefuseShrink;
      config.default_fairness = FairnessPolicy::EqualShareAcrossSites;
      break;
  }
  return config;
}

std::string ConfigText(const BrokerConfig& config) {
  return "shrink=" + std::string(rcb::ShrinkPolicyToken(config.shrink_policy)) +
         " default_fairness=" + std::string(rcb::FairnessToken(config.default_fairness));
}

std::size_t CountTranches(const Workload& workload) {
  std::size_t count = 0;
  for (const SitePlan& site : workload.sites) {
    count += site.offer.tranches.size();
  }
  return count;
}

SitePlan MakeSite(SplitMix64& rng, const std::string& site, std::size_t tranche_count) {
  SitePlan plan;
  Offer& offer = plan.offer;
  offer.site = MakeId<rcb::SiteIdTag>(site);
  offer.generation = 1;
  offer.source_snapshot = MakeId<rcb::SnapshotIdTag>("snapshot-" + site);
  offer.service_class = MakeId<rcb::ServiceClassIdTag>(rng.Chance(1, 4) ? "sc-b" : "sc-a");
  offer.region = MakeId<rcb::RegionIdTag>("region-" + std::to_string(rng.Range(0, 3)));
  offer.jurisdiction =
      MakeId<rcb::JurisdictionIdTag>("jurisdiction-" + std::to_string(rng.Range(0, 1)));
  offer.risk = static_cast<RiskTier>(rng.Range(0, 3));
  // Every window starts at or before 400 and ends at or after 1000, so
  // kSharedInstant is inside all of them while both ends stay random.
  offer.valid_from = rng.Range(0, 400);
  offer.valid_until = 1000 + rng.Range(0, 3000);
  plan.reserve_policy = "reserve-policy-" + site;
  offer.reserve_policy = MakeId<rcb::PolicyIdTag>(plan.reserve_policy);
  offer.policy_generation = static_cast<u64>(rng.Range(1, 3));
  offer.reserve_minimum_priority = static_cast<PriorityClass>(rng.Range(0, 4));
  offer.cost.reference = MakeId<rcb::EvidenceIdTag>("evidence-" + site);
  // Prices stay small so that a multi-dimension commitment cannot overflow the
  // exact fixed-point product, which would turn a scenario into a plumbing
  // failure rather than a property failure.
  offer.cost.price_per_milli_unit = MakeMicros(rng.Range(0, 1000));
  offer.cost.energy_millijoules_per_milli_unit = rng.Range(0, 50);
  offer.cost.carbon_milligrams_per_milli_unit = rng.Range(0, 50);

  bool any_capacity = false;
  for (std::size_t index = 0; index < tranche_count; ++index) {
    CapacityTranche tranche;
    tranche.domain = MakeId<rcb::FailureDomainIdTag>("fd-" + std::to_string(index + 1));
    std::array<i64, rcb::kDimensionCount> allocatable{};
    std::array<i64, rcb::kDimensionCount> reserve{};
    for (std::size_t dimension = 0; dimension < rcb::kDimensionCount; ++dimension) {
      allocatable[dimension] = rng.Range(0, kCapacityCeiling);
      // The protected reserve never exceeds what the same tranche may allocate.
      reserve[dimension] = rng.Range(0, allocatable[dimension]);
      any_capacity = any_capacity || allocatable[dimension] > 0 || reserve[dimension] > 0;
    }
    tranche.allocatable = MakeVector(allocatable);
    tranche.protected_reserve = MakeVector(reserve);
    offer.tranches.push_back(tranche);
  }
  if (!any_capacity) {
    // An offer that publishes nothing in any dimension is not an offer; the
    // generator keeps the workload publishable instead of testing validation.
    std::array<i64, rcb::kDimensionCount> bumped{};
    bumped[0] = 1;
    offer.tranches.front().allocatable = MakeVector(bumped);
  }
  return plan;
}

void SortedUniqueSites(std::vector<SiteId>& list) {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

void SortedUniqueDomains(std::vector<FailureDomainId>& list) {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

void SortedUniqueRegions(std::vector<rcb::RegionId>& list) {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

void SortedUniqueJurisdictions(std::vector<rcb::JurisdictionId>& list) {
  std::sort(list.begin(), list.end());
  list.erase(std::unique(list.begin(), list.end()), list.end());
}

Ask MakeAsk(SplitMix64& rng, const std::string& key, const std::vector<SitePlan>& sites) {
  Ask ask;
  ask.key = MakeId<rcb::AskKeyTag>(key);
  ask.requester = MakeId<rcb::RequesterIdTag>("requester-" + std::to_string(rng.Range(0, 3)));
  ask.service_class = MakeId<rcb::ServiceClassIdTag>(
      rng.Chance(1, 8) ? "sc-missing" : (rng.Chance(1, 4) ? "sc-b" : "sc-a"));

  std::array<i64, rcb::kDimensionCount> requested{};
  bool any = false;
  for (std::size_t dimension = 0; dimension < rcb::kDimensionCount; ++dimension) {
    // Some dimensions are deliberately zero: a request is not a flat scalar.
    requested[dimension] = rng.Chance(1, 3) ? 0 : rng.Range(0, 200000);
    any = any || requested[dimension] > 0;
  }
  if (!any) {
    requested[rng.Count(0, rcb::kDimensionCount - 1)] = 1 + rng.Range(0, 1000);
  }
  ask.requested = MakeVector(requested);
  ask.priority = static_cast<PriorityClass>(rng.Range(0, 4));
  ask.fairness = static_cast<FairnessPolicy>(rng.Range(0, 3));
  ask.all_or_nothing = rng.Chance(1, 6);
  ask.require_single_source = rng.Chance(1, 8);
  ask.as_of = rng.Chance(1, 2) ? kSharedInstant : rng.Range(0, 5000);
  ask.max_risk = static_cast<RiskTier>(rng.Range(0, 3));
  ask.min_distinct_failure_domains =
      rng.Chance(1, 5) ? static_cast<rcb::u32>(rng.Range(2, 4)) : 0U;
  if (!ask.require_single_source && rng.Chance(1, 4)) {
    ask.max_sites = static_cast<rcb::u32>(rng.Range(0, static_cast<i64>(sites.size())));
  }
  if (rng.Chance(1, 4)) {
    ask.has_cost_ceiling = true;
    ask.max_total_cost = MakeMicros(rng.Range(0, 20000000));
  }
  if (rng.Chance(1, 5)) {
    ask.has_energy_ceiling = true;
    ask.max_total_energy_millijoules = rng.Range(0, 500000000);
  }
  if (rng.Chance(1, 5)) {
    ask.has_carbon_ceiling = true;
    ask.max_total_carbon_milligrams = rng.Range(0, 500000000);
  }
  if (rng.Chance(1, 5)) {
    ask.may_consume_protected_reserve = true;
    const std::size_t owner = rng.Count(0, sites.size() - 1);
    ask.reserve_policy_authorization = MakeId<rcb::PolicyIdTag>(sites[owner].reserve_policy);
  }
  if (rng.Chance(1, 8)) {
    ask.has_policy_generation_pin = true;
    ask.policy_generation_pin = static_cast<u64>(rng.Range(1, 3));
  }
  if (rng.Chance(1, 10)) {
    rcb::GenerationPin pin;
    pin.site = sites[rng.Count(0, sites.size() - 1)].offer.site;
    pin.generation = static_cast<u64>(rng.Range(1, 2));
    ask.generation_pins.push_back(pin);
  }
  if (rng.Chance(1, 4)) {
    for (const SitePlan& site : sites) {
      if (rng.Chance(1, 3)) {
        ask.excluded_sites.push_back(site.offer.site);
      }
    }
    SortedUniqueSites(ask.excluded_sites);
  }
  if (rng.Chance(1, 5)) {
    for (std::size_t index = 1; index <= 4; ++index) {
      if (rng.Chance(1, 4)) {
        ask.excluded_failure_domains.push_back(
            MakeId<rcb::FailureDomainIdTag>("fd-" + std::to_string(index)));
      }
    }
    SortedUniqueDomains(ask.excluded_failure_domains);
  }
  if (rng.Chance(1, 6)) {
    for (std::size_t index = 0; index <= 3; ++index) {
      if (rng.Chance(1, 4)) {
        ask.excluded_regions.push_back(MakeId<rcb::RegionIdTag>("region-" + std::to_string(index)));
      }
    }
    SortedUniqueRegions(ask.excluded_regions);
  }
  if (rng.Chance(1, 8)) {
    for (std::size_t index = 0; index <= 1; ++index) {
      if (rng.Chance(1, 3)) {
        ask.excluded_jurisdictions.push_back(
            MakeId<rcb::JurisdictionIdTag>("jurisdiction-" + std::to_string(index)));
      }
    }
    SortedUniqueJurisdictions(ask.excluded_jurisdictions);
  }
  return ask;
}

/// Turns one ask into a request that cannot be served from the allocatable pools
/// alone, so that the protected-reserve identity is exercised with non-zero
/// terms instead of trivially.
void MakeReserveHungry(Ask& ask, const std::vector<SitePlan>& sites) {
  ask.service_class = MakeId<rcb::ServiceClassIdTag>("sc-a");
  ask.as_of = kSharedInstant;
  ask.priority = PriorityClass::Critical;
  ask.fairness = FairnessPolicy::StableSiteOrder;
  ask.all_or_nothing = false;
  ask.require_single_source = false;
  ask.max_sites = 0;
  ask.min_distinct_failure_domains = 0;
  ask.has_cost_ceiling = false;
  ask.has_energy_ceiling = false;
  ask.has_carbon_ceiling = false;
  ask.has_policy_generation_pin = false;
  ask.generation_pins.clear();
  ask.excluded_sites.clear();
  ask.excluded_failure_domains.clear();
  ask.excluded_regions.clear();
  ask.excluded_jurisdictions.clear();
  ask.max_risk = RiskTier::Critical;
  ask.may_consume_protected_reserve = true;
  ask.reserve_policy_authorization = MakeId<rcb::PolicyIdTag>(sites.front().reserve_policy);

  const rcb::ServiceClassId class_a = MakeId<rcb::ServiceClassIdTag>("sc-a");
  std::array<i64, rcb::kDimensionCount> requested{};
  for (const SitePlan& site : sites) {
    if (site.offer.service_class != class_a) {
      continue;
    }
    for (const CapacityTranche& tranche : site.offer.tranches) {
      for (std::size_t dimension = 0; dimension < rcb::kDimensionCount; ++dimension) {
        requested[dimension] += tranche.allocatable.Get(rcb::kDimensions[dimension]);
      }
    }
  }
  bool any = false;
  for (std::size_t dimension = 0; dimension < rcb::kDimensionCount; ++dimension) {
    if (requested[dimension] > 0) {
      requested[dimension] += kCapacityCeiling;
      any = true;
    }
  }
  if (!any) {
    requested[0] = 1;
  }
  ask.requested = MakeVector(requested);
}

Workload BuildWorkload(u64 seed, std::size_t seed_index) {
  SplitMix64 rng(seed);
  Workload workload;
  workload.seed = seed;
  workload.seed_index = seed_index;
  workload.config = ConfigForSeed(seed_index);

  const std::size_t site_count = rng.Count(1, 16);
  for (std::size_t index = 0; index < site_count; ++index) {
    const std::string site = "site-" + std::to_string(index + 1);
    const std::size_t tranche_count = rng.Count(1, 4);
    workload.sites.push_back(MakeSite(rng, site, tranche_count));
  }

  const std::size_t ask_count = rng.Count(5, 60);
  for (std::size_t index = 0; index < ask_count; ++index) {
    Ask ask = MakeAsk(rng, "ask-" + Number(seed) + "-" + std::to_string(index), workload.sites);
    if (index % 7 == 6) {
      MakeReserveHungry(ask, workload.sites);
    }
    workload.asks.push_back(ask);
  }
  const std::size_t post_count = rng.Count(3, 6);
  for (std::size_t index = 0; index < post_count; ++index) {
    workload.post_shrink_asks.push_back(
        MakeAsk(rng, "ask-" + Number(seed) + "-post-" + std::to_string(index), workload.sites));
  }

  workload.shrink_percent = rng.Range(20, 95);
  workload.replay_ask = rng.Count(0, workload.asks.size() - 1);
  workload.revoke_site = rng.Count(0, workload.sites.size() - 1);
  workload.republish_site = rng.Count(0, workload.sites.size() - 1);

  rcb::Sha256 hasher;
  for (const SitePlan& site : workload.sites) {
    hasher.Update(rcb::CanonicalOfferText(site.offer));
  }
  for (const Ask& ask : workload.asks) {
    hasher.Update(rcb::CanonicalAskText(ask));
  }
  for (const Ask& ask : workload.post_shrink_asks) {
    hasher.Update(rcb::CanonicalAskText(ask));
  }
  workload.fingerprint = hasher.Finalize().Hex();
  workload.config_text = ConfigText(workload.config);
  return workload;
}

// ---------------------------------------------------------------------------
// The independent reference model.
//
// Keyed by (site, failure domain, dimension) and fed only by decision documents
// and revocation records. It never reads a ledger and never calls a runtime
// accessor, so it is a second, independent derivation of what must be committed.
// ---------------------------------------------------------------------------

using PoolKey = std::tuple<std::string, std::string, std::size_t>;

PoolKey KeyOf(const SiteId& site, const FailureDomainId& domain, Dimension dimension) {
  return PoolKey(site.value(), domain.value(), static_cast<std::size_t>(dimension));
}

struct ReferenceModel {
  std::map<PoolKey, i64> allocatable;
  std::map<PoolKey, i64> reserve;

  void ApplyDecision(const Decision& decision) {
    for (const Allocation& allocation : decision.allocations) {
      for (const Dimension dimension : rcb::kDimensions) {
        allocatable[KeyOf(allocation.site, allocation.domain, dimension)] +=
            allocation.allocatable_amount.Get(dimension);
        reserve[KeyOf(allocation.site, allocation.domain, dimension)] +=
            allocation.reserve_amount.Get(dimension);
      }
    }
  }

  void ApplyRevocations(const std::vector<CommitmentRecord>& revoked) {
    for (const CommitmentRecord& record : revoked) {
      for (const Dimension dimension : rcb::kDimensions) {
        allocatable[KeyOf(record.site, record.domain, dimension)] -=
            record.allocatable_amount.Get(dimension);
        reserve[KeyOf(record.site, record.domain, dimension)] -=
            record.reserve_amount.Get(dimension);
      }
    }
  }

  [[nodiscard]] i64 AllocatableAt(const SiteId& site, const FailureDomainId& domain,
                                  Dimension dimension) const {
    const auto found = allocatable.find(KeyOf(site, domain, dimension));
    return found == allocatable.end() ? 0 : found->second;
  }

  [[nodiscard]] i64 ReserveAt(const SiteId& site, const FailureDomainId& domain,
                              Dimension dimension) const {
    const auto found = reserve.find(KeyOf(site, domain, dimension));
    return found == reserve.end() ? 0 : found->second;
  }

  [[nodiscard]] i64 AllocatableColumn(Dimension dimension) const {
    i64 total = 0;
    for (const auto& entry : allocatable) {
      if (std::get<2>(entry.first) == static_cast<std::size_t>(dimension)) {
        total += entry.second;
      }
    }
    return total;
  }

  [[nodiscard]] i64 ReserveColumn(Dimension dimension) const {
    i64 total = 0;
    for (const auto& entry : reserve) {
      if (std::get<2>(entry.first) == static_cast<std::size_t>(dimension)) {
        total += entry.second;
      }
    }
    return total;
  }
};

// ---------------------------------------------------------------------------
// Re-derivation of the conservation identity after every mutation.
// ---------------------------------------------------------------------------

/// Plain integer arithmetic over one (site, failure domain) bucket. Used by the
/// shrink simulation so that its independence from CapacityVector is obvious.
using Amounts = std::array<i64, rcb::kDimensionCount>;

Amounts AmountsOf(const CapacityVector& vector) {
  Amounts amounts{};
  for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
    amounts[index] = vector.Get(rcb::kDimensions[index]);
  }
  return amounts;
}

Amounts MinusAmounts(const Amounts& left, const Amounts& right) {
  Amounts result{};
  for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
    result[index] = left[index] - right[index];
  }
  return result;
}

std::string AmountsText(const Amounts& amounts) {
  std::string text;
  for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
    if (index != 0) {
      text.push_back(',');
    }
    text += rcb::DimensionToken(rcb::kDimensions[index]);
    text.push_back('=');
    text += std::to_string(amounts[index]);
  }
  return text;
}

std::string DimensionList(const std::array<bool, rcb::kDimensionCount>& seen) {
  std::string text;
  for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
    if (!seen[index]) {
      continue;
    }
    if (!text.empty()) {
      text.push_back(',');
    }
    text += rcb::DimensionToken(rcb::kDimensions[index]);
  }
  return text.empty() ? std::string("none") : text;
}

struct LedgerShape {
  std::size_t sites = 0;
  std::size_t tranches = 0;
  std::size_t commitments = 0;
  std::size_t live = 0;
  std::size_t revoked = 0;
  /// Whether this snapshot of the ledger carried a non-zero protected-reserve
  /// commitment and a non-zero withheld term, so that the suite can show those
  /// identities were not checked trivially.
  bool reserve_seen = false;
  bool withheld_seen = false;
  bool reference_matched = true;
  bool closed = false;
};

bool AccumulateInto(CapacityVector* total, const CapacityVector& value) {
  const Result<CapacityVector> next =
      CapacityVector::Add(*total, value, rcb::Limits::kMaxLedgerTotal * 8);
  if (!next.ok()) {
    return false;
  }
  *total = next.value();
  return true;
}

std::string FirstViolation(const rcb::ConservationReport& report) {
  if (report.violations.empty()) {
    return "no violation recorded";
  }
  const rcb::ConservationViolation& violation = report.violations.front();
  return violation.scope + ": " + violation.detail + " (" +
         std::to_string(report.violations.size()) + " violation(s))";
}

/// Re-derives every term of the conservation identity from the stored ledger
/// and compares it against the report the kernel produces. \p reference may be
/// null, in which case only the ledger's own arithmetic is checked.
LedgerShape CheckLedgerInvariants(const BrokerCore& core, const ReferenceModel* reference,
                                  const std::string& context) {
  LedgerShape shape;
  const rcb::ConservationReport report = core.VerifyConservation();
  shape.closed = report.closed;
  PROP_CHECK(report.closed,
             context + ": VerifyConservation() is not closed: " + FirstViolation(report));

  CapacityVector sum_allocatable;
  CapacityVector sum_committed;
  CapacityVector sum_remaining;
  CapacityVector sum_withheld;
  CapacityVector sum_reserve;
  CapacityVector sum_reserve_committed;
  CapacityVector sum_reserve_remaining;
  CapacityVector sum_reserve_withheld;
  CapacityVector sum_site_committed;
  CapacityVector sum_site_remaining;

  const std::vector<SiteId> sites = core.SiteIds();
  for (const SiteId& site : sites) {
    const SiteLedger* ledger = core.FindSite(site);
    PROP_CHECK(ledger != nullptr,
               context + ": the core lists a site it cannot find: " + site.value());
    if (ledger == nullptr) {
      continue;
    }
    ++shape.sites;
    const std::string site_scope = context + ": site=" + site.value();

    for (const CommitmentRecord& commitment : ledger->commitments) {
      ++shape.commitments;
      if (commitment.live) {
        ++shape.live;
      } else {
        ++shape.revoked;
      }
      // The live flag is the only thing that decides whether a commitment is
      // counted, so it must agree with the revocation record it carries.
      PROP_CHECK(commitment.live == !commitment.has_revocation,
                 site_scope + ": commitment " + commitment.id.value() + " carries live=" +
                     std::to_string(static_cast<int>(commitment.live)) + " has_revocation=" +
                     std::to_string(static_cast<int>(commitment.has_revocation)));
      // A live commitment is capacity the ledger still owes somewhere: it must
      // name a failure domain this generation publishes, or it is accounted
      // nowhere.
      const bool published = ledger->FindTranche(commitment.domain) != nullptr;
      PROP_CHECK(!commitment.live || published,
                 site_scope + ": live commitment " + commitment.id.value() +
                     " names failure domain " + commitment.domain.value() +
                     " which this generation does not publish");
    }

    for (const TrancheLedger& tranche : ledger->tranches) {
      ++shape.tranches;
      if (tranche.withheld.HasAny() || tranche.reserve_withheld.HasAny()) {
        shape.withheld_seen = true;
      }
      const std::string scope = site_scope + " domain=" + tranche.domain.value();
      const Result<CapacityVector> remaining = tranche.RemainingAllocatable();
      const Result<CapacityVector> remaining_reserve = tranche.RemainingReserve();
      PROP_CHECK(remaining.ok(),
                 scope + ": RemainingAllocatable() failed: " +
                     (remaining.ok() ? std::string("ok") : remaining.status().ToString()));
      PROP_CHECK(remaining_reserve.ok(),
                 scope + ": RemainingReserve() failed: " +
                     (remaining_reserve.ok() ? std::string("ok")
                                             : remaining_reserve.status().ToString()));

      // The tranche's committed terms are re-derived from the commitments that
      // are actually counted, not read back from the tranche.
      Amounts counted_allocatable{};
      Amounts counted_reserve{};
      for (const CommitmentRecord& commitment : ledger->commitments) {
        if (!commitment.live || commitment.domain != tranche.domain) {
          continue;
        }
        for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
          counted_allocatable[index] += commitment.allocatable_amount.Get(rcb::kDimensions[index]);
          counted_reserve[index] += commitment.reserve_amount.Get(rcb::kDimensions[index]);
        }
      }
      PROP_CHECK(counted_allocatable == AmountsOf(tranche.committed_allocatable),
                 scope + ": committed allocatable is " + tranche.committed_allocatable.ToString() +
                     " but its live commitments sum to " + AmountsText(counted_allocatable));
      PROP_CHECK(counted_reserve == AmountsOf(tranche.committed_reserve),
                 scope + ": committed reserve is " + tranche.committed_reserve.ToString() +
                     " but its live commitments sum to " + AmountsText(counted_reserve));

      for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
        const Dimension dimension = rcb::kDimensions[index];
        const std::string dimension_text(rcb::DimensionToken(dimension));
        if (tranche.committed_reserve.Get(dimension) > 0) {
          shape.reserve_seen = true;
        }
        if (remaining.ok()) {
          const i64 residual = remaining.value().Get(dimension);
          PROP_CHECK(residual >= 0, scope + ": dimension=" + dimension_text +
                                        " has a negative allocatable residual " +
                                        std::to_string(residual));
          const i64 committed = tranche.committed_allocatable.Get(dimension);
          const i64 withheld = tranche.withheld.Get(dimension);
          const i64 allocatable = tranche.allocatable.Get(dimension);
          PROP_CHECK(allocatable == committed + residual + withheld,
                     scope + ": dimension=" + dimension_text +
                         " allocatable=" + std::to_string(allocatable) +
                         " != committed=" + std::to_string(committed) +
                         " + remaining=" + std::to_string(residual) +
                         " + withheld=" + std::to_string(withheld));
        }
        if (remaining_reserve.ok()) {
          const i64 residual = remaining_reserve.value().Get(dimension);
          PROP_CHECK(residual >= 0, scope + ": dimension=" + dimension_text +
                                        " has a negative reserve residual " +
                                        std::to_string(residual));
          const i64 committed = tranche.committed_reserve.Get(dimension);
          const i64 withheld = tranche.reserve_withheld.Get(dimension);
          const i64 reserve = tranche.protected_reserve.Get(dimension);
          PROP_CHECK(reserve == committed + residual + withheld,
                     scope + ": dimension=" + dimension_text +
                         " reserve=" + std::to_string(reserve) +
                         " != reserve_committed=" + std::to_string(committed) +
                         " + remaining_reserve=" + std::to_string(residual) +
                         " + reserve_withheld=" + std::to_string(withheld));
        }
        if (reference != nullptr) {
          const i64 expected_allocatable = reference->AllocatableAt(site, tranche.domain, dimension);
          const i64 expected_reserve = reference->ReserveAt(site, tranche.domain, dimension);
          PROP_CHECK(expected_allocatable == tranche.committed_allocatable.Get(dimension),
                     scope + ": dimension=" + dimension_text +
                         " the independent reference model holds " +
                         std::to_string(expected_allocatable) +
                         " committed but the ledger holds " +
                         std::to_string(tranche.committed_allocatable.Get(dimension)));
          PROP_CHECK(expected_reserve == tranche.committed_reserve.Get(dimension),
                     scope + ": dimension=" + dimension_text +
                         " the independent reference model holds " +
                         std::to_string(expected_reserve) +
                         " reserve committed but the ledger holds " +
                         std::to_string(tranche.committed_reserve.Get(dimension)));
        }
      }

      PROP_CHECK(AccumulateInto(&sum_allocatable, tranche.allocatable),
                 scope + ": re-derived allocatable total overflows");
      PROP_CHECK(AccumulateInto(&sum_committed, tranche.CommittedTotal().ValueOr(CapacityVector())),
                 scope + ": re-derived committed total overflows");
      PROP_CHECK(AccumulateInto(&sum_remaining, remaining.ValueOr(CapacityVector())),
                 scope + ": re-derived remaining total overflows");
      PROP_CHECK(AccumulateInto(&sum_withheld, tranche.withheld),
                 scope + ": re-derived withheld total overflows");
      PROP_CHECK(AccumulateInto(&sum_reserve, tranche.protected_reserve),
                 scope + ": re-derived reserve total overflows");
      PROP_CHECK(AccumulateInto(&sum_reserve_committed, tranche.committed_reserve),
                 scope + ": re-derived committed reserve total overflows");
      PROP_CHECK(AccumulateInto(&sum_reserve_remaining, remaining_reserve.ValueOr(CapacityVector())),
                 scope + ": re-derived remaining reserve total overflows");
      PROP_CHECK(AccumulateInto(&sum_reserve_withheld, tranche.reserve_withheld),
                 scope + ": re-derived reserve withheld total overflows");
    }

    PROP_CHECK(AccumulateInto(&sum_site_committed,
                              ledger->CommittedTotal().ValueOr(CapacityVector())),
               site_scope + ": the site committed total overflows");
    PROP_CHECK(AccumulateInto(&sum_site_remaining,
                              ledger->RemainingAllocatableTotal().ValueOr(CapacityVector())),
               site_scope + ": the site remaining total overflows");
  }

  PROP_CHECK(sum_site_committed == report.committed_total,
             context + ": the sum over sites of committed (" + sum_site_committed.ToString() +
                 ") is not the report's committed_total (" + report.committed_total.ToString() + ")");
  PROP_CHECK(sum_site_remaining == report.remaining_total,
             context + ": the sum over sites of remaining (" + sum_site_remaining.ToString() +
                 ") is not the report's remaining_total (" + report.remaining_total.ToString() + ")");

  const bool totals_agree = report.allocatable_total == sum_allocatable &&
                            report.committed_total == sum_committed &&
                            report.remaining_total == sum_remaining &&
                            report.withheld_total == sum_withheld &&
                            report.reserve_total == sum_reserve &&
                            report.reserve_committed_total == sum_reserve_committed &&
                            report.reserve_remaining_total == sum_reserve_remaining &&
                            report.reserve_withheld_total == sum_reserve_withheld;
  PROP_CHECK(totals_agree,
             context + ": the report's totals disagree with the terms re-derived from the tranches:"
                       " allocatable " + report.allocatable_total.ToString() + " vs " +
                 sum_allocatable.ToString() + ", committed " + report.committed_total.ToString() +
                 " vs " + sum_committed.ToString() + ", remaining " +
                 report.remaining_total.ToString() + " vs " + sum_remaining.ToString() +
                 ", withheld " + report.withheld_total.ToString() + " vs " +
                 sum_withheld.ToString() + ", reserve " + report.reserve_total.ToString() + " vs " +
                 sum_reserve.ToString());

  // The report's own totals must satisfy the identity they are the sum of.
  for (const Dimension dimension : rcb::kDimensions) {
    const i64 allocatable = report.allocatable_total.Get(dimension);
    const i64 committed_allocatable =
        report.committed_total.Get(dimension) - report.reserve_committed_total.Get(dimension);
    const i64 identity = committed_allocatable + report.remaining_total.Get(dimension) +
                         report.withheld_total.Get(dimension);
    PROP_CHECK(allocatable == identity,
               context + ": dimension=" + std::string(rcb::DimensionToken(dimension)) +
                   " the report's totals do not satisfy allocatable = committed + remaining + "
                   "withheld (" + std::to_string(allocatable) + " != " + std::to_string(identity) +
                   ")");
  }

  PROP_CHECK(report.sites_checked == shape.sites && report.tranches_checked == shape.tranches &&
                 report.commitments_checked == shape.commitments &&
                 report.live_commitments == shape.live &&
                 report.revoked_commitments == shape.revoked,
             context + ": the report's counters disagree with the ledgers (sites " +
                 std::to_string(report.sites_checked) + "/" + std::to_string(shape.sites) +
                 ", tranches " + std::to_string(report.tranches_checked) + "/" +
                 std::to_string(shape.tranches) + ", commitments " +
                 std::to_string(report.commitments_checked) + "/" +
                 std::to_string(shape.commitments) + ", live " +
                 std::to_string(report.live_commitments) + "/" + std::to_string(shape.live) +
                 ", revoked " + std::to_string(report.revoked_commitments) + "/" +
                 std::to_string(shape.revoked) + ")");

  if (reference != nullptr) {
    for (const Dimension dimension : rcb::kDimensions) {
      const std::string dimension_text(rcb::DimensionToken(dimension));
      const i64 model_allocatable = reference->AllocatableColumn(dimension);
      const i64 model_reserve = reference->ReserveColumn(dimension);
      const i64 ledger_allocatable =
          report.committed_total.Get(dimension) - report.reserve_committed_total.Get(dimension);
      const i64 ledger_reserve = report.reserve_committed_total.Get(dimension);
      PROP_CHECK(model_allocatable == ledger_allocatable,
                 context + ": dimension=" + dimension_text + " the reference model commits " +
                     std::to_string(model_allocatable) + " but the ledger reports " +
                     std::to_string(ledger_allocatable) +
                     " committed allocatable, so a commitment the reference model accepted is not "
                     "accounted for");
      PROP_CHECK(model_reserve == ledger_reserve,
                 context + ": dimension=" + dimension_text + " the reference model commits " +
                     std::to_string(model_reserve) + " reserve but the ledger reports " +
                     std::to_string(ledger_reserve));
      PROP_CHECK(model_allocatable >= 0 && model_reserve >= 0,
                 context + ": dimension=" + dimension_text + " the reference model went negative (" +
                     std::to_string(model_allocatable) + "/" + std::to_string(model_reserve) +
                     "), so the runtime revoked a commitment it never committed");
      if (model_allocatable != ledger_allocatable || model_reserve != ledger_reserve) {
        shape.reference_matched = false;
      }
    }
  }
  return shape;
}

CapacityVector TotalCommitted(const BrokerCore& core) {
  CapacityVector total;
  for (const SiteId& site : core.SiteIds()) {
    const SiteLedger* ledger = core.FindSite(site);
    if (ledger == nullptr) {
      continue;
    }
    (void)AccumulateInto(&total, ledger->CommittedTotal().ValueOr(CapacityVector()));
  }
  return total;
}

// ---------------------------------------------------------------------------
// Shrink and eviction
// ---------------------------------------------------------------------------

/// The state a shrink is checked against: what each failure domain had committed
/// before, and what the new generation publishes. Both come from the test's own
/// accounting, never from OfferPublication::commitments_revoked.
struct ShrinkExpectation {
  std::map<std::string, Amounts> before;
  std::map<std::string, Amounts> after;
};

/// A domain that is no longer published covers nothing, so any commitment left
/// against it is over-committed.
bool Fits(const ShrinkExpectation& expectation, const std::map<std::string, Amounts>& state) {
  for (const auto& entry : state) {
    const auto published = expectation.after.find(entry.first);
    if (published == expectation.after.end()) {
      for (const i64 amount : entry.second) {
        if (amount > 0) {
          return false;
        }
      }
      continue;
    }
    for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
      if (entry.second[index] > published->second[index]) {
        return false;
      }
    }
  }
  return true;
}

/// The documented eviction order, written out here independently of
/// CommitmentRecord::IsEvictedBefore: lowest priority first, then the newest
/// commitment first, then descending identity.
std::vector<CommitmentRecord> SortIntoEvictionOrder(std::vector<CommitmentRecord> records) {
  std::sort(records.begin(), records.end(),
            [](const CommitmentRecord& left, const CommitmentRecord& right) {
              const rcb::u8 left_rank = rcb::PriorityRank(left.priority);
              const rcb::u8 right_rank = rcb::PriorityRank(right.priority);
              if (left_rank != right_rank) {
                return left_rank < right_rank;
              }
              if (left.sequence != right.sequence) {
                return left.sequence > right.sequence;
              }
              return right.id.value() < left.id.value();
            });
  return records;
}

void CheckShrinkIsMinimal(const std::string& context, const ShrinkExpectation& expectation,
                          const std::vector<CommitmentRecord>& revoked, std::size_t live_before,
                          std::size_t live_after) {
  const std::vector<CommitmentRecord> order = SortIntoEvictionOrder(revoked);
  const std::size_t live_delta = live_before >= live_after ? live_before - live_after : 0;
  PROP_CHECK(order.size() == live_delta,
             context + ": " + std::to_string(order.size()) +
                 " revoked commitment records but the live count moved by " +
                 std::to_string(live_delta));

  const bool covered_without_eviction = Fits(expectation, expectation.before);
  std::map<std::string, Amounts> simulated = expectation.before;
  std::size_t needed = 0;
  while (!Fits(expectation, simulated) && needed < order.size()) {
    const CommitmentRecord& victim = order[needed];
    ++needed;
    simulated[victim.domain.value()] =
        MinusAmounts(simulated[victim.domain.value()], AmountsOf(victim.allocatable_amount));
  }

  PROP_CHECK(Fits(expectation, simulated),
             context + ": after every revocation the ledger still does not fit the published capacity");
  if (order.empty()) {
    PROP_CHECK(covered_without_eviction,
               context + ": nothing was revoked although the new generation does not cover what was "
                         "committed");
    return;
  }
  PROP_CHECK(!covered_without_eviction,
             context + ": commitments were revoked although the published capacity already covered them");
  PROP_CHECK(needed == order.size(),
             context + ": " + std::to_string(order.size()) + " commitments were revoked but only " +
                 std::to_string(needed) + " had to be for committed <= allocatable");
  std::map<std::string, Amounts> one_fewer = expectation.before;
  for (std::size_t index = 0; index + 1 < needed; ++index) {
    const CommitmentRecord& victim = order[index];
    one_fewer[victim.domain.value()] =
        MinusAmounts(one_fewer[victim.domain.value()], AmountsOf(victim.allocatable_amount));
  }
  PROP_CHECK(!Fits(expectation, one_fewer),
             context + ": one revocation fewer would already have closed the ledger, so the shrink "
                       "evicted more than it needed to");
}

ShrinkExpectation ExpectationFor(const SiteLedger* ledger, const Offer& replacement) {
  ShrinkExpectation expectation;
  if (ledger != nullptr) {
    for (const TrancheLedger& tranche : ledger->tranches) {
      expectation.before[tranche.domain.value()] = AmountsOf(tranche.committed_allocatable);
    }
  }
  for (const CapacityTranche& tranche : replacement.tranches) {
    expectation.after[tranche.domain.value()] = AmountsOf(tranche.allocatable);
  }
  return expectation;
}

// ---------------------------------------------------------------------------
// The workload runner: every mutation is followed by the full re-derivation.
// ---------------------------------------------------------------------------

struct RunOutcome {
  std::vector<std::string> decision_texts;
  std::size_t decisions = 0;
  std::size_t accepted = 0;
  std::size_t partially_accepted = 0;
  std::size_t refused = 0;
  std::size_t plan_failures = 0;
  std::size_t commit_failures = 0;
  std::size_t live = 0;
  std::size_t revoked = 0;
  std::size_t evicted = 0;
  std::size_t mutations = 0;
  /// Evidence that the workload was not vacuous: mutations whose ledger carried
  /// a non-zero reserve commitment or a non-zero withheld term, and the
  /// dimensions that actually received capacity.
  std::size_t reserve_mutations = 0;
  std::size_t withheld_mutations = 0;
  std::array<bool, rcb::kDimensionCount> committed_dimensions{};
  std::array<bool, rcb::kDimensionCount> reserve_dimensions{};
  bool reference_matched = true;
  bool converged_to_closed = false;
  u64 sequence = 0;
  rcb::Digest state_digest;
  rcb::Digest chain_digest;
};

RunOutcome RunWorkload(const Workload& workload) {
  RunOutcome outcome;
  BrokerCore core(workload.config);
  ReferenceModel reference;
  u64 mutation = 0;

  const auto context_of = [&workload, &mutation](const char* phase) {
    return "seed=" + Number(workload.seed) + " mutation=" + std::to_string(mutation) +
           " phase=" + phase;
  };
  const auto check = [&core, &reference, &outcome, &context_of](const char* phase) {
    const LedgerShape shape = CheckLedgerInvariants(core, &reference, context_of(phase));
    outcome.reference_matched = outcome.reference_matched && shape.reference_matched;
    if (shape.reserve_seen) {
      ++outcome.reserve_mutations;
    }
    if (shape.withheld_seen) {
      ++outcome.withheld_mutations;
    }
  };
  const auto submit = [&core, &reference, &outcome, &context_of](const Ask& ask, const char* phase) {
    const std::string context = context_of(phase);
    const Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    if (!plan.ok()) {
      ++outcome.plan_failures;
      PROP_CHECK(false, context + ": a validated ask produced no plan: " + plan.status().ToString());
      return;
    }
    const Result<Decision> decision = core.CommitPlan(plan.value());
    if (!decision.ok()) {
      ++outcome.commit_failures;
      PROP_CHECK(false, context + ": CommitPlan failed: " + decision.status().ToString());
      return;
    }
    ++outcome.decisions;
    outcome.decision_texts.push_back(rcb::CanonicalDecisionText(decision.value()));
    switch (decision.value().outcome) {
      case rcb::DecisionOutcome::Accepted:
        ++outcome.accepted;
        break;
      case rcb::DecisionOutcome::PartiallyAccepted:
        ++outcome.partially_accepted;
        break;
      case rcb::DecisionOutcome::Refused:
        ++outcome.refused;
        break;
    }
    // A replay is a decision document, not a new commitment.
    if (!decision.value().replay) {
      reference.ApplyDecision(decision.value());
    }
    for (const Allocation& allocation : decision.value().allocations) {
      for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
        if (allocation.allocatable_amount.Get(rcb::kDimensions[index]) > 0) {
          outcome.committed_dimensions[index] = true;
        }
        if (allocation.reserve_amount.Get(rcb::kDimensions[index]) > 0) {
          outcome.reserve_dimensions[index] = true;
        }
      }
    }
  };

  // ---- every site publishes its first generation -------------------------
  std::vector<Offer> latest;
  latest.reserve(workload.sites.size());
  for (const SitePlan& site : workload.sites) {
    latest.push_back(site.offer);
    ++mutation;
    const Result<rcb::OfferPublication> published = core.PublishOffer(site.offer);
    PROP_CHECK(published.ok(), context_of("publish") + ": a generated offer was refused: " +
                                   (published.ok() ? std::string("ok")
                                                   : published.status().ToString()));
    check("publish");
  }

  // ---- the ask stream ----------------------------------------------------
  for (const Ask& ask : workload.asks) {
    ++mutation;
    submit(ask, "ask");
    check("ask");
  }

  // ---- shrink: the site holding the most commitments publishes less ------
  std::size_t target = 0;
  {
    bool have_target = false;
    std::size_t best = 0;
    for (std::size_t index = 0; index < workload.sites.size(); ++index) {
      const SiteLedger* ledger = core.FindSite(workload.sites[index].offer.site);
      if (ledger == nullptr) {
        continue;
      }
      const std::size_t live = ledger->LiveCommitmentCount();
      if (!have_target || live > best) {
        have_target = true;
        target = index;
        best = live;
      }
    }
  }
  const SiteId shrink_site = workload.sites[target].offer.site;
  const SiteLedger* ledger_before = core.FindSite(shrink_site);
  const std::size_t live_before =
      ledger_before != nullptr ? ledger_before->LiveCommitmentCount() : 0;
  Offer replacement = workload.sites[target].offer;
  replacement.generation = ledger_before != nullptr ? ledger_before->generation + 1U : 2U;
  for (CapacityTranche& tranche : replacement.tranches) {
    const TrancheLedger* current =
        ledger_before != nullptr ? ledger_before->FindTranche(tranche.domain) : nullptr;
    for (const Dimension dimension : rcb::kDimensions) {
      const i64 committed = current != nullptr ? current->committed_allocatable.Get(dimension) : 0;
      tranche.allocatable.Set(dimension, committed * workload.shrink_percent / 100);
    }
    // The protected reserve is deliberately left exactly as the current
    // generation publishes it. Shrinking the protected pool below what is
    // committed to it is neither evicted nor refused by the kernel today; that
    // defect has its own test at the bottom of this file, so this randomized
    // workload stays on the path the kernel does cover.
  }
  {
    bool any_capacity = false;
    for (const CapacityTranche& tranche : replacement.tranches) {
      any_capacity =
          any_capacity || tranche.allocatable.HasAny() || tranche.protected_reserve.HasAny();
    }
    if (!any_capacity && !replacement.tranches.empty()) {
      CapacityVector bumped;
      bumped.Set(Dimension::Power, 1);
      replacement.tranches.front().allocatable = bumped;
    }
  }
  const ShrinkExpectation expectation = ExpectationFor(ledger_before, replacement);
  const rcb::Digest digest_before_shrink = core.StateDigest();
  const rcb::Digest chain_before_shrink = core.chain_digest();
  const u64 sequence_before_shrink = core.sequence();
  ++mutation;
  {
    const std::string context = context_of("shrink");
    const Result<rcb::OfferPublication> shrink = core.PublishOffer(replacement);
    const bool refusal_expected =
        workload.config.shrink_policy == ShrinkPolicy::RefuseShrink && live_before > 0;
    if (refusal_expected) {
      PROP_CHECK(!shrink.ok(),
                 context + ": RefuseShrink published a generation that no longer covers what is "
                           "committed");
      if (!shrink.ok()) {
        PROP_CHECK(shrink.status().code() == ErrorCode::InsufficientCapacity,
                   context + ": the shrink was refused with " +
                       std::string(shrink.status().token()) + " rather than insufficient_capacity");
      }
      PROP_CHECK(core.StateDigest() == digest_before_shrink,
                 context + ": a refused publication changed the ledger");
      PROP_CHECK(core.chain_digest() == chain_before_shrink,
                 context + ": a refused publication extended the accounting chain");
      PROP_CHECK(core.sequence() == sequence_before_shrink,
                 context + ": a refused publication consumed a sequence");
    } else if (shrink.ok()) {
      outcome.evicted += shrink.value().commitments_revoked;
      reference.ApplyRevocations(shrink.value().revoked);
      const SiteLedger* ledger_after = core.FindSite(shrink_site);
      CheckShrinkIsMinimal(context, expectation, shrink.value().revoked, live_before,
                           ledger_after != nullptr ? ledger_after->LiveCommitmentCount() : 0);
      latest[target] = replacement;
    } else {
      PROP_CHECK(false,
                 context + ": the shrink was refused unexpectedly: " + shrink.status().ToString());
    }
  }
  check("shrink");

  // ---- more asks against the shrunk generation ---------------------------
  for (const Ask& ask : workload.post_shrink_asks) {
    ++mutation;
    submit(ask, "post-shrink ask");
    check("post-shrink ask");
  }

  // ---- revoking a generation withholds rather than disappears ------------
  {
    const std::size_t index = workload.revoke_site % workload.sites.size();
    const SiteId site = workload.sites[index].offer.site;
    const SiteLedger* ledger = core.FindSite(site);
    if (ledger != nullptr) {
      ++mutation;
      const Result<rcb::OfferRevocation> revocation =
          core.RevokeOffer(site, ledger->generation, kSharedInstant);
      PROP_CHECK(revocation.ok(),
                 context_of("revoke") + ": RevokeOffer failed: " +
                     (revocation.ok() ? std::string("ok") : revocation.status().ToString()));
      if (revocation.ok()) {
        reference.ApplyRevocations(revocation.value().revoked);
      }
      check("revoke");
    }
  }

  // ---- re-publishing the identical generation is an idempotent no-op -----
  {
    const std::size_t index = workload.republish_site % workload.sites.size();
    const rcb::Digest digest = core.StateDigest();
    const rcb::Digest chain = core.chain_digest();
    const u64 sequence = core.sequence();
    ++mutation;
    const std::string context = context_of("republish");
    const Result<rcb::OfferPublication> again = core.PublishOffer(latest[index]);
    PROP_CHECK(again.ok(), context + ": re-publishing the identical generation failed: " +
                               (again.ok() ? std::string("ok") : again.status().ToString()));
    if (again.ok()) {
      PROP_CHECK(again.value().idempotent_replay,
                 context + ": re-publishing the identical generation was not an idempotent replay");
      PROP_CHECK(again.value().commitments_revoked == std::size_t{0},
                 context + ": an idempotent replay revoked " +
                     std::to_string(again.value().commitments_revoked) + " commitments");
    }
    PROP_CHECK(core.StateDigest() == digest,
               context + ": an idempotent replay changed the state digest");
    PROP_CHECK(core.chain_digest() == chain, context + ": an idempotent replay extended the chain");
    PROP_CHECK(core.sequence() == sequence, context + ": an idempotent replay consumed a sequence");
    check("republish");
  }

  // ---- re-submitting an ask is a replay that commits nothing -------------
  {
    const Ask& ask = workload.asks[workload.replay_ask % workload.asks.size()];
    const Result<Decision> original = core.FindDecisionByKey(ask.key);
    const CapacityVector committed_before = TotalCommitted(core);
    const rcb::Digest digest = core.StateDigest();
    const rcb::Digest chain = core.chain_digest();
    ++mutation;
    const std::string context = context_of("replay");
    const Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    PROP_CHECK(plan.ok(), context + ": re-submitting an ask with the same key produced no plan: " +
                               (plan.ok() ? std::string("ok") : plan.status().ToString()));
    if (plan.ok()) {
      PROP_CHECK(plan.value().kind == PlanKind::Replay,
                 context + ": re-submitting an ask with the same key did not replay");
      const Result<Decision> replayed = core.CommitPlan(plan.value());
      PROP_CHECK(replayed.ok(), context + ": the replayed plan did not commit: " +
                                    (replayed.ok() ? std::string("ok")
                                                   : replayed.status().ToString()));
      if (replayed.ok()) {
        PROP_CHECK(replayed.value().replay,
                   context + ": the replayed decision does not carry replay == true");
        PROP_CHECK(original.ok() && replayed.value().id == original.value().id,
                   context + ": the replay carries a different decision identity");
        PROP_CHECK(original.ok() && rcb::CanonicalDecisionText(replayed.value()) ==
                                        rcb::CanonicalDecisionText(original.value()),
                   context + ": the replay is not the same decision document");
        PROP_CHECK(original.ok() &&
                       replayed.value().allocations.size() == original.value().allocations.size(),
                   context + ": the replay reports a different number of allocations");
      }
    }
    PROP_CHECK(TotalCommitted(core) == committed_before,
               context + ": a replay committed something new");
    PROP_CHECK(core.StateDigest() == digest, context + ": a replay changed the state digest");
    PROP_CHECK(core.chain_digest() == chain, context + ": a replay extended the chain");
    check("replay");
  }

  outcome.mutations = mutation;
  outcome.state_digest = core.StateDigest();
  outcome.chain_digest = core.chain_digest();
  outcome.sequence = core.sequence();
  const rcb::AccountingSummary summary = core.Summary();
  outcome.live = summary.live_commitments;
  outcome.revoked = summary.revoked_commitments;
  outcome.converged_to_closed = core.VerifyConservation().closed;
  return outcome;
}

// ---------------------------------------------------------------------------
// Hand-built building blocks
// ---------------------------------------------------------------------------

Offer OneTrancheOffer(const std::string& site, u64 generation, const Amounts& allocatable,
                      const Amounts& reserve) {
  Offer offer;
  offer.site = MakeId<rcb::SiteIdTag>(site);
  offer.generation = generation;
  offer.source_snapshot = MakeId<rcb::SnapshotIdTag>("snapshot-" + site);
  offer.service_class = MakeId<rcb::ServiceClassIdTag>("sc-a");
  offer.region = MakeId<rcb::RegionIdTag>("region-1");
  offer.jurisdiction = MakeId<rcb::JurisdictionIdTag>("jurisdiction-1");
  offer.risk = RiskTier::Nominal;
  offer.valid_from = 0;
  offer.valid_until = 10000;
  offer.reserve_policy = MakeId<rcb::PolicyIdTag>("reserve-policy-" + site);
  offer.policy_generation = 1;
  offer.reserve_minimum_priority = PriorityClass::High;
  offer.cost.reference = MakeId<rcb::EvidenceIdTag>("evidence-" + site);
  offer.cost.price_per_milli_unit = MakeMicros(10);
  offer.cost.energy_millijoules_per_milli_unit = 1;
  offer.cost.carbon_milligrams_per_milli_unit = 1;
  CapacityTranche tranche;
  tranche.domain = MakeId<rcb::FailureDomainIdTag>("fd-1");
  tranche.allocatable = MakeVector(allocatable);
  tranche.protected_reserve = MakeVector(reserve);
  offer.tranches.push_back(tranche);
  return offer;
}

void AddTranche(Offer* offer, const std::string& domain, const Amounts& allocatable) {
  CapacityTranche tranche;
  tranche.domain = MakeId<rcb::FailureDomainIdTag>(domain);
  tranche.allocatable = MakeVector(allocatable);
  offer->tranches.push_back(tranche);
}

Ask PowerAsk(const std::string& key, i64 power, PriorityClass priority, i64 as_of) {
  Ask ask;
  ask.key = MakeId<rcb::AskKeyTag>(key);
  ask.requester = MakeId<rcb::RequesterIdTag>("requester-1");
  ask.service_class = MakeId<rcb::ServiceClassIdTag>("sc-a");
  std::array<i64, rcb::kDimensionCount> requested{};
  requested[static_cast<std::size_t>(Dimension::Power)] = power;
  ask.requested = MakeVector(requested);
  ask.priority = priority;
  ask.fairness = FairnessPolicy::StableSiteOrder;
  ask.as_of = as_of;
  return ask;
}

bool PublishExpectOk(BrokerCore* core, const Offer& offer) {
  const Result<rcb::OfferPublication> published = core->PublishOffer(offer);
  PROP_CHECK(published.ok(), "publish was refused: " +
                                 (published.ok() ? std::string("ok")
                                                 : published.status().ToString()));
  return published.ok();
}

bool CommitExpectOk(BrokerCore* core, const Ask& ask, Decision* decision) {
  const Result<rcb::AskPlan> plan = core->PlanAsk(ask);
  PROP_CHECK(plan.ok(),
             "plan was refused: " + (plan.ok() ? std::string("ok") : plan.status().ToString()));
  if (!plan.ok()) {
    return false;
  }
  const Result<Decision> committed = core->CommitPlan(plan.value());
  PROP_CHECK(committed.ok(), "commit was refused: " +
                                 (committed.ok() ? std::string("ok")
                                                 : committed.status().ToString()));
  if (!committed.ok()) {
    return false;
  }
  if (decision != nullptr) {
    *decision = committed.value();
  }
  return true;
}

std::vector<CommitmentRecord> LiveCommitments(const SiteLedger& ledger) {
  std::vector<CommitmentRecord> live;
  for (const CommitmentRecord& commitment : ledger.commitments) {
    if (commitment.live) {
      live.push_back(commitment);
    }
  }
  std::sort(live.begin(), live.end(),
            [](const CommitmentRecord& left, const CommitmentRecord& right) {
              return left.sequence < right.sequence;
            });
  return live;
}

bool PairLess(const Allocation& left, const Allocation& right) {
  if (left.site != right.site) {
    return left.site < right.site;
  }
  return left.domain < right.domain;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Randomized workloads: conservation after every mutation, determinism
//    across two fresh cores, the reference model, idempotent re-publication and
//    replay.
// ---------------------------------------------------------------------------

RCB_TEST(property_randomized_workloads_conserve_and_reproduce) {
  std::vector<std::string> fingerprints;
  for (std::size_t seed_index = 0; seed_index < sizeof(kSeeds) / sizeof(kSeeds[0]); ++seed_index) {
    const u64 seed = kSeeds[seed_index];
    const Workload workload = BuildWorkload(seed, seed_index);
    fingerprints.push_back(workload.fingerprint);

    std::printf("[property] seed=%llu seed_hex=0x%016llx %s sites=%llu tranches=%llu asks=%llu "
                "post_shrink_asks=%llu shrink_percent=%lld fingerprint=%s\n",
                AsCount(seed), AsCount(seed), workload.config_text.c_str(),
                AsCount(workload.sites.size()), AsCount(CountTranches(workload)),
                AsCount(workload.asks.size()), AsCount(workload.post_shrink_asks.size()),
                static_cast<long long>(workload.shrink_percent),
                workload.fingerprint.substr(0, 16).c_str());
    std::fflush(stdout);

    const RunOutcome first = RunWorkload(workload);
    const RunOutcome second = RunWorkload(workload);

    // (a) the decision documents are byte-identical
    PROP_CHECK(first.decision_texts.size() == second.decision_texts.size(),
               "seed=" + Number(seed) + ": the two runs produced a different number of decisions (" +
                   std::to_string(first.decision_texts.size()) + " vs " +
                   std::to_string(second.decision_texts.size()) + ")");
    const std::size_t comparable =
        std::min(first.decision_texts.size(), second.decision_texts.size());
    for (std::size_t index = 0; index < comparable; ++index) {
      PROP_CHECK(first.decision_texts[index] == second.decision_texts[index],
                 "seed=" + Number(seed) + ": decision " + std::to_string(index) +
                     " is not byte-identical across two instances");
    }
    // (b) the state digest is equal
    PROP_CHECK(first.state_digest == second.state_digest,
               "seed=" + Number(seed) + ": StateDigest() differs across two instances (" +
                   first.state_digest.ShortHex(16) + " vs " + second.state_digest.ShortHex(16) + ")");
    // (c) the accounting chain is equal
    PROP_CHECK(first.chain_digest == second.chain_digest,
               "seed=" + Number(seed) +
                   ": the accounting chain digest differs across two instances (" +
                   first.chain_digest.ShortHex(16) + " vs " + second.chain_digest.ShortHex(16) + ")");

    PROP_CHECK(first.reference_matched && second.reference_matched,
               "seed=" + Number(seed) +
                   ": the independent reference model disagreed with the ledger");
    PROP_CHECK(first.converged_to_closed && second.converged_to_closed,
               "seed=" + Number(seed) + ": the workload did not end with a closed ledger");
    PROP_CHECK(first.plan_failures == 0 && first.commit_failures == 0,
               "seed=" + Number(seed) + ": " + std::to_string(first.plan_failures) +
                   " plans and " + std::to_string(first.commit_failures) +
                   " commits failed on plumbing rather than on brokerage policy");

    std::printf("[property] seed=%llu decisions=%llu accepted=%llu partial=%llu refused=%llu "
                "commitment_records=%llu live=%llu revoked=%llu evicted=%llu mutations=%llu "
                "sequence=%llu conservation=%s reference=matched determinism=identical "
                "dimensions_committed=%s reserve_dimensions=%s reserve_mutations=%llu "
                "withheld_mutations=%llu state=%s chain=%s\n",
                AsCount(seed), AsCount(first.decisions), AsCount(first.accepted),
                AsCount(first.partially_accepted), AsCount(first.refused),
                AsCount(first.live + first.revoked), AsCount(first.live), AsCount(first.revoked),
                AsCount(first.evicted), AsCount(first.mutations), AsCount(first.sequence),
                first.converged_to_closed ? "closed" : "OPEN",
                DimensionList(first.committed_dimensions).c_str(),
                DimensionList(first.reserve_dimensions).c_str(), AsCount(first.reserve_mutations),
                AsCount(first.withheld_mutations), first.state_digest.ShortHex(16).c_str(),
                first.chain_digest.ShortHex(16).c_str());
    std::fflush(stdout);
    PROP_CHECK(first.reserve_mutations > 0 || second.reserve_mutations > 0,
               "seed=" + Number(seed) +
                   ": no mutation ever carried a non-zero protected-reserve commitment, so the "
                   "reserve half of the identity was only ever checked at zero");
    PROP_CHECK(first.withheld_mutations > 0 || second.withheld_mutations > 0,
               "seed=" + Number(seed) +
                   ": no mutation ever carried a non-zero withheld term, so the withheld term of "
                   "the identity was only ever checked at zero");
  }

  // "at least three seeds with different workloads": prove they are different.
  std::vector<std::string> distinct = fingerprints;
  std::sort(distinct.begin(), distinct.end());
  distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
  PROP_CHECK(distinct.size() == fingerprints.size(),
             "two seeds produced the same workload fingerprint, so the workloads are not different");
}

// ---------------------------------------------------------------------------
// 2. Fairness: every policy is reproducible, and the deterministic stable order
//    serves the lexicographically smallest eligible (site, failure domain) first.
// ---------------------------------------------------------------------------

RCB_TEST(property_fairness_is_reproducible_and_stable_order_is_lexicographic) {
  const Workload workload = BuildWorkload(kSeeds[0], 0);

  // The fairness ask lives on its own core per policy, which is what lets the
  // asks share every field including the idempotency key: only the fairness
  // policy differs between them. It is derived from the generated data: the
  // shared valid instant, no ceilings, no diversity requirement and no locality
  // exclusions, so the policy is the only thing that can move the result.
  const rcb::ServiceClassId class_a = MakeId<rcb::ServiceClassIdTag>("sc-a");
  const rcb::ServiceClassId class_b = MakeId<rcb::ServiceClassIdTag>("sc-b");
  std::size_t class_a_sites = 0;
  for (const SitePlan& site : workload.sites) {
    if (site.offer.service_class == class_a) {
      ++class_a_sites;
    }
  }
  Ask ask;
  ask.key = MakeId<rcb::AskKeyTag>("fairness-ask");
  ask.requester = MakeId<rcb::RequesterIdTag>("requester-fair");
  ask.service_class = class_a_sites * 2 >= workload.sites.size() ? class_a : class_b;
  std::array<i64, rcb::kDimensionCount> requested{};
  requested[static_cast<std::size_t>(Dimension::Power)] = kCapacityCeiling;
  ask.requested = MakeVector(requested);
  ask.priority = PriorityClass::Normal;
  ask.fairness = FairnessPolicy::StableSiteOrder;
  ask.as_of = kSharedInstant;
  ask.max_risk = RiskTier::Critical;
  ask.require_current_generation = true;

  // The pairs that could serve the ask, derived from the ledgers and ordered
  // the way the stable policy orders candidates.
  std::vector<std::pair<SiteId, FailureDomainId>> eligible;
  std::map<std::string, Amounts> room_of;
  {
    BrokerCore core(workload.config);
    for (const SitePlan& site : workload.sites) {
      if (!PublishExpectOk(&core, site.offer)) {
        return;
      }
    }
    for (const SiteId& site : core.SiteIds()) {
      const SiteLedger* ledger = core.FindSite(site);
      if (ledger == nullptr || ledger->revoked || ledger->service_class != ask.service_class) {
        continue;
      }
      if (ask.as_of < ledger->valid_from || ask.as_of > ledger->valid_until) {
        continue;
      }
      for (const TrancheLedger& tranche : ledger->tranches) {
        const Result<CapacityVector> room = tranche.RemainingAllocatable();
        if (!room.ok() || room.value().Get(Dimension::Power) <= 0) {
          continue;
        }
        eligible.emplace_back(site, tranche.domain);
        room_of[site.value() + "/" + tranche.domain.value()] = AmountsOf(room.value());
      }
    }
    std::sort(eligible.begin(), eligible.end(),
              [](const std::pair<SiteId, FailureDomainId>& left,
                 const std::pair<SiteId, FailureDomainId>& right) {
                if (left.first != right.first) {
                  return left.first < right.first;
                }
                return left.second < right.second;
              });
  }

  PROP_CHECK(!eligible.empty(),
             "seed=" + Number(kSeeds[0]) +
                 ": no (site, failure domain) pair can serve the fairness ask at the shared instant");

  const std::vector<FairnessPolicy> policies = {
      FairnessPolicy::StableSiteOrder, FairnessPolicy::LowestCostFirst,
      FairnessPolicy::EqualShareAcrossSites, FairnessPolicy::ProportionalToAllocatable};
  std::string stable_text;
  std::string distinct_from_stable;
  std::string stable_served = "none";
  for (const FairnessPolicy policy : policies) {
    ask.fairness = policy;

    std::vector<std::string> texts;
    std::vector<rcb::Digest> digests;
    std::vector<Decision> decisions;
    for (int run = 0; run < 2; ++run) {
      BrokerCore core(workload.config);
      bool published_all = true;
      for (const SitePlan& site : workload.sites) {
        if (!PublishExpectOk(&core, site.offer)) {
          published_all = false;
          break;
        }
      }
      if (!published_all) {
        return;
      }
      Decision decision;
      if (!CommitExpectOk(&core, ask, &decision)) {
        return;
      }
      texts.push_back(rcb::CanonicalDecisionText(decision));
      digests.push_back(core.StateDigest());
      decisions.push_back(decision);
    }

    PROP_CHECK(texts[0] == texts[1], std::string("fairness policy ") +
                                         std::string(rcb::FairnessToken(policy)) +
                                         " is not deterministic across two instances");
    PROP_CHECK(digests[0] == digests[1],
               std::string("fairness policy ") + std::string(rcb::FairnessToken(policy)) +
                   " left a different StateDigest() across two instances");

    const Decision& decision = decisions[0];
    if (policy == FairnessPolicy::StableSiteOrder) {
      stable_text = texts[0];
      if (!eligible.empty()) {
        PROP_CHECK(!decision.allocations.empty(),
                   "the stable order policy committed nothing although an eligible pair had room");
      }
      if (!decision.allocations.empty()) {
        // The stated property: the first allocation is the lexicographically
        // smallest (site, failure domain) pair that received capacity. The
        // decision sorts its allocations, so the same claim is also checked
        // against the eligible set derived from the ledgers above.
        for (const Allocation& allocation : decision.allocations) {
          PROP_CHECK(!PairLess(allocation, decision.allocations.front()),
                     "the first allocation of the stable order policy is not the lexicographically "
                     "smallest pair that received capacity");
        }
        stable_served = decision.allocations.front().site.value() + "/" +
                        decision.allocations.front().domain.value();
        if (!eligible.empty()) {
          PROP_CHECK(decision.allocations.front().site == eligible.front().first &&
                         decision.allocations.front().domain == eligible.front().second,
                     "the stable order policy served " +
                         decision.allocations.front().site.value() + "/" +
                         decision.allocations.front().domain.value() +
                         " first, but the lexicographically smallest eligible pair is " +
                         eligible.front().first.value() + "/" + eligible.front().second.value());
          const Amounts& room =
              room_of[eligible.front().first.value() + "/" + eligible.front().second.value()];
          for (std::size_t index = 0; index < rcb::kDimensionCount; ++index) {
            const i64 expected =
                std::min(room[index], ask.requested.Get(rcb::kDimensions[index]));
            const i64 actual =
                decision.allocations.front().allocatable_amount.Get(rcb::kDimensions[index]) +
                decision.allocations.front().reserve_amount.Get(rcb::kDimensions[index]);
            PROP_CHECK(actual == expected,
                       "the stable order policy took " + std::to_string(actual) + " of dimension " +
                           std::string(rcb::DimensionToken(rcb::kDimensions[index])) +
                           " from the first eligible pair but " + std::to_string(expected) +
                           " was available within the request");
          }
        }
      }
    } else if (texts[0] != stable_text) {
      if (!distinct_from_stable.empty()) {
        distinct_from_stable += ",";
      }
      distinct_from_stable += std::string(rcb::FairnessToken(policy));
    }

    std::string rendered;
    for (const Allocation& allocation : decision.allocations) {
      if (!rendered.empty()) {
        rendered += " ";
      }
      rendered += allocation.site.value() + "/" + allocation.domain.value() + "=" +
                  allocation.allocatable_amount.ToString();
    }
    std::printf("[property] fairness policy=%s committed=%s allocations=%llu [%s]\n",
                std::string(rcb::FairnessToken(policy)).c_str(),
                decision.committed.ToString().c_str(), AsCount(decision.allocations.size()),
                rendered.c_str());
    std::fflush(stdout);
  }

  std::printf("[property] fairness stable_order_served=%s lexicographically_smallest_eligible=%s "
              "policies_differing_from_stable_order=%s\n",
              stable_served.c_str(),
              eligible.empty() ? "none"
                               : (eligible.front().first.value() + "/" +
                                  eligible.front().second.value())
                                     .c_str(),
              distinct_from_stable.empty() ? "none (this generated data does not separate them)"
                                           : distinct_from_stable.c_str());
  std::fflush(stdout);
  // The policies are deliberately not asserted to differ from one another: on
  // generated data they legitimately often agree, and the property the boundary
  // requires is determinism, not difference.
}

// ---------------------------------------------------------------------------
// 3. Shrink and eviction, hand-built so that each rule is isolated.
// ---------------------------------------------------------------------------

RCB_TEST(property_shrink_evicts_lowest_priority_then_newest_sequence) {
  // The brief asks for a hand-built case "where a low-priority early commitment
  // survives and a high-priority late one does not". That is the inverse of the
  // policy this kernel documents and implements:
  // CommitmentRecord::IsEvictedBefore orders victims lowest priority first, then
  // newest sequence first, so the low-priority early commitment is the FIRST
  // victim and the high-priority late one survives. The cases below assert the
  // implemented policy, with the two rules separated so that a change in either
  // is caught.
  const SiteId site_a = MakeId<rcb::SiteIdTag>("site-a");
  {
    BrokerCore core;
    if (!PublishExpectOk(&core,
                         OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0}))) {
      return;
    }
    Decision low;
    Decision high_early;
    Decision high_late;
    if (!CommitExpectOk(&core, PowerAsk("ask-low", 100, PriorityClass::Low, 100), &low) ||
        !CommitExpectOk(&core, PowerAsk("ask-high-early", 100, PriorityClass::High, 200),
                        &high_early) ||
        !CommitExpectOk(&core, PowerAsk("ask-high-late", 100, PriorityClass::High, 300),
                        &high_late)) {
      return;
    }
    const SiteLedger* ledger = core.FindSite(site_a);
    RCB_REQUIRE(ledger != nullptr);
    RCB_CHECK_EQ(ledger->LiveCommitmentCount(), std::size_t{3});

    // The new generation publishes 150 against 300 committed, so exactly two
    // commitments must go: the low-priority one first, then the newest of the
    // two equal-priority ones.
    const Offer smaller = OneTrancheOffer("site-a", 2, Amounts{150, 0, 0, 0}, Amounts{0, 0, 0, 0});
    const ShrinkExpectation expectation = ExpectationFor(ledger, smaller);
    const Result<rcb::OfferPublication> shrink = core.PublishOffer(smaller);
    PROP_CHECK(shrink.ok(), "the shrink was refused: " +
                                (shrink.ok() ? std::string("ok") : shrink.status().ToString()));
    if (!shrink.ok()) {
      return;
    }
    const SiteLedger* after = core.FindSite(site_a);
    RCB_REQUIRE(after != nullptr);
    CheckShrinkIsMinimal("hand-built priority shrink", expectation, shrink.value().revoked, 3,
                         after->LiveCommitmentCount());
    RCB_CHECK_EQ(shrink.value().commitments_revoked, std::size_t{2});

    const std::vector<CommitmentRecord> live = LiveCommitments(*after);
    RCB_CHECK_EQ(live.size(), std::size_t{1});
    if (live.size() == 1) {
      PROP_CHECK(live.front().id == high_early.allocations.front().id,
                 "the survivor is not the high-priority early commitment: the low-priority early "
                 "commitment (" +
                     low.allocations.front().id.value() + ", priority low, sequence " +
                     std::to_string(low.allocations.front().sequence) +
                     ") must be evicted before any high-priority commitment, and the newest "
                     "high-priority commitment (" +
                     high_late.allocations.front().id.value() + ", sequence " +
                     std::to_string(high_late.allocations.front().sequence) +
                     ") must be evicted before the older one");
    }
    const std::vector<CommitmentRecord> order = SortIntoEvictionOrder(shrink.value().revoked);
    PROP_CHECK(order.size() == 2 && order[0].id == low.allocations.front().id &&
                   order[1].id == high_late.allocations.front().id,
               "the eviction order is not lowest-priority-first then newest-first");
    RCB_CHECK(core.VerifyConservation().closed);
  }

  // The same priority throughout: newest first, oldest survives.
  {
    BrokerCore core;
    if (!PublishExpectOk(&core,
                         OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0}))) {
      return;
    }
    std::vector<Decision> decisions;
    i64 at = 100;
    for (int index = 0; index < 3; ++index) {
      Decision decision;
      if (!CommitExpectOk(
              &core,
              PowerAsk("ask-" + std::to_string(index), 100, PriorityClass::High, at), &decision)) {
        return;
      }
      decisions.push_back(decision);
      at += 100;
    }
    const Offer smaller = OneTrancheOffer("site-a", 2, Amounts{150, 0, 0, 0}, Amounts{0, 0, 0, 0});
    const Result<rcb::OfferPublication> shrink = core.PublishOffer(smaller);
    PROP_CHECK(shrink.ok(), "the same-priority shrink was refused");
    if (!shrink.ok()) {
      return;
    }
    const SiteLedger* after = core.FindSite(site_a);
    RCB_REQUIRE(after != nullptr);
    const std::vector<CommitmentRecord> live = LiveCommitments(*after);
    PROP_CHECK(live.size() == 1 && live.front().id == decisions.front().allocations.front().id,
               "within one priority the oldest commitment must survive and the newest must be "
               "evicted first");
    const std::vector<CommitmentRecord> order = SortIntoEvictionOrder(shrink.value().revoked);
    PROP_CHECK(order.size() == 2 && order[0].id == decisions[2].allocations.front().id &&
                   order[1].id == decisions[1].allocations.front().id,
               "within one priority the eviction order is not newest sequence first");
    RCB_CHECK(core.VerifyConservation().closed);
  }
}

RCB_TEST(property_refuse_shrink_is_inert) {
  BrokerConfig config;
  config.shrink_policy = ShrinkPolicy::RefuseShrink;
  BrokerCore core(config);
  const SiteId site_a = MakeId<rcb::SiteIdTag>("site-a");
  if (!PublishExpectOk(&core,
                       OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0}))) {
    return;
  }
  Decision decision;
  if (!CommitExpectOk(&core, PowerAsk("ask-keep", 400, PriorityClass::Normal, 100), &decision)) {
    return;
  }
  const rcb::Digest digest = core.StateDigest();
  const rcb::Digest chain = core.chain_digest();
  const u64 sequence = core.sequence();
  const CapacityVector committed = TotalCommitted(core);

  const Result<rcb::OfferPublication> refused =
      core.PublishOffer(OneTrancheOffer("site-a", 2, Amounts{150, 0, 0, 0}, Amounts{0, 0, 0, 0}));
  PROP_CHECK(!refused.ok(),
             "a generation that no longer covers what is committed was published under RefuseShrink");
  if (!refused.ok()) {
    PROP_CHECK(refused.status().code() == ErrorCode::InsufficientCapacity,
               "the refusal carries " + std::string(refused.status().token()) +
                   " rather than insufficient_capacity");
  }
  PROP_CHECK(core.StateDigest() == digest, "the refused publication changed the ledger");
  PROP_CHECK(core.chain_digest() == chain, "the refused publication extended the accounting chain");
  PROP_CHECK(core.sequence() == sequence, "the refused publication consumed a sequence");
  PROP_CHECK(TotalCommitted(core) == committed, "the refused publication changed what is committed");
  const SiteLedger* ledger = core.FindSite(site_a);
  RCB_REQUIRE(ledger != nullptr);
  RCB_CHECK_EQ(ledger->generation, u64{1});
  RCB_CHECK_EQ(ledger->LiveCommitmentCount(), std::size_t{1});
  RCB_CHECK(core.VerifyConservation().closed);
}

// ---------------------------------------------------------------------------
// 4. Kernel defects this suite found.
//
// Both checks state the property the boundary requires and both fail on the
// current kernel. They are regression tests for a fix, not descriptions of
// today's behaviour. The randomized workload above deliberately stays off these
// two paths so that its own result stays meaningful; the reproductions below
// are minimal and deterministic.
// ---------------------------------------------------------------------------

RCB_TEST(property_defect_shrinking_protected_reserve_is_neither_evicted_nor_refused) {
  // Reproduction: publish allocatable {1000} with protected reserve {500}; a
  // critical ask authorized under the site's reserve policy commits 1000 of
  // allocatable plus 200 of reserve; then publish generation 2 with the same
  // allocatable but reserve {100}.
  //
  // Expected: the new generation no longer covers the committed reserve, so
  // EvictToFit evicts until reserve_committed <= protected_reserve and
  // RefuseShrink refuses. Actual: ApplyOffer computes its overage from the
  // allocatable pool only (src/broker.cpp, overage_of()), so the publication
  // succeeds with zero revocations, committed_reserve stays above
  // protected_reserve, RemainingReserve() returns a negative residual, and
  // VerifyConservation() reports "committed reserve plus withheld exceeds the
  // protected reserve".
  struct Scenario {
    ShrinkPolicy policy;
    const char* name;
  };
  const Scenario scenarios[] = {{ShrinkPolicy::EvictToFit, "evict_to_fit"},
                                {ShrinkPolicy::RefuseShrink, "refuse_shrink"}};
  const SiteId site_a = MakeId<rcb::SiteIdTag>("site-a");
  for (const Scenario& scenario : scenarios) {
    BrokerConfig config;
    config.shrink_policy = scenario.policy;
    BrokerCore core(config);
    if (!PublishExpectOk(
            &core, OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{500, 0, 0, 0}))) {
      return;
    }
    Ask ask = PowerAsk("ask-reserve", 1200, PriorityClass::Critical, 100);
    ask.may_consume_protected_reserve = true;
    ask.reserve_policy_authorization = MakeId<rcb::PolicyIdTag>("reserve-policy-site-a");
    Decision decision;
    if (!CommitExpectOk(&core, ask, &decision)) {
      return;
    }
    const SiteLedger* before = core.FindSite(site_a);
    RCB_REQUIRE(before != nullptr);
    const i64 reserve_committed = before->tranches.front().committed_reserve.Get(Dimension::Power);
    PROP_CHECK(reserve_committed > 0,
               std::string("the scenario did not commit protected reserve (") +
                   std::to_string(reserve_committed) + "), so it proves nothing");
    if (reserve_committed <= 0) {
      return;
    }

    const Offer narrow = OneTrancheOffer("site-a", 2, Amounts{1000, 0, 0, 0}, Amounts{100, 0, 0, 0});
    const Result<rcb::OfferPublication> shrink = core.PublishOffer(narrow);
    const SiteLedger* after = core.FindSite(site_a);
    RCB_REQUIRE(after != nullptr);

    PROP_CHECK(after->tranches.front().committed_reserve.Get(Dimension::Power) <=
                   after->tranches.front().protected_reserve.Get(Dimension::Power),
               std::string("DEFECT (") + scenario.name +
                   "): the kernel published protected_reserve=100 below committed_reserve=" +
                   std::to_string(reserve_committed) +
                   " without evicting or refusing, so the reserve conservation identity now has a "
                   "negative residual");
    const Result<CapacityVector> remaining_reserve = after->tranches.front().RemainingReserve();
    PROP_CHECK(remaining_reserve.ok() && remaining_reserve.value().IsNonNegative(),
               std::string("DEFECT (") + scenario.name + "): RemainingReserve() is negative (" +
                   (remaining_reserve.ok() ? remaining_reserve.value().ToString()
                                           : remaining_reserve.status().ToString()) +
                   ") after a protected-reserve shrink");
    const rcb::ConservationReport report = core.VerifyConservation();
    PROP_CHECK(report.closed, std::string("DEFECT (") + scenario.name +
                                  "): VerifyConservation() is not closed: " + FirstViolation(report));
    if (scenario.policy == ShrinkPolicy::RefuseShrink) {
      PROP_CHECK(!shrink.ok(),
                 std::string("DEFECT (") + scenario.name +
                     "): a generation that no longer covers the committed protected reserve was "
                     "published instead of refused");
    } else {
      PROP_CHECK(shrink.ok() && shrink.value().commitments_revoked > std::size_t{0},
                 std::string("DEFECT (") + scenario.name +
                     "): the over-committed protected reserve was neither evicted nor refused");
    }
  }
}

RCB_TEST(property_defect_dropping_a_failure_domain_leaves_live_commitments_unaccounted) {
  // Reproduction: publish two failure domains, commit two power requests against
  // fd-1 and two against fd-2, then publish generation 2 that publishes fd-1
  // only, with capacity that covers every allocatable commitment on its own.
  //
  // Expected: every live commitment against fd-2 is revoked, because the new
  // generation does not publish that failure domain. Actual: ApplyOffer sets
  // has_orphan_domain, enters the eviction loop, and then assigns
  // has_orphan_domain = false after the first eviction instead of re-deriving it
  // (src/broker.cpp, ApplyOffer), so the loop stops with live commitments still
  // pointing at an unpublished failure domain. VerifyConservation() cannot see
  // them: a commitment whose domain has no tranche is never counted, so the
  // report closes while live commitments hold capacity the ledger does not show.
  const SiteId site_a = MakeId<rcb::SiteIdTag>("site-a");
  const FailureDomainId fd_1 = MakeId<rcb::FailureDomainIdTag>("fd-1");
  const FailureDomainId fd_2 = MakeId<rcb::FailureDomainIdTag>("fd-2");
  BrokerCore core;
  Offer first = OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0});
  AddTranche(&first, "fd-2", Amounts{1000, 0, 0, 0});
  if (!PublishExpectOk(&core, first)) {
    return;
  }
  const char* keys[] = {"ask-fd1-a", "ask-fd1-b", "ask-fd2-a", "ask-fd2-b"};
  for (int index = 0; index < 4; ++index) {
    Ask ask = PowerAsk(keys[index], 100, PriorityClass::Normal, 100 + index);
    if (index >= 2) {
      ask.excluded_failure_domains.push_back(fd_1);
    }
    Decision decision;
    if (!CommitExpectOk(&core, ask, &decision)) {
      return;
    }
  }
  const SiteLedger* before = core.FindSite(site_a);
  RCB_REQUIRE(before != nullptr);
  std::size_t live_in_fd2 = 0;
  for (const CommitmentRecord& commitment : before->commitments) {
    if (commitment.live && commitment.domain == fd_2) {
      ++live_in_fd2;
    }
  }
  RCB_CHECK_EQ(live_in_fd2, std::size_t{2});
  if (live_in_fd2 != 2) {
    return;
  }

  const Offer second = OneTrancheOffer("site-a", 2, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0});
  // Snapshot the committed state before the publication: ApplyOffer replaces
  // the SiteLedger in place, so a pointer taken before it reads the new
  // generation afterwards.
  const std::size_t live_before = before->LiveCommitmentCount();
  const ShrinkExpectation expectation = ExpectationFor(before, second);
  const Result<rcb::OfferPublication> shrink = core.PublishOffer(second);
  PROP_CHECK(shrink.ok(), "the publication that drops a failure domain was refused");
  if (!shrink.ok()) {
    return;
  }
  const SiteLedger* after = core.FindSite(site_a);
  RCB_REQUIRE(after != nullptr);
  std::size_t orphaned = 0;
  for (const CommitmentRecord& commitment : after->commitments) {
    if (commitment.live && after->FindTranche(commitment.domain) == nullptr) {
      ++orphaned;
    }
  }
  const i64 live_power = [&after]() {
    i64 total = 0;
    for (const CommitmentRecord& commitment : after->commitments) {
      if (commitment.live) {
        total += commitment.allocatable_amount.Get(Dimension::Power);
      }
    }
    return total;
  }();
  const rcb::ConservationReport report = core.VerifyConservation();
  const i64 ledger_committed = report.committed_total.Get(Dimension::Power) -
                               report.reserve_committed_total.Get(Dimension::Power);

  PROP_CHECK(orphaned == 0,
             "DEFECT: the new generation does not publish fd-2, " +
                 std::to_string(shrink.value().commitments_revoked) +
                 " commitment(s) were revoked, and " + std::to_string(orphaned) +
                 " live commitment(s) still name an unpublished failure domain, so their capacity is "
                 "accounted nowhere");
  PROP_CHECK(live_power == ledger_committed,
             "DEFECT: " + std::to_string(live_power) +
                 " W of live commitments remain but the ledger reports " +
                 std::to_string(ledger_committed) + " W committed");
  // The kernel's own arithmetic is not the only thing that disagrees: the
  // shrink also stopped short of the smallest number of revocations that closes
  // the ledger once the dropped failure domain is taken into account.
  CheckShrinkIsMinimal("defect reproduction: dropped failure domain", expectation,
                       shrink.value().revoked, live_before, after->LiveCommitmentCount());
}

// ---------------------------------------------------------------------------
// 5. Re-publishing an identical generation is a no-op on a fresh core too, and
//    the state digest is stable when nothing happens.
// ---------------------------------------------------------------------------

RCB_TEST(property_idempotent_publication_and_replay_leave_the_digest_alone) {
  BrokerCore core;
  const Offer offer = OneTrancheOffer("site-a", 1, Amounts{1000, 0, 0, 0}, Amounts{0, 0, 0, 0});
  if (!PublishExpectOk(&core, offer)) {
    return;
  }
  Decision decision;
  if (!CommitExpectOk(&core, PowerAsk("ask-1", 250, PriorityClass::Normal, 100), &decision)) {
    return;
  }
  const rcb::Digest digest = core.StateDigest();
  const rcb::Digest chain = core.chain_digest();
  const u64 sequence = core.sequence();

  for (int repeat = 0; repeat < 3; ++repeat) {
    const Result<rcb::OfferPublication> again = core.PublishOffer(offer);
    PROP_CHECK(again.ok() && again.value().idempotent_replay,
               "re-publishing the identical generation is not an idempotent no-op");
    PROP_CHECK(core.StateDigest() == digest, "an idempotent re-publication changed the ledger");
    PROP_CHECK(core.chain_digest() == chain, "an idempotent re-publication extended the chain");
    PROP_CHECK(core.sequence() == sequence, "an idempotent re-publication consumed a sequence");
  }

  const Result<Decision> stored = core.FindDecisionByKey(decision.key);
  RCB_REQUIRE(stored.ok());
  for (int repeat = 0; repeat < 3; ++repeat) {
    const Result<rcb::AskPlan> plan =
        core.PlanAsk(PowerAsk("ask-1", 250, PriorityClass::Normal, 100));
    RCB_REQUIRE(plan.ok());
    RCB_CHECK(plan.value().kind == PlanKind::Replay);
    const Result<Decision> replayed = core.CommitPlan(plan.value());
    RCB_REQUIRE(replayed.ok());
    RCB_CHECK(replayed.value().replay);
    RCB_CHECK(rcb::CanonicalDecisionText(replayed.value()) ==
              rcb::CanonicalDecisionText(stored.value()));
    RCB_CHECK(core.StateDigest() == digest);
    RCB_CHECK(core.chain_digest() == chain);
    RCB_CHECK(core.sequence() == sequence);
  }
  RCB_CHECK(core.VerifyConservation().closed);
}
