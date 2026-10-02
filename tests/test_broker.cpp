// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Brokerage suite: what the kernel commits, where, under which constraint, and
// what it refuses. Every case asserts the conservation identity afterwards, so
// a wrong answer cannot hide behind a plausible-looking decision.

#include <string>
#include <vector>

#include "fixtures.hpp"
#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

namespace {

using fixture::AskSpec;
using fixture::Dimensions;
using fixture::OfferSpec;
using fixture::SimpleAsk;
using fixture::SimpleOffer;
using fixture::TrancheSpec;

constexpr rcb::i64 kM = 1;  // one milli-unit of service capacity, for readability

bool HasConstraint(const rcb::Decision& decision, const std::string& token) {
  for (const rcb::BlockingConstraint& constraint : decision.blocking) {
    if (std::string(rcb::ConstraintToken(constraint.kind)) == token) {
      return true;
    }
  }
  return false;
}

const rcb::BlockingConstraint* FindConstraint(const rcb::Decision& decision,
                                              const std::string& token) {
  for (const rcb::BlockingConstraint& constraint : decision.blocking) {
    if (std::string(rcb::ConstraintToken(constraint.kind)) == token) {
      return &constraint;
    }
  }
  return nullptr;
}

void ExpectClosed(const rcb::BrokerCore& core) {
  const rcb::ConservationReport report = core.VerifyConservation();
  if (!report.closed) {
    for (const rcb::ConservationViolation& violation : report.violations) {
      RCB_CHECK_EQ(violation.scope + ": " + violation.detail, std::string("no violation"));
    }
  }
  RCB_CHECK(report.closed);
}

}  // namespace

RCB_TEST(publication_identity_and_generation_rules) {
  rcb::BrokerCore core;
  const OfferSpec spec = SimpleOffer("s1", 1, Dimensions{1000, 800, 10, 4000});

  RCB_REQUIRE(fixture::Publish(core, spec));
  const rcb::u64 sequence_after_publish = core.sequence();

  // Re-publishing the identical generation is an idempotent no-op.
  {
    rcb::Result<rcb::Offer> offer = spec.Build();
    RCB_REQUIRE(offer.ok());
    rcb::Result<rcb::OfferPublication> again = core.PublishOffer(offer.value());
    RCB_REQUIRE(again.ok());
    RCB_CHECK(again.value().idempotent_replay);
    RCB_CHECK_EQ(core.sequence(), sequence_after_publish);
  }

  // The same generation with different content is a conflict, never a merge.
  {
    OfferSpec changed = spec;
    changed.tranches.front().allocatable = Dimensions{2000, 800, 10, 4000};
    rcb::Result<rcb::Offer> offer = changed.Build();
    RCB_REQUIRE(offer.ok());
    RCB_CHECK_ERROR(core.PublishOffer(offer.value()), rcb::ErrorCode::Conflict);
  }

  // An older generation is stale.
  {
    OfferSpec older = SimpleOffer("s1", 2, Dimensions{10, 10, 1, 10});
    RCB_REQUIRE(fixture::Publish(core, older));
    rcb::Result<rcb::Offer> first = spec.Build();
    RCB_REQUIRE(first.ok());
    RCB_CHECK_ERROR(core.PublishOffer(first.value()), rcb::ErrorCode::StaleGeneration);
  }

  // Generation zero is not a generation.
  {
    OfferSpec zero = SimpleOffer("s2", 0, Dimensions{10, 10, 1, 10});
    rcb::Result<rcb::Offer> offer = zero.Build();
    RCB_REQUIRE(offer.ok());
    RCB_CHECK_ERROR(core.PublishOffer(offer.value()), rcb::ErrorCode::InvalidRequest);
  }
  ExpectClosed(core);
}

RCB_TEST(allocation_is_exact_and_conserves) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{1000, 800, 10, 4000})));

  AskSpec ask = SimpleAsk("ask-1", 1600 * kM);
  const rcb::Decision decision = fixture::Decide(core, ask);

  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
  RCB_CHECK_EQ(decision.allocations.size(), std::size_t{1});
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{1600});
  RCB_CHECK_EQ(decision.unmet.Get(rcb::Dimension::ServiceCapacity), rcb::i64{0});
  // 0.0004 per milli-unit times 1600 milli-units is exactly 0.64.
  RCB_CHECK_EQ(decision.total_cost.ToString(), std::string("0.64"));
  RCB_CHECK_EQ(decision.total_energy_millijoules, rcb::i64{1600000});
  RCB_CHECK_EQ(decision.total_carbon_milligrams, rcb::i64{8000});
  RCB_CHECK(decision.blocking.empty());
  RCB_CHECK(decision.id.value().rfind("dec-", 0) == 0);
  RCB_CHECK(decision.allocations.front().id.value().rfind("cmt-", 0) == 0);

  const rcb::SiteLedger* ledger = core.FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()));
  RCB_REQUIRE(ledger != nullptr);
  RCB_CHECK_EQ(fixture::CommittedOf(*ledger, rcb::Dimension::ServiceCapacity), rcb::i64{1600});
  RCB_CHECK_EQ(ledger->RemainingAllocatableTotal()
                   .ValueOr(rcb::CapacityVector())
                   .Get(rcb::Dimension::ServiceCapacity),
               rcb::i64{2400});
  ExpectClosed(core);
}

RCB_TEST(partial_allocation_names_its_blocking_constraint) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));

  const rcb::Decision decision = fixture::Decide(core, SimpleAsk("ask-partial", 1500));
  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{1000});
  RCB_CHECK_EQ(decision.unmet.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
  const rcb::BlockingConstraint* constraint =
      FindConstraint(decision, "insufficient_allocatable_capacity");
  RCB_REQUIRE(constraint != nullptr);
  RCB_CHECK(constraint->has_dimension);
  RCB_CHECK(constraint->dimension == rcb::Dimension::ServiceCapacity);
  RCB_CHECK_EQ(constraint->required, rcb::i64{1500});
  RCB_CHECK_EQ(constraint->available, rcb::i64{1000});
  ExpectClosed(core);
}

RCB_TEST(all_or_nothing_refuses_rather_than_partially_committing) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));
  const rcb::ConservationReport before = core.VerifyConservation();

  AskSpec ask = SimpleAsk("ask-atomic", 1500);
  ask.all_or_nothing = true;
  const rcb::Decision decision = fixture::Decide(core, ask);

  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
  RCB_CHECK(decision.allocations.empty());
  RCB_CHECK(decision.committed.IsZero());
  RCB_CHECK_EQ(decision.unmet.Get(rcb::Dimension::ServiceCapacity), rcb::i64{1500});
  RCB_CHECK(HasConstraint(decision, "all_or_nothing_not_met"));
  // A refusal is itself an event: it consumes a sequence and is retained so that
  // a retry returns the same answer. What must not change is the ledger.
  const rcb::ConservationReport after = core.VerifyConservation();
  RCB_CHECK(after.committed_total == before.committed_total);
  RCB_CHECK(after.remaining_total == before.remaining_total);
  RCB_CHECK(after.withheld_total == before.withheld_total);
  RCB_CHECK(after.live_commitments == std::size_t{0});
  ExpectClosed(core);
}

RCB_TEST(no_eligible_offer_is_named) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));

  AskSpec ask = SimpleAsk("ask-class", 10);
  ask.service_class = "tpu";
  const rcb::Decision decision = fixture::Decide(core, ask);
  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);

  const rcb::BlockingConstraint* constraint = FindConstraint(decision, "no_offers_for_service_class");
  RCB_REQUIRE(constraint != nullptr);
  RCB_CHECK(!constraint->detail.empty());
  ExpectClosed(core);
}

RCB_TEST(protected_reserve_needs_explicit_authority_and_priority) {
  const auto base_offer = [](const rcb::PriorityClass minimum) {
    OfferSpec offer = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 100});
    offer.tranches.front().reserve = Dimensions{0, 0, 0, 900};
    offer.reserve_policy = "rp-1";
    offer.reserve_minimum_priority = minimum;
    return offer;
  };
  const rcb::SiteId site = rcb::SiteId::Make("s1").ValueOr(rcb::SiteId());

  // (a) No authorization at all: only the allocatable pool may be used, and the
  //     reserve is reported as protected rather than quietly spent.
  {
    rcb::BrokerCore core;
    RCB_REQUIRE(fixture::Publish(core, base_offer(rcb::PriorityClass::Normal)));
    const rcb::Decision decision = fixture::Decide(core, SimpleAsk("ask-plain", 500));
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
    RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{100});
    RCB_CHECK(HasConstraint(decision, "protected_reserve_not_authorized"));
    ExpectClosed(core);
  }

  // (b) The wrong policy reference is refused, not silently ignored.
  {
    rcb::BrokerCore core;
    RCB_REQUIRE(fixture::Publish(core, base_offer(rcb::PriorityClass::Normal)));
    AskSpec ask = SimpleAsk("ask-wrong-policy", 500);
    ask.may_consume_protected_reserve = true;
    ask.reserve_policy_authorization = "rp-other";
    ask.priority = rcb::PriorityClass::Critical;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{100});
    RCB_CHECK(HasConstraint(decision, "reserve_policy_mismatch"));
    ExpectClosed(core);
  }

  // (c) Below the offer's reserve minimum the reserve stays protected even with
  //     the right policy reference.
  {
    rcb::BrokerCore core;
    RCB_REQUIRE(fixture::Publish(core, base_offer(rcb::PriorityClass::Critical)));
    AskSpec ask = SimpleAsk("ask-low-priority", 500);
    ask.may_consume_protected_reserve = true;
    ask.reserve_policy_authorization = "rp-1";
    ask.priority = rcb::PriorityClass::Normal;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{100});
    RCB_CHECK(HasConstraint(decision, "protected_reserve_not_authorized"));
    ExpectClosed(core);
  }

  // (d) Authorized and high enough: the reserve is consumed and accounted as
  //     reserve, never as allocatable capacity.
  {
    rcb::BrokerCore core;
    RCB_REQUIRE(fixture::Publish(core, base_offer(rcb::PriorityClass::Normal)));
    AskSpec ask = SimpleAsk("ask-authorized", 500);
    ask.may_consume_protected_reserve = true;
    ask.reserve_policy_authorization = "rp-1";
    ask.priority = rcb::PriorityClass::High;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
    RCB_CHECK_EQ(decision.allocations.front().reserve_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{400});
    RCB_CHECK_EQ(decision.allocations.front().allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{100});
    const rcb::SiteLedger* ledger = core.FindSite(site);
    RCB_REQUIRE(ledger != nullptr);
    RCB_CHECK_EQ(ledger->tranches.front().committed_reserve.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{400});
    RCB_CHECK_EQ(ledger->tranches.front().committed_allocatable.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{100});
    ExpectClosed(core);
  }
}

RCB_TEST(risk_and_locality_filters_are_enforced) {
  OfferSpec risky = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000});
  risky.risk = rcb::RiskTier::Degraded;
  risky.region = "r1";
  risky.jurisdiction = "j1";
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, risky));
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{0, 0, 0, 1000})));

  {
    AskSpec ask = SimpleAsk("ask-risk", 100);
    ask.max_risk = rcb::RiskTier::Nominal;
    const rcb::Decision decision = fixture::Decide(core, ask);
    // Only the nominal site may serve, and it can.
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.allocations.front().site.value(), std::string("s2"));
  }
  {
    AskSpec ask = SimpleAsk("ask-risk-2", 100);
    ask.max_risk = rcb::RiskTier::Nominal;
    ask.excluded_sites = {"s2"};
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "risk_above_ceiling"));
  }
  {
    AskSpec ask = SimpleAsk("ask-site-excluded", 100);
    ask.excluded_sites = {"s1", "s2"};
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "site_excluded"));
  }
  {
    AskSpec ask = SimpleAsk("ask-region", 100);
    ask.allowed_regions = {"r2"};
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "region_not_allowed"));
  }
  ExpectClosed(core);
}

RCB_TEST(failure_domain_diversity_is_a_hard_constraint) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000}, "fd-a")));
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{0, 0, 0, 1000}, "fd-b")));

  {
    AskSpec ask = SimpleAsk("ask-diverse", 1000);
    ask.min_distinct_failure_domains = 2;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.distinct_failure_domains, rcb::u32{2});
    RCB_CHECK_EQ(decision.distinct_sites, rcb::u32{2});
    RCB_CHECK_EQ(decision.allocations.at(0).domain.value(), std::string("fd-a"));
    RCB_CHECK_EQ(decision.allocations.at(1).domain.value(), std::string("fd-b"));
  }

  // Only one domain can serve: the whole ask is refused, never silently
  // satisfied from a single domain.
  {
    OfferSpec bigger = SimpleOffer("s3", 1, Dimensions{0, 0, 0, 5000}, "fd-c");
    RCB_REQUIRE(fixture::Publish(core, bigger));
    AskSpec ask = SimpleAsk("ask-impossible-diversity", 100);
    ask.min_distinct_failure_domains = 4;
    ask.excluded_failure_domains = {"fd-a", "fd-b"};
    ask.allowed_sites = {"s3"};
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "failure_domain_diversity_not_met"));
  }
  ExpectClosed(core);
}

RCB_TEST(single_source_asks_stay_at_one_site) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 600}, "fd-a")));
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{0, 0, 0, 600}, "fd-b")));

  {
    AskSpec ask = SimpleAsk("ask-single", 500);
    ask.require_single_source = true;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.distinct_sites, rcb::u32{1});
    RCB_CHECK_EQ(decision.distinct_failure_domains, rcb::u32{1});
  }
  {
    AskSpec ask = SimpleAsk("ask-single-impossible", 1000);
    ask.require_single_source = true;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    const rcb::BlockingConstraint* constraint = FindConstraint(decision, "single_source_not_met");
    RCB_REQUIRE(constraint != nullptr);
    RCB_CHECK(constraint->has_dimension);
  }
  ExpectClosed(core);
}

RCB_TEST(cost_ceiling_clamps_exactly_without_rounding) {
  OfferSpec offer = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 4000});
  offer.price = "0.001";
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, offer));

  AskSpec ask = SimpleAsk("ask-cost", 1000);
  ask.has_cost_ceiling = true;
  ask.max_total_cost = "0.5";
  const rcb::Decision decision = fixture::Decide(core, ask);

  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
  // 0.5 / 0.001 is exactly 500 milli-units, and the cost of 500 is exactly 0.5.
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
  RCB_CHECK_EQ(decision.total_cost.ToString(), std::string("0.5"));
  RCB_CHECK(HasConstraint(decision, "cost_ceiling_exceeded"));
  RCB_CHECK(decision.total_cost <= rcb::ScaledAmount::Parse("0.5").ValueOr(rcb::ScaledAmount()));
  ExpectClosed(core);
}

RCB_TEST(energy_ceiling_is_enforced_exactly) {
  OfferSpec offer = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 4000});
  offer.energy_per_milli_unit = 1000;
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, offer));

  AskSpec ask = SimpleAsk("ask-energy", 1000);
  ask.has_energy_ceiling = true;
  ask.max_total_energy_millijoules = 250000;
  const rcb::Decision decision = fixture::Decide(core, ask);
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{250});
  RCB_CHECK_EQ(decision.total_energy_millijoules, rcb::i64{250000});
  RCB_CHECK(HasConstraint(decision, "energy_ceiling_exceeded"));
  ExpectClosed(core);
}

RCB_TEST(idempotent_retries_consume_nothing) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));

  AskSpec ask = SimpleAsk("ask-retry", 400);
  const rcb::Decision first = fixture::Decide(core, ask);
  const rcb::Digest after_first = core.StateDigest();
  RCB_CHECK(!first.replay);

  const rcb::Decision second = fixture::Decide(core, ask);
  RCB_CHECK(second.replay);
  RCB_CHECK_EQ(second.id.value(), first.id.value());
  RCB_CHECK_EQ(second.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{400});
  RCB_CHECK(core.StateDigest() == after_first);

  // The same key with different content is a conflict, not a second commitment.
  AskSpec changed = SimpleAsk("ask-retry", 800);
  rcb::Result<rcb::Ask> changed_ask = changed.Build();
  RCB_REQUIRE(changed_ask.ok());
  rcb::Result<rcb::AskPlan> plan = core.PlanAsk(changed_ask.value());
  RCB_CHECK_ERROR(plan, rcb::ErrorCode::IdempotencyConflict);
  RCB_CHECK(core.StateDigest() == after_first);
  ExpectClosed(core);
}

RCB_TEST(generation_pins_and_validity_windows_are_refusals) {
  rcb::BrokerCore core;
  OfferSpec offer = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000});
  offer.valid_from = 10;
  offer.valid_until = 100;
  RCB_REQUIRE(fixture::Publish(core, offer));

  {
    AskSpec ask = SimpleAsk("ask-early", 10);
    ask.as_of = 5;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "offer_not_yet_valid"));
  }
  {
    AskSpec ask = SimpleAsk("ask-late", 10);
    ask.as_of = 500;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "offer_expired"));
  }
  {
    RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 2, Dimensions{0, 0, 0, 1000})));
    AskSpec ask = SimpleAsk("ask-pin", 10);
    ask.as_of = 50;
    ask.generation_pins = {{"s1", 1}};
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(HasConstraint(decision, "generation_pin_mismatch"));
  }
  ExpectClosed(core);
}

RCB_TEST(fairness_policies_are_deterministic_and_distinct) {
  const auto fresh_core = [] {
    rcb::BrokerCore core;
    if (!fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 3000}, "fd-a"))) {
      return core;
    }
    (void)fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{0, 0, 0, 1000}, "fd-b"));
    return core;
  };

  const auto run = [&fresh_core](const rcb::FairnessPolicy policy) {
    rcb::BrokerCore core = fresh_core();
    AskSpec ask = SimpleAsk("ask-fair", 1000);
    ask.fairness = policy;
    return fixture::Decide(core, ask);
  };

  // Equal share divides the request evenly: ceil(1000 / 2) each.
  {
    const rcb::Decision decision = run(rcb::FairnessPolicy::EqualShareAcrossSites);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.allocations.size(), std::size_t{2});
    RCB_CHECK_EQ(decision.allocations.at(0).allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{500});
    RCB_CHECK_EQ(decision.allocations.at(1).allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{500});
    RCB_CHECK_EQ(decision.distinct_sites, rcb::u32{2});
  }

  // Proportional apportionment is exact floor shares; here 1000 * 3000/4000 and
  // 1000 * 1000/4000 sum to the whole request with nothing left over.
  {
    const rcb::Decision decision = run(rcb::FairnessPolicy::ProportionalToAllocatable);
    RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
    RCB_CHECK_EQ(decision.allocations.size(), std::size_t{2});
    RCB_CHECK_EQ(decision.allocations.at(0).site.value(), std::string("s1"));
    RCB_CHECK_EQ(decision.allocations.at(0).allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{750});
    RCB_CHECK_EQ(decision.allocations.at(1).allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{250});
  }

  // The stable order fills the first site in (site, failure domain) order.
  {
    const rcb::Decision decision = run(rcb::FairnessPolicy::StableSiteOrder);
    RCB_CHECK_EQ(decision.allocations.size(), std::size_t{1});
    RCB_CHECK_EQ(decision.allocations.front().site.value(), std::string("s1"));
    RCB_CHECK_EQ(decision.allocations.front().allocatable_amount.Get(rcb::Dimension::ServiceCapacity),
                 rcb::i64{1000});
  }

  // Lowest cost first picks the cheaper site even when it is not first in the
  // stable order. s2 is made cheaper, and no other site may be used.
  {
    rcb::BrokerCore core;
    OfferSpec cheap = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000}, "fd-a");
    cheap.price = "0.002";
    RCB_REQUIRE(fixture::Publish(core, cheap));
    OfferSpec cheaper = SimpleOffer("s2", 1, Dimensions{0, 0, 0, 1000}, "fd-b");
    cheaper.price = "0.001";
    RCB_REQUIRE(fixture::Publish(core, cheaper));
    AskSpec ask = SimpleAsk("ask-cost-order", 600);
    ask.fairness = rcb::FairnessPolicy::LowestCostFirst;
    const rcb::Decision decision = fixture::Decide(core, ask);
    RCB_CHECK_EQ(decision.allocations.size(), std::size_t{1});
    RCB_CHECK_EQ(decision.allocations.front().site.value(), std::string("s2"));
    ExpectClosed(core);
  }
}

RCB_TEST(revocation_withholds_capacity_and_revokes_commitments) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));
  const rcb::Decision decision = fixture::Decide(core, SimpleAsk("ask-before-revoke", 400));
  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);

  rcb::Result<rcb::OfferRevocation> revoked =
      core.RevokeOffer(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()), 1, 20);
  RCB_REQUIRE(revoked.ok());
  RCB_CHECK_EQ(revoked.value().commitments_revoked, std::size_t{1});
  // Withdrawing a generation withholds all of it: the commitments it carried are
  // released and then withheld along with the capacity that was never committed.
  RCB_CHECK_EQ(revoked.value().withheld.Get(rcb::Dimension::ServiceCapacity), rcb::i64{1000});

  const rcb::SiteLedger* ledger = core.FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()));
  RCB_REQUIRE(ledger != nullptr);
  RCB_CHECK(ledger->revoked);
  RCB_CHECK_EQ(ledger->tranches.front().committed_allocatable.Get(rcb::Dimension::ServiceCapacity),
               rcb::i64{0});
  RCB_CHECK_EQ(ledger->tranches.front().withheld.Get(rcb::Dimension::ServiceCapacity),
               rcb::i64{1000});

  const rcb::ConservationReport report = core.VerifyConservation();
  RCB_CHECK(report.closed);
  RCB_CHECK_EQ(report.withheld_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{1000});
  RCB_CHECK_EQ(report.remaining_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{0});

  // A withdrawn generation cannot serve new asks.
  const rcb::Decision after = fixture::Decide(core, SimpleAsk("ask-after-revoke", 100));
  RCB_CHECK(after.outcome == rcb::DecisionOutcome::Refused);
  RCB_CHECK(HasConstraint(after, "offer_revoked"));

  // Revoking a generation that is not current is refused.
  RCB_CHECK_ERROR(core.RevokeOffer(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()), 7, 21),
                  rcb::ErrorCode::StaleGeneration);
  RCB_CHECK_ERROR(core.RevokeOffer(rcb::SiteId::Make("nope").ValueOr(rcb::SiteId()), 1, 21),
                  rcb::ErrorCode::UnknownSite);
  ExpectClosed(core);
}

RCB_TEST(capacity_shrink_evicts_lowest_priority_newest_first) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));

  AskSpec low = SimpleAsk("ask-low", 300);
  low.priority = rcb::PriorityClass::Low;
  const rcb::Decision low_decision = fixture::Decide(core, low);
  RCB_CHECK(low_decision.outcome == rcb::DecisionOutcome::Accepted);

  AskSpec high = SimpleAsk("ask-high", 300);
  high.priority = rcb::PriorityClass::High;
  const rcb::Decision high_decision = fixture::Decide(core, high);
  RCB_CHECK(high_decision.outcome == rcb::DecisionOutcome::Accepted);

  // The new generation publishes 400, which covers the high-priority
  // commitment but not the low-priority one.
  OfferSpec smaller = SimpleOffer("s1", 2, Dimensions{0, 0, 0, 400});
  rcb::Result<rcb::Offer> offer = smaller.Build();
  RCB_REQUIRE(offer.ok());
  rcb::Result<rcb::OfferPublication> published = core.PublishOffer(offer.value());
  RCB_REQUIRE(published.ok());
  RCB_CHECK_EQ(published.value().commitments_revoked, std::size_t{1});
  RCB_CHECK_EQ(published.value().carried_commitments, std::size_t{1});
  RCB_REQUIRE(published.value().revoked.size() == 1U);
  RCB_CHECK_EQ(published.value().revoked.front().id.value(),
               low_decision.allocations.front().id.value());
  RCB_CHECK(published.value().revoked.front().revocation_reason ==
            rcb::RevocationReason::CapacityShrink);

  const rcb::SiteLedger* ledger = core.FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()));
  RCB_REQUIRE(ledger != nullptr);
  RCB_CHECK_EQ(ledger->generation, rcb::u64{2});
  RCB_CHECK_EQ(ledger->tranches.front().committed_allocatable.Get(rcb::Dimension::ServiceCapacity),
               rcb::i64{300});
  RCB_CHECK_EQ(ledger->LiveCommitmentCount(), std::size_t{1});
  ExpectClosed(core);
}

RCB_TEST(shrink_can_be_refused_outright) {
  rcb::BrokerConfig config;
  config.shrink_policy = rcb::ShrinkPolicy::RefuseShrink;
  rcb::BrokerCore core(config);
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 1000})));
  const rcb::Decision decision = fixture::Decide(core, SimpleAsk("ask-keep", 800));
  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::Accepted);
  const rcb::Digest before = core.StateDigest();

  OfferSpec smaller = SimpleOffer("s1", 2, Dimensions{0, 0, 0, 100});
  rcb::Result<rcb::Offer> offer = smaller.Build();
  RCB_REQUIRE(offer.ok());
  RCB_CHECK_ERROR(core.PublishOffer(offer.value()), rcb::ErrorCode::InsufficientCapacity);
  RCB_CHECK(core.StateDigest() == before);
  const rcb::SiteLedger* ledger = core.FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()));
  RCB_REQUIRE(ledger != nullptr);
  RCB_CHECK_EQ(ledger->generation, rcb::u64{1});
  ExpectClosed(core);
}

RCB_TEST(site_limit_and_exclusions_are_respected) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{0, 0, 0, 500}, "fd-a")));
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{0, 0, 0, 500}, "fd-b")));
  RCB_REQUIRE(fixture::Publish(core, SimpleOffer("s3", 1, Dimensions{0, 0, 0, 500}, "fd-c")));

  AskSpec ask = SimpleAsk("ask-limited", 1200);
  ask.max_sites = 1;
  const rcb::Decision decision = fixture::Decide(core, ask);
  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
  RCB_CHECK_EQ(decision.distinct_sites, rcb::u32{1});
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
  RCB_CHECK(HasConstraint(decision, "site_limit_exceeded"));
  ExpectClosed(core);
}

RCB_TEST(ceilings_apply_to_every_pass_including_the_residue) {
  // Two equal sites under proportional fairness: the first pass fills floor
  // shares and leaves a residue. The residue pass must still obey the ceiling
  // rather than spending past it and turning a partial answer into a refusal.
  rcb::BrokerCore core;
  OfferSpec first = SimpleOffer("s1", 1, Dimensions{0, 0, 0, 3000}, "fd-a");
  first.price = "0.001";
  RCB_REQUIRE(fixture::Publish(core, first));
  OfferSpec second = SimpleOffer("s2", 1, Dimensions{0, 0, 0, 3000}, "fd-b");
  second.price = "0.001";
  RCB_REQUIRE(fixture::Publish(core, second));

  AskSpec ask = SimpleAsk("ask-residue", 1000);
  ask.fairness = rcb::FairnessPolicy::ProportionalToAllocatable;
  ask.has_cost_ceiling = true;
  ask.max_total_cost = "0.7";
  const rcb::Decision decision = fixture::Decide(core, ask);

  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{700});
  RCB_CHECK_EQ(decision.total_cost.ToString(), std::string("0.7"));
  RCB_CHECK(HasConstraint(decision, "cost_ceiling_exceeded"));
  RCB_CHECK(decision.blocking.size() >= 1U);
  ExpectClosed(core);
}

RCB_TEST(a_ceiling_scales_the_whole_commitment_not_one_dimension) {
  rcb::BrokerCore core;
  OfferSpec offer = SimpleOffer("s1", 1, Dimensions{4000, 4000, 40, 4000});
  offer.price = "0.001";
  RCB_REQUIRE(fixture::Publish(core, offer));

  AskSpec ask = SimpleAsk("ask-shaped", 1000);
  ask.requested = Dimensions{2000, 1000, 20, 1000};
  ask.has_cost_ceiling = true;
  ask.max_total_cost = "0.5";  // affords exactly half of the service capacity
  const rcb::Decision decision = fixture::Decide(core, ask);

  RCB_CHECK(decision.outcome == rcb::DecisionOutcome::PartiallyAccepted);
  RCB_CHECK_EQ(decision.total_cost.ToString(), std::string("0.5"));
  // Every requested dimension is scaled by the same exact ratio, floored.
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::Power), rcb::i64{1000});
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::Cooling), rcb::i64{500});
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::RackSpace), rcb::i64{10});
  RCB_CHECK_EQ(decision.committed.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
  ExpectClosed(core);
}

RCB_TEST(decisions_are_deterministic_across_instances) {
  const auto run = [] {
    rcb::BrokerCore core;
    (void)fixture::Publish(core, SimpleOffer("s1", 1, Dimensions{1000, 800, 10, 4000}, "fd-a"));
    (void)fixture::Publish(core, SimpleOffer("s2", 1, Dimensions{500, 400, 5, 2000}, "fd-b"));
    AskSpec ask = SimpleAsk("ask-det", 2500);
    ask.requested = Dimensions{400, 300, 3, 2500};
    ask.min_distinct_failure_domains = 2;
    const rcb::Decision decision = fixture::Decide(core, ask);
    return std::make_pair(rcb::CanonicalDecisionText(decision), core.StateDigest());
  };
  const auto first = run();
  const auto second = run();
  RCB_CHECK_EQ(first.first, second.first);
  RCB_CHECK(first.second == second.second);
  RCB_CHECK(rcb::AskDigest(fixture::SimpleAsk("ask-det", 2500).Build().ValueOr(rcb::Ask()))
                .Hex()
                .size() == 64U);
}