// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Adversarial suite.
//
// Every test here attacks one edge of the runtime boundary and pins the exact
// refusal: the exact ErrorCode, the exact bytes that were left behind, and the
// exact state of a ledger that must not have moved. "It did not crash" is not a
// result. A runtime that silently repairs, silently defaults or silently
// ignores an attack is a defect report, not a pass, so every case below asserts
// what the boundary did about it.
//
//   A. document attacks on DecodeOfferJson / DecodeAskJson, each with the exact
//      code and a ledger probe that must still equal a freshly built core;
//   B. journal framing attacks: one mutated byte at a time, with the exact
//      recovery outcome and the exact post-repair log size;
//   C. injected I/O failures at every fail point the memory adapters expose;
//   D. store-level attacks: missing files, oversized files, double locks,
//      directories where files are and files where directories are;
//   E. large-input bounds, refused before anything is allocated;
//   F. identifier and UTF-8 edges.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

namespace {

using rcb::Bytes;
using rcb::ErrorCode;
using rcb::i64;
using rcb::u32;
using rcb::u64;
using rcb::u8;

// ---- assertions ----------------------------------------------------------

/// Declares a probe ledger, asserts that the call is refused with the exact
/// code, and asserts that the probe is still byte-identical to a core that was
/// built from nothing: sequence 0, same state digest, conservation closed.
#define RCB_CHECK_REFUSED_AND_INERT(call, expected_code)                          \
  do {                                                                            \
    ::rcb::BrokerCore rcb_probe_core;                                             \
    const ::rcb::Digest rcb_probe_before = rcb_probe_core.StateDigest();          \
    const auto& rcb_probe_result = (call);                                        \
    RCB_CHECK_ERROR(rcb_probe_result, expected_code);                             \
    RCB_CHECK_EQ(rcb_probe_core.sequence(), ::rcb::u64{0});                       \
    RCB_CHECK(rcb_probe_core.StateDigest() == rcb_probe_before);                  \
    RCB_CHECK(rcb_probe_core.StateDigest() == ::rcb::BrokerCore().StateDigest()); \
    RCB_CHECK(rcb_probe_core.VerifyConservation().closed);                        \
    RCB_CHECK(rcb_probe_core.RecentDecisions(8).empty());                         \
  } while (false)

/// The same, against a ledger that already holds state and must not move.
#define RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, call, expected_code) \
  do {                                                                \
    const ::rcb::Digest rcb_ledger_before = (core).StateDigest();     \
    const ::rcb::u64 rcb_ledger_sequence = (core).sequence();         \
    const auto& rcb_ledger_result = (call);                           \
    RCB_CHECK_ERROR(rcb_ledger_result, expected_code);                \
    RCB_CHECK_EQ((core).sequence(), rcb_ledger_sequence);             \
    RCB_CHECK((core).StateDigest() == rcb_ledger_before);             \
    RCB_CHECK((core).VerifyConservation().closed);                    \
  } while (false)

/// Asserts that a bare Status (not a Result) carries the expected error code.
#define RCB_CHECK_STATUS(status_value, expected_code)                                \
  do {                                                                               \
    const ::rcb::Status& rcb_status_value = (status_value);                          \
    ++::rcbtest::CheckCount();                                                       \
    if (rcb_status_value.ok()) {                                                     \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                                   \
                               "expected failure " #expected_code                    \
                               " but the call succeeded");                           \
    } else if (rcb_status_value.code() != (expected_code)) {                         \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                                   \
                               std::string("expected failure " #expected_code        \
                                           " but got ") +                            \
                                   std::string(rcb_status_value.token()) + ": " +    \
                                   rcb_status_value.detail());                       \
    }                                                                                \
  } while (false)

/// The harness's success requirement materialises a copy of the result, which a
/// Result holding a unique_ptr cannot provide. This one inspects the caller's
/// own object and never copies it.
#define RCB_REQUIRE_OK_REF(result)                                                  \
  do {                                                                              \
    ++::rcbtest::CheckCount();                                                      \
    if (!(result).ok()) {                                                           \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                                  \
                               std::string("expected success but got ") +           \
                                   (result).status().ToString());                   \
      return;                                                                       \
    }                                                                               \
  } while (false)

// ---- fixture builders ----------------------------------------------------

template <class Tag>
rcb::TaggedId<Tag> Id(const std::string& text) {
  rcb::Result<rcb::TaggedId<Tag>> made = rcb::TaggedId<Tag>::Make(text);
  if (!made.ok()) {
    ::rcbtest::ReportFailure(__FILE__, __LINE__,
                             "fixture identifier '" + text + "' was refused: " +
                                 made.status().ToString());
    return rcb::TaggedId<Tag>();
  }
  return made.value();
}

rcb::ScaledAmount Micros(i64 value) {
  rcb::Result<rcb::ScaledAmount> made = rcb::ScaledAmount::FromMicros(value);
  if (!made.ok()) {
    ::rcbtest::ReportFailure(__FILE__, __LINE__,
                             "fixture amount was refused: " + made.status().ToString());
    return rcb::ScaledAmount();
  }
  return made.value();
}

rcb::CapacityVector Cap(i64 power, i64 cooling, i64 racks, i64 service) {
  const std::array<i64, rcb::kDimensionCount> values{power, cooling, racks, service};
  rcb::Result<rcb::CapacityVector> made = rcb::CapacityVector::Make(values);
  if (!made.ok()) {
    ::rcbtest::ReportFailure(__FILE__, __LINE__,
                             "fixture capacity was refused: " + made.status().ToString());
    return rcb::CapacityVector();
  }
  return made.value();
}

rcb::CapacityVector Cap(i64 power) { return Cap(power, 0, 0, 0); }

/// "site-000", "site-001", ... so that a generated list is sorted and unique.
std::string NumberedName(const char* prefix, std::size_t index) {
  std::string digits = std::to_string(index);
  while (digits.size() < 3) {
    digits.insert(digits.begin(), '0');
  }
  return std::string(prefix) + digits;
}

Bytes BytesOf(const std::string_view text) {
  Bytes bytes;
  bytes.reserve(text.size());
  for (const char raw : text) {
    bytes.push_back(static_cast<u8>(raw));
  }
  return bytes;
}

// ---- live (non-JSON) fixtures -------------------------------------------

rcb::Offer LiveOffer(const std::string& site, const u64 generation, const i64 allocatable,
                     const i64 reserve) {
  rcb::Offer offer;
  offer.site = Id<rcb::SiteIdTag>(site);
  offer.generation = generation;
  offer.source_snapshot = Id<rcb::SnapshotIdTag>("snap-1");
  offer.service_class = Id<rcb::ServiceClassIdTag>("general");
  offer.region = Id<rcb::RegionIdTag>("region-a");
  offer.jurisdiction = Id<rcb::JurisdictionIdTag>("jur-a");
  offer.risk = rcb::RiskTier::Nominal;
  offer.valid_from = 0;
  offer.valid_until = 100000;
  offer.reserve_policy = Id<rcb::PolicyIdTag>("policy-a");
  offer.policy_generation = 1;
  offer.reserve_minimum_priority = rcb::PriorityClass::Critical;
  offer.cost.reference = Id<rcb::EvidenceIdTag>("evidence-a");
  offer.cost.price_per_milli_unit = Micros(1000);
  offer.cost.energy_millijoules_per_milli_unit = 2;
  offer.cost.carbon_milligrams_per_milli_unit = 3;
  rcb::CapacityTranche tranche;
  tranche.domain = Id<rcb::FailureDomainIdTag>("fd-a");
  tranche.allocatable = Cap(allocatable);
  tranche.protected_reserve = Cap(reserve);
  offer.tranches.push_back(tranche);
  return offer;
}

rcb::Ask LiveAsk(const std::string& key, const i64 requested) {
  rcb::Ask ask;
  ask.key = Id<rcb::AskKeyTag>(key);
  ask.requester = Id<rcb::RequesterIdTag>("requester-a");
  ask.service_class = Id<rcb::ServiceClassIdTag>("general");
  ask.requested = Cap(requested);
  ask.as_of = 10;
  return ask;
}

// ---- JSON fixtures -------------------------------------------------------

rcb::JsonValue CapJson(const i64 power, const i64 cooling, const i64 racks, const i64 service) {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("power", rcb::JsonValue::MakeInt(power));
  (void)object.Set("cooling", rcb::JsonValue::MakeInt(cooling));
  (void)object.Set("rack_space", rcb::JsonValue::MakeInt(racks));
  (void)object.Set("service_capacity", rcb::JsonValue::MakeInt(service));
  return object;
}

rcb::JsonValue CapJson(const i64 power) { return CapJson(power, 0, 0, 0); }

rcb::JsonValue CostJson() {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("reference", rcb::JsonValue::MakeString("evidence-a"));
  (void)object.Set("price_per_milli_unit", rcb::JsonValue::MakeString("1.5"));
  (void)object.Set("energy_millijoules_per_milli_unit", rcb::JsonValue::MakeInt(2));
  (void)object.Set("carbon_milligrams_per_milli_unit", rcb::JsonValue::MakeInt(3));
  return object;
}

rcb::JsonValue TrancheJson(const std::string& domain, const i64 allocatable, const i64 reserve) {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("domain", rcb::JsonValue::MakeString(domain));
  (void)object.Set("allocatable", CapJson(allocatable));
  (void)object.Set("protected_reserve", CapJson(reserve));
  return object;
}

rcb::JsonValue PinJson(const std::string& site, const u64 generation) {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("site", rcb::JsonValue::MakeString(site));
  (void)object.Set("generation", rcb::JsonValue::MakeUint(generation));
  return object;
}

rcb::JsonValue PinJsonValue(const std::string& site, rcb::JsonValue generation) {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("site", rcb::JsonValue::MakeString(site));
  (void)object.Set("generation", std::move(generation));
  return object;
}

rcb::JsonValue ArrayOf(std::vector<rcb::JsonValue> items) {
  rcb::JsonValue array = rcb::JsonValue::MakeArray();
  for (rcb::JsonValue& item : items) {
    array.Push(std::move(item));
  }
  return array;
}

rcb::JsonValue StringArray(const std::vector<std::string>& items) {
  rcb::JsonValue array = rcb::JsonValue::MakeArray();
  for (const std::string& item : items) {
    array.Push(rcb::JsonValue::MakeString(item));
  }
  return array;
}

/// A copy of \p object with \p key replaced (or added).
rcb::JsonValue WithField(const rcb::JsonValue& object, const std::string& key,
                         rcb::JsonValue value) {
  rcb::JsonValue copy = rcb::JsonValue::MakeObject();
  for (const auto& member : object.members()) {
    if (member.first != key) {
      (void)copy.Set(member.first, member.second);
    }
  }
  (void)copy.Set(key, std::move(value));
  return copy;
}

/// A copy of \p object without \p key.
rcb::JsonValue WithOutField(const rcb::JsonValue& object, const std::string& key) {
  rcb::JsonValue copy = rcb::JsonValue::MakeObject();
  for (const auto& member : object.members()) {
    if (member.first != key) {
      (void)copy.Set(member.first, member.second);
    }
  }
  return copy;
}

/// The valid offer document every offer attack starts from.
rcb::JsonValue OfferJson() {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("schema", rcb::JsonValue::MakeUint(rcb::kRequestSchemaVersion));
  (void)object.Set("site", rcb::JsonValue::MakeString("site-a"));
  (void)object.Set("generation", rcb::JsonValue::MakeUint(1));
  (void)object.Set("source_snapshot", rcb::JsonValue::MakeString("snap-1"));
  (void)object.Set("service_class", rcb::JsonValue::MakeString("general"));
  (void)object.Set("region", rcb::JsonValue::MakeString("region-a"));
  (void)object.Set("jurisdiction", rcb::JsonValue::MakeString("jur-a"));
  (void)object.Set("risk", rcb::JsonValue::MakeString("nominal"));
  (void)object.Set("valid_from", rcb::JsonValue::MakeInt(0));
  (void)object.Set("valid_until", rcb::JsonValue::MakeInt(1000));
  (void)object.Set("reserve_policy", rcb::JsonValue::MakeString("policy-a"));
  (void)object.Set("policy_generation", rcb::JsonValue::MakeUint(1));
  (void)object.Set("reserve_minimum_priority", rcb::JsonValue::MakeString("critical"));
  (void)object.Set("cost", CostJson());
  (void)object.Set("tranches", ArrayOf({TrancheJson("fd-a", 100, 10)}));
  return object;
}

/// The valid ask document every ask attack starts from. Every field the decoder
/// defaults on purpose is left out, so this document is also the proof that the
/// documented defaults exist.
rcb::JsonValue AskJson() {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("schema", rcb::JsonValue::MakeUint(rcb::kRequestSchemaVersion));
  (void)object.Set("key", rcb::JsonValue::MakeString("ask-a"));
  (void)object.Set("requester", rcb::JsonValue::MakeString("requester-a"));
  (void)object.Set("service_class", rcb::JsonValue::MakeString("general"));
  (void)object.Set("requested", CapJson(40));
  return object;
}

// =========================================================================
// A. document attacks
// =========================================================================

RCB_TEST(unknown_fields_are_refused_at_every_level) {
  const rcb::JsonValue offer = OfferJson();
  const rcb::JsonValue ask = AskJson();

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "sites", rcb::JsonValue::MakeString("site-a"))),
                              ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "generations", rcb::JsonValue::MakeUint(1))),
                              ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "cost", WithField(CostJson(), "currency", rcb::JsonValue::MakeString("EUR")))),
      ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches",
                                     ArrayOf({WithField(TrancheJson("fd-a", 100, 10), "weight", rcb::JsonValue::MakeInt(1))}))),
      ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches",
                                     ArrayOf({WithField(TrancheJson("fd-a", 100, 10), "allocatable",
                                                        WithField(CapJson(100), "power_watts", rcb::JsonValue::MakeInt(100)))}))),
      ErrorCode::UnknownField);

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "keys", rcb::JsonValue::MakeString("ask-a"))),
                              ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins",
                                   ArrayOf({WithField(PinJson("site-a", 1), "epoch", rcb::JsonValue::MakeUint(1))}))),
      ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", WithField(CapJson(40), "power_w", rcb::JsonValue::MakeInt(40)))),
                              ErrorCode::UnknownField);

  // A misspelled field must not be treated as its near neighbour.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "max_site", rcb::JsonValue::MakeUint(1))),
                              ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "Site", rcb::JsonValue::MakeString("site-a"))),
                              ErrorCode::UnknownField);
}

RCB_TEST(missing_required_fields_are_refused_by_name) {
  const std::array<const char*, 13> offer_fields = {"schema",        "site",         "generation",
                                                    "source_snapshot", "service_class", "region",
                                                    "jurisdiction",  "risk",         "valid_from",
                                                    "valid_until",   "reserve_policy", "cost",
                                                    "tranches"};
  for (const char* key : offer_fields) {
    ::rcb::BrokerCore probe;
    const rcb::Result<rcb::Offer> result = rcb::DecodeOfferJson(WithOutField(OfferJson(), key));
    RCB_CHECK_ERROR(result, ErrorCode::MissingField);
    RCB_CHECK(!result.ok() && result.status().detail().find(key) != std::string::npos);
    RCB_CHECK_EQ(probe.sequence(), u64{0});
    RCB_CHECK(probe.StateDigest() == rcb::BrokerCore().StateDigest());
    RCB_CHECK(probe.VerifyConservation().closed);
  }

  const std::array<const char*, 5> ask_fields = {"schema", "key", "requester", "service_class",
                                                 "requested"};
  for (const char* key : ask_fields) {
    ::rcb::BrokerCore probe;
    const rcb::Result<rcb::Ask> result = rcb::DecodeAskJson(WithOutField(AskJson(), key));
    RCB_CHECK_ERROR(result, ErrorCode::MissingField);
    RCB_CHECK(!result.ok() && result.status().detail().find(key) != std::string::npos);
    RCB_CHECK_EQ(probe.sequence(), u64{0});
    RCB_CHECK(probe.StateDigest() == rcb::BrokerCore().StateDigest());
    RCB_CHECK(probe.VerifyConservation().closed);
  }

  // A required member that is present but empty is a missing field, not an
  // empty identifier that travels on.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(OfferJson(), "site", rcb::JsonValue::MakeString(""))),
                              ErrorCode::MissingField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(AskJson(), "key", rcb::JsonValue::MakeString(""))),
                              ErrorCode::MissingField);

  // A capacity vector must carry all four dimensions.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(AskJson(), "requested", WithOutField(CapJson(40), "cooling"))),
                              ErrorCode::MissingField);
}

RCB_TEST(duplicate_json_keys_are_refused_by_the_parser) {
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"a":1,"a":2})"), ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"site":"site-a","site":"site-b"})"),
                              ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"site":"site-a","site":1})"),
                              ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"requested":{"power":1,"power":2}})"),
                              ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"tranches":[{"domain":"fd-a","domain":"fd-b"}]})"),
                              ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"cost":{"reference":"e","reference":"f"}})"),
                              ErrorCode::DuplicateField);
  // A complete offer document whose schema key is repeated: the parser refuses
  // before the decoder can pick either one.
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::ParseJson(R"({"schema":1,"site":"site-a","generation":1,"source_snapshot":"s","service_class":"c","region":"r","jurisdiction":"j","risk":"nominal","valid_from":0,"valid_until":1,"reserve_policy":"p","cost":{"reference":"e","price_per_milli_unit":"1","energy_millijoules_per_milli_unit":0,"carbon_milligrams_per_milli_unit":0},"tranches":[{"domain":"fd-a","allocatable":{"power":1,"cooling":0,"rack_space":0,"service_capacity":0},"protected_reserve":{"power":0,"cooling":0,"rack_space":0,"service_capacity":0}}],"schema":2})"),
      ErrorCode::DuplicateField);
}

RCB_TEST(wrong_types_are_refused_with_malformed_input) {
  const rcb::JsonValue offer = OfferJson();
  const rcb::JsonValue ask = AskJson();

  // A number where a string is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "site", rcb::JsonValue::MakeUint(7))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "risk", rcb::JsonValue::MakeInt(0))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "cost", WithField(CostJson(), "reference", rcb::JsonValue::MakeInt(1)))),
      ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requester", rcb::JsonValue::MakeBool(true))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({WithField(TrancheJson("fd-a", 1, 0), "domain", rcb::JsonValue::MakeUint(3))}))),
      ErrorCode::MalformedInput);

  // A string where a number is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "generation", rcb::JsonValue::MakeString("1"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "valid_from", rcb::JsonValue::MakeString("0"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "policy_generation", rcb::JsonValue::MakeString("1"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "as_of", rcb::JsonValue::MakeString("0"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "max_total_energy_millijoules", rcb::JsonValue::MakeString("0"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({PinJsonValue("site-a", rcb::JsonValue::MakeString("3"))}))),
      ErrorCode::MalformedInput);

  // An object where an array is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "tranches", rcb::JsonValue::MakeObject())),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", rcb::JsonValue::MakeObject())),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "generation_pins", rcb::JsonValue::MakeObject())),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "excluded_failure_domains", rcb::JsonValue::MakeString("fd-a"))),
                              ErrorCode::MalformedInput);

  // An array where an object is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "cost", rcb::JsonValue::MakeArray())),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", rcb::JsonValue::MakeArray())),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({rcb::JsonValue::MakeString("site-a")}))),
                              ErrorCode::MalformedInput);

  // A string, a number or a null where a boolean is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "all_or_nothing", rcb::JsonValue::MakeString("true"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "may_consume_protected_reserve", rcb::JsonValue::MakeUint(1))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "require_current_generation", rcb::JsonValue())),
                              ErrorCode::MalformedInput);

  // A boolean where a number is required, and a string where a capacity
  // dimension is required.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "as_of", rcb::JsonValue::MakeBool(true))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", WithField(CapJson(40), "power", rcb::JsonValue::MakeString("40")))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", WithField(CapJson(40), "power", rcb::JsonValue()))),
                              ErrorCode::MalformedInput);

  // A document that is not an object at all.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(rcb::JsonValue::MakeArray()), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(rcb::JsonValue::MakeString("offer")),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(rcb::JsonValue::MakeString("ask")), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(rcb::JsonValue::MakeBool(false)), ErrorCode::MalformedInput);
}

RCB_TEST(schema_version_is_pinned_in_both_directions) {
  const rcb::JsonValue offer = OfferJson();
  const rcb::JsonValue ask = AskJson();

  for (const u64 version : {u64{0}, u64{2}, u64{3}, u64{1000}, u64{0xFFFFFFFFFFFFFFFFULL}}) {
    RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "schema", rcb::JsonValue::MakeUint(version))),
                                ErrorCode::UnsupportedVersion);
    RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "schema", rcb::JsonValue::MakeUint(version))),
                                ErrorCode::UnsupportedVersion);
  }

  // "1" is the right number with the wrong type: a type error, not a version
  // that happens to be unsupported.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "schema", rcb::JsonValue::MakeString("1"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "schema", rcb::JsonValue::MakeString("1"))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "schema", rcb::JsonValue::MakeBool(true))),
                              ErrorCode::MalformedInput);

  // -1 is a negative version: refused as out of range before any version
  // comparison happens.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "schema", rcb::JsonValue::MakeInt(-1))),
                              ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "schema", rcb::JsonValue::MakeInt(-1))),
                              ErrorCode::OutOfRange);

  // The one accepted version decodes both documents.
  const rcb::Result<rcb::Offer> decoded_offer = rcb::DecodeOfferJson(offer);
  RCB_CHECK(decoded_offer.ok());
  if (decoded_offer.ok()) {
    RCB_CHECK_EQ(decoded_offer.value().site.value(), std::string("site-a"));
    RCB_CHECK_EQ(decoded_offer.value().tranches.size(), std::size_t{1});
  }
  const rcb::Result<rcb::Ask> decoded_ask = rcb::DecodeAskJson(ask);
  RCB_CHECK(decoded_ask.ok());
  if (decoded_ask.ok()) {
    RCB_CHECK_EQ(decoded_ask.value().key.value(), std::string("ask-a"));
    RCB_CHECK(decoded_ask.value().requested.Get(rcb::Dimension::Power) == i64{40});
  }
}

RCB_TEST(floating_point_numbers_are_refused_wherever_they_appear) {
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"schema":1.0})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"generation":1.5})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"generation":1e3})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"generation":1E+3})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"power":-0.0})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"power":1.000000})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"tranches":[{"allocatable":{"power":1.5}}]})"),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"([1,2,3.5])"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(R"({"a":0.5e-3})"), ErrorCode::MalformedInput);

  // Exact decimal evidence travels as a string, so a JSON number in that slot
  // is a type error rather than a rounding opportunity.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(AskJson(), "max_total_cost", rcb::JsonValue::MakeInt(1))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(OfferJson(), "cost", WithField(CostJson(), "price_per_milli_unit", rcb::JsonValue::MakeUint(2)))),
      ErrorCode::MalformedInput);

  // Integers are still integers.
  const rcb::Result<rcb::JsonValue> integer = rcb::ParseJson(R"({"a":-9223372036854775808})");
  RCB_CHECK(integer.ok());
  if (integer.ok()) {
    RCB_CHECK(integer.value().Find("a") != nullptr);
    RCB_CHECK(integer.value().Find("a")->IsInt());
  }
}

RCB_TEST(capacity_bounds_signs_and_types_are_enforced) {
  const rcb::JsonValue offer = OfferJson();
  const rcb::JsonValue ask = AskJson();
  const i64 over = rcb::Limits::kMaxDimensionValue + 1;

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", CapJson(over))),
                              ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", CapJson(0, over, 0, 0))),
                              ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", over, 0)}))),
      ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", 0, over)}))),
      ErrorCode::OutOfRange);

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", CapJson(-1))),
                              ErrorCode::InvalidArgument);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", CapJson(0, -1, 0, 0))),
                              ErrorCode::InvalidArgument);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", -5, 0)}))),
      ErrorCode::InvalidArgument);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", 0, -5)}))),
      ErrorCode::InvalidArgument);

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", WithField(CapJson(40), "power", rcb::JsonValue::MakeString("40")))),
                              ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", 0, 0)}))),
      ErrorCode::InvalidRequest);

  // The bound itself is accepted.
  const rcb::Result<rcb::Offer> at_bound = rcb::DecodeOfferJson(
      WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", rcb::Limits::kMaxDimensionValue, 0)})));
  RCB_CHECK(at_bound.ok());
  if (at_bound.ok()) {
    RCB_CHECK_EQ(at_bound.value().tranches[0].allocatable.Get(rcb::Dimension::Power),
                 rcb::Limits::kMaxDimensionValue);
  }

  // A total above the per-offer bound is refused when the tranches are summed:
  // seventeen tranches at the per-dimension bound cross kMaxOfferTotal.
  rcb::Offer summed = LiveOffer("site-a", 1, 1, 0);
  summed.tranches.clear();
  for (std::size_t index = 0; index < 20; ++index) {
    rcb::CapacityTranche tranche;
    tranche.domain = Id<rcb::FailureDomainIdTag>(NumberedName("fd-", index));
    tranche.allocatable = Cap(rcb::Limits::kMaxDimensionValue);
    summed.tranches.push_back(tranche);
  }
  RCB_CHECK_STATUS(rcb::ValidateOffer(summed), ErrorCode::LimitExceeded);
}

RCB_TEST(offer_tranches_are_normalised_and_duplicates_refused) {
  const rcb::JsonValue offer = OfferJson();

  // Unsorted tranches are normalised, never rejected.
  const rcb::Result<rcb::Offer> unsorted = rcb::DecodeOfferJson(WithField(
      offer, "tranches",
      ArrayOf({TrancheJson("fd-z", 10, 0), TrancheJson("fd-a", 20, 0), TrancheJson("fd-m", 5, 0)})));
  RCB_CHECK(unsorted.ok());
  if (unsorted.ok()) {
    RCB_CHECK_EQ(unsorted.value().tranches.size(), std::size_t{3});
    RCB_CHECK_EQ(unsorted.value().tranches[0].domain.value(), std::string("fd-a"));
    RCB_CHECK_EQ(unsorted.value().tranches[1].domain.value(), std::string("fd-m"));
    RCB_CHECK_EQ(unsorted.value().tranches[2].domain.value(), std::string("fd-z"));
  }

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches",
                                     ArrayOf({TrancheJson("fd-a", 10, 0), TrancheJson("fd-a", 20, 0)}))),
      ErrorCode::DuplicateIdentity);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(offer, "tranches",
                                     ArrayOf({TrancheJson("fd-b", 10, 0), TrancheJson("fd-a", 20, 0),
                                              TrancheJson("fd-a", 30, 0)}))),
      ErrorCode::DuplicateIdentity);

  // An empty tranche list declares no capacity at all.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeOfferJson(WithField(offer, "tranches", rcb::JsonValue::MakeArray())),
                              ErrorCode::InvalidRequest);

  // The same rules hold when the structure is built directly.
  rcb::Offer duplicated = LiveOffer("site-a", 1, 10, 0);
  rcb::CapacityTranche second;
  second.domain = Id<rcb::FailureDomainIdTag>("fd-a");
  second.allocatable = Cap(1);
  duplicated.tranches.push_back(second);
  RCB_CHECK_STATUS(rcb::ValidateOffer(duplicated), ErrorCode::DuplicateIdentity);

  rcb::Offer unsorted_direct = LiveOffer("site-a", 1, 10, 0);
  rcb::CapacityTranche earlier;
  earlier.domain = Id<rcb::FailureDomainIdTag>("fd-0");
  earlier.allocatable = Cap(1);
  unsorted_direct.tranches.push_back(earlier);
  RCB_CHECK_STATUS(rcb::ValidateOffer(unsorted_direct), ErrorCode::DuplicateIdentity);
  RCB_CHECK(rcb::NormalizeOffer(unsorted_direct).ok());
}

RCB_TEST(ask_requested_vector_and_locality_conflicts_are_refused) {
  const rcb::JsonValue ask = AskJson();

  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "requested", CapJson(0, 0, 0, 0))),
                              ErrorCode::InvalidRequest);
  // A request that names its capacity in any single dimension is a request;
  // only the all-zero vector is empty.
  const rcb::Result<rcb::Ask> cooling_only =
      rcb::DecodeAskJson(WithField(ask, "requested", CapJson(0, 5, 0, 0)));
  RCB_CHECK(cooling_only.ok());
  if (cooling_only.ok()) {
    RCB_CHECK_EQ(cooling_only.value().requested.Get(rcb::Dimension::Cooling), i64{5});
    RCB_CHECK(cooling_only.value().requested.Get(rcb::Dimension::Power) == i64{0});
  }

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "allowed_regions", StringArray({"region-a"})),
                                   "excluded_regions", StringArray({"region-a"}))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "allowed_jurisdictions", StringArray({"jur-a"})),
                                   "excluded_jurisdictions", StringArray({"jur-a"}))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "allowed_sites", StringArray({"site-a"})),
                                   "excluded_sites", StringArray({"site-a"}))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "allowed_sites", StringArray({"site-a", "site-b"})),
                                   "excluded_sites", StringArray({"site-a", "site-z"}))),
      ErrorCode::InvalidRequest);

  // A list that repeats an entry is refused even though the decoder sorted it.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", StringArray({"site-a", "site-a"}))),
                              ErrorCode::DuplicateIdentity);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "excluded_regions", StringArray({"region-a", "region-a"}))),
                              ErrorCode::DuplicateIdentity);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "excluded_failure_domains", StringArray({"fd-a", "fd-a"}))),
      ErrorCode::DuplicateIdentity);

  // Unsorted lists are normalised, not rejected.
  const rcb::Result<rcb::Ask> sorted = rcb::DecodeAskJson(
      WithField(ask, "allowed_sites", StringArray({"site-z", "site-a", "site-m"})));
  RCB_CHECK(sorted.ok());
  if (sorted.ok()) {
    RCB_CHECK_EQ(sorted.value().allowed_sites.size(), std::size_t{3});
    RCB_CHECK_EQ(sorted.value().allowed_sites[0].value(), std::string("site-a"));
    RCB_CHECK_EQ(sorted.value().allowed_sites[2].value(), std::string("site-z"));
  }

  // An excluded list that names an identifier the edge would refuse.
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", StringArray({"site/a"}))),
                              ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", StringArray({"NUL"}))),
                              ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", ArrayOf({rcb::JsonValue::MakeUint(1)}))),
                              ErrorCode::MalformedInput);
}

RCB_TEST(ask_generation_pins_are_normalised_and_validated) {
  const rcb::JsonValue ask = AskJson();

  const rcb::Result<rcb::Ask> normalised = rcb::DecodeAskJson(WithField(
      ask, "generation_pins",
      ArrayOf({PinJson("site-z", 3), PinJson("site-a", 1), PinJson("site-m", 2)})));
  RCB_CHECK(normalised.ok());
  if (normalised.ok()) {
    RCB_CHECK_EQ(normalised.value().generation_pins.size(), std::size_t{3});
    RCB_CHECK_EQ(normalised.value().generation_pins[0].site.value(), std::string("site-a"));
    RCB_CHECK_EQ(normalised.value().generation_pins[0].generation, u64{1});
    RCB_CHECK_EQ(normalised.value().generation_pins[2].site.value(), std::string("site-z"));
  }

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({PinJson("site-a", 1), PinJson("site-a", 2)}))),
      ErrorCode::DuplicateIdentity);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({PinJson("site-a", 0)}))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({PinJsonValue("", rcb::JsonValue::MakeUint(1))}))),
      ErrorCode::MissingField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({WithOutField(PinJson("site-a", 1), "generation")}))),
      ErrorCode::MissingField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({WithOutField(PinJson("site-a", 1), "site")}))),
      ErrorCode::MissingField);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf({PinJson("site/a", 1)}))),
      ErrorCode::InvalidIdentifier);
}

RCB_TEST(ask_bounds_and_reserve_authority_rules_are_enforced) {
  const rcb::JsonValue ask = AskJson();

  // The documented bound is Limits::kMaxListElements.
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "min_distinct_failure_domains", rcb::JsonValue::MakeUint(257))),
      ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "max_sites", rcb::JsonValue::MakeUint(257))),
                              ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "max_sites", rcb::JsonValue::MakeUint(u64{1} << 32))),
      ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "min_distinct_failure_domains", rcb::JsonValue::MakeInt(-1))),
      ErrorCode::OutOfRange);

  RCB_CHECK(rcb::DecodeAskJson(WithField(WithField(ask, "min_distinct_failure_domains", rcb::JsonValue::MakeUint(256)),
                                         "max_sites", rcb::JsonValue::MakeUint(256)))
                .ok());

  // Single-source coherence cannot coexist with a two-site limit.
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "require_single_source", rcb::JsonValue::MakeBool(true)),
                                   "max_sites", rcb::JsonValue::MakeUint(2))),
      ErrorCode::InvalidRequest);
  RCB_CHECK(rcb::DecodeAskJson(WithField(WithField(ask, "require_single_source", rcb::JsonValue::MakeBool(true)),
                                         "max_sites", rcb::JsonValue::MakeUint(1)))
                .ok());
  // An explicitly unbounded site limit is not a contradiction the decoder
  // refuses: zero means unbounded, and the planner is what enforces the single
  // home. The boundary therefore accepts this document, and the check below
  // proves the acceptance is safe.
  const rcb::JsonValue unbounded = WithField(WithField(ask, "require_single_source", rcb::JsonValue::MakeBool(true)),
                                             "max_sites", rcb::JsonValue::MakeUint(0));
  const rcb::Result<rcb::Ask> unbounded_ask = rcb::DecodeAskJson(unbounded);
  RCB_CHECK(unbounded_ask.ok());
  if (unbounded_ask.ok()) {
    RCB_CHECK(unbounded_ask.value().require_single_source);
    RCB_CHECK_EQ(unbounded_ask.value().max_sites, u32{0});
    rcb::Ask split = unbounded_ask.value();
    split.requested = Cap(60);
    rcb::BrokerCore core;
    RCB_REQUIRE(core.PublishOffer(LiveOffer("site-a", 1, 50, 0)).ok());
    RCB_REQUIRE(core.PublishOffer(LiveOffer("site-b", 1, 50, 0)).ok());
    const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(split);
    RCB_CHECK(plan.ok());
    if (plan.ok()) {
      // Whatever the outcome, every allocation must share one site.
      for (const rcb::PlannedAllocation& allocation : plan.value().allocations) {
        RCB_CHECK(allocation.site == plan.value().allocations.front().site);
      }
    }
  }

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "may_consume_protected_reserve", rcb::JsonValue::MakeBool(true))),
      ErrorCode::MissingField);
  RCB_CHECK(rcb::DecodeAskJson(WithField(WithField(ask, "may_consume_protected_reserve", rcb::JsonValue::MakeBool(true)),
                                         "reserve_policy_authorization", rcb::JsonValue::MakeString("policy-a")))
                .ok());
  // An authorization naming a path-like identifier is refused at the edge.
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "may_consume_protected_reserve", rcb::JsonValue::MakeBool(true)),
                                   "reserve_policy_authorization", rcb::JsonValue::MakeString("policy/a"))),
      ErrorCode::InvalidIdentifier);

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "has_policy_generation_pin", rcb::JsonValue::MakeBool(true)),
                                   "policy_generation_pin", rcb::JsonValue::MakeUint(0))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "as_of", rcb::JsonValue::MakeInt(-1))),
                              ErrorCode::InvalidRequest);

  // Seven decimal places is more precision than a micro-unit carries.
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "max_total_cost", rcb::JsonValue::MakeString("1.0000000"))),
      ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "max_total_cost", rcb::JsonValue::MakeString("0.0000001"))),
      ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "max_total_cost", rcb::JsonValue::MakeString("1.000000e1"))),
      ErrorCode::MalformedInput);
  RCB_CHECK(rcb::DecodeAskJson(WithField(ask, "max_total_cost", rcb::JsonValue::MakeString("1.000001"))).ok());

  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "has_cost_ceiling", rcb::JsonValue::MakeBool(true)),
                                   "max_total_cost", rcb::JsonValue::MakeString("-1"))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "has_energy_ceiling", rcb::JsonValue::MakeBool(true)),
                                   "max_total_energy_millijoules", rcb::JsonValue::MakeInt(-1))),
      ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(WithField(ask, "has_carbon_ceiling", rcb::JsonValue::MakeBool(true)),
                                   "max_total_carbon_milligrams", rcb::JsonValue::MakeInt(-1))),
      ErrorCode::InvalidRequest);
}

RCB_TEST(identifier_attacks_are_refused_at_the_edge) {
  const std::string at_limit(rcb::Identifier::kMaxLength, 'a');
  const std::string over_limit(rcb::Identifier::kMaxLength + 1, 'a');

  RCB_CHECK(rcb::Identifier::Make(at_limit).ok());
  RCB_CHECK_ERROR(rcb::Identifier::Make(over_limit), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("site/a"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("site\\a"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make(std::string("site\0a", 6)), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make(std::string("\xFF\xFE", 2)), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xC3\x28"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("caf\xC3\xA9"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make(""), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("."), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make(".."), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("-lead"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("_lead"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("with space"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("with\ttab"), ErrorCode::InvalidIdentifier);

  // The Windows device names, with and without an extension, in any case.
  RCB_CHECK_ERROR(rcb::Identifier::Make("NUL"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("nul"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("NUL.txt"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("CON"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("PRN"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("AUX"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("COM1"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("LPT9.log"), ErrorCode::InvalidIdentifier);
  // Names that merely start with a device name are fine.
  RCB_CHECK(rcb::Identifier::Make("NULL").ok());
  RCB_CHECK(rcb::Identifier::Make("NULx").ok());
  RCB_CHECK(rcb::Identifier::Make("console").ok());
  RCB_CHECK(rcb::Identifier::Make("COM10").ok());
  RCB_CHECK(rcb::Identifier::Make("site-a.1_b:c-d").ok());

  // The same refusals arrive with the same code through the document decoder,
  // and the field that failed is named in the detail.
  const std::string invalid_utf8 = std::string("\xFF", 1);
  const std::vector<std::string> bad_sites = {"NUL", "site/a", "caf\xC3\xA9", over_limit, invalid_utf8};
  for (const std::string& bad : bad_sites) {
    ::rcb::BrokerCore probe;
    const rcb::Result<rcb::Offer> result =
        rcb::DecodeOfferJson(WithField(OfferJson(), "site", rcb::JsonValue::MakeString(bad)));
    RCB_CHECK_ERROR(result, ErrorCode::InvalidIdentifier);
    RCB_CHECK(!result.ok() && result.status().detail().find("site") != std::string::npos);
    RCB_CHECK_EQ(probe.sequence(), u64{0});
    RCB_CHECK(probe.StateDigest() == rcb::BrokerCore().StateDigest());
  }
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(AskJson(), "requester", rcb::JsonValue::MakeString("NUL"))),
      ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(OfferJson(), "cost", WithField(CostJson(), "reference", rcb::JsonValue::MakeString("..")))),
      ErrorCode::InvalidIdentifier);
}

RCB_TEST(document_attacks_leave_a_live_ledger_untouched) {
  rcb::BrokerCore core;
  RCB_REQUIRE(core.PublishOffer(LiveOffer("site-a", 1, 100, 10)).ok());
  const rcb::Result<rcb::Ask> ask = rcb::DecodeAskJson(AskJson());
  RCB_REQUIRE_OK_REF(ask);
  RCB_REQUIRE(core.PlanAsk(ask.value()).ok());

  const rcb::u64 sequence = core.sequence();
  const rcb::Digest digest = core.StateDigest();
  RCB_CHECK(sequence > u64{0});

  const rcb::JsonValue offer = OfferJson();
  const rcb::JsonValue ask_document = AskJson();
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "nope", rcb::JsonValue::MakeInt(1))), ErrorCode::UnknownField);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithOutField(offer, "site")), ErrorCode::MissingField);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "schema", rcb::JsonValue::MakeUint(2))), ErrorCode::UnsupportedVersion);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "site", rcb::JsonValue::MakeUint(1))), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "site", rcb::JsonValue::MakeString("NUL"))), ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", 1, 0), TrancheJson("fd-a", 1, 0)}))), ErrorCode::DuplicateIdentity);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeOfferJson(WithField(offer, "tranches", ArrayOf({TrancheJson("fd-a", rcb::Limits::kMaxDimensionValue + 1, 0)}))), ErrorCode::OutOfRange);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeAskJson(WithField(ask_document, "requested", CapJson(0))), ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::DecodeAskJson(WithField(WithField(ask_document, "allowed_sites", StringArray({"site-a"})), "excluded_sites", StringArray({"site-a"}))), ErrorCode::InvalidRequest);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::ParseJson(R"({"a":1,"a":2})"), ErrorCode::DuplicateField);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::ParseJson(R"({"a":1.5})"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_LEDGER_INERT(core, rcb::ParseJson("{\"a\":\"\xC3\x28\"}"), ErrorCode::InvalidUnicode);

  RCB_CHECK_EQ(core.sequence(), sequence);
  RCB_CHECK(core.StateDigest() == digest);
  RCB_CHECK(core.VerifyConservation().closed);
  RCB_CHECK_EQ(core.RecentDecisions(8).size(), std::size_t{0});
  RCB_CHECK_EQ(core.Summary().sites, std::size_t{1});
}

// =========================================================================
// B. journal framing
// =========================================================================

// The framing is public contract: a fixed 64-byte header (magic, version, kind,
// flags, epoch, sequence, payload length, payload CRC-32C, chained SHA-256)
// followed by the payload. The tests below mutate exactly one field of exactly
// one frame and assert the exact classification, because "torn" and "corrupt"
// have different consequences: a torn tail is dropped, corruption is refused.
constexpr std::size_t kFrameHeaderBytes = 64;
constexpr std::size_t kSequenceOffset = 16;
constexpr std::size_t kPayloadLengthOffset = 24;
constexpr std::size_t kPayloadCrcOffset = 28;
constexpr std::size_t kChainOffset = 32;

struct JournalImage {
  Bytes bytes;
  std::vector<std::size_t> frame_start;
  std::vector<std::size_t> frame_size;
  std::vector<std::size_t> payload_start;
  std::vector<std::size_t> payload_size;

  [[nodiscard]] std::size_t frames() const { return frame_start.size(); }
  [[nodiscard]] std::size_t payload_byte(const std::size_t frame, const std::size_t index) const {
    return payload_start[frame] + index;
  }
  [[nodiscard]] std::size_t chain_byte(const std::size_t frame) const {
    return frame_start[frame] + kChainOffset;
  }
  [[nodiscard]] std::size_t end() const {
    return frame_start.empty() ? 0 : frame_start.back() + frame_size.back();
  }
};

/// Writes real records through a real Journal while remembering exactly where
/// every frame and every payload landed.
class JournalBuilder {
 public:
  explicit JournalBuilder(const rcb::JournalLimits& limits) : limits_(limits) {
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log_, limits_, false, 0);
    if (opened.ok()) {
      journal_ = std::move(opened.value());
    }
  }

  JournalBuilder(const JournalBuilder&) = delete;
  JournalBuilder& operator=(const JournalBuilder&) = delete;

  [[nodiscard]] bool ready() const { return journal_ != nullptr; }
  rcb::Journal& journal() { return *journal_; }
  rcb::MemoryByteLog& log() { return log_; }

  bool Record(const rcb::RecordKind kind, const u64 sequence, const std::string& payload,
              const bool commit) {
    if (journal_ == nullptr) {
      return false;
    }
    const std::size_t before = static_cast<std::size_t>(log_.size());
    const std::size_t payload_bytes = payload.size();
    if (!journal_->Prepare(kind, sequence, payload).ok()) {
      return false;
    }
    const std::size_t after_prepare = static_cast<std::size_t>(log_.size());
    image_.frame_start.push_back(before);
    image_.frame_size.push_back(after_prepare - before);
    image_.payload_start.push_back(after_prepare - payload_bytes);
    image_.payload_size.push_back(payload_bytes);
    if (after_prepare - before - payload_bytes != kFrameHeaderBytes) {
      return false;
    }
    if (!commit) {
      return true;
    }
    if (!journal_->MarkCommitted(rcb::DurabilityClass::Buffered).ok()) {
      return false;
    }
    const std::size_t after_marker = static_cast<std::size_t>(log_.size());
    image_.frame_start.push_back(after_prepare);
    image_.frame_size.push_back(after_marker - after_prepare);
    image_.payload_start.push_back(after_marker);
    image_.payload_size.push_back(0);
    return after_marker - after_prepare == kFrameHeaderBytes;
  }

  bool Close() {
    if (journal_ == nullptr) {
      return false;
    }
    const std::size_t before = static_cast<std::size_t>(log_.size());
    if (!journal_->Close().ok()) {
      return false;
    }
    const std::size_t after = static_cast<std::size_t>(log_.size());
    if (after - before < kFrameHeaderBytes) {
      return false;
    }
    image_.frame_start.push_back(before);
    image_.frame_size.push_back(after - before);
    image_.payload_start.push_back(before + kFrameHeaderBytes);
    image_.payload_size.push_back(after - before - kFrameHeaderBytes);
    return true;
  }

  [[nodiscard]] JournalImage Finish() {
    image_.bytes = log_.buffer();
    return image_;
  }

 private:
  rcb::JournalLimits limits_;
  rcb::MemoryByteLog log_;
  std::unique_ptr<rcb::Journal> journal_;
  JournalImage image_;
};

std::string DecisionPayload(const u64 sequence) {
  rcb::JsonValue object = rcb::JsonValue::MakeObject();
  (void)object.Set("sequence", rcb::JsonValue::MakeUint(sequence));
  (void)object.Set("note", rcb::JsonValue::MakeString("adversarial framing payload"));
  return object.ToText(false);
}

/// An epoch record and two decision-shaped records, every one of them marked
/// committed: six frames, three prepared records and their three markers.
bool BuildCommittedImage(JournalImage* image, const rcb::JournalLimits& limits, const bool close) {
  JournalBuilder builder(limits);
  if (!builder.ready()) {
    return false;
  }
  builder.journal().SetEpoch(4);
  if (!builder.Record(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(4), true)) {
    return false;
  }
  if (!builder.Record(rcb::RecordKind::Decision, 1, DecisionPayload(1), true)) {
    return false;
  }
  if (!builder.Record(rcb::RecordKind::Decision, 2, DecisionPayload(2), true)) {
    return false;
  }
  if (close && !builder.Close()) {
    return false;
  }
  *image = builder.Finish();
  return true;
}

/// The same, except that the final prepared record was never marked committed.
bool BuildImageWithPendingTail(JournalImage* image, const rcb::JournalLimits& limits) {
  JournalBuilder builder(limits);
  if (!builder.ready()) {
    return false;
  }
  builder.journal().SetEpoch(4);
  if (!builder.Record(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(4), true)) {
    return false;
  }
  if (!builder.Record(rcb::RecordKind::Decision, 1, DecisionPayload(1), true)) {
    return false;
  }
  if (!builder.Record(rcb::RecordKind::Decision, 2, DecisionPayload(2), false)) {
    return false;
  }
  *image = builder.Finish();
  return true;
}

/// A real decision record payload: replaying it would commit capacity.
bool BuildDecisionPayload(std::string* payload, rcb::AskKey* key) {
  rcb::BrokerCore core;
  if (!core.PublishOffer(LiveOffer("site-a", 1, 100, 0)).ok()) {
    return false;
  }
  const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(LiveAsk("ask-record", 40));
  if (!plan.ok()) {
    return false;
  }
  const rcb::Result<rcb::Decision> decision = core.MaterializeDecision(plan.value());
  if (!decision.ok() || !decision.value().committed_anything()) {
    return false;
  }
  *payload = rcb::EncodeDecisionRecord(decision.value());
  *key = decision.value().key;
  return true;
}

// ---- byte-level mutation -------------------------------------------------

void StoreLe64At(u8* out, const u64 value) {
  for (std::size_t i = 0; i < 8; ++i) {
    out[i] = static_cast<u8>((value >> (i * 8U)) & 0xFFU);
  }
}

void StoreLe32At(u8* out, const u32 value) {
  for (std::size_t i = 0; i < 4; ++i) {
    out[i] = static_cast<u8>((value >> (i * 8U)) & 0xFFU);
  }
}

void StoreLe(Bytes& bytes, const std::size_t index, const u64 value, const std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    bytes[index + i] = static_cast<u8>((value >> (i * 8U)) & 0xFFU);
  }
}

Bytes Flipped(const Bytes& bytes, const std::size_t index) {
  Bytes copy = bytes;
  copy[index] = static_cast<u8>(copy[index] ^ 0x01U);
  return copy;
}

Bytes Zeroed(const Bytes& bytes, const std::size_t index, const std::size_t length) {
  Bytes copy = bytes;
  for (std::size_t i = 0; i < length; ++i) {
    copy[index + i] = 0;
  }
  return copy;
}

Bytes Resized(const Bytes& bytes, const std::size_t size) {
  Bytes copy = bytes;
  copy.resize(size);
  return copy;
}

Bytes Padded(const Bytes& bytes, const std::size_t count, const u8 value) {
  Bytes copy = bytes;
  copy.insert(copy.end(), count, value);
  return copy;
}

/// Rewrites one payload byte and the CRC that covers it, so the per-record
/// checksum agrees and only the running chain can catch the change.
Bytes Forged(const Bytes& bytes, const JournalImage& image, const std::size_t frame,
             const std::size_t payload_index) {
  Bytes copy = bytes;
  const std::size_t at = image.payload_byte(frame, payload_index);
  copy[at] = static_cast<u8>(copy[at] ^ 0x20U);
  const std::span<const u8> payload(copy.data() + image.payload_start[frame],
                                    image.payload_size[frame]);
  StoreLe(copy, image.frame_start[frame] + kPayloadCrcOffset, rcb::Crc32c(payload), 4);
  return copy;
}

/// A hand-built commit marker whose chain is computed with the documented rule
/// chain_n = SHA-256(chain_{n-1} || header[0..32) || payload), so that the only
/// thing wrong with the frame is the sequence it carries. Without this the chain
/// check would fire first and the marker check would never be reached.
Bytes BuildMarkerFrame(const Bytes& prefix, const std::size_t chain_offset, const u64 sequence,
                       const u64 epoch) {
  std::array<u8, kFrameHeaderBytes> header{};
  header[0] = 'R';
  header[1] = 'C';
  header[2] = 'B';
  header[3] = '1';
  header[4] = 1;  // format version, little endian
  header[5] = 0;
  header[6] = static_cast<u8>(rcb::RecordKind::CommitMarker);
  header[7] = 0;  // no flags: a marker is never prepared
  StoreLe64At(header.data() + 8, epoch);
  StoreLe64At(header.data() + kSequenceOffset, sequence);
  StoreLe32At(header.data() + kPayloadLengthOffset, 0);  // a marker carries no payload
  const std::span<const u8> empty_payload;
  StoreLe32At(header.data() + kPayloadCrcOffset, rcb::Crc32c(empty_payload));
  std::array<u8, rcb::Digest::kSize> previous_bytes{};
  std::copy_n(prefix.begin() + static_cast<std::ptrdiff_t>(chain_offset), rcb::Digest::kSize,
              previous_bytes.begin());
  const rcb::Digest previous = rcb::Digest::FromBytes(previous_bytes);
  rcb::Sha256 hasher;
  hasher.Update(std::span<const u8>(previous.bytes()));
  hasher.Update(std::span<const u8>(header.data(), kChainOffset));
  const rcb::Digest chain = hasher.Finalize();
  std::memcpy(header.data() + kChainOffset, chain.bytes().data(), rcb::Digest::kSize);
  return Bytes(header.begin(), header.end());
}

// ---- opening a mutated image --------------------------------------------

struct OpenOutcome {
  bool ok = false;
  rcb::Status status;
  std::size_t log_size = 0;
  std::size_t records = 0;
  std::size_t prepared_discarded = 0;
  std::size_t records_verified = 0;
  bool torn_tail = false;
  bool repaired = false;
  bool clean_close = false;
  u64 torn_tail_offset = 0;
  u64 torn_tail_bytes = 0;
  std::vector<rcb::RecordKind> kinds;
  std::vector<std::size_t> payload_sizes;
};

/// Opens \p log and reports exactly what recovery did to it. The log is left
/// holding whatever recovery left behind, so the caller can assert its size.
OpenOutcome OpenImage(rcb::MemoryByteLog& log, const rcb::JournalLimits& limits) {
  OpenOutcome outcome;
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
  outcome.log_size = static_cast<std::size_t>(log.size());
  if (!opened.ok()) {
    outcome.status = opened.status();
    return outcome;
  }
  const rcb::Journal& journal = *opened.value();
  outcome.ok = true;
  outcome.records = journal.records().size();
  outcome.prepared_discarded = journal.recovery().prepared_discarded;
  outcome.records_verified = journal.recovery().records_verified;
  outcome.torn_tail = journal.recovery().torn_tail;
  outcome.repaired = journal.recovery().repaired;
  outcome.clean_close = journal.recovery().clean_close;
  outcome.torn_tail_offset = journal.recovery().torn_tail_offset;
  outcome.torn_tail_bytes = journal.recovery().torn_tail_bytes;
  for (const rcb::JournalRecord& record : journal.records()) {
    outcome.kinds.push_back(record.kind);
    outcome.payload_sizes.push_back(record.payload.size());
  }
  return outcome;
}

/// Sets \p log's content and opens it. The caller keeps the log object.
OpenOutcome OpenBytes(rcb::MemoryByteLog& log, const Bytes& bytes, const rcb::JournalLimits& limits) {
  log.SetContent(bytes);
  return OpenImage(log, limits);
}

RCB_TEST(a_torn_tail_is_repaired_to_the_last_complete_record_boundary) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  RCB_REQUIRE(image.frames() == 6);

  // Cut in the middle of the final prepared record's payload. The commit marker
  // that followed it is gone with it, so the log ends mid-payload.
  const std::size_t cut = image.payload_start[4] + image.payload_size[4] / 2;
  const Bytes damaged = Resized(image.bytes, cut);

  rcb::MemoryByteLog log;
  const OpenOutcome outcome = OpenBytes(log, damaged, limits);

  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(outcome.torn_tail_offset, u64{image.frame_start[4]});
  RCB_CHECK_EQ(outcome.log_size, image.frame_start[4]);
  RCB_CHECK_EQ(log.size(), u64{image.frame_start[4]});
  RCB_CHECK_EQ(outcome.records, std::size_t{2});
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{0});
  RCB_CHECK(!outcome.clean_close);
  RCB_REQUIRE(outcome.kinds.size() == 2);
  RCB_CHECK(outcome.kinds[0] == rcb::RecordKind::Epoch);
  RCB_CHECK(outcome.kinds[1] == rcb::RecordKind::Decision);
  // What survived is exactly the prefix of the original log.
  RCB_CHECK(std::equal(log.buffer().begin(), log.buffer().end(), image.bytes.begin()));

  // A cut inside the header of the final record behaves the same way.
  rcb::MemoryByteLog header_log;
  const OpenOutcome header_outcome =
      OpenBytes(header_log, Resized(image.bytes, image.frame_start[5] + 7), limits);
  RCB_CHECK(header_outcome.ok);
  RCB_CHECK(header_outcome.torn_tail);
  RCB_CHECK(header_outcome.repaired);
  RCB_CHECK_EQ(header_log.size(), u64{image.frame_start[5]});
}

RCB_TEST(a_flipped_byte_in_a_non_final_payload_is_interior_corruption) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const std::size_t size_before = image.bytes.size();

  const std::array<std::size_t, 2> frames = {0, 2};
  for (const std::size_t frame : frames) {
    RCB_REQUIRE(image.payload_size[frame] > 0);
    const Bytes damaged = Flipped(image.bytes, image.payload_byte(frame, image.payload_size[frame] / 2));
    rcb::MemoryByteLog log;
    const OpenOutcome outcome = OpenBytes(log, damaged, limits);
    RCB_CHECK(!outcome.ok);
    RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
    RCB_CHECK_EQ(log.size(), u64{size_before});
    RCB_CHECK_EQ(outcome.log_size, size_before);
  }

  // The last byte of a non-final payload is caught too, and so is the first.
  rcb::MemoryByteLog first_log;
  const OpenOutcome first_outcome =
      OpenBytes(first_log, Flipped(image.bytes, image.payload_start[0]), limits);
  RCB_CHECK(!first_outcome.ok);
  RCB_CHECK_EQ(first_outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(first_log.size(), u64{size_before});

  rcb::MemoryByteLog last_log;
  const OpenOutcome last_outcome =
      OpenBytes(last_log, Flipped(image.bytes, image.payload_byte(2, image.payload_size[2] - 1)), limits);
  RCB_CHECK(!last_outcome.ok);
  RCB_CHECK_EQ(last_outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(last_log.size(), u64{size_before});
}

RCB_TEST(a_flipped_byte_in_the_final_payload_is_a_torn_tail) {
  rcb::JournalLimits limits;

  // (a) A log that ends with a close record.
  JournalImage closed;
  RCB_REQUIRE(BuildCommittedImage(&closed, limits, true));
  const std::size_t close_frame = closed.frames() - 1;
  RCB_REQUIRE(closed.payload_size[close_frame] > 0);
  RCB_CHECK(closed.end() == closed.bytes.size());
  rcb::MemoryByteLog close_log;
  const OpenOutcome close_outcome =
      OpenBytes(close_log, Flipped(closed.bytes, closed.payload_byte(close_frame, 0)), limits);
  RCB_CHECK(close_outcome.ok);
  RCB_CHECK(close_outcome.torn_tail);
  RCB_CHECK(close_outcome.repaired);
  RCB_CHECK_EQ(close_outcome.torn_tail_offset, u64{closed.frame_start[close_frame]});
  RCB_CHECK_EQ(close_log.size(), u64{closed.frame_start[close_frame]});
  RCB_CHECK_EQ(close_outcome.records, std::size_t{3});
  RCB_CHECK(!close_outcome.clean_close);

  // (b) A log whose final frame is a prepared record that was never marked.
  JournalImage pending;
  RCB_REQUIRE(BuildImageWithPendingTail(&pending, limits));
  const std::size_t pending_frame = pending.frames() - 1;
  RCB_REQUIRE(pending.payload_size[pending_frame] > 0);
  rcb::MemoryByteLog pending_log;
  const OpenOutcome pending_outcome = OpenBytes(
      pending_log, Flipped(pending.bytes, pending.payload_byte(pending_frame, pending.payload_size[pending_frame] - 1)),
      limits);
  RCB_CHECK(pending_outcome.ok);
  RCB_CHECK(pending_outcome.torn_tail);
  RCB_CHECK(pending_outcome.repaired);
  RCB_CHECK_EQ(pending_log.size(), u64{pending.frame_start[pending_frame]});
  RCB_CHECK_EQ(pending_outcome.records, std::size_t{2});
  RCB_CHECK_EQ(pending_outcome.prepared_discarded, std::size_t{0});
}

RCB_TEST(a_rewritten_chain_is_caught_even_when_the_checksum_agrees) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const std::size_t size_before = image.bytes.size();

  // (a) Zeroing the chain of a non-final record is interior corruption.
  for (const std::size_t frame : {std::size_t{0}, std::size_t{2}}) {
    rcb::MemoryByteLog log;
    const OpenOutcome outcome =
        OpenBytes(log, Zeroed(image.bytes, image.chain_byte(frame), rcb::Digest::kSize), limits);
    RCB_CHECK(!outcome.ok);
    RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
    RCB_CHECK_EQ(log.size(), u64{size_before});
  }

  // (b) Zeroing the chain of the final record is a torn tail: the marker is
  // dropped, and the prepared record it belonged to is discarded with it.
  const std::size_t final_frame = image.frames() - 1;
  rcb::MemoryByteLog final_log;
  const OpenOutcome final_outcome =
      OpenBytes(final_log, Zeroed(image.bytes, image.chain_byte(final_frame), rcb::Digest::kSize), limits);
  RCB_CHECK(final_outcome.ok);
  RCB_CHECK(final_outcome.torn_tail);
  RCB_CHECK(final_outcome.repaired);
  RCB_CHECK_EQ(final_log.size(), u64{image.frame_start[final_frame]});
  RCB_CHECK_EQ(final_outcome.records, std::size_t{2});
  RCB_CHECK_EQ(final_outcome.prepared_discarded, std::size_t{1});

  // (c) Rewriting a payload byte and recomputing its CRC defeats the checksum,
  // and the chain catches it anyway.
  rcb::MemoryByteLog forged_log;
  const OpenOutcome forged_outcome = OpenBytes(forged_log, Forged(image.bytes, image, 2, 1), limits);
  RCB_CHECK(!forged_outcome.ok);
  RCB_CHECK_EQ(forged_outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(forged_log.size(), u64{size_before});

  // (d) The same forgery on the final record is only a torn tail, which is why
  // the chain, and not the checksum, is what makes the interior case work.
  JournalImage pending;
  RCB_REQUIRE(BuildImageWithPendingTail(&pending, limits));
  const std::size_t pending_frame = pending.frames() - 1;
  RCB_REQUIRE(pending.payload_size[pending_frame] > 0);
  rcb::MemoryByteLog forged_tail_log;
  const OpenOutcome forged_tail_outcome =
      OpenBytes(forged_tail_log, Forged(pending.bytes, pending, pending_frame, 1), limits);
  RCB_CHECK(forged_tail_outcome.ok);
  RCB_CHECK(forged_tail_outcome.torn_tail);
  RCB_CHECK(forged_tail_outcome.repaired);
  RCB_CHECK_EQ(forged_tail_log.size(), u64{pending.frame_start[pending_frame]});
}

RCB_TEST(a_declared_payload_length_is_bounded_before_it_is_trusted) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const std::size_t size_before = image.bytes.size();
  const u64 over_limit = static_cast<u64>(limits.max_record_payload) + 1U;

  // (a) A non-final record that declares more than the payload limit.
  Bytes damaged = image.bytes;
  StoreLe(damaged, image.frame_start[2] + kPayloadLengthOffset, over_limit, 4);
  rcb::MemoryByteLog log;
  OpenOutcome outcome = OpenBytes(log, damaged, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(log.size(), u64{size_before});

  // (b) The final record declaring the same: still corruption, never a
  // repairable tail, because the length is refused before the tail rule runs.
  Bytes final_damaged = image.bytes;
  StoreLe(final_damaged, image.frame_start[5] + kPayloadLengthOffset, over_limit, 4);
  rcb::MemoryByteLog final_log;
  outcome = OpenBytes(final_log, final_damaged, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(final_log.size(), u64{size_before});

  // (c) An absurd length is refused the same way.
  Bytes absurd = image.bytes;
  StoreLe(absurd, image.frame_start[0] + kPayloadLengthOffset, 0xFFFFFFFFU, 4);
  rcb::MemoryByteLog absurd_log;
  outcome = OpenBytes(absurd_log, absurd, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(absurd_log.size(), u64{size_before});

  // (d) A length that is inside the limit but longer than the remaining bytes
  // is a torn tail, not corruption.
  Bytes torn = image.bytes;
  StoreLe(torn, image.frame_start[2] + kPayloadLengthOffset, static_cast<u64>(limits.max_record_payload), 4);
  rcb::MemoryByteLog torn_log;
  outcome = OpenBytes(torn_log, torn, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(torn_log.size(), u64{image.frame_start[2]});
  RCB_CHECK_EQ(outcome.records, std::size_t{1});
}

RCB_TEST(all_zero_and_all_garbage_images_are_classified_exactly) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const std::size_t boundary = image.end();
  RCB_CHECK(boundary == image.bytes.size());

  // A log that is nothing but zeros: no records, nothing to report as corrupt.
  rcb::MemoryByteLog zero_log;
  OpenOutcome outcome = OpenBytes(zero_log, Bytes(4096, static_cast<u8>(0)), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(outcome.torn_tail_offset, u64{0});
  RCB_CHECK_EQ(outcome.records, std::size_t{0});
  RCB_CHECK_EQ(zero_log.size(), u64{0});

  // Zeros after a valid record: a repairable tail, and the records survive.
  rcb::MemoryByteLog padded_log;
  outcome = OpenBytes(padded_log, Padded(image.bytes, 4096, static_cast<u8>(0)), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(outcome.torn_tail_offset, u64{boundary});
  RCB_CHECK_EQ(outcome.records, std::size_t{3});
  RCB_CHECK_EQ(padded_log.size(), u64{boundary});

  // Zeros followed by junk are no longer a tail: nothing may be repaired.
  const Bytes junked = Padded(Padded(image.bytes, 1024, static_cast<u8>(0)), 64, static_cast<u8>(0x5A));
  rcb::MemoryByteLog junked_log;
  outcome = OpenBytes(junked_log, junked, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(junked_log.size(), u64{junked.size()});

  // A single zero byte appended to a valid log is a torn header.
  rcb::MemoryByteLog one_log;
  outcome = OpenBytes(one_log, Padded(image.bytes, 1, static_cast<u8>(0)), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(one_log.size(), u64{boundary});
  RCB_CHECK_EQ(outcome.torn_tail_bytes, u64{1});
}

RCB_TEST(a_prepared_record_without_a_marker_is_discarded_and_never_replayed) {
  rcb::JournalLimits limits;
  std::string payload;
  rcb::AskKey key;
  RCB_REQUIRE(BuildDecisionPayload(&payload, &key));

  JournalBuilder builder(limits);
  RCB_REQUIRE(builder.ready());
  builder.journal().SetEpoch(4);
  RCB_REQUIRE(builder.Record(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(4), true));
  RCB_REQUIRE(builder.Record(rcb::RecordKind::Decision, 1, payload, false));
  const JournalImage image = builder.Finish();

  rcb::MemoryByteLog log;
  const OpenOutcome outcome = OpenBytes(log, image.bytes, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{1});
  RCB_REQUIRE(outcome.kinds.size() == 1);
  RCB_CHECK(outcome.kinds[0] == rcb::RecordKind::Epoch);
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{1});
  RCB_CHECK(!outcome.torn_tail);
  RCB_CHECK(!outcome.repaired);
  // The inert bytes stay in the log.
  RCB_CHECK_EQ(log.size(), u64{image.bytes.size()});

  // Replay never sees the discarded decision.
  rcb::MemoryByteLog replay_log;
  replay_log.SetContent(image.bytes);
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(replay_log, limits, false, 0);
  RCB_REQUIRE_OK_REF(opened);
  rcb::BrokerCore core;
  RCB_CHECK(opened.value()->ApplyTo(core).ok());
  RCB_CHECK_EQ(core.sequence(), u64{0});
  RCB_CHECK(core.RecentDecisions(8).empty());
  RCB_CHECK(core.StateDigest() == rcb::BrokerCore().StateDigest());
  RCB_CHECK(core.VerifyConservation().closed);
  RCB_CHECK(!core.FindDecisionByKey(key).ok());

  // The contrast that makes the previous paragraph mean something: the same
  // payload with its commit marker is present and is replayed.
  JournalBuilder marked(limits);
  RCB_REQUIRE(marked.ready());
  marked.journal().SetEpoch(4);
  RCB_REQUIRE(marked.Record(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(4), true));
  RCB_REQUIRE(marked.Record(rcb::RecordKind::Decision, 1, payload, true));
  const JournalImage committed = marked.Finish();
  rcb::MemoryByteLog committed_log;
  const OpenOutcome committed_outcome = OpenBytes(committed_log, committed.bytes, limits);
  RCB_CHECK(committed_outcome.ok);
  RCB_CHECK_EQ(committed_outcome.records, std::size_t{2});
  RCB_REQUIRE(committed_outcome.kinds.size() == 2);
  RCB_CHECK(committed_outcome.kinds[1] == rcb::RecordKind::Decision);
  RCB_CHECK_EQ(committed_outcome.prepared_discarded, std::size_t{0});
}

RCB_TEST(a_commit_marker_whose_sequence_does_not_match_is_interior_corruption) {
  rcb::JournalLimits limits;

  // A log that holds one prepared record and no marker yet.
  JournalBuilder builder(limits);
  RCB_REQUIRE(builder.ready());
  builder.journal().SetEpoch(4);
  RCB_REQUIRE(builder.Record(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(4), false));
  const JournalImage prepared = builder.Finish();
  RCB_REQUIRE(prepared.frames() == 1);

  // (a) A marker that claims sequence 7 for a prepared record that carries 0.
  Bytes candidate = prepared.bytes;
  const Bytes wrong = BuildMarkerFrame(prepared.bytes, prepared.chain_byte(0), 7, 4);
  candidate.insert(candidate.end(), wrong.begin(), wrong.end());
  rcb::MemoryByteLog wrong_log;
  OpenOutcome outcome = OpenBytes(wrong_log, candidate, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(wrong_log.size(), u64{candidate.size()});

  // (b) The control: the same hand-built frame with the matching sequence is
  // accepted, which proves (a) failed on the sequence and not on the framing.
  Bytes matching = prepared.bytes;
  const Bytes right = BuildMarkerFrame(prepared.bytes, prepared.chain_byte(0), 0, 4);
  matching.insert(matching.end(), right.begin(), right.end());
  rcb::MemoryByteLog right_log;
  outcome = OpenBytes(right_log, matching, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{1});
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{0});
  RCB_CHECK(!outcome.torn_tail);

  // (c) A marker with no prepared record before it at all.
  JournalImage committed;
  RCB_REQUIRE(BuildCommittedImage(&committed, limits, false));
  const std::size_t final_frame = committed.frames() - 1;
  Bytes orphan = committed.bytes;
  const Bytes stray = BuildMarkerFrame(committed.bytes, committed.chain_byte(final_frame), 99, 4);
  orphan.insert(orphan.end(), stray.begin(), stray.end());
  rcb::MemoryByteLog orphan_log;
  outcome = OpenBytes(orphan_log, orphan, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(orphan_log.size(), u64{orphan.size()});

  // (d) The same frame with a broken chain is only a torn tail, which is why
  // (a) and (c) had to be framed honestly to reach the marker check.
  Bytes broken = wrong;
  broken[kChainOffset] = static_cast<u8>(broken[kChainOffset] ^ 0x01U);
  Bytes broken_log_bytes = prepared.bytes;
  broken_log_bytes.insert(broken_log_bytes.end(), broken.begin(), broken.end());
  rcb::MemoryByteLog broken_log;
  outcome = OpenBytes(broken_log, broken_log_bytes, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(broken_log.size(), u64{prepared.bytes.size()});
}

RCB_TEST(absurd_images_are_classified_and_bounded) {
  rcb::JournalLimits limits;
  const u8 ones = static_cast<u8>(0xFFU);

  // 1 MiB of 0xFF with no valid record before it is a tail, not corruption.
  rcb::MemoryByteLog garbage_log;
  OpenOutcome outcome = OpenBytes(garbage_log, Bytes(1024 * 1024, ones), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(outcome.records, std::size_t{0});
  RCB_CHECK_EQ(garbage_log.size(), u64{0});

  // The same garbage after a valid record is corruption.
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const Bytes garbage_after = Padded(image.bytes, 1024 * 1024, ones);
  rcb::MemoryByteLog garbage_after_log;
  outcome = OpenBytes(garbage_after_log, garbage_after, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(garbage_after_log.size(), u64{garbage_after.size()});

  // A valid header followed by nothing.
  const Bytes header_only(image.bytes.begin(), image.bytes.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes));
  rcb::MemoryByteLog header_log;
  outcome = OpenBytes(header_log, header_only, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK(outcome.repaired);
  RCB_CHECK_EQ(outcome.torn_tail_offset, u64{0});
  RCB_CHECK_EQ(header_log.size(), u64{0});

  // Sixty-three bytes of a header, and an empty log.
  rcb::MemoryByteLog short_log;
  outcome = OpenBytes(short_log, Resized(image.bytes, kFrameHeaderBytes - 1), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(outcome.torn_tail);
  RCB_CHECK_EQ(short_log.size(), u64{0});

  rcb::MemoryByteLog empty_log;
  outcome = OpenBytes(empty_log, Bytes(), limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK(!outcome.torn_tail);
  RCB_CHECK(!outcome.repaired);
  RCB_CHECK_EQ(outcome.records, std::size_t{0});
  RCB_CHECK_EQ(empty_log.size(), u64{0});

  // A payload of exactly the limit is accepted; one byte more is refused before
  // anything is written.
  rcb::JournalLimits small;
  small.max_record_payload = 512;
  JournalBuilder exact(small);
  RCB_REQUIRE(exact.ready());
  exact.journal().SetEpoch(1);
  const std::string at_limit(512, 'a');
  RCB_REQUIRE(exact.Record(rcb::RecordKind::Decision, 1, at_limit, true));
  const JournalImage exact_image = exact.Finish();
  rcb::MemoryByteLog exact_log;
  outcome = OpenBytes(exact_log, exact_image.bytes, small);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{1});
  RCB_REQUIRE(outcome.payload_sizes.size() == 1);
  RCB_CHECK_EQ(outcome.payload_sizes[0], std::size_t{512});

  JournalBuilder oversized(small);
  RCB_REQUIRE(oversized.ready());
  oversized.journal().SetEpoch(1);
  const std::string over_limit(513, 'a');
  const rcb::Status refused = oversized.journal().Prepare(rcb::RecordKind::Decision, 1, over_limit);
  RCB_CHECK(!refused.ok());
  RCB_CHECK_EQ(refused.code(), ErrorCode::LimitExceeded);
  RCB_CHECK_EQ(oversized.log().size(), u64{0});
}

// =========================================================================
// C. injected I/O failures
// =========================================================================

constexpr std::size_t kFrameBytes = 64;

RCB_TEST(an_injected_append_failure_is_reported_and_writes_nothing) {
  rcb::JournalLimits limits;
  rcb::MemoryByteLog log;
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
  RCB_REQUIRE_OK_REF(opened);
  std::unique_ptr<rcb::Journal>& journal = opened.value();

  log.FailOnce(rcb::MemoryByteLog::FailPoint::Append);
  const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(1));
  RCB_CHECK(!prepared.ok());
  RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK_EQ(log.size(), u64{0});
  RCB_CHECK(!journal->has_pending());
  RCB_CHECK_EQ(journal->records().size(), std::size_t{0});

  // The injected failure is one-shot: the next attempt writes normally, which
  // proves the refusal above came from the platform call and not from a state
  // the journal got stuck in.
  RCB_CHECK(journal->Prepare(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(1)).ok());
  RCB_CHECK(journal->MarkCommitted(rcb::DurabilityClass::Buffered).ok());
  RCB_CHECK(!journal->has_pending());
  // In-process, records() carries the commit marker as well; a reopened journal
  // reports only the record the marker completed.
  RCB_CHECK_EQ(journal->records().size(), std::size_t{2});
  RCB_CHECK(journal->records()[0].kind == rcb::RecordKind::Epoch);
  RCB_CHECK(journal->records()[1].kind == rcb::RecordKind::CommitMarker);
}

RCB_TEST(an_injected_partial_append_leaves_a_repairable_tail) {
  rcb::JournalLimits limits;
  rcb::MemoryByteLog log;
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
  RCB_REQUIRE_OK_REF(opened);
  std::unique_ptr<rcb::Journal>& journal = opened.value();

  const std::string payload = rcb::EncodeEpochRecord(9);
  const std::size_t frame_bytes = kFrameBytes + payload.size();
  log.FailOnce(rcb::MemoryByteLog::FailPoint::PartialAppend);
  const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Epoch, 0, payload);
  RCB_CHECK(!prepared.ok());
  RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK_EQ(log.size(), u64{frame_bytes / 2});
  RCB_CHECK(!journal->has_pending());
  RCB_CHECK_EQ(journal->records().size(), std::size_t{0});

  // On its own, the half frame is a torn tail: recovery drops it and says so.
  rcb::MemoryByteLog half_log;
  const OpenOutcome half_outcome = OpenBytes(half_log, log.buffer(), limits);
  RCB_CHECK(half_outcome.ok);
  RCB_CHECK(half_outcome.torn_tail);
  RCB_CHECK(half_outcome.repaired);
  RCB_CHECK_EQ(half_outcome.records, std::size_t{0});
  RCB_CHECK_EQ(half_log.size(), u64{0});

  // A retry is never reported as success either: the journal's append offset is
  // stale after the half write, so the read-back verification refuses it.
  const rcb::Status retry = journal->Prepare(rcb::RecordKind::Epoch, 0, payload);
  RCB_CHECK(!retry.ok());
  RCB_CHECK_EQ(retry.code(), ErrorCode::PersistenceIoError);

  // And now the half frame is no longer the tail, because a complete frame
  // follows it. Recovery refuses to repair over the top of a record that might
  // be live, reports interior corruption, and truncates nothing. This is the
  // exact reason a failed append has to poison the session rather than be
  // retried on the same journal.
  const u64 after_retry = log.size();
  RCB_CHECK_EQ(after_retry, u64{frame_bytes + frame_bytes / 2});
  const OpenOutcome outcome = OpenImage(log, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(log.size(), after_retry);
}

RCB_TEST(an_injected_sync_failure_is_reported_after_the_marker_lands) {
  rcb::JournalLimits limits;
  rcb::MemoryByteLog log;
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
  RCB_REQUIRE_OK_REF(opened);
  std::unique_ptr<rcb::Journal>& journal = opened.value();

  RCB_REQUIRE(journal->Prepare(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(1)).ok());
  const u64 before = log.size();
  log.FailOnce(rcb::MemoryByteLog::FailPoint::Sync);
  const rcb::Status marked = journal->MarkCommitted(rcb::DurabilityClass::Durable);
  RCB_CHECK(!marked.ok());
  RCB_CHECK_EQ(marked.code(), ErrorCode::PersistenceIoError);
  // The marker bytes are in the log even though the caller was told the commit
  // failed: the durability barrier is what failed, not the append.
  RCB_CHECK_EQ(log.size(), before + kFrameBytes);
  RCB_CHECK(!journal->has_pending());
  const OpenOutcome outcome = OpenImage(log, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{1});
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{0});
  RCB_CHECK(!outcome.torn_tail);

  // Flush is the same barrier and fails the same way.
  log.FailOnce(rcb::MemoryByteLog::FailPoint::Sync);
  const rcb::Status flushed = journal->Flush();
  RCB_CHECK(!flushed.ok());
  RCB_CHECK_EQ(flushed.code(), ErrorCode::PersistenceIoError);
}

RCB_TEST(an_injected_truncate_failure_leaves_the_torn_tail_in_place) {
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  const Bytes damaged = Resized(image.bytes, image.payload_start[4] + 1);
  rcb::MemoryByteLog log;
  log.SetContent(damaged);
  log.FailOnce(rcb::MemoryByteLog::FailPoint::Truncate);
  const OpenOutcome outcome = OpenImage(log, limits);
  RCB_CHECK(!outcome.ok);
  RCB_CHECK_EQ(outcome.status.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK_EQ(outcome.log_size, damaged.size());
  RCB_CHECK_EQ(log.size(), u64{damaged.size()});
}

RCB_TEST(an_injected_read_failure_surfaces_from_open_and_from_prepare) {
  rcb::JournalLimits limits;

  // (a) Recovery cannot read the log at all.
  rcb::MemoryByteLog open_log;
  open_log.SetContent(Bytes(256, static_cast<u8>(0)));
  open_log.FailOnce(rcb::MemoryByteLog::FailPoint::Read);
  const OpenOutcome open_outcome = OpenImage(open_log, limits);
  RCB_CHECK(!open_outcome.ok);
  RCB_CHECK_EQ(open_outcome.status.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK_EQ(open_log.size(), u64{256});

  // (b) The read-back that verifies an append.
  rcb::MemoryByteLog log;
  rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
  RCB_REQUIRE_OK_REF(opened);
  std::unique_ptr<rcb::Journal>& journal = opened.value();
  const std::string payload = rcb::EncodeEpochRecord(3);
  log.FailOnce(rcb::MemoryByteLog::FailPoint::Read);
  const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Epoch, 0, payload);
  RCB_CHECK(!prepared.ok());
  RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK(!journal->has_pending());
  RCB_CHECK_EQ(journal->records().size(), std::size_t{0});
  // The bytes are in the log even though the journal refused to adopt them: an
  // unverified append is never adopted, and recovery discards the prepared
  // record instead of replaying it.
  RCB_CHECK_EQ(log.size(), u64{kFrameBytes + payload.size()});
  const OpenOutcome outcome = OpenImage(log, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{0});
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{1});
}

RCB_TEST(injected_failures_never_move_the_ledger_by_accident) {
  rcb::JournalLimits limits;
  const rcb::Offer offer = LiveOffer("site-a", 1, 100, 10);
  const rcb::Ask ask = LiveAsk("ask-txn", 40);

  // (a) An append failure during Prepare: the transaction stops before anything
  // is applied.
  {
    rcb::BrokerCore core;
    rcb::MemoryByteLog log;
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    RCB_REQUIRE(core.PublishOffer(offer).ok());
    const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    RCB_REQUIRE_OK_REF(plan);
    const rcb::Result<rcb::Decision> decision = core.MaterializeDecision(plan.value());
    RCB_REQUIRE_OK_REF(decision);
    const u64 sequence_before = core.sequence();
    log.FailOnce(rcb::MemoryByteLog::FailPoint::Append);
    const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Decision, core.next_sequence(),
                                                  rcb::EncodeDecisionRecord(decision.value()));
    RCB_CHECK(!prepared.ok());
    RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
    RCB_CHECK_EQ(core.sequence(), sequence_before);
    RCB_CHECK(core.VerifyConservation().closed);
    RCB_CHECK(core.RecentDecisions(8).empty());
  }

  // (b) A partial append during Prepare: still nothing applied.
  {
    rcb::BrokerCore core;
    rcb::MemoryByteLog log;
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    RCB_REQUIRE(core.PublishOffer(offer).ok());
    const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    RCB_REQUIRE_OK_REF(plan);
    const rcb::Result<rcb::Decision> decision = core.MaterializeDecision(plan.value());
    RCB_REQUIRE_OK_REF(decision);
    const u64 sequence_before = core.sequence();
    log.FailOnce(rcb::MemoryByteLog::FailPoint::PartialAppend);
    const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Decision, core.next_sequence(),
                                                  rcb::EncodeDecisionRecord(decision.value()));
    RCB_CHECK(!prepared.ok());
    RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
    RCB_CHECK_EQ(core.sequence(), sequence_before);
    RCB_CHECK(core.VerifyConservation().closed);
    RCB_CHECK(core.RecentDecisions(8).empty());
  }

  // (c) A sync failure after the decision was applied: the ledger is still
  // exactly conservative, and the caller was told the commit failed rather than
  // being told it succeeded.
  {
    rcb::BrokerCore core;
    rcb::MemoryByteLog log;
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    RCB_REQUIRE(core.PublishOffer(offer).ok());
    const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    RCB_REQUIRE_OK_REF(plan);
    const rcb::Result<rcb::Decision> decision = core.MaterializeDecision(plan.value());
    RCB_REQUIRE_OK_REF(decision);
    RCB_REQUIRE(journal
                    ->Prepare(rcb::RecordKind::Decision, decision.value().broker_sequence,
                              rcb::EncodeDecisionRecord(decision.value()))
                    .ok());
    RCB_REQUIRE(core.ApplyMaterializedDecision(decision.value()).ok());
    log.FailOnce(rcb::MemoryByteLog::FailPoint::Sync);
    const rcb::Status marked = journal->MarkCommitted(rcb::DurabilityClass::Durable);
    RCB_CHECK(!marked.ok());
    RCB_CHECK_EQ(marked.code(), ErrorCode::PersistenceIoError);
    const rcb::ConservationReport report = core.VerifyConservation();
    RCB_CHECK(report.closed);
    RCB_CHECK_EQ(report.violations.size(), std::size_t{0});
    RCB_CHECK(report.live_commitments > std::size_t{0});
    RCB_CHECK_EQ(core.sequence(), decision.value().broker_sequence);
    // The ledger still refuses to over-commit after the failure.
    const rcb::Result<rcb::AskPlan> over = core.PlanAsk(LiveAsk("ask-over", 1000));
    RCB_CHECK(over.ok());
    if (over.ok()) {
      RCB_CHECK(!over.value().unmet.IsZero());
    }
  }

  // (d) A read failure during the verification of an append: the transaction
  // stops, the ledger does not move, and the bytes that did reach the log are
  // discarded by recovery rather than adopted.
  {
    rcb::BrokerCore core;
    rcb::MemoryByteLog log;
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    RCB_REQUIRE(core.PublishOffer(offer).ok());
    const rcb::Result<rcb::AskPlan> plan = core.PlanAsk(ask);
    RCB_REQUIRE_OK_REF(plan);
    const rcb::Result<rcb::Decision> decision = core.MaterializeDecision(plan.value());
    RCB_REQUIRE_OK_REF(decision);
    const u64 sequence_before = core.sequence();
    log.FailOnce(rcb::MemoryByteLog::FailPoint::Read);
    const rcb::Status prepared = journal->Prepare(rcb::RecordKind::Decision, core.next_sequence(),
                                                  rcb::EncodeDecisionRecord(decision.value()));
    RCB_CHECK(!prepared.ok());
    RCB_CHECK_EQ(prepared.code(), ErrorCode::PersistenceIoError);
    RCB_CHECK_EQ(core.sequence(), sequence_before);
    RCB_CHECK(core.RecentDecisions(8).empty());
    const rcb::ConservationReport report = core.VerifyConservation();
    RCB_CHECK(report.closed);
    RCB_CHECK_EQ(report.violations.size(), std::size_t{0});
    const OpenOutcome outcome = OpenImage(log, limits);
    RCB_CHECK(outcome.ok);
    RCB_CHECK_EQ(outcome.records, std::size_t{0});
    RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{1});
  }

  // (e) A truncate failure while the journal is reset for compaction: the reset
  // is refused, the epoch does not move, and the ledger is untouched.
  {
    rcb::BrokerCore core;
    rcb::MemoryByteLog log;
    rcb::Result<std::unique_ptr<rcb::Journal>> opened = rcb::Journal::Open(log, limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    RCB_REQUIRE(core.PublishOffer(offer).ok());
    const u64 sequence_before = core.sequence();
    log.FailOnce(rcb::MemoryByteLog::FailPoint::Truncate);
    const rcb::Status reset = journal->Reset(9);
    RCB_CHECK(!reset.ok());
    RCB_CHECK_EQ(reset.code(), ErrorCode::PersistenceIoError);
    RCB_CHECK_EQ(core.sequence(), sequence_before);
    RCB_CHECK(core.VerifyConservation().closed);
    RCB_CHECK(!journal->has_pending());
  }
}

RCB_TEST(a_snapshot_publish_failure_keeps_the_published_snapshot_and_the_log) {
  rcb::MemorySnapshotStore store;
  const Bytes first = BytesOf("first snapshot payload");
  const Bytes second = BytesOf("second snapshot payload, which is longer");
  RCB_REQUIRE(store.Publish(first).ok());
  RCB_CHECK_EQ(store.publish_count(), std::size_t{1});
  RCB_CHECK(store.exists());
  RCB_CHECK(store.content() == first);

  // A one-shot publish failure leaves the published snapshot exactly as it was.
  store.FailOnce(rcb::MemorySnapshotStore::FailPoint::Publish);
  const rcb::Status failed = store.Publish(second);
  RCB_CHECK(!failed.ok());
  RCB_CHECK_EQ(failed.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK(store.content() == first);
  RCB_CHECK_EQ(store.publish_count(), std::size_t{1});
  RCB_CHECK(store.exists());
  const rcb::Result<std::optional<Bytes>> loaded = store.Load(1U << 20);
  RCB_REQUIRE_OK_REF(loaded);
  RCB_CHECK(loaded.value().has_value());
  RCB_CHECK(*loaded.value() == first);

  // A partial publish is the torn-write injection, and it is the one failure
  // that does replace the published bytes with a truncated prefix. The
  // file-backed store cannot do this, because it publishes by rename; the
  // memory adapter models the torn write itself, so the test pins that.
  store.FailOnce(rcb::MemorySnapshotStore::FailPoint::PartialPublish);
  const rcb::Status partial = store.Publish(second);
  RCB_CHECK(!partial.ok());
  RCB_CHECK_EQ(partial.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK_EQ(store.content().size(), second.size() / 2);
  RCB_CHECK(store.content() != second);
  RCB_CHECK_EQ(store.publish_count(), std::size_t{1});

  // Restore a known good snapshot, then hold the store in permanent failure.
  RCB_REQUIRE(store.Publish(second).ok());
  RCB_CHECK(store.content() == second);
  store.FailAlways(rcb::MemorySnapshotStore::FailPoint::Publish);
  RCB_CHECK(!store.Publish(first).ok());
  RCB_CHECK(store.content() == second);
  RCB_CHECK_EQ(store.publish_count(), std::size_t{2});
  store.FailAlways(rcb::MemorySnapshotStore::FailPoint::None);

  // A failing load is reported, never defaulted to "there is no snapshot".
  store.FailAlways(rcb::MemorySnapshotStore::FailPoint::Load);
  const rcb::Result<std::optional<Bytes>> unreadable = store.Load(1U << 20);
  RCB_CHECK(!unreadable.ok());
  RCB_CHECK_EQ(unreadable.status().code(), ErrorCode::PersistenceIoError);
  store.FailAlways(rcb::MemorySnapshotStore::FailPoint::None);

  // A failing publish never touches the log it describes.
  rcb::JournalLimits limits;
  JournalImage image;
  RCB_REQUIRE(BuildCommittedImage(&image, limits, false));
  rcb::MemoryByteLog log;
  log.SetContent(image.bytes);
  store.FailOnce(rcb::MemorySnapshotStore::FailPoint::Publish);
  RCB_CHECK(!store.Publish(BytesOf("snapshot")).ok());
  const OpenOutcome outcome = OpenImage(log, limits);
  RCB_CHECK(outcome.ok);
  RCB_CHECK_EQ(outcome.records, std::size_t{3});
  RCB_CHECK_EQ(log.size(), u64{image.bytes.size()});
  RCB_CHECK_EQ(outcome.prepared_discarded, std::size_t{0});
}

// =========================================================================
// D. store-level attacks
// =========================================================================

/// A scratch directory below the working directory. Relative paths keep every
/// byte ASCII, which is what the platform adapters expect.
class ScratchDirectory {
 public:
  explicit ScratchDirectory(const std::string& name) : path_("rcb_adversarial_" + name) {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
    std::filesystem::create_directories(path_, ignored);
    ready_ = !ignored;
  }

  ~ScratchDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScratchDirectory(const ScratchDirectory&) = delete;
  ScratchDirectory& operator=(const ScratchDirectory&) = delete;

  [[nodiscard]] bool ready() const { return ready_; }
  [[nodiscard]] std::string path() const { return path_.string(); }
  [[nodiscard]] std::string file(const std::string& name) const {
    return (path_ / name).string();
  }

 private:
  std::filesystem::path path_;
  bool ready_ = false;
};

RCB_TEST(read_whole_file_reports_missing_oversized_and_directory_paths) {
  ScratchDirectory scratch("read");
  RCB_REQUIRE(scratch.ready());

  const std::string missing = scratch.file("absent.bin");
  RCB_CHECK(!rcb::FileExists(missing));
  RCB_CHECK_ERROR(rcb::ReadWholeFile(missing, 4096), ErrorCode::NotFound);

  const std::string present = scratch.file("payload.bin");
  const Bytes payload = BytesOf("0123456789abcdef");
  RCB_REQUIRE(rcb::WriteFileAtomically(present, payload).ok());
  const rcb::Result<Bytes> exact = rcb::ReadWholeFile(present, payload.size());
  RCB_REQUIRE_OK_REF(exact);
  RCB_CHECK(exact.value() == payload);
  RCB_CHECK_ERROR(rcb::ReadWholeFile(present, payload.size() - 1), ErrorCode::LimitExceeded);
  RCB_CHECK_ERROR(rcb::ReadWholeFile(present, 0), ErrorCode::LimitExceeded);

  // A directory is not a file: it is refused rather than reported as missing.
  const rcb::Result<Bytes> directory = rcb::ReadWholeFile(scratch.path(), 4096);
  RCB_CHECK(!directory.ok());
  RCB_CHECK_EQ(directory.status().code(), ErrorCode::PersistenceIoError);
}

RCB_TEST(store_lock_is_exclusive_and_reusable) {
  ScratchDirectory scratch("lock");
  RCB_REQUIRE(scratch.ready());
  const std::string path = scratch.file("store.lock");

  rcb::Result<std::unique_ptr<rcb::StoreLock>> first = rcb::StoreLock::Acquire(path);
  RCB_REQUIRE_OK_REF(first);
  RCB_CHECK(first.value() != nullptr);

  rcb::Result<std::unique_ptr<rcb::StoreLock>> second = rcb::StoreLock::Acquire(path);
  RCB_CHECK(!second.ok());
  if (!second.ok()) {
    RCB_CHECK_EQ(second.status().code(), ErrorCode::PersistenceIoError);
  }

  // A third attempt also fails while the first is held.
  RCB_CHECK(!rcb::StoreLock::Acquire(path).ok());

  // Releasing the first makes the path acquirable again.
  first.value().reset();
  rcb::Result<std::unique_ptr<rcb::StoreLock>> third = rcb::StoreLock::Acquire(path);
  RCB_CHECK(third.ok());
  if (third.ok()) {
    RCB_CHECK(third.value() != nullptr);
  }
}

RCB_TEST(create_directories_refuses_a_path_that_is_a_file) {
  ScratchDirectory scratch("mkdir");
  RCB_REQUIRE(scratch.ready());

  const std::string file_path = scratch.file("plain.bin");
  RCB_REQUIRE(rcb::WriteFileAtomically(file_path, BytesOf("x")).ok());
  RCB_CHECK(rcb::FileExists(file_path));

  const rcb::Status created = rcb::CreateDirectories(file_path);
  RCB_CHECK(!created.ok());
  RCB_CHECK_EQ(created.code(), ErrorCode::PersistenceIoError);
  // The file is still a file: the refusal did not quietly replace it.
  RCB_CHECK(rcb::FileExists(file_path));
  const rcb::Result<Bytes> still_there = rcb::ReadWholeFile(file_path, 16);
  RCB_REQUIRE_OK_REF(still_there);
  RCB_CHECK(still_there.value() == BytesOf("x"));

  // An existing directory is fine, and missing parents are created.
  RCB_CHECK(rcb::CreateDirectories(scratch.path()).ok());
  RCB_CHECK(rcb::CreateDirectories(scratch.file("a/b/c")).ok());
  RCB_CHECK(rcb::CreateDirectories(scratch.file("a/b/c")).ok());

  // An empty path is an argument error, not a silent success.
  const rcb::Status empty = rcb::CreateDirectories("");
  RCB_CHECK(!empty.ok());
  RCB_CHECK_EQ(empty.code(), ErrorCode::InvalidArgument);
}

RCB_TEST(write_file_atomically_round_trips_bytes_including_empty) {
  ScratchDirectory scratch("roundtrip");
  RCB_REQUIRE(scratch.ready());
  const std::string path = scratch.file("bytes.bin");

  const Bytes payload{0x00U, 0x01U, 0x7FU, 0x80U, 0xFFU, 0x41U, 0x0AU};
  RCB_REQUIRE(rcb::WriteFileAtomically(path, payload).ok());
  const rcb::Result<Bytes> read = rcb::ReadWholeFile(path, 1024);
  RCB_REQUIRE_OK_REF(read);
  RCB_CHECK(read.value() == payload);

  // A shorter rewrite replaces rather than appends.
  RCB_REQUIRE(rcb::WriteFileAtomically(path, BytesOf("tiny")).ok());
  const rcb::Result<Bytes> shorter = rcb::ReadWholeFile(path, 1024);
  RCB_REQUIRE_OK_REF(shorter);
  RCB_CHECK(shorter.value() == BytesOf("tiny"));

  // An empty file round-trips, and writing it truncates what was there.
  RCB_REQUIRE(rcb::WriteFileAtomically(path, Bytes()).ok());
  const rcb::Result<Bytes> empty = rcb::ReadWholeFile(path, 1024);
  RCB_REQUIRE_OK_REF(empty);
  RCB_CHECK(empty.value().empty());
  RCB_CHECK(rcb::FileExists(path));

  // Removal succeeds, and removing an absent file still succeeds.
  RCB_CHECK(rcb::RemoveFile(path).ok());
  RCB_CHECK(!rcb::FileExists(path));
  RCB_CHECK(rcb::RemoveFile(path).ok());
  RCB_CHECK_ERROR(rcb::ReadWholeFile(path, 1024), ErrorCode::NotFound);

  // A path whose parent does not exist cannot be written.
  RCB_CHECK(!rcb::WriteFileAtomically(scratch.file("no/such/dir/file.bin"), BytesOf("x")).ok());
}

RCB_TEST(a_file_snapshot_publish_that_cannot_complete_leaves_the_previous_one) {
  ScratchDirectory scratch("snapshot");
  RCB_REQUIRE(scratch.ready());
  const std::string path = scratch.file("state.snap");
  rcb::FileSnapshotStore store(path);

  RCB_CHECK(!store.exists());
  const rcb::Result<std::optional<Bytes>> absent = store.Load(4096);
  RCB_REQUIRE_OK_REF(absent);
  RCB_CHECK(!absent.value().has_value());

  const Bytes first = BytesOf("first published snapshot");
  RCB_REQUIRE(store.Publish(first).ok());
  RCB_CHECK(store.exists());

  // Block the temporary path with a directory so the publish cannot complete.
  // The published file must survive: that is what write-verify-rename buys.
  RCB_REQUIRE(rcb::CreateDirectories(path + ".tmp").ok());
  const rcb::Status failed = store.Publish(BytesOf("second snapshot"));
  RCB_CHECK(!failed.ok());
  RCB_CHECK_EQ(failed.code(), ErrorCode::PersistenceIoError);
  RCB_CHECK(store.exists());
  const rcb::Result<std::optional<Bytes>> loaded = store.Load(4096);
  RCB_REQUIRE_OK_REF(loaded);
  RCB_CHECK(loaded.value().has_value());
  RCB_CHECK(*loaded.value() == first);

  // Once the obstacle is gone the store publishes again.
  std::error_code ignored;
  std::filesystem::remove_all(path + ".tmp", ignored);
  const Bytes second = BytesOf("second published snapshot");
  RCB_REQUIRE(store.Publish(second).ok());
  const rcb::Result<std::optional<Bytes>> reloaded = store.Load(4096);
  RCB_REQUIRE_OK_REF(reloaded);
  RCB_CHECK(reloaded.value().has_value());
  RCB_CHECK(*reloaded.value() == second);

  RCB_CHECK(store.Remove().ok());
  RCB_CHECK(!store.exists());
  RCB_CHECK(store.Remove().ok());
}

RCB_TEST(a_file_backed_journal_round_trips_and_refuses_corruption) {
  ScratchDirectory scratch("journal");
  RCB_REQUIRE(scratch.ready());
  const std::string path = scratch.file("journal.log");
  rcb::JournalLimits limits;

  {
    rcb::Result<std::unique_ptr<rcb::FileByteLog>> log = rcb::FileByteLog::Open(path, true);
    RCB_REQUIRE_OK_REF(log);
    rcb::Result<std::unique_ptr<rcb::Journal>> opened =
        rcb::Journal::Open(*log.value(), limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    std::unique_ptr<rcb::Journal>& journal = opened.value();
    journal->SetEpoch(3);
    RCB_REQUIRE(journal->Prepare(rcb::RecordKind::Epoch, 0, rcb::EncodeEpochRecord(3)).ok());
    RCB_REQUIRE(journal->MarkCommitted(rcb::DurabilityClass::Durable).ok());
    RCB_REQUIRE(journal->Prepare(rcb::RecordKind::Decision, 1, DecisionPayload(1)).ok());
    RCB_REQUIRE(journal->MarkCommitted(rcb::DurabilityClass::Durable).ok());
  }

  {
    rcb::Result<std::unique_ptr<rcb::FileByteLog>> log = rcb::FileByteLog::Open(path, false);
    RCB_REQUIRE_OK_REF(log);
    rcb::Result<std::unique_ptr<rcb::Journal>> opened =
        rcb::Journal::Open(*log.value(), limits, false, 0);
    RCB_REQUIRE_OK_REF(opened);
    RCB_CHECK_EQ(opened.value()->records().size(), std::size_t{2});
    RCB_CHECK_EQ(opened.value()->recovery().prepared_discarded, std::size_t{0});
    RCB_CHECK(!opened.value()->recovery().torn_tail);
    RCB_CHECK_EQ(opened.value()->epoch(), u64{3});
  }

  // Flip one byte in the first record's payload on disk: an interior record is
  // corruption, and the file on disk must not be repaired.
  const rcb::Result<Bytes> on_disk = rcb::ReadWholeFile(path, 1U << 20);
  RCB_REQUIRE_OK_REF(on_disk);
  Bytes damaged = on_disk.value();
  RCB_REQUIRE(damaged.size() > kFrameBytes);
  damaged[kFrameBytes + 1] = static_cast<u8>(damaged[kFrameBytes + 1] ^ 0x01U);
  RCB_REQUIRE(rcb::WriteFileAtomically(path, damaged).ok());
  rcb::Result<std::unique_ptr<rcb::FileByteLog>> log = rcb::FileByteLog::Open(path, false);
  RCB_REQUIRE_OK_REF(log);
  const rcb::Result<std::unique_ptr<rcb::Journal>> opened =
      rcb::Journal::Open(*log.value(), limits, false, 0);
  RCB_CHECK(!opened.ok());
  RCB_CHECK_EQ(opened.status().code(), ErrorCode::PersistenceInteriorCorruption);
  RCB_CHECK_EQ(log.value()->size(), u64{damaged.size()});
}

// =========================================================================
// E. large-input bounds
// =========================================================================

RCB_TEST(an_offer_with_too_many_tranches_is_refused_before_it_is_built) {
  std::vector<rcb::JsonValue> tranches;
  for (std::size_t index = 0; index <= rcb::Limits::kMaxTranchesPerOffer; ++index) {
    tranches.push_back(TrancheJson(NumberedName("fd-", index), 1, 0));
  }
  RCB_CHECK_EQ(tranches.size(), rcb::Limits::kMaxTranchesPerOffer + 1);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(OfferJson(), "tranches", ArrayOf(tranches))),
      ErrorCode::LimitExceeded);

  // Exactly the bound is accepted, so the refusal is the bound and not a
  // blanket "too big".
  tranches.pop_back();
  const rcb::Result<rcb::Offer> at_bound =
      rcb::DecodeOfferJson(WithField(OfferJson(), "tranches", ArrayOf(tranches)));
  RCB_CHECK(at_bound.ok());
  if (at_bound.ok()) {
    RCB_CHECK_EQ(at_bound.value().tranches.size(), rcb::Limits::kMaxTranchesPerOffer);
  }

  // A core built after the attack is still empty.
  rcb::BrokerCore probe;
  RCB_CHECK_EQ(probe.sequence(), u64{0});
  RCB_CHECK(probe.StateDigest() == rcb::BrokerCore().StateDigest());
  RCB_CHECK(probe.VerifyConservation().closed);

  // The structural validator applies the same bound when the structure is built
  // directly rather than decoded.
  rcb::Offer wide = LiveOffer("site-a", 1, 1, 0);
  wide.tranches.clear();
  for (std::size_t index = 0; index <= rcb::Limits::kMaxTranchesPerOffer; ++index) {
    rcb::CapacityTranche tranche;
    tranche.domain = Id<rcb::FailureDomainIdTag>(NumberedName("fd-", index));
    tranche.allocatable = Cap(1);
    wide.tranches.push_back(tranche);
  }
  const rcb::Status wide_status = rcb::ValidateOffer(wide);
  RCB_CHECK(!wide_status.ok());
  RCB_CHECK_EQ(wide_status.code(), ErrorCode::LimitExceeded);
}

std::string NestedDocument(const std::size_t depth) {
  return std::string(depth, '[') + "1" + std::string(depth, ']');
}

RCB_TEST(json_size_depth_string_key_and_element_limits_are_enforced) {
  // max_bytes.
  rcb::JsonLimits byte_limits;
  byte_limits.max_bytes = 64;
  const std::string at_byte_limit = "\"" + std::string(62, 'a') + "\"";
  RCB_CHECK_EQ(at_byte_limit.size(), std::size_t{64});
  RCB_CHECK(rcb::ParseJson(at_byte_limit, byte_limits).ok());
  const std::string over_byte_limit = "\"" + std::string(63, 'a') + "\"";
  RCB_CHECK_EQ(over_byte_limit.size(), std::size_t{65});
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(over_byte_limit, byte_limits),
                              ErrorCode::LimitExceeded);
  const rcb::JsonLimits defaults;
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(std::string(defaults.max_bytes + 1, ' '), defaults),
                              ErrorCode::LimitExceeded);

  // max_depth: the bound is the parser's own, and the document is refused
  // before the parser recurses into it.
  rcb::JsonLimits depth_limits;
  depth_limits.max_depth = 8;
  RCB_CHECK(rcb::ParseJson(NestedDocument(8), depth_limits).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(NestedDocument(9), depth_limits),
                              ErrorCode::LimitExceeded);
  RCB_CHECK(rcb::ParseJson(NestedDocument(defaults.max_depth), defaults).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson(NestedDocument(defaults.max_depth + 1), defaults),
                              ErrorCode::LimitExceeded);
  // The documented nesting bound and the parser's default are the same number,
  // and a document nested one level beyond it is refused. A static assertion in
  // src/json.cpp keeps the two from drifting apart again.
  RCB_CHECK_EQ(rcb::Limits::kMaxJsonDepth, defaults.max_depth);
  RCB_CHECK(rcb::ParseJson(NestedDocument(rcb::Limits::kMaxJsonDepth), defaults).ok());
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::ParseJson(NestedDocument(rcb::Limits::kMaxJsonDepth + 1), defaults),
      ErrorCode::LimitExceeded);

  // max_string_bytes.
  rcb::JsonLimits string_limits;
  string_limits.max_string_bytes = 32;
  RCB_CHECK(rcb::ParseJson("\"" + std::string(32, 'a') + "\"", string_limits).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("\"" + std::string(33, 'a') + "\"", string_limits),
                              ErrorCode::LimitExceeded);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::ParseJson("\"" + std::string(defaults.max_string_bytes + 1, 'b') + "\"", defaults),
      ErrorCode::LimitExceeded);

  // max_key_bytes.
  rcb::JsonLimits key_limits;
  key_limits.max_key_bytes = 8;
  RCB_CHECK(rcb::ParseJson("{\"12345678\":1}", key_limits).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"123456789\":1}", key_limits),
                              ErrorCode::LimitExceeded);

  // max_elements.
  rcb::JsonLimits element_limits;
  element_limits.max_elements = 4;
  RCB_CHECK(rcb::ParseJson("[1,2,3,4]", element_limits).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("[1,2,3,4,5]", element_limits),
                              ErrorCode::LimitExceeded);
  RCB_CHECK(rcb::ParseJson("{\"a\":1,\"b\":2,\"c\":3,\"d\":4}", element_limits).ok());
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}", element_limits),
                              ErrorCode::LimitExceeded);
}

RCB_TEST(ask_list_and_generation_pin_counts_are_bounded) {
  const rcb::JsonValue ask = AskJson();

  std::vector<rcb::JsonValue> sites;
  for (std::size_t index = 0; index < rcb::Limits::kMaxListElements; ++index) {
    sites.push_back(rcb::JsonValue::MakeString(NumberedName("site-", index)));
  }
  const rcb::Result<rcb::Ask> at_bound =
      rcb::DecodeAskJson(WithField(ask, "allowed_sites", ArrayOf(sites)));
  RCB_CHECK(at_bound.ok());
  if (at_bound.ok()) {
    RCB_CHECK_EQ(at_bound.value().allowed_sites.size(), rcb::Limits::kMaxListElements);
  }
  sites.push_back(rcb::JsonValue::MakeString(NumberedName("site-", rcb::Limits::kMaxListElements)));
  RCB_CHECK_REFUSED_AND_INERT(rcb::DecodeAskJson(WithField(ask, "allowed_sites", ArrayOf(sites))),
                              ErrorCode::LimitExceeded);

  std::vector<rcb::JsonValue> pins;
  for (std::size_t index = 0; index < rcb::Limits::kMaxGenerationPins; ++index) {
    pins.push_back(PinJson(NumberedName("site-", index), 1));
  }
  const rcb::Result<rcb::Ask> pins_at_bound =
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf(pins)));
  RCB_CHECK(pins_at_bound.ok());
  if (pins_at_bound.ok()) {
    RCB_CHECK_EQ(pins_at_bound.value().generation_pins.size(), rcb::Limits::kMaxGenerationPins);
  }
  pins.push_back(PinJson(NumberedName("site-", rcb::Limits::kMaxGenerationPins), 1));
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(ask, "generation_pins", ArrayOf(pins))),
      ErrorCode::LimitExceeded);
}

// =========================================================================
// F. identifier and unicode edges
// =========================================================================

struct Utf8Sample {
  const char* bytes;
  std::size_t size;
  bool valid;
};

RCB_TEST(is_valid_utf8_classifies_the_byte_table) {
  const Utf8Sample samples[] = {
      {"", 0, true},
      {"plain ascii", 11, true},
      {"\x7F", 1, true},
      {"\xC2\x80", 2, true},
      {"\xDF\xBF", 2, true},
      {"\xE0\xA0\x80", 3, true},
      {"\xE2\x82\xAC", 3, true},
      {"\xEF\xBF\xBD", 3, true},
      // A byte order mark is a valid code point (U+FEFF), even though it is not
      // JSON whitespace.
      {"\xEF\xBB\xBF", 3, true},
      {"\xF0\x90\x80\x80", 4, true},
      {"\xF4\x8F\xBF\xBF", 4, true},
      {"caf\xC3\xA9", 5, true},
      // Lone continuation bytes and impossible leads.
      {"\x80", 1, false},
      {"\xBF", 1, false},
      {"\xFF", 1, false},
      {"\xFE", 1, false},
      // Overlong encodings.
      {"\xC0\x80", 2, false},
      {"\xC1\xBF", 2, false},
      {"\xE0\x80\xAF", 3, false},
      {"\xF0\x80\x80\x80", 4, false},
      // Surrogates encoded as UTF-8, alone and as a CESU-8 pair.
      {"\xED\xA0\x80", 3, false},
      {"\xED\xBF\xBF", 3, false},
      {"\xED\xA0\x80\xED\xB0\x80", 6, false},
      // Five- and six-byte sequences.
      {"\xF8\x88\x80\x80\x80", 5, false},
      {"\xFC\x84\x80\x80\x80\x80", 6, false},
      // Truncated sequences, at the end and in the middle.
      {"\xC3", 1, false},
      {"\xE2\x82", 2, false},
      {"\xF0\x9F\x92", 3, false},
      {"a\xC3", 2, false},
      {"\xC2\x41", 2, false},
      {"\xE0\xA0\x41", 3, false},
      // Code points beyond U+10FFFF.
      {"\xF4\x90\x80\x80", 4, false},
      {"\xF5\x80\x80\x80", 4, false},
      {"\xC3\x28", 2, false},
  };

  for (const Utf8Sample& sample : samples) {
    const std::string_view text(sample.bytes, sample.size);
    RCB_CHECK_EQ(rcb::IsValidUtf8(text), sample.valid);
  }
}

RCB_TEST(identifiers_and_json_strings_reject_multibyte_and_invalid_unicode) {
  // Identifiers are ASCII by construction, so every multi-byte sequence is a
  // character outside the allowed set.
  RCB_CHECK_ERROR(rcb::Identifier::Make("caf\xC3\xA9"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xE2\x82\xAC"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xF0\x9F\x92\xA9"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xEF\xBB\xBF" "site"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xC3\x28"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_ERROR(rcb::Identifier::Make("\xED\xA0\x80"), ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeOfferJson(WithField(OfferJson(), "site", rcb::JsonValue::MakeString("caf\xC3\xA9"))),
      ErrorCode::InvalidIdentifier);
  RCB_CHECK_REFUSED_AND_INERT(
      rcb::DecodeAskJson(WithField(AskJson(), "key", rcb::JsonValue::MakeString("\xE2\x82\xAC"))),
      ErrorCode::InvalidIdentifier);

  // The JSON front end refuses the same bytes as unicode errors, and refuses an
  // escaped surrogate that has no partner.
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\xC3\x28\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\xC0\x80\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\xED\xA0\x80\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\xF8\x88\x80\x80\x80\"}"),
                              ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\xE2\x82\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\\uD800\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\\uDC00\"}"), ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"\\uD800\\u0041\"}"),
                              ErrorCode::InvalidUnicode);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"a\nb\"}"), ErrorCode::MalformedInput);
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("{\"site\":\"a\x01" "b\"}"), ErrorCode::MalformedInput);

  // A BOM is well-formed UTF-8 and is still not a JSON document.
  RCB_CHECK(rcb::IsValidUtf8("\xEF\xBB\xBF"));
  RCB_CHECK_REFUSED_AND_INERT(rcb::ParseJson("\xEF\xBB\xBF" "{}"), ErrorCode::MalformedInput);

  // A paired surrogate is accepted and becomes the code point it names.
  const rcb::Result<rcb::JsonValue> paired = rcb::ParseJson("{\"a\":\"\\uD83D\\uDCA9\"}");
  RCB_CHECK(paired.ok());
  if (paired.ok()) {
    const rcb::JsonValue* member = paired.value().Find("a");
    RCB_REQUIRE(member != nullptr);
    RCB_CHECK(member->IsString());
    RCB_CHECK_EQ(member->AsString().size(), std::size_t{4});
    RCB_CHECK(rcb::IsValidUtf8(member->AsString()));
  }
}

}  // namespace
