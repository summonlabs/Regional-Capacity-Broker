# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Summon Software Labs.

# In-tree walkthrough of the public API. The installed-package proof is the
# separate downstream consumer under examples/downstream, driven by
# cmake/PackageCheck.cmake; this example is compiled against the build tree so
# that a developer can run it directly.
add_executable(rcb_walkthrough walkthrough.cpp)
target_link_libraries(rcb_walkthrough PRIVATE regional_capacity_broker)
set_target_properties(rcb_walkthrough PROPERTIES OUTPUT_NAME rcb_walkthrough)
