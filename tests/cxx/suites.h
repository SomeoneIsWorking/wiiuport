#pragma once

#include <cstdint>

// The owning declaration for every test suite. main.cpp includes this instead
// of forward-declaring the suites itself, so adding a suite is one edit and a
// renamed suite fails to compile rather than silently not running.
namespace wiiuport::tests {
void runFrameTests();
void runControlTests();
void runPaintTests();
void runBlockCensusTests();
void runLogicGateTests();
void runObjectPoseHistoryTests();
void runGlobalPoseCensusTests();
void runObjectPoseLocatorTests();
void runPoseByShaderTests();
void runPoseBlendTests();
void runQuadBlendTests();
void runBufferedBlocksTests();
void runDrawInterpolationTests();
void runGuestCallProbeTests();
void runNodePoseLocatorTests();
void runJsonBodyTests();
void runCommandStreamIdentityTests();
void runVertexComponentTests();
void runDrawAttributeCensusTests();
void runVertexPoseHistoryTests();
void runUniformBlockRingTests();
void runInputTests();
void runBufferWritersTests();
void runCaptureTests();
void runSelectionTests();
void runProductTests();
// The random frames are drawn from `seed`: fixed by default, so a failure
// is the same failure on every run, and chosen with --seed to look further.
void runGateTests();

} // namespace wiiuport::tests
