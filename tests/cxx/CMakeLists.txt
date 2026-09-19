add_executable(wiiuport_tests
  main.cpp
  frame_tests.cpp
  transform_tests.cpp
)
target_link_libraries(wiiuport_tests PRIVATE wiiuport::wiiuport)
