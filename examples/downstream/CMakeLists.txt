# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.
#
# An independent downstream consumer of the installed Regional Capacity Broker
# package.
#
# This is a separate CMake project on purpose: it is the packaging proof. It is
# configured against an installed prefix, never against the runtime's build
# tree:
#
#   cmake -S examples/downstream -B <build> -DCMAKE_PREFIX_PATH=<install prefix>
#   cmake --build <build>
#
# cmake/PackageCheck.cmake performs exactly that after installing the runtime
# into a scratch prefix, then runs the executable and requires its
# "RCB-DOWNSTREAM-OK" marker line. This project must therefore never be added
# with add_subdirectory() from the repository's own build, and it must never
# reach into the source tree for headers.
#
# There are no timeouts here: no test timeout, no watchdog, no timeout wrapper.

cmake_minimum_required(VERSION 3.20)

# Refuse to be configured as part of the repository's own build. The whole point
# of this project is that it sees only the installed package; an in-tree
# configure would silently fall back to the build tree and prove nothing.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR
    "examples/downstream is a standalone project that proves the installed package. "
    "Configure it on its own: cmake -S <repo>/examples/downstream -B <build> "
    "-DCMAKE_PREFIX_PATH=<install prefix>")
endif()

project(RcbDownstreamConsumer
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of the installed RegionalCapacityBroker package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The consumer is held to the same warnings discipline as the runtime. It has no
# third-party dependency: the C++20 standard library plus whatever the imported
# target carries transitively.
option(RCB_DOWNSTREAM_WARNINGS_AS_ERRORS "Treat downstream consumer warnings as errors" ON)

if(MSVC)
  set(RCB_DOWNSTREAM_WARNINGS /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /Zc:preprocessor)
  if(RCB_DOWNSTREAM_WARNINGS_AS_ERRORS)
    list(APPEND RCB_DOWNSTREAM_WARNINGS /WX)
  endif()
else()
  set(RCB_DOWNSTREAM_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(RCB_DOWNSTREAM_WARNINGS_AS_ERRORS)
    list(APPEND RCB_DOWNSTREAM_WARNINGS -Werror)
  endif()
endif()
add_compile_options(${RCB_DOWNSTREAM_WARNINGS})

# The one line this project exists to prove: the installed config package is
# found, with no build-tree fallback.
find_package(RegionalCapacityBroker CONFIG REQUIRED)

if(NOT TARGET SummonLabs::RegionalCapacityBroker::regional_capacity_broker)
  message(FATAL_ERROR
    "the installed package was found but does not export "
    "SummonLabs::RegionalCapacityBroker::regional_capacity_broker")
endif()

add_executable(rcb_downstream_consumer consumer.cpp)
target_link_libraries(rcb_downstream_consumer
  PRIVATE SummonLabs::RegionalCapacityBroker::regional_capacity_broker)

# Evidence for the packaging proof: the package directory and the include
# directories the imported target actually resolved to. Both must live inside
# the installed prefix, never inside the runtime's build tree; PackageCheck.cmake
# checks the package directory it printed here.
message(STATUS "RCB-DOWNSTREAM-PACKAGE-DIR=${RegionalCapacityBroker_DIR}")
message(STATUS "RCB-DOWNSTREAM-PACKAGE-VERSION=${RegionalCapacityBroker_VERSION}")
get_target_property(RCB_DOWNSTREAM_INCLUDES
  SummonLabs::RegionalCapacityBroker::regional_capacity_broker INTERFACE_INCLUDE_DIRECTORIES)
message(STATUS "RCB-DOWNSTREAM-INCLUDES=${RCB_DOWNSTREAM_INCLUDES}")

foreach(rcb_include IN LISTS RCB_DOWNSTREAM_INCLUDES)
  if(NOT IS_DIRECTORY "${rcb_include}")
    message(FATAL_ERROR "the installed package points at a missing include directory: ${rcb_include}")
  endif()
  if(NOT EXISTS "${rcb_include}/rcb/broker.hpp")
    message(FATAL_ERROR "the installed include directory has no rcb/broker.hpp: ${rcb_include}")
  endif()
endforeach()
