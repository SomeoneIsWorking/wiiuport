add_executable(wiiuport_tests
  main.cpp
  control_tests.cpp
  frame_tests.cpp
  replay_tests.cpp
  search_tests.cpp
  input_tests.cpp
  transform_tests.cpp
)
target_link_libraries(wiiuport_tests PRIVATE wiiuport::wiiuport)
