// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The write-ahead journal: framing, verification, two-phase completion and
// recovery.
//
// Every authoritative change is written twice:
//
//   1. a prepared record, which carries the change and is explicitly marked as
//      not yet committed. A prepared record that is never marked is inert: the
//      ledger it describes is not adopted, and recovery discards it;
//   2. a commit marker with the same sequence, written only after the change was
//      applied in memory and immediately before the result is published to the
//      caller. The marker is what makes the change durable, and for
//      DurabilityClass::Durable the marker is fsynced before the caller is told
//      anything.
//
// Consequences that are relied on and tested: a crash between the two records
// leaves no effect; a crash after the marker leaves the whole effect; a caller
// is never told a change succeeded before the marker reached the platform's
// durability call; and a change that was cancelled before its marker was
// written can never appear after a restart.
//
// Framing: a fixed 64-byte header (magic, version, kind, flags, epoch,
// sequence, payload length, payload CRC-32C, chained SHA-256) followed by the
// payload. The chain binds every record to its whole history, so a rewrite, a
// reorder or a splice is detectable. A torn tail (a partially written final
// record) is truncated and reported; corruption that is not at the tail is
// reported and refused, never repaired.

#ifndef RCB_JOURNAL_HPP
#define RCB_JOURNAL_HPP

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rcb/broker.hpp"
#include "rcb/digest.hpp"
#include "rcb/status.hpp"
#include "rcb/store.hpp"
#include "rcb/types.hpp"

namespace rcb {

enum class RecordKind : u8 {
  Epoch = 1,
  Offer = 2,
  Revocation = 3,
  Decision = 4,
  Snapshot = 5,
  Close = 6,
  CommitMarker = 7,
};

std::string_view RecordKindToken(RecordKind kind) noexcept;

struct JournalRecord {
  RecordKind kind = RecordKind::Epoch;
  u64 epoch = 0;
  u64 sequence = 0;
  std::string payload;
  u64 offset = 0;
  /// True when the snapshot already covers this record, so replay skips it.
  bool skipped = false;
};

struct JournalLimits {
  /// Payload bound, checked before anything is allocated.
  std::size_t max_record_payload = Limits::kMaxRecordPayloadBytes;
  /// Whole-log bound for recovery.
  std::size_t max_log_bytes = 512U * 1024U * 1024U;
  /// Records retained in a replayed log.
  std::size_t max_records = 1U << 22;
  /// Truncate a torn tail (a defect only when it hides committed data, which
  /// the commit markers make detectable).
  bool repair_torn_tail = true;
};

struct RecoveryReport {
  bool snapshot_sequence_known = false;
  u64 snapshot_sequence = 0;
  bool clean_close = false;
  std::size_t records_verified = 0;
  std::size_t records_applied = 0;
  std::size_t records_skipped = 0;
  std::size_t prepared_discarded = 0;
  bool torn_tail = false;
  u64 torn_tail_offset = 0;
  u64 torn_tail_bytes = 0;
  bool repaired = false;
  u64 last_sequence = 0;
  u64 last_epoch = 0;
  Digest chain;
};

/// The two-phase journal over one byte log.
class Journal {
 public:
  /// Reads and verifies the log. \p snapshot_sequence, when the store has a
  /// snapshot, marks every record it already covers as skipped.
  static Result<std::unique_ptr<Journal>> Open(ByteLog& log, const JournalLimits& limits,
                                               bool has_snapshot_sequence,
                                               u64 snapshot_sequence);

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return report_; }
  [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
  [[nodiscard]] const Digest& chain() const noexcept { return chain_; }
  [[nodiscard]] u64 epoch() const noexcept { return epoch_; }
  [[nodiscard]] u64 last_sequence() const noexcept { return last_sequence_; }
  [[nodiscard]] u64 size() const noexcept { return size_; }
  void SetEpoch(u64 epoch) noexcept { epoch_ = epoch; }
  /// True when a prepared record is waiting to be marked.
  [[nodiscard]] bool has_pending() const noexcept { return pending_; }

  /// Appends a prepared record and reads it back to verify it. Not durable.
  Status Prepare(RecordKind kind, u64 sequence, std::string payload);
  /// Marks the pending record committed. For DurabilityClass::Durable the
  /// marker is synced before this returns.
  Status MarkCommitted(DurabilityClass durability);
  /// Retires a prepared record without committing it. The bytes stay in the
  /// log, but the record carries no marker, so replay ignores it: the change it
  /// describes never happened.
  Status AbandonPrepared();
  /// Makes everything appended so far durable.
  Status Flush();
  /// Writes a close marker, flushes, and closes the log.
  Status Close();
  /// Appends a snapshot marker describing a published snapshot.
  Status NoteSnapshot(u64 sequence, const Digest& state_digest, u64 bytes);
  /// Discards the whole log and starts a fresh one with an epoch record. Only
  /// called once a snapshot covering everything in the log is durable.
  Status Reset(u64 epoch);

  /// Applies the replayed records to a kernel.
  Status ApplyTo(BrokerCore& core) const;

  [[nodiscard]] ByteLog& log() noexcept { return *log_; }

 private:
  Journal(ByteLog& log, const JournalLimits& limits) : log_(&log), limits_(limits) {}

  Status AppendRecord(RecordKind kind, u64 sequence, std::string_view payload, u8 flags);
  Digest ExtendChain(const u8* header_without_chain, std::string_view payload) const;

  ByteLog* log_ = nullptr;
  JournalLimits limits_;
  std::vector<JournalRecord> records_;
  RecoveryReport report_;
  Digest chain_;
  u64 epoch_ = 0;
  u64 last_sequence_ = 0;
  u64 size_ = 0;
  bool pending_ = false;
  u64 pending_sequence_ = 0;
  bool closed_ = false;
};

/// Applies one verified record. Exposed so that a recovery driver can report
/// per-record progress.
Status ApplyRecord(const JournalRecord& record, BrokerCore& core);

/// Renders the payload of an offer, revocation or decision record.
std::string EncodeOfferRecord(const Offer& offer);
std::string EncodeRevocationRecord(const SiteId& site, u64 generation, i64 at,
                                   RevocationReason reason);
std::string EncodeDecisionRecord(const Decision& decision);
std::string EncodeEpochRecord(u64 epoch);
std::string EncodeSnapshotRecord(u64 sequence, const Digest& digest, u64 bytes);
std::string EncodeCloseRecord(u64 sequence);

}  // namespace rcb

#endif  // RCB_JOURNAL_HPP
