#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"

#include <array>
#include <cstring>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;

namespace {

void aRecordedListSurvivesItsSource() {
    // The measurement that makes this the whole point: every display-list
    // address in the captured run was written again in a later frame. A
    // recording that aliased guest storage would replay the overwrite.
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    FrameRecording recording;
    check::isTrue(recording.addDisplayList(0x10000000, guest.data(), guest.size() * 4),
                  "the list was recorded");
    guest = {9, 9, 9, 9};

    check::equal(recording.displayLists().size(), size_t{1}, "one list is held");
    std::array<uint32_t, 4> readBack{};
    std::memcpy(readBack.data(), recording.displayLists()[0].data.data(), readBack.size() * 4);
    check::equal(readBack[0], uint32_t{1}, "the recorded bytes are the ones submitted");
    check::equal(readBack[3], uint32_t{4}, "and not what the guest wrote afterwards");
    check::equal(recording.displayLists()[0].physicalAddress, uint32_t{0x10000000},
                 "the guest address is kept, because it is the only identity a list has");
}

void anEmptyListIsStillARecordedList() {
    FrameRecording recording;
    check::isTrue(recording.addDisplayList(0x20000000, nullptr, 0), "a zero-byte list is accepted");
    check::equal(recording.displayLists().size(), size_t{1},
                 "and is present, because a frame that submitted it did so for a reason");
}

void goingOverBudgetRefusesRatherThanTruncating() {
    std::array<uint32_t, 4> data{1, 2, 3, 4};
    FrameRecording recording(24);
    check::isTrue(recording.addDisplayList(1, data.data(), 16), "the first list fits");
    check::isTrue(!recording.addDisplayList(2, data.data(), 16), "the second does not");
    check::equal(recording.displayLists().size(), size_t{1}, "and nothing partial was stored");
    check::equal(recording.refusedOverBudget(), size_t{1}, "the refusal is counted");
    check::isTrue(!recording.isComplete(),
                  "so the frame reports itself incomplete instead of replaying as whole");
}

void aUniformAssemblyIsHeldByValue() {
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = 0xb7252004aba21c10ull;
    assembly.stageIndex = 0;
    assembly.blockSources = {4, 0xf4000000};
    assembly.data = {1.0f, 0.0f, 0.0f, 12.0f};

    FrameRecording recording;
    check::isTrue(recording.addUniformAssembly(assembly), "it was recorded");
    assembly.data[3] = 999.0f;

    check::equal(recording.uniformAssemblies()[0].data[3], 12.0f,
                 "the copy is not moved by a later write to the source");
    check::equal(recording.uniformAssemblies()[0].blockSources[1], uint32_t{0xf4000000},
                 "the block address is kept, since it is the object's identity");
}

void clearingReturnsTheRecordingToEmpty() {
    std::array<uint32_t, 4> data{1, 2, 3, 4};
    FrameRecording recording(16);
    recording.addDisplayList(1, data.data(), 16);
    recording.addDisplayList(2, data.data(), 16);
    check::equal(recording.refusedOverBudget(), size_t{1}, "the second was refused");

    recording.clear();
    check::equal(recording.displayLists().size(), size_t{0}, "no lists remain");
    check::equal(recording.byteCount(), size_t{0}, "the budget is released");
    check::isTrue(recording.isComplete(),
                  "and the previous frame's refusal does not condemn the next one");
    check::isTrue(recording.addDisplayList(3, data.data(), 16), "so recording can begin again");
}

} // namespace

namespace wiiuport::tests {

void runFrameTests() {
    aRecordedListSurvivesItsSource();
    anEmptyListIsStillARecordedList();
    goingOverBudgetRefusesRatherThanTruncating();
    aUniformAssemblyIsHeldByValue();
    clearingReturnsTheRecordingToEmpty();
}

} // namespace wiiuport::tests
