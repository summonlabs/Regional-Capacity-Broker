// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Concurrency suite: many submitters, one ledger; cancellation; bounded queues;
// shutdown in flight; observers that throw or re-enter the broker. Every case
// re-derives the conservation identity afterwards, because a race that loses or
// duplicates a commitment would show up there and nowhere else.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <latch>
#include <memory>
#include <string>
#include <thread>
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

rcb::ServiceConfig MemoryConfig(const std::size_t workers = 4, const std::size_t queue = 64) {
  rcb::ServiceConfig config;
  config.store_directory.clear();
  config.durability = rcb::DurabilityClass::Volatile;
  config.worker_threads = workers;
  config.queue_capacity = queue;
  config.compaction_enabled = false;
  return config;
}

rcb::Result<std::unique_ptr<rcb::BrokerService>> OpenMemory(const std::size_t workers = 4,
                                                            const std::size_t queue = 64) {
  return rcb::BrokerService::Open(MemoryConfig(workers, queue));
}

OfferSpec SiteOffer(const std::string& site, const rcb::u64 generation, const rcb::i64 service) {
  return fixture::SimpleOffer(site, generation, Dimensions{0, 0, 0, service}, "fd-" + site);
}

/// Waits for every handle and returns the outcomes; fails the test when a handle
/// cannot be resolved at all.
std::vector<rcb::Result<rcb::AskOutcome>> Collect(
    const std::vector<std::shared_ptr<rcb::AskHandle>>& handles) {
  std::vector<rcb::Result<rcb::AskOutcome>> outcomes;
  outcomes.reserve(handles.size());
  for (const std::shared_ptr<rcb::AskHandle>& handle : handles) {
    outcomes.push_back(handle->Wait());
  }
  return outcomes;
}

}  // namespace

RCB_TEST(parallel_submissions_commit_exactly_once_each) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(4, 512);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 1000000).Build().ValueOr(rcb::Offer())).ok());

  constexpr int kThreads = 8;
  constexpr int kPerThread = 25;
  std::vector<std::shared_ptr<rcb::AskHandle>> handles;
  std::mutex handles_mutex;
  std::vector<std::thread> submitters;
  std::atomic<int> refused{0};
  for (int thread = 0; thread < kThreads; ++thread) {
    submitters.emplace_back([&, thread] {
      for (int index = 0; index < kPerThread; ++index) {
        AskSpec spec = SimpleAsk("ask-" + std::to_string(thread) + "-" + std::to_string(index), 100);
        rcb::Result<rcb::Ask> ask = spec.Build();
        RCB_CHECK(ask.ok());
        rcb::Result<std::shared_ptr<rcb::AskHandle>> submitted = service.value()->Submit(ask.value());
        if (!submitted.ok()) {
          refused.fetch_add(1);
          RCB_CHECK(submitted.status().code() == rcb::ErrorCode::QueueFull ||
                    submitted.status().code() == rcb::ErrorCode::ShuttingDown);
          continue;
        }
        std::lock_guard<std::mutex> lock(handles_mutex);
        handles.push_back(submitted.value());
      }
    });
  }
  for (std::thread& thread : submitters) {
    thread.join();
  }
  const std::vector<rcb::Result<rcb::AskOutcome>> outcomes = Collect(handles);
  rcb::i64 committed = 0;
  std::vector<std::string> ids;
  for (const rcb::Result<rcb::AskOutcome>& outcome : outcomes) {
    RCB_REQUIRE(outcome.ok());
    RCB_CHECK(outcome.value().decision.outcome == rcb::DecisionOutcome::Accepted);
    for (const rcb::Allocation& allocation : outcome.value().decision.allocations) {
      committed += allocation.allocatable_amount.Get(rcb::Dimension::ServiceCapacity);
      ids.push_back(allocation.id.value());
    }
  }
  std::sort(ids.begin(), ids.end());
  RCB_CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
  const rcb::AccountingSummary summary = service.value()->Summary().ValueOr(rcb::AccountingSummary());
  RCB_CHECK_EQ(summary.committed_total.Get(rcb::Dimension::ServiceCapacity), committed);
  RCB_CHECK_EQ(committed, static_cast<rcb::i64>(handles.size()) * 100);
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}

RCB_TEST(concurrent_offers_and_asks_stay_conserved) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(4, 256);
  RCB_REQUIRE(service.ok());

  std::vector<std::thread> threads;
  for (int site = 0; site < 6; ++site) {
    threads.emplace_back([&, site] {
      for (rcb::u64 generation = 1; generation <= 4; ++generation) {
        OfferSpec spec = SiteOffer("site-" + std::to_string(site), generation, 10000);
        rcb::Result<rcb::Offer> offer = spec.Build();
        RCB_CHECK(offer.ok());
        rcb::Result<rcb::OfferPublication> published = service.value()->PublishOffer(offer.value());
        // A concurrent publisher may have moved the generation on already; a
        // stale or conflicting publication is a legitimate refusal here.
        if (!published.ok()) {
          RCB_CHECK(published.status().code() == rcb::ErrorCode::StaleGeneration ||
                    published.status().code() == rcb::ErrorCode::Conflict ||
                    published.status().code() == rcb::ErrorCode::ShuttingDown);
        }
      }
    });
  }
  for (int thread = 0; thread < 6; ++thread) {
    threads.emplace_back([&, thread] {
      for (int index = 0; index < 20; ++index) {
        AskSpec spec = SimpleAsk("ask-" + std::to_string(thread) + "-" + std::to_string(index), 250);
        rcb::Result<rcb::Ask> ask = spec.Build();
        RCB_REQUIRE(ask.ok());
        rcb::Result<rcb::Decision> decision = service.value()->AskNow(ask.value());
        RCB_REQUIRE(decision.ok());
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  const rcb::ConservationReport report =
      service.value()->VerifyConservation().ValueOr(rcb::ConservationReport());
  RCB_CHECK(report.closed);
  // Everything committed is accounted for exactly once.
  RCB_CHECK(report.allocatable_total.Covers(report.committed_total));
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}

RCB_TEST(a_retry_storm_commits_once) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(4, 256);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 100000).Build().ValueOr(rcb::Offer())).ok());

  constexpr int kThreads = 8;
  std::vector<std::thread> threads;
  std::mutex results_mutex;
  std::vector<rcb::Status> failures;
  int replays = 0;
  int firsts = 0;
  for (int thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&] {
      rcb::Result<rcb::Ask> ask = SimpleAsk("ask-same-key", 500).Build();
      RCB_REQUIRE(ask.ok());
      rcb::Result<rcb::Decision> decision = service.value()->AskNow(ask.value());
      std::lock_guard<std::mutex> lock(results_mutex);
      if (!decision.ok()) {
        failures.push_back(decision.status());
        return;
      }
      if (decision.value().replay) {
        ++replays;
      } else {
        ++firsts;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  RCB_CHECK(failures.empty());
  RCB_CHECK_EQ(firsts, 1);
  RCB_CHECK_EQ(replays, kThreads - 1);
  const rcb::AccountingSummary summary = service.value()->Summary().ValueOr(rcb::AccountingSummary());
  RCB_CHECK_EQ(summary.committed_total.Get(rcb::Dimension::ServiceCapacity), rcb::i64{500});
  RCB_CHECK_EQ(summary.live_commitments, std::size_t{1});
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}

RCB_TEST(cancelled_submissions_never_commit) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(2, 64);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 100000).Build().ValueOr(rcb::Offer())).ok());

  int cancelled = 0;
  for (int index = 0; index < 40; ++index) {
    auto token = std::make_shared<rcb::CancelToken>();
    AskSpec spec = SimpleAsk("ask-cancel-" + std::to_string(index), 100);
    rcb::Result<rcb::Ask> ask = spec.Build();
    RCB_REQUIRE(ask.ok());
    if (index % 2 == 0) {
      token->Cancel();
    }
    rcb::Result<std::shared_ptr<rcb::AskHandle>> submitted = service.value()->Submit(ask.value(), token);
    RCB_REQUIRE(submitted.ok());
    if (index % 2 == 1 && index > 30) {
      // Cancel something that is likely still queued; whether it committed or
      // not, the outcome must say which happened.
      token->Cancel();
    }
    rcb::Result<rcb::AskOutcome> outcome = submitted.value()->Wait();
    if (!outcome.ok()) {
      RCB_CHECK(outcome.status().code() == rcb::ErrorCode::Cancelled);
      ++cancelled;
      continue;
    }
    RCB_CHECK(outcome.value().decision.committed_anything() ||
              outcome.value().cancelled_before_start);
    if (outcome.value().cancelled_before_start) {
      ++cancelled;
      RCB_CHECK(outcome.value().decision.allocations.empty());
    }
  }
  RCB_CHECK(cancelled >= 20);
  const rcb::AccountingSummary summary = service.value()->Summary().ValueOr(rcb::AccountingSummary());
  // Every commitment in the ledger belongs to an outcome that reported one.
  RCB_CHECK(summary.live_commitments <= 20U);
  const rcb::ConservationReport report =
      service.value()->VerifyConservation().ValueOr(rcb::ConservationReport());
  RCB_CHECK(report.closed);
  RCB_CHECK_EQ(report.committed_total.Get(rcb::Dimension::ServiceCapacity),
               static_cast<rcb::i64>(summary.live_commitments) * 100);
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}

RCB_TEST(the_submission_queue_is_bounded_and_reports_it) {
  // One worker held inside a blocking observer makes the queue the only place
  // work can go, so the bound is reached deterministically rather than by
  // racing a fast worker.
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(1, 2);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 1000000).Build().ValueOr(rcb::Offer())).ok());

  std::latch release(1);
  std::atomic<bool> observer_entered{false};
  service.value()->SetDecisionObserver([&](const rcb::Decision&) {
    if (!observer_entered.exchange(true)) {
      release.wait();
    }
  });

  rcb::Result<rcb::Ask> first = SimpleAsk("ask-block", 10).Build();
  RCB_REQUIRE(first.ok());
  RCB_REQUIRE(service.value()->Submit(first.value()).ok());
  while (!observer_entered.load()) {
    std::this_thread::yield();
  }
  // The worker is inside the observer, so nothing is being drained.
  RCB_REQUIRE(service.value()->Submit(first.value()).ok());   // fills the queue
  RCB_REQUIRE(service.value()->Submit(first.value()).ok());   // queue is now full
  rcb::Result<rcb::Ask> extra = SimpleAsk("ask-overflow", 10).Build();
  RCB_REQUIRE(extra.ok());
  rcb::Result<std::shared_ptr<rcb::AskHandle>> overflow = service.value()->Submit(extra.value());
  RCB_CHECK(!overflow.ok());
  if (!overflow.ok()) {
    RCB_CHECK_EQ(overflow.status().code(), rcb::ErrorCode::QueueFull);
  }
  release.count_down();
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
}

RCB_TEST(shutdown_drains_accepted_work) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(4, 256);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 1000000).Build().ValueOr(rcb::Offer())).ok());

  std::vector<std::shared_ptr<rcb::AskHandle>> handles;
  for (int index = 0; index < 120; ++index) {
    rcb::Result<rcb::Ask> ask = SimpleAsk("ask-drain-" + std::to_string(index), 10).Build();
    RCB_REQUIRE(ask.ok());
    rcb::Result<std::shared_ptr<rcb::AskHandle>> submitted = service.value()->Submit(ask.value());
    RCB_REQUIRE(submitted.ok());
    handles.push_back(submitted.value());
  }
  // Two threads shutting down concurrently must be safe.
  std::thread other([&] { (void)service.value()->Shutdown(rcb::ShutdownMode::Drain); });
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
  other.join();

  int accepted = 0;
  for (const std::shared_ptr<rcb::AskHandle>& handle : handles) {
    rcb::Result<rcb::AskOutcome> outcome = handle->Wait();
    RCB_REQUIRE(outcome.ok());
    if (outcome.value().decision.outcome == rcb::DecisionOutcome::Accepted) {
      ++accepted;
    }
  }
  const rcb::AccountingSummary summary = service.value()->Summary().ValueOr(rcb::AccountingSummary());
  RCB_CHECK_EQ(summary.committed_total.Get(rcb::Dimension::ServiceCapacity),
               static_cast<rcb::i64>(accepted) * 10);
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
}

RCB_TEST(abandoning_shutdown_resolves_every_waiting_caller) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(1, 512);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 1000000).Build().ValueOr(rcb::Offer())).ok());

  std::vector<std::shared_ptr<rcb::AskHandle>> handles;
  for (int index = 0; index < 100; ++index) {
    rcb::Result<rcb::Ask> ask = SimpleAsk("ask-abandon-" + std::to_string(index), 10).Build();
    RCB_REQUIRE(ask.ok());
    rcb::Result<std::shared_ptr<rcb::AskHandle>> submitted = service.value()->Submit(ask.value());
    RCB_REQUIRE(submitted.ok());
    handles.push_back(submitted.value());
  }
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Abandon).ok());

  int resolved = 0;
  for (const std::shared_ptr<rcb::AskHandle>& handle : handles) {
    rcb::Result<rcb::AskOutcome> outcome = handle->Wait();
    RCB_REQUIRE(outcome.ok());
    ++resolved;
    if (outcome.value().cancelled_before_start) {
      RCB_CHECK(outcome.value().decision.allocations.empty());
    }
  }
  RCB_CHECK_EQ(resolved, 100);
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
  // The service refuses new work once it is stopping.
  rcb::Result<rcb::Ask> late = SimpleAsk("ask-late", 10).Build();
  RCB_REQUIRE(late.ok());
  RCB_CHECK_ERROR(service.value()->Submit(late.value()), rcb::ErrorCode::ShuttingDown);
  RCB_CHECK_ERROR(service.value()->AskNow(late.value()), rcb::ErrorCode::ShuttingDown);
}

RCB_TEST(an_observer_that_throws_cannot_break_the_broker) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(2, 32);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 100000).Build().ValueOr(rcb::Offer())).ok());

  std::atomic<int> observed{0};
  service.value()->SetDecisionObserver([&](const rcb::Decision&) {
    observed.fetch_add(1);
    throw std::runtime_error("observer failure");
  });
  for (int index = 0; index < 10; ++index) {
    rcb::Result<rcb::Ask> ask = SimpleAsk("ask-throw-" + std::to_string(index), 100).Build();
    RCB_REQUIRE(ask.ok());
    rcb::Result<rcb::Decision> decision = service.value()->AskNow(ask.value());
    RCB_CHECK(decision.ok());
  }
  RCB_CHECK_EQ(observed.load(), 10);
  RCB_CHECK(service.value()->VerifyConservation().ValueOr(rcb::ConservationReport()).closed);
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}

RCB_TEST(an_observer_may_re_enter_the_broker_without_deadlocking) {
  rcb::Result<std::unique_ptr<rcb::BrokerService>> service = OpenMemory(1, 32);
  RCB_REQUIRE(service.ok());
  RCB_REQUIRE(service.value()->PublishOffer(SiteOffer("s1", 1, 100000).Build().ValueOr(rcb::Offer())).ok());

  std::atomic<int> reentries{0};
  service.value()->SetDecisionObserver([&](const rcb::Decision&) {
    // Both of these take the transactional mutex. If the observer were invoked
    // while it was held, this would deadlock instead of returning.
    rcb::Result<rcb::AccountingSummary> summary = service.value()->Summary();
    rcb::Result<rcb::ConservationReport> report = service.value()->VerifyConservation();
    RCB_CHECK(summary.ok());
    RCB_CHECK(report.ok());
    reentries.fetch_add(1);
  });
  rcb::Result<rcb::Ask> ask = SimpleAsk("ask-reentrant", 100).Build();
  RCB_REQUIRE(ask.ok());
  RCB_REQUIRE(service.value()->AskNow(ask.value()).ok());
  RCB_CHECK_EQ(reentries.load(), 1);
  RCB_CHECK(service.value()->Shutdown(rcb::ShutdownMode::Drain).ok());
}
