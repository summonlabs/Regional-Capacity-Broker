// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Builders for offers and asks, so that a test reads as the scenario it sets up
// rather than as a pile of field assignments. Every builder returns a Result and
// every identifier is built through the validated constructor, so a fixture can
// never smuggle in an identity the runtime would refuse.

#ifndef RCB_TEST_FIXTURES_HPP
#define RCB_TEST_FIXTURES_HPP

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "rcb/rcb.hpp"

namespace fixture {

using Dimensions = std::array<rcb::i64, rcb::kDimensionCount>;

struct TrancheSpec {
  std::string domain = "fd-1";
  Dimensions allocatable{0, 0, 0, 0};
  Dimensions reserve{0, 0, 0, 0};
};

struct OfferSpec {
  std::string site = "s1";
  rcb::u64 generation = 1;
  std::string source_snapshot = "snap-1";
  std::string service_class = "gpu";
  std::string region = "r1";
  std::string jurisdiction = "j1";
  rcb::RiskTier risk = rcb::RiskTier::Nominal;
  rcb::i64 valid_from = 0;
  rcb::i64 valid_until = 1000;
  std::string reserve_policy = "rp-1";
  rcb::u64 policy_generation = 1;
  rcb::PriorityClass reserve_minimum_priority = rcb::PriorityClass::Critical;
  std::string price = "0.0004";
  std::string evidence = "ev-1";
  rcb::i64 energy_per_milli_unit = 1000;
  rcb::i64 carbon_per_milli_unit = 5;
  std::vector<TrancheSpec> tranches;

  rcb::Result<rcb::Offer> Build() const;
};

struct AskSpec {
  std::string key = "ask-1";
  std::string requester = "req-1";
  std::string service_class = "gpu";
  Dimensions requested{0, 0, 0, 0};
  rcb::PriorityClass priority = rcb::PriorityClass::Normal;
  rcb::FairnessPolicy fairness = rcb::FairnessPolicy::StableSiteOrder;
  bool all_or_nothing = false;
  bool require_single_source = false;
  rcb::i64 as_of = 10;
  rcb::u32 min_distinct_failure_domains = 0;
  rcb::u32 max_sites = 0;
  rcb::RiskTier max_risk = rcb::RiskTier::Critical;
  bool has_cost_ceiling = false;
  std::string max_total_cost = "0";
  bool has_energy_ceiling = false;
  rcb::i64 max_total_energy_millijoules = 0;
  bool may_consume_protected_reserve = false;
  std::string reserve_policy_authorization;
  bool has_policy_generation_pin = false;
  rcb::u64 policy_generation_pin = 0;
  std::vector<std::pair<std::string, rcb::u64>> generation_pins;
  std::vector<std::string> allowed_regions;
  std::vector<std::string> excluded_regions;
  std::vector<std::string> allowed_jurisdictions;
  std::vector<std::string> excluded_jurisdictions;
  std::vector<std::string> allowed_sites;
  std::vector<std::string> excluded_sites;
  std::vector<std::string> excluded_failure_domains;
  bool require_current_generation = true;

  rcb::Result<rcb::Ask> Build() const;
};

/// A single-tranche offer with the given allocatable capacity and no reserve.
OfferSpec SimpleOffer(const std::string& site, rcb::u64 generation, const Dimensions& allocatable,
                      const std::string& domain = "fd-1");

/// A one-dimensional ask for service capacity.
AskSpec SimpleAsk(const std::string& key, rcb::i64 service_capacity);

/// Publishes an offer and fails the test if it is refused.
bool Publish(rcb::BrokerCore& core, const OfferSpec& spec);

/// Plans and commits an ask, returning the decision; fails the test on refusal
/// of the plumbing (not on a refused brokerage outcome).
rcb::Decision Decide(rcb::BrokerCore& core, const AskSpec& spec);

/// Sums the live commitments of a ledger for one dimension.
rcb::i64 CommittedOf(const rcb::SiteLedger& ledger, rcb::Dimension dimension);

}  // namespace fixture

#endif  // RCB_TEST_FIXTURES_HPP
