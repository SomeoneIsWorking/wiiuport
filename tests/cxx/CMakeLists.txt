add_executable(wiiuport_tests
  main.cpp
  control_tests.cpp
  logic_gate_tests.cpp
  json_body_tests.cpp
  draw_interpolation_tests.cpp
  environment_interpolation_tests.cpp
  particle_interpolation_tests.cpp
  sea_interpolation_tests.cpp
  guest_call_probe_tests.cpp
  paint_tests.cpp
  frame_tests.cpp
  input_tests.cpp
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
