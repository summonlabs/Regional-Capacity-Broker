// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The concurrency and durability shell around the brokerage kernel.
//
// Ownership rules, deliberately small enough to audit:
//
//   * exactly one transactional mutex guards the kernel, the journal and every
//     mutation. There are no read locks anywhere, so a read-to-write upgrade is
//     impossible by construction rather than by discipline;
//   * no callback is invoked while any lock is held. Observers run after the
//     transaction has released the mutex, and a throwing observer cannot reach
//     the kernel;
//   * workers are joined without holding the transactional mutex, the queue
//     mutex or the state they need to finish, so shutdown cannot wait on work
//     while preventing its completion;
//   * the queue is bounded and its bound is checked before insertion, so a
//     submission storm is refused (or blocks, if the operator asked for that)
//     rather than growing without limit;
//   * a durability failure poisons the session: after a failed durable commit
//     the service refuses further mutations instead of continuing with a store
//     whose state is no longer trustworthy.

#ifndef RCB_SERVICE_HPP
#define RCB_SERVICE_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rcb/broker.hpp"
#include "rcb/journal.hpp"
#include "rcb/ledger.hpp"
#include "rcb/model.hpp"
#include "rcb/status.hpp"
#include "rcb/store.hpp"

namespace rcb {

enum class SubmissionMode : u8 {
  /// Refuse the submission when the bounded queue is full.
  RejectWhenFull = 0,
  /// Wait for room, and give up when the service stops or the token is
  /// cancelled.
  BlockWhenFull = 1,
};

enum class ShutdownMode : u8 {
  /// Finish the work already accepted, then stop.
  Drain = 0,
  /// Cancel the work already accepted, then stop.
  Abandon = 1,
};

struct ServiceConfig {
  /// Empty means an in-memory store: nothing is persisted and every decision is
  /// labelled Volatile.
  std::string store_directory;
  DurabilityClass durability = DurabilityClass::Durable;
  /// Bounded worker pool.
  std::size_t worker_threads = 2;
  /// Bounded queue.
  std::size_t queue_capacity = 256;
  SubmissionMode submission_mode = SubmissionMode::RejectWhenFull;
  bool compaction_enabled = true;
  /// Compact once the log holds at least this many records.
  std::size_t compact_after_records = 2048;
  BrokerConfig broker;
  JournalLimits journal;
};

/// A cooperative cancellation flag.
class CancelToken {
 public:
  void Cancel() noexcept { cancelled_.store(true, std::memory_order_relaxed); }
  [[nodiscard]] bool cancelled() const noexcept {
    return cancelled_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<bool> cancelled_{false};
};

/// What a caller learns about an ask. Cancellation that arrived after the
/// durable commit boundary is reported as such, and the decision stands: the
/// alternative would be a durable commitment nobody knows about.
struct AskOutcome {
  Decision decision;
  bool cancelled_before_start = false;
  bool cancelled_after_commit = false;
};

/// The handle for one asynchronous submission.
class AskHandle {
 public:
  AskHandle() = default;
  AskHandle(const AskHandle&) = delete;
  AskHandle& operator=(const AskHandle&) = delete;

  /// Blocks until the outcome is available.
  Result<AskOutcome> Wait();
  /// True when the outcome is already available.
  [[nodiscard]] bool ready() const;

  void Complete(Result<AskOutcome> outcome);
  void CancelPending();

 private:
  mutable std::mutex mutex_;
  std::condition_variable ready_;
  bool completed_ = false;
  std::shared_ptr<Result<AskOutcome>> outcome_;
};

class BrokerService {
 public:
  static Result<std::unique_ptr<BrokerService>> Open(const ServiceConfig& config);

  ~BrokerService();
  BrokerService(const BrokerService&) = delete;
  BrokerService& operator=(const BrokerService&) = delete;

  // ---- mutations ----

  /// Takes one ask through the whole transaction: plan, validate authority,
  /// prepare, apply, durably commit, publish. Synchronous, and serialised with
  /// every other mutation by the single transactional mutex.
  Result<Decision> AskNow(const Ask& ask, const CancelToken* token = nullptr);
  Result<OfferPublication> PublishOffer(const Offer& offer);
  Result<OfferRevocation> RevokeOffer(const SiteId& site, u64 generation, i64 at,
                                      RevocationReason reason = RevocationReason::OfferRevoked);

  // ---- asynchronous submission ----

  Result<std::shared_ptr<AskHandle>> Submit(const Ask& ask,
                                            std::shared_ptr<CancelToken> token = nullptr);

  // ---- queries ----

  Result<AccountingSummary> Summary();
  Result<ConservationReport> VerifyConservation();
  Result<std::string> CanonicalState();
  Result<Decision> FindDecision(const AskKey& key);
  Result<SiteLedger> FindSite(const SiteId& site);
  Result<std::vector<Decision>> RecentDecisions(std::size_t limit);
  [[nodiscard]] RecoveryReport Recovery() const;
  [[nodiscard]] bool durable() const noexcept { return durability_ != DurabilityClass::Volatile; }
  [[nodiscard]] bool failed() const;
  [[nodiscard]] std::size_t pending_submissions() const;

  /// Applies a compaction: flush, publish a verified snapshot, mark it, and
  /// start a fresh log. Refuses when anything is uncommitted.
  Status Compact();

  /// Observers run after the transaction, outside every lock. A throwing
  /// observer is caught and cannot affect the broker.
  void SetDecisionObserver(std::function<void(const Decision&)> observer);

  /// Stops accepting work and joins the workers. Idempotent.
  Status Shutdown(ShutdownMode mode = ShutdownMode::Drain);

 private:
  explicit BrokerService(ServiceConfig config) : config_(std::move(config)) {}

  Status OpenStores();
  Status RecoverState();
  /// \p accepted_before_stop is true for work a worker already took from the
  /// queue: a draining shutdown finishes what it accepted and refuses only what
  /// arrives afterwards.
  Result<Decision> ExecuteAsk(const Ask& ask, const CancelToken* token, bool accepted_before_stop);
  Status CompactLocked();
  void WorkerLoop();
  void MaybeCompactLocked();
  void NotifyObserver(const Decision& decision);
  void ResolvePending(bool cancelled);

  ServiceConfig config_;
  BrokerCore core_;
  std::unique_ptr<StoreLock> lock_;
  std::unique_ptr<ByteLog> log_;
  std::unique_ptr<SnapshotStore> snapshots_;
  std::unique_ptr<Journal> journal_;
  DurabilityClass durability_ = DurabilityClass::Volatile;
  RecoveryReport recovery_;

  mutable std::mutex txn_mutex_;
  /// Serialises Shutdown: two callers racing to join the same workers would be
  /// undefined behaviour, and a shutdown must be idempotent.
  std::mutex shutdown_mutex_;
  std::atomic<bool> failed_{false};
  std::size_t records_since_compaction_ = 0;

  struct PendingJob {
    Ask ask;
    std::shared_ptr<AskHandle> handle;
    std::shared_ptr<CancelToken> token;
  };

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::deque<PendingJob> queue_;
  std::vector<std::thread> workers_;
  std::atomic<bool> stopping_{false};

  std::mutex observer_mutex_;
  std::function<void(const Decision&)> observer_;
};

}  // namespace rcb

#endif  // RCB_SERVICE_HPP
