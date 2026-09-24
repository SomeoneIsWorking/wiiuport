#pragma once

#include <cstdint>

// The owning declaration for every test suite. main.cpp includes this instead
// of forward-declaring the suites itself, so adding a suite is one edit and a
// renamed suite fails to compile rather than silently not running.
namespace wiiuport::tests {

void runTransformTests();
void runFrameTests();
void runControlTests();
void runReplayTests();
void runSearchTests();
void runInputTests();
void runCaptureTests();
void runPresentTests();
void runSelectionTests();
void runProductTests();
void runSubstitutionTests();
void runContinuousTests();
void runObjectBlendTests();
void runObjectBlendSharedValueTests();
// The random frames are drawn from `seed`: fixed by default, so a failure
// is the same failure on every run, and chosen with --seed to look further.
void runDrawTreeTests(uint32_t seed);
void runVertexBlendTests();
void runSharedVertexReadsTests();
void runSlotPoolTests();
void runGateTests();
void runShadowTests();
void runMapPassTests();

} // namespace wiiuport::tests
