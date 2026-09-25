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
  product_tests.cpp
  substitution_tests.cpp
  continuous_tests.cpp
  object_blend_fixture.cpp
  object_blend_shared_value_tests.cpp
  object_blend_tests.cpp
  draw_tree_tests.cpp
  vertex_blend_fixture.cpp
  vertex_blend_tests.cpp
  shared_vertex_reads_tests.cpp
  slot_pool_tests.cpp
  gate_tests.cpp
  map_pass_tests.cpp
  shadow_tests.cpp
  light_look_up_tests.cpp
  particles_tests.cpp
  # The shipping implementation, compiled into the tests rather than
  # reimplemented beside them. It is free of the emulated system on purpose,
  # so what the player's remembered title does can be checked without one.
  ../../src/wiiuport/shell/TitleIdentity.cpp
  ../../src/wiiuport/shell/TitleSelection.cpp
  ../../src/wiiuport/shell/ProductCommandLine.cpp
)
target_link_libraries(wiiuport_tests PRIVATE wiiuport::wiiuport)
