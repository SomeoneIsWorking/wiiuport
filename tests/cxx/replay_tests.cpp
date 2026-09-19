#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameReplayer.h"

#include <array>
#include <cstdint>

using wiiuport::frame::FrameRecording;
using wiiuport::frame::FrameReplayer;

namespace {

// Submission is injected, so these drive the shipping replayer rather than a
// second copy of its logic. The counters live in file scope because the
// interface takes a plain function pointer: nothing crosses this boundary but
// a buffer and a size, which is what the fork's own hook takes.
int g_submissions = 0;
uint32_t g_lastSize = 0;
bool g_accept = true;

bool recordSubmission(const void*, uint32_t sizeInBytes) {
    g_submissions++;
    g_lastSize = sizeInBytes;
    return g_accept;
}

void reset() {
    g_submissions = 0;
    g_lastSize = 0;
    g_accept = true;
}

FrameRecording twoLists() {
    std::array<uint32_t, 4> data{1, 2, 3, 4};
    FrameRecording recording;
    recording.addDisplayList(1, data.data(), 16);
    recording.addDisplayList(2, data.data(), 8);
    return recording;
}

void anUnarmedReplayerSubmitsNothing() {
    reset();
    FrameReplayer replayer(&recordSubmission);
    check::equal(replayer.replayIfArmed(twoLists()), uint64_t{0}, "nothing was replayed");
    check::equal(g_submissions, 0, "and nothing reached the command processor");
    check::equal(replayer.replaysRun(), uint64_t{0}, "so no replay is counted either");
}

void armingReplaysExactlyOneFrame() {
    reset();
    FrameReplayer replayer(&recordSubmission);
    replayer.armOnce();
    check::isTrue(replayer.isArmed(), "it is armed");
    check::equal(replayer.replayIfArmed(twoLists()), uint64_t{2}, "both lists were submitted");
    check::isTrue(!replayer.isArmed(), "and firing disarms it");
    check::equal(replayer.replayIfArmed(twoLists()), uint64_t{0}, "so the next frame is untouched");
    check::equal(g_submissions, 2, "exactly two submissions in total");
    check::equal(g_lastSize, uint32_t{8}, "the second list's own size was passed, not the first's");
}

void aRefusedSubmissionIsCountedNotIgnored() {
    reset();
    g_accept = false;
    FrameReplayer replayer(&recordSubmission);
    replayer.armOnce();
    check::equal(replayer.replayIfArmed(twoLists()), uint64_t{0}, "nothing was accepted");
    check::equal(replayer.listsRefused(), uint64_t{2}, "and both refusals are counted");
    check::equal(replayer.replaysRun(), uint64_t{1},
                 "the replay still ran, so a run that drew nothing is visible as such");
}

void anIncompleteFrameIsNotReplayed() {
    reset();
    std::array<uint32_t, 4> data{1, 2, 3, 4};
    FrameRecording overBudget(16);
    overBudget.addDisplayList(1, data.data(), 16);
    overBudget.addDisplayList(2, data.data(), 16);
    check::isTrue(!overBudget.isComplete(), "the recording is short of a frame");

    FrameReplayer replayer(&recordSubmission);
    replayer.armOnce();
    check::equal(replayer.replayIfArmed(overBudget), uint64_t{0}, "it is not replayed");
    check::equal(g_submissions, 0, "so a partial image is never drawn");
    check::isTrue(!replayer.isArmed(),
                  "and the arming is spent, rather than firing on a later frame nobody asked for");
}

} // namespace

namespace wiiuport::tests {

void runReplayTests() {
    anUnarmedReplayerSubmitsNothing();
    armingReplaysExactlyOneFrame();
    aRefusedSubmissionIsCountedNotIgnored();
    anIncompleteFrameIsNotReplayed();
}

} // namespace wiiuport::tests
