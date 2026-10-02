// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Multiprocess suite: every case here runs the real rcb executable as a real
// independent operating-system process against a real store directory. Nothing
// is simulated in-process: each invocation is a genuine open, recovery, work and
// close, so restart, torn-tail repair, writer fencing and cross-process
// idempotency are proven on the artefact an operator uses.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rcb/json.hpp"
#include "rcb/rcb.hpp"
#include "test_framework.hpp"
#include "test_main.hpp"

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#endif

#ifndef RCB_CLI_PATH
#error "RCB_CLI_PATH must name the rcb executable under test"
#endif

namespace {

namespace fs = std::filesystem;

class TempDir {
 public:
  TempDir() {
    static int counter = 0;
    ++counter;
    path_ = fs::temp_directory_path() /
            ("rcb-multiproc-" + std::to_string(counter) + "-" + std::to_string(std::rand()));
    std::error_code error;
    fs::remove_all(path_, error);
    fs::create_directories(path_, error);
  }
  ~TempDir() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  [[nodiscard]] const fs::path& path() const { return path_; }
  [[nodiscard]] std::string str() const { return path_.string(); }

 private:
  fs::path path_;
};

struct CliResult {
  int exit_code = -1;
  std::string output;
  bool parsed = false;
  rcb::JsonValue json;
};

int NormalizeExitCode(const int raw) {
#if defined(_WIN32)
  return raw;
#else
  if (raw == -1) {
    return -1;
  }
  return WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
}

/// Quotes a path for the platform shell.
std::string Quote(const std::string& text) {
#if defined(_WIN32)
  std::string quoted = "\"";
  for (const char c : text) {
    if (c == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(c);
    }
  }
  quoted += "\"";
  return quoted;
#else
  std::string quoted = "'";
  for (const char c : text) {
    if (c == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(c);
    }
  }
  quoted += "'";
  return quoted;
#endif
}

/// Runs the CLI with the given arguments, capturing stdout into a file.
CliResult RunCli(const std::vector<std::string>& arguments) {
  TempDir scratch;
  const std::string output_path = (scratch.path() / "out.json").string();
#if defined(_WIN32)
  // The command line must not begin with a quote: cmd.exe strips the first and
  // last quote of such a line, which would unquote a path containing spaces.
  // Prefixing with the builtin "call" keeps the quoting intact and still
  // propagates the exit code of the program it calls.
  std::string command = "call ";
#else
  std::string command;
#endif
  command += Quote(RCB_CLI_PATH);
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command += Quote(argument);
  }
  command += " > ";
  command += Quote(output_path);
  command += " 2>&1";

  CliResult result;
  result.exit_code = NormalizeExitCode(std::system(command.c_str()));
  std::ifstream stream(output_path, std::ios::binary);
  if (stream.is_open()) {
    result.output.assign(std::istreambuf_iterator<char>(stream),
                         std::istreambuf_iterator<char>());
  }
  rcb::Result<rcb::JsonValue> parsed = rcb::ParseJson(result.output);
  if (parsed.ok()) {
    result.parsed = true;
    result.json = parsed.value();
  }
  return result;
}

/// Writes a document to a temporary file and returns its path.
std::string WriteDocument(const std::string& name, const std::string& text) {
  static TempDir documents;
  const fs::path path = documents.path() / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << text;
  stream.close();
  return path.string();
}

bool JsonTrue(const rcb::JsonValue& value, const char* key) {
  const rcb::JsonValue* member = value.Find(key);
  return member != nullptr && member->IsBool() && member->AsBool();
}

std::string JsonString(const rcb::JsonValue& value, const char* key) {
  const rcb::JsonValue* member = value.Find(key);
  return member != nullptr && member->IsString() ? member->AsString() : std::string();
}

const char* kOffer = R"({
  "schema": 1,
  "site": "dc-alpha",
  "generation": 1,
  "source_snapshot": "snap-1",
  "service_class": "gpu-h100",
  "region": "eu-north",
  "jurisdiction": "eu",
  "risk": "nominal",
  "valid_from": 0,
  "valid_until": 100000,
  "reserve_policy": "rp-1",
  "policy_generation": 1,
  "cost": {
    "reference": "ev-1",
    "price_per_milli_unit": "0.0004",
    "energy_millijoules_per_milli_unit": 1000,
    "carbon_milligrams_per_milli_unit": 5
  },
  "tranches": [
    {
      "domain": "hall-a",
      "allocatable": {"power": 100000, "cooling": 90000, "rack_space": 20, "service_capacity": 40000},
      "protected_reserve": {"power": 10000, "cooling": 9000, "rack_space": 2, "service_capacity": 4000}
    },
    {
      "domain": "hall-b",
      "allocatable": {"power": 60000, "cooling": 55000, "rack_space": 12, "service_capacity": 24000},
      "protected_reserve": {"power": 0, "cooling": 0, "rack_space": 0, "service_capacity": 0}
    }
  ]
})";

std::string AskDocument(const std::string& key, const int service_capacity) {
  return std::string(R"({
  "schema": 1,
  "key": ")") + key + R"(",
  "requester": "platform",
  "service_class": "gpu-h100",
  "requested": {"power": 0, "cooling": 0, "rack_space": 0, "service_capacity": )" +
         std::to_string(service_capacity) + R"(},
  "priority": "high",
  "as_of": 10
})";
}

}  // namespace

RCB_TEST(an_offer_and_an_ask_survive_separate_processes) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  const std::string ask_path = WriteDocument("ask.json", AskDocument("order-1", 20000));

  const CliResult published =
      RunCli({"--store", store.str(), "offer", "--file", offer_path});
  RCB_REQUIRE(published.parsed);
  RCB_CHECK_EQ(published.exit_code, 0);
  RCB_CHECK(JsonTrue(published.json, "ok"));

  const CliResult decision = RunCli({"--store", store.str(), "ask", "--file", ask_path});
  RCB_REQUIRE(decision.parsed);
  RCB_CHECK_EQ(decision.exit_code, 0);
  RCB_CHECK_EQ(JsonString(decision.json, "outcome"), std::string("accepted"));
  const std::string decision_id = JsonString(decision.json, "id");
  RCB_CHECK(!decision_id.empty());

  // A second process re-submitting the same ask must recognise the retry.
  const CliResult retry = RunCli({"--store", store.str(), "ask", "--file", ask_path});
  RCB_REQUIRE(retry.parsed);
  RCB_CHECK_EQ(retry.exit_code, 0);
  RCB_CHECK(JsonTrue(retry.json, "replay"));
  RCB_CHECK_EQ(JsonString(retry.json, "id"), decision_id);

  const CliResult verify = RunCli({"--store", store.str(), "verify"});
  RCB_REQUIRE(verify.parsed);
  RCB_CHECK_EQ(verify.exit_code, 0);
  RCB_CHECK(JsonTrue(verify.json, "closed"));

  const CliResult status = RunCli({"--store", store.str(), "status"});
  RCB_REQUIRE(status.parsed);
  const rcb::JsonValue* accounting = status.json.Find("accounting");
  RCB_REQUIRE(accounting != nullptr);
  const rcb::JsonValue* committed = accounting->Find("committed_total");
  RCB_REQUIRE(committed != nullptr);
  const rcb::JsonValue* service = committed->Find("service_capacity");
  RCB_REQUIRE(service != nullptr);
  RCB_CHECK(service->IsNumber());
  RCB_CHECK_EQ(service->AsInt(), rcb::i64{20000});
}

RCB_TEST(successive_processes_accumulate_exactly_and_conserve) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  RCB_REQUIRE(RunCli({"--store", store.str(), "offer", "--file", offer_path}).exit_code == 0);

  rcb::i64 expected = 0;
  for (int index = 0; index < 6; ++index) {
    const std::string path =
        WriteDocument("ask-" + std::to_string(index) + ".json", AskDocument("order-" + std::to_string(index), 4000));
    const CliResult result = RunCli({"--store", store.str(), "ask", "--file", path});
    RCB_REQUIRE(result.parsed);
    RCB_REQUIRE(result.exit_code == 0);
    expected += 4000;
  }

  const CliResult verify = RunCli({"--store", store.str(), "verify"});
  RCB_REQUIRE(verify.parsed);
  RCB_CHECK(JsonTrue(verify.json, "closed"));
  const rcb::JsonValue* committed = verify.json.Find("committed_total");
  RCB_REQUIRE(committed != nullptr);
  const rcb::JsonValue* service = committed->Find("service_capacity");
  RCB_REQUIRE(service != nullptr);
  RCB_CHECK_EQ(service->AsInt(), expected);

  // The recovery report of a later process must show a clean history.
  const CliResult recover = RunCli({"--store", store.str(), "recover"});
  RCB_REQUIRE(recover.parsed);
  const rcb::JsonValue* recovery = recover.json.Find("recovery");
  RCB_REQUIRE(recovery != nullptr);
  RCB_CHECK(JsonTrue(*recovery, "clean_close"));
  RCB_CHECK(!JsonTrue(*recovery, "torn_tail"));
}

RCB_TEST(a_torn_write_is_repaired_by_a_later_process) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  RCB_REQUIRE(RunCli({"--store", store.str(), "offer", "--file", offer_path}).exit_code == 0);
  const std::string ask_path = WriteDocument("ask.json", AskDocument("order-torn", 4000));
  RCB_REQUIRE(RunCli({"--store", store.str(), "ask", "--file", ask_path}).exit_code == 0);

  const fs::path journal = store.path() / "journal.log";
  RCB_REQUIRE(fs::exists(journal));
  const std::uintmax_t size = fs::file_size(journal);
  RCB_REQUIRE(size > 40);
  // Simulate a process killed in the middle of writing its last record.
  fs::resize_file(journal, size - 11);

  const CliResult recover = RunCli({"--store", store.str(), "recover"});
  RCB_REQUIRE(recover.parsed);
  RCB_CHECK_EQ(recover.exit_code, 0);
  const rcb::JsonValue* recovery = recover.json.Find("recovery");
  RCB_REQUIRE(recovery != nullptr);
  RCB_CHECK(JsonTrue(*recovery, "torn_tail"));
  RCB_CHECK(JsonTrue(*recovery, "torn_tail_repaired"));

  const CliResult verify = RunCli({"--store", store.str(), "verify"});
  RCB_REQUIRE(verify.parsed);
  RCB_CHECK(JsonTrue(verify.json, "closed"));
  const rcb::JsonValue* committed = verify.json.Find("committed_total");
  RCB_REQUIRE(committed != nullptr);
  const rcb::JsonValue* service = committed->Find("service_capacity");
  RCB_REQUIRE(service != nullptr);
  RCB_CHECK_EQ(service->AsInt(), rcb::i64{4000});
}

RCB_TEST(interior_corruption_stops_a_process_with_a_persistence_exit) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  RCB_REQUIRE(RunCli({"--store", store.str(), "offer", "--file", offer_path}).exit_code == 0);
  for (int index = 0; index < 3; ++index) {
    const std::string path = WriteDocument("ask-" + std::to_string(index) + ".json",
                                           AskDocument("order-" + std::to_string(index), 2000));
    RCB_REQUIRE(RunCli({"--store", store.str(), "ask", "--file", path}).exit_code == 0);
  }
  const fs::path journal = store.path() / "journal.log";
  const std::uintmax_t size_before = fs::file_size(journal);
  RCB_REQUIRE(size_before > 400);

  // Damage a byte that cannot be part of a torn tail.
  {
    std::fstream stream(journal, std::ios::in | std::ios::out | std::ios::binary);
    RCB_REQUIRE(stream.is_open());
    stream.seekg(150);
    char byte = 0;
    stream.read(&byte, 1);
    byte = static_cast<char>(byte ^ 0x33);
    stream.seekp(150);
    stream.write(&byte, 1);
  }

  const CliResult status = RunCli({"--store", store.str(), "status"});
  RCB_REQUIRE(status.parsed);
  RCB_CHECK_EQ(status.exit_code, 4);
  RCB_CHECK_EQ(JsonString(status.json, "error"), std::string("persistence_interior_corruption"));
  // The damaged store was not "repaired" behind the operator's back.
  RCB_CHECK_EQ(fs::file_size(journal), size_before);
}

RCB_TEST(two_processes_on_one_store_cannot_corrupt_it) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  RCB_REQUIRE(RunCli({"--store", store.str(), "offer", "--file", offer_path}).exit_code == 0);

  // One process does a long durable workload; another opens the same store while
  // it is running. Whether the second is refused by the store lock or simply
  // runs after the first, the store must end up consistent and must account for
  // exactly the work that reported success.
  std::string bulk = "[";
  for (int index = 0; index < 400; ++index) {
    if (index != 0) {
      bulk += ",";
    }
    bulk += AskDocument("bulk-" + std::to_string(index), 10);
  }
  bulk += "]";
  const std::string bulk_path = WriteDocument("bulk.json", bulk);
  const std::string single_path = WriteDocument("single.json", AskDocument("single-1", 10));

  CliResult bulk_result;
  std::thread bulk_thread([&] {
    bulk_result = RunCli({"--store", store.str(), "asks", "--file", bulk_path});
  });
  const CliResult single_result = RunCli({"--store", store.str(), "ask", "--file", single_path});
  bulk_thread.join();

  RCB_REQUIRE(bulk_result.parsed);
  RCB_REQUIRE(single_result.parsed);
  const bool bulk_ok = bulk_result.exit_code == 0;
  const bool single_ok = single_result.exit_code == 0;

  // Losing the writer lock is not corruption: the loser must fail as a
  // persistence error and must not have published anything.
  if (!bulk_ok) {
    RCB_CHECK_EQ(bulk_result.exit_code, 4);
    RCB_CHECK_EQ(JsonString(bulk_result.json, "category"), std::string("persistence"));
  }
  if (!single_ok) {
    RCB_CHECK_EQ(single_result.exit_code, 4);
    RCB_CHECK_EQ(JsonString(single_result.json, "category"), std::string("persistence"));
  }
  RCB_CHECK(bulk_ok || single_ok);

  const CliResult verify = RunCli({"--store", store.str(), "verify"});
  RCB_REQUIRE(verify.parsed);
  RCB_CHECK(JsonTrue(verify.json, "closed"));
  const rcb::JsonValue* committed = verify.json.Find("committed_total");
  RCB_REQUIRE(committed != nullptr);
  const rcb::JsonValue* service = committed->Find("service_capacity");
  RCB_REQUIRE(service != nullptr);
  // Exactly the work that reported success is accounted for.
  const rcb::i64 expected = (bulk_ok ? rcb::i64{4000} : rcb::i64{0}) +
                            (single_ok ? rcb::i64{10} : rcb::i64{0});
  RCB_CHECK_EQ(service->AsInt(), expected);
}

RCB_TEST(a_refused_ask_exits_with_the_constraint_code) {
  TempDir store;
  const std::string offer_path = WriteDocument("offer.json", kOffer);
  RCB_REQUIRE(RunCli({"--store", store.str(), "offer", "--file", offer_path}).exit_code == 0);

  // 200000 exceeds every pool in the offer. A partial request is still a
  // successful brokerage, so the process exits 0 and names what blocked the
  // rest.
  const std::string ask_path = WriteDocument("too-big.json", AskDocument("order-too-big", 200000));
  const CliResult partial = RunCli({"--store", store.str(), "ask", "--file", ask_path});
  RCB_REQUIRE(partial.parsed);
  RCB_CHECK_EQ(partial.exit_code, 0);
  RCB_CHECK_EQ(JsonString(partial.json, "outcome"), std::string("partially_accepted"));
  const rcb::JsonValue* blocking = partial.json.Find("blocking");
  RCB_REQUIRE(blocking != nullptr);
  RCB_CHECK(blocking->IsArray());
  RCB_CHECK(!blocking->items().empty());

  // The same demand with all-or-nothing semantics is refused, and the process
  // says so through its exit code as well as its output.
  std::string atomic = AskDocument("order-atomic", 200000);
  atomic.insert(atomic.size() - 1, ",\n  \"all_or_nothing\": true");
  const std::string atomic_path = WriteDocument("atomic.json", atomic);
  const CliResult result = RunCli({"--store", store.str(), "ask", "--file", atomic_path});
  RCB_REQUIRE(result.parsed);
  RCB_CHECK_EQ(result.exit_code, 3);
  RCB_CHECK_EQ(JsonString(result.json, "outcome"), std::string("refused"));
  const rcb::JsonValue* atomic_blocking = result.json.Find("blocking");
  RCB_REQUIRE(atomic_blocking != nullptr);
  RCB_CHECK(!atomic_blocking->items().empty());

  const CliResult verify = RunCli({"--store", store.str(), "verify"});
  RCB_REQUIRE(verify.parsed);
  RCB_CHECK(JsonTrue(verify.json, "closed"));
}
