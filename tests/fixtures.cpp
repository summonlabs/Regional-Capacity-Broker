// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "fixtures.hpp"

#include <algorithm>
#include <utility>

#include "test_framework.hpp"

namespace fixture {
namespace {

template <class Tag>
rcb::TaggedId<Tag> Id(const std::string& text) {
  rcb::Result<rcb::TaggedId<Tag>> made = rcb::TaggedId<Tag>::Make(text);
  RCB_CHECK(made.ok());
  return made.ValueOr(rcb::TaggedId<Tag>());
}

rcb::CapacityVector Vector(const Dimensions& values) {
  rcb::Result<rcb::CapacityVector> made = rcb::CapacityVector::Make(values);
  RCB_CHECK(made.ok());
  return made.ValueOr(rcb::CapacityVector());
}

}  // namespace

rcb::Result<rcb::Offer> OfferSpec::Build() const {
  rcb::Offer offer;
  offer.site = Id<rcb::SiteIdTag>(site);
  offer.generation = generation;
  offer.source_snapshot = Id<rcb::SnapshotIdTag>(source_snapshot);
  offer.service_class = Id<rcb::ServiceClassIdTag>(service_class);
  offer.region = Id<rcb::RegionIdTag>(region);
  offer.jurisdiction = Id<rcb::JurisdictionIdTag>(jurisdiction);
  offer.risk = risk;
  offer.valid_from = valid_from;
  offer.valid_until = valid_until;
  offer.reserve_policy = Id<rcb::PolicyIdTag>(reserve_policy);
  offer.policy_generation = policy_generation;
  offer.reserve_minimum_priority = reserve_minimum_priority;
  offer.cost.reference = Id<rcb::EvidenceIdTag>(evidence);
  rcb::Result<rcb::ScaledAmount> decoded_price = rcb::ScaledAmount::Parse(price);
  if (!decoded_price.ok()) {
    return rcb::Result<rcb::Offer>(decoded_price.status());
  }
  offer.cost.price_per_milli_unit = decoded_price.value();
  offer.cost.energy_millijoules_per_milli_unit = energy_per_milli_unit;
  offer.cost.carbon_milligrams_per_milli_unit = carbon_per_milli_unit;
  for (const TrancheSpec& tranche : tranches) {
    rcb::CapacityTranche entry;
    entry.domain = Id<rcb::FailureDomainIdTag>(tranche.domain);
    entry.allocatable = Vector(tranche.allocatable);
    entry.protected_reserve = Vector(tranche.reserve);
    offer.tranches.push_back(entry);
  }
  return rcb::NormalizeOffer(std::move(offer));
}

rcb::Result<rcb::Ask> AskSpec::Build() const {
  rcb::Ask ask;
  ask.key = Id<rcb::AskKeyTag>(key);
  ask.requester = Id<rcb::RequesterIdTag>(requester);
  ask.service_class = Id<rcb::ServiceClassIdTag>(service_class);
  ask.requested = Vector(requested);
  ask.priority = priority;
  ask.fairness = fairness;
  ask.all_or_nothing = all_or_nothing;
  ask.require_single_source = require_single_source;
  ask.as_of = as_of;
  ask.min_distinct_failure_domains = min_distinct_failure_domains;
  ask.max_sites = max_sites;
  ask.max_risk = max_risk;
  ask.has_cost_ceiling = has_cost_ceiling;
  rcb::Result<rcb::ScaledAmount> decoded_cost = rcb::ScaledAmount::Parse(max_total_cost);
  if (!decoded_cost.ok()) {
    return rcb::Result<rcb::Ask>(decoded_cost.status());
  }
  ask.max_total_cost = decoded_cost.value();
  ask.has_energy_ceiling = has_energy_ceiling;
  ask.max_total_energy_millijoules = max_total_energy_millijoules;
  ask.may_consume_protected_reserve = may_consume_protected_reserve;
  if (!reserve_policy_authorization.empty()) {
    ask.reserve_policy_authorization = Id<rcb::PolicyIdTag>(reserve_policy_authorization);
  }
  ask.has_policy_generation_pin = has_policy_generation_pin;
  ask.policy_generation_pin = policy_generation_pin;
  for (const auto& pin : generation_pins) {
    rcb::GenerationPin entry;
    entry.site = Id<rcb::SiteIdTag>(pin.first);
    entry.generation = pin.second;
    ask.generation_pins.push_back(entry);
  }
  std::sort(ask.generation_pins.begin(), ask.generation_pins.end(),
            [](const rcb::GenerationPin& a, const rcb::GenerationPin& b) { return a.site < b.site; });
  for (const std::string& region : allowed_regions) {
    ask.allowed_regions.push_back(Id<rcb::RegionIdTag>(region));
  }
  for (const std::string& region : excluded_regions) {
    ask.excluded_regions.push_back(Id<rcb::RegionIdTag>(region));
  }
  for (const std::string& value : allowed_jurisdictions) {
    ask.allowed_jurisdictions.push_back(Id<rcb::JurisdictionIdTag>(value));
  }
  for (const std::string& value : excluded_jurisdictions) {
    ask.excluded_jurisdictions.push_back(Id<rcb::JurisdictionIdTag>(value));
  }
  for (const std::string& value : allowed_sites) {
    ask.allowed_sites.push_back(Id<rcb::SiteIdTag>(value));
  }
  for (const std::string& value : excluded_sites) {
    ask.excluded_sites.push_back(Id<rcb::SiteIdTag>(value));
  }
  for (const std::string& value : excluded_failure_domains) {
    ask.excluded_failure_domains.push_back(Id<rcb::FailureDomainIdTag>(value));
  }
  std::sort(ask.allowed_regions.begin(), ask.allowed_regions.end());
  std::sort(ask.excluded_regions.begin(), ask.excluded_regions.end());
  std::sort(ask.allowed_jurisdictions.begin(), ask.allowed_jurisdictions.end());
  std::sort(ask.excluded_jurisdictions.begin(), ask.excluded_jurisdictions.end());
  std::sort(ask.allowed_sites.begin(), ask.allowed_sites.end());
  std::sort(ask.excluded_sites.begin(), ask.excluded_sites.end());
  std::sort(ask.excluded_failure_domains.begin(), ask.excluded_failure_domains.end());
  ask.require_current_generation = require_current_generation;
  return ask;
}

OfferSpec SimpleOffer(const std::string& site, const rcb::u64 generation,
                      const Dimensions& allocatable, const std::string& domain) {
  OfferSpec spec;
  spec.site = site;
  spec.generation = generation;
  TrancheSpec tranche;
  tranche.domain = domain;
  tranche.allocatable = allocatable;
  spec.tranches.push_back(tranche);
  return spec;
}

AskSpec SimpleAsk(const std::string& key, const rcb::i64 service_capacity) {
  AskSpec spec;
  spec.key = key;
  spec.requested = Dimensions{0, 0, 0, service_capacity};
  return spec;
}

bool Publish(rcb::BrokerCore& core, const OfferSpec& spec) {
  rcb::Result<rcb::Offer> offer = spec.Build();
  RCB_CHECK(offer.ok());
  if (!offer.ok()) {
    return false;
  }
  rcb::Result<rcb::OfferPublication> published = core.PublishOffer(offer.value());
  if (!published.ok()) {
    RCB_CHECK_EQ(std::string("publishing ") + spec.site + " generation " +
                     std::to_string(spec.generation) + ": " + published.status().ToString(),
                 std::string("ok"));
  }
  return published.ok();
}

rcb::Decision Decide(rcb::BrokerCore& core, const AskSpec& spec) {
  rcb::Result<rcb::Ask> ask = spec.Build();
  RCB_CHECK(ask.ok());
  rcb::Result<rcb::AskPlan> plan = core.PlanAsk(ask.value());
  RCB_CHECK(plan.ok());
  rcb::Result<rcb::Decision> decision = core.CommitPlan(plan.ValueOr(rcb::AskPlan()));
  RCB_CHECK(decision.ok());
  return decision.ValueOr(rcb::Decision());
}

rcb::i64 CommittedOf(const rcb::SiteLedger& ledger, const rcb::Dimension dimension) {
  rcb::i64 total = 0;
  for (const rcb::TrancheLedger& tranche : ledger.tranches) {
    total += tranche.committed_allocatable.Get(dimension) + tranche.committed_reserve.Get(dimension);
  }
  return total;
}

}  // namespace fixture
