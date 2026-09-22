#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/FrameShapeLog.h"
#include "wiiuport/frame/RecordingObserver.h"

#include <array>
#include <cstring>
#include <vector>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::frame::RecordingObserver;

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

// The observer is driven through the fork's own hook types rather than a
// stand-in for them, so a change to that interface breaks this build instead
// of leaving a test that agrees with a version nobody ships any more.
LatteFrameHooks::DisplayList listOf(uint32_t address, const void* data, uint32_t size) {
    LatteFrameHooks::DisplayList list{};
    list.physicalAddress = address;
    list.data = data;
    list.sizeInBytes = size;
    list.topLevel = true;
    return list;
}

void aFrameIsOnlyPublishedWhenItEnds() {
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    RecordingObserver observer;
    observer.OnDisplayList(listOf(0x30000000, guest.data(), 16));

    check::equal(observer.lastCompleteFrame().displayLists().size(), size_t{0},
                 "a frame still being recorded is not readable as a complete one");
    check::equal(observer.framesObserved(), uint64_t{0}, "and no frame has ended yet");

    observer.OnFrameComplete();
    check::equal(observer.framesObserved(), uint64_t{1}, "the frame ended");
    check::equal(observer.lastCompleteFrame().displayLists().size(), size_t{1},
                 "and is now readable");
}

void theNextFrameDoesNotAccumulateOntoTheLast() {
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    RecordingObserver observer;
    observer.OnDisplayList(listOf(1, guest.data(), 16));
    observer.OnFrameComplete();
    observer.OnDisplayList(listOf(2, guest.data(), 16));
    observer.OnDisplayList(listOf(3, guest.data(), 16));
    observer.OnFrameComplete();

    check::equal(observer.lastCompleteFrame().displayLists().size(), size_t{2},
                 "the second frame holds its own two lists, not four");
    check::equal(observer.lastCompleteFrame().displayLists()[0].physicalAddress, uint32_t{2},
                 "and starts at the second frame's first list");
    check::equal(observer.displayListsSeen(), uint64_t{3}, "while the total seen still counts all");
}

// A filter that records what it was offered, so "never called" is visible.
struct RecordingFilter final : public wiiuport::frame::AssemblyFilter {
    std::vector<uint64_t> offered;

    bool onRuntimeAssembly(const LatteFrameHooks::UniformAssembly& assembly) override {
        offered.push_back(assembly.shaderBaseHash);
        return true;
    }
};

void aReplaysOwnDrawsAreNotRecordedAsTheNextFrame() {
    // The runtime replays inside the frame end, so its draws arrive while the
    // next frame is being recorded. Recording them would make every frame a
    // copy of the one before with the replay folded in, and the error would
    // compound once a replay runs every frame.
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    RecordingObserver observer;
    observer.OnFrameComplete();

    LatteFrameHooks::DisplayList replayed = listOf(0x50000000, guest.data(), 16);
    replayed.fromRuntime = true;
    observer.OnDisplayList(replayed);
    observer.OnDisplayList(listOf(0x60000000, guest.data(), 16));
    observer.OnFrameComplete();

    check::equal(observer.lastCompleteFrame().displayLists().size(), size_t{1},
                 "the frame holds the guest's list alone");
    check::equal(observer.lastCompleteFrame().displayLists()[0].physicalAddress,
                 uint32_t{0x60000000}, "and it is the guest's");
    check::equal(observer.displayListsFromRuntime(), uint64_t{1},
                 "the replay's list is counted rather than silently dropped");
    check::equal(observer.displayListsSeen(), uint64_t{2}, "with both still in the total");
}

void onlyTheRuntimesOwnAssembliesReachTheFilter() {
    // A blend edits the runtime's replay. Editing the guest's own draw would
    // change what the title is showing, which is not interpolation.
    std::array<float, 12> values{};
    LatteFrameHooks::UniformAssembly guestDraw{};
    guestDraw.shaderBaseHash = 0x1111;
    guestDraw.data = values.data();
    guestDraw.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));

    LatteFrameHooks::UniformAssembly replayedDraw = guestDraw;
    replayedDraw.shaderBaseHash = 0x2222;
    replayedDraw.fromRuntime = true;

    RecordingFilter filter;
    RecordingObserver observer;
    observer.setAssemblyFilter(&filter);
    observer.OnUniformAssembly(guestDraw);
    observer.OnUniformAssembly(replayedDraw);
    observer.OnFrameComplete();

    check::equal(filter.offered.size(), size_t{1}, "one assembly was offered to the filter");
    check::equal(filter.offered.front(), uint64_t{0x2222}, "and it is the runtime's own");
    check::equal(observer.lastCompleteFrame().uniformAssemblies().size(), size_t{1},
                 "only the guest's assembly is recorded");
    check::equal(observer.uniformAssembliesFromRuntime(), uint64_t{1},
                 "and the runtime's is counted");
}

void aUniformAssemblyCrossesTheHookIntact() {
    std::array<float, 12> values{1, 0, 0, 10, 0, 1, 0, 20, 0, 0, 1, 30};
    std::array<uint32_t, 2> sources{4, 0xf4000000};
    LatteFrameHooks::UniformAssembly assembly{};
    assembly.shaderBaseHash = 0xb7252004aba21c10ull;
    assembly.stageIndex = 0;
    assembly.data = values.data();
    assembly.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));
    assembly.blockAddresses = sources.data();
    // Counted in pairs, as the renderer passes it: one block, two words.
    assembly.blockAddressCount = 1;

    RecordingObserver observer;
    observer.OnUniformAssembly(assembly);
    observer.OnFrameComplete();
    values[3] = 999.0f;

    const auto& recorded = observer.lastCompleteFrame().uniformAssemblies();
    check::equal(recorded.size(), size_t{1}, "one assembly was recorded");
    check::equal(recorded[0].data.size(), size_t{12}, "all twelve floats came across");
    check::equal(recorded[0].data[3], 10.0f, "by copy, so the guest's later write is not seen");
    check::equal(recorded[0].blockSources.size(), size_t{2}, "both source words came across");
    check::equal(recorded[0].blockSources[1], uint32_t{0xf4000000},
                 "including the address, which is the object's identity");
    check::equal(recorded[0].shaderBaseHash, 0xb7252004aba21c10ull,
                 "with the shader it belongs to");
}

void anOversizedSourceListIsCappedNotTrusted() {
    std::array<uint32_t, 128> sources{};
    std::array<float, 4> values{};
    LatteFrameHooks::UniformAssembly assembly{};
    assembly.data = values.data();
    assembly.sizeInBytes = static_cast<uint32_t>(values.size() * sizeof(float));
    assembly.blockAddresses = sources.data();
    // A count the interface says cannot happen. If it does, reading it would
    // run off the caller's buffer.
    assembly.blockAddressCount = 64;

    RecordingObserver observer;
    observer.OnUniformAssembly(assembly);
    observer.OnFrameComplete();
    check::equal(observer.lastCompleteFrame().uniformAssemblies()[0].blockSources.size(),
                 size_t{LatteFrameHooks::kMaxUniformBlockSources} * 2,
                 "the read stops at the interface's own cap");
}

void whatASubmissionReachedIsSummedRatherThanOverwritten() {
    // The question this answers is the one a replayed frame cannot: a
    // submission that walked no packets and one that drew every triangle
    // leave the same colour buffer behind.
    RecordingObserver recorder;
    recorder.OnRuntimeSubmission({0, 0});
    check::equal(recorder.runtimeSubmissions(), uint64_t{1}, "an empty submission still counts");
    check::equal(recorder.runtimePacketsProcessed(), uint64_t{0},
                 "with zero packets reported as zero, not as absence");
    recorder.OnRuntimeSubmission({470, 96});
    check::equal(recorder.runtimeSubmissions(), uint64_t{2}, "a second submission is counted");
    check::equal(recorder.runtimePacketsProcessed(), uint64_t{470}, "and its packets added");
    check::equal(recorder.runtimeDrawsIssued(), uint64_t{96}, "and its draws");
}

void aNestedBufferIsCountedAndNotRecordedTwice() {
    // The buffer a frame's draws live in references others. Replaying the
    // outer one walks into them, so recording them as well would issue their
    // contents a second time -- and recording only them, as this did before
    // the top-level buffers were hooked, replays a frame with no draws in it
    // at all.
    std::array<uint32_t, 4> guest{1, 2, 3, 4};
    RecordingObserver observer;
    auto nested = listOf(0x30000000, guest.data(), 16);
    nested.topLevel = false;
    observer.OnDisplayList(nested);
    observer.OnDisplayList(listOf(0x30001000, guest.data(), 16));
    observer.OnFrameComplete();

    check::equal(observer.lastCompleteFrame().displayLists().size(), size_t{1},
                 "only the buffer the command queue submitted is recorded");
    check::equal(observer.nestedListsSeen(), uint64_t{1}, "and the nested one is counted");
    check::equal(observer.displayListsSeen(), uint64_t{2}, "against everything seen");
}

void aDrawTheRingIssuedIsCountedApartFromOneACommandBufferDid() {
    // What a recording can hold and what a frame contains are not the same
    // number, and a replay that reproduces every buffer it was given can
    // still be a fraction of the frame.
    RecordingObserver observer;
    observer.OnGuestDraw(true);
    observer.OnGuestDraw(false);
    observer.OnGuestDraw(true);
    check::equal(observer.guestDrawsFromCommandBuffers(), uint64_t{2},
                 "draws from a buffer a recording holds are counted");
    check::equal(observer.guestDrawsFromRing(), uint64_t{1},
                 "and the ones no recording of buffers can reach are counted apart");
}

void theShapeOfEachPublishedFrameIsKeptAndTheOldestDropped() {
    // A frame's own totals cannot say whether the recorder publishes whole
    // frames or halves of them. A run of consecutive frames can, which is why
    // this keeps a window rather than a latest.
    wiiuport::frame::FrameShapeLog log{2};
    check::equal(log.shapes().size(), size_t{0}, "nothing published is an empty window");

    for (uint64_t index = 0; index < 3; ++index) {
        FrameRecording frame;
        RecordedUniformAssembly one;
        one.shaderBaseHash = index;
        one.data = {1.0f};
        frame.addUniformAssembly(one);
        RecordedUniformAssembly again = one;
        frame.addUniformAssembly(again);
        log.onFrameRecorded(frame);
    }

    check::equal(log.framesLogged(), uint64_t{3}, "every published frame is counted");
    check::equal(log.shapes().size(), size_t{2}, "and the window holds the depth asked for");
    check::equal(log.shapes().front().frameIndex, uint64_t{1}, "oldest first, the oldest dropped");
    check::equal(log.shapes().back().uniformAssemblies, size_t{2}, "with the draws it held");
    check::equal(log.shapes().back().distinctShaders, size_t{1},
                 "and the shaders behind them, which is the number a replay is judged on");
}

} // namespace

namespace wiiuport::tests {

void runFrameTests() {
    aRecordedListSurvivesItsSource();
    anEmptyListIsStillARecordedList();
    goingOverBudgetRefusesRatherThanTruncating();
    aUniformAssemblyIsHeldByValue();
    clearingReturnsTheRecordingToEmpty();
    aFrameIsOnlyPublishedWhenItEnds();
    theNextFrameDoesNotAccumulateOntoTheLast();
    aReplaysOwnDrawsAreNotRecordedAsTheNextFrame();
    onlyTheRuntimesOwnAssembliesReachTheFilter();
    aUniformAssemblyCrossesTheHookIntact();
    anOversizedSourceListIsCappedNotTrusted();
    whatASubmissionReachedIsSummedRatherThanOverwritten();
    aNestedBufferIsCountedAndNotRecordedTwice();
    aDrawTheRingIssuedIsCountedApartFromOneACommandBufferDid();
    theShapeOfEachPublishedFrameIsKeptAndTheOldestDropped();
}

} // namespace wiiuport::tests
