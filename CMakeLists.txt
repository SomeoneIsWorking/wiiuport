# First-party C++ only: the library the fork links, and its tests.
#
# The runtime itself is built from external/cemu, which adds src/wiiuport as a
# subdirectory when WIIUPORT_SOURCE_DIR is passed. This project exists so the
# library and its tests can be built and run on their own, without configuring
# or compiling the whole emulator to check a blend.
cmake_minimum_required(VERSION 3.20)
project(wiiuport CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The C++ ownership gate parses these translation units with the same flags
# the compiler used, so the database is part of the build, not a side task.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(src/wiiuport)

if(WIIUPORT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests/cxx)
  add_test(NAME wiiuport_tests COMMAND wiiuport_tests)
endif()
