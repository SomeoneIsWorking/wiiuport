#include "check.h"
#include "suites.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FramePresenter.h"
#include "wiiuport/frame/FrameRecording.h"
#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/GuestStateGuard.h"
#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/frame/ReplayScheduler.h"
#include "wiiuport/interp/ContinuousInterpolator.h"
#include "wiiuport/interp/CutDetector.h"
#include "wiiuport/interp/ObjectBlend.h"
#include "wiiuport/interp/RestoreCheck.h"
#include "wiiuport/interp/Transform3x4.h"
#include "wiiuport/interp/TransformSearch.h"
#include "wiiuport/interp/TransformSubstitution.h"
#include "wiiuport/interp/ViewTracker.h"

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using wiiuport::frame::FramePresenter;
using wiiuport::frame::FrameRecording;
using wiiuport::frame::FrameReplayer;
using wiiuport::frame::GuestStateGuard;
using wiiuport::frame::RecordedUniformAssembly;
using wiiuport::frame::RecordingSnapshot;
using wiiuport::interp::ContinuousInterpolator;
using wiiuport::interp::CutDetector;
using wiiuport::interp::RestoreCheck;
using wiiuport::interp::Transform3x4;
using wiiuport::interp::TransformSearch;
using wiiuport::interp::TransformSubstitution;
using wiiuport::interp::ViewTracker;
using Skip = wiiuport::interp::ContinuousInterpolator::Skip;
using std::chrono::milliseconds;

namespace {

// How far from the origin Wind Waker's camera stands on the sea.
constexpr float kSeaDistance = 300000.0f;

// A view at a translation, with an identity rotation.
std::array<float, 12> viewAt(float x, float y, float z) {
    return {1, 0, 0, x, 0, 1, 0, y, 0, 0, 1, z};
}

// A view turned `radians` about the vertical axis, at the origin.
std::array<float, 12> viewTurned(float radians) {
    float c = std::cos(radians);
    float s = std::sin(radians);
    return {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0};
}

Transform3x4 transformOf(const std::array<float, 12>& values) {
    return Transform3x4::fromRowMajor(values.data());
}

// Two world shaders carrying the view at different offsets, and one list to
// replay, which is what a frame of the title looks like to the interpolator.
FrameRecording worldFrame(const std::array<float, 12>& view) {
    FrameRecording frame;
    for (auto [hash, lead] : {std::pair<uint64_t, size_t>{0xaaaa, 0}, {0xbbbb, 4}}) {
        RecordedUniformAssembly assembly;
        assembly.shaderBaseHash = hash;
        assembly.data.assign(lead, 0.5f);
        assembly.data.insert(assembly.data.end(), view.begin(), view.end());
        frame.addUniformAssembly(assembly);
    }
    std::array<uint32_t, 4> list{};
    frame.addDisplayList(0x1000, list.data(), sizeof(list));
    return frame;
}

// Frames it takes a tracker to have a pair: the first frame's search has
// nothing to compare, the next search is kReseedInterval frames later and
// only finds the view, and the frame after that is the first one followed.
constexpr size_t kFramesUntilPaired = ViewTracker::kReseedInterval + 3;

using Clock = ContinuousInterpolator::Clock;

// The fake world the interpolator runs against: a clock that moves a fixed
// step each time it is read, and submissions counted instead of executed.
Clock::time_point g_now{};
constexpr std::chrono::milliseconds kReadStep{2};
std::vector<std::string> g_sequence;
bool g_presentAccepted = true;
bool g_captureAccepted = true;
// What the next restore reports: a copy that undid everything by default.
LatteFrameHooks::GuestStateRestore g_restore{};

Clock::time_point fakeNow() {
    g_now += kReadStep;
    return g_now;
}

bool fakeSubmitList(const void*, uint32_t) {
    g_sequence.emplace_back("replay");
    return true;
}

bool fakePresent(const LatteFrameHooks::PresentArguments&) {
    g_sequence.emplace_back("present");
    return g_presentAccepted;
}

bool fakeCopy(const LatteFrameHooks::PresentArguments&) {
    g_sequence.emplace_back("copy");
    return true;
}

void fakeGuard() {
    g_sequence.emplace_back("guard");
}

LatteFrameHooks::GuestStateRestore fakeRestore() {
    g_sequence.emplace_back("restore");
    return g_restore;
}

bool fakeCapture(LatteFrameHooks::CaptureCallback) {
    g_sequence.emplace_back("capture");
    return g_captureAccepted;
}

LatteFrameHooks::PresentArguments tvScanBuffer() {
    return LatteFrameHooks::PresentArguments{0x1000, 1920, 1080, 1920, 4, 0, 0, 1, 1, true, false};
}

void resetFakes() {
    g_now = {};
    g_sequence.clear();
    g_presentAccepted = true;
    g_captureAccepted = true;
    g_restore = LatteFrameHooks::GuestStateRestore{.subresourcesRestored = 5, .shadowsCreated = 2};
}

// Everything the interpolator is wired to in the product, with the fakes in
// place of the command processor.
struct Rig {
    TransformSearch search;
    ViewTracker tracker{search};
    TransformSubstitution substitution;
    wiiuport::interp::ObjectBlend objects{ContinuousInterpolator::kBlendPoint};
    FrameReplayer replayer{&fakeSubmitList};
    FramePresenter presenter{&fakePresent, &fakeCopy};
    wiiuport::frame::FrameCapture capture{&fakeCapture};
    GuestStateGuard guard{&fakeGuard, &fakeRestore};
    wiiuport::frame::ReplayScheduler scheduler{replayer, presenter, capture};
    RestoreCheck restoreCheck{presenter, capture};
    ContinuousInterpolator continuous{tracker, substitution, objects,      replayer, presenter,
                                      guard,   scheduler,    restoreCheck, &fakeNow};

    Rig() {
        resetFakes();
        continuous.setEnabled(true);
        presenter.onPresentObserved(tvScanBuffer());
    }

    // A camera walking one unit a tick until the tracker has a pair. Returns
    // where it stands next.
    float walkUntilPaired() {
        float x = 0.0f;
        for (size_t frame = 0; frame < kFramesUntilPaired; ++frame) {
            tick(viewAt(x, 0, 0));
            x += 1.0f;
        }
        return x;
    }

    // One guest tick: the frame is recorded, the listeners run in product
    // order, and the clock advances by the tick.
    void tick(const std::array<float, 12>& view) {
        FrameRecording frame = worldFrame(view);
        search.observe(frame);
        for (const auto& assembly : frame.uniformAssemblies()) {
            objects.onAssemblyRecorded(assembly);
        }
        tracker.onFrameRecorded(frame);
        objects.onFrameRecorded(frame);
        continuous.onFrameRecorded(frame);
        g_now += milliseconds(33);
    }
};

void aTurnTooFastForAnyCameraIsACut() {
    CutDetector cuts;
    check::isTrue(!cuts.isCut(transformOf(viewTurned(0)), transformOf(viewTurned(0.1f))),
                  "a tenth of a radian a tick is a camera turning");
    check::isTrue(cuts.isCut(transformOf(viewTurned(0)), transformOf(viewTurned(1.5f))),
                  "a right angle in one tick is a cut");
    check::equal(cuts.cutsByTurn(), uint64_t{1}, "and is counted as one by turn");
}

void aStepFarBeyondTheRecentOnesIsACut() {
    CutDetector cuts;
    for (int step = 0; step < 4; ++step) {
        auto from = static_cast<float>(step);
        check::isTrue(
            !cuts.isCut(transformOf(viewAt(from, 0, 0)), transformOf(viewAt(from + 1.0f, 0, 0))),
            "a steady walk is not a cut");
    }
    check::isTrue(!cuts.isCut(transformOf(viewAt(4, 0, 0)), transformOf(viewAt(4, 0, 0))),
                  "and standing still is not evidence either way");
    check::isTrue(!cuts.isCut(transformOf(viewAt(4, 0, 0)), transformOf(viewAt(7, 0, 0))),
                  "three times the usual step is a camera speeding up");
    check::isTrue(cuts.isCut(transformOf(viewAt(7, 0, 0)), transformOf(viewAt(107, 0, 0))),
                  "a hundred times it is a cut");
    check::equal(cuts.cutsByStep(), uint64_t{1}, "counted as one by step");
}

// Measured on the sea: the camera rests about 3e5 units out, where its
// recovered position wobbles by a unit in the last place, then walks at about
// 3.4 units a tick. Remembering only smooth moves locked the median at the
// wobble and called 745 of 753 walking ticks cuts.
void aWalkFromRestFarFromTheOriginIsNotACut() {
    CutDetector cuts;
    for (int tick = 0; tick < 20; ++tick) {
        float wobble = (tick % 2 == 0) ? 0.03125f : 0.0f;
        cuts.isCut(transformOf(viewAt(kSeaDistance, 0, 0)),
                   transformOf(viewAt(kSeaDistance + wobble, 0, 0)));
    }
    for (int tick = 0; tick < 30; ++tick) {
        float from = kSeaDistance + (3.5f * static_cast<float>(tick));
        cuts.isCut(transformOf(viewAt(from, 0, 0)), transformOf(viewAt(from + 3.5f, 0, 0)));
    }
    check::equal(cuts.cutsByStep(), uint64_t{0}, "rounding at rest sets no speed to jump from");
}

void aCameraThatSetsOffFasterIsFollowedWithinHalfAWindow() {
    CutDetector cuts;
    float at = 0.0f;
    for (int tick = 0; tick < 16; ++tick) {
        cuts.isCut(transformOf(viewAt(at, 0, 0)), transformOf(viewAt(at + 0.5f, 0, 0)));
        at += 0.5f;
    }
    int lastCut = -1;
    for (int tick = 0; tick < 30; ++tick) {
        if (cuts.isCut(transformOf(viewAt(at, 0, 0)), transformOf(viewAt(at + 10.0f, 0, 0)))) {
            lastCut = tick;
        }
        at += 10.0f;
    }
    check::isTrue(lastCut < static_cast<int>(CutDetector::kWindow / 2),
                  "a sustained new speed stops reading as a cut once it is half the window");
    check::isTrue(cuts.isCut(transformOf(viewAt(at, 0, 0)), transformOf(viewAt(at + 500.0f, 0, 0))),
                  "and a jump from that speed is still a cut");
    check::isTrue(
        !cuts.isCut(transformOf(viewAt(at + 500.0f, 0, 0)), transformOf(viewAt(at + 510.0f, 0, 0))),
        "after which the walk goes on uncut");
}

void aViewBlendIsExactAtItsEnds() {
    Transform3x4 from = transformOf(viewTurned(0.2f));
    Transform3x4 to = transformOf(viewAt(3, 4, 5));
    check::isTrue(Transform3x4::blendView(from, to, 0.0f).values() == from.values(),
                  "t=0 is the earlier view, bit for bit");
    check::isTrue(Transform3x4::blendView(from, to, 1.0f).values() == to.values(),
                  "t=1 is the later view, bit for bit");
}

void aViewBlendMovesTheCameraNotTheWorld() {
    // A camera that turns in place: a matrix lerp would also move it, a pose
    // blend keeps it at the origin and turns it half way.
    Transform3x4 half =
        Transform3x4::blendView(transformOf(viewTurned(0.0f)), transformOf(viewTurned(1.0f)), 0.5f);
    check::near(Transform3x4::rotationAngleBetween(half, transformOf(viewTurned(0.5f))), 0.0f,
                1e-4f, "half way through a turn is half the turn");
    auto position = half.rigidInverse().translation();
    check::near(position.x, 0.0f, 1e-5f, "with the camera still at the origin (x)");
    check::near(position.z, 0.0f, 1e-5f, "with the camera still at the origin (z)");
}

void aTransformIsFoundWhereItSitsInABuffer() {
    auto view = viewAt(1, 2, 3);
    std::vector<float> buffer(7, 0.25f);
    buffer.insert(buffer.end(), view.begin(), view.end());
    check::equal(transformOf(view).findIn(buffer.data(), buffer.size()), size_t{7},
                 "at the offset it was written at");
    check::equal(transformOf(viewAt(1, 2, 4)).findIn(buffer.data(), buffer.size()),
                 Transform3x4::kNotFound, "and not at all when one float differs");
}

void walkTracked(TransformSearch& search, ViewTracker& tracker) {
    for (size_t frame = 0; frame < kFramesUntilPaired; ++frame) {
        FrameRecording recorded = worldFrame(viewAt(static_cast<float>(frame), 0, 0));
        search.observe(recorded);
        tracker.onFrameRecorded(recorded);
    }
}

void theTrackerFollowsTheViewFrameToFrame() {
    TransformSearch search;
    ViewTracker tracker{search};
    walkTracked(search, tracker);
    check::isTrue(tracker.pair().has_value(), "a view followed across two frames has a pair");
    if (!tracker.pair().has_value()) {
        return;
    }
    check::equal(tracker.pair()->size(), size_t{2}, "one slot for each shader carrying it");
    auto last = static_cast<float>(kFramesUntilPaired - 1);
    check::equal(tracker.pair()->front().before.translation().x, last - 1.0f,
                 "with the frame before as one end");
    check::equal(tracker.pair()->front().after.translation().x, last,
                 "and the frame just drawn as the other");
}

void theTrackerLosesAViewNoLongerDrawn() {
    TransformSearch search;
    ViewTracker tracker{search};
    walkTracked(search, tracker);
    uint64_t lostBefore = tracker.framesLost();
    FrameRecording menu;
    RecordedUniformAssembly assembly;
    assembly.shaderBaseHash = 0xcccc;
    assembly.data.assign(12, 1.0f);
    menu.addUniformAssembly(assembly);
    search.observe(menu);
    tracker.onFrameRecorded(menu);
    check::isTrue(!tracker.pair().has_value(), "a frame without the view has no pair");
    check::equal(tracker.framesLost(), lostBefore + 1, "and is counted lost");
}

void everyTickWithoutAnInBetweenFrameSaysWhy() {
    Rig rig;
    rig.continuous.setEnabled(false);
    rig.tick(viewAt(0, 0, 0));
    check::equal(rig.continuous.skipped(Skip::Disabled), uint64_t{1}, "switched off");
    rig.continuous.setEnabled(true);
    rig.tick(viewAt(1, 0, 0));
    check::equal(rig.continuous.skipped(Skip::NoView), uint64_t{1},
                 "no view pair while the view is still being looked for");
    check::equal(rig.continuous.ticks(), uint64_t{2}, "and every tick is counted");
    check::equal(rig.continuous.framesInterpolated(), uint64_t{0}, "none of them interpolated");
    check::isTrue(g_sequence.empty(), "and nothing was submitted for them");
}

void aTickIsSplitInTwoByAnInBetweenFrame() {
    Rig rig;
    float x = rig.walkUntilPaired();
    rig.tick(viewAt(x, 0, 0));
    rig.tick(viewAt(x + 1.0f, 0, 0));
    check::equal(rig.continuous.framesInterpolated(), uint64_t{3},
                 "every tick after the view is found and followed is interpolated");
    std::vector<std::string> one = {"guard", "replay", "present", "restore", "copy"};
    std::vector<std::string> last(g_sequence.end() - 5, g_sequence.end());
    check::isTrue(last == one, "in-between frame replayed under the guard and shown, then the "
                               "guest's frame copied back and to the scan buffer");
    check::equal(rig.continuous.restoresByCopy(), uint64_t{3}, "every restore by copy");
    check::equal(rig.continuous.restoresByReplay(), uint64_t{0}, "and none drawn again");
    check::equal(rig.continuous.shadowsCreated(), uint64_t{6},
                 "the copies each allocated are summed");
    check::equal(rig.continuous.subresourcesRestored(), uint64_t{15},
                 "with the subresources each copied back summed");
    check::isTrue(!rig.guard.isOpen(), "and the guard closed after each");
    check::isTrue(!rig.substitution.isArmed() && !rig.objects.isArmed(),
                  "with both blends taken down after their replay");
    check::equal(rig.presenter.copiesSubmitted(), uint64_t{3}, "one restore per in-between frame");
    // Each read of the fake clock advances it a step, so every phase that
    // ran was charged time, and none that did not.
    for (auto phase :
         {ContinuousInterpolator::Phase::BlendedReplay, ContinuousInterpolator::Phase::Present,
          ContinuousInterpolator::Phase::Restore}) {
        check::isTrue(rig.continuous.timeIn(phase) == 3 * kReadStep,
                      std::string("three ticks charged to ") +
                          std::string(ContinuousInterpolator::phaseName(phase)));
    }
}

void aCameraCutIsShownAsDrawn() {
    Rig rig;
    float x = rig.walkUntilPaired();
    for (int step = 0; step < 4; ++step) {
        rig.tick(viewAt(x, 0, 0));
        x += 1.0f;
    }
    uint64_t before = rig.continuous.framesInterpolated();
    rig.tick(viewAt(500, 0, 0));
    check::equal(rig.continuous.skipped(Skip::CameraCut), uint64_t{1},
                 "a jump of hundreds of steps is not blended across");
    check::equal(rig.continuous.framesInterpolated(), before, "and is not interpolated");
}

void aRefusedPresentStillPutsTheGuestFrameBack() {
    Rig rig;
    float x = rig.walkUntilPaired();
    g_presentAccepted = false;
    g_sequence.clear();
    rig.tick(viewAt(x, 0, 0));
    check::equal(rig.continuous.skipped(Skip::PresentRefused), uint64_t{1},
                 "a refused present is a skip");
    check::isTrue(!g_sequence.empty() && g_sequence.back() == "copy",
                  "and the guest's own frame is still restored over the blended one");
}

void aRestoreTheCopiesCannotFinishDrawsTheGuestFrameAgain() {
    Rig rig;
    float x = rig.walkUntilPaired();
    g_sequence.clear();
    g_restore = LatteFrameHooks::GuestStateRestore{
        .subresourcesRestored = 5, .texturesCreated = 1, .streamoutWrites = 2};
    rig.tick(viewAt(x, 0, 0));
    std::vector<std::string> one = {"guard", "replay", "present", "restore", "replay", "copy"};
    check::isTrue(g_sequence == one, "a texture made or a buffer streamed out is put back by "
                                     "drawing the guest's frame again, after the copies");
    check::equal(rig.continuous.restoresByReplay(), uint64_t{1}, "counted as a replay restore");
    check::equal(rig.continuous.restoresByCopy(), rig.continuous.framesInterpolated() - 1,
                 "and not as a copy");
    check::equal(rig.continuous.notCopied().texturesCreated, uint64_t{1}, "with the texture");
    check::equal(rig.continuous.notCopied().streamoutWrites, uint64_t{2}, "and the writes named");
}

void aRestoreCheckCapturesTheGuestFrameBeforeAndAfter() {
    Rig rig;
    float x = rig.walkUntilPaired();
    check::isTrue(rig.restoreCheck.arm(RestoreCheck::Against::Restored), "a check is armed");
    check::isTrue(!rig.restoreCheck.arm(RestoreCheck::Against::InBetween),
                  "and a second one while it waits is refused");
    g_sequence.clear();
    rig.tick(viewAt(x, 0, 0));
    std::vector<std::string> restored = {"capture", "present", "guard",   "replay",
                                         "present", "restore", "capture", "copy"};
    check::isTrue(g_sequence == restored,
                  "the guest's frame is presented and captured before anything is drawn over "
                  "it, and captured again at the copy that puts it back, which is where a "
                  "capture is taken");
    check::equal(rig.restoreCheck.completed(), uint64_t{1}, "one check completed");

    check::isTrue(rig.restoreCheck.arm(RestoreCheck::Against::InBetween), "the control is armed");
    g_sequence.clear();
    rig.tick(viewAt(x + 1.0f, 0, 0));
    std::vector<std::string> control = {"capture", "present", "guard",   "replay",
                                        "capture", "present", "restore", "copy"};
    check::isTrue(g_sequence == control, "the control captures the in-between frame instead");
    check::equal(rig.restoreCheck.completed(), uint64_t{2}, "and completes too");

    g_sequence.clear();
    rig.tick(viewAt(x + 2.0f, 0, 0));
    check::isTrue(g_sequence.front() == "guard", "an unarmed tick captures and presents nothing");
}

void aRestoreCheckWhoseCaptureIsRefusedSaysSo() {
    Rig rig;
    float x = rig.walkUntilPaired();
    g_captureAccepted = false;
    rig.restoreCheck.arm(RestoreCheck::Against::Restored);
    rig.tick(viewAt(x, 0, 0));
    check::equal(rig.restoreCheck.refused(), uint64_t{1},
                 "a slot that was never armed would hold some other frame, so it is refused");
    check::equal(rig.restoreCheck.completed(), uint64_t{0}, "and not completed");
}

void theGuardRefusesToOpenTwiceOrCloseUnopened() {
    resetFakes();
    GuestStateGuard guard{&fakeGuard, &fakeRestore};
    bool refused = false;
    try {
        guard.restore();
    } catch (const std::logic_error&) {
        refused = true;
    }
    check::isTrue(refused, "a restore with nothing kept is refused");
    guard.open();
    refused = false;
    try {
        guard.open();
    } catch (const std::logic_error&) {
        refused = true;
    }
    check::isTrue(refused, "as is a second open, which would drop what the first kept");
    check::equal(g_sequence.size(), size_t{1}, "and neither reached the fork");
}

void aSnapshotHoldsConsecutiveFramesAsRecorded() {
    RecordingSnapshot snapshot;
    check::isTrue(!snapshot.arm(0), "zero frames is refused");
    check::isTrue(!snapshot.arm(RecordingSnapshot::kMaxFrames + 1), "as is more than the cap");
    check::isTrue(snapshot.framed().empty(), "and nothing is framed before a snapshot completes");
    check::isTrue(snapshot.arm(2), "two frames are armed");
    check::isTrue(!snapshot.arm(2), "and a second arm while filling is refused");
    snapshot.onFrameRecorded(worldFrame(viewAt(1, 0, 0)));
    check::equal(snapshot.snapshotsCompleted(), uint64_t{0}, "one frame is not yet two");
    snapshot.onFrameRecorded(worldFrame(viewAt(2, 0, 0)));
    check::equal(snapshot.snapshotsCompleted(), uint64_t{1}, "two frames complete it");

    std::string framed = snapshot.framed();
    check::isTrue(framed.rfind(RecordingSnapshot::kMagic, 0) == 0, "framed under its magic");
    uint32_t frames = 0;
    std::memcpy(&frames, framed.data() + 8, sizeof(frames));
    check::equal(frames, uint32_t{2}, "holding both frames");
    // magic, frames, complete, assemblies, base, aux, stage, sources, floats
    size_t firstFloat = 8 + 4 + 4 + 4 + 8 + 8 + 4 + 4 + 4;
    float value = 0.0f;
    std::memcpy(&value, framed.data() + firstFloat + (3 * sizeof(float)), sizeof(value));
    check::equal(value, 1.0f, "with the first frame's values first");
}

} // namespace

namespace wiiuport::tests {

void runContinuousTests() {
    aTurnTooFastForAnyCameraIsACut();
    aStepFarBeyondTheRecentOnesIsACut();
    aWalkFromRestFarFromTheOriginIsNotACut();
    aCameraThatSetsOffFasterIsFollowedWithinHalfAWindow();
    aViewBlendIsExactAtItsEnds();
    aViewBlendMovesTheCameraNotTheWorld();
    aTransformIsFoundWhereItSitsInABuffer();
    theTrackerFollowsTheViewFrameToFrame();
    theTrackerLosesAViewNoLongerDrawn();
    everyTickWithoutAnInBetweenFrameSaysWhy();
    aTickIsSplitInTwoByAnInBetweenFrame();
    aCameraCutIsShownAsDrawn();
    aRefusedPresentStillPutsTheGuestFrameBack();
    aRestoreTheCopiesCannotFinishDrawsTheGuestFrameAgain();
    theGuardRefusesToOpenTwiceOrCloseUnopened();
    aRestoreCheckCapturesTheGuestFrameBeforeAndAfter();
    aRestoreCheckWhoseCaptureIsRefusedSaysSo();
    aSnapshotHoldsConsecutiveFramesAsRecorded();
}

} // namespace wiiuport::tests
