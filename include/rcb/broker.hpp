// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// The brokerage kernel.
//
// BrokerCore is a pure, single-threaded, deterministic state machine: no
// threads, no clocks, no I/O, no floating point, no randomness. Everything it
// decides is a function of the offers published to it, the asks submitted to
// it, and its configuration. Persistence and concurrency are layered outside
// it (see journal.hpp and service.hpp), which is what makes replay, restart and
// property testing tractable.
//
// The kernel owns regional capacity offers, asks, brokerage decisions,
// brokerage-layer allocation commitments and exact accounting. It does not own
// each site's capacity truth, cross-site placement topology, reservation
// execution, local admission policy or workload scheduling, and it never
// infers them.

#ifndef RCB_BROKER_HPP
#define RCB_BROKER_HPP

#include <cstddef>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "rcb/digest.hpp"
#include "rcb/ledger.hpp"
#include "rcb/model.hpp"
#include "rcb/status.hpp"
#include "rcb/types.hpp"

namespace rcb {

struct BrokerConfig {
  /// What a new generation does when it publishes less than is committed.
  ShrinkPolicy shrink_policy = ShrinkPolicy::EvictToFit;
  /// Fairness used when an ask does not name one.
  FairnessPolicy default_fairness = FairnessPolicy::StableSiteOrder;
  /// Bounded retention of idempotency records. A record that still has a live
  /// commitment is never dropped.
  std::size_t max_retained_decisions = Limits::kMaxRetainedDecisions;
  /// Bounded number of blocking constraints attached to one decision.
  std::size_t max_blocking_constraints = 32;
  /// Superseded-generation audit entries retained per site.
  std::size_t generation_history = 8;
};

/// Audit entry for a superseded or revoked generation.
struct GenerationHistoryEntry {
  u64 generation = 0;
  u64 sequence = 0;
  i64 at = 0;
  RevocationReason reason = RevocationReason::SupersededByGeneration;
  std::size_t commitments_revoked = 0;
};

/// The result of publishing an offer generation.
struct OfferPublication {
  SiteId site;
  u64 generation = 0;
  u64 sequence = 0;
  bool idempotent_replay = false;
  std::size_t carried_commitments = 0;
  std::size_t commitments_revoked = 0;
  /// The commitments evicted to make the new generation fit, in eviction order.
  std::vector<CommitmentRecord> revoked;
  CapacityVector allocatable_total;
  CapacityVector reserve_total;
};

/// The result of withdrawing an offer generation.
struct OfferRevocation {
  SiteId site;
  u64 generation = 0;
  u64 sequence = 0;
  i64 at = 0;
  std::size_t commitments_revoked = 0;
  std::vector<CommitmentRecord> revoked;
  CapacityVector withheld;
};

/// One commitment the broker proposes to make, before it is committed.
struct PlannedAllocation {
  SiteId site;
  u64 generation = 0;
  FailureDomainId domain;
  CapacityVector allocatable_amount;
  CapacityVector reserve_amount;
  ScaledAmount cost;
  i64 energy_millijoules = 0;
  i64 carbon_milligrams = 0;
  PriorityClass priority = PriorityClass::Normal;
};

enum class PlanKind : u8 { Commit = 0, Replay = 1 };

/// A decision the kernel has worked out but not applied. Plans are pure: they
/// carry no authority until CommitPlan re-checks the epoch and the ledger.
struct AskPlan {
  PlanKind kind = PlanKind::Commit;
  Ask ask;
  Digest request_digest;
  std::vector<PlannedAllocation> allocations;
  std::vector<BlockingConstraint> blocking;
  CapacityVector committed;
  CapacityVector unmet;
  ScaledAmount total_cost;
  i64 total_energy_millijoules = 0;
  i64 total_carbon_milligrams = 0;
  DecisionOutcome outcome = DecisionOutcome::Refused;
  u64 broker_epoch = 0;
  /// Ledger version the plan was computed against. CommitPlan refuses a plan
  /// whose version is no longer current, which is how a stale asynchronous
  /// completion is stopped from mutating newer state.
  u64 state_version = 0;
  DecisionId replay_decision_id;

  [[nodiscard]] bool IsReplay() const { return kind == PlanKind::Replay; }
};

/// The deterministic brokerage kernel.
class BrokerCore {
 public:
  explicit BrokerCore(BrokerConfig config = BrokerConfig(), u64 epoch = 1);

  BrokerCore(const BrokerCore&) = delete;
  BrokerCore& operator=(const BrokerCore&) = delete;
  /// Movable, so that a kernel can be built, configured and handed to a caller
  /// (or a fresh one returned from a helper) without copying the ledger.
  BrokerCore(BrokerCore&&) noexcept = default;
  BrokerCore& operator=(BrokerCore&&) noexcept = default;

  [[nodiscard]] u64 epoch() const noexcept { return epoch_; }
  [[nodiscard]] u64 sequence() const noexcept { return sequence_; }
  [[nodiscard]] u64 next_sequence() const noexcept { return sequence_ + 1; }
  [[nodiscard]] const BrokerConfig& config() const noexcept { return config_; }

  // ---- authority ----

  /// Publishes an offer generation. The generation must be greater than the
  /// site's current generation; re-publishing the identical generation is an
  /// idempotent no-op, and re-publishing it with different content is a
  /// conflict. Publishing a generation that no longer covers what is committed
  /// either refuses (ShrinkPolicy::RefuseShrink) or evicts, lowest priority
  /// first, until the ledger closes.
  Result<OfferPublication> PublishOffer(const Offer& offer);

  /// Withdraws a generation. Every commitment against it is revoked, and the
  /// uncommitted capacity is moved to the withheld term rather than silently
  /// disappearing.
  Result<OfferRevocation> RevokeOffer(const SiteId& site, u64 generation, i64 at,
                                      RevocationReason reason = RevocationReason::OfferRevoked);

  // ---- requests ----

  /// Works out what could be committed, without changing anything.
  Result<AskPlan> PlanAsk(const Ask& ask) const;

  /// Materialises the decision a plan will produce: identities, sequences, the
  /// accounting-chain digest and the durability class are all fixed here, so
  /// the exact record can be written and made durable before anything is
  /// applied. Pure: it changes nothing.
  Result<Decision> MaterializeDecision(const AskPlan& plan) const;

  /// Applies a plan. Fails with ErrorCode::Fenced when the plan was produced
  /// under a different broker epoch or against another ledger version, and with
  /// ErrorCode::InsufficientCapacity when the ledger no longer covers the plan
  /// (which is how a stale asynchronous completion is prevented from mutating
  /// newer state). The decision it returns is identical to the materialised one.
  Result<Decision> CommitPlan(const AskPlan& plan);

  /// Applies a decision that was materialised from a plan, without recomputing
  /// it, and verifies that the accounting chain reproduces the digest the
  /// decision carries. A live decision must carry the next sequence.
  Result<Decision> ApplyMaterializedDecision(const Decision& decision);

  // ---- recovery surface ----
  // These entry points exist for replay and restore. They are validated, they
  // never derive a new sequence, and they refuse a record that does not fit the
  // ledger rather than repairing it.

  void SetEpoch(u64 epoch) noexcept { epoch_ = epoch; }
  void SetSequence(u64 sequence) noexcept { sequence_ = sequence; }
  void SetStateVersion(u64 version) noexcept { state_version_ = version; }
  void SetChainDigest(const Digest& digest) noexcept { chain_digest_ = digest; }
  [[nodiscard]] u64 state_version() const noexcept { return state_version_; }
  Result<OfferPublication> ReplayOffer(const Offer& offer, u64 sequence);
  Result<OfferRevocation> ReplayRevocation(const SiteId& site, u64 generation, u64 sequence,
                                           i64 at, RevocationReason reason);
  Status ReplayDecision(const Decision& decision);
  Status RestoreConfig(const BrokerConfig& config);
  /// Records the durability boundary a decision actually reached. Called by the
  /// service once the commit marker is durable. It deliberately does not touch
  /// the accounting chain: the chain covers the decision, and the durability
  /// class is a property of the record, not of the decision.
  Status SetDecisionDurability(const DecisionId& id, DurabilityClass durability);

  /// Adopts a fully formed ledger entry from a snapshot. The entry is
  /// re-derived and re-checked here: a tranche whose committed totals disagree
  /// with the sum of its live commitments, a negative residual, an unsorted
  /// collection or a duplicate identity is refused as corruption.
  Status RestoreSiteLedger(SiteLedger ledger, std::vector<GenerationHistoryEntry> history = {});
  /// Adopts a decision record into the idempotency index without re-applying
  /// its allocations (the ledger was restored separately).
  Status RestoreDecisionRecord(const Decision& decision);
  /// Checks that every allocation a decision claims is present in the ledger.
  Status VerifyDecisionProvenance(const Decision& decision) const;

  // ---- queries ----

  [[nodiscard]] std::vector<SiteId> SiteIds() const;
  [[nodiscard]] const SiteLedger* FindSite(const SiteId& site) const;
  [[nodiscard]] Result<Decision> FindDecision(const DecisionId& id) const;
  [[nodiscard]] Result<Decision> FindDecisionByKey(const AskKey& key) const;
  [[nodiscard]] std::vector<Decision> RecentDecisions(std::size_t limit) const;
  [[nodiscard]] std::vector<AskKey> DecisionKeys() const;
  [[nodiscard]] std::vector<GenerationHistoryEntry> GenerationHistory(const SiteId& site) const;

  /// Totals derived from the stored terms, never from a running counter.
  [[nodiscard]] AccountingSummary Summary() const;

  /// Re-derives the conservation identity for every tranche and every
  /// dimension. A closed report is the primary invariant of this runtime.
  [[nodiscard]] ConservationReport VerifyConservation() const;

  /// SHA-256 over the canonical encoding of the entire authoritative state.
  /// O(state), computed on demand for snapshots, verification and recovery.
  [[nodiscard]] Digest StateDigest() const;

  /// The running order-sensitive digest of every authoritative change. Each
  /// mutation folds its canonical record into the chain, so the chain binds the
  /// whole history in order and is O(1) per operation. It is the digest a
  /// decision carries.
  [[nodiscard]] const Digest& chain_digest() const noexcept { return chain_digest_; }

  /// The digest an ask contributes, used for idempotency conflict detection.
  [[nodiscard]] static Digest RequestDigest(const Ask& ask);

 private:
  struct SiteState {
    SiteLedger ledger;
    std::vector<GenerationHistoryEntry> history;
  };

  /// Shared implementation of publish and replay.
  Result<OfferPublication> ApplyOffer(const Offer& offer, u64 sequence, bool replay);
  Result<OfferRevocation> ApplyRevocation(const SiteId& site, u64 generation, u64 sequence,
                                          i64 at, RevocationReason reason, bool replay);
  Status ApplyDecision(const Decision& decision, bool replay);
  void RetireIdempotencyRecords();

  /// Folds one canonical authoritative record into the running chain digest.
  void ExtendChain(std::string_view record_bytes);

  BrokerConfig config_;
  u64 epoch_ = 1;
  u64 sequence_ = 0;
  u64 state_version_ = 0;
  Digest chain_digest_;
  std::map<SiteId, SiteState> sites_;
  std::map<AskKey, Decision> decisions_by_key_;
  std::map<DecisionId, AskKey> decision_ids_;
  std::deque<AskKey> decision_order_;
};

}  // namespace rcb

#endif  // RCB_BROKER_HPP
