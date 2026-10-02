// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rcb/serialize.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "rcb/assert.hpp"
#include "rcb/checked.hpp"
#include "rcb/version.hpp"

namespace rcb {
namespace {

constexpr std::string_view kOfferFields[] = {
    "schema",       "site",         "generation",     "source_snapshot", "service_class",
    "region",       "jurisdiction", "risk",           "valid_from",      "valid_until",
    "reserve_policy", "policy_generation", "reserve_minimum_priority", "cost", "tranches"};

constexpr std::string_view kCostFields[] = {"reference", "price_per_milli_unit",
                                            "energy_millijoules_per_milli_unit",
                                            "carbon_milligrams_per_milli_unit"};

constexpr std::string_view kTrancheFields[] = {"domain", "allocatable", "protected_reserve"};

constexpr std::string_view kAskFields[] = {
    "schema",           "key",              "requester",           "service_class",
    "requested",        "priority",         "fairness",            "all_or_nothing",
    "require_single_source", "as_of",       "allowed_regions",     "excluded_regions",
    "allowed_jurisdictions", "excluded_jurisdictions", "allowed_sites", "excluded_sites",
    "excluded_failure_domains", "min_distinct_failure_domains", "max_sites", "max_risk",
    "has_cost_ceiling", "max_total_cost",   "has_energy_ceiling",  "max_total_energy_millijoules",
    "has_carbon_ceiling", "max_total_carbon_milligrams", "may_consume_protected_reserve",
    "reserve_policy_authorization", "has_policy_generation_pin", "policy_generation_pin",
    "generation_pins",  "require_current_generation"};

constexpr std::string_view kDecisionFields[] = {
    "id",         "key",           "requester",     "service_class", "outcome",
    "allocations", "blocking",     "requested",     "committed",     "unmet",
    "total_cost", "total_energy_millijoules", "total_carbon_milligrams",
    "distinct_failure_domains", "distinct_sites", "broker_sequence", "broker_epoch",
    "decided_at", "durability",    "replay",        "accounting_digest", "request_digest"};

constexpr std::string_view kAllocationFields[] = {
    "id",       "site",       "generation",   "domain",     "service_class",
    "allocatable_amount", "reserve_amount", "cost", "energy_millijoules", "carbon_milligrams",
    "priority", "sequence",   "created_at"};

constexpr std::string_view kCommitmentFields[] = {
    "id",       "ask_key",  "decision_id", "requester",  "site",        "created_generation",
    "accounted_generation", "domain", "service_class", "allocatable_amount", "reserve_amount",
    "cost",     "energy_millijoules", "carbon_milligrams", "priority", "sequence",
    "created_at", "live",   "has_revocation", "revocation_reason", "revoked_sequence", "revoked_at"};

constexpr std::string_view kTrancheLedgerFields[] = {
    "domain",   "allocatable",           "protected_reserve",  "committed_allocatable",
    "committed_reserve", "withheld",     "reserve_withheld"};

constexpr std::string_view kSiteLedgerFields[] = {
    "site",         "generation",     "source_snapshot", "service_class", "region",
    "jurisdiction", "risk",           "valid_from",      "valid_until",   "reserve_policy",
    "policy_generation", "reserve_minimum_priority", "cost", "revoked", "revocation_reason",
    "revoked_sequence", "revoked_at", "published_sequence", "tranches", "commitments"};

constexpr std::string_view kSiteStateFields[] = {"ledger", "history"};

constexpr std::string_view kHistoryFields[] = {"generation", "sequence", "at", "reason",
                                               "commitments_revoked"};

constexpr std::string_view kStateFields[] = {"chain_digest", "config",   "decisions", "epoch",
                                             "format",       "sequence", "sites",     "state_version"};

constexpr std::string_view kConfigFields[] = {"shrink_policy", "default_fairness",
                                              "max_retained_decisions", "max_blocking_constraints",
                                              "generation_history"};

/// Rejects any member that is not in the documented field set. A typo in an
/// operator document is a refusal, not a default.
Status CheckMembers(const JsonValue& object, const std::string_view* allowed, const std::size_t count) {
  if (!object.IsObject()) {
    return Status::Error(ErrorCode::MalformedInput, "expected a JSON object");
  }
  for (const auto& member : object.members()) {
    bool known = false;
    for (std::size_t i = 0; i < count; ++i) {
      if (member.first == allowed[i]) {
        known = true;
        break;
      }
    }
    if (!known) {
      return Status::Error(ErrorCode::UnknownField, "unknown field '" + member.first + "'");
    }
  }
  return Status::Ok();
}

template <std::size_t N>
Status CheckMembers(const JsonValue& object, const std::string_view (&allowed)[N]) {
  return CheckMembers(object, allowed, N);
}

Result<const JsonValue*> Require(const JsonValue& object, const char* key) {
  const JsonValue* member = object.Find(key);
  if (member == nullptr) {
    return Fail<const JsonValue*>(ErrorCode::MissingField,
                                 std::string("field '") + key + "' is required");
  }
  return member;
}

Result<std::string> RequireString(const JsonValue& object, const char* key,
                                  const bool allow_empty = false) {
  Result<const JsonValue*> member = Require(object, key);
  if (!member.ok()) {
    return Result<std::string>(member.status());
  }
  if (!member.value()->IsString()) {
    return Fail<std::string>(ErrorCode::MalformedInput,
                             std::string("field '") + key + "' must be a string");
  }
  if (!allow_empty && member.value()->AsString().empty()) {
    return Fail<std::string>(ErrorCode::MissingField,
                             std::string("field '") + key + "' must not be empty");
  }
  return member.value()->AsString();
}

Result<i64> RequireInt(const JsonValue& object, const char* key) {
  Result<const JsonValue*> member = Require(object, key);
  if (!member.ok()) {
    return Result<i64>(member.status());
  }
  if (!member.value()->IsInt()) {
    return Fail<i64>(ErrorCode::MalformedInput,
                     std::string("field '") + key + "' must be an integer");
  }
  return member.value()->AsInt();
}

Result<u64> RequireUint(const JsonValue& object, const char* key) {
  Result<const JsonValue*> member = Require(object, key);
  if (!member.ok()) {
    return Result<u64>(member.status());
  }
  if (member.value()->IsUint()) {
    return member.value()->AsUint();
  }
  if (member.value()->IsInt()) {
    if (member.value()->AsInt() < 0) {
      return Fail<u64>(ErrorCode::OutOfRange,
                       std::string("field '") + key + "' must not be negative");
    }
    return static_cast<u64>(member.value()->AsInt());
  }
  return Fail<u64>(ErrorCode::MalformedInput,
                   std::string("field '") + key + "' must be an unsigned integer");
}

Result<bool> RequireBool(const JsonValue& object, const char* key) {
  Result<const JsonValue*> member = Require(object, key);
  if (!member.ok()) {
    return Result<bool>(member.status());
  }
  if (!member.value()->IsBool()) {
    return Fail<bool>(ErrorCode::MalformedInput,
                      std::string("field '") + key + "' must be a boolean");
  }
  return member.value()->AsBool();
}

Result<std::string> OptionalString(const JsonValue& object, const char* key,
                                   const std::string& fallback = std::string()) {
  const JsonValue* member = object.Find(key);
  if (member == nullptr) {
    return fallback;
  }
  if (!member->IsString()) {
    return Fail<std::string>(ErrorCode::MalformedInput,
                             std::string("field '") + key + "' must be a string");
  }
  return member->AsString();
}

template <class Tag>
Result<TaggedId<Tag>> RequireId(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<TaggedId<Tag>>(text.status());
  }
  Result<TaggedId<Tag>> identifier = TaggedId<Tag>::Make(text.value());
  if (!identifier.ok()) {
    return Fail<TaggedId<Tag>>(identifier.status().code(),
                               std::string(key) + ": " + identifier.status().detail());
  }
  return identifier;
}

template <class Tag>
Result<TaggedId<Tag>> OptionalId(const JsonValue& object, const char* key) {
  Result<std::string> text = OptionalString(object, key);
  if (!text.ok()) {
    return Result<TaggedId<Tag>>(text.status());
  }
  if (text.value().empty()) {
    return TaggedId<Tag>();
  }
  Result<TaggedId<Tag>> identifier = TaggedId<Tag>::Make(text.value());
  if (!identifier.ok()) {
    return Fail<TaggedId<Tag>>(identifier.status().code(),
                               std::string(key) + ": " + identifier.status().detail());
  }
  return identifier;
}

Result<CapacityVector> DecodeCapacityVector(const JsonValue& value) {
  const Status members = CheckMembers(value, {"power", "cooling", "rack_space", "service_capacity"});
  if (!members.ok()) {
    return Fail<CapacityVector>(members);
  }
  std::array<i64, kDimensionCount> values{0, 0, 0, 0};
  for (std::size_t i = 0; i < kDimensionCount; ++i) {
    const std::string key(DimensionToken(kDimensions[i]));
    const JsonValue* member = value.Find(key);
    if (member == nullptr) {
      return Fail<CapacityVector>(ErrorCode::MissingField,
                                  "capacity vector is missing dimension '" + key + "'");
    }
    if (!member->IsNumber()) {
      return Fail<CapacityVector>(ErrorCode::MalformedInput,
                                  "dimension '" + key + "' must be an integer");
    }
    if (member->IsInt() && member->AsInt() < 0) {
      return Fail<CapacityVector>(ErrorCode::InvalidArgument,
                                  "dimension '" + key + "' must not be negative");
    }
    const u64 raw = member->IsUint() ? member->AsUint() : static_cast<u64>(member->AsInt());
    if (raw > static_cast<u64>(Limits::kMaxDimensionValue)) {
      return Fail<CapacityVector>(ErrorCode::OutOfRange,
                                  "dimension '" + key + "' exceeds the per-dimension bound");
    }
    values[i] = static_cast<i64>(raw);
  }
  return CapacityVector::Make(values);
}

Result<ScaledAmount> DecodeScaledAmount(const JsonValue& value, const char* what) {
  if (!value.IsString()) {
    return Fail<ScaledAmount>(ErrorCode::MalformedInput,
                              std::string(what) +
                                  " must be an exact decimal string, not a JSON number");
  }
  Result<ScaledAmount> amount = ScaledAmount::Parse(value.AsString());
  if (!amount.ok()) {
    return Fail<ScaledAmount>(amount.status().code(),
                              std::string(what) + ": " + amount.status().detail());
  }
  return amount;
}

Result<std::vector<std::string>> DecodeStringList(const JsonValue& value, const char* what) {
  if (!value.IsArray()) {
    return Fail<std::vector<std::string>>(ErrorCode::MalformedInput,
                                          std::string(what) + " must be an array");
  }
  if (value.items().size() > Limits::kMaxListElements) {
    return Fail<std::vector<std::string>>(ErrorCode::LimitExceeded,
                                          std::string(what) + " has too many elements");
  }
  std::vector<std::string> out;
  out.reserve(value.items().size());
  for (const JsonValue& item : value.items()) {
    if (!item.IsString()) {
      return Fail<std::vector<std::string>>(ErrorCode::MalformedInput,
                                            std::string(what) + " must contain strings");
    }
    out.push_back(item.AsString());
  }
  return out;
}

template <class Tag>
Result<std::vector<TaggedId<Tag>>> DecodeIdList(const JsonValue& value, const char* what) {
  Result<std::vector<std::string>> texts = DecodeStringList(value, what);
  if (!texts.ok()) {
    return Result<std::vector<TaggedId<Tag>>>(texts.status());
  }
  std::vector<TaggedId<Tag>> ids;
  ids.reserve(texts.value().size());
  for (const std::string& text : texts.value()) {
    Result<TaggedId<Tag>> identifier = TaggedId<Tag>::Make(text);
    if (!identifier.ok()) {
      return Fail<std::vector<TaggedId<Tag>>>(
          identifier.status().code(), std::string(what) + ": " + identifier.status().detail());
    }
    ids.push_back(identifier.value());
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

template <class Tag>
JsonValue MakeIdList(const std::vector<TaggedId<Tag>>& values) {
  JsonValue array = JsonValue::MakeArray();
  for (const auto& value : values) {
    array.Push(JsonValue::MakeString(value.value()));
  }
  return array;
}

/// The decision object minus the fields that are derived after the fact. The
/// accounting chain is computed over exactly these bytes, so a decision can be
/// replayed and re-checked without depending on when its record was flushed.
JsonValue DecisionJsonForChain(const Decision& decision) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("id", JsonValue::MakeString(decision.id.value()));
  object.Set("key", JsonValue::MakeString(decision.key.value()));
  object.Set("requester", JsonValue::MakeString(decision.requester.value()));
  object.Set("service_class", JsonValue::MakeString(decision.service_class.value()));
  object.Set("outcome", JsonValue::MakeString(std::string(DecisionOutcomeToken(decision.outcome))));
  JsonValue allocations = JsonValue::MakeArray();
  for (const Allocation& allocation : decision.allocations) {
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("id", JsonValue::MakeString(allocation.id.value()));
    entry.Set("site", JsonValue::MakeString(allocation.site.value()));
    entry.Set("generation", JsonValue::MakeUint(allocation.generation));
    entry.Set("domain", JsonValue::MakeString(allocation.domain.value()));
    entry.Set("service_class", JsonValue::MakeString(allocation.service_class.value()));
    entry.Set("allocatable_amount", ToJson(allocation.allocatable_amount));
    entry.Set("reserve_amount", ToJson(allocation.reserve_amount));
    entry.Set("cost", ToJson(allocation.cost));
    entry.Set("energy_millijoules", JsonValue::MakeInt(allocation.energy_millijoules));
    entry.Set("carbon_milligrams", JsonValue::MakeInt(allocation.carbon_milligrams));
    entry.Set("priority", JsonValue::MakeString(std::string(PriorityToken(allocation.priority))));
    entry.Set("sequence", JsonValue::MakeUint(allocation.sequence));
    entry.Set("created_at", JsonValue::MakeInt(allocation.created_at));
    allocations.Push(std::move(entry));
  }
  object.Set("allocations", std::move(allocations));
  JsonValue blocking = JsonValue::MakeArray();
  for (const BlockingConstraint& constraint : decision.blocking) {
    blocking.Push(ToJson(constraint));
  }
  object.Set("blocking", std::move(blocking));
  object.Set("requested", ToJson(decision.requested));
  object.Set("committed", ToJson(decision.committed));
  object.Set("unmet", ToJson(decision.unmet));
  object.Set("total_cost", ToJson(decision.total_cost));
  object.Set("total_energy_millijoules", JsonValue::MakeInt(decision.total_energy_millijoules));
  object.Set("total_carbon_milligrams", JsonValue::MakeInt(decision.total_carbon_milligrams));
  object.Set("distinct_failure_domains", JsonValue::MakeUint(decision.distinct_failure_domains));
  object.Set("distinct_sites", JsonValue::MakeUint(decision.distinct_sites));
  object.Set("broker_sequence", JsonValue::MakeUint(decision.broker_sequence));
  object.Set("broker_epoch", JsonValue::MakeUint(decision.broker_epoch));
  object.Set("decided_at", JsonValue::MakeInt(decision.decided_at));
  object.Set("request_digest", JsonValue::MakeString(decision.request_digest.Hex()));
  return object;
}

JsonValue SiteLedgerJson(const SiteLedger& ledger) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("site", JsonValue::MakeString(ledger.site.value()));
  object.Set("generation", JsonValue::MakeUint(ledger.generation));
  object.Set("source_snapshot", JsonValue::MakeString(ledger.source_snapshot.value()));
  object.Set("service_class", JsonValue::MakeString(ledger.service_class.value()));
  object.Set("region", JsonValue::MakeString(ledger.region.value()));
  object.Set("jurisdiction", JsonValue::MakeString(ledger.jurisdiction.value()));
  object.Set("risk", JsonValue::MakeString(std::string(RiskTierToken(ledger.risk))));
  object.Set("valid_from", JsonValue::MakeInt(ledger.valid_from));
  object.Set("valid_until", JsonValue::MakeInt(ledger.valid_until));
  object.Set("reserve_policy", JsonValue::MakeString(ledger.reserve_policy.value()));
  object.Set("policy_generation", JsonValue::MakeUint(ledger.policy_generation));
  object.Set("reserve_minimum_priority",
             JsonValue::MakeString(std::string(PriorityToken(ledger.reserve_minimum_priority))));
  object.Set("cost", ToJson(ledger.cost));
  object.Set("revoked", JsonValue::MakeBool(ledger.revoked));
  object.Set("revocation_reason",
             JsonValue::MakeString(std::string(RevocationReasonToken(ledger.revocation_reason))));
  object.Set("revoked_sequence", JsonValue::MakeUint(ledger.revoked_sequence));
  object.Set("revoked_at", JsonValue::MakeInt(ledger.revoked_at));
  object.Set("published_sequence", JsonValue::MakeUint(ledger.published_sequence));
  JsonValue tranches = JsonValue::MakeArray();
  for (const TrancheLedger& tranche : ledger.tranches) {
    tranches.Push(ToJson(tranche));
  }
  object.Set("tranches", std::move(tranches));
  JsonValue commitments = JsonValue::MakeArray();
  for (const CommitmentRecord& commitment : ledger.commitments) {
    commitments.Push(ToJson(commitment));
  }
  object.Set("commitments", std::move(commitments));
  return object;
}

}  // namespace

// ---- JSON projections ----------------------------------------------------

JsonValue ToJson(const CapacityVector& value) {
  JsonValue object = JsonValue::MakeObject();
  for (const Dimension dimension : kDimensions) {
    JsonValue entry = JsonValue::MakeInt(value.Get(dimension));
    object.Set(std::string(DimensionToken(dimension)), std::move(entry));
  }
  return object;
}

JsonValue ToJson(const ScaledAmount& value) {
  return JsonValue::MakeString(value.ToString());
}

JsonValue ToJson(const CostEvidence& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("reference", JsonValue::MakeString(value.reference.value()));
  object.Set("price_per_milli_unit", ToJson(value.price_per_milli_unit));
  object.Set("energy_millijoules_per_milli_unit",
             JsonValue::MakeInt(value.energy_millijoules_per_milli_unit));
  object.Set("carbon_milligrams_per_milli_unit",
             JsonValue::MakeInt(value.carbon_milligrams_per_milli_unit));
  return object;
}

JsonValue ToJson(const CapacityTranche& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("domain", JsonValue::MakeString(value.domain.value()));
  object.Set("allocatable", ToJson(value.allocatable));
  object.Set("protected_reserve", ToJson(value.protected_reserve));
  return object;
}

JsonValue ToJson(const Offer& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("schema", JsonValue::MakeUint(kRequestSchemaVersion));
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("generation", JsonValue::MakeUint(value.generation));
  object.Set("source_snapshot", JsonValue::MakeString(value.source_snapshot.value()));
  object.Set("service_class", JsonValue::MakeString(value.service_class.value()));
  object.Set("region", JsonValue::MakeString(value.region.value()));
  object.Set("jurisdiction", JsonValue::MakeString(value.jurisdiction.value()));
  object.Set("risk", JsonValue::MakeString(std::string(RiskTierToken(value.risk))));
  object.Set("valid_from", JsonValue::MakeInt(value.valid_from));
  object.Set("valid_until", JsonValue::MakeInt(value.valid_until));
  object.Set("reserve_policy", JsonValue::MakeString(value.reserve_policy.value()));
  object.Set("policy_generation", JsonValue::MakeUint(value.policy_generation));
  object.Set("reserve_minimum_priority",
             JsonValue::MakeString(std::string(PriorityToken(value.reserve_minimum_priority))));
  object.Set("cost", ToJson(value.cost));
  JsonValue tranches = JsonValue::MakeArray();
  for (const CapacityTranche& tranche : value.tranches) {
    tranches.Push(ToJson(tranche));
  }
  object.Set("tranches", std::move(tranches));
  return object;
}

JsonValue ToJson(const GenerationPin& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("generation", JsonValue::MakeUint(value.generation));
  return object;
}

JsonValue ToJson(const Ask& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("schema", JsonValue::MakeUint(kRequestSchemaVersion));
  object.Set("key", JsonValue::MakeString(value.key.value()));
  object.Set("requester", JsonValue::MakeString(value.requester.value()));
  object.Set("service_class", JsonValue::MakeString(value.service_class.value()));
  object.Set("requested", ToJson(value.requested));
  object.Set("priority", JsonValue::MakeString(std::string(PriorityToken(value.priority))));
  object.Set("fairness", JsonValue::MakeString(std::string(FairnessToken(value.fairness))));
  object.Set("all_or_nothing", JsonValue::MakeBool(value.all_or_nothing));
  object.Set("require_single_source", JsonValue::MakeBool(value.require_single_source));
  object.Set("as_of", JsonValue::MakeInt(value.as_of));
  object.Set("allowed_regions", MakeIdList(value.allowed_regions));
  object.Set("excluded_regions", MakeIdList(value.excluded_regions));
  object.Set("allowed_jurisdictions", MakeIdList(value.allowed_jurisdictions));
  object.Set("excluded_jurisdictions", MakeIdList(value.excluded_jurisdictions));
  object.Set("allowed_sites", MakeIdList(value.allowed_sites));
  object.Set("excluded_sites", MakeIdList(value.excluded_sites));
  object.Set("excluded_failure_domains", MakeIdList(value.excluded_failure_domains));
  object.Set("min_distinct_failure_domains", JsonValue::MakeUint(value.min_distinct_failure_domains));
  object.Set("max_sites", JsonValue::MakeUint(value.max_sites));
  object.Set("max_risk", JsonValue::MakeString(std::string(RiskTierToken(value.max_risk))));
  object.Set("has_cost_ceiling", JsonValue::MakeBool(value.has_cost_ceiling));
  object.Set("max_total_cost", ToJson(value.max_total_cost));
  object.Set("has_energy_ceiling", JsonValue::MakeBool(value.has_energy_ceiling));
  object.Set("max_total_energy_millijoules", JsonValue::MakeInt(value.max_total_energy_millijoules));
  object.Set("has_carbon_ceiling", JsonValue::MakeBool(value.has_carbon_ceiling));
  object.Set("max_total_carbon_milligrams", JsonValue::MakeInt(value.max_total_carbon_milligrams));
  object.Set("may_consume_protected_reserve",
             JsonValue::MakeBool(value.may_consume_protected_reserve));
  object.Set("reserve_policy_authorization",
             JsonValue::MakeString(value.reserve_policy_authorization.value()));
  object.Set("has_policy_generation_pin", JsonValue::MakeBool(value.has_policy_generation_pin));
  object.Set("policy_generation_pin", JsonValue::MakeUint(value.policy_generation_pin));
  JsonValue pins = JsonValue::MakeArray();
  for (const GenerationPin& pin : value.generation_pins) {
    pins.Push(ToJson(pin));
  }
  object.Set("generation_pins", std::move(pins));
  object.Set("require_current_generation", JsonValue::MakeBool(value.require_current_generation));
  return object;
}

JsonValue ToJson(const BlockingConstraint& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("kind", JsonValue::MakeString(std::string(ConstraintToken(value.kind))));
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("domain", JsonValue::MakeString(value.domain.value()));
  object.Set("has_dimension", JsonValue::MakeBool(value.has_dimension));
  object.Set("dimension", JsonValue::MakeString(std::string(DimensionToken(value.dimension))));
  object.Set("required", JsonValue::MakeInt(value.required));
  object.Set("available", JsonValue::MakeInt(value.available));
  object.Set("detail", JsonValue::MakeString(value.detail));
  return object;
}

JsonValue ToJson(const Allocation& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("id", JsonValue::MakeString(value.id.value()));
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("generation", JsonValue::MakeUint(value.generation));
  object.Set("domain", JsonValue::MakeString(value.domain.value()));
  object.Set("service_class", JsonValue::MakeString(value.service_class.value()));
  object.Set("allocatable_amount", ToJson(value.allocatable_amount));
  object.Set("reserve_amount", ToJson(value.reserve_amount));
  object.Set("cost", ToJson(value.cost));
  object.Set("energy_millijoules", JsonValue::MakeInt(value.energy_millijoules));
  object.Set("carbon_milligrams", JsonValue::MakeInt(value.carbon_milligrams));
  object.Set("priority", JsonValue::MakeString(std::string(PriorityToken(value.priority))));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("created_at", JsonValue::MakeInt(value.created_at));
  return object;
}

JsonValue ToJson(const Decision& value) {
  JsonValue object = DecisionJsonForChain(value);
  object.Set("durability", JsonValue::MakeString(std::string(DurabilityToken(value.durability))));
  object.Set("replay", JsonValue::MakeBool(value.replay));
  object.Set("accounting_digest", JsonValue::MakeString(value.accounting_digest.Hex()));
  return object;
}

JsonValue ToJson(const CommitmentRecord& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("id", JsonValue::MakeString(value.id.value()));
  object.Set("ask_key", JsonValue::MakeString(value.ask_key.value()));
  object.Set("decision_id", JsonValue::MakeString(value.decision_id.value()));
  object.Set("requester", JsonValue::MakeString(value.requester.value()));
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("created_generation", JsonValue::MakeUint(value.created_generation));
  object.Set("accounted_generation", JsonValue::MakeUint(value.accounted_generation));
  object.Set("domain", JsonValue::MakeString(value.domain.value()));
  object.Set("service_class", JsonValue::MakeString(value.service_class.value()));
  object.Set("allocatable_amount", ToJson(value.allocatable_amount));
  object.Set("reserve_amount", ToJson(value.reserve_amount));
  object.Set("cost", ToJson(value.cost));
  object.Set("energy_millijoules", JsonValue::MakeInt(value.energy_millijoules));
  object.Set("carbon_milligrams", JsonValue::MakeInt(value.carbon_milligrams));
  object.Set("priority", JsonValue::MakeString(std::string(PriorityToken(value.priority))));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("created_at", JsonValue::MakeInt(value.created_at));
  object.Set("live", JsonValue::MakeBool(value.live));
  object.Set("has_revocation", JsonValue::MakeBool(value.has_revocation));
  object.Set("revocation_reason",
             JsonValue::MakeString(std::string(RevocationReasonToken(value.revocation_reason))));
  object.Set("revoked_sequence", JsonValue::MakeUint(value.revoked_sequence));
  object.Set("revoked_at", JsonValue::MakeInt(value.revoked_at));
  return object;
}

JsonValue ToJson(const TrancheLedger& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("domain", JsonValue::MakeString(value.domain.value()));
  object.Set("allocatable", ToJson(value.allocatable));
  object.Set("protected_reserve", ToJson(value.protected_reserve));
  object.Set("committed_allocatable", ToJson(value.committed_allocatable));
  object.Set("committed_reserve", ToJson(value.committed_reserve));
  object.Set("withheld", ToJson(value.withheld));
  object.Set("reserve_withheld", ToJson(value.reserve_withheld));
  return object;
}

JsonValue ToJson(const SiteLedger& value) { return SiteLedgerJson(value); }

JsonValue ToJson(const GenerationHistoryEntry& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("generation", JsonValue::MakeUint(value.generation));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("at", JsonValue::MakeInt(value.at));
  object.Set("reason", JsonValue::MakeString(std::string(RevocationReasonToken(value.reason))));
  object.Set("commitments_revoked", JsonValue::MakeUint(value.commitments_revoked));
  return object;
}

JsonValue ToJson(const AccountingSummary& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("sites", JsonValue::MakeUint(value.sites));
  object.Set("tranches", JsonValue::MakeUint(value.tranches));
  object.Set("live_commitments", JsonValue::MakeUint(value.live_commitments));
  object.Set("revoked_commitments", JsonValue::MakeUint(value.revoked_commitments));
  object.Set("decisions", JsonValue::MakeUint(value.decisions));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("epoch", JsonValue::MakeUint(value.epoch));
  object.Set("committed_total", ToJson(value.committed_total));
  object.Set("remaining_allocatable_total", ToJson(value.remaining_allocatable_total));
  object.Set("withheld_total", ToJson(value.withheld_total));
  object.Set("reserve_remaining_total", ToJson(value.reserve_remaining_total));
  object.Set("state_digest", JsonValue::MakeString(value.state_digest.Hex()));
  return object;
}

JsonValue ToJson(const ConservationReport& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("closed", JsonValue::MakeBool(value.closed));
  object.Set("sites_checked", JsonValue::MakeUint(value.sites_checked));
  object.Set("tranches_checked", JsonValue::MakeUint(value.tranches_checked));
  object.Set("commitments_checked", JsonValue::MakeUint(value.commitments_checked));
  object.Set("live_commitments", JsonValue::MakeUint(value.live_commitments));
  object.Set("revoked_commitments", JsonValue::MakeUint(value.revoked_commitments));
  object.Set("allocatable_total", ToJson(value.allocatable_total));
  object.Set("committed_total", ToJson(value.committed_total));
  object.Set("remaining_total", ToJson(value.remaining_total));
  object.Set("withheld_total", ToJson(value.withheld_total));
  object.Set("reserve_total", ToJson(value.reserve_total));
  object.Set("reserve_committed_total", ToJson(value.reserve_committed_total));
  object.Set("reserve_remaining_total", ToJson(value.reserve_remaining_total));
  object.Set("reserve_withheld_total", ToJson(value.reserve_withheld_total));
  JsonValue violations = JsonValue::MakeArray();
  for (const ConservationViolation& violation : value.violations) {
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("scope", JsonValue::MakeString(violation.scope));
    entry.Set("detail", JsonValue::MakeString(violation.detail));
    entry.Set("has_dimension", JsonValue::MakeBool(violation.has_dimension));
    entry.Set("dimension", JsonValue::MakeString(std::string(DimensionToken(violation.dimension))));
    violations.Push(std::move(entry));
  }
  object.Set("violations", std::move(violations));
  return object;
}

JsonValue ToJson(const OfferPublication& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("generation", JsonValue::MakeUint(value.generation));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("idempotent_replay", JsonValue::MakeBool(value.idempotent_replay));
  object.Set("carried_commitments", JsonValue::MakeUint(value.carried_commitments));
  object.Set("commitments_revoked", JsonValue::MakeUint(value.commitments_revoked));
  object.Set("allocatable_total", ToJson(value.allocatable_total));
  object.Set("reserve_total", ToJson(value.reserve_total));
  JsonValue revoked = JsonValue::MakeArray();
  for (const CommitmentRecord& commitment : value.revoked) {
    revoked.Push(ToJson(commitment));
  }
  object.Set("revoked", std::move(revoked));
  return object;
}

JsonValue ToJson(const OfferRevocation& value) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("site", JsonValue::MakeString(value.site.value()));
  object.Set("generation", JsonValue::MakeUint(value.generation));
  object.Set("sequence", JsonValue::MakeUint(value.sequence));
  object.Set("at", JsonValue::MakeInt(value.at));
  object.Set("commitments_revoked", JsonValue::MakeUint(value.commitments_revoked));
  object.Set("withheld", ToJson(value.withheld));
  JsonValue revoked = JsonValue::MakeArray();
  for (const CommitmentRecord& commitment : value.revoked) {
    revoked.Push(ToJson(commitment));
  }
  object.Set("revoked", std::move(revoked));
  return object;
}

std::string EncodeJson(const JsonValue& value, const bool pretty) { return value.ToText(pretty); }

std::string CanonicalOfferText(const Offer& offer) { return ToJson(offer).ToText(false); }

std::string CanonicalAskText(const Ask& ask) { return ToJson(ask).ToText(false); }

std::string CanonicalDecisionText(const Decision& decision) {
  return DecisionJsonForChain(decision).ToText(false);
}

Digest AskDigest(const Ask& ask) { return Sha256Of(CanonicalAskText(ask)); }

Digest OfferDigest(const Offer& offer) { return Sha256Of(CanonicalOfferText(offer)); }

std::string CanonicalStateText(const BrokerCore& core) {
  JsonValue root = JsonValue::MakeObject();
  root.Set("format", JsonValue::MakeUint(kPersistenceFormatVersion));
  root.Set("epoch", JsonValue::MakeUint(core.epoch()));
  root.Set("sequence", JsonValue::MakeUint(core.sequence()));
  root.Set("state_version", JsonValue::MakeUint(core.state_version()));
  root.Set("chain_digest", JsonValue::MakeString(core.chain_digest().Hex()));

  JsonValue config = JsonValue::MakeObject();
  config.Set("shrink_policy", JsonValue::MakeString(std::string(ShrinkPolicyToken(core.config().shrink_policy))));
  config.Set("default_fairness",
             JsonValue::MakeString(std::string(FairnessToken(core.config().default_fairness))));
  config.Set("max_retained_decisions", JsonValue::MakeUint(core.config().max_retained_decisions));
  config.Set("max_blocking_constraints", JsonValue::MakeUint(core.config().max_blocking_constraints));
  config.Set("generation_history", JsonValue::MakeUint(core.config().generation_history));
  root.Set("config", std::move(config));

  JsonValue sites = JsonValue::MakeArray();
  for (const SiteId& site : core.SiteIds()) {
    const SiteLedger* ledger = core.FindSite(site);
    if (ledger == nullptr) {
      continue;
    }
    JsonValue entry = JsonValue::MakeObject();
    entry.Set("ledger", ToJson(*ledger));
    JsonValue history = JsonValue::MakeArray();
    for (const GenerationHistoryEntry& item : core.GenerationHistory(site)) {
      history.Push(ToJson(item));
    }
    entry.Set("history", std::move(history));
    sites.Push(std::move(entry));
  }
  root.Set("sites", std::move(sites));

  JsonValue decisions = JsonValue::MakeArray();
  for (const AskKey& key : core.DecisionKeys()) {
    Result<Decision> decision = core.FindDecisionByKey(key);
    if (decision.ok()) {
      decisions.Push(ToJson(decision.value()));
    }
  }
  root.Set("decisions", std::move(decisions));
  return root.ToText(false);
}

Digest StateDigestOf(const BrokerCore& core) { return Sha256Of(CanonicalStateText(core)); }

// ---- decoding ------------------------------------------------------------

namespace {

Result<RiskTier> DecodeRisk(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<RiskTier>(text.status());
  }
  Result<RiskTier> parsed = ParseRiskTier(text.value());
  if (!parsed.ok()) {
    return Fail<RiskTier>(parsed.status().code(),
                          std::string(key) + ": " + parsed.status().detail());
  }
  return parsed;
}

Result<PriorityClass> DecodePriority(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<PriorityClass>(text.status());
  }
  Result<PriorityClass> parsed = ParsePriority(text.value());
  if (!parsed.ok()) {
    return Fail<PriorityClass>(parsed.status().code(),
                               std::string(key) + ": " + parsed.status().detail());
  }
  return parsed;
}

Result<FairnessPolicy> DecodeFairness(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<FairnessPolicy>(text.status());
  }
  Result<FairnessPolicy> parsed = ParseFairness(text.value());
  if (!parsed.ok()) {
    return Fail<FairnessPolicy>(parsed.status().code(),
                                std::string(key) + ": " + parsed.status().detail());
  }
  return parsed;
}

Result<RevocationReason> DecodeRevocationReason(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<RevocationReason>(text.status());
  }
  if (text.value() == "superseded_by_generation") {
    return RevocationReason::SupersededByGeneration;
  }
  if (text.value() == "offer_revoked") {
    return RevocationReason::OfferRevoked;
  }
  if (text.value() == "capacity_shrink") {
    return RevocationReason::CapacityShrink;
  }
  return Fail<RevocationReason>(ErrorCode::InvalidArgument,
                                std::string(key) + ": unknown revocation reason token");
}

Result<DurabilityClass> DecodeDurability(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<DurabilityClass>(text.status());
  }
  if (text.value() == "durable") {
    return DurabilityClass::Durable;
  }
  if (text.value() == "buffered") {
    return DurabilityClass::Buffered;
  }
  if (text.value() == "volatile") {
    return DurabilityClass::Volatile;
  }
  return Fail<DurabilityClass>(ErrorCode::InvalidArgument,
                               std::string(key) + ": unknown durability token");
}

Result<DecisionOutcome> DecodeOutcome(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<DecisionOutcome>(text.status());
  }
  if (text.value() == "accepted") {
    return DecisionOutcome::Accepted;
  }
  if (text.value() == "partially_accepted") {
    return DecisionOutcome::PartiallyAccepted;
  }
  if (text.value() == "refused") {
    return DecisionOutcome::Refused;
  }
  return Fail<DecisionOutcome>(ErrorCode::InvalidArgument,
                               std::string(key) + ": unknown decision outcome token");
}

Result<ConstraintKind> DecodeConstraintKind(const JsonValue& object, const char* key) {
  Result<std::string> text = RequireString(object, key);
  if (!text.ok()) {
    return Result<ConstraintKind>(text.status());
  }
  for (u32 raw = 0; raw <= static_cast<u32>(ConstraintKind::LedgerLimitReached); ++raw) {
    const auto kind = static_cast<ConstraintKind>(raw);
    if (ConstraintToken(kind) == text.value()) {
      return kind;
    }
  }
  return Fail<ConstraintKind>(ErrorCode::InvalidArgument,
                              std::string(key) + ": unknown blocking constraint token");
}

Result<CostEvidence> DecodeCostEvidence(const JsonValue& value) {
  const Status members = CheckMembers(value, kCostFields);
  if (!members.ok()) {
    return Fail<CostEvidence>(members);
  }
  CostEvidence evidence;
  Result<EvidenceId> reference = RequireId<EvidenceIdTag>(value, "reference");
  if (!reference.ok()) {
    return Result<CostEvidence>(reference.status());
  }
  evidence.reference = reference.value();
  Result<const JsonValue*> price = Require(value, "price_per_milli_unit");
  if (!price.ok()) {
    return Result<CostEvidence>(price.status());
  }
  Result<ScaledAmount> decoded_price = DecodeScaledAmount(*price.value(), "price_per_milli_unit");
  if (!decoded_price.ok()) {
    return Result<CostEvidence>(decoded_price.status());
  }
  evidence.price_per_milli_unit = decoded_price.value();
  Result<i64> energy = RequireInt(value, "energy_millijoules_per_milli_unit");
  if (!energy.ok()) {
    return Result<CostEvidence>(energy.status());
  }
  evidence.energy_millijoules_per_milli_unit = energy.value();
  Result<i64> carbon = RequireInt(value, "carbon_milligrams_per_milli_unit");
  if (!carbon.ok()) {
    return Result<CostEvidence>(carbon.status());
  }
  evidence.carbon_milligrams_per_milli_unit = carbon.value();
  return evidence;
}

Result<CapacityTranche> DecodeCapacityTranche(const JsonValue& value) {
  const Status members = CheckMembers(value, kTrancheFields);
  if (!members.ok()) {
    return Fail<CapacityTranche>(members);
  }
  CapacityTranche tranche;
  Result<FailureDomainId> domain = RequireId<FailureDomainIdTag>(value, "domain");
  if (!domain.ok()) {
    return Result<CapacityTranche>(domain.status());
  }
  tranche.domain = domain.value();
  Result<const JsonValue*> allocatable = Require(value, "allocatable");
  if (!allocatable.ok()) {
    return Result<CapacityTranche>(allocatable.status());
  }
  Result<CapacityVector> decoded_allocatable = DecodeCapacityVector(*allocatable.value());
  if (!decoded_allocatable.ok()) {
    return Result<CapacityTranche>(decoded_allocatable.status());
  }
  tranche.allocatable = decoded_allocatable.value();
  Result<const JsonValue*> reserve = Require(value, "protected_reserve");
  if (!reserve.ok()) {
    return Result<CapacityTranche>(reserve.status());
  }
  Result<CapacityVector> decoded_reserve = DecodeCapacityVector(*reserve.value());
  if (!decoded_reserve.ok()) {
    return Result<CapacityTranche>(decoded_reserve.status());
  }
  tranche.protected_reserve = decoded_reserve.value();
  return tranche;
}

Result<TrancheLedger> DecodeTrancheLedger(const JsonValue& value) {
  const Status members = CheckMembers(value, kTrancheLedgerFields);
  if (!members.ok()) {
    return Fail<TrancheLedger>(members);
  }
  TrancheLedger tranche;
  Result<FailureDomainId> domain = RequireId<FailureDomainIdTag>(value, "domain");
  if (!domain.ok()) {
    return Result<TrancheLedger>(domain.status());
  }
  tranche.domain = domain.value();
  const std::pair<const char*, CapacityVector*> vectors[] = {
      {"allocatable", &tranche.allocatable},
      {"protected_reserve", &tranche.protected_reserve},
      {"committed_allocatable", &tranche.committed_allocatable},
      {"committed_reserve", &tranche.committed_reserve},
      {"withheld", &tranche.withheld},
      {"reserve_withheld", &tranche.reserve_withheld}};
  for (const auto& entry : vectors) {
    Result<const JsonValue*> member = Require(value, entry.first);
    if (!member.ok()) {
      return Result<TrancheLedger>(member.status());
    }
    Result<CapacityVector> decoded = DecodeCapacityVector(*member.value());
    if (!decoded.ok()) {
      return Result<TrancheLedger>(decoded.status());
    }
    *entry.second = decoded.value();
  }
  return tranche;
}

}  // namespace

namespace {

/// Documented optional fields and their defaults. Applying them here keeps the
/// strict decoder strict: a field that is present is still type-checked and an
/// unknown field is still refused, but an operator is not forced to spell out
/// every default by hand.
void ApplyAskDefaults(JsonValue& object) {
  const auto set = [&object](const char* key, JsonValue value) {
    if (object.Find(key) == nullptr) {
      (void)object.Set(key, std::move(value));
    }
  };
  set("priority", JsonValue::MakeString("normal"));
  set("fairness", JsonValue::MakeString("stable_site_order"));
  set("all_or_nothing", JsonValue::MakeBool(false));
  set("require_single_source", JsonValue::MakeBool(false));
  set("as_of", JsonValue::MakeInt(0));
  set("allowed_regions", JsonValue::MakeArray());
  set("excluded_regions", JsonValue::MakeArray());
  set("allowed_jurisdictions", JsonValue::MakeArray());
  set("excluded_jurisdictions", JsonValue::MakeArray());
  set("allowed_sites", JsonValue::MakeArray());
  set("excluded_sites", JsonValue::MakeArray());
  set("excluded_failure_domains", JsonValue::MakeArray());
  set("min_distinct_failure_domains", JsonValue::MakeUint(0));
  set("max_sites", JsonValue::MakeUint(0));
  set("max_risk", JsonValue::MakeString("critical"));
  set("has_cost_ceiling", JsonValue::MakeBool(false));
  set("max_total_cost", JsonValue::MakeString("0"));
  set("has_energy_ceiling", JsonValue::MakeBool(false));
  set("max_total_energy_millijoules", JsonValue::MakeInt(0));
  set("has_carbon_ceiling", JsonValue::MakeBool(false));
  set("max_total_carbon_milligrams", JsonValue::MakeInt(0));
  set("may_consume_protected_reserve", JsonValue::MakeBool(false));
  set("reserve_policy_authorization", JsonValue::MakeString(""));
  set("has_policy_generation_pin", JsonValue::MakeBool(false));
  set("policy_generation_pin", JsonValue::MakeUint(0));
  set("generation_pins", JsonValue::MakeArray());
  set("require_current_generation", JsonValue::MakeBool(true));
}

void ApplyOfferDefaults(JsonValue& object) {
  const auto set = [&object](const char* key, JsonValue value) {
    if (object.Find(key) == nullptr) {
      (void)object.Set(key, std::move(value));
    }
  };
  set("policy_generation", JsonValue::MakeUint(0));
  set("reserve_minimum_priority", JsonValue::MakeString("critical"));
}

}  // namespace

Result<Offer> DecodeOfferJson(const JsonValue& input) {
  const Status members = CheckMembers(input, kOfferFields);
  if (!members.ok()) {
    return Fail<Offer>(members);
  }
  JsonValue value = input;
  ApplyOfferDefaults(value);
  Result<u64> schema = RequireUint(value, "schema");
  if (!schema.ok()) {
    return Result<Offer>(schema.status());
  }
  if (schema.value() != static_cast<u64>(kRequestSchemaVersion)) {
    return Fail<Offer>(ErrorCode::UnsupportedVersion,
                       "offer document schema " + std::to_string(schema.value()) +
                           " is not supported by this build");
  }
  Offer offer;
  Result<SiteId> site = RequireId<SiteIdTag>(value, "site");
  if (!site.ok()) return Result<Offer>(site.status());
  offer.site = site.value();
  Result<u64> generation = RequireUint(value, "generation");
  if (!generation.ok()) return Result<Offer>(generation.status());
  offer.generation = generation.value();
  Result<SnapshotId> snapshot = RequireId<SnapshotIdTag>(value, "source_snapshot");
  if (!snapshot.ok()) return Result<Offer>(snapshot.status());
  offer.source_snapshot = snapshot.value();
  Result<ServiceClassId> service_class = RequireId<ServiceClassIdTag>(value, "service_class");
  if (!service_class.ok()) return Result<Offer>(service_class.status());
  offer.service_class = service_class.value();
  Result<RegionId> region = RequireId<RegionIdTag>(value, "region");
  if (!region.ok()) return Result<Offer>(region.status());
  offer.region = region.value();
  Result<JurisdictionId> jurisdiction = RequireId<JurisdictionIdTag>(value, "jurisdiction");
  if (!jurisdiction.ok()) return Result<Offer>(jurisdiction.status());
  offer.jurisdiction = jurisdiction.value();
  Result<RiskTier> risk = DecodeRisk(value, "risk");
  if (!risk.ok()) return Result<Offer>(risk.status());
  offer.risk = risk.value();
  Result<i64> valid_from = RequireInt(value, "valid_from");
  if (!valid_from.ok()) return Result<Offer>(valid_from.status());
  offer.valid_from = valid_from.value();
  Result<i64> valid_until = RequireInt(value, "valid_until");
  if (!valid_until.ok()) return Result<Offer>(valid_until.status());
  offer.valid_until = valid_until.value();
  Result<PolicyId> reserve_policy = RequireId<PolicyIdTag>(value, "reserve_policy");
  if (!reserve_policy.ok()) return Result<Offer>(reserve_policy.status());
  offer.reserve_policy = reserve_policy.value();
  Result<u64> policy_generation = RequireUint(value, "policy_generation");
  if (!policy_generation.ok()) return Result<Offer>(policy_generation.status());
  offer.policy_generation = policy_generation.value();
  Result<PriorityClass> reserve_priority = DecodePriority(value, "reserve_minimum_priority");
  if (!reserve_priority.ok()) return Result<Offer>(reserve_priority.status());
  offer.reserve_minimum_priority = reserve_priority.value();
  Result<const JsonValue*> cost = Require(value, "cost");
  if (!cost.ok()) return Result<Offer>(cost.status());
  Result<CostEvidence> evidence = DecodeCostEvidence(*cost.value());
  if (!evidence.ok()) return Result<Offer>(evidence.status());
  offer.cost = evidence.value();
  Result<const JsonValue*> tranches = Require(value, "tranches");
  if (!tranches.ok()) return Result<Offer>(tranches.status());
  if (!tranches.value()->IsArray()) {
    return Fail<Offer>(ErrorCode::MalformedInput, "field 'tranches' must be an array");
  }
  if (tranches.value()->items().size() > Limits::kMaxTranchesPerOffer) {
    return Fail<Offer>(ErrorCode::LimitExceeded, "offer has too many tranches");
  }
  for (const JsonValue& item : tranches.value()->items()) {
    Result<CapacityTranche> tranche = DecodeCapacityTranche(item);
    if (!tranche.ok()) return Result<Offer>(tranche.status());
    offer.tranches.push_back(tranche.value());
  }
  Result<Offer> normalized = NormalizeOffer(std::move(offer));
  if (!normalized.ok()) return normalized;
  const Status valid = ValidateOffer(normalized.value());
  if (!valid.ok()) return Fail<Offer>(valid);
  return normalized;
}

Result<Ask> DecodeAskJson(const JsonValue& input, const FairnessPolicy default_fairness) {
  const Status members = CheckMembers(input, kAskFields);
  if (!members.ok()) {
    return Fail<Ask>(members);
  }
  JsonValue value = input;
  ApplyAskDefaults(value);
  Result<u64> schema = RequireUint(value, "schema");
  if (!schema.ok()) {
    return Result<Ask>(schema.status());
  }
  if (schema.value() != static_cast<u64>(kRequestSchemaVersion)) {
    return Fail<Ask>(ErrorCode::UnsupportedVersion,
                     "ask document schema " + std::to_string(schema.value()) +
                         " is not supported by this build");
  }
  Ask ask;
  Result<AskKey> key = RequireId<AskKeyTag>(value, "key");
  if (!key.ok()) return Result<Ask>(key.status());
  ask.key = key.value();
  Result<RequesterId> requester = RequireId<RequesterIdTag>(value, "requester");
  if (!requester.ok()) return Result<Ask>(requester.status());
  ask.requester = requester.value();
  Result<ServiceClassId> service_class = RequireId<ServiceClassIdTag>(value, "service_class");
  if (!service_class.ok()) return Result<Ask>(service_class.status());
  ask.service_class = service_class.value();
  Result<const JsonValue*> requested = Require(value, "requested");
  if (!requested.ok()) return Result<Ask>(requested.status());
  Result<CapacityVector> decoded_requested = DecodeCapacityVector(*requested.value());
  if (!decoded_requested.ok()) return Result<Ask>(decoded_requested.status());
  ask.requested = decoded_requested.value();
  Result<PriorityClass> priority = DecodePriority(value, "priority");
  if (!priority.ok()) return Result<Ask>(priority.status());
  ask.priority = priority.value();
  ask.fairness = default_fairness;
  if (value.Find("fairness") != nullptr) {
    Result<FairnessPolicy> fairness = DecodeFairness(value, "fairness");
    if (!fairness.ok()) return Result<Ask>(fairness.status());
    ask.fairness = fairness.value();
  }
  Result<bool> all_or_nothing = RequireBool(value, "all_or_nothing");
  if (!all_or_nothing.ok()) return Result<Ask>(all_or_nothing.status());
  ask.all_or_nothing = all_or_nothing.value();
  Result<bool> single_source = RequireBool(value, "require_single_source");
  if (!single_source.ok()) return Result<Ask>(single_source.status());
  ask.require_single_source = single_source.value();
  Result<i64> as_of = RequireInt(value, "as_of");
  if (!as_of.ok()) return Result<Ask>(as_of.status());
  ask.as_of = as_of.value();

  const std::pair<const char*, std::vector<RegionId>*> region_lists[] = {
      {"allowed_regions", &ask.allowed_regions}, {"excluded_regions", &ask.excluded_regions}};
  for (const auto& entry : region_lists) {
    Result<const JsonValue*> member = Require(value, entry.first);
    if (!member.ok()) return Result<Ask>(member.status());
    Result<std::vector<RegionId>> decoded = DecodeIdList<RegionIdTag>(*member.value(), entry.first);
    if (!decoded.ok()) return Result<Ask>(decoded.status());
    *entry.second = decoded.value();
  }
  const std::pair<const char*, std::vector<JurisdictionId>*> jurisdiction_lists[] = {
      {"allowed_jurisdictions", &ask.allowed_jurisdictions},
      {"excluded_jurisdictions", &ask.excluded_jurisdictions}};
  for (const auto& entry : jurisdiction_lists) {
    Result<const JsonValue*> member = Require(value, entry.first);
    if (!member.ok()) return Result<Ask>(member.status());
    Result<std::vector<JurisdictionId>> decoded =
        DecodeIdList<JurisdictionIdTag>(*member.value(), entry.first);
    if (!decoded.ok()) return Result<Ask>(decoded.status());
    *entry.second = decoded.value();
  }
  const std::pair<const char*, std::vector<SiteId>*> site_lists[] = {
      {"allowed_sites", &ask.allowed_sites}, {"excluded_sites", &ask.excluded_sites}};
  for (const auto& entry : site_lists) {
    Result<const JsonValue*> member = Require(value, entry.first);
    if (!member.ok()) return Result<Ask>(member.status());
    Result<std::vector<SiteId>> decoded = DecodeIdList<SiteIdTag>(*member.value(), entry.first);
    if (!decoded.ok()) return Result<Ask>(decoded.status());
    *entry.second = decoded.value();
  }
  Result<const JsonValue*> excluded_domains = Require(value, "excluded_failure_domains");
  if (!excluded_domains.ok()) return Result<Ask>(excluded_domains.status());
  Result<std::vector<FailureDomainId>> decoded_domains =
      DecodeIdList<FailureDomainIdTag>(*excluded_domains.value(), "excluded_failure_domains");
  if (!decoded_domains.ok()) return Result<Ask>(decoded_domains.status());
  ask.excluded_failure_domains = decoded_domains.value();

  Result<u64> min_domains = RequireUint(value, "min_distinct_failure_domains");
  if (!min_domains.ok()) return Result<Ask>(min_domains.status());
  if (min_domains.value() > static_cast<u64>(std::numeric_limits<u32>::max())) {
    return Fail<Ask>(ErrorCode::OutOfRange, "min_distinct_failure_domains is too large");
  }
  ask.min_distinct_failure_domains = static_cast<u32>(min_domains.value());
  Result<u64> max_sites = RequireUint(value, "max_sites");
  if (!max_sites.ok()) return Result<Ask>(max_sites.status());
  if (max_sites.value() > static_cast<u64>(std::numeric_limits<u32>::max())) {
    return Fail<Ask>(ErrorCode::OutOfRange, "max_sites is too large");
  }
  ask.max_sites = static_cast<u32>(max_sites.value());
  Result<RiskTier> max_risk = DecodeRisk(value, "max_risk");
  if (!max_risk.ok()) return Result<Ask>(max_risk.status());
  ask.max_risk = max_risk.value();

  Result<bool> has_cost = RequireBool(value, "has_cost_ceiling");
  if (!has_cost.ok()) return Result<Ask>(has_cost.status());
  ask.has_cost_ceiling = has_cost.value();
  Result<const JsonValue*> max_cost = Require(value, "max_total_cost");
  if (!max_cost.ok()) return Result<Ask>(max_cost.status());
  Result<ScaledAmount> decoded_cost = DecodeScaledAmount(*max_cost.value(), "max_total_cost");
  if (!decoded_cost.ok()) return Result<Ask>(decoded_cost.status());
  ask.max_total_cost = decoded_cost.value();
  Result<bool> has_energy = RequireBool(value, "has_energy_ceiling");
  if (!has_energy.ok()) return Result<Ask>(has_energy.status());
  ask.has_energy_ceiling = has_energy.value();
  Result<i64> max_energy = RequireInt(value, "max_total_energy_millijoules");
  if (!max_energy.ok()) return Result<Ask>(max_energy.status());
  ask.max_total_energy_millijoules = max_energy.value();
  Result<bool> has_carbon = RequireBool(value, "has_carbon_ceiling");
  if (!has_carbon.ok()) return Result<Ask>(has_carbon.status());
  ask.has_carbon_ceiling = has_carbon.value();
  Result<i64> max_carbon = RequireInt(value, "max_total_carbon_milligrams");
  if (!max_carbon.ok()) return Result<Ask>(max_carbon.status());
  ask.max_total_carbon_milligrams = max_carbon.value();

  Result<bool> may_consume = RequireBool(value, "may_consume_protected_reserve");
  if (!may_consume.ok()) return Result<Ask>(may_consume.status());
  ask.may_consume_protected_reserve = may_consume.value();
  Result<PolicyId> authorization = OptionalId<PolicyIdTag>(value, "reserve_policy_authorization");
  if (!authorization.ok()) return Result<Ask>(authorization.status());
  ask.reserve_policy_authorization = authorization.value();
  Result<bool> has_policy_pin = RequireBool(value, "has_policy_generation_pin");
  if (!has_policy_pin.ok()) return Result<Ask>(has_policy_pin.status());
  ask.has_policy_generation_pin = has_policy_pin.value();
  Result<u64> policy_pin = RequireUint(value, "policy_generation_pin");
  if (!policy_pin.ok()) return Result<Ask>(policy_pin.status());
  ask.policy_generation_pin = policy_pin.value();

  Result<const JsonValue*> pins = Require(value, "generation_pins");
  if (!pins.ok()) return Result<Ask>(pins.status());
  if (!pins.value()->IsArray()) {
    return Fail<Ask>(ErrorCode::MalformedInput, "field 'generation_pins' must be an array");
  }
  if (pins.value()->items().size() > Limits::kMaxGenerationPins) {
    return Fail<Ask>(ErrorCode::LimitExceeded, "ask has too many generation pins");
  }
  for (const JsonValue& item : pins.value()->items()) {
    const Status pin_members = CheckMembers(item, {"site", "generation"});
    if (!pin_members.ok()) return Fail<Ask>(pin_members);
    GenerationPin pin;
    Result<SiteId> pin_site = RequireId<SiteIdTag>(item, "site");
    if (!pin_site.ok()) return Result<Ask>(pin_site.status());
    pin.site = pin_site.value();
    Result<u64> pin_generation = RequireUint(item, "generation");
    if (!pin_generation.ok()) return Result<Ask>(pin_generation.status());
    pin.generation = pin_generation.value();
    ask.generation_pins.push_back(pin);
  }
  std::sort(ask.generation_pins.begin(), ask.generation_pins.end(),
            [](const GenerationPin& a, const GenerationPin& b) { return a.site < b.site; });
  Result<bool> require_current = RequireBool(value, "require_current_generation");
  if (!require_current.ok()) return Result<Ask>(require_current.status());
  ask.require_current_generation = require_current.value();

  const Status valid = ValidateAsk(ask);
  if (!valid.ok()) return Fail<Ask>(valid);
  return ask;
}

Result<CommitmentRecord> DecodeCommitmentJson(const JsonValue& value) {
  const Status members = CheckMembers(value, kCommitmentFields);
  if (!members.ok()) {
    return Fail<CommitmentRecord>(members);
  }
  CommitmentRecord record;
  Result<CommitmentId> id = RequireId<CommitmentIdTag>(value, "id");
  if (!id.ok()) return Result<CommitmentRecord>(id.status());
  record.id = id.value();
  Result<AskKey> key = RequireId<AskKeyTag>(value, "ask_key");
  if (!key.ok()) return Result<CommitmentRecord>(key.status());
  record.ask_key = key.value();
  Result<DecisionId> decision = RequireId<DecisionIdTag>(value, "decision_id");
  if (!decision.ok()) return Result<CommitmentRecord>(decision.status());
  record.decision_id = decision.value();
  Result<RequesterId> requester = RequireId<RequesterIdTag>(value, "requester");
  if (!requester.ok()) return Result<CommitmentRecord>(requester.status());
  record.requester = requester.value();
  Result<SiteId> site = RequireId<SiteIdTag>(value, "site");
  if (!site.ok()) return Result<CommitmentRecord>(site.status());
  record.site = site.value();
  Result<u64> created_generation = RequireUint(value, "created_generation");
  if (!created_generation.ok()) return Result<CommitmentRecord>(created_generation.status());
  record.created_generation = created_generation.value();
  Result<u64> accounted_generation = RequireUint(value, "accounted_generation");
  if (!accounted_generation.ok()) return Result<CommitmentRecord>(accounted_generation.status());
  record.accounted_generation = accounted_generation.value();
  Result<FailureDomainId> domain = RequireId<FailureDomainIdTag>(value, "domain");
  if (!domain.ok()) return Result<CommitmentRecord>(domain.status());
  record.domain = domain.value();
  Result<ServiceClassId> service_class = RequireId<ServiceClassIdTag>(value, "service_class");
  if (!service_class.ok()) return Result<CommitmentRecord>(service_class.status());
  record.service_class = service_class.value();
  Result<const JsonValue*> allocatable = Require(value, "allocatable_amount");
  if (!allocatable.ok()) return Result<CommitmentRecord>(allocatable.status());
  Result<CapacityVector> decoded_allocatable = DecodeCapacityVector(*allocatable.value());
  if (!decoded_allocatable.ok()) return Result<CommitmentRecord>(decoded_allocatable.status());
  record.allocatable_amount = decoded_allocatable.value();
  Result<const JsonValue*> reserve = Require(value, "reserve_amount");
  if (!reserve.ok()) return Result<CommitmentRecord>(reserve.status());
  Result<CapacityVector> decoded_reserve = DecodeCapacityVector(*reserve.value());
  if (!decoded_reserve.ok()) return Result<CommitmentRecord>(decoded_reserve.status());
  record.reserve_amount = decoded_reserve.value();
  Result<const JsonValue*> cost = Require(value, "cost");
  if (!cost.ok()) return Result<CommitmentRecord>(cost.status());
  Result<ScaledAmount> decoded_cost = DecodeScaledAmount(*cost.value(), "cost");
  if (!decoded_cost.ok()) return Result<CommitmentRecord>(decoded_cost.status());
  record.cost = decoded_cost.value();
  Result<i64> energy = RequireInt(value, "energy_millijoules");
  if (!energy.ok()) return Result<CommitmentRecord>(energy.status());
  record.energy_millijoules = energy.value();
  Result<i64> carbon = RequireInt(value, "carbon_milligrams");
  if (!carbon.ok()) return Result<CommitmentRecord>(carbon.status());
  record.carbon_milligrams = carbon.value();
  Result<PriorityClass> priority = DecodePriority(value, "priority");
  if (!priority.ok()) return Result<CommitmentRecord>(priority.status());
  record.priority = priority.value();
  Result<u64> sequence = RequireUint(value, "sequence");
  if (!sequence.ok()) return Result<CommitmentRecord>(sequence.status());
  record.sequence = sequence.value();
  Result<i64> created_at = RequireInt(value, "created_at");
  if (!created_at.ok()) return Result<CommitmentRecord>(created_at.status());
  record.created_at = created_at.value();
  Result<bool> live = RequireBool(value, "live");
  if (!live.ok()) return Result<CommitmentRecord>(live.status());
  record.live = live.value();
  Result<bool> has_revocation = RequireBool(value, "has_revocation");
  if (!has_revocation.ok()) return Result<CommitmentRecord>(has_revocation.status());
  record.has_revocation = has_revocation.value();
  Result<RevocationReason> reason = DecodeRevocationReason(value, "revocation_reason");
  if (!reason.ok()) return Result<CommitmentRecord>(reason.status());
  record.revocation_reason = reason.value();
  Result<u64> revoked_sequence = RequireUint(value, "revoked_sequence");
  if (!revoked_sequence.ok()) return Result<CommitmentRecord>(revoked_sequence.status());
  record.revoked_sequence = revoked_sequence.value();
  Result<i64> revoked_at = RequireInt(value, "revoked_at");
  if (!revoked_at.ok()) return Result<CommitmentRecord>(revoked_at.status());
  record.revoked_at = revoked_at.value();
  if (record.id.empty() || record.sequence == 0) {
    return Fail<CommitmentRecord>(ErrorCode::PersistenceCorrupt,
                                  "a commitment record needs an identity and a sequence");
  }
  return record;
}

Result<SiteLedger> DecodeSiteLedgerJson(const JsonValue& value) {
  const Status members = CheckMembers(value, kSiteLedgerFields);
  if (!members.ok()) {
    return Fail<SiteLedger>(members);
  }
  SiteLedger ledger;
  Result<SiteId> site = RequireId<SiteIdTag>(value, "site");
  if (!site.ok()) return Result<SiteLedger>(site.status());
  ledger.site = site.value();
  Result<u64> generation = RequireUint(value, "generation");
  if (!generation.ok()) return Result<SiteLedger>(generation.status());
  ledger.generation = generation.value();
  Result<SnapshotId> snapshot = RequireId<SnapshotIdTag>(value, "source_snapshot");
  if (!snapshot.ok()) return Result<SiteLedger>(snapshot.status());
  ledger.source_snapshot = snapshot.value();
  Result<ServiceClassId> service_class = RequireId<ServiceClassIdTag>(value, "service_class");
  if (!service_class.ok()) return Result<SiteLedger>(service_class.status());
  ledger.service_class = service_class.value();
  Result<RegionId> region = RequireId<RegionIdTag>(value, "region");
  if (!region.ok()) return Result<SiteLedger>(region.status());
  ledger.region = region.value();
  Result<JurisdictionId> jurisdiction = RequireId<JurisdictionIdTag>(value, "jurisdiction");
  if (!jurisdiction.ok()) return Result<SiteLedger>(jurisdiction.status());
  ledger.jurisdiction = jurisdiction.value();
  Result<RiskTier> risk = DecodeRisk(value, "risk");
  if (!risk.ok()) return Result<SiteLedger>(risk.status());
  ledger.risk = risk.value();
  Result<i64> valid_from = RequireInt(value, "valid_from");
  if (!valid_from.ok()) return Result<SiteLedger>(valid_from.status());
  ledger.valid_from = valid_from.value();
  Result<i64> valid_until = RequireInt(value, "valid_until");
  if (!valid_until.ok()) return Result<SiteLedger>(valid_until.status());
  ledger.valid_until = valid_until.value();
  Result<PolicyId> policy = RequireId<PolicyIdTag>(value, "reserve_policy");
  if (!policy.ok()) return Result<SiteLedger>(policy.status());
  ledger.reserve_policy = policy.value();
  Result<u64> policy_generation = RequireUint(value, "policy_generation");
  if (!policy_generation.ok()) return Result<SiteLedger>(policy_generation.status());
  ledger.policy_generation = policy_generation.value();
  Result<PriorityClass> reserve_priority = DecodePriority(value, "reserve_minimum_priority");
  if (!reserve_priority.ok()) return Result<SiteLedger>(reserve_priority.status());
  ledger.reserve_minimum_priority = reserve_priority.value();
  Result<const JsonValue*> cost = Require(value, "cost");
  if (!cost.ok()) return Result<SiteLedger>(cost.status());
  Result<CostEvidence> evidence = DecodeCostEvidence(*cost.value());
  if (!evidence.ok()) return Result<SiteLedger>(evidence.status());
  ledger.cost = evidence.value();
  Result<bool> revoked = RequireBool(value, "revoked");
  if (!revoked.ok()) return Result<SiteLedger>(revoked.status());
  ledger.revoked = revoked.value();
  Result<RevocationReason> reason = DecodeRevocationReason(value, "revocation_reason");
  if (!reason.ok()) return Result<SiteLedger>(reason.status());
  ledger.revocation_reason = reason.value();
  Result<u64> revoked_sequence = RequireUint(value, "revoked_sequence");
  if (!revoked_sequence.ok()) return Result<SiteLedger>(revoked_sequence.status());
  ledger.revoked_sequence = revoked_sequence.value();
  Result<i64> revoked_at = RequireInt(value, "revoked_at");
  if (!revoked_at.ok()) return Result<SiteLedger>(revoked_at.status());
  ledger.revoked_at = revoked_at.value();
  Result<u64> published_sequence = RequireUint(value, "published_sequence");
  if (!published_sequence.ok()) return Result<SiteLedger>(published_sequence.status());
  ledger.published_sequence = published_sequence.value();

  Result<const JsonValue*> tranches = Require(value, "tranches");
  if (!tranches.ok()) return Result<SiteLedger>(tranches.status());
  if (!tranches.value()->IsArray()) {
    return Fail<SiteLedger>(ErrorCode::PersistenceCorrupt, "tranches must be an array");
  }
  for (const JsonValue& item : tranches.value()->items()) {
    Result<TrancheLedger> tranche = DecodeTrancheLedger(item);
    if (!tranche.ok()) return Result<SiteLedger>(tranche.status());
    ledger.tranches.push_back(tranche.value());
  }
  Result<const JsonValue*> commitments = Require(value, "commitments");
  if (!commitments.ok()) return Result<SiteLedger>(commitments.status());
  if (!commitments.value()->IsArray()) {
    return Fail<SiteLedger>(ErrorCode::PersistenceCorrupt, "commitments must be an array");
  }
  for (const JsonValue& item : commitments.value()->items()) {
    Result<CommitmentRecord> commitment = DecodeCommitmentJson(item);
    if (!commitment.ok()) return Result<SiteLedger>(commitment.status());
    ledger.commitments.push_back(commitment.value());
  }
  return ledger;
}

Result<Decision> DecodeDecisionJson(const JsonValue& value) {
  const Status members = CheckMembers(value, kDecisionFields);
  if (!members.ok()) {
    return Fail<Decision>(members);
  }
  Decision decision;
  Result<DecisionId> id = RequireId<DecisionIdTag>(value, "id");
  if (!id.ok()) return Result<Decision>(id.status());
  decision.id = id.value();
  Result<AskKey> key = RequireId<AskKeyTag>(value, "key");
  if (!key.ok()) return Result<Decision>(key.status());
  decision.key = key.value();
  Result<RequesterId> requester = RequireId<RequesterIdTag>(value, "requester");
  if (!requester.ok()) return Result<Decision>(requester.status());
  decision.requester = requester.value();
  Result<ServiceClassId> service_class = RequireId<ServiceClassIdTag>(value, "service_class");
  if (!service_class.ok()) return Result<Decision>(service_class.status());
  decision.service_class = service_class.value();
  Result<DecisionOutcome> outcome = DecodeOutcome(value, "outcome");
  if (!outcome.ok()) return Result<Decision>(outcome.status());
  decision.outcome = outcome.value();

  Result<const JsonValue*> allocations = Require(value, "allocations");
  if (!allocations.ok()) return Result<Decision>(allocations.status());
  if (!allocations.value()->IsArray()) {
    return Fail<Decision>(ErrorCode::PersistenceCorrupt, "allocations must be an array");
  }
  if (allocations.value()->items().size() > Limits::kMaxAllocationsPerDecision) {
    return Fail<Decision>(ErrorCode::LimitExceeded, "decision has too many allocations");
  }
  for (const JsonValue& item : allocations.value()->items()) {
    const Status item_members = CheckMembers(item, kAllocationFields);
    if (!item_members.ok()) return Fail<Decision>(item_members);
    Allocation allocation;
    Result<CommitmentId> allocation_id = RequireId<CommitmentIdTag>(item, "id");
    if (!allocation_id.ok()) return Result<Decision>(allocation_id.status());
    allocation.id = allocation_id.value();
    Result<SiteId> site = RequireId<SiteIdTag>(item, "site");
    if (!site.ok()) return Result<Decision>(site.status());
    allocation.site = site.value();
    Result<u64> generation = RequireUint(item, "generation");
    if (!generation.ok()) return Result<Decision>(generation.status());
    allocation.generation = generation.value();
    Result<FailureDomainId> domain = RequireId<FailureDomainIdTag>(item, "domain");
    if (!domain.ok()) return Result<Decision>(domain.status());
    allocation.domain = domain.value();
    Result<ServiceClassId> allocation_class = RequireId<ServiceClassIdTag>(item, "service_class");
    if (!allocation_class.ok()) return Result<Decision>(allocation_class.status());
    allocation.service_class = allocation_class.value();
    Result<const JsonValue*> allocatable = Require(item, "allocatable_amount");
    if (!allocatable.ok()) return Result<Decision>(allocatable.status());
    Result<CapacityVector> decoded_allocatable = DecodeCapacityVector(*allocatable.value());
    if (!decoded_allocatable.ok()) return Result<Decision>(decoded_allocatable.status());
    allocation.allocatable_amount = decoded_allocatable.value();
    Result<const JsonValue*> reserve = Require(item, "reserve_amount");
    if (!reserve.ok()) return Result<Decision>(reserve.status());
    Result<CapacityVector> decoded_reserve = DecodeCapacityVector(*reserve.value());
    if (!decoded_reserve.ok()) return Result<Decision>(decoded_reserve.status());
    allocation.reserve_amount = decoded_reserve.value();
    Result<const JsonValue*> cost = Require(item, "cost");
    if (!cost.ok()) return Result<Decision>(cost.status());
    Result<ScaledAmount> decoded_cost = DecodeScaledAmount(*cost.value(), "cost");
    if (!decoded_cost.ok()) return Result<Decision>(decoded_cost.status());
    allocation.cost = decoded_cost.value();
    Result<i64> energy = RequireInt(item, "energy_millijoules");
    if (!energy.ok()) return Result<Decision>(energy.status());
    allocation.energy_millijoules = energy.value();
    Result<i64> carbon = RequireInt(item, "carbon_milligrams");
    if (!carbon.ok()) return Result<Decision>(carbon.status());
    allocation.carbon_milligrams = carbon.value();
    Result<PriorityClass> priority = DecodePriority(item, "priority");
    if (!priority.ok()) return Result<Decision>(priority.status());
    allocation.priority = priority.value();
    Result<u64> sequence = RequireUint(item, "sequence");
    if (!sequence.ok()) return Result<Decision>(sequence.status());
    allocation.sequence = sequence.value();
    Result<i64> created_at = RequireInt(item, "created_at");
    if (!created_at.ok()) return Result<Decision>(created_at.status());
    allocation.created_at = created_at.value();
    decision.allocations.push_back(allocation);
  }

  Result<const JsonValue*> blocking = Require(value, "blocking");
  if (!blocking.ok()) return Result<Decision>(blocking.status());
  if (!blocking.value()->IsArray()) {
    return Fail<Decision>(ErrorCode::PersistenceCorrupt, "blocking must be an array");
  }
  for (const JsonValue& item : blocking.value()->items()) {
    const Status item_members = CheckMembers(
        item, {"kind", "site", "domain", "has_dimension", "dimension", "required", "available",
               "detail"});
    if (!item_members.ok()) return Fail<Decision>(item_members);
    BlockingConstraint constraint;
    Result<ConstraintKind> kind = DecodeConstraintKind(item, "kind");
    if (!kind.ok()) return Result<Decision>(kind.status());
    constraint.kind = kind.value();
    Result<SiteId> site = OptionalId<SiteIdTag>(item, "site");
    if (!site.ok()) return Result<Decision>(site.status());
    constraint.site = site.value();
    Result<FailureDomainId> domain = OptionalId<FailureDomainIdTag>(item, "domain");
    if (!domain.ok()) return Result<Decision>(domain.status());
    constraint.domain = domain.value();
    Result<bool> has_dimension = RequireBool(item, "has_dimension");
    if (!has_dimension.ok()) return Result<Decision>(has_dimension.status());
    constraint.has_dimension = has_dimension.value();
    Result<std::string> dimension_text = RequireString(item, "dimension");
    if (!dimension_text.ok()) return Result<Decision>(dimension_text.status());
    Result<Dimension> dimension = ParseDimension(dimension_text.value());
    if (!dimension.ok()) return Result<Decision>(dimension.status());
    constraint.dimension = dimension.value();
    Result<i64> required = RequireInt(item, "required");
    if (!required.ok()) return Result<Decision>(required.status());
    constraint.required = required.value();
    Result<i64> available = RequireInt(item, "available");
    if (!available.ok()) return Result<Decision>(available.status());
    constraint.available = available.value();
    Result<std::string> detail = OptionalString(item, "detail");
    if (!detail.ok()) return Result<Decision>(detail.status());
    constraint.detail = detail.value();
    decision.blocking.push_back(std::move(constraint));
  }

  const std::pair<const char*, CapacityVector*> vectors[] = {
      {"requested", &decision.requested},
      {"committed", &decision.committed},
      {"unmet", &decision.unmet}};
  for (const auto& entry : vectors) {
    Result<const JsonValue*> member = Require(value, entry.first);
    if (!member.ok()) return Result<Decision>(member.status());
    Result<CapacityVector> decoded = DecodeCapacityVector(*member.value());
    if (!decoded.ok()) return Result<Decision>(decoded.status());
    *entry.second = decoded.value();
  }
  Result<const JsonValue*> total_cost = Require(value, "total_cost");
  if (!total_cost.ok()) return Result<Decision>(total_cost.status());
  Result<ScaledAmount> decoded_total_cost = DecodeScaledAmount(*total_cost.value(), "total_cost");
  if (!decoded_total_cost.ok()) return Result<Decision>(decoded_total_cost.status());
  decision.total_cost = decoded_total_cost.value();

  Result<i64> total_energy = RequireInt(value, "total_energy_millijoules");
  if (!total_energy.ok()) return Result<Decision>(total_energy.status());
  decision.total_energy_millijoules = total_energy.value();
  Result<i64> total_carbon = RequireInt(value, "total_carbon_milligrams");
  if (!total_carbon.ok()) return Result<Decision>(total_carbon.status());
  decision.total_carbon_milligrams = total_carbon.value();
  Result<u64> distinct_domains = RequireUint(value, "distinct_failure_domains");
  if (!distinct_domains.ok()) return Result<Decision>(distinct_domains.status());
  decision.distinct_failure_domains = static_cast<u32>(distinct_domains.value());
  Result<u64> distinct_sites = RequireUint(value, "distinct_sites");
  if (!distinct_sites.ok()) return Result<Decision>(distinct_sites.status());
  decision.distinct_sites = static_cast<u32>(distinct_sites.value());
  Result<u64> broker_sequence = RequireUint(value, "broker_sequence");
  if (!broker_sequence.ok()) return Result<Decision>(broker_sequence.status());
  decision.broker_sequence = broker_sequence.value();
  Result<u64> broker_epoch = RequireUint(value, "broker_epoch");
  if (!broker_epoch.ok()) return Result<Decision>(broker_epoch.status());
  decision.broker_epoch = broker_epoch.value();
  Result<i64> decided_at = RequireInt(value, "decided_at");
  if (!decided_at.ok()) return Result<Decision>(decided_at.status());
  decision.decided_at = decided_at.value();
  Result<DurabilityClass> durability = DecodeDurability(value, "durability");
  if (!durability.ok()) return Result<Decision>(durability.status());
  decision.durability = durability.value();
  Result<bool> replay = RequireBool(value, "replay");
  if (!replay.ok()) return Result<Decision>(replay.status());
  decision.replay = replay.value();
  Result<std::string> accounting = RequireString(value, "accounting_digest");
  if (!accounting.ok()) return Result<Decision>(accounting.status());
  Result<Digest> accounting_digest = Digest::FromHex(accounting.value());
  if (!accounting_digest.ok()) return Result<Decision>(accounting_digest.status());
  decision.accounting_digest = accounting_digest.value();
  Result<std::string> request_digest = RequireString(value, "request_digest");
  if (!request_digest.ok()) return Result<Decision>(request_digest.status());
  Result<Digest> decoded_request_digest = Digest::FromHex(request_digest.value());
  if (!decoded_request_digest.ok()) return Result<Decision>(decoded_request_digest.status());
  decision.request_digest = decoded_request_digest.value();
  return decision;
}

Status RestoreStateFromJson(const JsonValue& root, BrokerCore& core) {
  const Status members = CheckMembers(root, kStateFields);
  if (!members.ok()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, members.ToString());
  }
  Result<u64> format = RequireUint(root, "format");
  if (!format.ok()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, format.status().ToString());
  }
  if (format.value() != static_cast<u64>(kPersistenceFormatVersion)) {
    return Status::Error(ErrorCode::UnsupportedVersion,
                         "state snapshot format " + std::to_string(format.value()) +
                             " is not supported by this build");
  }
  Result<const JsonValue*> config = Require(root, "config");
  if (!config.ok()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, config.status().ToString());
  }
  const Status config_members = CheckMembers(*config.value(), kConfigFields);
  if (!config_members.ok()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, config_members.ToString());
  }
  BrokerConfig decoded_config;
  Result<std::string> shrink = RequireString(*config.value(), "shrink_policy");
  if (!shrink.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, shrink.status().ToString());
  Result<ShrinkPolicy> shrink_policy = ParseShrinkPolicy(shrink.value());
  if (!shrink_policy.ok()) {
    return Status::Error(shrink_policy.status().code(), shrink_policy.status().ToString());
  }
  decoded_config.shrink_policy = shrink_policy.value();
  Result<std::string> fairness = RequireString(*config.value(), "default_fairness");
  if (!fairness.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, fairness.status().ToString());
  Result<FairnessPolicy> fairness_policy = ParseFairness(fairness.value());
  if (!fairness_policy.ok()) {
    return Status::Error(fairness_policy.status().code(), fairness_policy.status().ToString());
  }
  decoded_config.default_fairness = fairness_policy.value();
  Result<u64> retained = RequireUint(*config.value(), "max_retained_decisions");
  if (!retained.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, retained.status().ToString());
  decoded_config.max_retained_decisions = static_cast<std::size_t>(retained.value());
  Result<u64> blocking = RequireUint(*config.value(), "max_blocking_constraints");
  if (!blocking.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, blocking.status().ToString());
  decoded_config.max_blocking_constraints = static_cast<std::size_t>(blocking.value());
  Result<u64> history = RequireUint(*config.value(), "generation_history");
  if (!history.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, history.status().ToString());
  decoded_config.generation_history = static_cast<std::size_t>(history.value());

  Result<u64> epoch = RequireUint(root, "epoch");
  if (!epoch.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, epoch.status().ToString());
  Result<u64> sequence = RequireUint(root, "sequence");
  if (!sequence.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, sequence.status().ToString());
  Result<u64> state_version = RequireUint(root, "state_version");
  if (!state_version.ok()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, state_version.status().ToString());
  }
  Result<std::string> chain = RequireString(root, "chain_digest");
  if (!chain.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, chain.status().ToString());
  Result<Digest> chain_digest = Digest::FromHex(chain.value());
  if (!chain_digest.ok()) {
    return Status::Error(chain_digest.status().code(), chain_digest.status().ToString());
  }

  const Status restored_config = core.RestoreConfig(decoded_config);
  if (!restored_config.ok()) {
    return restored_config;
  }
  core.SetEpoch(epoch.value());
  core.SetSequence(sequence.value());
  core.SetChainDigest(chain_digest.value());

  Result<const JsonValue*> sites = Require(root, "sites");
  if (!sites.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, sites.status().ToString());
  if (!sites.value()->IsArray()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, "sites must be an array");
  }
  if (sites.value()->items().size() > Limits::kMaxSites) {
    return Status::Error(ErrorCode::LimitExceeded, "snapshot holds more sites than the limit");
  }
  for (const JsonValue& item : sites.value()->items()) {
    const Status site_members = CheckMembers(item, kSiteStateFields);
    if (!site_members.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt, site_members.ToString());
    }
    Result<const JsonValue*> ledger = Require(item, "ledger");
    if (!ledger.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, ledger.status().ToString());
    Result<SiteLedger> decoded = DecodeSiteLedgerJson(*ledger.value());
    if (!decoded.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "site ledger: " + decoded.status().ToString());
    }
    Result<const JsonValue*> history_member = Require(item, "history");
    if (!history_member.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt, history_member.status().ToString());
    }
    if (!history_member.value()->IsArray()) {
      return Status::Error(ErrorCode::PersistenceCorrupt, "history must be an array");
    }
    std::vector<GenerationHistoryEntry> entries;
    for (const JsonValue& entry : history_member.value()->items()) {
      const Status entry_members = CheckMembers(entry, kHistoryFields);
      if (!entry_members.ok()) {
        return Status::Error(ErrorCode::PersistenceCorrupt, entry_members.ToString());
      }
      GenerationHistoryEntry history_entry;
      Result<u64> generation = RequireUint(entry, "generation");
      if (!generation.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, generation.status().ToString());
      history_entry.generation = generation.value();
      Result<u64> entry_sequence = RequireUint(entry, "sequence");
      if (!entry_sequence.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, entry_sequence.status().ToString());
      history_entry.sequence = entry_sequence.value();
      Result<i64> at = RequireInt(entry, "at");
      if (!at.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, at.status().ToString());
      history_entry.at = at.value();
      Result<RevocationReason> reason = DecodeRevocationReason(entry, "reason");
      if (!reason.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, reason.status().ToString());
      history_entry.reason = reason.value();
      Result<u64> revoked = RequireUint(entry, "commitments_revoked");
      if (!revoked.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, revoked.status().ToString());
      history_entry.commitments_revoked = static_cast<std::size_t>(revoked.value());
      entries.push_back(history_entry);
    }
    const Status adopted = core.RestoreSiteLedger(decoded.value(), std::move(entries));
    if (!adopted.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "restoring a site ledger failed: " + adopted.ToString());
    }
  }

  Result<const JsonValue*> decisions = Require(root, "decisions");
  if (!decisions.ok()) return Status::Error(ErrorCode::PersistenceCorrupt, decisions.status().ToString());
  if (!decisions.value()->IsArray()) {
    return Status::Error(ErrorCode::PersistenceCorrupt, "decisions must be an array");
  }
  for (const JsonValue& item : decisions.value()->items()) {
    Result<Decision> decision = DecodeDecisionJson(item);
    if (!decision.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "decision record: " + decision.status().ToString());
    }
    Status adopted = core.RestoreDecisionRecord(decision.value());
    if (!adopted.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "restoring a decision failed: " + adopted.ToString());
    }
    adopted = core.VerifyDecisionProvenance(decision.value());
    if (!adopted.ok()) {
      return Status::Error(ErrorCode::PersistenceCorrupt,
                           "decision provenance: " + adopted.ToString());
    }
  }

  core.SetStateVersion(state_version.value());
  const ConservationReport report = core.VerifyConservation();
  if (!report.closed) {
    std::string detail = "the restored state does not satisfy its conservation identity";
    if (!report.violations.empty()) {
      detail += ": ";
      detail += report.violations.front().scope;
      detail += " ";
      detail += report.violations.front().detail;
    }
    return Status::Error(ErrorCode::PersistenceCorrupt, std::move(detail));
  }
  return Status::Ok();
}

}  // namespace rcb
