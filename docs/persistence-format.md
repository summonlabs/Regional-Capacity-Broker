# Persistence format

This document is the byte-level contract of the durable store. It exists so that
an operator, a forensic reader or a future version of this runtime can interpret
a store without reading the implementation, and so that a change to any of it is
recognised as a format change with a version decision attached.

Nothing here is public API. The public contract is
`include/rcb/journal.hpp`, `include/rcb/store.hpp` and the behaviour described
in the README; this file describes the bytes those interfaces produce.

## Store directory

```
<store>/
  store.lock        exclusive writer lock; empty file, held for the session
  journal.log       append-only write-ahead journal
  state.snapshot    the most recently published snapshot (absent until the first compaction)
```

The runtime creates no other file, writes no temporary file outside this
directory, writes nothing to the user profile, makes no network connection and
emits no telemetry. A snapshot is published through `<store>/state.snapshot.tmp`,
which exists only for the duration of a publish and is removed on failure.

Writer fencing: `store.lock` is opened with an exclusive share mode on Windows
and `flock(LOCK_EX|LOCK_NB)` elsewhere. A second process either fails to open
the store (`persistence_io_error`) or waits for the first to close; it never
writes concurrently. The lock is released when the process exits, including on a
crash.

## Journal records

Every record is a fixed 64-byte header followed by its payload. All integers are
little-endian.

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `R` `C` `B` `1` |
| 4 | 2 | format version (currently `1`) |
| 6 | 1 | record kind (see below) |
| 7 | 1 | flags: bit 0 = prepared, all other bits reserved and must be zero |
| 8 | 8 | broker epoch that wrote the record |
| 16 | 8 | sequence |
| 24 | 4 | payload length in bytes |
| 28 | 4 | CRC-32C (Castagnoli) of the payload |
| 32 | 32 | chain digest (see below) |
| 64 | n | payload: UTF-8 JSON for data records, empty for markers |

Record kinds: `1` epoch, `2` offer, `3` revocation, `4` decision,
`5` snapshot marker, `6` close, `7` commit marker.

### Chain rule

```
chain[0] = SHA-256("rcb-journal-chain-v1")
chain[i] = SHA-256(chain[i-1] || header[i][0..32) || payload[i])
```

The chain covers the header words before the digest field and the whole payload,
so a rewritten field, a reordered record or a spliced-in record from another
store is detectable even when the payload's own CRC has been recomputed. It is
verified while reading, record by record, before anything is adopted.

### Payloads

| Kind | Payload |
|---|---|
| epoch | `{"epoch":N}` |
| offer | the canonical offer document (`rcb::ToJson(Offer)`) |
| revocation | `{"at":T,"generation":G,"reason":"...","site":"S"}` |
| decision | the canonical decision document, including its allocations and its accounting digest |
| snapshot marker | `{"bytes":B,"sequence":S,"state_digest":"..."}` |
| close | `{"clean":true,"sequence":S}` |
| commit marker | empty |

## Two-phase completion

A change becomes authoritative in two steps:

1. **Prepared record.** The record is appended with flags bit 0 set and is read
   back from the log and compared byte for byte with what was intended. A short
   write, a torn write or a device that misreports extent is caught here, before
   anything is published. A prepared record is *not* durable and carries no
   authority; it exists so that the exact change is known before it is applied.
2. **Commit marker.** After the change has been applied in memory, a
   `commit_marker` record with the same sequence is appended. For
   `DurabilityClass::Durable` the platform durability call
   (`FlushFileBuffers` on Windows, `fsync` elsewhere) is issued before the
   caller is told anything.

Recovery marks a prepared record committed only when it is followed by a commit
marker with the same sequence. Anything else — a prepared record at the end of
the log, a prepared record superseded by another prepared record, or a prepared
record whose marker was lost to a torn tail — is discarded, counted in
`RecoveryReport::prepared_discarded`, and has no effect on the ledger.

Consequences, each one covered by a test:

* a crash between the two records leaves **no** effect;
* a crash after the marker leaves the **whole** effect;
* a caller is never told a change succeeded before it reached the promised
  durability boundary;
* an ask cancelled before its marker was written can never appear after a
  restart, and a cancellation observed after the marker is reported as
  `cancelled_after_commit` with the committed decision.

If the commit marker cannot be written or flushed, the session is poisoned:
further mutations are refused with `persistence_io_error` until the store is
reopened, because continuing on a store whose commit state is unknown would be
worse than stopping.

## Recovery

Recovery reads the snapshot (if any) and then the whole log, in this order:

1. Parse the header. A magic or version mismatch, or a payload length above
   `JournalLimits::max_record_payload`, ends the scan.
2. If the remaining bytes cannot hold the declared payload, the record is
   **torn**.
3. Verify the payload CRC-32C, then the chain digest.
4. Pair prepared records with their commit markers.

Then the scan decides between the two failure classes:

* **Torn tail.** The damage is confined to the final record of the log (an
  incomplete header, an incomplete payload, a failed checksum on the last
  record, or a failed chain on the last record), or the log holds no valid
  record at all, or the remaining bytes are all zero, or the remaining bytes are
  larger than one maximum-size record could be. The log is truncated to the last
  complete record boundary and the truncation is reported: `torn_tail`,
  `torn_tail_offset`, `torn_tail_bytes`, `repaired`. Repair is refused
  outright (with `persistence_torn_tail`) when `JournalLimits::repair_torn_tail`
  is false.
* **Interior corruption.** Anything else: a checksum or chain failure in a
  record that is followed by more bytes, an invalid header after at least one
  valid record, or an impossible declared length. The store refuses to open with
  `persistence_interior_corruption` and is left **byte for byte unchanged**.
  Recovery never repairs through corruption, never truncates to hide it, and
  never pretends it succeeded.

After the framing is verified, the records are applied to a fresh kernel in
order: offers and revocations rebuild the ledger, decisions re-apply their
allocations (and are refused if they do not fit the ledger they claim), and the
accounting chain digest is recomputed and compared with the digest each decision
carries. A mismatch is corruption.

## Snapshot frame

`state.snapshot` is a self-describing frame around the canonical state text:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `RCBSNAP1` |
| 8 | 4 | format version (currently `1`) |
| 12 | 8 | journal sequence the body covers |
| 20 | 8 | body length in bytes |
| 28 | 32 | SHA-256 of the body |
| 60 | n | body: `rcb::CanonicalStateText` |
| 60+n | 4 | CRC-32C of everything before it |

The body is canonical JSON: object keys in byte order, collections in a defined
order, integers as integers, fixed-point amounts as exact decimal strings, and
all identifiers present. Two runs over the same state produce the same bytes,
which is what makes the digest meaningful.

A snapshot is adopted only if the frame's CRC, its body length and the body
digest all check out, and if the restored ledger satisfies its own conservation
identity and every retained decision's allocations are present in the ledger.
Any failure refuses the store; a snapshot is never partially adopted.

## Compaction

Compaction is the only operation that removes records, and it is ordered so that
it can never lose state:

1. flush the log (everything appended so far becomes durable);
2. encode the canonical state and publish it through a temporary file, reading
   it back and verifying it, then renaming atomically, then making the rename
   durable (`MOVEFILE_WRITE_THROUGH` on Windows, a directory `fsync` elsewhere);
3. append a snapshot marker naming the sequence and the state digest;
4. only then truncate the log and write a fresh epoch record.

Interrupted at any point, at least one complete copy survives: before step 2 the
log is intact; between 2 and 4 the snapshot is published and the log still holds
everything; after 4 the snapshot is authoritative and the log is fresh. A
snapshot can never cover less than the log it replaces, because the boundary is
the kernel's own sequence at the moment the log was flushed and the state was
encoded, and compaction refuses to run while a prepared record is waiting.

## Durability and the flush boundary

| Class | What it means |
|---|---|
| `durable` | the commit marker reached `FlushFileBuffers`/`fsync` before the caller was answered |
| `buffered` | the record was written and reads back correctly, but no per-operation flush was issued; a flush happens at compaction and at close |
| `volatile` | there is no store at all (in-memory service) |

The class a decision actually reached is recorded on the decision itself. A
`buffered` decision is explicitly not claimed to be durable.

## Versioning

`rcb::kPersistenceFormatVersion` (currently `1`) covers the record frame, the
snapshot frame and the state document. A reader refuses any other version with
`unsupported_version`; it never guesses. Adding a field to the state document,
changing a field's meaning, changing a bound that a stored value must satisfy, or
changing the chain rule all require a version decision before they are made.
The request documents the CLI accepts carry their own `schema` field
(`rcb::kRequestSchemaVersion`) and are refused the same way.
