// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A walkthrough of the public API, compiled against the build tree. It publishes
// an offer for one site with two failure domains, asks for capacity that must
// span both of them, retries the same ask to show that a retry is recognised,
// then shrinks the site's generation and prints exactly what the broker evicted
// and what the ledger holds afterwards. Every step re-derives the conservation
// identity, because that is the invariant this runtime exists to keep.

#include <cstdio>
#include <string>

#include "rcb/rcb.hpp"

namespace {

rcb::CapacityVector Vector(const rcb::i64 power, const rcb::i64 cooling, const rcb::i64 racks,
                           const rcb::i64 service) {
  return rcb::CapacityVector::Make({power, cooling, racks, service})
      .ValueOr(rcb::CapacityVector());
}

void PrintLedger(const rcb::BrokerCore& core, const rcb::SiteId& site) {
  const rcb::SiteLedger* ledger = core.FindSite(site);
  if (ledger == nullptr) {
    std::printf("ledger: site %s is not published\n", site.value().c_str());
    return;
  }
  for (const rcb::TrancheLedger& tranche : ledger->tranches) {
    const rcb::CapacityVector committed =
        tranche.CommittedTotal().ValueOr(rcb::CapacityVector());
    const rcb::CapacityVector remaining =
        tranche.RemainingAllocatable().ValueOr(rcb::CapacityVector());
    std::printf("ledger: %s/%s committed=%s remaining=%s withheld=%s\n",
                ledger->site.value().c_str(), tranche.domain.value().c_str(),
                committed.ToString().c_str(), remaining.ToString().c_str(),
                tranche.withheld.ToString().c_str());
  }
}

bool CheckConservation(const rcb::BrokerCore& core, const char* where) {
  const rcb::ConservationReport report = core.VerifyConservation();
  std::printf("conservation after %s: closed=%s sites=%zu live_commitments=%zu\n", where,
              report.closed ? "yes" : "NO", report.sites_checked, report.live_commitments);
  for (const rcb::ConservationViolation& violation : report.violations) {
    std::printf("  violation: %s: %s\n", violation.scope.c_str(), violation.detail.c_str());
  }
  return report.closed;
}

}  // namespace

int main() {
  std::printf("regional capacity broker %s\n", rcb::VersionString().c_str());

  rcb::BrokerConfig config;
  config.shrink_policy = rcb::ShrinkPolicy::EvictToFit;
  rcb::BrokerCore core(config, /*epoch=*/1);

  // ---- a site publishes what it can broker --------------------------------
  rcb::Offer offer;
  offer.site = rcb::SiteId::Make("site-hel1").ValueOr(rcb::SiteId());
  offer.generation = 1;
  offer.source_snapshot = rcb::SnapshotId::Make("snap-2026-02-11").ValueOr(rcb::SnapshotId());
  offer.service_class = rcb::ServiceClassId::Make("gpu-h100").ValueOr(rcb::ServiceClassId());
  offer.region = rcb::RegionId::Make("eu-north").ValueOr(rcb::RegionId());
  offer.jurisdiction = rcb::JurisdictionId::Make("eu").ValueOr(rcb::JurisdictionId());
  offer.risk = rcb::RiskTier::Nominal;
  offer.valid_from = 0;
  offer.valid_until = 100000;
  offer.reserve_policy = rcb::PolicyId::Make("rp-regional").ValueOr(rcb::PolicyId());
  offer.policy_generation = 1;
  offer.reserve_minimum_priority = rcb::PriorityClass::High;
  offer.cost.reference = rcb::EvidenceId::Make("ev-2026-02").ValueOr(rcb::EvidenceId());
  offer.cost.price_per_milli_unit =
      rcb::ScaledAmount::Parse("0.0004").ValueOr(rcb::ScaledAmount());
  offer.cost.energy_millijoules_per_milli_unit = 1000;
  offer.cost.carbon_milligrams_per_milli_unit = 5;

  rcb::CapacityTranche hall_a;
  hall_a.domain = rcb::FailureDomainId::Make("hall-a").ValueOr(rcb::FailureDomainId());
  hall_a.allocatable = Vector(40000, 36000, 8, 12000);
  hall_a.protected_reserve = Vector(4000, 3600, 1, 1200);
  rcb::CapacityTranche hall_b;
  hall_b.domain = rcb::FailureDomainId::Make("hall-b").ValueOr(rcb::FailureDomainId());
  hall_b.allocatable = Vector(30000, 27000, 6, 9000);
  offer.tranches.push_back(hall_a);
  offer.tranches.push_back(hall_b);

  rcb::Result<rcb::OfferPublication> published = core.PublishOffer(offer);
  if (!published.ok()) {
    std::printf("publication refused: %s\n", published.status().ToString().c_str());
    return 1;
  }
  std::printf("published: site=%s generation=%llu allocatable=%s reserve=%s\n",
              published.value().site.value().c_str(),
              static_cast<unsigned long long>(published.value().generation),
              published.value().allocatable_total.ToString().c_str(),
              published.value().reserve_total.ToString().c_str());
  if (!CheckConservation(core, "publication")) {
    return 1;
  }

  // ---- a caller asks for capacity that must span both failure domains ------
  rcb::Ask ask;
  ask.key = rcb::AskKey::Make("order-2026-0211-0007").ValueOr(rcb::AskKey());
  ask.requester = rcb::RequesterId::Make("training-platform").ValueOr(rcb::RequesterId());
  ask.service_class = offer.service_class;
  ask.requested = Vector(20000, 18000, 4, 8000);
  ask.priority = rcb::PriorityClass::High;
  ask.fairness = rcb::FairnessPolicy::ProportionalToAllocatable;
  ask.as_of = 100;
  ask.min_distinct_failure_domains = 2;
  ask.has_cost_ceiling = true;
  ask.max_total_cost = rcb::ScaledAmount::Parse("3.5").ValueOr(rcb::ScaledAmount());

  const auto decide = [&core](const rcb::Ask& request) -> rcb::Result<rcb::Decision> {
    rcb::Result<rcb::AskPlan> plan = core.PlanAsk(request);
    if (!plan.ok()) {
      return rcb::Result<rcb::Decision>(plan.status());
    }
    return core.CommitPlan(plan.value());
  };

  rcb::Result<rcb::Decision> decision = decide(ask);
  if (!decision.ok()) {
    std::printf("ask failed: %s\n", decision.status().ToString().c_str());
    return 1;
  }
  std::printf("decision: id=%s outcome=%s replay=%s committed=%s unmet=%s cost=%s\n",
              decision.value().id.value().c_str(),
              std::string(rcb::DecisionOutcomeToken(decision.value().outcome)).c_str(),
              decision.value().replay ? "yes" : "no",
              decision.value().committed.ToString().c_str(),
              decision.value().unmet.ToString().c_str(),
              decision.value().total_cost.ToString().c_str());
  for (const rcb::Allocation& allocation : decision.value().allocations) {
    std::printf("  allocated: %s/%s allocatable=%s reserve=%s\n",
                allocation.site.value().c_str(), allocation.domain.value().c_str(),
                allocation.allocatable_amount.ToString().c_str(),
                allocation.reserve_amount.ToString().c_str());
  }
  for (const rcb::BlockingConstraint& constraint : decision.value().blocking) {
    std::printf("  blocked: %s\n", constraint.ToString().c_str());
  }
  PrintLedger(core, offer.site);
  if (!CheckConservation(core, "the decision")) {
    return 1;
  }

  // ---- the same ask again is a retry, not a second commitment -------------
  rcb::Result<rcb::Decision> retry = decide(ask);
  if (!retry.ok()) {
    std::printf("retry failed: %s\n", retry.status().ToString().c_str());
    return 1;
  }
  std::printf("retry: id=%s replay=%s committed=%s\n", retry.value().id.value().c_str(),
              retry.value().replay ? "yes" : "no", retry.value().committed.ToString().c_str());
  if (!retry.value().replay || retry.value().id.value() != decision.value().id.value()) {
    std::printf("retry was not recognised as an idempotent repeat\n");
    return 1;
  }

  // ---- a ceiling is an exact cap, and the shortfall names it --------------
  rcb::Ask capped = ask;
  capped.key = rcb::AskKey::Make("order-2026-0211-0009").ValueOr(rcb::AskKey());
  capped.max_total_cost = rcb::ScaledAmount::Parse("1.0").ValueOr(rcb::ScaledAmount());
  rcb::Result<rcb::Decision> limited = decide(capped);
  if (!limited.ok()) {
    std::printf("capped ask failed: %s\n", limited.status().ToString().c_str());
    return 1;
  }
  std::printf("capped ask: outcome=%s committed=%s cost=%s\n",
              std::string(rcb::DecisionOutcomeToken(limited.value().outcome)).c_str(),
              limited.value().committed.ToString().c_str(),
              limited.value().total_cost.ToString().c_str());
  for (const rcb::BlockingConstraint& constraint : limited.value().blocking) {
    std::printf("  blocked: %s\n", constraint.ToString().c_str());
  }
  if (!CheckConservation(core, "the capped ask")) {
    return 1;
  }

  // ---- the site shrinks its next generation, and the broker evicts --------
  offer.generation = 2;
  offer.tranches.clear();
  rcb::CapacityTranche smaller;
  smaller.domain = hall_a.domain;
  smaller.allocatable = Vector(4000, 3600, 1, 1200);
  offer.tranches.push_back(smaller);

  rcb::Result<rcb::OfferPublication> shrunk = core.PublishOffer(offer);
  if (!shrunk.ok()) {
    std::printf("shrink refused: %s\n", shrunk.status().ToString().c_str());
  } else {
    std::printf("shrink: generation=%llu carried=%zu evicted=%zu\n",
                static_cast<unsigned long long>(shrunk.value().generation),
                shrunk.value().carried_commitments, shrunk.value().commitments_revoked);
    for (const rcb::CommitmentRecord& revoked : shrunk.value().revoked) {
      std::printf("  evicted: %s (%s) at %s/%s\n", revoked.id.value().c_str(),
                  std::string(rcb::RevocationReasonToken(revoked.revocation_reason)).c_str(),
                  revoked.site.value().c_str(), revoked.domain.value().c_str());
    }
  }
  PrintLedger(core, offer.site);
  if (!CheckConservation(core, "the shrink")) {
    return 1;
  }

  // ---- an ask that cannot be served still says why ------------------------
  rcb::Ask impossible = ask;
  impossible.key = rcb::AskKey::Make("order-2026-0211-0008").ValueOr(rcb::AskKey());
  impossible.min_distinct_failure_domains = 2;
  rcb::Result<rcb::Decision> refused = decide(impossible);
  if (!refused.ok()) {
    std::printf("ask failed: %s\n", refused.status().ToString().c_str());
    return 1;
  }
  std::printf("refused ask: outcome=%s\n",
              std::string(rcb::DecisionOutcomeToken(refused.value().outcome)).c_str());
  for (const rcb::BlockingConstraint& constraint : refused.value().blocking) {
    std::printf("  blocked: %s\n", constraint.ToString().c_str());
  }

  std::printf("state digest: %s\n", core.StateDigest().Hex().c_str());
  std::printf("RCB-WALKTHROUGH-OK\n");
  return 0;
}
