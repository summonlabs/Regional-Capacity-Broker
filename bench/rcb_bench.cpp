// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// rcb_bench: the measurement harness for the Regional Capacity Broker.
//
// What this harness measures, and what it refuses to do:
//
//   * the unit under test is one COMPLETED brokerage operation. For the kernel
//     that is BrokerCore::PlanAsk followed by BrokerCore::CommitPlan; for the
//     service it is BrokerService::AskNow. Submission, enqueue, queue-wait and
//     asynchronous-handle latency are never measured: those measure a queue,
//     not a broker;
//   * every number printed here comes from an interval this process observed
//     with std::chrono::steady_clock around real work it performed. Nothing is
//     estimated, interpolated, extrapolated or carried over from another run.
//     A scenario that could not be run is reported as "UNSUPPORTED" together
//     with the reason (the exact status text where the runtime produced one)
//     and never replaced by a placeholder number;
//   * the accounting in this file is integer-only. rcb::JsonValue has no
//     floating-point kind, so a rate that is not an exact integer is reported
//     as a truncated integer next to the elapsed time it was derived from, and
//     the exact interval is always part of the measurement's detail;
//   * synthetic capacity is generated from one seed that is printed in the
//     output, using a first-party splitmix64 generator: no <random>, no
//     distribution implementation drift between standard libraries;
//   * every store directory this harness creates is removed before the process
//     exits, and the outcome of that removal is reported in "stores".
//
// Scenarios (each measurement carries its own scenario description):
//
//   1. one ask per round over a synthetic region of N sites, each publishing
//      3 or more failure domains with power, cooling, rack space and
//      service-class capacity. The ask requests twice the region's whole
//      allocatable capacity, so it can only be satisfied in part and the
//      allocator has to spread the commitment over the region. Between rounds
//      every site publishes a new generation that adds fresh headroom, which is
//      what the next ask consumes;
//   2. the same region with a large ask fan-out: M asks per round, each scoped
//      by locality to one slice of the region, each with its own idempotency
//      key, so the cost of decisions taken under fan-out can be compared with
//      the single-ask case;
//   3. the same ask set against rcb::BrokerService three times: a real
//      directory-backed store with DurabilityClass::Durable (one fsync per
//      decision), the same directory-backed store with DurabilityClass::Buffered,
//      and an in-memory service (Volatile). The fsync cost per decision is the
//      measured difference;
//   4. idempotent retry: M distinct asks are committed, then the identical M
//      asks (same keys, same content) are re-submitted, and the replay path is
//      measured separately from the commit path;
//   5. rcb::BrokerCore::VerifyConservation over the largest ledger this run
//      produced, reported once together with the ledger size it covered.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "rcb/broker.hpp"
#include "rcb/json.hpp"
#include "rcb/model.hpp"
#include "rcb/serialize.hpp"
#include "rcb/service.hpp"
#include "rcb/status.hpp"
#include "rcb/types.hpp"
#include "rcb/units.hpp"
#include "rcb/version.hpp"

namespace {

using rcb::Ask;
using rcb::AskKey;
using rcb::AskPlan;
using rcb::BrokerConfig;
using rcb::BrokerCore;
using rcb::BrokerService;
using rcb::CapacityTranche;
using rcb::CapacityVector;
using rcb::Decision;
using rcb::DecisionOutcome;
using rcb::Dimension;
using rcb::DurabilityClass;
using rcb::i64;
using rcb::JsonValue;
using rcb::kDimensionCount;
using rcb::kDimensions;
using rcb::Offer;
using rcb::OfferPublication;
using rcb::PriorityClass;
using rcb::Result;
using rcb::RiskTier;
using rcb::ScaledAmount;
using rcb::ServiceClassId;
using rcb::ServiceConfig;
using rcb::ShutdownMode;
using rcb::Status;
using rcb::u64;

/// One value per capacity dimension, in the canonical dimension order.
using Dims = std::array<i64, kDimensionCount>;

// ---- scenario constants ---------------------------------------------------

/// Printed default seed. Every synthetic quantity in the run derives from it.
constexpr u64 kDefaultSeed = 0x5EED2026ULL;

/// Allocatable capacity of one failure domain of one site, per dimension:
/// 1 MW of facility power, 900 kW of heat rejection, 200 racks and 2000 units
/// (2,000,000 milli-units) of service-class-qualified capacity.
constexpr Dims kTrancheBase = {1000000LL, 900000LL, 200LL, 2000000LL};

/// Fresh headroom one new generation adds per failure domain per round: 2% of
/// the base above. This is what the next ask consumes, so the allocator keeps
/// working on every decision without the region ever running dry.
constexpr Dims kTrancheStep = {20000LL, 18000LL, 4LL, 40000LL};

/// Protected reserve is published, but no ask in this harness is authorized to
/// consume it, so it stays inert and the ask sees allocatable capacity only.
constexpr i64 kReserveDivisor = 10;

/// Cost, energy and carbon evidence per milli-unit of service capacity.
constexpr i64 kPriceMicros = 10;
constexpr i64 kEnergyMillijoulesPerMilliUnit = 340;
constexpr i64 kCarbonMilligramsPerMilliUnit = 120;

/// Every ask requests this multiple of the capacity it could ever be given.
constexpr i64 kAskFactor = 2;

constexpr i64 kValidityEnd = 4000000000LL;
constexpr i64 kAsOf = 1000000LL;

/// Rounds published and submitted before the timed interval starts.
constexpr std::size_t kNormalWarmup = 2;
constexpr std::size_t kQuickWarmup = 1;

// ---- process state and cleanup --------------------------------------------

/// A directory this process created and must remove before it exits.
struct PendingRemoval {
  std::filesystem::path path;
  /// True when only an empty directory may be removed (a root the operator
  /// named, which this harness created but does not own).
  bool only_when_empty = false;
};

std::vector<PendingRemoval>& CleanupPaths() {
  static std::vector<PendingRemoval> paths;
  return paths;
}

struct CleanupReport {
  std::size_t attempted = 0;
  std::size_t removed = 0;
  std::vector<std::string> remaining;
};

/// UTF-8 text of a path, which is the encoding rcb::store expects on every
/// platform (the Windows adapter converts UTF-8 to UTF-16 internally).
std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  if (text.empty()) {
    return std::string();
  }
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

/// Removes every directory this harness created. Runs in reverse creation
/// order so that a store root is only considered once its children are gone.
CleanupReport CleanupStores() {
  CleanupReport report;
  std::vector<PendingRemoval>& paths = CleanupPaths();
  for (auto entry = paths.rbegin(); entry != paths.rend(); ++entry) {
    ++report.attempted;
    std::error_code error;
    if (entry->only_when_empty) {
      (void)std::filesystem::remove(entry->path, error);
    } else {
      (void)std::filesystem::remove_all(entry->path, error);
    }
  }
  for (const PendingRemoval& entry : paths) {
    std::error_code error;
    if (std::filesystem::exists(entry.path, error)) {
      report.remaining.push_back(PathToUtf8(entry.path));
    } else {
      ++report.removed;
    }
  }
  paths.clear();
  return report;
}

void CleanupOnExit() { (void)CleanupStores(); }

[[noreturn]] void Fail(int code, const std::string& message) {
  (void)CleanupStores();
  std::fputs("rcb_bench: ", stderr);
  std::fputs(message.c_str(), stderr);
  std::fputc('\n', stderr);
  std::fflush(stderr);
  std::exit(code);
}

// ---- deterministic synthetic data -----------------------------------------

/// splitmix64: a small, fully specified generator, so the synthetic region is
/// reproducible on every standard library and every platform.
class SplitMix64 {
 public:
  explicit SplitMix64(u64 seed) noexcept : state_(seed) {}

  u64 Next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    u64 z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  /// Uniform in [0, bound); zero when bound is zero.
  u64 Below(u64 bound) noexcept { return bound == 0ULL ? 0ULL : Next() % bound; }

 private:
  u64 state_;
};

std::string ZeroPadded(u64 value, std::size_t width) {
  std::string digits = std::to_string(value);
  if (digits.size() >= width) {
    return digits;
  }
  std::string text(width - digits.size(), '0');
  text += digits;
  return text;
}

std::string HexU64(u64 value) {
  static const char* const kDigits = "0123456789abcdef";
  if (value == 0ULL) {
    return "0";
  }
  std::string text;
  while (value != 0ULL) {
    text.push_back(kDigits[static_cast<std::size_t>(value & 0xFULL)]);
    value >>= 4;
  }
  std::reverse(text.begin(), text.end());
  return text;
}

template <class Tag>
rcb::TaggedId<Tag> MakeId(const std::string& text) {
  Result<rcb::TaggedId<Tag>> made = rcb::TaggedId<Tag>::Make(text);
  if (!made.ok()) {
    Fail(1, "the harness built an invalid synthetic identifier '" + text +
                "': " + made.status().ToString());
  }
  return made.value();
}

rcb::SiteId SiteIdOf(std::size_t site) {
  return MakeId<rcb::SiteIdTag>("site-" + ZeroPadded(static_cast<u64>(site), 5));
}

rcb::FailureDomainId DomainIdOf(std::size_t domain) {
  return MakeId<rcb::FailureDomainIdTag>("domain-" + ZeroPadded(static_cast<u64>(domain), 2));
}

CapacityVector MakeCapacity(const Dims& values) {
  CapacityVector capacity;
  for (std::size_t index = 0; index < kDimensionCount; ++index) {
    capacity.Set(kDimensions[index], values[index]);
  }
  return capacity;
}

// ---- timing ---------------------------------------------------------------

i64 NowNs() noexcept {
  return static_cast<i64>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

/// Sum, minimum and maximum of the individually timed intervals of one batch.
struct Timing {
  i64 total_ns = 0;
  i64 min_ns = 0;
  i64 max_ns = 0;
  i64 count = 0;

  void Add(i64 interval_ns) {
    const i64 interval = interval_ns > 0 ? interval_ns : 0;
    if (count == 0 || interval < min_ns) {
      min_ns = interval;
    }
    if (count == 0 || interval > max_ns) {
      max_ns = interval;
    }
    total_ns += interval;
    ++count;
  }

  [[nodiscard]] i64 MeanNs() const { return count == 0 ? 0 : total_ns / count; }
};

/// Rounds half away from zero; used only to express a measured nanosecond
/// quantity in whole microseconds, never to invent a value.
i64 RoundDiv(i64 numerator, i64 denominator) {
  if (denominator <= 0) {
    return 0;
  }
  const i64 half = denominator / 2;
  if (numerator >= 0) {
    return (numerator + half) / denominator;
  }
  return -((-numerator + half) / denominator);
}

i64 RatePerSecond(i64 operations, i64 elapsed_ns) {
  if (operations <= 0 || elapsed_ns <= 0) {
    return 0;
  }
  const u64 scaled = static_cast<u64>(operations) * 1000000000ULL;
  return static_cast<i64>(scaled / static_cast<u64>(elapsed_ns));
}

struct DecisionTally {
  i64 accepted = 0;
  i64 partially_accepted = 0;
  i64 refused = 0;
  i64 allocations = 0;

  void Add(const Decision& decision) {
    if (decision.outcome == DecisionOutcome::Accepted) {
      ++accepted;
    } else if (decision.outcome == DecisionOutcome::PartiallyAccepted) {
      ++partially_accepted;
    } else {
      ++refused;
    }
    allocations += static_cast<i64>(decision.allocations.size());
  }
};

// ---- measurements ---------------------------------------------------------

struct Measurement {
  std::string id;
  std::string description;
  std::string unit;
  i64 samples = 0;
  bool measured = false;
  i64 value = 0;
  std::string evidence;
  std::string reason;
  JsonValue detail;
};

void PutInt(JsonValue& object, const char* key, i64 value) {
  (void)object.Set(key, JsonValue::MakeInt(value));
}
void PutUint(JsonValue& object, const char* key, u64 value) {
  (void)object.Set(key, JsonValue::MakeUint(value));
}
void PutText(JsonValue& object, const char* key, const std::string& value) {
  (void)object.Set(key, JsonValue::MakeString(value));
}
void PutBool(JsonValue& object, const char* key, bool value) {
  (void)object.Set(key, JsonValue::MakeBool(value));
}
void PutValue(JsonValue& object, const char* key, JsonValue value) {
  (void)object.Set(key, std::move(value));
}

JsonValue Object() { return JsonValue::MakeObject(); }

void AddReal(std::vector<Measurement>& measurements, std::string id, std::string description,
             std::string unit, i64 samples, i64 value, JsonValue detail) {
  Measurement measurement;
  measurement.id = std::move(id);
  measurement.description = std::move(description);
  measurement.unit = std::move(unit);
  measurement.samples = samples;
  measurement.measured = true;
  measurement.value = value;
  measurement.evidence = "REAL";
  measurement.detail = std::move(detail);
  measurements.push_back(std::move(measurement));
}

void AddUnsupported(std::vector<Measurement>& measurements, std::string id, std::string description,
                    std::string unit, std::string reason) {
  Measurement measurement;
  measurement.id = std::move(id);
  measurement.description = std::move(description);
  measurement.unit = std::move(unit);
  measurement.samples = 0;
  measurement.measured = false;
  measurement.evidence = "UNSUPPORTED";
  measurement.reason = std::move(reason);
  measurements.push_back(std::move(measurement));
}

JsonValue TimingDetail(const Timing& timing) {
  JsonValue detail = Object();
  PutInt(detail, "elapsed_ns", timing.total_ns);
  PutInt(detail, "mean_ns", timing.MeanNs());
  PutInt(detail, "min_ns", timing.min_ns);
  PutInt(detail, "max_ns", timing.max_ns);
  return detail;
}

JsonValue TallyDetail(const DecisionTally& tally) {
  JsonValue detail = Object();
  PutInt(detail, "accepted", tally.accepted);
  PutInt(detail, "partially_accepted", tally.partially_accepted);
  PutInt(detail, "refused", tally.refused);
  PutInt(detail, "allocations", tally.allocations);
  return detail;
}

JsonValue MergeDetail(JsonValue left, const JsonValue& right) {
  for (const auto& member : right.members()) {
    (void)left.Set(member.first, member.second);
  }
  return left;
}

/// Adds the paired "time per decision" and "decisions per second" measurements
/// for one batch of completed decisions timed in nanoseconds.
void AddNanosecondPair(std::vector<Measurement>& measurements, const std::string& id_prefix,
                       const std::string& description, const Timing& timing,
                       const JsonValue& extra_detail) {
  if (timing.count <= 0 || timing.total_ns <= 0) {
    AddUnsupported(measurements, id_prefix + ".ns_per_decision", description,
                   "nanoseconds per decision",
                   "no completed decision was timed in this scenario");
    AddUnsupported(measurements, id_prefix + ".decisions_per_second", description,
                   "decisions per second",
                   "no completed decision was timed in this scenario");
    return;
  }
  JsonValue detail = MergeDetail(TimingDetail(timing), extra_detail);
  PutInt(detail, "decisions", timing.count);
  AddReal(measurements, id_prefix + ".ns_per_decision", description, "nanoseconds per decision",
          timing.count, timing.MeanNs(), detail);
  AddReal(measurements, id_prefix + ".decisions_per_second", description, "decisions per second",
          timing.count, RatePerSecond(timing.count, timing.total_ns), detail);
}

/// Adds the paired measurements for one batch of completed decisions reported
/// in whole microseconds, with the exact nanosecond figure kept in the detail.
void AddMicrosecondPair(std::vector<Measurement>& measurements, const std::string& id_prefix,
                        const std::string& description, const Timing& timing,
                        const JsonValue& extra_detail) {
  if (timing.count <= 0 || timing.total_ns <= 0) {
    AddUnsupported(measurements, id_prefix + ".us_per_decision", description,
                   "microseconds per decision",
                   "no completed decision was timed in this scenario");
    AddUnsupported(measurements, id_prefix + ".decisions_per_second", description,
                   "decisions per second",
                   "no completed decision was timed in this scenario");
    return;
  }
  JsonValue detail = MergeDetail(TimingDetail(timing), extra_detail);
  PutInt(detail, "decisions", timing.count);
  PutInt(detail, "exact_ns_per_decision", timing.MeanNs());
  AddReal(measurements, id_prefix + ".us_per_decision", description, "microseconds per decision",
          timing.count, RoundDiv(timing.MeanNs(), 1000LL), detail);
  AddReal(measurements, id_prefix + ".decisions_per_second", description, "decisions per second",
          timing.count, RatePerSecond(timing.count, timing.total_ns), detail);
}

// ---- command line ---------------------------------------------------------

struct Options {
  std::size_t sites = 16;
  std::size_t tranches = 4;
  std::size_t asks = 64;
  std::string service_class = "svc-general";
  std::string out_path;
  bool out_given = false;
  bool json_only = false;
  std::string store_root;
  bool store_given = false;
  std::string durability = "durable";
  bool quick = false;
  bool help = false;
  u64 seed = kDefaultSeed;
};

const char* const kUsage =
    "usage: rcb_bench [--sites N] [--tranches K] [--asks M] [--service-class S] [--out FILE]\n"
    "                 [--json] [--store DIR] [--durability durable|buffered] [--quick] [--seed N]\n"
    "\n"
    "  --sites N          headline site count of the synthetic region (default 16); the core\n"
    "                     ladder measures this count, a quarter of it and four times it\n"
    "  --tranches K       failure domains per site, at least 3 (default 4)\n"
    "  --asks M           largest ask fan-out per round (default 64)\n"
    "  --service-class S  service class carried by every synthetic offer and ask (default\n"
    "                     svc-general)\n"
    "  --out FILE         also write the canonical JSON document to FILE\n"
    "  --json             write only the canonical JSON document (no summary on stderr)\n"
    "  --store DIR        create the benchmark's store directories under DIR; without it a\n"
    "                     unique directory under the system temporary directory is used\n"
    "  --durability X     durability of the directory-backed service arm: durable (default,\n"
    "                     measures the fsync-per-decision arm next to the buffered arm) or\n"
    "                     buffered (skips the fsync-per-decision arm)\n"
    "  --quick            smaller ladders, fewer rounds, fewer asks\n"
    "  --seed N           synthetic data seed (printed in the output)\n"
    "  -h, --help         this text\n"
    "\n"
    "The canonical JSON document is always written to stdout. Every measurement records the\n"
    "unit, the sample count and the evidence: REAL for a timed operation in this process,\n"
    "UNSUPPORTED with a reason for anything that could not be measured.\n";

bool ParseUnsigned(const std::string& text, u64& out) {
  if (text.empty()) {
    return false;
  }
  u64 value = 0;
  for (const char digit : text) {
    if (digit < '0' || digit > '9') {
      return false;
    }
    const u64 addend = static_cast<u64>(digit - '0');
    if (value > (std::numeric_limits<u64>::max() - addend) / 10ULL) {
      return false;
    }
    value = value * 10ULL + addend;
  }
  out = value;
  return true;
}

bool ParseSizeOption(const std::string& text, std::size_t minimum, std::size_t maximum,
                     std::size_t& out, std::string& error, const std::string& name) {
  u64 parsed = 0;
  if (!ParseUnsigned(text, parsed)) {
    error = name + " expects an unsigned decimal integer, got '" + text + "'";
    return false;
  }
  if (parsed < static_cast<u64>(minimum) || parsed > static_cast<u64>(maximum)) {
    error = name + " must be between " + std::to_string(minimum) + " and " +
            std::to_string(maximum) + ", got " + text;
    return false;
  }
  out = static_cast<std::size_t>(parsed);
  return true;
}

bool ParseOptions(int argc, char** argv, Options& options, std::string& error) {
  bool quick = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "-h" || argument == "--help") {
      options.help = true;
      return true;
    }
    if (argument.size() < 3 || argument[0] != '-' || argument[1] != '-') {
      error = "unexpected argument '" + argument + "'";
      return false;
    }
    std::string name = argument.substr(2);
    std::string value;
    bool has_inline_value = false;
    const std::size_t equals = name.find('=');
    if (equals != std::string::npos) {
      value = name.substr(equals + 1);
      name = name.substr(0, equals);
      has_inline_value = true;
    }
    const auto take_value = [&](const std::string& option) -> bool {
      if (has_inline_value) {
        return true;
      }
      if (index + 1 >= argc) {
        error = option + " expects a value";
        return false;
      }
      ++index;
      value = argv[index];
      return true;
    };

    if (name == "sites") {
      if (!take_value("--sites")) {
        return false;
      }
      if (!ParseSizeOption(value, 1, 4096, options.sites, error, "--sites")) {
        return false;
      }
    } else if (name == "tranches") {
      if (!take_value("--tranches")) {
        return false;
      }
      if (!ParseSizeOption(value, 3, 64, options.tranches, error, "--tranches")) {
        return false;
      }
    } else if (name == "asks") {
      if (!take_value("--asks")) {
        return false;
      }
      if (!ParseSizeOption(value, 1, 256, options.asks, error, "--asks")) {
        return false;
      }
    } else if (name == "service-class") {
      if (!take_value("--service-class")) {
        return false;
      }
      if (value.empty() || value.size() > rcb::Identifier::kMaxLength) {
        error = "--service-class must be 1 to 64 characters";
        return false;
      }
      options.service_class = value;
    } else if (name == "out") {
      if (!take_value("--out")) {
        return false;
      }
      options.out_path = value;
      options.out_given = true;
    } else if (name == "json") {
      options.json_only = true;
    } else if (name == "quick") {
      quick = true;
    } else if (name == "store") {
      if (!take_value("--store")) {
        return false;
      }
      options.store_root = value;
      options.store_given = true;
    } else if (name == "durability") {
      if (!take_value("--durability")) {
        return false;
      }
      if (value != "durable" && value != "buffered") {
        error = "--durability must be 'durable' or 'buffered', got '" + value + "'";
        return false;
      }
      options.durability = value;
    } else if (name == "seed") {
      if (!take_value("--seed")) {
        return false;
      }
      u64 parsed = 0;
      if (!ParseUnsigned(value, parsed)) {
        error = "--seed expects an unsigned decimal integer, got '" + value + "'";
        return false;
      }
      options.seed = parsed;
    } else {
      error = "unknown option '--" + name + "'";
      return false;
    }
    if (!error.empty()) {
      return false;
    }
  }
  if (options.store_given && options.store_root.empty()) {
    error = "--store must name a directory";
    return false;
  }
  options.quick = quick;
  return true;
}

// ---- run parameters -------------------------------------------------------

std::size_t ClampSize(std::size_t value, std::size_t low, std::size_t high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

struct BenchParams {
  u64 seed = kDefaultSeed;
  std::size_t sites = 16;
  std::size_t tranches = 4;
  std::size_t asks = 64;
  std::string service_class;
  bool quick = false;
  std::size_t warmup_rounds = kNormalWarmup;
  std::vector<std::size_t> site_ladder;
  std::vector<std::size_t> fanout_ladder;
  std::size_t fanout_sites = 0;
  std::size_t fanout_rounds = 0;
  std::size_t service_sites = 0;
  std::size_t service_rounds = 0;
  std::size_t service_warmup = 0;
  std::size_t retry_sites = 0;
  std::size_t retry_target = 0;

  [[nodiscard]] std::size_t timed_rounds_for(std::size_t site_count) const {
    const std::size_t budget = quick ? 256U : 2048U;
    const std::size_t low = quick ? 8U : 24U;
    const std::size_t high = quick ? 48U : 160U;
    const std::size_t divisor = site_count == 0 ? 1U : site_count;
    return ClampSize(budget / divisor, low, high);
  }
};

void PushUnique(std::vector<std::size_t>& values, std::size_t value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

BenchParams MakeParams(const Options& options) {
  BenchParams params;
  params.seed = options.seed;
  params.sites = options.sites;
  params.tranches = options.tranches;
  params.asks = options.asks;
  params.service_class = options.service_class;
  params.quick = options.quick;

  // One decision must stay under the kernel's allocation budget.
  const std::size_t site_max = ClampSize(4096U / params.tranches, 1U, 512U);
  params.warmup_rounds = options.quick ? kQuickWarmup : kNormalWarmup;

  const std::size_t quarter = params.sites / 4U;
  PushUnique(params.site_ladder, ClampSize(quarter, 1U, site_max));
  PushUnique(params.site_ladder, ClampSize(params.sites, 1U, site_max));
  if (!options.quick) {
    PushUnique(params.site_ladder, ClampSize(params.sites * 4U, 1U, site_max));
  }
  std::sort(params.site_ladder.begin(), params.site_ladder.end());

  params.fanout_sites = ClampSize(params.sites, 1U, options.quick ? 16U : 64U);
  params.fanout_rounds = options.quick ? 8U : 24U;
  PushUnique(params.fanout_ladder, 1U);
  if (!options.quick) {
    PushUnique(params.fanout_ladder, ClampSize(8U, 1U, params.fanout_sites));
  }
  PushUnique(params.fanout_ladder, ClampSize(params.asks, 1U, params.fanout_sites));
  std::sort(params.fanout_ladder.begin(), params.fanout_ladder.end());

  params.service_sites = ClampSize(params.sites, 1U, options.quick ? 4U : 8U);
  params.service_rounds = options.quick ? 6U : 24U;
  params.service_warmup = options.quick ? 1U : 2U;

  params.retry_sites =
      ClampSize(std::max<std::size_t>(options.quick ? 32U : 64U, params.sites), 1U, 256U);
  params.retry_target = options.quick ? 64U : 512U;
  return params;
}

// ---- synthetic region -----------------------------------------------------

class RegionModel {
 public:
  RegionModel(u64 seed, std::size_t sites, std::size_t tranches,
              const ServiceClassId& service_class)
      : tranches_(tranches), service_class_(service_class) {
    SplitMix64 rng(seed);
    base_.resize(sites);
    step_.resize(sites);
    reserve_.resize(sites);
    price_.resize(sites);
    energy_.resize(sites);
    carbon_.resize(sites);
    risk_.resize(sites);
    region_.resize(sites);
    jurisdiction_.resize(sites);
    for (std::size_t site = 0; site < sites; ++site) {
      for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
        const i64 base = kTrancheBase[dimension];
        const i64 jitter = static_cast<i64>(rng.Below(static_cast<u64>(base / 20LL) + 1ULL));
        base_[site][dimension] = base + jitter;
        step_[site][dimension] = kTrancheStep[dimension];
        reserve_[site][dimension] = (base + jitter) / kReserveDivisor;
      }
      price_[site] =
          ScaledAmount::FromMicros(kPriceMicros + static_cast<i64>(rng.Below(7ULL))).value();
      energy_[site] = kEnergyMillijoulesPerMilliUnit + static_cast<i64>(rng.Below(21ULL));
      carbon_[site] = kCarbonMilligramsPerMilliUnit + static_cast<i64>(rng.Below(11ULL));
      risk_[site] = static_cast<RiskTier>(static_cast<rcb::u8>(rng.Below(4ULL)));
      region_[site] = static_cast<rcb::u32>(rng.Below(3ULL));
      jurisdiction_[site] = static_cast<rcb::u32>(rng.Below(2ULL));
    }
  }

  [[nodiscard]] std::size_t sites() const noexcept { return base_.size(); }
  [[nodiscard]] std::size_t tranches() const noexcept { return tranches_; }

  [[nodiscard]] std::vector<std::size_t> AllSites() const {
    std::vector<std::size_t> sites(base_.size());
    for (std::size_t index = 0; index < sites.size(); ++index) {
      sites[index] = index;
    }
    return sites;
  }

  /// Contiguous slice \p index of \p count over the region, used by the
  /// fan-out scenario so that each ask has its own locality scope.
  [[nodiscard]] std::vector<std::size_t> Slice(std::size_t index, std::size_t count) const {
    const std::size_t total = base_.size();
    if (count <= 1U || count > total) {
      return AllSites();
    }
    const std::size_t begin = (total * index) / count;
    const std::size_t end = (total * (index + 1U)) / count;
    std::vector<std::size_t> slice;
    for (std::size_t site = begin; site < end; ++site) {
      slice.push_back(site);
    }
    return slice;
  }

  /// Published base allocatable capacity of one failure domain of one site.
  [[nodiscard]] i64 PerTrancheBase(std::size_t site, std::size_t dimension) const {
    return base_[site][dimension];
  }

  /// Sum of the published base allocatable capacity of a slice, per dimension.
  /// A site publishes one tranche per failure domain, so every site
  /// contributes its per-tranche base once per domain.
  [[nodiscard]] Dims SliceBase(const std::vector<std::size_t>& slice) const {
    const i64 domains = static_cast<i64>(tranches_);
    Dims total{};
    for (const std::size_t site : slice) {
      for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
        total[dimension] += base_[site][dimension] * domains;
      }
    }
    return total;
  }

  [[nodiscard]] Offer MakeOffer(std::size_t site, u64 generation) const {
    Offer offer;
    offer.site = SiteIdOf(site);
    offer.generation = generation;
    offer.source_snapshot = MakeId<rcb::SnapshotIdTag>("snapshot-" + ZeroPadded(generation, 6));
    offer.service_class = service_class_;
    offer.region = MakeId<rcb::RegionIdTag>("region-" +
                                            ZeroPadded(static_cast<u64>(region_[site]) + 1ULL, 2));
    offer.jurisdiction = MakeId<rcb::JurisdictionIdTag>(
        "jurisdiction-" + ZeroPadded(static_cast<u64>(jurisdiction_[site]) + 1ULL, 2));
    offer.risk = risk_[site];
    offer.valid_from = 0;
    offer.valid_until = kValidityEnd;
    offer.reserve_policy = MakeId<rcb::PolicyIdTag>("reserve-policy-01");
    offer.policy_generation = 1ULL;
    offer.reserve_minimum_priority = PriorityClass::Critical;
    offer.cost.reference = MakeId<rcb::EvidenceIdTag>("evidence-" + ZeroPadded(
                                                          static_cast<u64>(site), 5));
    offer.cost.price_per_milli_unit = price_[site];
    offer.cost.energy_millijoules_per_milli_unit = energy_[site];
    offer.cost.carbon_milligrams_per_milli_unit = carbon_[site];
    offer.tranches.reserve(tranches_);
    for (std::size_t domain = 0; domain < tranches_; ++domain) {
      CapacityTranche tranche;
      tranche.domain = DomainIdOf(domain);
      Dims allocatable{};
      for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
        allocatable[dimension] = base_[site][dimension] +
                                 static_cast<i64>(generation - 1ULL) * step_[site][dimension];
      }
      tranche.allocatable = MakeCapacity(allocatable);
      tranche.protected_reserve = MakeCapacity(reserve_[site]);
      offer.tranches.push_back(tranche);
    }
    return offer;
  }

  /// The region's request shape: \p factor times the whole allocatable base of
  /// the slice, so the ask can only ever be satisfied in part.
  [[nodiscard]] Ask MakeAsk(const std::string& key, const std::string& requester,
                            const std::vector<std::size_t>& slice, i64 factor) const {
    const Dims total = SliceBase(slice);
    Ask ask;
    ask.key = MakeId<rcb::AskKeyTag>(key);
    ask.requester = MakeId<rcb::RequesterIdTag>(requester);
    ask.service_class = service_class_;
    for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
      ask.requested.Set(kDimensions[dimension], factor * total[dimension]);
    }
    ask.priority = PriorityClass::Normal;
    ask.fairness = rcb::FairnessPolicy::StableSiteOrder;
    ask.as_of = kAsOf;
    ask.max_risk = RiskTier::Critical;
    ask.require_current_generation = true;
    if (slice.size() < base_.size()) {
      // An empty allowed list means "every site"; a short one scopes locality.
      ask.allowed_sites.reserve(slice.size());
      for (const std::size_t site : slice) {
        ask.allowed_sites.push_back(SiteIdOf(site));
      }
    }
    return ask;
  }

 private:
  std::size_t tranches_;
  ServiceClassId service_class_;
  std::vector<Dims> base_;
  std::vector<Dims> step_;
  std::vector<Dims> reserve_;
  std::vector<ScaledAmount> price_;
  std::vector<i64> energy_;
  std::vector<i64> carbon_;
  std::vector<RiskTier> risk_;
  std::vector<rcb::u32> region_;
  std::vector<rcb::u32> jurisdiction_;
};

// ---- scenario descriptions ------------------------------------------------

std::string CoreScenarioDescription(const BenchParams& params, std::size_t sites, std::size_t fanout,
                                    std::size_t rounds, std::size_t warmup) {
  std::string text = "one ask per round over a synthetic region of " + std::to_string(sites) +
                     " sites and " + std::to_string(params.tranches) +
                     " failure domains per site, each domain publishing power, cooling, rack "
                     "space and service-class capacity; every round publishes a new offer "
                     "generation that adds " +
                     std::to_string(kTrancheStep[0]) + " W / " + std::to_string(kTrancheStep[1]) +
                     " W / " + std::to_string(kTrancheStep[2]) + " racks / " +
                     std::to_string(kTrancheStep[3]) + " milli-units per domain, and each ask";
  if (fanout > 1) {
    text += " (one of " + std::to_string(fanout) +
            " per round, each scoped by locality to one slice of the region and carrying its own "
            "idempotency key)";
  }
  text += " requests " + std::to_string(kAskFactor) +
          " times the allocatable capacity it can reach, so every decision is partially "
          "accepted and the allocator spreads the commitment over the region; " +
          std::to_string(warmup) + " untimed warm-up round(s) precede " + std::to_string(rounds) +
          " timed rounds, and the timed interval covers PlanAsk plus CommitPlan only";
  return text;
}

std::string ServiceScenarioDescription(std::size_t sites, std::size_t tranches, std::size_t rounds,
                                       std::size_t warmup, const std::string& store_text,
                                       const std::string& durability_text) {
  return "the same ask set (one ask per round, " + std::to_string(kAskFactor) +
         " times the reachable allocatable capacity, " + std::to_string(sites) + " sites and " +
         std::to_string(tranches) +
         " failure domains per site) submitted through rcb::BrokerService::AskNow, which plans, "
         "applies and durable-commits one decision per call; " +
         std::to_string(warmup) + " untimed warm-up round(s) precede " + std::to_string(rounds) +
         " timed rounds; store: " + store_text + "; durability class: " + durability_text +
         "; automatic compaction is disabled so the measurement isolates the per-decision commit";
}

// ---- kernel scenarios -----------------------------------------------------

struct CoreBenchResult {
  bool ok = true;
  std::string failure;
  std::size_t sites = 0;
  std::size_t rounds = 0;
  std::size_t warmup = 0;
  std::size_t fanout = 1;
  Timing timing;
  DecisionTally tally;
};

/// Drives the kernel workload described above and keeps the resulting ledger
/// alive when \p keep is true, so the verification scenario can cover it.
CoreBenchResult RunCoreWorkload(const BenchParams& params, std::size_t sites, std::size_t fanout,
                                bool keep, std::unique_ptr<BrokerCore>& kept_core) {
  CoreBenchResult result;
  result.sites = sites;
  result.fanout = fanout;
  result.warmup = params.warmup_rounds;
  result.rounds = params.timed_rounds_for(sites);

  RegionModel model(params.seed, sites, params.tranches,
                    MakeId<rcb::ServiceClassIdTag>(params.service_class));
  std::vector<std::vector<std::size_t>> slices;
  slices.reserve(fanout);
  for (std::size_t index = 0; index < fanout; ++index) {
    slices.push_back(model.Slice(index, fanout));
  }

  std::unique_ptr<BrokerCore> core = std::make_unique<BrokerCore>(BrokerConfig());
  const std::size_t total_rounds = result.warmup + result.rounds;
  for (std::size_t round = 0; round < total_rounds; ++round) {
    const u64 generation = static_cast<u64>(round) + 1ULL;
    for (std::size_t site = 0; site < sites; ++site) {
      const Result<OfferPublication> published = core->PublishOffer(model.MakeOffer(site, generation));
      if (!published.ok()) {
        result.ok = false;
        result.failure = "PublishOffer(site " + std::to_string(site) + ", generation " +
                         std::to_string(generation) + "): " + published.status().ToString();
        return result;
      }
    }
    const bool timed = round >= result.warmup;
    for (std::size_t index = 0; index < fanout; ++index) {
      const std::string key = "ask-" + ZeroPadded(static_cast<u64>(round), 5) + "-" +
                              ZeroPadded(static_cast<u64>(index), 3);
      const Ask ask = model.MakeAsk(key, "requester-bench", slices[index], kAskFactor);
      const i64 start = timed ? NowNs() : 0;
      Result<AskPlan> plan = core->PlanAsk(ask);
      if (!plan.ok()) {
        result.ok = false;
        result.failure = "PlanAsk(" + key + "): " + plan.status().ToString();
        return result;
      }
      Result<Decision> decision = core->CommitPlan(plan.value());
      const i64 stop = timed ? NowNs() : 0;
      if (!decision.ok()) {
        result.ok = false;
        result.failure = "CommitPlan(" + key + "): " + decision.status().ToString();
        return result;
      }
      if (timed) {
        result.timing.Add(stop - start);
        result.tally.Add(decision.value());
      }
    }
  }
  if (keep) {
    kept_core = std::move(core);
  }
  return result;
}

void RunCoreScenarios(const BenchParams& params, std::vector<Measurement>& measurements,
                      std::vector<std::string>& failures, std::unique_ptr<BrokerCore>& ledger_core,
                      std::size_t& ledger_sites, std::size_t& ledger_tranches) {
  for (const std::size_t sites : params.site_ladder) {
    const bool keep = sites == params.site_ladder.back();
    CoreBenchResult result = RunCoreWorkload(params, sites, 1U, keep, ledger_core);
    const std::string id = "broker.core.decision.sites." + std::to_string(sites);
    const std::string description = CoreScenarioDescription(params, sites, 1U, result.rounds,
                                                            result.warmup);
    if (!result.ok) {
      failures.push_back(id + ": " + result.failure);
      AddUnsupported(measurements, id + ".ns_per_decision", description,
                     "nanoseconds per decision", result.failure);
      AddUnsupported(measurements, id + ".decisions_per_second", description,
                     "decisions per second", result.failure);
      continue;
    }
    JsonValue detail = Object();
    PutInt(detail, "region_sites", static_cast<i64>(sites));
    PutInt(detail, "failure_domains_per_site", static_cast<i64>(params.tranches));
    PutInt(detail, "ask_fanout", 1);
    AddNanosecondPair(measurements, id, description, result.timing,
                      MergeDetail(TallyDetail(result.tally), detail));
    if (keep) {
      ledger_sites = sites;
      ledger_tranches = params.tranches;
    }
  }

  for (const std::size_t fanout : params.fanout_ladder) {
    std::unique_ptr<BrokerCore> unused;
    CoreBenchResult result = RunCoreWorkload(params, params.fanout_sites, fanout, false, unused);
    const std::string id = "broker.core.decision.fanout." + std::to_string(fanout);
    const std::string description = CoreScenarioDescription(params, params.fanout_sites, fanout,
                                                            result.rounds, result.warmup);
    if (!result.ok) {
      failures.push_back(id + ": " + result.failure);
      AddUnsupported(measurements, id + ".ns_per_decision", description,
                     "nanoseconds per decision", result.failure);
      AddUnsupported(measurements, id + ".decisions_per_second", description,
                     "decisions per second", result.failure);
      continue;
    }
    JsonValue detail = Object();
    PutInt(detail, "region_sites", static_cast<i64>(params.fanout_sites));
    PutInt(detail, "failure_domains_per_site", static_cast<i64>(params.tranches));
    PutInt(detail, "ask_fanout", static_cast<i64>(fanout));
    PutInt(detail, "sites_per_slice",
           static_cast<i64>((params.fanout_sites + fanout - 1U) / fanout));
    AddNanosecondPair(measurements, id, description, result.timing,
                      MergeDetail(TallyDetail(result.tally), detail));
  }
}

// ---- service scenarios ----------------------------------------------------

struct ServiceArmResult {
  bool ok = true;
  std::string failure;
  std::size_t sites = 0;
  std::size_t rounds = 0;
  Timing timing;
  DecisionTally tally;
  i64 journal_bytes = 0;
  bool reports_durable = false;
  bool store_backed = false;
};

ServiceArmResult RunServiceWorkload(const BenchParams& params,
                                    const std::filesystem::path& directory,
                                    DurabilityClass durability, std::size_t sites) {
  ServiceArmResult result;
  result.sites = sites;
  result.rounds = params.service_rounds;
  result.store_backed = !directory.empty();

  RegionModel model(params.seed, sites, params.tranches,
                    MakeId<rcb::ServiceClassIdTag>(params.service_class));
  const std::vector<std::size_t> whole = model.AllSites();

  ServiceConfig config;
  config.store_directory = directory.empty() ? std::string() : PathToUtf8(directory);
  config.durability = durability;
  config.worker_threads = 1;
  config.compaction_enabled = false;
  config.broker = BrokerConfig();

  Result<std::unique_ptr<BrokerService>> opened = BrokerService::Open(config);
  if (!opened.ok()) {
    result.ok = false;
    result.failure = "BrokerService::Open: " + opened.status().ToString();
    return result;
  }
  std::unique_ptr<BrokerService> service = std::move(opened.value());
  result.reports_durable = service->durable();

  const std::size_t total_rounds = params.service_warmup + params.service_rounds;
  for (std::size_t round = 0; round < total_rounds; ++round) {
    const u64 generation = static_cast<u64>(round) + 1ULL;
    for (std::size_t site = 0; site < sites; ++site) {
      const Result<OfferPublication> published =
          service->PublishOffer(model.MakeOffer(site, generation));
      if (!published.ok()) {
        result.ok = false;
        result.failure = "BrokerService::PublishOffer(site " + std::to_string(site) +
                         "): " + published.status().ToString();
        return result;
      }
    }
    const bool timed = round >= params.service_warmup;
    const std::string key = "service-ask-" + ZeroPadded(static_cast<u64>(round), 5);
    const Ask ask = model.MakeAsk(key, "requester-service", whole, kAskFactor);
    const i64 start = timed ? NowNs() : 0;
    Result<Decision> decision = service->AskNow(ask);
    const i64 stop = timed ? NowNs() : 0;
    if (!decision.ok()) {
      result.ok = false;
      result.failure = "BrokerService::AskNow(" + key + "): " + decision.status().ToString();
      return result;
    }
    if (timed) {
      result.timing.Add(stop - start);
      result.tally.Add(decision.value());
    }
  }

  if (result.store_backed) {
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(directory / "journal.log", error);
    if (!error) {
      result.journal_bytes = static_cast<i64>(bytes);
    }
  }
  const Status stopped = service->Shutdown(ShutdownMode::Drain);
  service.reset();
  if (!stopped.ok()) {
    result.ok = false;
    result.failure = "BrokerService::Shutdown: " + stopped.ToString();
  }
  return result;
}

void RunServiceScenarios(const Options& options, const BenchParams& params,
                         const std::filesystem::path& store_root, SplitMix64& rng,
                         std::vector<Measurement>& measurements, std::vector<std::string>& failures) {
  const std::size_t sites = params.service_sites;

  struct ArmPlan {
    std::string id;
    std::string tag;
    DurabilityClass durability;
    bool store_backed;
    std::string store_text;
    std::string durability_text;
    bool enabled;
  };
  const bool durable_enabled = options.durability == "durable";
  const std::vector<ArmPlan> arms = {
      {"service.durable.dir", "durable", DurabilityClass::Durable, true,
       "a real directory-backed store, one fsync per decision", "durable", durable_enabled},
      {"service.buffered.dir", "buffered", DurabilityClass::Buffered, true,
       "the same directory-backed store without a per-decision fsync", "buffered", true},
      {"service.volatile.memory", "volatile", DurabilityClass::Volatile, false,
       "an in-memory service (no store directory), every decision volatile", "volatile", true},
  };

  Timing durable_timing;
  Timing buffered_timing;
  Timing volatile_timing;
  bool durable_measured = false;
  bool buffered_measured = false;
  bool volatile_measured = false;
  std::string durable_reason;
  std::string buffered_reason;
  std::string volatile_reason;

  for (const ArmPlan& arm : arms) {
    const std::string description = ServiceScenarioDescription(
        sites, params.tranches, params.service_rounds, params.service_warmup, arm.store_text,
        arm.durability_text);
    if (!arm.enabled) {
      const std::string reason =
          "not measured: --durability buffered selected the buffered arm; pass --durability "
          "durable to measure the fsync-per-decision cost";
      AddUnsupported(measurements, arm.id + ".us_per_decision", description,
                     "microseconds per decision", reason);
      AddUnsupported(measurements, arm.id + ".decisions_per_second", description,
                     "decisions per second", reason);
      durable_reason = reason;
      continue;
    }

    std::filesystem::path directory;
    if (arm.store_backed) {
      directory = store_root / ("store-" + arm.tag + "-" + HexU64(rng.Next()));
      std::error_code error;
      std::filesystem::create_directories(directory, error);
      if (error) {
        const std::string reason =
            "the store directory '" + directory.string() + "' could not be created: " + error.message();
        failures.push_back(arm.id + ": " + reason);
        AddUnsupported(measurements, arm.id + ".us_per_decision", description,
                       "microseconds per decision", reason);
        AddUnsupported(measurements, arm.id + ".decisions_per_second", description,
                       "decisions per second", reason);
        continue;
      }
      CleanupPaths().push_back(PendingRemoval{directory, false});
    }

    ServiceArmResult result = RunServiceWorkload(params, directory, arm.durability, sites);
    if (!result.ok) {
      failures.push_back(arm.id + ": " + result.failure);
      AddUnsupported(measurements, arm.id + ".us_per_decision", description,
                     "microseconds per decision", result.failure);
      AddUnsupported(measurements, arm.id + ".decisions_per_second", description,
                     "decisions per second", result.failure);
      if (arm.tag == "durable") {
        durable_reason = result.failure;
      } else if (arm.tag == "buffered") {
        buffered_reason = result.failure;
      } else {
        volatile_reason = result.failure;
      }
      continue;
    }

    JsonValue detail = Object();
    PutInt(detail, "region_sites", static_cast<i64>(sites));
    PutInt(detail, "failure_domains_per_site", static_cast<i64>(params.tranches));
    PutText(detail, "service_durability", arm.durability_text);
    PutBool(detail, "service_reports_durable", result.reports_durable);
    PutBool(detail, "store_backed", result.store_backed);
    PutInt(detail, "journal_bytes", result.journal_bytes);
    AddMicrosecondPair(measurements, arm.id, description, result.timing,
                       MergeDetail(TallyDetail(result.tally), detail));

    if (arm.tag == "durable") {
      durable_timing = result.timing;
      durable_measured = result.timing.count > 0;
    } else if (arm.tag == "buffered") {
      buffered_timing = result.timing;
      buffered_measured = result.timing.count > 0;
    } else {
      volatile_timing = result.timing;
      volatile_measured = result.timing.count > 0;
    }
  }

  const std::string delta_unit = "microseconds per decision";
  const std::string delta_description =
      "measured fsync cost per completed durable decision: the identical ask set and region "
      "submitted through rcb::BrokerService twice, once against a directory-backed store with "
      "DurabilityClass::Durable (one fsync per decision) and once against the same kind of store "
      "with DurabilityClass::Buffered; the value is the difference of the two measured totals "
      "divided by the common decision count";
  const auto add_delta = [&](const std::string& id, const Timing& other, bool other_measured,
                             const std::string& other_text, const std::string& unavailable_reason) {
    if (durable_measured && other_measured) {
      const i64 count = std::min(durable_timing.count, other.count);
      const i64 delta_total = durable_timing.total_ns - other.total_ns;
      JsonValue detail = Object();
      PutInt(detail, "decisions", count);
      PutInt(detail, "durable_elapsed_ns", durable_timing.total_ns);
      PutInt(detail, "comparison_elapsed_ns", other.total_ns);
      PutInt(detail, "durable_exact_ns_per_decision", durable_timing.MeanNs());
      PutInt(detail, "comparison_exact_ns_per_decision", other.MeanNs());
      PutInt(detail, "delta_ns_per_decision", count == 0 ? 0 : delta_total / count);
      PutText(detail, "comparison", other_text);
      AddReal(measurements, id, delta_description, delta_unit, count,
              RoundDiv(count == 0 ? 0 : delta_total / count, 1000LL), detail);
      return;
    }
    std::string reason;
    if (!durable_measured) {
      reason = durable_reason;
    }
    if (reason.empty()) {
      reason = unavailable_reason;
    }
    if (reason.empty()) {
      reason = "one of the two arms produced no timed decision";
    }
    AddUnsupported(measurements, id, delta_description, delta_unit, reason);
  };
  add_delta("service.durable.minus.buffered", buffered_timing, buffered_measured,
            "directory-backed store with DurabilityClass::Buffered", buffered_reason);
  add_delta("service.durable.minus.volatile", volatile_timing, volatile_measured,
            "in-memory service (DurabilityClass::Volatile)", volatile_reason);
}

// ---- idempotent retry -----------------------------------------------------

struct RetryResult {
  bool ok = true;
  std::string failure;
  std::size_t asks = 0;
  std::size_t sites = 0;
  std::size_t replayed = 0;
  std::size_t replay_mismatch = 0;
  Timing commit_timing;
  Timing replay_timing;
  DecisionTally commit_tally;
  DecisionTally replay_tally;
};

RetryResult RunRetryScenario(const BenchParams& params) {
  RetryResult result;
  const std::size_t sites = params.retry_sites;
  result.sites = sites;

  RegionModel model(params.seed, sites, params.tranches,
                    MakeId<rcb::ServiceClassIdTag>(params.service_class));
  const std::vector<std::size_t> whole = model.AllSites();
  const Dims capacity = model.SliceBase(whole);

  Dims request{};
  for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
    i64 smallest = capacity[dimension];
    for (std::size_t site = 0; site < sites; ++site) {
      smallest = std::min(smallest, model.PerTrancheBase(site, dimension));
    }
    request[dimension] = std::max<i64>(1, smallest / 4);
  }

  std::size_t count = params.retry_target;
  for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
    if (request[dimension] <= 0) {
      continue;
    }
    const i64 allowed = capacity[dimension] / (2 * request[dimension]);
    const std::size_t bounded = allowed <= 0 ? 0U : static_cast<std::size_t>(allowed);
    count = std::min(count, bounded);
  }
  count = std::max<std::size_t>(count, 1U);
  result.asks = count;

  std::unique_ptr<BrokerCore> core = std::make_unique<BrokerCore>(BrokerConfig());
  for (std::size_t site = 0; site < sites; ++site) {
    const Result<OfferPublication> published = core->PublishOffer(model.MakeOffer(site, 1ULL));
    if (!published.ok()) {
      result.ok = false;
      result.failure = "PublishOffer(site " + std::to_string(site) + "): " +
                       published.status().ToString();
      return result;
    }
  }

  std::vector<Ask> asks;
  asks.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    Ask ask;
    ask.key = MakeId<rcb::AskKeyTag>("retry-" + ZeroPadded(static_cast<u64>(index), 6));
    ask.requester = MakeId<rcb::RequesterIdTag>("requester-retry");
    ask.service_class = MakeId<rcb::ServiceClassIdTag>(params.service_class);
    for (std::size_t dimension = 0; dimension < kDimensionCount; ++dimension) {
      ask.requested.Set(kDimensions[dimension], request[dimension]);
    }
    ask.priority = PriorityClass::Normal;
    ask.fairness = rcb::FairnessPolicy::StableSiteOrder;
    ask.as_of = kAsOf;
    ask.max_risk = RiskTier::Critical;
    ask.require_current_generation = true;
    asks.push_back(std::move(ask));
  }

  std::vector<rcb::DecisionId> committed_ids;
  committed_ids.reserve(count);
  for (const Ask& ask : asks) {
    const i64 start = NowNs();
    Result<AskPlan> plan = core->PlanAsk(ask);
    if (!plan.ok()) {
      result.ok = false;
      result.failure = "PlanAsk(commit " + ask.key.value() + "): " + plan.status().ToString();
      return result;
    }
    Result<Decision> decision = core->CommitPlan(plan.value());
    const i64 stop = NowNs();
    if (!decision.ok()) {
      result.ok = false;
      result.failure = "CommitPlan(" + ask.key.value() + "): " + decision.status().ToString();
      return result;
    }
    result.commit_timing.Add(stop - start);
    result.commit_tally.Add(decision.value());
    committed_ids.push_back(decision.value().id);
  }

  for (std::size_t index = 0; index < asks.size(); ++index) {
    const Ask& ask = asks[index];
    const i64 start = NowNs();
    Result<AskPlan> plan = core->PlanAsk(ask);
    if (!plan.ok()) {
      result.ok = false;
      result.failure = "PlanAsk(replay " + ask.key.value() + "): " + plan.status().ToString();
      return result;
    }
    Result<Decision> decision = core->CommitPlan(plan.value());
    const i64 stop = NowNs();
    if (!decision.ok()) {
      result.ok = false;
      result.failure = "CommitPlan(replay " + ask.key.value() + "): " + decision.status().ToString();
      return result;
    }
    result.replay_timing.Add(stop - start);
    result.replay_tally.Add(decision.value());
    if (decision.value().replay) {
      ++result.replayed;
    }
    if (decision.value().id != committed_ids[index]) {
      ++result.replay_mismatch;
    }
  }
  return result;
}

void RunRetryMeasurements(const BenchParams& params, std::vector<Measurement>& measurements,
                          std::vector<std::string>& failures) {
  RetryResult result = RunRetryScenario(params);
  const std::string description =
      "idempotent retry cost on rcb::BrokerCore: " + std::to_string(result.asks) +
      " distinct asks (keys retry-000000..., request a quarter of the smallest failure domain per "
      "site) are committed against a region of " +
      std::to_string(result.sites) + " sites with " + std::to_string(params.tranches) +
      " failure domains each, then the identical asks (same keys, same content) are submitted "
      "again; the commit phase timing covers PlanAsk plus CommitPlan, the replay phase covers the "
      "same calls served from the idempotency record, which plans nothing and touches no "
      "capacity";
  if (!result.ok) {
    failures.push_back("broker.core.retry: " + result.failure);
    AddUnsupported(measurements, "broker.core.retry.commit.ns_per_decision", description,
                   "nanoseconds per decision", result.failure);
    AddUnsupported(measurements, "broker.core.retry.replay.ns_per_decision", description,
                   "nanoseconds per replayed decision", result.failure);
    AddUnsupported(measurements, "broker.core.retry.replay_saving", description,
                   "nanoseconds saved per replay", result.failure);
    return;
  }

  JsonValue commit_detail = Object();
  PutInt(commit_detail, "asks", static_cast<i64>(result.asks));
  PutInt(commit_detail, "region_sites", static_cast<i64>(result.sites));
  AddNanosecondPair(measurements, "broker.core.retry.commit", description, result.commit_timing,
                    MergeDetail(TallyDetail(result.commit_tally), commit_detail));

  JsonValue replay_detail = Object();
  PutInt(replay_detail, "asks", static_cast<i64>(result.asks));
  PutInt(replay_detail, "region_sites", static_cast<i64>(result.sites));
  PutInt(replay_detail, "decisions_flagged_replay", static_cast<i64>(result.replayed));
  PutInt(replay_detail, "decision_identity_mismatches", static_cast<i64>(result.replay_mismatch));
  AddNanosecondPair(measurements, "broker.core.retry.replay", description, result.replay_timing,
                    MergeDetail(TallyDetail(result.replay_tally), replay_detail));

  const i64 commit_mean = result.commit_timing.MeanNs();
  const i64 replay_mean = result.replay_timing.MeanNs();
  JsonValue saving_detail = Object();
  PutInt(saving_detail, "commit_exact_ns_per_decision", commit_mean);
  PutInt(saving_detail, "replay_exact_ns_per_decision", replay_mean);
  PutInt(saving_detail, "commit_decisions", result.commit_timing.count);
  PutInt(saving_detail, "replay_decisions", result.replay_timing.count);
  PutInt(saving_detail, "replay_percent_of_commit",
         commit_mean == 0 ? 0 : (replay_mean * 100) / commit_mean);
  AddReal(measurements, "broker.core.retry.replay_saving",
          description + "; the saving is the commit path minus the replay path on the same asks",
          "nanoseconds saved per replay", std::min(result.commit_timing.count,
                                                   result.replay_timing.count),
          commit_mean - replay_mean, saving_detail);
}

// ---- ledger verification --------------------------------------------------

void RunVerificationScenario(const std::unique_ptr<BrokerCore>& core, std::size_t sites,
                             std::size_t tranches, std::vector<Measurement>& measurements) {
  const std::string description =
      "one rcb::BrokerCore::VerifyConservation() call over the largest ledger this run produced: " +
      std::to_string(sites) + " sites with " + std::to_string(tranches) +
      " failure domains each, re-deriving the conservation identity for every tranche and every "
      "dimension from the stored terms";
  if (core == nullptr) {
    AddUnsupported(measurements, "broker.core.verify_conservation",
                   "one rcb::BrokerCore::VerifyConservation() call over the final ledger",
                   "nanoseconds per verification",
                   "the ledger-producing scenario did not complete, so there is no ledger to "
                   "verify");
    return;
  }
  const i64 start = NowNs();
  const rcb::ConservationReport report = core->VerifyConservation();
  const i64 stop = NowNs();
  JsonValue detail = Object();
  PutBool(detail, "closed", report.closed);
  PutInt(detail, "sites_checked", static_cast<i64>(report.sites_checked));
  PutInt(detail, "tranches_checked", static_cast<i64>(report.tranches_checked));
  PutInt(detail, "commitments_checked", static_cast<i64>(report.commitments_checked));
  PutInt(detail, "live_commitments", static_cast<i64>(report.live_commitments));
  PutInt(detail, "revoked_commitments", static_cast<i64>(report.revoked_commitments));
  PutInt(detail, "violations", static_cast<i64>(report.violations.size()));
  PutInt(detail, "region_sites", static_cast<i64>(sites));
  PutInt(detail, "failure_domains_per_site", static_cast<i64>(tranches));
  PutInt(detail, "elapsed_ns", stop - start);
  AddReal(measurements, "broker.core.verify_conservation", description,
          "nanoseconds per verification", 1, stop - start, detail);
}

// ---- store root -----------------------------------------------------------

struct StorePlan {
  std::filesystem::path root;
  bool created_root = false;
};

StorePlan ResolveStoreRoot(const Options& options, SplitMix64& rng) {
  StorePlan plan;
  std::error_code error;
  if (options.store_given) {
    plan.root = std::filesystem::path(options.store_root);
    if (std::filesystem::exists(plan.root, error)) {
      if (error) {
        Fail(2, "cannot inspect --store '" + options.store_root + "': " + error.message());
      }
      if (!std::filesystem::is_directory(plan.root, error)) {
        Fail(2, "--store must name a directory: '" + options.store_root + "'");
      }
    } else {
      std::filesystem::create_directories(plan.root, error);
      if (error) {
        Fail(2, "cannot create --store '" + options.store_root + "': " + error.message());
      }
      plan.created_root = true;
    }
  } else {
    const std::filesystem::path temporary = std::filesystem::temp_directory_path(error);
    if (error) {
      Fail(1, std::string("no temporary directory is available for the durable store: ") +
                  error.message());
    }
    plan.root = temporary / ("rcb_bench-" + HexU64(options.seed) + "-" + HexU64(rng.Next()) + "-" +
                             HexU64(rng.Next()));
    std::filesystem::create_directories(plan.root, error);
    if (error) {
      Fail(1, "cannot create the temporary store root '" + plan.root.string() +
                  "': " + error.message());
    }
    plan.created_root = true;
  }
  if (plan.created_root) {
    CleanupPaths().push_back(PendingRemoval{plan.root, options.store_given});
  }
  return plan;
}

// ---- document -------------------------------------------------------------

JsonValue EnvironmentFacts() {
  JsonValue environment = Object();
#if defined(_WIN32)
  PutText(environment, "os", "windows");
#elif defined(__linux__)
  PutText(environment, "os", "linux");
#elif defined(__APPLE__)
  PutText(environment, "os", "macos");
#else
  PutText(environment, "os", "unknown");
#endif
#if defined(_MSC_VER)
  PutText(environment, "compiler", "msvc");
  PutText(environment, "compiler_version", std::to_string(static_cast<long long>(_MSC_FULL_VER)));
  PutInt(environment, "msvc_version", static_cast<i64>(_MSC_VER));
#elif defined(__clang__)
  PutText(environment, "compiler", "clang");
  PutText(environment, "compiler_version", __VERSION__);
#elif defined(__GNUC__)
  PutText(environment, "compiler", "gcc");
  PutText(environment, "compiler_version", __VERSION__);
#else
  PutText(environment, "compiler", "unknown");
  PutText(environment, "compiler_version", "unknown");
#endif
#ifdef NDEBUG
  PutText(environment, "build_type", "release");
  PutBool(environment, "ndebug", true);
#else
  PutText(environment, "build_type", "debug");
  PutBool(environment, "ndebug", false);
#endif
  PutInt(environment, "cpp_standard", static_cast<i64>(__cplusplus));
  PutInt(environment, "hardware_concurrency",
         static_cast<i64>(std::thread::hardware_concurrency()));
  PutInt(environment, "pointer_bits", static_cast<i64>(sizeof(void*) * 8U));
  PutText(environment, "steady_clock_period",
          std::to_string(std::chrono::steady_clock::period::num) + "/" +
              std::to_string(std::chrono::steady_clock::period::den));
  PutText(environment, "rcb_version", rcb::VersionString());
  PutInt(environment, "rcb_persistence_format_version",
         static_cast<i64>(rcb::kPersistenceFormatVersion));
  return environment;
}

JsonValue ScenarioList(const BenchParams& params) {
  JsonValue scenarios = JsonValue::MakeArray();
  const auto push = [&scenarios](const std::string& id, const std::string& description) {
    JsonValue entry = Object();
    PutText(entry, "id", id);
    PutText(entry, "description", description);
    scenarios.Push(std::move(entry));
  };
  push("single-ask",
       "One ask per round over a synthetic region of N sites with " +
           std::to_string(params.tranches) +
           " failure domains per site. Every domain publishes power, cooling, rack space and "
           "service-class capacity; every round every site publishes a new generation that adds "
           "fresh headroom, and one ask that exceeds the reachable allocatable capacity is "
           "planned and committed, so the decision is partially accepted and the allocator "
           "distributes the commitment. Measured at " +
           std::to_string(params.site_ladder.size()) + " site counts.");
  push("ask-fanout",
       "The same region with " + std::to_string(params.fanout_ladder.size()) +
           " fan-out levels: M asks per round, each scoped by locality to one contiguous slice "
           "of the region and each carrying its own idempotency key, so decision cost under "
           "fan-out can be compared with the single-ask case. Region: " +
           std::to_string(params.fanout_sites) + " sites; " + std::to_string(params.fanout_rounds) +
           " timed rounds per level.");
  push("durable-commit",
       "The same ask set submitted through rcb::BrokerService::AskNow against a real "
       "directory-backed store with DurabilityClass::Durable (one fsync per decision), the same "
       "kind of store with DurabilityClass::Buffered, and an in-memory service; the fsync cost "
       "per decision is the measured difference. Region: " + std::to_string(params.service_sites) +
           " sites, " + std::to_string(params.service_rounds) + " timed rounds per arm.");
  push("idempotent-retry",
       "M distinct asks are committed, then the identical M asks with the same idempotency keys "
       "are re-submitted; the replay path (served from the idempotency record) is measured "
       "separately from the commit path.");
  push("ledger-verification",
       "One rcb::BrokerCore::VerifyConservation() call over the largest ledger this run "
       "produced, with the ledger size it covered.");
  return scenarios;
}

JsonValue MeasurementsJson(const std::vector<Measurement>& measurements) {
  JsonValue array = JsonValue::MakeArray();
  for (const Measurement& measurement : measurements) {
    JsonValue entry = Object();
    PutText(entry, "id", measurement.id);
    PutText(entry, "description", measurement.description);
    PutText(entry, "unit", measurement.unit);
    PutInt(entry, "samples", measurement.samples);
    if (measurement.measured) {
      PutInt(entry, "value", measurement.value);
    } else {
      PutValue(entry, "value", JsonValue());
      PutText(entry, "reason", measurement.reason);
    }
    PutText(entry, "evidence", measurement.evidence);
    if (measurement.detail.IsObject() && !measurement.detail.members().empty()) {
      PutValue(entry, "detail", measurement.detail);
    }
    array.Push(std::move(entry));
  }
  return array;
}

void PrintSummary(const std::vector<Measurement>& measurements) {
  std::fputs("rcb_bench: measurements (the canonical JSON document is on stdout)\n", stderr);
  for (const Measurement& measurement : measurements) {
    std::string line = "  " + measurement.id + " [" + measurement.unit +
                       "] samples=" + std::to_string(measurement.samples) + " ";
    if (measurement.measured) {
      line += "value=" + std::to_string(measurement.value);
    } else {
      line += "UNSUPPORTED (" + measurement.reason + ")";
    }
    line += "\n";
    std::fputs(line.c_str(), stderr);
  }
  std::fflush(stderr);
}

bool WriteDocument(const std::filesystem::path& path, const std::string& text,
                   std::string& error) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    error = "cannot open '" + path.string() + "' for writing";
    return false;
  }
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
  stream.flush();
  if (!stream) {
    error = "cannot write the document to '" + path.string() + "'";
    return false;
  }
  stream.close();
  if (stream.fail()) {
    error = "cannot close '" + path.string() + "' after writing";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::atexit(&CleanupOnExit);

  Options options;
  std::string error;
  if (!ParseOptions(argc, argv, options, error)) {
    std::fputs("rcb_bench: ", stderr);
    std::fputs(error.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fputs(kUsage, stderr);
    return 2;
  }
  if (options.help) {
    std::fputs(kUsage, stdout);
    std::fflush(stdout);
    return 0;
  }

  // The service class must be a valid identifier before anything is built.
  const Result<ServiceClassId> service_class = ServiceClassId::Make(options.service_class);
  if (!service_class.ok()) {
    std::fputs("rcb_bench: --service-class is not a valid identifier: ", stderr);
    std::fputs(service_class.status().ToString().c_str(), stderr);
    std::fputc('\n', stderr);
    return 2;
  }

  const BenchParams params = MakeParams(options);
  SplitMix64 rng(options.seed ^ 0xD1B54A32D192ED03ULL);
  const StorePlan store = ResolveStoreRoot(options, rng);

  std::vector<Measurement> measurements;
  std::vector<std::string> notes;
  std::vector<std::string> failures;
  std::unique_ptr<BrokerCore> ledger_core;
  std::size_t ledger_sites = 0;
  std::size_t ledger_tranches = params.tranches;

  RunCoreScenarios(params, measurements, failures, ledger_core, ledger_sites, ledger_tranches);
  RunServiceScenarios(options, params, store.root, rng, measurements, failures);
  RunRetryMeasurements(params, measurements, failures);
  RunVerificationScenario(ledger_core, ledger_sites, ledger_tranches, measurements);

  notes.push_back(
      "the measured unit is one completed brokerage operation: BrokerCore::PlanAsk followed by "
      "BrokerCore::CommitPlan for the kernel, BrokerService::AskNow for the service. Submission, "
      "enqueue and asynchronous-handle latency are never measured.");
  notes.push_back(
      "every decision is timed individually with two std::chrono::steady_clock reads; the "
      "reported per-decision figure is the sum of those intervals divided by the number of "
      "completed decisions, and the rate is the decision count divided by that sum. The clock "
      "read overhead is inside the interval.");
  notes.push_back(
      "region replenishment (publishing the next offer generation for every site) happens "
      "between timed intervals and is therefore not part of any decision measurement.");
  notes.push_back(
      "the synthetic region is generated from seed " + std::to_string(options.seed) +
      " with a first-party splitmix64 generator; no <random> distribution is used, so the "
      "workload is identical on every standard library.");
  notes.push_back(
      "all accounting and all reported values are integers; rcb::JsonValue cannot represent a "
      "floating-point number, so rates are integer-truncated and every measurement carries the "
      "exact elapsed nanoseconds it was derived from.");
  notes.push_back(
      "the directory-backed service arms run with automatic compaction disabled so that the "
      "per-decision commit cost is what is measured; the durable arm performs exactly one "
      "journal fsync per decision (Journal::MarkCommitted with DurabilityClass::Durable).");
  notes.push_back(
      "durability evidence recorded per service arm: the configured durability class, the "
      "service's own durable() flag, and the number of journal bytes the arm left on disk after "
      "the run. The fsync cost itself is the measured difference between the durable and buffered "
      "arms, which submit an identical ask set.");

  const CleanupReport cleanup = CleanupStores();
  if (!cleanup.remaining.empty()) {
    failures.push_back("store directories could not be removed: " + cleanup.remaining.front());
  }

  JsonValue document = Object();
  PutText(document, "schema", "rcb_bench/1");
  PutUint(document, "seed", options.seed);
  PutValue(document, "environment", EnvironmentFacts());

  JsonValue parameters = Object();
  PutInt(parameters, "sites", static_cast<i64>(options.sites));
  PutInt(parameters, "tranches", static_cast<i64>(options.tranches));
  PutInt(parameters, "asks", static_cast<i64>(options.asks));
  PutText(parameters, "service_class", options.service_class);
  PutText(parameters, "durability", options.durability);
  PutBool(parameters, "quick", options.quick);
  PutBool(parameters, "json_only", options.json_only);
  PutInt(parameters, "warmup_rounds", static_cast<i64>(params.warmup_rounds));
  PutText(parameters, "store_root", PathToUtf8(store.root));
  PutBool(parameters, "store_given", options.store_given);
  if (options.out_given) {
    PutText(parameters, "out", options.out_path);
  } else {
    PutValue(parameters, "out", JsonValue());
  }
  JsonValue site_ladder = JsonValue::MakeArray();
  for (const std::size_t sites : params.site_ladder) {
    site_ladder.Push(JsonValue::MakeUint(static_cast<u64>(sites)));
  }
  PutValue(parameters, "site_ladder", std::move(site_ladder));
  JsonValue fanout_ladder = JsonValue::MakeArray();
  for (const std::size_t fanout : params.fanout_ladder) {
    fanout_ladder.Push(JsonValue::MakeUint(static_cast<u64>(fanout)));
  }
  PutValue(parameters, "fanout_ladder", std::move(fanout_ladder));
  PutValue(document, "parameters", std::move(parameters));

  PutValue(document, "scenarios", ScenarioList(params));
  PutValue(document, "measurements", MeasurementsJson(measurements));

  JsonValue stores = Object();
  PutText(stores, "root", PathToUtf8(store.root));
  PutBool(stores, "created_by_benchmark", store.created_root);
  PutInt(stores, "directories_created", static_cast<i64>(cleanup.attempted));
  PutBool(stores, "all_removed", cleanup.remaining.empty());
  JsonValue remaining = JsonValue::MakeArray();
  for (const std::string& path : cleanup.remaining) {
    remaining.Push(JsonValue::MakeString(path));
  }
  PutValue(stores, "remaining", std::move(remaining));
  PutValue(document, "stores", std::move(stores));

  JsonValue failure_array = JsonValue::MakeArray();
  for (const std::string& failure : failures) {
    failure_array.Push(JsonValue::MakeString(failure));
  }
  PutValue(document, "failures", std::move(failure_array));

  JsonValue note_array = JsonValue::MakeArray();
  for (const std::string& note : notes) {
    note_array.Push(JsonValue::MakeString(note));
  }
  PutValue(document, "notes", std::move(note_array));

  const std::string text = rcb::EncodeJson(document, false);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  bool out_ok = true;
  std::string out_error;
  if (options.out_given) {
    out_ok = WriteDocument(std::filesystem::path(options.out_path), text + "\n", out_error);
  }

  if (!options.json_only) {
    PrintSummary(measurements);
    if (!failures.empty()) {
      std::fputs("rcb_bench: scenario failures:\n", stderr);
      for (const std::string& failure : failures) {
        const std::string line = "  " + failure + "\n";
        std::fputs(line.c_str(), stderr);
      }
      std::fflush(stderr);
    }
  }

  if (!out_ok) {
    std::fputs("rcb_bench: ", stderr);
    std::fputs(out_error.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    return 3;
  }
  return 0;
}
