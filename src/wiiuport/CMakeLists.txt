# The first-party library the fork links when WIIUPORT_SOURCE_DIR is passed.
# It owns recording, blending and policy; the fork owns only the hooks.

# The hook interface has one owner, in the fork, and is included from there
# rather than copied here. It is deliberately free of Latte headers, so this
# costs one include directory and nothing else. The same path serves both
# builds: standalone, and as a subdirectory of the fork's own tree.
set(WIIUPORT_FORK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/cemu/src"
    CACHE PATH "Where the fork's hook interface header is found")
if(NOT EXISTS "${WIIUPORT_FORK_INCLUDE_DIR}/Cafe/HW/Latte/Core/LatteFrameHooks.h")
  message(FATAL_ERROR
    "The fork's hook interface is not at ${WIIUPORT_FORK_INCLUDE_DIR}. The "
    "first-party library implements that interface and cannot be built "
    "without it. Check out the submodule: git submodule update --init "
    "--depth 1 external/cemu")
endif()

add_library(wiiuport STATIC
  frame/FrameRecording.cpp
  frame/FrameRecording.h
  frame/RecordingObserver.cpp
  frame/RecordingObserver.h
  interp/Transform3x4.cpp
  interp/Transform3x4.h
)

target_include_directories(wiiuport PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${WIIUPORT_FORK_INCLUDE_DIR}"
)
target_compile_features(wiiuport PUBLIC cxx_std_20)
add_library(wiiuport::wiiuport ALIAS wiiuport)
