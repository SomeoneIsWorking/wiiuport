#include "wiiuport/interp/ContinuousInterpolator.h"

namespace wiiuport::interp {

std::string_view ContinuousInterpolator::skipName(Skip skip) {
    switch (skip) {
    case Skip::Disabled:
        return "disabled";
    case Skip::OneShotInFlight:
        return "oneShotInFlight";
    case Skip::IncompleteRecording:
        return "incompleteRecording";
    case Skip::NoPresentSeen:
        return "noPresentSeen";
    case Skip::NoView:
        return "noView";
    case Skip::CameraCut:
        return "cameraCut";
    case Skip::ReplayDrewNothing:
        return "replayDrewNothing";
    case Skip::PresentRefused:
        return "presentRefused";
    case Skip::Count:
        break;
    }
    return "unknown";
}

std::string_view ContinuousInterpolator::phaseName(Phase phase) {
    switch (phase) {
    case Phase::BlendedReplay:
        return "blendedReplay";
    case Phase::Present:
        return "present";
    case Phase::Restore:
        return "restore";
    case Phase::Count:
        break;
    }
    return "unknown";
}

ContinuousInterpolator::ContinuousInterpolator(
    const ViewTracker& tracker, TransformSubstitution& substitution, ObjectBlend& objects,
    frame::FrameReplayer& replayer, frame::FramePresenter& presenter, frame::GuestStateGuard& guard,
    const frame::ReplayScheduler& scheduler, TickProbe& probe, Now now)
    : m_tracker(tracker), m_substitution(substitution), m_objects(objects), m_replayer(replayer),
      m_presenter(presenter), m_guard(guard), m_scheduler(scheduler), m_probe(probe), m_now(now) {
}

void ContinuousInterpolator::skip(Skip reason) {
    ++m_skipped[static_cast<size_t>(reason)];
}

ContinuousInterpolator::Clock::time_point ContinuousInterpolator::charge(Phase phase,
                                                                         Clock::time_point since) {
    Clock::time_point now = m_now();
    m_timeIn[static_cast<size_t>(phase)] +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - since);
    return now;
}

void ContinuousInterpolator::restoreGuestFrame(const frame::FrameRecording& recording) {
    LatteFrameHooks::GuestStateRestore restored = m_guard.restore();
    m_subresourcesRestored += restored.subresourcesRestored;
    m_shadowsCreated += restored.shadowsCreated;
    if (restored.Complete()) {
        ++m_restoresByCopy;
    } else {
        // Some of the in-between frame is still where the guest will read
        // it, and only drawing the guest's frame again overwrites it.
        ++m_restoresByReplay;
        m_notCopied.subresources += restored.subresourcesUncopied;
        m_notCopied.texturesCreated += restored.texturesCreated;
        m_notCopied.streamoutWrites += restored.streamoutWrites;
        m_replayer.armOnce();
        if (m_replayer.replayIfArmed(recording) == 0) {
            ++m_restoresRefused;
            return;
        }
    }
    m_probe.beforeGuestFrameCopied();
    if (!m_presenter.copyNow()) {
        ++m_restoresRefused;
    }
}

void ContinuousInterpolator::onFrameRecorded(const frame::FrameRecording& recording) {
    ++m_ticks;
    if (!m_enabled.load()) {
        skip(Skip::Disabled);
        return;
    }
    if (m_scheduler.nullDiffPending() || m_replayer.isArmed()) {
        skip(Skip::OneShotInFlight);
        return;
    }
    if (!recording.isComplete()) {
        skip(Skip::IncompleteRecording);
        return;
    }
    if (!m_presenter.hasObservedPresent()) {
        skip(Skip::NoPresentSeen);
        return;
    }
    const auto& pair = m_tracker.pair();
    if (!pair.has_value() || pair->empty()) {
        skip(Skip::NoView);
        return;
    }
    if (m_cuts.isCut(pair->front().before, pair->front().after)) {
        skip(Skip::CameraCut);
        return;
    }

    m_probe.beforeInBetween();
    Clock::time_point started = m_now();
    // Planned while the guest drew; a frame planning missed draws as drawn.
    m_objects.armOnce();
    m_substitution.armOnce(*pair, kBlendPoint);
    m_guard.open();
    m_replayer.armOnce();
    uint64_t lists = m_replayer.replayIfArmed(recording);
    m_substitution.disarm();
    m_objects.disarm();
    Clock::time_point replayed = charge(Phase::BlendedReplay, started);
    if (lists == 0) {
        // Nothing was drawn, so the colour buffer still holds the guest's
        // frame and there is nothing to put back.
        m_guard.restore();
        m_probe.afterTick();
        skip(Skip::ReplayDrewNothing);
        return;
    }
    m_probe.beforeInBetweenPresent();
    if (!m_presenter.presentNow()) {
        restoreGuestFrame(recording);
        m_probe.afterTick();
        skip(Skip::PresentRefused);
        return;
    }
    Clock::time_point shownAt = charge(Phase::Present, replayed);
    restoreGuestFrame(recording);
    charge(Phase::Restore, shownAt);
    m_probe.afterTick();
    ++m_framesInterpolated;
}

} // namespace wiiuport::interp
