# The first-party library the fork links when WIIUPORT_SOURCE_DIR is passed.
# It owns recording, blending and policy; the fork owns only the hooks.
add_library(wiiuport STATIC
  frame/FrameRecording.cpp
  frame/FrameRecording.h
  interp/Transform3x4.cpp
  interp/Transform3x4.h
)

target_include_directories(wiiuport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(wiiuport PUBLIC cxx_std_20)
add_library(wiiuport::wiiuport ALIAS wiiuport)
