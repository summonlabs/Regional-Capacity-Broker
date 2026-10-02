// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Internal invariant checks. These are for programmer errors and impossible
// states, never for untrusted input: untrusted input is validated and returns
// Status. An assertion firing is a defect in this runtime.

#ifndef RCB_ASSERT_HPP
#define RCB_ASSERT_HPP

#include <cstdio>
#include <cstdlib>

namespace rcb {
namespace detail {

[[noreturn]] inline void AssertFailure(const char* expression, const char* file, int line) noexcept {
  std::fprintf(stderr, "rcb: invariant violated: (%s) at %s:%d\n", expression, file, line);
  std::fflush(stderr);
  std::abort();
}

}  // namespace detail
}  // namespace rcb

#define RCB_ASSERT(expression)                                          \
  ((expression) ? static_cast<void>(0)                                  \
                : ::rcb::detail::AssertFailure(#expression, __FILE__, __LINE__))

#endif  // RCB_ASSERT_HPP
