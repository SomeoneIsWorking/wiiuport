add_executable(wiiuport_tests
  main.cpp
  block_census_tests.cpp
  draw_attribute_census_tests.cpp
  uniform_block_ring_tests.cpp
  vertex_pose_history_tests.cpp
  control_tests.cpp
  logic_gate_tests.cpp
  pose_history_tests.cpp
  global_pose_tests.cpp
  json_body_tests.cpp
  node_pose_tests.cpp
  command_stream_identity_tests.cpp
  vertex_component_tests.cpp
  pose_blend_tests.cpp
  quad_blend_tests.cpp
  pose_by_shader_tests.cpp
  pose_locator_tests.cpp
  paint_tests.cpp
  frame_tests.cpp
  input_tests.cpp
  buffer_writers_tests.cpp
  capture_tests.cpp
  selection_tests.cpp
  product_tests.cpp
  gate_tests.cpp
  # The shipping implementation, compiled into the tests rather than
  # reimplemented beside them. It is free of the emulated system on purpose,
  # so what the player's remembered title does can be checked without one.
  ../../src/wiiuport/shell/TitleIdentity.cpp
  ../../src/wiiuport/shell/TitleSelection.cpp
  ../../src/wiiuport/shell/ProductCommandLine.cpp
)
target_link_libraries(wiiuport_tests PRIVATE wiiuport::wiiuport)
