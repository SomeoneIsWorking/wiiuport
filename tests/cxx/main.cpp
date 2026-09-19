#include "check.h"
#include "suites.h"

#include <cstdio>

int main() {
    wiiuport::tests::runTransformTests();
    wiiuport::tests::runFrameTests();
    wiiuport::tests::runControlTests();
    std::printf("%d checks, %d failures\n", check::g_checks, check::g_failures);
    if (check::g_checks == 0) {
        std::printf("no checks ran, which is a failure and not a pass\n");
        return 2;
    }
    return check::g_failures == 0 ? 0 : 1;
}
