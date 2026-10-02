// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/journal.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"
#include "rcb/json.hpp"
#include "rcb/serialize.hpp"

namespace rcb {
namespace {

constexpr u8 kMagic[4] = {'R', 'C', 'B', '1'};
constexpr u16 kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 64;
constexpr std::size_t kChainOffset = 32;
constexpr u8 kFlagPrepared = 0x01U;

constexpr std::string_view kChainSeed = "rcb-journal-chain-v1";

void StoreLe16(u8* out, const u16 value) {
  out[0] = static_cast<u8>(value & 0xFFU);
  out[1] = static_cast<u8>((value >> 8U) & 0xFFU);
}

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

u16 LoadLe16(const u8* data) {
  return static_cast<u16>(static_cast<u16>(data[0]) | (static_cast<u16>(data[1]) << 8U));
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

bool IsAllZero(const u8* data, const std::size_t length) {
  for (std::size_t i = 0; i < length; ++i) {
    if (data[i] != 0) {
      return false;
    }
  }
  return true;
}

struct Header {
  RecordKind kind = RecordKind::Epoch;
  u8 flags = 0;
  u64 epoch = 0;
  u64 sequence = 0;
  u32 payload_length = 0;
  u32 payload_crc = 0;
  std::array<u8, Digest::kSize> chain{};
};

bool HasValidMagic(const u8* header) {
  return header[0] == kMagic[0] && header[1] == kMagic[1] && header[2] == kMagic[2] &&
         header[3] == kMagic[3];
}

Header ParseHeader(const u8* header) {
  Header parsed;
  parsed.kind = static_cast<RecordKind>(header[6]);
  parsed.flags = header[7];
  parsed.epoch = LoadLe64(header + 8);
  parsed.sequence = LoadLe64(header + 16);
  parsed.payload_length = LoadLe32(header + 24);
  parsed.payload_crc = LoadLe32(header + 28);
  std::memcpy(parsed.chain.data(), header + kChainOffset, Digest::kSize);
  return parsed;
}

}  // namespace

std::string_view RecordKindToken(RecordKind kind) noexcept {
  switch (kind) {
    case RecordKind::Epoch: return "epoch";
    case RecordKind::Offer: return "offer";
    case RecordKind::Revocation: return "revocation";
    case RecordKind::Decision: return "decision";
    case RecordKind::Snapshot: return "snapshot";
    case RecordKind::Close: return "close";
    case RecordKind::CommitMarker: return "commit_marker";
  }
  return "unknown";
}

Result<std::unique_ptr<Journal>> Journal::Open(ByteLog& log, const JournalLimits& limits,
                                               const bool has_snapshot_sequence,
                                               const u64 snapshot_sequence) {
  auto journal = std::unique_ptr<Journal>(new Journal(log, limits));
  journal->chain_ = Sha256Of(kChainSeed);
  journal->size_ = log.size();
  journal->report_.snapshot_sequence_known = has_snapshot_sequence;
  journal->report_.snapshot_sequence = snapshot_sequence;

  Result<Bytes> bytes = log.ReadAll(limits.max_log_bytes);
  if (!bytes.ok()) {
    return Result<std::unique_ptr<Journal>>(bytes.status());
  }
  const Bytes& data = bytes.value();
  std::size_t offset = 0;
  bool have_pending = false;
  u64 pending_sequence = 0;
  std::size_t pending_index = 0;

  while (offset < data.size()) {
    const std::size_t remaining = data.size() - offset;
    if (remaining < kHeaderSize) {
      journal->report_.torn_tail = true;
      journal->report_.torn_tail_offset = offset;
      journal->report_.torn_tail_bytes = remaining;
      break;
    }
    const u8* header_bytes = data.data() + offset;
    if (!HasValidMagic(header_bytes) || LoadLe16(header_bytes + 4) != kFormatVersion) {
      const bool interior = !journal->records_.empty() || have_pending;
      const bool zeroed = IsAllZero(header_bytes, remaining);
      if (interior && !zeroed) {
        return Fail<std::unique_ptr<Journal>>(
            ErrorCode::PersistenceInteriorCorruption,
            "journal corruption at offset " + std::to_string(offset) +
                ": a record header is not valid and it is not the tail of the log");
      }
      journal->report_.torn_tail = true;
      journal->report_.torn_tail_offset = offset;
      journal->report_.torn_tail_bytes = remaining;
      break;
    }
    const Header header = ParseHeader(header_bytes);
    if (header.payload_length > limits.max_record_payload) {
      return Fail<std::unique_ptr<Journal>>(
          ErrorCode::PersistenceInteriorCorruption,
          "journal record at offset " + std::to_string(offset) +
              " declares a payload larger than the limit");
    }
    if (header.payload_length > remaining - kHeaderSize) {
      journal->report_.torn_tail = true;
      journal->report_.torn_tail_offset = offset;
      journal->report_.torn_tail_bytes = remaining;
      break;
    }
    const std::size_t record_size = kHeaderSize + header.payload_length;
    const bool is_last = offset + record_size == data.size();
    const std::span<const u8> payload(data.data() + offset + kHeaderSize, header.payload_length);
    if (Crc32c(payload) != header.payload_crc) {
      if (is_last) {
        journal->report_.torn_tail = true;
        journal->report_.torn_tail_offset = offset;
        journal->report_.torn_tail_bytes = remaining;
        break;
      }
      return Fail<std::unique_ptr<Journal>>(
          ErrorCode::PersistenceInteriorCorruption,
          "journal record at offset " + std::to_string(offset) +
              " fails its checksum and is not the final record");
    }
    Sha256 hasher;
    hasher.Update(std::span<const u8>(journal->chain_.bytes()));
    hasher.Update(std::span<const u8>(header_bytes, kChainOffset));
    hasher.Update(payload);
    const Digest expected = hasher.Finalize();
    if (expected != Digest::FromBytes(header.chain)) {
      if (is_last) {
        journal->report_.torn_tail = true;
        journal->report_.torn_tail_offset = offset;
        journal->report_.torn_tail_bytes = remaining;
        break;
      }
      return Fail<std::unique_ptr<Journal>>(
          ErrorCode::PersistenceInteriorCorruption,
          "journal record at offset " + std::to_string(offset) +
              " does not chain to its predecessor and is not the final record");
    }
    journal->chain_ = expected;
    if (header.kind != RecordKind::CommitMarker) {
      ++journal->report_.records_verified;
    }
    journal->last_sequence_ = header.sequence;
    journal->epoch_ = header.epoch;
    journal->report_.last_sequence = header.sequence;
    journal->report_.last_epoch = header.epoch;

    JournalRecord record;
    record.kind = header.kind;
    record.epoch = header.epoch;
    record.sequence = header.sequence;
    record.payload.assign(reinterpret_cast<const char*>(payload.data()), payload.size());
    record.offset = offset;
    record.skipped = has_snapshot_sequence && header.sequence <= snapshot_sequence &&
                     header.kind != RecordKind::Epoch && header.kind != RecordKind::Snapshot &&
                     header.kind != RecordKind::Close;

    if (header.kind == RecordKind::CommitMarker) {
      if (!have_pending || pending_sequence != header.sequence) {
        return Fail<std::unique_ptr<Journal>>(
            ErrorCode::PersistenceInteriorCorruption,
            "a commit marker at offset " + std::to_string(offset) +
                " does not match the prepared record before it");
      }
      have_pending = false;
      journal->records_[pending_index].skipped = record.skipped;
    } else if ((header.flags & kFlagPrepared) != 0) {
      if (have_pending) {
        // The previous prepared record was never marked. It is inert data, not
        // corruption: nothing was published from it, so replay drops it and the
        // count is reported.
        journal->records_.erase(journal->records_.begin() +
                                static_cast<std::ptrdiff_t>(pending_index));
        ++journal->report_.prepared_discarded;
      }
      have_pending = true;
      pending_sequence = header.sequence;
      journal->records_.push_back(record);
      pending_index = journal->records_.size() - 1;
    } else {
      journal->records_.push_back(record);
    }

    if (journal->records_.size() > limits.max_records) {
      return Fail<std::unique_ptr<Journal>>(ErrorCode::LimitExceeded,
                                            "the journal holds more records than the limit");
    }
    offset += record_size;
  }

  if (have_pending) {
    // The log ends with a prepared record that was never marked committed: the
    // change it describes was never published, so it is discarded.
    journal->records_.erase(journal->records_.begin() + static_cast<std::ptrdiff_t>(pending_index));
    ++journal->report_.prepared_discarded;
  }

  for (const JournalRecord& record : journal->records_) {
    if (record.kind == RecordKind::Close && record.sequence != 0) {
      journal->report_.clean_close = true;
      break;
    }
  }

  if (journal->report_.torn_tail) {
    if (!limits.repair_torn_tail) {
      return Fail<std::unique_ptr<Journal>>(
          ErrorCode::PersistenceTornTail,
          "the journal ends in a torn record at offset " +
              std::to_string(journal->report_.torn_tail_offset) + " and repair is disabled");
    }
    const Status truncated = log.Truncate(journal->report_.torn_tail_offset);
    if (!truncated.ok()) {
      return Result<std::unique_ptr<Journal>>(truncated);
    }
    journal->report_.repaired = true;
    journal->size_ = log.size();
  }

  journal->report_.chain = journal->chain_;
  for (const JournalRecord& record : journal->records_) {
    if (record.skipped) {
      ++journal->report_.records_skipped;
    }
  }
  return journal;
}

Digest Journal::ExtendChain(const u8* header_without_chain, const std::string_view payload) const {
  Sha256 hasher;
  hasher.Update(std::span<const u8>(chain_.bytes()));
  hasher.Update(std::span<const u8>(header_without_chain, kChainOffset));
  hasher.Update(payload);
  return hasher.Finalize();
}

Status Journal::AppendRecord(const RecordKind kind, const u64 sequence, const std::string_view payload,
                             const u8 flags) {
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the journal is closed");
  }
  if (payload.size() > limits_.max_record_payload) {
    return Status::Error(ErrorCode::LimitExceeded, "the record payload is larger than the limit");
  }
  std::array<u8, kHeaderSize> header{};
  std::memcpy(header.data(), kMagic, sizeof(kMagic));
  StoreLe16(header.data() + 4, kFormatVersion);
  header[6] = static_cast<u8>(kind);
  header[7] = flags;
  StoreLe64(header.data() + 8, epoch_);
  StoreLe64(header.data() + 16, sequence);
  StoreLe32(header.data() + 24, static_cast<u32>(payload.size()));
  StoreLe32(header.data() + 28,
            Crc32c(std::span<const u8>(reinterpret_cast<const u8*>(payload.data()), payload.size())));

  const Digest next = ExtendChain(header.data(), payload);
  std::memcpy(header.data() + kChainOffset, next.bytes().data(), Digest::kSize);

  Bytes frame;
  frame.reserve(kHeaderSize + payload.size());
  frame.insert(frame.end(), header.begin(), header.end());
  frame.insert(frame.end(), reinterpret_cast<const u8*>(payload.data()),
               reinterpret_cast<const u8*>(payload.data()) + payload.size());

  const u64 offset = size_;
  const Status appended = log_->Append(frame);
  if (!appended.ok()) {
    return appended;
  }
  size_ = log_->size();

  // Verification step: read back what was appended and compare it byte for byte
  // with what was intended. A short write, a torn write or a device that lies
  // about extent is caught here, before anything is published.
  Result<Bytes> read_back = log_->ReadRange(offset, frame.size());
  if (!read_back.ok()) {
    return read_back.status();
  }
  if (read_back.value().size() != frame.size() ||
      !std::equal(frame.begin(), frame.end(), read_back.value().begin())) {
    return Status::Error(ErrorCode::PersistenceIoError,
                         "the bytes read back from the journal do not match what was written");
  }

  chain_ = next;
  last_sequence_ = sequence;
  return Status::Ok();
}

Status Journal::Prepare(const RecordKind kind, const u64 sequence, std::string payload) {
  if (pending_) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "a prepared record is already waiting to be marked");
  }
  if (kind == RecordKind::CommitMarker) {
    return Status::Error(ErrorCode::InvalidArgument, "a commit marker cannot be prepared");
  }
  const Status appended = AppendRecord(kind, sequence, payload, kFlagPrepared);
  if (!appended.ok()) {
    return appended;
  }
  JournalRecord record;
  record.kind = kind;
  record.epoch = epoch_;
  record.sequence = sequence;
  record.payload = std::move(payload);
  record.offset = size_ - (kHeaderSize + record.payload.size());
  records_.push_back(std::move(record));
  pending_ = true;
  pending_sequence_ = sequence;
  report_.last_sequence = sequence;
  return Status::Ok();
}

Status Journal::MarkCommitted(const DurabilityClass durability) {
  if (!pending_) {
    return Status::Error(ErrorCode::InvariantViolation, "no prepared record to mark committed");
  }
  const Status appended = AppendRecord(RecordKind::CommitMarker, pending_sequence_, {}, 0);
  if (!appended.ok()) {
    return appended;
  }
  JournalRecord marker;
  marker.kind = RecordKind::CommitMarker;
  marker.epoch = epoch_;
  marker.sequence = pending_sequence_;
  marker.offset = size_ - kHeaderSize;
  records_.push_back(std::move(marker));
  pending_ = false;
  if (durability == DurabilityClass::Durable) {
    return log_->Sync();
  }
  return Status::Ok();
}

Status Journal::AbandonPrepared() {
  if (!pending_) {
    return Status::Error(ErrorCode::InvariantViolation, "no prepared record to retire");
  }
  pending_ = false;
  const auto last = std::find_if(records_.rbegin(), records_.rend(),
                                 [](const JournalRecord& record) {
                                   return record.kind != RecordKind::CommitMarker;
                                 });
  if (last != records_.rend()) {
    records_.erase(std::next(last).base());
  }
  return Status::Ok();
}

Status Journal::Flush() { return log_->Sync(); }

Status Journal::NoteSnapshot(const u64 sequence, const Digest& state_digest, const u64 bytes) {
  if (pending_) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "a snapshot marker cannot be written while a prepared record waits");
  }
  const Status appended =
      AppendRecord(RecordKind::Snapshot, sequence,
                   EncodeSnapshotRecord(sequence, state_digest, bytes), 0);
  if (!appended.ok()) {
    return appended;
  }
  JournalRecord record;
  record.kind = RecordKind::Snapshot;
  record.epoch = epoch_;
  record.sequence = sequence;
  record.payload = EncodeSnapshotRecord(sequence, state_digest, bytes);
  record.offset = size_ - (kHeaderSize + record.payload.size());
  records_.push_back(std::move(record));
  return log_->Sync();
}

Status Journal::Close() {
  if (closed_) {
    return Status::Ok();
  }
  if (pending_) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "the journal cannot close while a prepared record waits to be marked");
  }
  const Status appended = AppendRecord(RecordKind::Close, last_sequence_, EncodeCloseRecord(last_sequence_), 0);
  if (!appended.ok()) {
    return appended;
  }
  const Status synced = log_->Sync();
  if (!synced.ok()) {
    return synced;
  }
  closed_ = true;
  return log_->Close();
}

Status Journal::Reset(const u64 epoch) {
  if (pending_) {
    return Status::Error(ErrorCode::InvariantViolation,
                         "the journal cannot be reset while a prepared record waits");
  }
  if (closed_) {
    return Status::Error(ErrorCode::PersistenceIoError, "the journal is closed");
  }
  const Status truncated = log_->Truncate(0);
  if (!truncated.ok()) {
    return truncated;
  }
  size_ = 0;
  epoch_ = epoch;
  records_.clear();
  chain_ = Sha256Of(kChainSeed);
  const Status appended = AppendRecord(RecordKind::Epoch, 0, EncodeEpochRecord(epoch), 0);
  if (!appended.ok()) {
    return appended;
  }
  const Status synced = log_->Sync();
  if (!synced.ok()) {
    return synced;
  }
  JournalRecord record;
  record.kind = RecordKind::Epoch;
  record.epoch = epoch;
  record.sequence = 0;
  record.payload = EncodeEpochRecord(epoch);
  record.offset = 0;
  records_.push_back(std::move(record));
  return Status::Ok();
}

Status Journal::ApplyTo(BrokerCore& core) const {
  for (const JournalRecord& record : records_) {
    if (record.skipped) {
      continue;
    }
    const Status applied = ApplyRecord(record, core);
    if (!applied.ok()) {
      return Status::Error(applied.code(),
                           std::string(RecordKindToken(record.kind)) + " record at offset " +
                               std::to_string(record.offset) + ": " + applied.detail());
    }
  }
  return Status::Ok();
}

// ---- payload encoding ----------------------------------------------------

std::string EncodeOfferRecord(const Offer& offer) { return ToJson(offer).ToText(false); }

std::string EncodeRevocationRecord(const SiteId& site, const u64 generation, const i64 at,
                                   const RevocationReason reason) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("site", JsonValue::MakeString(site.value()));
  object.Set("generation", JsonValue::MakeUint(generation));
  object.Set("at", JsonValue::MakeInt(at));
  object.Set("reason", JsonValue::MakeString(std::string(RevocationReasonToken(reason))));
  return object.ToText(false);
}

std::string EncodeDecisionRecord(const Decision& decision) { return ToJson(decision).ToText(false); }

std::string EncodeEpochRecord(const u64 epoch) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("epoch", JsonValue::MakeUint(epoch));
  return object.ToText(false);
}

std::string EncodeSnapshotRecord(const u64 sequence, const Digest& digest, const u64 bytes) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("sequence", JsonValue::MakeUint(sequence));
  object.Set("state_digest", JsonValue::MakeString(digest.Hex()));
  object.Set("bytes", JsonValue::MakeUint(bytes));
  return object.ToText(false);
}

std::string EncodeCloseRecord(const u64 sequence) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("sequence", JsonValue::MakeUint(sequence));
  object.Set("clean", JsonValue::MakeBool(true));
  return object.ToText(false);
}

// ---- replay --------------------------------------------------------------

Status ApplyRecord(const JournalRecord& record, BrokerCore& core) {
  switch (record.kind) {
    case RecordKind::Epoch:
    case RecordKind::Snapshot:
    case RecordKind::Close:
    case RecordKind::CommitMarker:
      return Status::Ok();
    case RecordKind::Offer: {
      Result<JsonValue> document = ParseJson(record.payload);
      if (!document.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "offer record payload is not valid JSON: " +
                                 document.status().ToString());
      }
      Result<Offer> offer = DecodeOfferJson(document.value());
      if (!offer.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "offer record payload: " + offer.status().ToString());
      }
      Result<OfferPublication> published = core.ReplayOffer(offer.value(), record.sequence);
      if (!published.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "replaying an offer publication failed: " +
                                 published.status().ToString());
      }
      return Status::Ok();
    }
    case RecordKind::Revocation: {
      Result<JsonValue> document = ParseJson(record.payload);
      if (!document.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "revocation record payload is not valid JSON");
      }
      const JsonValue& object = document.value();
      if (!object.IsObject()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "revocation record payload must be an object");
      }
      const JsonValue* site = object.Find("site");
      const JsonValue* generation = object.Find("generation");
      const JsonValue* at = object.Find("at");
      const JsonValue* reason = object.Find("reason");
      if (site == nullptr || generation == nullptr || at == nullptr || reason == nullptr ||
          !site->IsString() || !generation->IsNumber() || !at->IsInt() || !reason->IsString()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "revocation record payload is missing a field");
      }
      Result<SiteId> site_id = SiteId::Make(site->AsString());
      if (!site_id.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt, "revocation record names no site");
      }
      RevocationReason parsed_reason = RevocationReason::OfferRevoked;
      if (reason->AsString() == "superseded_by_generation") {
        parsed_reason = RevocationReason::SupersededByGeneration;
      } else if (reason->AsString() == "capacity_shrink") {
        parsed_reason = RevocationReason::CapacityShrink;
      } else if (reason->AsString() != "offer_revoked") {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "revocation record carries an unknown reason token");
      }
      const u64 record_generation =
          generation->IsUint() ? generation->AsUint() : static_cast<u64>(generation->AsInt());
      Result<OfferRevocation> revoked =
          core.ReplayRevocation(site_id.value(), record_generation, record.sequence, at->AsInt(),
                                parsed_reason);
      if (!revoked.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "replaying an offer revocation failed: " +
                                 revoked.status().ToString());
      }
      return Status::Ok();
    }
    case RecordKind::Decision: {
      Result<JsonValue> document = ParseJson(record.payload);
      if (!document.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "decision record payload is not valid JSON");
      }
      Result<Decision> decision = DecodeDecisionJson(document.value());
      if (!decision.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt,
                             "decision record payload: " + decision.status().ToString());
      }
      return core.ReplayDecision(decision.value());
    }
  }
  return Status::Error(ErrorCode::PersistenceCorrupt, "unknown journal record kind");
}

}  // namespace rcb
