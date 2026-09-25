#include "check.h"
#include "suites.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

namespace {

// The draw tree's frames by default; any other seed is a run that looks
// somewhere the default does not.
constexpr uint32_t kDefaultSeed = 0x5e7a11;

// `--seed N` or nothing. Anything else refuses, so a mistyped seed is not a
// run of the default that looks like the one asked for.
bool readSeed(std::span<char*> arguments, uint32_t& seed) {
    if (arguments.empty()) {
        return true;
    }
    if (arguments.size() != 2 || std::string_view(arguments[0]) != "--seed") {
        return false;
    }
    std::string_view text(arguments[1]);
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed);
    return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
    uint32_t seed = kDefaultSeed;
    if (!readSeed(std::span<char*>(argv, static_cast<size_t>(argc)).subspan(1), seed)) {
        std::printf("usage: wiiuport_tests [--seed N]\n");
        return 2;
    }
    wiiuport::tests::runTransformTests();
    wiiuport::tests::runFrameTests();
    wiiuport::tests::runControlTests();
    wiiuport::tests::runReplayTests();
    wiiuport::tests::runSearchTests();
    wiiuport::tests::runInputTests();
    wiiuport::tests::runCaptureTests();
    wiiuport::tests::runPresentTests();
    wiiuport::tests::runSelectionTests();
    wiiuport::tests::runProductTests();
    wiiuport::tests::runSubstitutionTests();
    wiiuport::tests::runContinuousTests();
    wiiuport::tests::runObjectBlendTests();
    wiiuport::tests::runObjectBlendSharedValueTests();
    wiiuport::tests::runVertexBlendTests();
    wiiuport::tests::runSharedVertexReadsTests();
    wiiuport::tests::runSlotPoolTests();
    wiiuport::tests::runDrawTreeTests(seed);
    wiiuport::tests::runGateTests();
    wiiuport::tests::runShadowTests();
    wiiuport::tests::runMapPassTests();
    wiiuport::tests::runLightLookUpTests();
    std::printf("draw tree seed %u\n", seed);
    std::printf("%d checks, %d failures\n", check::g_checks, check::g_failures);
    if (check::g_checks == 0) {
        std::printf("no checks ran, which is a failure and not a pass\n");
        return 2;
    }
    return check::g_failures == 0 ? 0 : 1;
}
