# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Every suite is a separate executable registered with CTest. There are no test
# timeouts anywhere: a test that hangs is a defect to diagnose, not to kill.

function(rcb_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE regional_capacity_broker)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE RCB_SUITE_NAME="${name}")
  add_test(NAME ${name} COMMAND ${name})
  set_tests_properties(${name} PROPERTIES LABELS "unit")
endfunction()

rcb_add_test(rcb_test_foundation test_foundation.cpp)
rcb_add_test(rcb_test_property test_property.cpp)
rcb_add_test(rcb_test_adversarial test_adversarial.cpp)
rcb_add_test(rcb_test_broker test_broker.cpp fixtures.cpp)
rcb_add_test(rcb_test_persistence test_persistence.cpp fixtures.cpp)
rcb_add_test(rcb_test_concurrency test_concurrency.cpp fixtures.cpp)

# Multiprocess suite: drives the real rcb executable as independent operating
# system processes. The CLI path is compiled in so that the test runs the exact
# artefact the build produced.
if(TARGET rcb)
  rcb_add_test(rcb_test_cli_multiprocess test_cli_multiprocess.cpp)
  target_compile_definitions(rcb_test_cli_multiprocess
    PRIVATE RCB_CLI_PATH="$<TARGET_FILE:rcb>")
  add_dependencies(rcb_test_cli_multiprocess rcb)
  set_tests_properties(rcb_test_cli_multiprocess PROPERTIES LABELS "multiprocess")
endif()
