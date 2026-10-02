// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// rcb -- the operator front end of the Regional Capacity Broker.
//
// Every command opens the store, does one thing, reports it as canonical JSON
// and closes. That is deliberate: the CLI is the multiprocess proof surface, so
// each invocation exercises a real open, a real recovery and a real close.
//
// Exit codes: 0 success; 2 usage error; 3 a request was refused by a named
// constraint; 4 store or persistence failure; 5 an invariant (conservation)
// failed; 6 unsupported request.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "rcb/rcb.hpp"

namespace {

using rcb::ErrorCode;
using rcb::JsonValue;
using rcb::Result;
using rcb::Status;

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitRefused = 3;
constexpr int kExitStore = 4;
constexpr int kExitInvariant = 5;
constexpr int kExitUnsupported = 6;

struct Options {
  std::string store;
  rcb::DurabilityClass durability = rcb::DurabilityClass::Durable;
  bool pretty = false;
  bool quiet = false;
  bool full = false;
  std::string command;
  std::map<std::string, std::string> values;
  std::vector<std::string> positional;
};

std::string Usage() {
  return
      "rcb -- regional capacity broker\n"
      "\n"
      "usage: rcb [options] <command> [command options]\n"
      "\n"
      "options:\n"
      "  --store DIR            durable store directory (default: volatile memory)\n"
      "  --durability durable|buffered   durability boundary (default: durable)\n"
      "  --pretty               pretty-print JSON output\n"
      "  --quiet                print only the essential field(s)\n"
      "  --now T                override the logical instant of an ask document\n"
      "\n"
      "commands:\n"
      "  version                          build and format versions\n"
      "  status                           accounting summary\n"
      "  ledger [--site S]                ledger terms, per site or summarised\n"
      "  decisions [--limit N]            most recent decisions\n"
      "  verify                           re-derive the conservation identity\n"
      "  recover                          open the store and report recovery\n"
      "  compact                          publish a snapshot and reset the log\n"
      "  state [--full]                   canonical state digest (and state)\n"
      "  offer --file F                   publish an offer document\n"
      "  revoke --site S --generation G --at T [--reason R]\n"
      "  ask --file F                     submit one ask document\n"
      "  asks --file F                    submit an array of ask documents\n"
      "  digest --file F                  canonical digest of an offer or ask\n"
      "  selftest                         exercise the boundary end to end in process\n";
}

int ExitCodeFor(const Status& status) {
  switch (status.category()) {
    case rcb::ErrorCategory::Ok:
      return kExitOk;
    case rcb::ErrorCategory::Unmet:
      return kExitRefused;
    case rcb::ErrorCategory::Persistence:
      return kExitStore;
    case rcb::ErrorCategory::Invariant:
      return kExitInvariant;
    case rcb::ErrorCategory::Unsupported:
      return kExitUnsupported;
    default:
      return kExitUsage;
  }
}

void PrintError(const std::string& command, const Status& status) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("command", JsonValue::MakeString(command));
  object.Set("ok", JsonValue::MakeBool(false));
  object.Set("error", JsonValue::MakeString(std::string(status.token())));
  object.Set("category", JsonValue::MakeString(std::string(rcb::ErrorCategoryToken(status.category()))));
  object.Set("detail", JsonValue::MakeString(status.detail()));
  std::cout << object.ToText(false) << "\n";
}

/// Parses the command line. Flags are either switches or take exactly one value.
Result<Options> ParseOptions(const std::vector<std::string>& args) {
  static const std::vector<std::string> kSwitches = {"--pretty", "--quiet", "--full", "--quick"};
  static const std::vector<std::string> kValued = {
      "--store",   "--durability", "--file",  "--site",  "--generation", "--at",
      "--reason",  "--key",        "--limit", "--now",   "--repeat",     "--out",
      "--sites",   "--asks",       "--tranches", "--domains", "--seed",   "--service-class"};

  Options options;
  std::size_t index = 0;
  while (index < args.size()) {
    const std::string& token = args[index];
    if (token == "--help" || token == "-h") {
      options.command = "help";
      return options;
    }
    if (token.size() > 2 && token[0] == '-' && token[1] == '-') {
      const auto valued = std::find(kValued.begin(), kValued.end(), token);
      if (valued != kValued.end()) {
        if (index + 1 >= args.size()) {
          return rcb::Fail<Options>(ErrorCode::InvalidArgument, token + " needs a value");
        }
        options.values[token] = args[index + 1];
        index += 2;
        continue;
      }
      const auto switched = std::find(kSwitches.begin(), kSwitches.end(), token);
      if (switched != kSwitches.end()) {
        options.values[token] = "true";
        index += 1;
        continue;
      }
      return rcb::Fail<Options>(ErrorCode::InvalidArgument, "unknown option " + token);
    }
    if (options.command.empty()) {
      options.command = token;
    } else {
      options.positional.push_back(token);
    }
    index += 1;
  }

  options.pretty = options.values.count("--pretty") != 0;
  options.quiet = options.values.count("--quiet") != 0;
  options.full = options.values.count("--full") != 0;
  if (options.values.count("--store") != 0) {
    options.store = options.values["--store"];
  }
  if (options.values.count("--durability") != 0) {
    const std::string& text = options.values["--durability"];
    if (text == "durable") {
      options.durability = rcb::DurabilityClass::Durable;
    } else if (text == "buffered") {
      options.durability = rcb::DurabilityClass::Buffered;
    } else {
      return rcb::Fail<Options>(ErrorCode::InvalidArgument,
                                "--durability must be durable or buffered");
    }
  }
  if (options.command.empty()) {
    options.command = "help";
  }
  return options;
}

bool Has(const Options& options, const char* key) { return options.values.count(key) != 0; }

std::string Value(const Options& options, const char* key, const std::string& fallback = "") {
  const auto found = options.values.find(key);
  return found == options.values.end() ? fallback : found->second;
}

Result<long long> ValueAsInt(const Options& options, const char* key, const long long fallback) {
  if (!Has(options, key)) {
    return fallback;
  }
  const std::string& text = options.values.at(key);
  if (text.empty()) {
    return rcb::Fail<long long>(ErrorCode::InvalidArgument, std::string(key) + " is empty");
  }
  std::size_t consumed = 0;
  long long value = 0;
  try {
    value = std::stoll(text, &consumed);
  } catch (const std::exception&) {
    return rcb::Fail<long long>(ErrorCode::InvalidArgument,
                                std::string(key) + " must be an integer");
  }
  if (consumed != text.size()) {
    return rcb::Fail<long long>(ErrorCode::InvalidArgument,
                                std::string(key) + " must be an integer");
  }
  return value;
}

Result<JsonValue> LoadDocument(const Options& options) {
  if (!Has(options, "--file")) {
    return rcb::Fail<JsonValue>(ErrorCode::MissingField, "--file is required");
  }
  Result<rcb::Bytes> bytes = rcb::ReadWholeFile(Value(options, "--file"),
                                                rcb::Limits::kMaxDocumentBytes);
  if (!bytes.ok()) {
    return Result<JsonValue>(bytes.status());
  }
  const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
  return rcb::ParseJson(text);
}

void Print(const JsonValue& value, const Options& options) {
  std::cout << value.ToText(options.pretty) << (options.pretty ? "" : "\n");
}

rcb::ServiceConfig MakeConfig(const Options& options) {
  rcb::ServiceConfig config;
  config.store_directory = options.store;
  config.durability = options.durability;
  config.worker_threads = 2;
  config.queue_capacity = 64;
  return config;
}

Result<std::unique_ptr<rcb::BrokerService>> OpenService(const Options& options) {
  return rcb::BrokerService::Open(MakeConfig(options));
}

JsonValue RecoveryJson(const rcb::RecoveryReport& report) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("clean_close", JsonValue::MakeBool(report.clean_close));
  object.Set("snapshot_sequence_known", JsonValue::MakeBool(report.snapshot_sequence_known));
  object.Set("snapshot_sequence", JsonValue::MakeUint(report.snapshot_sequence));
  object.Set("records_verified", JsonValue::MakeUint(report.records_verified));
  object.Set("records_skipped", JsonValue::MakeUint(report.records_skipped));
  object.Set("prepared_discarded", JsonValue::MakeUint(report.prepared_discarded));
  object.Set("torn_tail", JsonValue::MakeBool(report.torn_tail));
  object.Set("torn_tail_offset", JsonValue::MakeUint(report.torn_tail_offset));
  object.Set("torn_tail_bytes", JsonValue::MakeUint(report.torn_tail_bytes));
  object.Set("torn_tail_repaired", JsonValue::MakeBool(report.repaired));
  object.Set("last_sequence", JsonValue::MakeUint(report.last_sequence));
  object.Set("last_epoch", JsonValue::MakeUint(report.last_epoch));
  object.Set("journal_chain", JsonValue::MakeString(report.chain.Hex()));
  return object;
}

JsonValue DecisionJson(const rcb::Decision& decision) {
  JsonValue object = rcb::ToJson(decision);
  object.Set("committed_anything", JsonValue::MakeBool(decision.committed_anything()));
  return object;
}

// ---- commands ------------------------------------------------------------

int CommandVersion(const Options&) {
  JsonValue object = JsonValue::MakeObject();
  object.Set("name", JsonValue::MakeString("regional-capacity-broker"));
  object.Set("version", JsonValue::MakeString(rcb::VersionString()));
  object.Set("persistence_format", JsonValue::MakeUint(rcb::kPersistenceFormatVersion));
  object.Set("request_schema", JsonValue::MakeUint(rcb::kRequestSchemaVersion));
  object.Set("third_party_dependencies", JsonValue::MakeUint(0));
  std::cout << object.ToText(false) << "\n";
  return kExitOk;
}

int CommandStatus(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("status", service.status());
    return ExitCodeFor(service.status());
  }
  Result<rcb::AccountingSummary> summary = service.value()->Summary();
  if (!summary.ok()) {
    PrintError("status", summary.status());
    return ExitCodeFor(summary.status());
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("durable", JsonValue::MakeBool(service.value()->durable()));
  object.Set("durability", JsonValue::MakeString(std::string(
                               rcb::DurabilityToken(service.value()->durable()
                                                        ? options.durability
                                                        : rcb::DurabilityClass::Volatile))));
  object.Set("accounting", rcb::ToJson(summary.value()));
  object.Set("recovery", RecoveryJson(service.value()->Recovery()));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandLedger(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("ledger", service.status());
    return ExitCodeFor(service.status());
  }
  if (Has(options, "--site")) {
    Result<rcb::SiteId> site = rcb::SiteId::Make(Value(options, "--site"));
    if (!site.ok()) {
      PrintError("ledger", site.status());
      return ExitCodeFor(site.status());
    }
    Result<rcb::SiteLedger> ledger = service.value()->FindSite(site.value());
    if (!ledger.ok()) {
      PrintError("ledger", ledger.status());
      return ExitCodeFor(ledger.status());
    }
    Print(rcb::ToJson(ledger.value()), options);
    service.value()->Shutdown();
    return kExitOk;
  }
  Result<std::string> state = service.value()->CanonicalState();
  if (!state.ok()) {
    PrintError("ledger", state.status());
    return ExitCodeFor(state.status());
  }
  Result<JsonValue> parsed = rcb::ParseJson(state.value());
  if (!parsed.ok()) {
    PrintError("ledger", parsed.status());
    return ExitCodeFor(parsed.status());
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("sites", *parsed.value().Find("sites"));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandDecisions(const Options& options) {
  Result<long long> limit = ValueAsInt(options, "--limit", 10);
  if (!limit.ok()) {
    PrintError("decisions", limit.status());
    return ExitCodeFor(limit.status());
  }
  if (limit.value() < 0) {
    PrintError("decisions", Status::Error(ErrorCode::InvalidArgument, "--limit must not be negative"));
    return kExitUsage;
  }
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("decisions", service.status());
    return ExitCodeFor(service.status());
  }
  Result<std::vector<rcb::Decision>> decisions =
      service.value()->RecentDecisions(static_cast<std::size_t>(limit.value()));
  if (!decisions.ok()) {
    PrintError("decisions", decisions.status());
    return ExitCodeFor(decisions.status());
  }
  JsonValue array = JsonValue::MakeArray();
  for (const rcb::Decision& decision : decisions.value()) {
    array.Push(DecisionJson(decision));
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("count", JsonValue::MakeUint(array.items().size()));
  object.Set("decisions", std::move(array));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandVerify(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("verify", service.status());
    return ExitCodeFor(service.status());
  }
  Result<rcb::ConservationReport> report = service.value()->VerifyConservation();
  if (!report.ok()) {
    PrintError("verify", report.status());
    return ExitCodeFor(report.status());
  }
  JsonValue object = rcb::ToJson(report.value());
  object.Set("ok", JsonValue::MakeBool(report.value().closed));
  Print(object, options);
  const bool closed = report.value().closed;
  service.value()->Shutdown();
  return closed ? kExitOk : kExitInvariant;
}

int CommandRecover(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("recover", service.status());
    return ExitCodeFor(service.status());
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("recovery", RecoveryJson(service.value()->Recovery()));
  Result<rcb::AccountingSummary> summary = service.value()->Summary();
  if (summary.ok()) {
    object.Set("accounting", rcb::ToJson(summary.value()));
  }
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandCompact(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("compact", service.status());
    return ExitCodeFor(service.status());
  }
  const Status compacted = service.value()->Compact();
  if (!compacted.ok()) {
    PrintError("compact", compacted);
    return ExitCodeFor(compacted);
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("compacted", JsonValue::MakeBool(true));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandState(const Options& options) {
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("state", service.status());
    return ExitCodeFor(service.status());
  }
  Result<std::string> state = service.value()->CanonicalState();
  if (!state.ok()) {
    PrintError("state", state.status());
    return ExitCodeFor(state.status());
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(true));
  object.Set("state_digest", JsonValue::MakeString(rcb::Sha256Of(state.value()).Hex()));
  object.Set("state_bytes", JsonValue::MakeUint(state.value().size()));
  if (options.full) {
    object.Set("state", JsonValue::MakeString(state.value()));
  }
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandOffer(const Options& options) {
  Result<JsonValue> document = LoadDocument(options);
  if (!document.ok()) {
    PrintError("offer", document.status());
    return ExitCodeFor(document.status());
  }
  Result<rcb::Offer> offer = rcb::DecodeOfferJson(document.value());
  if (!offer.ok()) {
    PrintError("offer", offer.status());
    return ExitCodeFor(offer.status());
  }
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("offer", service.status());
    return ExitCodeFor(service.status());
  }
  Result<rcb::OfferPublication> published = service.value()->PublishOffer(offer.value());
  if (!published.ok()) {
    PrintError("offer", published.status());
    service.value()->Shutdown();
    return ExitCodeFor(published.status());
  }
  JsonValue object = rcb::ToJson(published.value());
  object.Set("ok", JsonValue::MakeBool(true));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandRevoke(const Options& options) {
  if (!Has(options, "--site") || !Has(options, "--generation")) {
    PrintError("revoke", Status::Error(ErrorCode::MissingField,
                                       "--site and --generation are required"));
    return kExitUsage;
  }
  Result<rcb::SiteId> site = rcb::SiteId::Make(Value(options, "--site"));
  if (!site.ok()) {
    PrintError("revoke", site.status());
    return ExitCodeFor(site.status());
  }
  Result<long long> generation = ValueAsInt(options, "--generation", 0);
  Result<long long> at = ValueAsInt(options, "--at", 0);
  if (!generation.ok() || !at.ok()) {
    PrintError("revoke", !generation.ok() ? generation.status() : at.status());
    return kExitUsage;
  }
  if (generation.value() <= 0) {
    PrintError("revoke", Status::Error(ErrorCode::InvalidArgument, "--generation must be positive"));
    return kExitUsage;
  }
  rcb::RevocationReason reason = rcb::RevocationReason::OfferRevoked;
  const std::string reason_text = Value(options, "--reason", "offer_revoked");
  if (reason_text == "offer_revoked") {
    reason = rcb::RevocationReason::OfferRevoked;
  } else if (reason_text == "superseded_by_generation") {
    reason = rcb::RevocationReason::SupersededByGeneration;
  } else if (reason_text == "capacity_shrink") {
    reason = rcb::RevocationReason::CapacityShrink;
  } else {
    PrintError("revoke", Status::Error(ErrorCode::InvalidArgument, "unknown --reason value"));
    return kExitUsage;
  }
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("revoke", service.status());
    return ExitCodeFor(service.status());
  }
  Result<rcb::OfferRevocation> revoked =
      service.value()->RevokeOffer(site.value(), static_cast<rcb::u64>(generation.value()),
                                   at.value(), reason);
  if (!revoked.ok()) {
    PrintError("revoke", revoked.status());
    service.value()->Shutdown();
    return ExitCodeFor(revoked.status());
  }
  JsonValue object = rcb::ToJson(revoked.value());
  object.Set("ok", JsonValue::MakeBool(true));
  Print(object, options);
  service.value()->Shutdown();
  return kExitOk;
}

int CommandAsk(const Options& options) {
  Result<JsonValue> document = LoadDocument(options);
  if (!document.ok()) {
    PrintError("ask", document.status());
    return ExitCodeFor(document.status());
  }
  Result<rcb::Ask> ask = rcb::DecodeAskJson(document.value());
  if (!ask.ok()) {
    PrintError("ask", ask.status());
    return ExitCodeFor(ask.status());
  }
  if (Has(options, "--key")) {
    Result<rcb::AskKey> key = rcb::AskKey::Make(Value(options, "--key"));
    if (!key.ok()) {
      PrintError("ask", key.status());
      return ExitCodeFor(key.status());
    }
    ask.value().key = key.value();
  }
  if (Has(options, "--now")) {
    Result<long long> now = ValueAsInt(options, "--now", 0);
    if (!now.ok()) {
      PrintError("ask", now.status());
      return kExitUsage;
    }
    ask.value().as_of = now.value();
  }
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("ask", service.status());
    return ExitCodeFor(service.status());
  }
  const long long repeat = ValueAsInt(options, "--repeat", 1).ValueOr(1);
  Result<rcb::Decision> decision = service.value()->AskNow(ask.value());
  for (long long i = 1; i < repeat && decision.ok(); ++i) {
    decision = service.value()->AskNow(ask.value());
  }
  if (!decision.ok()) {
    PrintError("ask", decision.status());
    service.value()->Shutdown();
    return ExitCodeFor(decision.status());
  }
  Print(DecisionJson(decision.value()), options);
  service.value()->Shutdown();
  return decision.value().outcome == rcb::DecisionOutcome::Refused ? kExitRefused : kExitOk;
}

int CommandAsks(const Options& options) {
  Result<JsonValue> document = LoadDocument(options);
  if (!document.ok()) {
    PrintError("asks", document.status());
    return ExitCodeFor(document.status());
  }
  if (!document.value().IsArray()) {
    PrintError("asks", Status::Error(ErrorCode::MalformedInput,
                                     "the document must be an array of ask documents"));
    return kExitUsage;
  }
  Result<std::unique_ptr<rcb::BrokerService>> service = OpenService(options);
  if (!service.ok()) {
    PrintError("asks", service.status());
    return ExitCodeFor(service.status());
  }
  JsonValue results = JsonValue::MakeArray();
  bool any_refused = false;
  int failure_exit = kExitOk;
  std::size_t accepted = 0;
  std::size_t partial = 0;
  std::size_t refused = 0;
  std::size_t failed = 0;
  for (const JsonValue& item : document.value().items()) {
    Result<rcb::Ask> ask = rcb::DecodeAskJson(item);
    if (!ask.ok()) {
      JsonValue failure = JsonValue::MakeObject();
      failure.Set("ok", JsonValue::MakeBool(false));
      failure.Set("error", JsonValue::MakeString(std::string(ask.status().token())));
      failure.Set("detail", JsonValue::MakeString(ask.status().detail()));
      results.Push(std::move(failure));
      ++failed;
      failure_exit = ExitCodeFor(ask.status());
      continue;
    }
    Result<rcb::Decision> decision = service.value()->AskNow(ask.value());
    if (!decision.ok()) {
      JsonValue failure = JsonValue::MakeObject();
      failure.Set("ok", JsonValue::MakeBool(false));
      failure.Set("error", JsonValue::MakeString(std::string(decision.status().token())));
      failure.Set("detail", JsonValue::MakeString(decision.status().detail()));
      results.Push(std::move(failure));
      ++failed;
      failure_exit = ExitCodeFor(decision.status());
      continue;
    }
    results.Push(DecisionJson(decision.value()));
    switch (decision.value().outcome) {
      case rcb::DecisionOutcome::Accepted: ++accepted; break;
      case rcb::DecisionOutcome::PartiallyAccepted: ++partial; break;
      case rcb::DecisionOutcome::Refused:
        ++refused;
        any_refused = true;
        break;
    }
  }
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(failed == 0));
  object.Set("accepted", JsonValue::MakeUint(accepted));
  object.Set("partially_accepted", JsonValue::MakeUint(partial));
  object.Set("refused", JsonValue::MakeUint(refused));
  object.Set("failed", JsonValue::MakeUint(failed));
  object.Set("results", std::move(results));
  Print(object, options);
  service.value()->Shutdown();
  if (failed != 0) {
    return failure_exit;
  }
  return any_refused ? kExitRefused : kExitOk;
}

int CommandDigest(const Options& options) {
  Result<JsonValue> document = LoadDocument(options);
  if (!document.ok()) {
    PrintError("digest", document.status());
    return ExitCodeFor(document.status());
  }
  const bool is_offer = document.value().Find("tranches") != nullptr;
  const bool is_ask = document.value().Find("requested") != nullptr;
  JsonValue object = JsonValue::MakeObject();
  if (is_offer) {
    Result<rcb::Offer> offer = rcb::DecodeOfferJson(document.value());
    if (!offer.ok()) {
      PrintError("digest", offer.status());
      return ExitCodeFor(offer.status());
    }
    object.Set("kind", JsonValue::MakeString("offer"));
    object.Set("canonical_digest", JsonValue::MakeString(rcb::OfferDigest(offer.value()).Hex()));
    object.Set("canonical_text_bytes",
               JsonValue::MakeUint(rcb::CanonicalOfferText(offer.value()).size()));
  } else if (is_ask) {
    Result<rcb::Ask> ask = rcb::DecodeAskJson(document.value());
    if (!ask.ok()) {
      PrintError("digest", ask.status());
      return ExitCodeFor(ask.status());
    }
    object.Set("kind", JsonValue::MakeString("ask"));
    object.Set("canonical_digest", JsonValue::MakeString(rcb::AskDigest(ask.value()).Hex()));
    object.Set("decision_id",
               JsonValue::MakeString("dec-" + rcb::AskDigest(ask.value()).ShortHex(24)));
  } else {
    object.Set("kind", JsonValue::MakeString("state"));
    object.Set("canonical_digest", JsonValue::MakeString(rcb::Sha256Of(rcb::EncodeJson(document.value())).Hex()));
  }
  object.Set("ok", JsonValue::MakeBool(true));
  Print(object, options);
  return kExitOk;
}

/// Exercises the boundary end to end in one process, including a conservation
/// check after every operation. Used by the CLI smoke test and by operators as
/// a first-run sanity check.
int CommandSelfTest(const Options& options) {
  rcb::BrokerConfig config;
  rcb::BrokerCore core(config, 1);

  rcb::Offer offer;
  offer.site = rcb::SiteId::Make("site-a").ValueOr(rcb::SiteId());
  offer.generation = 1;
  offer.source_snapshot = rcb::SnapshotId::Make("snap-1").ValueOr(rcb::SnapshotId());
  offer.service_class = rcb::ServiceClassId::Make("gpu").ValueOr(rcb::ServiceClassId());
  offer.region = rcb::RegionId::Make("r1").ValueOr(rcb::RegionId());
  offer.jurisdiction = rcb::JurisdictionId::Make("j1").ValueOr(rcb::JurisdictionId());
  offer.valid_from = 0;
  offer.valid_until = 1000;
  offer.reserve_policy = rcb::PolicyId::Make("rp-1").ValueOr(rcb::PolicyId());
  offer.policy_generation = 1;
  offer.cost.reference = rcb::EvidenceId::Make("ev-1").ValueOr(rcb::EvidenceId());
  offer.cost.price_per_milli_unit =
      rcb::ScaledAmount::Parse("0.0004").ValueOr(rcb::ScaledAmount());
  offer.cost.energy_millijoules_per_milli_unit = 1000;
  offer.cost.carbon_milligrams_per_milli_unit = 5;

  rcb::CapacityTranche first;
  first.domain = rcb::FailureDomainId::Make("fd-1").ValueOr(rcb::FailureDomainId());
  first.allocatable = rcb::CapacityVector::Make({1000, 800, 10, 4000}).ValueOr(rcb::CapacityVector());
  first.protected_reserve =
      rcb::CapacityVector::Make({200, 160, 2, 800}).ValueOr(rcb::CapacityVector());
  rcb::CapacityTranche second;
  second.domain = rcb::FailureDomainId::Make("fd-2").ValueOr(rcb::FailureDomainId());
  second.allocatable = rcb::CapacityVector::Make({500, 400, 5, 2000}).ValueOr(rcb::CapacityVector());
  offer.tranches.push_back(first);
  offer.tranches.push_back(second);

  Result<rcb::OfferPublication> published = core.PublishOffer(offer);
  if (!published.ok()) {
    PrintError("selftest", published.status());
    return ExitCodeFor(published.status());
  }

  rcb::Ask ask;
  ask.key = rcb::AskKey::Make("ask-1").ValueOr(rcb::AskKey());
  ask.requester = rcb::RequesterId::Make("req-1").ValueOr(rcb::RequesterId());
  ask.service_class = offer.service_class;
  ask.requested = rcb::CapacityVector::Make({400, 320, 4, 1600}).ValueOr(rcb::CapacityVector());
  ask.as_of = 10;
  ask.min_distinct_failure_domains = 2;

  for (int attempt = 0; attempt < 2; ++attempt) {
    Result<rcb::Decision> decision = core.CommitPlan(core.PlanAsk(ask).ValueOr(rcb::AskPlan()));
    if (!decision.ok()) {
      PrintError("selftest", decision.status());
      return ExitCodeFor(decision.status());
    }
  }

  const rcb::ConservationReport report = core.VerifyConservation();
  const rcb::AccountingSummary summary = core.Summary();
  JsonValue object = JsonValue::MakeObject();
  object.Set("ok", JsonValue::MakeBool(report.closed));
  object.Set("conservation_closed", JsonValue::MakeBool(report.closed));
  object.Set("violations", JsonValue::MakeUint(report.violations.size()));
  object.Set("live_commitments", JsonValue::MakeUint(report.live_commitments));
  object.Set("committed_total", rcb::ToJson(summary.committed_total));
  object.Set("state_digest", JsonValue::MakeString(core.StateDigest().Hex()));
  Print(object, options);
  return report.closed ? kExitOk : kExitInvariant;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  Result<Options> parsed = ParseOptions(args);
  if (!parsed.ok()) {
    PrintError("cli", parsed.status());
    std::cerr << Usage();
    return kExitUsage;
  }
  const Options& options = parsed.value();
  const std::string& command = options.command;

  try {
    if (command == "help") {
      std::cout << Usage();
      return kExitOk;
    }
    if (command == "version") return CommandVersion(options);
    if (command == "status") return CommandStatus(options);
    if (command == "ledger") return CommandLedger(options);
    if (command == "decisions") return CommandDecisions(options);
    if (command == "verify") return CommandVerify(options);
    if (command == "recover") return CommandRecover(options);
    if (command == "compact") return CommandCompact(options);
    if (command == "state") return CommandState(options);
    if (command == "offer") return CommandOffer(options);
    if (command == "revoke") return CommandRevoke(options);
    if (command == "ask") return CommandAsk(options);
    if (command == "asks") return CommandAsks(options);
    if (command == "digest") return CommandDigest(options);
    if (command == "selftest") return CommandSelfTest(options);
  } catch (const std::exception& error) {
    PrintError(command, Status::Error(ErrorCode::Internal, error.what()));
    return kExitInvariant;
  }

  PrintError(command, Status::Error(ErrorCode::InvalidArgument, "unknown command " + command));
  std::cerr << Usage();
  return kExitUsage;
}
