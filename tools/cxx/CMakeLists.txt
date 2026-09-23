# Maintainer tools over the first-party library: run against data kept from
# the title, never part of the product or its gate.
add_executable(wiiuport_plan_replay plan_replay.cpp)
target_link_libraries(wiiuport_plan_replay PRIVATE wiiuport::wiiuport)
