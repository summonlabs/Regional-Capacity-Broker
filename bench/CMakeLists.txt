# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(rcb_bench rcb_bench.cpp)
target_link_libraries(rcb_bench PRIVATE regional_capacity_broker)
set_target_properties(rcb_bench PROPERTIES OUTPUT_NAME rcb_bench)

install(TARGETS rcb_bench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
