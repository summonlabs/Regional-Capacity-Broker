# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# Regional Capacity Broker -- build, test and install.
#
# The runtime is portable C++20 with no third-party dependency: the C++20
# standard library and, on Windows, the operating system libraries the platform
# already has (kernel32 for files and durability).

cmake_minimum_required(VERSION 3.20)

project(RegionalCapacityBroker
  VERSION 1.0.0
  DESCRIPTION "Arbitration of facility-level capacity commitments across data-center sites"
  HOMEPAGE_URL "https://github.com/summonlabs/Regional-Capacity-Broker"
  LANGUAGES CXX)

set(REGIONAL_CAPACITY_BROKER_VERSION "${PROJECT_VERSION}")

option(REGIONAL_CAPACITY_BROKER_BUILD_TOOLS "Build the command line tools" ON)
option(REGIONAL_CAPACITY_BROKER_BUILD_TESTS "Build the test suites" ON)
option(REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES "Build the examples" ON)
option(REGIONAL_CAPACITY_BROKER_BUILD_BENCH "Build the benchmarks" ON)
option(REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(REGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS "Build with AddressSanitizer/UndefinedBehaviorSanitizer where supported" OFF)
option(REGIONAL_CAPACITY_BROKER_BUILD_SHARED "Build the runtime as a shared library" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(REGIONAL_CAPACITY_BROKER_BUILD_SHARED)
  set(REGIONAL_CAPACITY_BROKER_LIBRARY_TYPE SHARED)
else()
  set(REGIONAL_CAPACITY_BROKER_LIBRARY_TYPE STATIC)
endif()

set(REGIONAL_CAPACITY_BROKER_SOURCES
  src/broker.cpp
  src/checked.cpp
  src/digest.cpp
  src/identifier.cpp
  src/journal.cpp
  src/json.cpp
  src/ledger.cpp
  src/model.cpp
  src/scaled.cpp
  src/serialize.cpp
  src/service.cpp
  src/status.cpp
  src/store.cpp
  src/units.cpp
  src/version.cpp)

# ---- warnings ------------------------------------------------------------
# First-party code is warnings-clean at this level. Nothing is suppressed
# globally: a warning here is a defect to fix, not noise to silence.
if(MSVC)
  set(REGIONAL_CAPACITY_BROKER_WARNINGS
    /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor
    /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
    /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
  if(REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS)
    list(APPEND REGIONAL_CAPACITY_BROKER_WARNINGS /WX)
  endif()
else()
  set(REGIONAL_CAPACITY_BROKER_WARNINGS
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(REGIONAL_CAPACITY_BROKER_WARNINGS_AS_ERRORS)
    list(APPEND REGIONAL_CAPACITY_BROKER_WARNINGS -Werror)
  endif()
endif()

add_compile_options(${REGIONAL_CAPACITY_BROKER_WARNINGS})

if(REGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
  else()
    # Some toolchains accept the sanitizer flags but cannot link the runtime
    # (MinGW has no libasan/libubsan). Detect that here so the operator is told
    # what is missing instead of getting a link error several minutes later.
    include(CheckCXXSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" REGIONAL_CAPACITY_BROKER_SANITIZERS_LINK)
    unset(CMAKE_REQUIRED_FLAGS)
    if(NOT REGIONAL_CAPACITY_BROKER_SANITIZERS_LINK)
      message(FATAL_ERROR
        "REGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS=ON was requested, but this toolchain "
        "cannot link the AddressSanitizer/UndefinedBehaviorSanitizer runtime. On MinGW-w64 "
        "there is no libasan/libubsan; use MSVC with /fsanitize=address, or a Linux GCC/Clang "
        "toolchain, or configure with -DREGIONAL_CAPACITY_BROKER_ENABLE_SANITIZERS=OFF.")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

add_library(regional_capacity_broker ${REGIONAL_CAPACITY_BROKER_LIBRARY_TYPE}
  ${REGIONAL_CAPACITY_BROKER_SOURCES})

add_library(SummonLabs::RegionalCapacityBroker::regional_capacity_broker
  ALIAS regional_capacity_broker)

target_include_directories(regional_capacity_broker
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(regional_capacity_broker PUBLIC cxx_std_20)

if(REGIONAL_CAPACITY_BROKER_BUILD_SHARED)
  target_compile_definitions(regional_capacity_broker
    PUBLIC REGIONAL_CAPACITY_BROKER_SHARED
    PRIVATE REGIONAL_CAPACITY_BROKER_BUILDING_LIBRARY)
endif()

find_package(Threads REQUIRED)
target_link_libraries(regional_capacity_broker PUBLIC Threads::Threads)

# ---- components ----------------------------------------------------------
if(REGIONAL_CAPACITY_BROKER_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(REGIONAL_CAPACITY_BROKER_BUILD_BENCH)
  add_subdirectory(bench)
endif()

if(REGIONAL_CAPACITY_BROKER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(REGIONAL_CAPACITY_BROKER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- installation --------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS regional_capacity_broker
  EXPORT RegionalCapacityBrokerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT RegionalCapacityBrokerTargets
  FILE RegionalCapacityBrokerTargets.cmake
  NAMESPACE SummonLabs::RegionalCapacityBroker::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RegionalCapacityBroker)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RegionalCapacityBrokerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/RegionalCapacityBrokerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RegionalCapacityBroker)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/RegionalCapacityBrokerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/RegionalCapacityBrokerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/RegionalCapacityBrokerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RegionalCapacityBroker)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/RegionalCapacityBroker)
