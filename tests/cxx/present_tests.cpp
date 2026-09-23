#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/PresentPacing.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/ReplayScheduler.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

using wiiuport::frame::FrameCapture;
using wiiuport::frame::FramePresenter;
using wiiuport::frame::FrameRecording;
using wiiuport::frame::FrameReplayer;
using wiiuport::frame::PresentPacing;
using wiiuport::frame::ReplayScheduler;

namespace {

std::vector<LatteFrameHooks::PresentArguments> g_submitted;
bool g_acceptPresent = true;

bool recordPresent(const LatteFrameHooks::PresentArguments& present) {
    if (!g_acceptPresent) {
        return false;
    }
    g_submitted.push_back(present);
    return true;
}

// Which slots were armed, in order. The null diff's whole correctness is
// that each half goes to a slot chosen when it is armed, so this is the
// thing worth recording.
std::vector<size_t> g_armedSlots;
size_t g_nextSlot = 0;

bool recordArmedSlot(LatteFrameHooks::CaptureCallback) {
    g_armedSlots.push_back(g_nextSlot);
    return true;
}

// Callbacks kept unfired, so a test can deliver them out of order the way
// two detached renderer threads can.
std::vector<LatteFrameHooks::CaptureCallback> g_heldCallbacks;

bool holdCallback(LatteFrameHooks::CaptureCallback callback) {
    g_heldCallbacks.push_back(std::move(callback));
    return true;
}

uint64_t g_listsSubmitted = 0;

bool acceptList(const void*, uint32_t) {
    ++g_listsSubmitted;
    return true;
}

// The TV's copy: the screen the main window shows and every capture holds.
LatteFrameHooks::PresentArguments scanBuffer(uint32_t address) {
    return LatteFrameHooks::PresentArguments{address, 1920, 1080, 1920, 4, 0, 0, 1, 1, true, false};
}

// The GamePad's copy, which a title emits in the same frame.
LatteFrameHooks::PresentArguments padScanBuffer(uint32_t address) {
    return LatteFrameHooks::PresentArguments{address, 854, 480, 854, 4, 0, 0, 1, 4, false, true};
}

void reset() {
    g_heldCallbacks.clear();
    g_armedSlots.clear();
    g_nextSlot = 0;
    g_submitted.clear();
    g_acceptPresent = true;
    g_listsSubmitted = 0;
}

void presentingBeforeTheTitleHasIsRefusedNotInvented() {
    // A present built from zeroes would address a scan buffer nothing wrote,
    // and would look exactly like a present that worked.
    reset();
    FramePresenter presenter(&recordPresent);
    check::isTrue(!presenter.presentNow(), "nothing is presented before one has been observed");
    check::equal(presenter.presentsRefusedUnobserved(), uint64_t{1}, "and the refusal is counted");
    check::equal(g_submitted.size(), size_t{0}, "with nothing reaching the command processor");
}

void theArgumentsPresentedAreTheOnesTheTitleLastUsed() {
    reset();
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x1000));
    presenter.onPresentObserved(scanBuffer(0x2000));
    check::isTrue(presenter.presentNow(), "a present is submitted once one has been seen");
    check::equal(g_submitted.size(), size_t{1}, "exactly one");
    check::equal(g_submitted.front().physicalAddress, uint32_t{0x2000},
                 "carrying the most recent scan buffer, not the first");
    check::equal(g_submitted.front().width, uint32_t{1920}, "and its width");
    check::equal(presenter.presentsObserved(), uint64_t{2}, "both observations are counted");
    check::equal(presenter.presentsSubmitted(), uint64_t{1}, "and one submission");
}

void theGamePadsCopyIsNotWhatTheMainWindowIsShowing() {
    // A title copies twice a frame. Presenting the second one re-presents the
    // GamePad's scan buffer into the window the user is watching, which is
    // how a null-diff control that must be byte-identical came back with a
    // GamePad item icon drawn across it.
    reset();
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x1000));
    presenter.onPresentObserved(padScanBuffer(0x2000));
    check::isTrue(presenter.presentNow(), "a present is still submitted");
    check::equal(g_submitted.front().physicalAddress, uint32_t{0x1000},
                 "carrying the TV's scan buffer and not the GamePad's");
    check::equal(presenter.presentsObserved(), uint64_t{2}, "both copies are counted");
    check::equal(presenter.presentsObservedTv(), uint64_t{1}, "one of them the TV's");
    check::equal(presenter.presentsObservedDrc(), uint64_t{1}, "and one the GamePad's");
}

void aGamePadOnlyTitleIsRefusedRatherThanShownTheWrongScreen() {
    reset();
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(padScanBuffer(0x2000));
    check::isTrue(!presenter.presentNow(), "nothing is presented from the GamePad's copy alone");
    check::equal(presenter.presentsRefusedUnobserved(), uint64_t{1}, "and the refusal is counted");
    check::equal(presenter.presentsObservedDrc(), uint64_t{1},
                 "with the GamePad copies it did see reported, so the silence is explained");
}

void aRefusedSubmissionIsCountedSeparately() {
    // "The fork would not take it" and "I had nothing to send" are different
    // failures and a single counter would hide one behind the other.
    reset();
    g_acceptPresent = false;
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x3000));
    check::isTrue(!presenter.presentNow(), "a refused submission is not a present");
    check::equal(presenter.presentsRefusedBySubmit(), uint64_t{1}, "counted as a refusal");
    check::equal(presenter.presentsRefusedUnobserved(), uint64_t{0},
                 "and not as having nothing to send");
    check::equal(presenter.presentsSubmitted(), uint64_t{0}, "with no submission recorded");
}

void armingPresentsExactlyOnce() {
    reset();
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x4000));
    check::isTrue(!presenter.presentIfArmed(), "an unarmed presenter does nothing");
    presenter.armOnce();
    check::isTrue(presenter.isArmed(), "arming takes");
    check::isTrue(presenter.presentIfArmed(), "and the armed present fires");
    check::isTrue(!presenter.isArmed(), "leaving it disarmed");
    check::isTrue(!presenter.presentIfArmed(), "so it does not fire again");
    check::equal(g_submitted.size(), size_t{1}, "one present from one arming");
}

void observationsFlowFromTheRecorderToThePresenter() {
    // The wiring is the thing under test: a presenter that never hears about
    // a present refuses forever, and would do so silently.
    reset();
    wiiuport::frame::RecordingObserver recorder;
    FramePresenter presenter(&recordPresent);
    recorder.addPresentListener(&presenter);
    recorder.OnPresent(scanBuffer(0x5000));
    check::equal(recorder.presentsSeen(), uint64_t{1}, "the recorder counts the present");
    check::equal(presenter.presentsObserved(), uint64_t{1}, "and the presenter was told");
    check::equal(presenter.lastPresent().physicalAddress, uint32_t{0x5000},
                 "with the arguments intact");
}

void aReplayThatDrewSomethingIsPresented() {
    // A replay nobody presents is overdrawn before it is ever seen, which is
    // what the first null diff measured. The scheduler owns that pairing.
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x6000));
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);

    FrameRecording recording;
    std::array<uint32_t, 4> words{};
    recording.addDisplayList(0x100, words.data(), sizeof(words));

    scheduler.onFrameShown(recording);
    check::equal(g_submitted.size(), size_t{0}, "an unarmed frame end presents nothing");

    replayer.armOnce();
    scheduler.onFrameShown(recording);
    check::isTrue(g_listsSubmitted > 0, "the replay submitted its lists");
    check::equal(g_submitted.size(), size_t{1}, "and the frame it drew was presented");
}

void aReplayThatDrewNothingIsNotPresented() {
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x7000));
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);

    FrameRecording empty;
    replayer.armOnce();
    scheduler.onFrameShown(empty);
    check::equal(g_submitted.size(), size_t{0},
                 "presenting an empty replay would show the guest's frame as the replay's");
}

void aNullDiffCapturesTheSameFrameTwice() {
    // The two halves must straddle one frame boundary: the title's present
    // just before the frame end, the replay's just after. Comparing captures
    // taken seconds apart is what the first attempt did, and it measured the
    // scene moving rather than the replay.
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x8000));
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);

    check::isTrue(scheduler.armNullDiff(true), "a null diff arms when nothing else is replaying");
    check::equal(g_armedSlots.size(), size_t{0},
                 "nothing is captured yet: which frame it belongs to is not decided here");
    check::isTrue(scheduler.nullDiffPending(), "but the run is pending");

    FrameRecording recording;
    std::array<uint32_t, 4> words{};
    recording.addDisplayList(0x100, words.data(), sizeof(words));

    // First frame end: the title's capture is armed, to be consumed by the
    // swap that ends the next frame.
    scheduler.onFrameShown(recording);
    check::equal(g_armedSlots.size(), size_t{1}, "the title's half is armed at a frame end");
    check::equal(g_armedSlots.front(), ReplayScheduler::kTitleSlot, "into the title's slot");
    check::equal(g_listsSubmitted, uint64_t{0}, "and nothing is replayed yet");

    g_nextSlot = ReplayScheduler::kReplaySlot;
    scheduler.onFrameShown(recording);

    check::equal(g_armedSlots.size(), size_t{2}, "the replay's half is armed at the frame end");
    check::equal(g_armedSlots.back(), ReplayScheduler::kReplaySlot, "into the other slot");
    check::equal(g_submitted.size(), size_t{1}, "and the replay is presented so it can be seen");
    check::isTrue(!scheduler.nullDiffPending(), "the run is no longer pending");
    check::equal(scheduler.nullDiffsCompleted(), uint64_t{1}, "and is counted");
}

void aNullDiffWithoutRedrawPresentsWhatTheTitleDrew() {
    // The control that must come out identical. It replays nothing, so the
    // second capture is the same colour buffer re-presented; anything it
    // reports as a difference is the capture or present path lying.
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0xa000));
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);

    check::isTrue(scheduler.armNullDiff(false), "the control arms");
    scheduler.onFrameShown(FrameRecording{});
    check::isTrue(!replayer.isArmed(), "without arming a replay");

    g_nextSlot = ReplayScheduler::kReplaySlot;
    scheduler.onFrameShown(FrameRecording{});
    check::equal(g_armedSlots.size(), size_t{2}, "both halves are still captured");
    check::equal(g_listsSubmitted, uint64_t{0}, "with nothing replayed");
    check::equal(g_submitted.size(), size_t{1}, "and the same buffer presented again");
    check::equal(scheduler.nullDiffsCompleted(), uint64_t{1}, "the control counts as a run");
}

void aNullDiffWillNotStealAnArmedReplay() {
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);
    replayer.armOnce();
    check::isTrue(!scheduler.armNullDiff(true), "an already-armed replay is not taken over");
    check::isTrue(!scheduler.armNullDiff(false), "and neither is one for the control");
    check::equal(g_armedSlots.size(), size_t{0}, "and nothing is captured for a run not started");
}

void aNullDiffThatDrewNothingDoesNotCountAsOne() {
    // Presenting here would capture the title's own frame as the replay's,
    // and the comparison would pass by comparing an image against itself.
    reset();
    FrameReplayer replayer(&acceptList);
    FramePresenter presenter(&recordPresent);
    presenter.onPresentObserved(scanBuffer(0x9000));
    FrameCapture capture(&recordArmedSlot);
    ReplayScheduler scheduler(replayer, presenter, capture);

    check::isTrue(scheduler.armNullDiff(true), "armed");
    scheduler.onFrameShown(FrameRecording{});
    g_nextSlot = ReplayScheduler::kReplaySlot;
    scheduler.onFrameShown(FrameRecording{});
    check::equal(g_armedSlots.size(), size_t{1}, "only the title's half was ever armed");
    check::equal(g_submitted.size(), size_t{0}, "nothing was presented");
    check::equal(scheduler.nullDiffsCompleted(), uint64_t{0}, "and no run is claimed");
    check::isTrue(!scheduler.nullDiffPending(), "the pending run is cleared rather than left set");
}

void slotsAreChosenWhenArmedNotWhenDelivered() {
    // Two detached threads deliver these, so arrival order is not ordering.
    // The destination has to be decided at arming or the two halves of a null
    // diff can swap places without anything reporting that they did.
    reset();
    FrameCapture capture(&holdCallback);
    check::isTrue(capture.armOnce(0), "slot 0 arms");
    check::isTrue(capture.armOnce(1), "slot 1 arms");
    check::equal(g_heldCallbacks.size(), size_t{2}, "two callbacks are outstanding");

    std::array<uint8_t, 3> fromSlotOne{9, 9, 9};
    std::array<uint8_t, 3> fromSlotZero{1, 1, 1};
    // Deliver in the opposite order to arming.
    LatteFrameHooks::FrameImage second{fromSlotOne.data(), 3, 1, 1, true};
    g_heldCallbacks[1](second);
    LatteFrameHooks::FrameImage first{fromSlotZero.data(), 3, 1, 1, true};
    g_heldCallbacks[0](first);

    check::equal(capture.lastImage(0).rgb.front(), uint8_t{1}, "slot 0 holds what slot 0 armed");
    check::equal(capture.lastImage(1).rgb.front(), uint8_t{9}, "and slot 1 what slot 1 armed");
}

void anOutOfRangeSlotIsRefusedNotClamped() {
    reset();
    FrameCapture capture(&recordArmedSlot);
    check::isTrue(!capture.armOnce(FrameCapture::kSlotCount), "a slot that does not exist refuses");
    check::equal(capture.capturesRefused(), uint64_t{1}, "and is counted as a refusal");
    check::isTrue(capture.lastImage(FrameCapture::kSlotCount).empty(),
                  "and reading it gives nothing rather than another slot's image");
}

// The pacing tests' clock: moved by hand, so every interval is exact.
PresentPacing::Clock::time_point g_displayedAt{};

PresentPacing::Clock::time_point displayedAt() {
    return g_displayedAt;
}

using std::chrono::microseconds;
using std::chrono::milliseconds;

// One interpolated tick as it should reach the display: the runtime's frame
// `first` after the title's last one, then the title's `second` after that.
void displayTick(PresentPacing& pacing, milliseconds first, milliseconds second) {
    g_displayedAt += first;
    pacing.onDisplayed(true);
    g_displayedAt += second;
    pacing.onDisplayed(false);
}

void pacingTimesEveryFrameTheDisplayIsHanded() {
    PresentPacing pacing(&displayedAt);
    pacing.onDisplayed(false);
    for (int tick = 0; tick < 50; ++tick) {
        displayTick(pacing, milliseconds(12), milliseconds(21));
    }
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.guestFrames, uint64_t{51}, "the title's frames are counted");
    check::equal(seen.runtimeFrames, uint64_t{50}, "and the runtime's");
    check::equal(seen.intervals, uint64_t{100}, "one interval fewer than frames");
    check::equal(seen.guestToRuntimeMedian.count(), microseconds(milliseconds(12)).count(),
                 "the title's frame is on screen for its own share of the tick");
    check::equal(seen.runtimeToGuestMedian.count(), microseconds(milliseconds(21)).count(),
                 "and the runtime's for the rest: uneven pacing is visible as such");
    check::equal(seen.longest.count(), microseconds(milliseconds(21)).count(),
                 "the longest interval is the longer half");
}

void aStallShowsInTheTailNotTheMedian() {
    PresentPacing pacing(&displayedAt);
    pacing.onDisplayed(false);
    for (int tick = 0; tick < 99; ++tick) {
        displayTick(pacing, milliseconds(16), milliseconds(16));
    }
    displayTick(pacing, milliseconds(16), milliseconds(100));
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.p50.count(), microseconds(milliseconds(16)).count(), "the median holds");
    check::equal(seen.longest.count(), microseconds(milliseconds(100)).count(),
                 "the stall is the longest");
}

void restartingForgetsWhatCameBefore() {
    PresentPacing pacing(&displayedAt);
    pacing.onDisplayed(false);
    displayTick(pacing, milliseconds(500), milliseconds(500));
    pacing.restart();
    pacing.onDisplayed(false);
    displayTick(pacing, milliseconds(16), milliseconds(17));
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.intervals, uint64_t{2}, "only intervals after the restart count");
    check::equal(seen.longest.count(), microseconds(milliseconds(17)).count(),
                 "and the menus before it are not in the tail");
}

void nothingDisplayedReportsNoIntervals() {
    PresentPacing pacing(&displayedAt);
    pacing.onDisplayed(false);
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.intervals, uint64_t{0}, "one frame is no interval");
    check::equal(seen.intervalsKept, uint64_t{0}, "and none is kept");
}

// A frame the presentation engine reports shown, `at` ms on `clock`.
LatteFrameHooks::ShownFrame shownAt(bool fromRuntime, int64_t at, uint64_t clock) {
    return {fromRuntime, LatteFrameHooks::ShownStage::FirstPixelOut,
            static_cast<uint64_t>(std::chrono::nanoseconds(milliseconds(at)).count()), clock};
}

void scanOutTimesFramesByTheEnginesClock() {
    PresentPacing pacing(&displayedAt);
    check::isTrue(!pacing.summary().stage.has_value(), "no stage before a frame is shown");
    pacing.onScannedOut(shownAt(false, 1000, 7));
    pacing.onScannedOut(shownAt(true, 1017, 7));
    pacing.onScannedOut(shownAt(false, 1050, 7));
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.intervals, uint64_t{2}, "each shown frame after the first is an interval");
    check::equal(seen.guestToRuntimeMedian.count(), microseconds(milliseconds(17)).count(),
                 "timed by when the engine showed them, not when they were handed over");
    check::equal(seen.runtimeToGuestMedian.count(), microseconds(milliseconds(33)).count(),
                 "so a frame held for two refreshes shows as such");
    check::isTrue(seen.stage == LatteFrameHooks::ShownStage::FirstPixelOut,
                  "and the stage the times were taken at is reported");
}

void scanOutTimesOnAnotherClockAreNoInterval() {
    PresentPacing pacing(&displayedAt);
    pacing.onScannedOut(shownAt(false, 1000, 7));
    pacing.onScannedOut(shownAt(true, 5, 8));
    pacing.onScannedOut(shownAt(false, 22, 8));
    PresentPacing::Summary seen = pacing.summary();
    check::equal(seen.intervals, uint64_t{1}, "a change of clock starts the intervals afresh");
    check::equal(seen.longest.count(), microseconds(milliseconds(17)).count(),
                 "and times of two clocks are never subtracted");
}

} // namespace

namespace wiiuport::tests {

void runPresentTests() {
    scanOutTimesFramesByTheEnginesClock();
    scanOutTimesOnAnotherClockAreNoInterval();
    presentingBeforeTheTitleHasIsRefusedNotInvented();
    theArgumentsPresentedAreTheOnesTheTitleLastUsed();
    theGamePadsCopyIsNotWhatTheMainWindowIsShowing();
    aGamePadOnlyTitleIsRefusedRatherThanShownTheWrongScreen();
    aRefusedSubmissionIsCountedSeparately();
    armingPresentsExactlyOnce();
    observationsFlowFromTheRecorderToThePresenter();
    aReplayThatDrewSomethingIsPresented();
    aReplayThatDrewNothingIsNotPresented();
    aNullDiffCapturesTheSameFrameTwice();
    aNullDiffWithoutRedrawPresentsWhatTheTitleDrew();
    aNullDiffWillNotStealAnArmedReplay();
    aNullDiffThatDrewNothingDoesNotCountAsOne();
    slotsAreChosenWhenArmedNotWhenDelivered();
    anOutOfRangeSlotIsRefusedNotClamped();
    pacingTimesEveryFrameTheDisplayIsHanded();
    aStallShowsInTheTailNotTheMedian();
    restartingForgetsWhatCameBefore();
    nothingDisplayedReportsNoIntervals();
}

} // namespace wiiuport::tests
