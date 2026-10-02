// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/service.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <utility>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"
#include "rcb/json.hpp"
#include "rcb/serialize.hpp"
#include "rcb/version.hpp"

namespace rcb {
namespace {

constexpr u8 kSnapshotMagic[8] = {'R', 'C', 'B', 'S', 'N', 'A', 'P', '1'};
constexpr u32 kSnapshotVersion = 1;
constexpr std::size_t kSnapshotPrefix = 8 + 4 + 8 + 8 + Digest::kSize;
constexpr std::size_t kSnapshotTrailer = 4;

void StoreLe32(u8* out, const u32 value) {
  out[0] = static_cast<u8>(value & 0xFFU);
  out[1] = static_cast<u8>((value >> 8U) & 0xFFU);
  out[2] = static_cast<u8>((value >> 16U) & 0xFFU);
  out[3] = static_cast<u8>((value >> 24U) & 0xFFU);
}

void StoreLe64(u8* out, const u64 value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<u8>((value >> (i * 8U)) & 0xFFU);
  }
}

u32 LoadLe32(const u8* data) {
  return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8U) |
         (static_cast<u32>(data[2]) << 16U) | (static_cast<u32>(data[3]) << 24U);
}

u64 LoadLe64(const u8* data) {
  u64 value = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    value |= static_cast<u64>(data[i]) << (i * 8U);
  }
  return value;
}

/// The snapshot envelope: a self-describing frame carrying the sequence it
/// covers, the digest of its body and a checksum over the whole frame. The body
/// is the canonical state text, so a snapshot is machine-verifiable and
/// readable at the same time.
Bytes EncodeSnapshotEnvelope(const std::string_view body, const u64 sequence,
                             const Digest& digest) {
  Bytes out;
  out.reserve(kSnapshotPrefix + body.size() + kSnapshotTrailer);
  out.insert(out.end(), std::begin(kSnapshotMagic), std::end(kSnapshotMagic));
  std::array<u8, 8> wide{};
  std::array<u8, 4> small{};
  StoreLe32(small.data(), kSnapshotVersion);
  out.insert(out.end(), small.begin(), small.end());
  StoreLe64(wide.data(), sequence);
  out.insert(out.end(), wide.begin(), wide.end());
  StoreLe64(wide.data(), static_cast<u64>(body.size()));
  out.insert(out.end(), wide.begin(), wide.end());
  out.insert(out.end(), digest.bytes().begin(), digest.bytes().end());
  out.insert(out.end(), reinterpret_cast<const u8*>(body.data()),
             reinterpret_cast<const u8*>(body.data()) + body.size());
  StoreLe32(small.data(), Crc32c(std::span<const u8>(out.data(), out.size())));
  out.insert(out.end(), small.begin(), small.end());
  return out;
}

struct SnapshotBody {
  u64 sequence = 0;
  std::string body;
};

Result<SnapshotBody> DecodeSnapshotEnvelope(const Bytes& frame) {
  if (frame.size() < kSnapshotPrefix + kSnapshotTrailer) {
    return Fail<SnapshotBody>(ErrorCode::PersistenceCorrupt, "the snapshot frame is too short");
  }
  if (!std::equal(std::begin(kSnapshotMagic), std::end(kSnapshotMagic), frame.begin())) {
    return Fail<SnapshotBody>(ErrorCode::PersistenceCorrupt, "the snapshot frame has no magic");
  }
  if (LoadLe32(frame.data() + 8) != kSnapshotVersion) {
    return Fail<SnapshotBody>(ErrorCode::UnsupportedVersion,
                              "the snapshot frame carries an unsupported format version");
  }
  const u64 declared_crc = LoadLe32(frame.data() + frame.size() - kSnapshotTrailer);
  const u32 actual_crc = Crc32c(std::span<const u8>(frame.data(), frame.size() - kSnapshotTrailer));
  if (declared_crc != actual_crc) {
    return Fail<SnapshotBody>(ErrorCode::PersistenceCorrupt,
                              "the snapshot frame fails its checksum");
  }
  const u64 sequence = LoadLe64(frame.data() + 12);
  const u64 body_bytes = LoadLe64(frame.data() + 20);
  if (body_bytes != frame.size() - kSnapshotPrefix - kSnapshotTrailer) {
    return Fail<SnapshotBody>(ErrorCode::PersistenceCorrupt,
                              "the snapshot frame declares a body length it does not have");
  }
  const u8* body = frame.data() + kSnapshotPrefix;
  Sha256 hasher;
  hasher.Update(std::span<const u8>(body, static_cast<std::size_t>(body_bytes)));
  std::array<u8, Digest::kSize> declared_bytes{};
  std::memcpy(declared_bytes.data(), frame.data() + 28, Digest::kSize);
  if (hasher.Finalize() != Digest::FromBytes(declared_bytes)) {
    return Fail<SnapshotBody>(ErrorCode::PersistenceCorrupt,
                              "the snapshot body does not match the digest it carries");
  }
  SnapshotBody decoded;
  decoded.sequence = sequence;
  decoded.body.assign(reinterpret_cast<const char*>(body), static_cast<std::size_t>(body_bytes));
  return decoded;
}

}  // namespace

// ---- ask handle ----------------------------------------------------------

Result<AskOutcome> AskHandle::Wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  ready_.wait(lock, [this] { return completed_; });
  RCB_ASSERT(outcome_ != nullptr);
  return *outcome_;
}

bool AskHandle::ready() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_;
}

void AskHandle::Complete(Result<AskOutcome> outcome) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (completed_) {
      return;
    }
    outcome_ = std::make_shared<Result<AskOutcome>>(std::move(outcome));
    completed_ = true;
  }
  ready_.notify_all();
}

void AskHandle::CancelPending() {
  AskOutcome outcome;
  outcome.cancelled_before_start = true;
  Complete(Result<AskOutcome>(outcome));
}

// ---- open ----------------------------------------------------------------

Result<std::unique_ptr<BrokerService>> BrokerService::Open(const ServiceConfig& config) {
  auto service = std::unique_ptr<BrokerService>(new BrokerService(config));
  const Status stores = service->OpenStores();
  if (!stores.ok()) {
    return Fail<std::unique_ptr<BrokerService>>(stores);
  }
  const Status recovered = service->RecoverState();
  if (!recovered.ok()) {
    return Fail<std::unique_ptr<BrokerService>>(recovered);
  }
  service->workers_.reserve(service->config_.worker_threads);
  for (std::size_t i = 0; i < service->config_.worker_threads; ++i) {
    service->workers_.emplace_back([raw = service.get()] { raw->WorkerLoop(); });
  }
  return service;
}

Status BrokerService::OpenStores() {
  if (config_.store_directory.empty()) {
    durability_ = DurabilityClass::Volatile;
    log_ = std::make_unique<MemoryByteLog>();
    snapshots_ = std::make_unique<MemorySnapshotStore>();
    return Status::Ok();
  }
  const Status created = CreateDirectories(config_.store_directory);
  if (!created.ok()) {
    return created;
  }
  const char last = config_.store_directory.back();
  const std::string separator = (last == '/' || last == '\\') ? std::string() : std::string("/");
  const std::string base = config_.store_directory + separator;

  Result<std::unique_ptr<StoreLock>> acquired = StoreLock::Acquire(base + "store.lock");
  if (!acquired.ok()) {
    return acquired.status();
  }
  lock_ = std::move(acquired.value());

  Result<std::unique_ptr<FileByteLog>> opened = FileByteLog::Open(base + "journal.log", true);
  if (!opened.ok()) {
    return opened.status();
  }
  log_ = std::move(opened.value());
  snapshots_ = std::make_unique<FileSnapshotStore>(base + "state.snapshot");
  durability_ = config_.durability;
  return Status::Ok();
}

Status BrokerService::RecoverState() {
  bool has_snapshot = false;
  u64 snapshot_sequence = 0;

  Result<std::optional<Bytes>> loaded = snapshots_->Load(Limits::kMaxSnapshotBytes);
  if (!loaded.ok()) {
    return loaded.status();
  }
  if (loaded.value().has_value()) {
    Result<SnapshotBody> decoded = DecodeSnapshotEnvelope(loaded.value().value());
    if (!decoded.ok()) {
      return Status::Error(decoded.status().code(),
                           "the published snapshot could not be adopted: " +
                               decoded.status().ToString());
    }
    Result<JsonValue> document = ParseJson(decoded.value().body);
    if (!document.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "the snapshot body is not valid JSON: " +
                               document.status().ToString());
    }
    const Status restored = RestoreStateFromJson(document.value(), core_);
    if (!restored.ok()) {
      return Status::Error(restored.code(), "restoring the snapshot failed: " + restored.ToString());
    }
    has_snapshot = true;
    snapshot_sequence = decoded.value().sequence;
  }

  Result<std::unique_ptr<Journal>> journal =
      Journal::Open(*log_, config_.journal, has_snapshot, snapshot_sequence);
  if (!journal.ok()) {
    return journal.status();
  }
  journal_ = std::move(journal.value());
  const Status applied = journal_->ApplyTo(core_);
  if (!applied.ok()) {
    return applied;
  }
  const ConservationReport conservation = core_.VerifyConservation();
  if (!conservation.closed) {
    std::string detail = "the recovered ledger does not satisfy its conservation identity";
    if (!conservation.violations.empty()) {
      detail += ": ";
      detail += conservation.violations.front().scope;
      detail += " ";
      detail += conservation.violations.front().detail;
    }
    return Status::Error(ErrorCode::PersistenceCorrupt, std::move(detail));
  }

  // Fence everything written by an earlier incarnation: the epoch advances on
  // every open, so a plan or a record from an older epoch can never be adopted.
  const u64 previous_epoch = std::max(core_.epoch(), journal_->recovery().last_epoch);
  const u64 next_epoch = previous_epoch + 1;
  core_.SetEpoch(next_epoch);
  journal_->SetEpoch(next_epoch);
  const Status prepared = journal_->Prepare(RecordKind::Epoch, 0, EncodeEpochRecord(next_epoch));
  if (!prepared.ok()) {
    return prepared;
  }
  const Status marked = journal_->MarkCommitted(durability_);
  if (!marked.ok()) {
    return marked;
  }
  recovery_ = journal_->recovery();
  recovery_.last_epoch = next_epoch;
  records_since_compaction_ = journal_->records().size();
  return Status::Ok();
}

// ---- transactions --------------------------------------------------------

Result<Decision> BrokerService::ExecuteAsk(const Ask& ask, const CancelToken* token,
                                          const bool accepted_before_stop) {
  std::unique_lock<std::mutex> lock(txn_mutex_);
  if (failed_.load()) {
    return Fail<Decision>(ErrorCode::PersistenceIoError,
                          "the store failed a durable commit; close and reopen it before "
                          "continuing");
  }
  if (stopping_.load() && !accepted_before_stop) {
    return Fail<Decision>(ErrorCode::ShuttingDown, "the service is stopping");
  }
  if (token != nullptr && token->cancelled()) {
    return Fail<Decision>(ErrorCode::Cancelled, "the ask was cancelled before it started");
  }

  Result<AskPlan> plan = core_.PlanAsk(ask);
  if (!plan.ok()) {
    return Fail<Decision>(plan.status());
  }
  if (plan.value().IsReplay()) {
    // A retry of an ask that was already decided returns the recorded decision
    // and consumes nothing.
    return core_.CommitPlan(plan.value());
  }

  Result<Decision> materialized = core_.MaterializeDecision(plan.value());
  if (!materialized.ok()) {
    return materialized;
  }

  // 1. Prepare: the exact decision document is written and read back, but it
  //    carries no marker, so a crash here leaves no trace in the ledger.
  const Status prepared =
      journal_->Prepare(RecordKind::Decision, materialized.value().broker_sequence,
                        EncodeDecisionRecord(materialized.value()));
  if (!prepared.ok()) {
    failed_.store(true);
    return Fail<Decision>(prepared);
  }

  // 2. Apply: the ledger reserves the capacity. A refusal here retires the
  //    prepared record without a marker, so nothing is published.
  Result<Decision> applied = core_.ApplyMaterializedDecision(materialized.value());
  if (!applied.ok()) {
    (void)journal_->AbandonPrepared();
    return applied;
  }

  // 3. Durable commit: only now can the decision be published to the caller.
  const Status committed = journal_->MarkCommitted(durability_);
  if (!committed.ok()) {
    failed_.store(true);
    return Fail<Decision>(ErrorCode::PersistenceIoError,
                          "the decision was applied but its commit marker could not be made "
                          "durable; the store is no longer trustworthy and further mutations "
                          "are refused: " +
                              committed.ToString());
  }

  // 4. Publish: the decision now reports the durability boundary it reached.
  const Status stamped =
      core_.SetDecisionDurability(applied.value().id, durability_);
  if (stamped.ok()) {
    applied.value().durability = durability_;
  }
  ++records_since_compaction_;
  MaybeCompactLocked();
  return applied;
}

Result<Decision> BrokerService::AskNow(const Ask& ask, const CancelToken* token) {
  Result<Decision> decision = ExecuteAsk(ask, token, false);
  if (decision.ok()) {
    NotifyObserver(decision.value());
  }
  return decision;
}

Result<OfferPublication> BrokerService::PublishOffer(const Offer& offer) {
  std::unique_lock<std::mutex> lock(txn_mutex_);
  if (failed_.load()) {
    return Fail<OfferPublication>(ErrorCode::PersistenceIoError,
                                  "the store failed a durable commit; close and reopen it");
  }
  if (stopping_.load()) {
    return Fail<OfferPublication>(ErrorCode::ShuttingDown, "the service is stopping");
  }
  const u64 sequence = core_.next_sequence();
  const Status prepared = journal_->Prepare(RecordKind::Offer, sequence, EncodeOfferRecord(offer));
  if (!prepared.ok()) {
    failed_.store(true);
    return Fail<OfferPublication>(prepared);
  }
  Result<OfferPublication> published = core_.PublishOffer(offer);
  if (!published.ok()) {
    (void)journal_->AbandonPrepared();
    return published;
  }
  if (published.value().idempotent_replay) {
    (void)journal_->AbandonPrepared();
    return published;
  }
  const Status committed = journal_->MarkCommitted(durability_);
  if (!committed.ok()) {
    failed_.store(true);
    return Fail<OfferPublication>(
        ErrorCode::PersistenceIoError,
        "the publication could not be made durable: " + committed.ToString());
  }
  ++records_since_compaction_;
  MaybeCompactLocked();
  return published;
}

Result<OfferRevocation> BrokerService::RevokeOffer(const SiteId& site, const u64 generation,
                                                   const i64 at, const RevocationReason reason) {
  std::unique_lock<std::mutex> lock(txn_mutex_);
  if (failed_.load()) {
    return Fail<OfferRevocation>(ErrorCode::PersistenceIoError,
                                 "the store failed a durable commit; close and reopen it");
  }
  if (stopping_.load()) {
    return Fail<OfferRevocation>(ErrorCode::ShuttingDown, "the service is stopping");
  }
  const u64 sequence = core_.next_sequence();
  const Status prepared = journal_->Prepare(RecordKind::Revocation, sequence,
                                            EncodeRevocationRecord(site, generation, at, reason));
  if (!prepared.ok()) {
    failed_.store(true);
    return Fail<OfferRevocation>(prepared);
  }
  Result<OfferRevocation> revoked = core_.RevokeOffer(site, generation, at, reason);
  if (!revoked.ok()) {
    (void)journal_->AbandonPrepared();
    return revoked;
  }
  const Status committed = journal_->MarkCommitted(durability_);
  if (!committed.ok()) {
    failed_.store(true);
    return Fail<OfferRevocation>(
        ErrorCode::PersistenceIoError,
        "the revocation could not be made durable: " + committed.ToString());
  }
  ++records_since_compaction_;
  MaybeCompactLocked();
  return revoked;
}

// ---- compaction ----------------------------------------------------------

Status BrokerService::CompactLocked() {
  if (journal_ == nullptr) {
    return Status::Error(ErrorCode::Unsupported, "there is no durable store to compact");
  }
  if (journal_->has_pending()) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "a prepared record is waiting; compaction would drop it");
  }
  const Status flushed = journal_->Flush();
  if (!flushed.ok()) {
    failed_.store(true);
    return flushed;
  }
  const std::string state = CanonicalStateText(core_);
  const Digest digest = Sha256Of(state);
  const u64 sequence = core_.sequence();
  const Bytes envelope = EncodeSnapshotEnvelope(state, sequence, digest);
  if (envelope.size() > Limits::kMaxSnapshotBytes) {
    return Status::Error(ErrorCode::LimitExceeded, "the snapshot would exceed the size limit");
  }
  // Publish the snapshot first. Only once it is on disk and verified is the log
  // reset, so an interrupted compaction leaves both copies intact and the log
  // can never lose a state the snapshot does not already hold.
  const Status published = snapshots_->Publish(envelope);
  if (!published.ok()) {
    return published;
  }
  const Status noted = journal_->NoteSnapshot(sequence, digest, envelope.size());
  if (!noted.ok()) {
    return noted;
  }
  const Status reset = journal_->Reset(core_.epoch());
  if (!reset.ok()) {
    failed_.store(true);
    return reset;
  }
  records_since_compaction_ = 1;
  return Status::Ok();
}

Status BrokerService::Compact() {
  std::unique_lock<std::mutex> lock(txn_mutex_);
  if (failed_.load()) {
    return Status::Error(ErrorCode::PersistenceIoError,
                         "the store failed a durable commit; compaction is refused");
  }
  return CompactLocked();
}

void BrokerService::MaybeCompactLocked() {
  if (!config_.compaction_enabled || journal_ == nullptr) {
    return;
  }
  if (records_since_compaction_ < config_.compact_after_records) {
    return;
  }
  // Compaction is an optimisation: a failure is reported through the operator
  // surface but never fails the transaction that triggered it, because the log
  // still holds everything the snapshot would have covered.
  (void)CompactLocked();
}

// ---- asynchronous submission --------------------------------------------

Result<std::shared_ptr<AskHandle>> BrokerService::Submit(const Ask& ask,
                                                         std::shared_ptr<CancelToken> token) {
  auto handle = std::make_shared<AskHandle>();
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    if (stopping_.load()) {
      return Fail<std::shared_ptr<AskHandle>>(ErrorCode::ShuttingDown, "the service is stopping");
    }
    if (queue_.size() >= config_.queue_capacity) {
      if (config_.submission_mode == SubmissionMode::RejectWhenFull) {
        return Fail<std::shared_ptr<AskHandle>>(ErrorCode::QueueFull,
                                                "the submission queue is full");
      }
      queue_ready_.wait(lock, [this] {
        return stopping_.load() || queue_.size() < config_.queue_capacity;
      });
      if (stopping_.load()) {
        return Fail<std::shared_ptr<AskHandle>>(ErrorCode::ShuttingDown,
                                                "the service is stopping");
      }
    }
    PendingJob job;
    job.ask = ask;
    job.handle = handle;
    job.token = std::move(token);
    queue_.push_back(std::move(job));
  }
  queue_ready_.notify_one();
  return handle;
}

void BrokerService::WorkerLoop() {
  for (;;) {
    PendingJob job;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_ready_.wait(lock, [this] { return stopping_.load() || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_.load()) {
          return;
        }
        continue;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }
    queue_ready_.notify_one();

    // No lock is held here: the transaction takes the transactional mutex on its
    // own and releases it before the handle is completed.
    if (job.token != nullptr && job.token->cancelled()) {
      job.handle->CancelPending();
      continue;
    }
    try {
      Result<Decision> decision = ExecuteAsk(job.ask, job.token.get(), true);
      if (decision.ok()) {
        AskOutcome outcome;
        outcome.decision = decision.value();
        if (job.token != nullptr && job.token->cancelled()) {
          outcome.cancelled_after_commit = true;
        }
        job.handle->Complete(Result<AskOutcome>(std::move(outcome)));
        NotifyObserver(decision.value());
      } else {
        job.handle->Complete(Result<AskOutcome>(decision.status()));
      }
    } catch (const std::exception& error) {
      job.handle->Complete(Result<AskOutcome>(Status::Error(
          ErrorCode::Internal, std::string("a worker caught an exception: ") + error.what())));
    } catch (...) {
      job.handle->Complete(Result<AskOutcome>(
          Status::Error(ErrorCode::Internal, "a worker caught an unknown exception")));
    }
  }
}

void BrokerService::ResolvePending(const bool cancelled) {
  std::deque<PendingJob> pending;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    pending.swap(queue_);
  }
  for (PendingJob& job : pending) {
    if (cancelled) {
      job.handle->CancelPending();
    } else {
      job.handle->Complete(Result<AskOutcome>(
          Status::Error(ErrorCode::ShuttingDown, "the service stopped before this ask ran")));
    }
  }
}

// ---- queries -------------------------------------------------------------

Result<AccountingSummary> BrokerService::Summary() {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return core_.Summary();
}

Result<ConservationReport> BrokerService::VerifyConservation() {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return core_.VerifyConservation();
}

Result<std::string> BrokerService::CanonicalState() {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return CanonicalStateText(core_);
}

Result<Decision> BrokerService::FindDecision(const AskKey& key) {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return core_.FindDecisionByKey(key);
}

Result<SiteLedger> BrokerService::FindSite(const SiteId& site) {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  const SiteLedger* ledger = core_.FindSite(site);
  if (ledger == nullptr) {
    return Fail<SiteLedger>(ErrorCode::UnknownSite, "no offer has been published for this site");
  }
  return *ledger;
}

Result<std::vector<Decision>> BrokerService::RecentDecisions(const std::size_t limit) {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return core_.RecentDecisions(limit);
}

RecoveryReport BrokerService::Recovery() const {
  std::lock_guard<std::mutex> lock(txn_mutex_);
  return recovery_;
}

bool BrokerService::failed() const { return failed_.load(); }

std::size_t BrokerService::pending_submissions() const {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  return queue_.size();
}

void BrokerService::SetDecisionObserver(std::function<void(const Decision&)> observer) {
  std::lock_guard<std::mutex> lock(observer_mutex_);
  observer_ = std::move(observer);
}

void BrokerService::NotifyObserver(const Decision& decision) {
  std::function<void(const Decision&)> observer;
  {
    std::lock_guard<std::mutex> lock(observer_mutex_);
    observer = observer_;
  }
  if (!observer) {
    return;
  }
  // Called with no lock held: an observer that throws, blocks or re-enters the
  // broker can neither corrupt the ledger nor deadlock the service.
  try {
    observer(decision);
  } catch (...) {
  }
}

// ---- shutdown ------------------------------------------------------------

Status BrokerService::Shutdown(const ShutdownMode mode) {
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  stopping_.store(true);
  queue_ready_.notify_all();
  if (mode == ShutdownMode::Abandon) {
    // Abandoning: everything still queued is cancelled now, so no caller waits
    // for work that will never run.
    ResolvePending(true);
  }
  // Join without holding any lock the workers need in order to finish. Under
  // Drain the workers keep taking work from the queue until it is empty, so a
  // caller that was already accepted always gets an answer.
  for (std::thread& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  workers_.clear();
  // Safety net: anything enqueued in the window between the abandon sweep and
  // the workers stopping is answered here rather than left waiting forever.
  ResolvePending(mode == ShutdownMode::Abandon);

  std::lock_guard<std::mutex> lock(txn_mutex_);
  Status result = Status::Ok();
  if (journal_ != nullptr) {
    const Status closed = journal_->Close();
    journal_.reset();
    if (!closed.ok()) {
      result = closed;
    }
  }
  if (log_ != nullptr) {
    log_->Close();
    log_.reset();
  }
  snapshots_.reset();
  lock_.reset();
  return result;
}

BrokerService::~BrokerService() { (void)Shutdown(ShutdownMode::Drain); }

}  // namespace rcb

