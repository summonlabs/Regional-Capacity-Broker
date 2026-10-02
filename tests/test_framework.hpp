// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// A deliberately small test harness: registration, checks with file and line,
// an independent count of checks and failures, and a non-zero exit when
// anything fails. No timeouts, no retries, no hidden skips: a suite either
// passes or says exactly which check failed.

#ifndef RCB_TEST_FRAMEWORK_HPP
#define RCB_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rcb/status.hpp"

namespace rcbtest {

using TestFunction = void (*)();

struct TestCase {
  const char* name = "";
  TestFunction function = nullptr;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

inline int& CheckCount() {
  static int count = 0;
  return count;
}

inline int& FailureCount() {
  static int count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, TestFunction function) {
    Registry().push_back(TestCase{name, function});
  }
};

inline void ReportFailure(const char* file, int line, const std::string& message) {
  ++FailureCount();
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, message.c_str());
  std::fflush(stderr);
}

inline int RunAll(const char* suite_name) {
  int failed_tests = 0;
  for (const TestCase& test : Registry()) {
    std::fprintf(stderr, "RUN %s::%s\n", suite_name, test.name);
    std::fflush(stderr);
    const int failures_before = FailureCount();
    test.function();
    if (FailureCount() != failures_before) {
      ++failed_tests;
      std::fprintf(stderr, "FAILED %s::%s\n", suite_name, test.name);
      std::fflush(stderr);
    }
  }
  std::printf("%s: %zu tests, %d checks, %d failed checks, %d failed tests\n", suite_name,
              Registry().size(), CheckCount(), FailureCount(), failed_tests);
  std::fflush(stdout);
  return failed_tests == 0 ? 0 : 1;
}

/// Accepts either a Result<T> or a bare Status, so one macro covers both.
template <class T>
const rcb::Status& AsStatus(const rcb::Result<T>& result) {
  return result.status();
}
inline const rcb::Status& AsStatus(const rcb::Status& status) { return status; }

/// Renders a value for a failure message.
template <class T>
std::string Describe(const T& value) {
  if constexpr (std::is_convertible_v<T, std::string>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

}  // namespace rcbtest

#define RCB_TEST(name)                                                          \
  static void name();                                                           \
  static const ::rcbtest::Registrar rcb_registrar_##name(#name, &name);         \
  static void name()

#define RCB_CHECK(expression)                                                   \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    if (!(expression)) {                                                        \
      ::rcbtest::ReportFailure(__FILE__, __LINE__, "check failed: " #expression); \
    }                                                                           \
  } while (false)

#define RCB_CHECK_EQ(actual, expected)                                          \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    const auto& rcb_actual_value = (actual);                                    \
    const auto& rcb_expected_value = (expected);                                \
    if (!(rcb_actual_value == rcb_expected_value)) {                            \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                              \
                               std::string("check failed: " #actual " == " #expected " (got ") + \
                                   ::rcbtest::Describe(rcb_actual_value) + ")"); \
    }                                                                           \
  } while (false)

#define RCB_REQUIRE(expression)                                                 \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    if (!(expression)) {                                                        \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                              \
                               "requirement failed: " #expression);             \
      return;                                                                   \
    }                                                                           \
  } while (false)

/// Asserts that a Result or a bare Status carries the expected error code.
#define RCB_CHECK_ERROR(result, expected_code)                                  \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    const auto& rcb_result_holder = (result);                                   \
    const ::rcb::Status& rcb_status_value = ::rcbtest::AsStatus(rcb_result_holder); \
    if (rcb_status_value.ok()) {                                                \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                              \
                               std::string("expected failure " #expected_code    \
                                           " but the call succeeded"));         \
    } else if (rcb_status_value.code() != (expected_code)) {                    \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                              \
                               std::string("expected failure " #expected_code    \
                                           " but got ") +                       \
                                   std::string(rcb_status_value.token()) + ": " + \
                                   rcb_status_value.detail());                  \
    }                                                                           \
  } while (false)

/// Asserts that a Result carries a value.
#define RCB_REQUIRE_OK(result)                                                  \
  do {                                                                          \
    ++::rcbtest::CheckCount();                                                  \
    const auto& rcb_result_holder = (result);                                   \
    if (!rcb_result_holder.ok()) {                                              \
      ::rcbtest::ReportFailure(__FILE__, __LINE__,                              \
                               std::string("expected success but got ") +       \
                                   ::rcbtest::AsStatus(rcb_result_holder).ToString()); \
      return;                                                                   \
    }                                                                           \
  } while (false)

#endif  // RCB_TEST_FRAMEWORK_HPP
