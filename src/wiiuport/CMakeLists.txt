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

# lucent is the logger and the HTTP control channel. It is consumed at a
# pinned revision, never copied in. Added here rather than by the root project
# so the fork's build gets it too, and guarded so a parent that already has it
# is not clobbered.
set(WIIUPORT_LUCENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/lucent"
    CACHE PATH "The lucent checkout this library links")
if(NOT TARGET lucent::lucent)
  if(NOT EXISTS "${WIIUPORT_LUCENT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
      "lucent is not at ${WIIUPORT_LUCENT_DIR}. It owns this library's logging "
      "and control channel and is not vendored. Check out the submodule: "
      "git submodule update --init --depth 1 external/lucent")
  endif()
  add_subdirectory("${WIIUPORT_LUCENT_DIR}" lucent EXCLUDE_FROM_ALL)
endif()

add_library(wiiuport STATIC
  Runtime.cpp
  Runtime.h
  control/ControlChannel.cpp
  control/ControlChannel.h
  frame/FrameCapture.cpp
  frame/FrameCapture.h
  frame/FramePresenter.cpp
  frame/FramePresenter.h
  frame/FrameRecording.cpp
  frame/FrameRecording.h
  frame/FrameReplayer.cpp
  frame/FrameReplayer.h
  frame/RecordingObserver.cpp
  frame/RecordingObserver.h
  frame/ReplayScheduler.cpp
  frame/ReplayScheduler.h
  frame/SearchFeed.cpp
  frame/SearchFeed.h
  input/InputDriver.cpp
  input/InputDriver.h
  interp/Transform3x4.cpp
  interp/Transform3x4.h
  interp/TransformSearch.cpp
  interp/TransformSearch.h
)

target_include_directories(wiiuport PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${WIIUPORT_FORK_INCLUDE_DIR}"
)
target_link_libraries(wiiuport PUBLIC lucent::lucent)
target_compile_features(wiiuport PUBLIC cxx_std_20)
add_library(wiiuport::wiiuport ALIAS wiiuport)
