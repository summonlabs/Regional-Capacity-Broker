# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Regional Capacity Broker -- whole-tree packaging proof.
#
# Run it from the repository root:
#
#   cmake -DSOURCE_DIR=. -DWORK_DIR=../rcb-package-check -P cmake/PackageCheck.cmake
#
# What it proves, in order:
#
#   1. the repository configures, builds and installs into a pristine scratch
#      directory; the test and tool components are switched off so that the
#      install set is exercised on its own (the component switches are passed
#      through, see below, and tests are always off);
#   2. examples/downstream -- a separate CMake project that shares no state with
#      this build -- configures against the installed prefix through
#      CMAKE_PREFIX_PATH alone, with the CMake package registries disabled, so it
#      cannot accidentally resolve the build tree or another copy of the package;
#   3. that consumer builds, runs, and prints its documented marker line
#      "RCB-DOWNSTREAM-OK";
#   4. the installed tree really contains the public headers, the exported CMake
#      package configuration and the library artifact.
#
# Any problem fails the script with a non-zero exit status: every step is run
# with execute_process(... RESULT_VARIABLE ...) and a failure is reported with a
# FAIL summary followed by message(FATAL_ERROR ...).
#
# THERE ARE NO TIMEOUTS IN THIS SCRIPT, and none may ever be added: no TIMEOUT on
# execute_process(), no watchdog, no shell timeout wrapper, and no job-level
# timeout-minutes in CI around it. A configure, build, install or consumer that
# hangs is a defect to diagnose from the output, not a step to kill.
#
# Optional cache variables. Every one of them has a default, so the invocation
# above needs nothing else:
#
#   CMAKE_BUILD_TYPE                            Release
#   CMAKE_GENERATOR                             the host default
#   CMAKE_CXX_COMPILER                          the host default
#   REGIONAL_CAPACITY_BROKER_BUILD_TOOLS        OFF
#   REGIONAL_CAPACITY_BROKER_BUILD_BENCH        OFF
#   REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES     OFF
#   REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS ON
#   RCB_PACKAGE_CHECK_VERBOSE                   OFF (echo every step's output)
#
# The in-tree examples are off by default because they are built against the
# build tree and are therefore not part of the install proof; the independent
# consumer below is the proof. Pass -DREGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES=ON
# to build them here as well.

cmake_minimum_required(VERSION 3.20)

# ---------------------------------------------------------------- arguments --

# Windows PowerShell 5.1 splits an unquoted assignment whose value starts with a
# dot -- "cmake -DSOURCE_DIR=. ..." reaches cmake as two arguments ("-DSOURCE_DIR="
# and ".") -- which would leave SOURCE_DIR and WORK_DIR empty. Recover the value
# from the raw command line so that the documented invocation works in every
# shell. Quoting the assignment ("-DSOURCE_DIR=.") avoids the split entirely and
# is never wrong.
function(rcb_recover_split_definition name out)
  set(${out} "" PARENT_SCOPE)
  if(NOT DEFINED CMAKE_ARGC)
    return()
  endif()
  math(EXPR _rcb_last "${CMAKE_ARGC} - 1")
  foreach(_rcb_index RANGE 0 ${_rcb_last})
    if(_rcb_index GREATER 0)
      math(EXPR _rcb_previous "${_rcb_index} - 1")
      if("${CMAKE_ARGV${_rcb_previous}}" STREQUAL "-D${name}=")
        set(${out} "${CMAKE_ARGV${_rcb_index}}" PARENT_SCOPE)
      endif()
    endif()
  endforeach()
endfunction()

foreach(_rcb_argument SOURCE_DIR WORK_DIR)
  if(NOT DEFINED ${_rcb_argument} OR "${${_rcb_argument}}" STREQUAL "")
    rcb_recover_split_definition(${_rcb_argument} _rcb_recovered)
    if(NOT _rcb_recovered STREQUAL "")
      set(${_rcb_argument} "${_rcb_recovered}")
      message(NOTICE
        "[PackageCheck] recovered ${_rcb_argument}=${_rcb_recovered} from the raw command line; "
        "quote the assignment (\"-D${_rcb_argument}=...\") to avoid this notice")
    endif()
  endif()
endforeach()

# Last resort for the source tree: this script lives in <repo>/cmake, so the
# repository root is derivable even when the caller passed nothing.
if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
  get_filename_component(_rcb_implied_source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  if(EXISTS "${_rcb_implied_source}/CMakeLists.txt")
    set(SOURCE_DIR "${_rcb_implied_source}")
    message(NOTICE "[PackageCheck] no -DSOURCE_DIR given; using the repository this script lives in: ${SOURCE_DIR}")
  endif()
endif()

if(NOT DEFINED SOURCE_DIR OR SOURCE_DIR STREQUAL "")
  message(FATAL_ERROR
    "[PackageCheck] FAIL: pass -DSOURCE_DIR=<repository root> "
    "(quote it if your shell splits it: \"-DSOURCE_DIR=.\")")
endif()
if(NOT DEFINED WORK_DIR OR WORK_DIR STREQUAL "")
  message(FATAL_ERROR
    "[PackageCheck] FAIL: pass -DWORK_DIR=<fresh scratch directory>, for example "
    "-DWORK_DIR=../rcb-package-check (quote it if your shell splits it: \"-DWORK_DIR=..\")")
endif()

# Relative paths are resolved against the directory the script is run from, so
# that the documented invocation behaves the way it reads.
if(WIN32)
  string(REPLACE "\\" "/" SOURCE_DIR "${SOURCE_DIR}")
  string(REPLACE "\\" "/" WORK_DIR "${WORK_DIR}")
endif()
get_filename_component(RCB_SOURCE_DIR "${SOURCE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")
get_filename_component(RCB_WORK_DIR "${WORK_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT EXISTS "${RCB_SOURCE_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "[PackageCheck] FAIL: ${RCB_SOURCE_DIR} is not the repository root (no CMakeLists.txt)")
endif()
if(NOT EXISTS "${RCB_SOURCE_DIR}/examples/downstream/CMakeLists.txt")
  message(FATAL_ERROR
    "[PackageCheck] FAIL: ${RCB_SOURCE_DIR}/examples/downstream is missing; "
    "the packaging proof needs the independent downstream consumer")
endif()

# Guard rails around file(REMOVE_RECURSE): this script wipes WORK_DIR.
if(RCB_WORK_DIR STREQUAL "/" OR RCB_WORK_DIR MATCHES "^[A-Za-z]:/?$")
  message(FATAL_ERROR "[PackageCheck] FAIL: WORK_DIR is a filesystem root (${RCB_WORK_DIR})")
endif()
string(TOLOWER "${RCB_SOURCE_DIR}" _rcb_source_lower)
string(TOLOWER "${RCB_WORK_DIR}" _rcb_work_lower)
if(_rcb_work_lower STREQUAL _rcb_source_lower)
  message(FATAL_ERROR
    "[PackageCheck] FAIL: WORK_DIR and SOURCE_DIR are the same directory; "
    "refusing to wipe the source tree")
endif()
cmake_path(IS_PREFIX _rcb_work_lower "${_rcb_source_lower}" NORMALIZE _rcb_source_inside_work)
if(_rcb_source_inside_work)
  message(FATAL_ERROR
    "[PackageCheck] FAIL: WORK_DIR (${RCB_WORK_DIR}) contains the source tree; "
    "refusing to wipe it")
endif()
cmake_path(IS_PREFIX _rcb_source_lower "${_rcb_work_lower}" NORMALIZE _rcb_work_inside_source)
if(_rcb_work_inside_source)
  message(NOTICE
    "[PackageCheck] WORK_DIR is inside the source tree and will be wiped: ${RCB_WORK_DIR}")
endif()

# ------------------------------------------------------------------ defaults --

if(NOT DEFINED CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "")
  set(RCB_BUILD_TYPE "Release")
else()
  set(RCB_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
endif()

foreach(_rcb_option
    REGIONAL_CAPACITY_BROKER_BUILD_TOOLS
    REGIONAL_CAPACITY_BROKER_BUILD_BENCH
    REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES)
  if(NOT DEFINED ${_rcb_option} OR "${${_rcb_option}}" STREQUAL "")
    set(${_rcb_option} OFF)
  endif()
endforeach()
if(NOT DEFINED REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS
   OR REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS STREQUAL "")
  set(REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS ON)
endif()
if(NOT DEFINED RCB_PACKAGE_CHECK_VERBOSE OR RCB_PACKAGE_CHECK_VERBOSE STREQUAL "")
  set(RCB_PACKAGE_CHECK_VERBOSE OFF)
endif()

set(RCB_PREFIX_DIR "${RCB_WORK_DIR}/prefix")
set(RCB_BUILD_DIR "${RCB_WORK_DIR}/build")
set(RCB_CONSUMER_BUILD_DIR "${RCB_WORK_DIR}/consumer-build")
set(RCB_MARKER "RCB-DOWNSTREAM-OK")

# ------------------------------------------------------------- step running --

# Runs one external command. Its output is captured and shown when the step
# fails or when RCB_PACKAGE_CHECK_VERBOSE is ON, so a CI log carries the whole
# story. There is no TIMEOUT argument here and there must never be one.
function(rcb_run what)
  execute_process(
    COMMAND ${ARGN}
    RESULT_VARIABLE rcb_result
    OUTPUT_VARIABLE rcb_stdout
    ERROR_VARIABLE rcb_stderr)
  set(RCB_STEP_STDOUT "${rcb_stdout}" PARENT_SCOPE)
  set(RCB_STEP_STDERR "${rcb_stderr}" PARENT_SCOPE)
  if(RCB_PACKAGE_CHECK_VERBOSE)
    if(NOT rcb_stdout STREQUAL "")
      message(STATUS "${rcb_stdout}")
    endif()
    if(NOT rcb_stderr STREQUAL "")
      message(STATUS "${rcb_stderr}")
    endif()
  endif()
  if(NOT rcb_result STREQUAL "0")
    message(STATUS "")
    message(STATUS "[PackageCheck] FAIL: ${what}")
    message(STATUS "[PackageCheck] command: ${ARGN}")
    message(STATUS "[PackageCheck] exit status: ${rcb_result}")
    if(NOT rcb_stdout STREQUAL "")
      message(STATUS "[PackageCheck] ---- stdout ----\n${rcb_stdout}[PackageCheck] ---- end stdout ----")
    endif()
    if(NOT rcb_stderr STREQUAL "")
      message(STATUS "[PackageCheck] ---- stderr ----\n${rcb_stderr}[PackageCheck] ---- end stderr ----")
    endif()
    message(FATAL_ERROR
      "[PackageCheck] FAIL: ${what} (exit status ${rcb_result}). The packaging proof is incomplete.")
  endif()
  message(STATUS "[PackageCheck] ok: ${what}")
endfunction()

message(STATUS "[PackageCheck] source tree   : ${RCB_SOURCE_DIR}")
message(STATUS "[PackageCheck] work dir      : ${RCB_WORK_DIR}")
message(STATUS "[PackageCheck] install prefix: ${RCB_PREFIX_DIR}")
message(STATUS "[PackageCheck] build type    : ${RCB_BUILD_TYPE}")
message(STATUS "[PackageCheck] components    : tools=${REGIONAL_CAPACITY_BROKER_BUILD_TOOLS} "
               "bench=${REGIONAL_CAPACITY_BROKER_BUILD_BENCH} "
               "examples=${REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES} tests=OFF")

file(REMOVE_RECURSE "${RCB_WORK_DIR}")
file(MAKE_DIRECTORY "${RCB_WORK_DIR}")

# ---------------------------------------------- 1. configure/build/install --

set(_rcb_configure
  "${CMAKE_COMMAND}" -S "${RCB_SOURCE_DIR}" -B "${RCB_BUILD_DIR}"
  "-DCMAKE_BUILD_TYPE=${RCB_BUILD_TYPE}"
  "-DCMAKE_INSTALL_PREFIX=${RCB_PREFIX_DIR}"
  "-DREGIONAL_CAPACITY_BROKER_BUILD_TESTS=OFF"
  "-DREGIONAL_CAPACITY_BROKER_BUILD_TOOLS=${REGIONAL_CAPACITY_BROKER_BUILD_TOOLS}"
  "-DREGIONAL_CAPACITY_BROKER_BUILD_BENCH=${REGIONAL_CAPACITY_BROKER_BUILD_BENCH}"
  "-DREGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES=${REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES}"
  "-DREGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS=${REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS}")
if(DEFINED CMAKE_GENERATOR AND NOT CMAKE_GENERATOR STREQUAL "")
  list(APPEND _rcb_configure -G "${CMAKE_GENERATOR}")
endif()
if(DEFINED CMAKE_CXX_COMPILER AND NOT CMAKE_CXX_COMPILER STREQUAL "")
  list(APPEND _rcb_configure "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
rcb_run("configure the runtime (tests off) into ${RCB_BUILD_DIR}" ${_rcb_configure})

rcb_run("build the runtime"
  "${CMAKE_COMMAND}" --build "${RCB_BUILD_DIR}" --config "${RCB_BUILD_TYPE}" --parallel)

rcb_run("install the runtime into ${RCB_PREFIX_DIR}"
  "${CMAKE_COMMAND}" --install "${RCB_BUILD_DIR}" --config "${RCB_BUILD_TYPE}")

# ------------------------------- 2. the installed tree must be complete -----

file(GLOB_RECURSE _rcb_installed_configs "${RCB_PREFIX_DIR}/*RegionalCapacityBrokerConfig.cmake")
file(GLOB _rcb_installed_libraries
  "${RCB_PREFIX_DIR}/lib/*regional_capacity_broker.*"
  "${RCB_PREFIX_DIR}/lib64/*regional_capacity_broker.*"
  "${RCB_PREFIX_DIR}/bin/*regional_capacity_broker.*")
list(LENGTH _rcb_installed_configs _rcb_config_count)
list(LENGTH _rcb_installed_libraries _rcb_library_count)
set(_rcb_missing "")
if(NOT EXISTS "${RCB_PREFIX_DIR}/include/rcb/broker.hpp")
  list(APPEND _rcb_missing "the installed public header include/rcb/broker.hpp")
endif()
if(_rcb_config_count EQUAL 0)
  list(APPEND _rcb_missing "the exported package configuration RegionalCapacityBrokerConfig.cmake")
endif()
if(_rcb_library_count EQUAL 0)
  list(APPEND _rcb_missing "the installed library artifact")
endif()
if(NOT _rcb_missing STREQUAL "")
  message(STATUS "[PackageCheck] FAIL: the installed tree under ${RCB_PREFIX_DIR} is incomplete")
  foreach(_rcb_item IN LISTS _rcb_missing)
    message(STATUS "[PackageCheck]   missing: ${_rcb_item}")
  endforeach()
  message(FATAL_ERROR "[PackageCheck] FAIL: the install step did not produce a usable package")
endif()
list(GET _rcb_installed_configs 0 _rcb_package_config)
message(STATUS "[PackageCheck] installed package configuration: ${_rcb_package_config}")

# ----------------------------------------- 3. the independent consumer ------

set(_rcb_consumer_configure
  "${CMAKE_COMMAND}" -S "${RCB_SOURCE_DIR}/examples/downstream" -B "${RCB_CONSUMER_BUILD_DIR}"
  "-DCMAKE_BUILD_TYPE=${RCB_BUILD_TYPE}"
  "-DCMAKE_PREFIX_PATH=${RCB_PREFIX_DIR}"
  "-DRCB_DOWNSTREAM_WARNINGS_AS_ERRORS=${REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS}"
  "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE"
  "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE")
if(DEFINED CMAKE_GENERATOR AND NOT CMAKE_GENERATOR STREQUAL "")
  list(APPEND _rcb_consumer_configure -G "${CMAKE_GENERATOR}")
endif()
if(DEFINED CMAKE_CXX_COMPILER AND NOT CMAKE_CXX_COMPILER STREQUAL "")
  list(APPEND _rcb_consumer_configure "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
endif()
rcb_run("configure the downstream consumer against the installed prefix only"
  ${_rcb_consumer_configure})
set(_rcb_consumer_configure_output "${RCB_STEP_STDOUT}")

# The consumer reports the package directory it resolved. It must live inside the
# installed prefix: that is what makes this a packaging proof rather than a
# build-tree accident.
string(REGEX MATCH "RCB-DOWNSTREAM-PACKAGE-DIR=([^\r\n]*)" _rcb_package_dir_match
  "${_rcb_consumer_configure_output}")
if(NOT _rcb_package_dir_match)
  message(STATUS "[PackageCheck] FAIL: the consumer did not report the package directory it used")
  message(STATUS "[PackageCheck] consumer configure output:\n${_rcb_consumer_configure_output}")
  message(FATAL_ERROR "[PackageCheck] FAIL: no RegionalCapacityBroker_DIR in the consumer configure output")
endif()
set(_rcb_consumer_package_dir "${CMAKE_MATCH_1}")
string(STRIP "${_rcb_consumer_package_dir}" _rcb_consumer_package_dir)
cmake_path(IS_PREFIX RCB_PREFIX_DIR "${_rcb_consumer_package_dir}" NORMALIZE _rcb_from_prefix)
if(NOT _rcb_from_prefix)
  message(STATUS "[PackageCheck] FAIL: the consumer resolved RegionalCapacityBroker from")
  message(STATUS "[PackageCheck]   ${_rcb_consumer_package_dir}")
  message(STATUS "[PackageCheck] which is outside the installed prefix ${RCB_PREFIX_DIR}")
  message(FATAL_ERROR "[PackageCheck] FAIL: the consumer did not use the installed package")
endif()
message(STATUS "[PackageCheck] consumer resolved the package from ${_rcb_consumer_package_dir}")

rcb_run("build the downstream consumer"
  "${CMAKE_COMMAND}" --build "${RCB_CONSUMER_BUILD_DIR}" --config "${RCB_BUILD_TYPE}" --parallel)

set(_rcb_exe_suffix "")
if(WIN32)
  set(_rcb_exe_suffix ".exe")
endif()
set(_rcb_consumer_exe "")
foreach(_rcb_candidate
    "${RCB_CONSUMER_BUILD_DIR}/rcb_downstream_consumer${_rcb_exe_suffix}"
    "${RCB_CONSUMER_BUILD_DIR}/${RCB_BUILD_TYPE}/rcb_downstream_consumer${_rcb_exe_suffix}")
  if(EXISTS "${_rcb_candidate}")
    set(_rcb_consumer_exe "${_rcb_candidate}")
    break()
  endif()
endforeach()
if(_rcb_consumer_exe STREQUAL "")
  file(GLOB_RECURSE _rcb_consumer_exes
    "${RCB_CONSUMER_BUILD_DIR}/rcb_downstream_consumer${_rcb_exe_suffix}")
  list(LENGTH _rcb_consumer_exes _rcb_consumer_exe_count)
  if(_rcb_consumer_exe_count GREATER 0)
    list(GET _rcb_consumer_exes 0 _rcb_consumer_exe)
  endif()
endif()
if(_rcb_consumer_exe STREQUAL "")
  message(STATUS "[PackageCheck] FAIL: the consumer build produced no rcb_downstream_consumer executable")
  message(FATAL_ERROR "[PackageCheck] FAIL: cannot run the downstream consumer")
endif()

rcb_run("run the downstream consumer ${_rcb_consumer_exe}" "${_rcb_consumer_exe}")
set(_rcb_consumer_output "${RCB_STEP_STDOUT}")
set(_rcb_consumer_errors "${RCB_STEP_STDERR}")
message(STATUS "[PackageCheck] ---- downstream consumer output ----")
message(STATUS "${_rcb_consumer_output}[PackageCheck] ---- end consumer output ----")
if(NOT _rcb_consumer_errors STREQUAL "")
  message(STATUS "[PackageCheck] ---- downstream consumer stderr ----")
  message(STATUS "${_rcb_consumer_errors}[PackageCheck] ---- end consumer stderr ----")
endif()

if(NOT _rcb_consumer_output MATCHES "(^|\n)[ \t]*${RCB_MARKER}[ \t]*(\r?\n|$)")
  message(STATUS "[PackageCheck] FAIL: the consumer output does not contain the marker line ${RCB_MARKER}")
  message(STATUS "[PackageCheck] the consumer must print it as its last line on success")
  message(FATAL_ERROR "[PackageCheck] FAIL: marker line ${RCB_MARKER} not found in the consumer output")
endif()

# ------------------------------------------------------------- 4. summary ---

message(STATUS "")
message(STATUS "[PackageCheck] =========================================================")
message(STATUS "[PackageCheck] PASS")
message(STATUS "[PackageCheck]   source tree     : ${RCB_SOURCE_DIR}")
message(STATUS "[PackageCheck]   work dir        : ${RCB_WORK_DIR}")
message(STATUS "[PackageCheck]   installed prefix: ${RCB_PREFIX_DIR}")
message(STATUS "[PackageCheck]   package config  : ${_rcb_package_config}")
message(STATUS "[PackageCheck]   consumer        : ${_rcb_consumer_exe}")
message(STATUS "[PackageCheck]   marker line     : ${RCB_MARKER}")
message(STATUS "[PackageCheck] =========================================================")
