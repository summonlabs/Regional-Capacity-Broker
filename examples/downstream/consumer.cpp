// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// An independent downstream consumer of the Regional Capacity Broker.
//
// This translation unit is deliberately outside the runtime's build tree: it
// includes only the installed public headers (rcb/*.hpp), links only the
// installed library, and never reaches into src/. It exercises the real
// brokerage path end to end:
//
//   * publish an offer for one site with one failure domain;
//   * submit an ask against it and commit the resulting plan;
//   * print the decision outcome, the committed quantities and the per-site
//     ledger residual;
//   * ask the kernel to re-derive the conservation identity and print whether
//     the ledger is closed;
//   * print the documented marker line "RCB-DOWNSTREAM-OK" as the last line.
//
// cmake/PackageCheck.cmake drives this program and requires that marker, so a
// change to the public API, the installed headers or the exported target that
// breaks a downstream consumer is caught by the packaging proof.
//
// It is dependency-free: the C++20 standard library and the public headers.

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "rcb/broker.hpp"
#include "rcb/identifier.hpp"
#include "rcb/ledger.hpp"
#include "rcb/model.hpp"
#include "rcb/scaled.hpp"
#include "rcb/status.hpp"
#include "rcb/units.hpp"

namespace {

/// Printed, alone on its last line, when the whole consumer ran to completion.
/// cmake/PackageCheck.cmake matches this exact line, so never weaken it and
/// never print it from a partial run.
constexpr std::string_view kSuccessMarker = "RCB-DOWNSTREAM-OK";

constexpr int kExitFailure = 1;

/// Unwraps a Result or ends the program with the error's stable token. The
/// consumer has nothing useful to do with a failure, and the packaging proof
/// wants the exact status text in the log.
template <class T>
T Require(rcb::Result<T> result, std::string_view what) {
  if (!result.ok()) {
    std::cerr << "RCB-DOWNSTREAM-FAILED: " << what << ": " << result.status().ToString() << '\n';
    std::exit(kExitFailure);
  }
  return result.value();
}

void Complain(std::string_view what) {
  std::cerr << "RCB-DOWNSTREAM-FAILED: " << what << '\n';
}

}  // namespace

int main() {
  // ---- the identities a site publishes under ----
  const rcb::SiteId site = Require(rcb::SiteId::Make("site-hel1"), "SiteId::Make");
  const rcb::ServiceClassId service_class =
      Require(rcb::ServiceClassId::Make("accelerator.class-a"), "ServiceClassId::Make");
  const rcb::RegionId region = Require(rcb::RegionId::Make("eu-north"), "RegionId::Make");
  const rcb::JurisdictionId jurisdiction =
      Require(rcb::JurisdictionId::Make("fi"), "JurisdictionId::Make");
  const rcb::FailureDomainId failure_domain =
      Require(rcb::FailureDomainId::Make("fd-a"), "FailureDomainId::Make");

  // ---- the offer: one site, one failure domain ----
  rcb::Offer offer;
  offer.site = site;
  offer.generation = 1;
  offer.source_snapshot = Require(rcb::SnapshotId::Make("snapshot-0001"), "SnapshotId::Make");
  offer.service_class = service_class;
  offer.region = region;
  offer.jurisdiction = jurisdiction;
  offer.risk = rcb::RiskTier::Nominal;
  offer.valid_from = 0;
  offer.valid_until = 3600;
  offer.reserve_policy = Require(rcb::PolicyId::Make("reserve.default"), "PolicyId::Make");
  offer.policy_generation = 1;
  offer.reserve_minimum_priority = rcb::PriorityClass::Critical;
  offer.cost.reference = Require(rcb::EvidenceId::Make("evidence-0001"), "EvidenceId::Make");
  // 250 micro-units (0.000250) per milli-unit of ServiceCapacity.
  offer.cost.price_per_milli_unit =
      Require(rcb::ScaledAmount::FromMicros(250), "ScaledAmount::FromMicros");
  offer.cost.energy_millijoules_per_milli_unit = 12;
  offer.cost.carbon_milligrams_per_milli_unit = 4;

  rcb::CapacityTranche tranche;
  tranche.domain = failure_domain;
  // power(W) / cooling(W) / rack slots / service capacity (milli-units)
  tranche.allocatable =
      Require(rcb::CapacityVector::Make({4800, 3600, 8, 4000}), "tranche allocatable capacity");
  tranche.protected_reserve =
      Require(rcb::CapacityVector::Make({1200, 900, 2, 1000}), "tranche protected reserve");
  offer.tranches.push_back(tranche);

  // ---- the kernel ----
  rcb::BrokerCore broker;

  const rcb::OfferPublication publication =
      Require(broker.PublishOffer(offer), "BrokerCore::PublishOffer");
  std::cout << "published offer: site=" << publication.site.value()
            << " generation=" << publication.generation << " sequence=" << publication.sequence
            << " allocatable_total=" << publication.allocatable_total.ToString()
            << " reserve_total=" << publication.reserve_total.ToString() << '\n';

  // ---- the ask: a caller wants part of that tranche ----
  rcb::Ask ask;
  ask.key = Require(rcb::AskKey::Make("ask-0001"), "AskKey::Make");
  ask.requester = Require(rcb::RequesterId::Make("requester-1"), "RequesterId::Make");
  ask.service_class = service_class;
  ask.requested =
      Require(rcb::CapacityVector::Make({1000, 500, 2, 1000}), "ask requested capacity");
  ask.priority = rcb::PriorityClass::High;
  ask.fairness = rcb::FairnessPolicy::StableSiteOrder;
  ask.all_or_nothing = true;
  ask.as_of = 60;
  ask.require_current_generation = true;

  const rcb::AskPlan plan = Require(broker.PlanAsk(ask), "BrokerCore::PlanAsk");
  const rcb::Decision decision = Require(broker.CommitPlan(plan), "BrokerCore::CommitPlan");

  std::cout << "decision: id=" << decision.id.value()
            << " outcome=" << rcb::DecisionOutcomeToken(decision.outcome)
            << " allocations=" << decision.allocations.size()
            << " committed=" << decision.committed.ToString()
            << " unmet=" << decision.unmet.ToString()
            << " total_cost=" << decision.total_cost.ToString()
            << " broker_sequence=" << decision.broker_sequence
            << " replay=" << (decision.replay ? "yes" : "no") << '\n';
  for (const rcb::BlockingConstraint& blocking : decision.blocking) {
    std::cout << "blocking constraint: " << blocking.ToString() << '\n';
  }

  // ---- the ledger the decision left behind ----
  const rcb::SiteLedger* ledger = broker.FindSite(site);
  if (ledger == nullptr) {
    Complain("the site ledger disappeared after a decision was committed");
    return kExitFailure;
  }
  const rcb::CapacityVector remaining =
      Require(ledger->RemainingAllocatableTotal(), "SiteLedger::RemainingAllocatableTotal");
  std::cout << "site ledger: site=" << ledger->site.value() << " generation=" << ledger->generation
            << " live_commitments=" << ledger->LiveCommitmentCount()
            << " remaining_allocatable=" << remaining.ToString() << '\n';

  // ---- the primary invariant ----
  const rcb::ConservationReport conservation = broker.VerifyConservation();
  std::cout << "conservation: closed=" << (conservation.closed ? "yes" : "no")
            << " sites=" << conservation.sites_checked
            << " tranches=" << conservation.tranches_checked
            << " live_commitments=" << conservation.live_commitments
            << " committed_total=" << conservation.committed_total.ToString() << '\n';
  for (const rcb::ConservationViolation& violation : conservation.violations) {
    std::cout << "conservation violation: " << violation.scope << ": " << violation.detail << '\n';
  }

  const rcb::AccountingSummary summary = broker.Summary();
  std::cout << "summary: decisions=" << summary.decisions
            << " live_commitments=" << summary.live_commitments
            << " committed_total=" << summary.committed_total.ToString()
            << " state_digest=" << summary.state_digest.Hex() << '\n';

  // ---- what this consumer asserts about the run ----
  if (decision.outcome != rcb::DecisionOutcome::Accepted) {
    Complain("the broker did not accept an ask the published offer fully covers");
    return kExitFailure;
  }
  if (decision.committed != ask.requested) {
    Complain("the committed quantities are not the requested quantities");
    return kExitFailure;
  }
  if (ledger->LiveCommitmentCount() != decision.allocations.size()) {
    Complain("the ledger does not hold exactly the commitments the decision produced");
    return kExitFailure;
  }
  if (!conservation.closed || !conservation.violations.empty()) {
    Complain("the conservation identity does not close");
    return kExitFailure;
  }

  std::cout << kSuccessMarker << '\n';
  return 0;
}
