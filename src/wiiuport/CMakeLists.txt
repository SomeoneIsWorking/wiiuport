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
  frame/FrameGate.cpp
  frame/FrameGate.h
  frame/FramePresenter.cpp
  frame/FramePresenter.h
  frame/GuestMemorySnapshot.cpp
  frame/GuestMemorySnapshot.h
  frame/GuestStateGuard.cpp
  frame/GuestStateGuard.h
  frame/FrameRecording.cpp
  frame/FrameRecording.h
  frame/FrameShapeLog.cpp
  frame/FrameShapeLog.h
  frame/FrameReplayer.cpp
  frame/FrameReplayer.h
  frame/PresentPacing.cpp
  frame/PresentPacing.h
  frame/RecordingObserver.cpp
  frame/RecordingObserver.h
  frame/RecordingSnapshot.cpp
  frame/RecordingSnapshot.h
  frame/ReplayScheduler.cpp
  frame/ReplayScheduler.h
  frame/SearchFeed.cpp
  frame/SearchFeed.h
  frame/VertexChanges.cpp
  frame/VertexChanges.h
    input/InputDriver.cpp
  input/InputDriver.h
  interp/ContinuousInterpolator.cpp
  interp/ShadowCheck.cpp
  interp/ShadowCheck.h
  interp/ContinuousInterpolator.h
  interp/AssemblyKey.cpp
  interp/AssemblyKey.h
  interp/Blendable.h
  interp/CutDetector.cpp
  interp/CutDetector.h
  interp/DrawTree.cpp
  interp/DrawTree.h
  interp/KeyedFrame.cpp
  interp/KeyedFrame.h
  interp/ObjectBlend.cpp
  interp/ObjectBlend.h
  interp/VertexBlend.cpp
  interp/VertexBlend.h
  interp/ObjectCensus.cpp
  interp/ObjectCensus.h
  interp/NeighbourCheck.cpp
  interp/RestoreCheck.cpp
  interp/SharedTransforms.cpp
  interp/MapPassValues.cpp
  interp/MapPassValues.h
  interp/SharedValues.cpp
  interp/SharedValues.h
  interp/SlotPool.cpp
  interp/SlotPool.h
  interp/RestoreCheck.h
  interp/Midpoint.h
  interp/ObjectPlanner.cpp
  interp/ObjectPlanner.h
  interp/PlanReplay.cpp
  interp/PlanReplay.h
  interp/ReplayBlend.h
  interp/Transform3x4.cpp
  interp/Transform3x4.h
  interp/TransformSearch.cpp
  interp/TransformSearch.h
  interp/FrameInterpolator.cpp
  interp/FrameInterpolator.h
  interp/TransformSubstitution.cpp
  interp/TransformSubstitution.h
  interp/ViewTracker.cpp
  interp/ViewTracker.h
)

target_include_directories(wiiuport PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${WIIUPORT_FORK_INCLUDE_DIR}"
)
target_link_libraries(wiiuport PUBLIC lucent::lucent)
target_compile_features(wiiuport PUBLIC cxx_std_20)
add_library(wiiuport::wiiuport ALIAS wiiuport)

# The product's own shell: the executable a player runs.
#
# It replaces Cemu's front end rather than sitting beside it, so it is built
# only inside the fork's tree, where the emulated system's targets exist. A
# standalone build of this directory still produces the library and its tests.
if(TARGET CemuCafe)
  find_package(SDL3 REQUIRED CONFIG)

  # setup-ui is the shared first-run setup screen: RmlUi markup drawn into
  # the product's own SDL3 window. It is consumed, never vendored, and it
  # resolves RmlUi from a checkout beside itself -- which is why RmlUi is a
  # submodule here rather than something setup-ui carries.
  set(WIIUPORT_SETUP_UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/setup-ui"
      CACHE PATH "The setup-ui checkout the host's first-run screen uses")
  if(NOT TARGET setup_ui::setup_ui)
    if(NOT EXISTS "${WIIUPORT_SETUP_UI_DIR}/CMakeLists.txt")
      message(FATAL_ERROR
        "setup-ui is not at ${WIIUPORT_SETUP_UI_DIR}. It owns the first-run "
        "screen this host shows when no title is configured, and is not "
        "vendored. Check out the submodules: git submodule update --init "
        "--depth 1 external/setup-ui external/RmlUi")
    endif()
    set(SETUP_UI_RMLUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../external/RmlUi"
        CACHE PATH "The RmlUi checkout setup-ui renders through")
    if(NOT EXISTS "${SETUP_UI_RMLUI_DIR}/CMakeLists.txt")
      message(FATAL_ERROR
        "RmlUi is not at ${SETUP_UI_RMLUI_DIR}, and setup-ui cannot render "
        "without it. Check out the submodule: git submodule update --init "
        "--depth 1 external/RmlUi")
    endif()
    add_subdirectory("${WIIUPORT_SETUP_UI_DIR}" setup_ui EXCLUDE_FROM_ALL)
  endif()

  # The host a product is built on: everything but an entry point. A title
  # project's main is one call to shell::runProduct with what it fixes (its
  # name and title), and it links this; it never copies the entry point.
  add_library(wiiuport_host STATIC
    shell/ControllerAutoMap.cpp
    shell/ControllerAutoMap.h
    shell/HostPaths.cpp
    shell/HostPaths.h
    shell/ProductCommandLine.cpp
    shell/ProductCommandLine.h
    shell/RunProduct.cpp
    shell/RunProduct.h
    shell/SetupScreen.cpp
    shell/SetupScreen.h
    shell/ShellHost.cpp
    shell/ShellHost.h
    shell/ShellWindow.cpp
    shell/ShellWindow.h
    shell/TitleIdentity.cpp
    shell/TitleIdentity.h
    shell/TitleSelection.cpp
    shell/TitleSelection.h
    shell/WindowSystemBridge.cpp
  )
  target_link_libraries(wiiuport_host
    PUBLIC
      wiiuport::wiiuport
    PRIVATE
      CemuAudio
      CemuBoot
      CemuCafe
      CemuCommon
      CemuComponents
      CemuConfig
      CemuGui
      CemuInput
      CemuUtil
      SDL3::SDL3
      setup_ui::setup_ui
  )
  target_include_directories(wiiuport_host PRIVATE "${WIIUPORT_FORK_INCLUDE_DIR}")
  # Cemu's headers are written against its precompiled header, which is where
  # its integer aliases and the standard library it assumes come from. A
  # target that includes them uses the same one its own targets do.
  cemu_use_precompiled_header(wiiuport_host)
  add_library(wiiuport::host ALIAS wiiuport_host)

  # The maintainer's runtime: the product that fixes no title.
  add_executable(wiiuport_shell shell/Main.cpp)
  target_link_libraries(wiiuport_shell PRIVATE wiiuport::host)
  set_target_properties(wiiuport_shell PROPERTIES
    # Beside the fork's data directories (resources/, gameProfiles/), because
    # the core resolves those relative to the running executable.
    RUNTIME_OUTPUT_DIRECTORY "${WIIUPORT_FORK_INCLUDE_DIR}/../bin/$<1:>"
    OUTPUT_NAME "wiiuport"
  )

  # A product built the way a title project builds one, through the host's
  # interface alone. It is compiled and linked with every runtime build so a
  # change that narrows or breaks that interface fails here, not in a title.
  add_executable(wiiuport_fixed_title_product ../../tests/consumer/FixedTitleProduct.cpp)
  target_link_libraries(wiiuport_fixed_title_product PRIVATE wiiuport::host)
endif()
