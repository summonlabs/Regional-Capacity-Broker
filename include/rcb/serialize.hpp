// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Canonical encodings.
//
// Two encodings share one implementation:
//
//   * the canonical text of an offer, an ask, a decision or the whole ledger,
//     which is what digests, the accounting chain and snapshots are computed
//     over. It is deterministic: object keys are ordered, collections are in a
//     defined order, and no field depends on the host, the clock or the run;
//   * the JSON projection of the same structures for operators and for the CLI.
//
// Decoding is strict. An unknown field, a missing field, a duplicate key, a
// floating-point number, an out-of-range quantity or a malformed identifier is
// refused with the exact token that failed, never defaulted.

#ifndef RCB_SERIALIZE_HPP
#define RCB_SERIALIZE_HPP

#include <string>
#include <string_view>

#include "rcb/broker.hpp"
#include "rcb/json.hpp"
#include "rcb/ledger.hpp"
#include "rcb/model.hpp"
#include "rcb/status.hpp"

namespace rcb {

// ---- canonical text ----

/// Canonical text of an offer. Used for idempotent re-publication comparison and
/// for the offer part of the accounting chain.
std::string CanonicalOfferText(const Offer& offer);
/// Canonical text of an ask. This is the request digest input.
std::string CanonicalAskText(const Ask& ask);
/// Canonical text of a decision. Excludes the two derived fields
/// (accounting_digest and replay) so that the chain can be computed over it.
std::string CanonicalDecisionText(const Decision& decision);

Digest AskDigest(const Ask& ask);
Digest OfferDigest(const Offer& offer);

/// Canonical text of the entire authoritative state, as persisted.
std::string CanonicalStateText(const BrokerCore& core);
Digest StateDigestOf(const BrokerCore& core);

// ---- JSON projections ----

JsonValue ToJson(const CapacityVector& value);
JsonValue ToJson(const ScaledAmount& value);
JsonValue ToJson(const CostEvidence& value);
JsonValue ToJson(const CapacityTranche& value);
JsonValue ToJson(const Offer& value);
JsonValue ToJson(const GenerationPin& value);
JsonValue ToJson(const Ask& value);
JsonValue ToJson(const BlockingConstraint& value);
JsonValue ToJson(const Allocation& value);
JsonValue ToJson(const Decision& value);
JsonValue ToJson(const CommitmentRecord& value);
JsonValue ToJson(const TrancheLedger& value);
JsonValue ToJson(const SiteLedger& value);
JsonValue ToJson(const GenerationHistoryEntry& value);
JsonValue ToJson(const AccountingSummary& value);
JsonValue ToJson(const ConservationReport& value);
JsonValue ToJson(const OfferPublication& value);
JsonValue ToJson(const OfferRevocation& value);

/// Compact canonical text (no insignificant whitespace).
std::string EncodeJson(const JsonValue& value, bool pretty = false);

// ---- decoding ----

/// Decodes an offer document. Rejects unknown fields.
Result<Offer> DecodeOfferJson(const JsonValue& value);
/// Decodes an ask document. Rejects unknown fields. \p default_fairness is used
/// when the document does not choose one.
Result<Ask> DecodeAskJson(const JsonValue& value,
                          FairnessPolicy default_fairness = FairnessPolicy::StableSiteOrder);

/// Rebinds a decoded offer onto a ledger entry (used by snapshots).
Result<SiteLedger> DecodeSiteLedgerJson(const JsonValue& value);
Result<CommitmentRecord> DecodeCommitmentJson(const JsonValue& value);
Result<Decision> DecodeDecisionJson(const JsonValue& value);

/// Rebuilds a core from its canonical state text. Fails with
/// ErrorCode::PersistenceCorrupt when the state does not satisfy its own
/// conservation identity, and with ErrorCode::UnsupportedVersion when the
/// format version is not the one this build writes.
Status RestoreStateFromJson(const JsonValue& root, BrokerCore& core);

}  // namespace rcb

#endif  // RCB_SERIALIZE_HPP
