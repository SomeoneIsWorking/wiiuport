add_executable(wiiuport_tests
  main.cpp
  control_tests.cpp
  frame_tests.cpp
  replay_tests.cpp
  search_tests.cpp
  input_tests.cpp
  capture_tests.cpp
  present_tests.cpp
  transform_tests.cpp
  selection_tests.cpp
  substitution_tests.cpp
  continuous_tests.cpp
  object_blend_tests.cpp
  draw_tree_tests.cpp
  vertex_blend_tests.cpp
  # The shipping implementation, compiled into the tests rather than
  # reimplemented beside them. It is free of the emulated system on purpose,
  # so what the player's remembered title does can be checked without one.
  ../../src/wiiuport/shell/TitleSelection.cpp
)
target_link_libraries(wiiuport_tests PRIVATE wiiuport::wiiuport)
