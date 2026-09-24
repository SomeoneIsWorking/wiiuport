#include "check.h"
#include "suites.h"

#include <cstdio>

int main() {
    wiiuport::tests::runTransformTests();
    wiiuport::tests::runFrameTests();
    wiiuport::tests::runControlTests();
    wiiuport::tests::runReplayTests();
    wiiuport::tests::runSearchTests();
    wiiuport::tests::runInputTests();
    wiiuport::tests::runCaptureTests();
    wiiuport::tests::runPresentTests();
    wiiuport::tests::runSelectionTests();
    wiiuport::tests::runSubstitutionTests();
    wiiuport::tests::runContinuousTests();
    wiiuport::tests::runObjectBlendTests();
    wiiuport::tests::runVertexBlendTests();
    wiiuport::tests::runSlotPoolTests();
    wiiuport::tests::runDrawTreeTests();
    wiiuport::tests::runGateTests();
    std::printf("%d checks, %d failures\n", check::g_checks, check::g_failures);
    if (check::g_checks == 0) {
        std::printf("no checks ran, which is a failure and not a pass\n");
        return 2;
    }
    return check::g_failures == 0 ? 0 : 1;
}
