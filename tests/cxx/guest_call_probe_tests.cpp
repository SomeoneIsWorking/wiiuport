// The register a probe's stub branches back through, chosen against the instruction it displaces.
#include "check.h"
#include "suites.h"

#include "Cafe/HW/Espresso/GuestCallProbes.h"

#include <cstdint>

namespace {

constexpr uint32_t kMoveFromLinkR0 = 0x7c0802a6;   // mflr r0
constexpr uint32_t kBlockCommitEntry = 0x7c6c1b78; // or r12,r3,r3
constexpr uint32_t kNamesR12R11R0 = 0x7d8b0378;    // or r11,r12,r0

void aLinkRegisterSaveLeavesR12ToTheStub() {
    check::equal(GuestCallProbes::ScratchRegister(kMoveFromLinkR0).value_or(99), uint32_t{12},
                 "mflr r0 names no r12");
}

void anEntryThatSetsR12GetsAnotherRegister() {
    const auto scratch = GuestCallProbes::ScratchRegister(kBlockCommitEntry);
    check::isTrue(scratch.has_value() && *scratch != 12 && *scratch != 3,
                  "the g3d block commit's or r12,r3,r3 keeps the r12 it sets");
}

void anEntryNamingEveryCandidateHasNone() {
    check::isTrue(!GuestCallProbes::ScratchRegister(kNamesR12R11R0).has_value(),
                  "an instruction naming r12, r11 and r0 leaves no scratch register");
}

} // namespace

void wiiuport::tests::runGuestCallProbeTests() {
    aLinkRegisterSaveLeavesR12ToTheStub();
    anEntryThatSetsR12GetsAnotherRegister();
    anEntryNamingEveryCandidateHasNone();
}
