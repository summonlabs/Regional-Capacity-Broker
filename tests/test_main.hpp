// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.
//
// Each suite is one translation unit: it includes test_framework.hpp for the
// harness and this header for main(). The suite name comes from the build.

#ifndef RCB_TEST_MAIN_HPP
#define RCB_TEST_MAIN_HPP

#include "test_framework.hpp"

#ifndef RCB_SUITE_NAME
#define RCB_SUITE_NAME "rcb_test"
#endif

int main() { return ::rcbtest::RunAll(RCB_SUITE_NAME); }

#endif  // RCB_TEST_MAIN_HPP
