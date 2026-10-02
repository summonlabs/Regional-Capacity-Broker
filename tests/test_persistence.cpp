// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Persistence and recovery suite: real files in a real directory, real close and
// reopen, torn tails, interior corruption, snapshots, compaction, restart
// idempotency and epoch fencing. Nothing here is simulated except the byte-level
// damage, which is applied to the real files the runtime wrote.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "fixtures.hpp"
#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

namespace {

namespace fs = std::filesystem;

/// A unique directory under the system temporary directory, removed on scope
/// exit whether the test passed or not.
class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    ++counter;
    path_ = fs::temp_directory_path() /
            ("rcb-test-" + std::to_string(counter) + "-" + std::to_string(std::rand()));
    std::error_code error;
    fs::remove_all(path_, error);
    fs::create_directories(path_, error);
  }

  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const fs::path& path() const { return path_; }
  [[nodiscard]] std::string str() const { return path_.string(); }
  [[nodiscard]] fs::path file(const std::string& name) const { return path_ / name; }

 private:
  fs::path path_;
};

rcb::ServiceConfig Config(const TempDir& dir, const rcb::DurabilityClass durability) {
  rcb::ServiceConfig config;
  config.store_directory = dir.str();
  config.durability = durability;
  config.worker_threads = 1;
  config.compaction_enabled = false;
  return config;
}

rcb::Result<std::unique_ptr<rcb::BrokerService>> OpenStore(const TempDir& dir,
                                                           const rcb::DurabilityClass durability) {
  return rcb::BrokerService::Open(Config(dir, durability));
}

std::uintmax_t FileSize(const fs::path& path) {
  std::error_code error;
  const std::uintmax_t size = fs::file_size(path, error);
  return error ? 0 : size;
}

void Truncate(const fs::path& path, const std::uintmax_t bytes) {
  std::error_code error;
  fs::resize_file(path, bytes, error);
  RCB_CHECK(!error);
}

/// Flips one byte at a given offset, leaving the file the same length.
void FlipByte(const fs::path& path, const std::uintmax_t offset) {
  std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
  RCB_REQUIRE(stream.is_open());
  stream.seekg(static_cast<std::streamoff>(offset));
  char byte = 0;
  stream.read(&byte, 1);
  RCB_REQUIRE(stream.gcount() == 1);
  byte = static_cast<char>(byte ^ 0x5A);
  stream.seekp(static_cast<std::streamoff>(offset));
  stream.write(&byte, 1);
  stream.flush();
  RCB_CHECK(stream.good());
}

const fixture::OfferSpec kSiteA = [] {
  fixture::OfferSpec spec = fixture::SimpleOffer("s1", 1, fixture::Dimensions{1000, 800, 10, 4000},
                                                 "fd-a");
  return spec;
}();

}  // namespace

RCB_TEST(store_round_trips_through_close_and_reopen) {
  TempDir dir;
  rcb::u64 sequence_before = 0;
  std::string decision_id;
  rcb::i64 committed_before = 0;
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    rcb::Result<rcb::Decision> decision =
        service.value()->AskNow(fixture::SimpleAsk("ask-persist", 1600).Build().ValueOr(rcb::Ask()));
    RCB_REQUIRE(decision.ok());
    RCB_CHECK(decision.value().durability == rcb::DurabilityClass::Durable);
    decision_id = decision.value().id.value();
    sequence_before = service.value()->Summary().ValueOr(rcb::AccountingSummary()).sequence;
    committed_before =
        service.value()->Summary().ValueOr(rcb::AccountingSummary())
            .committed_total.Get(rcb::Dimension::ServiceCapacity);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    const rcb::AccountingSummary summary = service.value()->Summary().ValueOr(rcb::AccountingSummary());
    RCB_CHECK_EQ(summary.committed_total.Get(rcb::Dimension::ServiceCapacity), committed_before);
    // Reopening advances the epoch, not the sequence: nothing new was decided.
    RCB_CHECK_EQ(summary.sequence, sequence_before);
    RCB_CHECK(summary.epoch >= 2);
    RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
    const rcb::RecoveryReport recovery = service.value()->Recovery();
    RCB_CHECK(!recovery.torn_tail);
    RCB_CHECK(!recovery.repaired);
    RCB_CHECK(!recovery.prepared_discarded);
    RCB_CHECK(recovery.last_epoch >= 2);

    // The decision is still queryable, by key, after the restart.
    rcb::Result<rcb::Decision> found =
        service.value()->FindDecision(rcb::AskKey::Make("ask-persist").ValueOr(rcb::AskKey()));
    RCB_REQUIRE(found.ok());
    RCB_CHECK_EQ(found.value().id.value(), decision_id);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
}

RCB_TEST(retried_asks_stay_idempotent_across_restart) {
  TempDir dir;
  rcb::i64 committed_before = 0;
  std::string decision_id;
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    rcb::Result<rcb::Decision> decision =
        service.value()->AskNow(fixture::SimpleAsk("ask-retry", 400).Build().ValueOr(rcb::Ask()));
    RCB_REQUIRE(decision.ok());
    decision_id = decision.value().id.value();
    committed_before =
        service.value()->Summary().ValueOr(rcb::AccountingSummary())
            .committed_total.Get(rcb::Dimension::ServiceCapacity);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    // The retry must be recognised as a retry: same identity, nothing consumed.
    rcb::Result<rcb::Decision> replay =
        service.value()->AskNow(fixture::SimpleAsk("ask-retry", 400).Build().ValueOr(rcb::Ask()));
    RCB_REQUIRE(replay.ok());
    RCB_CHECK(replay.value().replay);
    RCB_CHECK_EQ(replay.value().id.value(), decision_id);
    RCB_CHECK_EQ(service.value()->Summary().ValueOr(rcb::AccountingSummary())
                     .committed_total.Get(rcb::Dimension::ServiceCapacity),
                 committed_before);
    RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
}

RCB_TEST(revocation_and_withheld_capacity_survive_restart) {
  TempDir dir;
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-before", 1000).Build().ValueOr(rcb::Ask()))
                    .ok());
    RCB_REQUIRE(service.value()
                    ->RevokeOffer(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()), 1, 30)
                    .ok());
    RCB_CHECK(service.value()->Shutdown().ok());
  }
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    const rcb::ConservationReport report =
        service.value()->VerifyConservation().ValueOr(rcb::ConservationReport());
    RCB_CHECK(report.closed);
    RCB_CHECK_EQ(report.withheld_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{4000});
    RCB_CHECK_EQ(report.committed_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{0});
    RCB_CHECK_EQ(report.live_commitments, std::size_t{0});
    RCB_CHECK_EQ(report.revoked_commitments, std::size_t{1});
    // A withdrawn generation stays withdrawn after the restart.
    rcb::Result<rcb::Decision> refused =
        service.value()->AskNow(fixture::SimpleAsk("ask-after", 10).Build().ValueOr(rcb::Ask()));
    RCB_REQUIRE(refused.ok());
    RCB_CHECK(refused.value().outcome == rcb::DecisionOutcome::Refused);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
}

RCB_TEST(a_torn_tail_is_repaired_and_reported) {
  TempDir dir;
  rcb::i64 committed_before = 0;
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-torn", 800).Build().ValueOr(rcb::Ask()))
                    .ok());
    committed_before =
        service.value()->Summary().ValueOr(rcb::AccountingSummary())
            .committed_total.Get(rcb::Dimension::ServiceCapacity);
    RCB_CHECK(service.value()->Shutdown().ok());
  }

  const fs::path journal = dir.file("journal.log");
  const std::uintmax_t size = FileSize(journal);
  RCB_REQUIRE(size > 40);
  // Cut the last record short: exactly what a process killed mid-write leaves.
  Truncate(journal, size - 13);

  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    const rcb::RecoveryReport recovery = service.value()->Recovery();
    RCB_CHECK(recovery.torn_tail);
    RCB_CHECK(recovery.repaired);
    // The committed state is intact: only the torn record was dropped.
    RCB_CHECK_EQ(service.value()->Summary().ValueOr(rcb::AccountingSummary())
                     .committed_total.Get(rcb::Dimension::ServiceCapacity),
                 committed_before);
    RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
}

RCB_TEST(interior_corruption_is_refused_and_never_truncated) {
  TempDir dir;
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-1", 100).Build().ValueOr(rcb::Ask()))
                    .ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-2", 100).Build().ValueOr(rcb::Ask()))
                    .ok());
    RCB_CHECK(service.value()->Shutdown().ok());
  }

  const fs::path journal = dir.file("journal.log");
  const std::uintmax_t size_before = FileSize(journal);
  RCB_REQUIRE(size_before > 400);
  // Damage a byte in the middle of the file: it cannot be a torn tail.
  FlipByte(journal, 200);

  rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
      OpenStore(dir, rcb::DurabilityClass::Durable);
  RCB_CHECK(!service.ok());
  if (!service.ok()) {
    RCB_CHECK_EQ(service.status().code(), rcb::ErrorCode::PersistenceInteriorCorruption);
  }
  // The store was left exactly as it was found: no repair, no truncation.
  RCB_CHECK_EQ(FileSize(journal), size_before);
}

RCB_TEST(corrupt_snapshot_is_refused) {
  TempDir dir;
  {
    rcb::ServiceConfig config = Config(dir, rcb::DurabilityClass::Durable);
    config.compaction_enabled = true;
    config.compact_after_records = 2;
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service = rcb::BrokerService::Open(config);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-1", 100).Build().ValueOr(rcb::Ask()))
                    .ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-2", 100).Build().ValueOr(rcb::Ask()))
                    .ok());
    RCB_CHECK(service.value()->Shutdown().ok());
  }
  const fs::path snapshot = dir.file("state.snapshot");
  RCB_REQUIRE(fs::exists(snapshot));
  FlipByte(snapshot, 60);

  rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
      OpenStore(dir, rcb::DurabilityClass::Durable);
  RCB_CHECK(!service.ok());
  if (!service.ok()) {
    RCB_CHECK(service.status().category() == rcb::ErrorCategory::Persistence);
  }
}

RCB_TEST(compaction_preserves_the_state_across_restart) {
  TempDir dir;
  std::string ledger_before;
  std::string decision_before;
  rcb::i64 committed_before = 0;
  {
    rcb::ServiceConfig config = Config(dir, rcb::DurabilityClass::Durable);
    config.compaction_enabled = true;
    config.compact_after_records = 1000;
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service = rcb::BrokerService::Open(config);
    RCB_REQUIRE(service.ok());
    RCB_REQUIRE(service.value()->PublishOffer(kSiteA.Build().ValueOr(rcb::Offer())).ok());
    RCB_REQUIRE(service.value()
                    ->AskNow(fixture::SimpleAsk("ask-1", 900).Build().ValueOr(rcb::Ask()))
                    .ok());
    ledger_before = rcb::EncodeJson(rcb::ToJson(
        service.value()->FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()))
            .ValueOr(rcb::SiteLedger())));
    decision_before = rcb::CanonicalDecisionText(
        service.value()->FindDecision(rcb::AskKey::Make("ask-1").ValueOr(rcb::AskKey()))
            .ValueOr(rcb::Decision()));
    committed_before =
        service.value()->Summary().ValueOr(rcb::AccountingSummary())
            .committed_total.Get(rcb::Dimension::ServiceCapacity);
    RCB_CHECK(service.value()->Compact().ok());
    RCB_CHECK(service.value()->Compact().ok());  // idempotent, and safe twice
    RCB_CHECK(service.value()->Shutdown().ok());
  }
  RCB_CHECK(fs::exists(dir.file("state.snapshot")));
  {
    rcb::Result<std::unique_ptr<rcb::BrokerService>> service =
        OpenStore(dir, rcb::DurabilityClass::Durable);
    RCB_REQUIRE(service.ok());
    const rcb::RecoveryReport recovery = service.value()->Recovery();
    RCB_CHECK(recovery.snapshot_sequence_known);
    RCB_CHECK(recovery.snapshot_sequence > 0);
    RCB_CHECK_EQ(service.value()->Summary().ValueOr(rcb::AccountingSummary())
                     .committed_total.Get(rcb::Dimension::ServiceCapacity),
                 committed_before);
    // The epoch differs by design (a reopen fences the previous incarnation), so
    // the comparison is of the substantive state: the ledger and the decision.
    RCB_CHECK_EQ(rcb::EncodeJson(rcb::ToJson(
                     service.value()->FindSite(rcb::SiteId::Make("s1").ValueOr(rcb::SiteId()))
                         .ValueOr(rcb::SiteLedger()))),
                 ledger_before);
    RCB_CHECK_EQ(rcb::CanonicalDecisionText(
                     service.value()->FindDecision(rcb::AskKey::Make("ask-1").ValueOr(rcb::AskKey()))
                         .ValueOr(rcb::Decision())),
                 decision_before);
    RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
    // A retry after a restart that came through a snapshot is still idempotent.
    rcb::Result<rcb::Decision> replay =
        service.value()->AskNow(fixture::SimpleAsk("ask-1", 900).Build().ValueOr(rcb::Ask()));
    RCB_REQUIRE(replay.ok());
    RCB_CHECK(replay.value().replay);
    RCB_CHECK(service.value()->Shutdown().ok());
  }
}

RCB_TEST(the_store_is_fenced_to_one_writer) {
  TempDir dir;
  rcb::Result<std::unique_ptr<rcb::BrokerService>> first =
      OpenStore(dir, rcb::DurabilityClass::Durable);
  RCB_REQUIRE(first.ok());
  rcb::Result<std::unique_ptr<rcb::BrokerService>> second =
      OpenStore(dir, rcb::DurabilityClass::Durable);
  RCB_CHECK(!second.ok());
  if (!second.ok()) {
    RCB_CHECK(second.status().category() == rcb::ErrorCategory::Persistence);
  }
  RCB_CHECK(first.value()->Shutdown().ok());
  // Once the writer released the lock the store can be opened again.
  rcb::Result<std::unique_ptr<rcb::BrokerService>> third =
      OpenStore(dir, rcb::DurabilityClass::Durable);
  RCB_CHECK(third.ok());
  if (third.ok()) {
    RCB_CHECK(third.value()->Shutdown().ok());
  }
}

RCB_TEST(a_stale_epoch_can_never_commit) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, kSiteA));
  rcb::Result<rcb::AskPlan> plan =
      core.PlanAsk(fixture::SimpleAsk("ask-fenced", 100).Build().ValueOr(rcb::Ask()));
  RCB_REQUIRE(plan.ok());
  // A new incarnation advances the epoch; the old plan is no longer authority.
  core.SetEpoch(core.epoch() + 1);
  RCB_CHECK_ERROR(core.CommitPlan(plan.value()), rcb::ErrorCode::Fenced);
  RCB_CHECK_EQ(core.Summary().committed_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{0});
  // The same ask replanned under the new epoch commits normally.
  rcb::Result<rcb::AskPlan> replanned =
      core.PlanAsk(fixture::SimpleAsk("ask-fenced", 100).Build().ValueOr(rcb::Ask()));
  RCB_REQUIRE(replanned.ok());
  RCB_CHECK(replanned.value().broker_epoch == core.epoch());
  RCB_REQUIRE(core.CommitPlan(replanned.value()).ok());
  RCB_CHECK(core.VerifyConservation().closed);
}

RCB_TEST(a_plan_from_another_ledger_version_is_refused) {
  rcb::BrokerCore core;
  RCB_REQUIRE(fixture::Publish(core, kSiteA));
  rcb::Result<rcb::AskPlan> plan =
      core.PlanAsk(fixture::SimpleAsk("ask-version", 100).Build().ValueOr(rcb::Ask()));
  RCB_REQUIRE(plan.ok());
  // Something else changes the ledger between planning and committing.
  RCB_REQUIRE(fixture::Publish(core, fixture::SimpleOffer("s2", 1, fixture::Dimensions{0, 0, 0, 500})));
  RCB_CHECK_ERROR(core.CommitPlan(plan.value()), rcb::ErrorCode::Fenced);
  RCB_CHECK(core.VerifyConservation().closed);
}
