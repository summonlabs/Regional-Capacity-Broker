# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

add_executable(rcb rcb.cpp)
target_link_libraries(rcb PRIVATE regional_capacity_broker)
set_target_properties(rcb PROPERTIES OUTPUT_NAME rcb)

install(TARGETS rcb RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
