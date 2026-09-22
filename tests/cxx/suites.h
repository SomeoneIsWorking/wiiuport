#pragma once

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
void runSubstitutionTests();
void runContinuousTests();
void runObjectBlendTests();

} // namespace wiiuport::tests
