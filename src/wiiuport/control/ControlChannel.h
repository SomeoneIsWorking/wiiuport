#pragma once

#include "wiiuport/control/ControllerStatus.h"
#include "wiiuport/control/GuestMemoryRead.h"
#include "wiiuport/control/HostStop.h"
#include "wiiuport/control/SetupStatus.h"
#include "wiiuport/frame/FrameCapture.h"
#include "wiiuport/frame/FrameGate.h"
#include "wiiuport/frame/FrameShapeLog.h"
#include "wiiuport/frame/PresentPacing.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/frame/RecordingSnapshot.h"
#include "wiiuport/frame/VertexChanges.h"
#include "wiiuport/guest/CallerCensus.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/title/DrawInterpolation.h"
#include "wiiuport/title/LogicGate.h"
#include "wiiuport/title/ParticleInterpolation.h"
#include "wiiuport/title/WindWakerPaint.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lucent::http {
class Server;
struct Request;
struct Response;
} // namespace lucent::http

namespace wiiuport::control {

// The way an agent asks the running product what it is doing.
//
// It exists because the alternative is reading the log after the fact, which
// forces a pre-baked run and cannot ask a question the script did not already
// know to ask. Off unless a port is configured, loopback only, and it owns no
// state of its own: every answer is read from the recorder it was given.
class ControlChannel {
  public:
    // How many candidates GET /transforms lists. The totals beside them
    // are never capped, so a cut list still reports how many there were.
    // The product always listens, on loopback only, so a player's own session
    // can be asked what it is doing; WIIUPORT_CONTROL_PORT only moves it.
    static constexpr long long kDefaultPort = 21337;
    // How long POST /gate waits for the title to hold: a frame, or a step's
    // frames, end well inside it at any rate the title runs.
    static constexpr std::chrono::milliseconds kGateHoldTimeout{10000};

    // How many gamepad reads a press is held for when the caller does not
    // say. Long enough that a title sampling once a frame cannot miss it.
    static constexpr uint32_t kDefaultPressReads = 8;

    // Every owner the channel reads or drives, by name. Positional arguments
    // stopped scaling once the list passed half a dozen references of
    // interchangeable-looking types.
    struct Sources {
        const frame::RecordingObserver& recorder;
        input::InputDriver& input;
        frame::FrameCapture& capture;
        const frame::FrameShapeLog& shapeLog;
        const guest::CallerCensus& callers;
        title::WindWakerPaint& paint;
        title::DrawInterpolation& drawInterpolation;
        const title::ParticleInterpolation& particleInterpolation;
        title::LogicGate& logic;
        GuestMemoryRead::GuestBytes guestBytes;
        frame::RecordingSnapshot& snapshot;
        frame::PresentPacing& pacing;
        // Frame times as the presentation engine reports them shown.
        frame::PresentPacing& scanOut;
        frame::VertexChanges& vertexChanges;
        frame::FrameGate& gate;
    };

    explicit ControlChannel(const Sources& sources);
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // The host registers what it knows about physical controllers. Absent
    // until it does, and GET /controllers then says so rather than reporting
    // an empty state that looks like "no pad".
    void setControllerStatus(const ControllerStatusSource* status);

    // What is attached now, or an explicit statement that no host reported.
    std::string controllersJson() const;

    // The host registers the first-run screen while it is up. Absent until it
    // does, and GET /setup then says so rather than reporting a screen that
    // is merely not shown.
    void setSetupStatus(const SetupStatusSource* status);

    // What the setup screen is waiting for, or that no host reported one.
    std::string setupJson() const;

    // The host registers itself while a title runs, so POST /quit can ask it
    // to stop the way a player closing its window does; nullptr unregisters.
    void setHostStop(HostStopTarget* target);

    // The body of POST /quit, and whether a host took the request: none is
    // registered while nothing runs, which is refused rather than ignored.
    std::string requestHostStop(bool& accepted);

    // False when the listener could not bind, which is reported and not fatal:
    // a busy port must not stop the product running.
    bool start(uint16_t port);
    bool running() const;
    uint16_t port() const;

    // The routing table, reachable without a socket. A client sends a method
    // and a target; this answers it exactly as the listener would, so a test
    // can ask about every route the channel claims to serve and see whether the
    // claim is true. It is public because that is the point: the routes are the
    // channel's contract, not its wiring.
    lucent::http::Response dispatch(const lucent::http::Request& request);

    // The routes this channel serves, as the unknown-route refusal names them.
    // One owner, so the refusal and the routing table cannot disagree -- which
    // is how a route came to be advertised and unreachable.
    static std::string_view routeList();

    // The bodies of the two GET routes. Pure, so a test reads exactly what a
    // client would without opening a socket.
    std::string countersJson() const;
    // The pacing route's answer, which is the pacing's own summary. It was `interpolationJson`,
    // and it carried the retired mechanism's frame counts as well; a route that keeps a name must
    // keep an answer, so what is left is the pacing's numbers and nothing else.
    std::string pacingJson() const;

    // What the transform search has found, with the denominators that say
    // whether it looked. `limit` caps the candidate list only; the totals
    // describe the whole search.

    // The two shader key sets an interpolated frame depends on agreeing:
    // what the blend was armed for, and what the replay actually offered.
    // Reported together because "none of them carried the view" is a
    // statement about both and neither alone can show it.

    // What the last few published frames held, oldest first. One frame's
    // totals cannot show whether the recorder publishes whole frames or
    // halves of them; a run of them can.
    std::string framesJson() const;

    // The title's draws whose vertex shader reads no uniforms, by vertex
    // shader, beside every draw it prepared: what no blend can move.
    std::string drawsJson() const;

    // Whether every frame is being interpolated, and for every tick that was
    // not, why -- with the tracker, phase-time and withheld-packet counts beside
    // it, so a run that never interpolated says so in numbers.
    // interpolationJson's fields for the replayed draws whose vertices the
    // title rewrote: by what blending them came to, over the runtime's draws
    // that could take new vertices, with what keeping and blending cost.
    // GET /vertices: each vertex shader's replayed draws by outcome.

    // Which capture slot a query names, defaulting to the first.
    static size_t requestedSlot(const std::string& query);

    // Where between the last two frames an interpolated frame stands. An
    // unparseable value is refused by the interpolator's range check rather
    // than read as the default, so a typo cannot quietly ask for t=0.
    static float requestedBlend(const std::string& query, float fallback);

    // A boolean query parameter, defaulting when it is absent.
    static bool requestedFlag(const std::string& query, std::string_view name, bool fallback);

    // A non-negative count query parameter. Zero when present but not a
    // plain decimal number, so the caller's range check refuses it.
    static size_t requestedCount(const std::string& query, std::string_view name, size_t fallback);

    // Applies one input request and returns the body describing what it did.
    // `accepted` is false when nothing in the query named a button or a
    // stick, so a typo is refused rather than answered with a cheerful no-op.
    std::string applyInput(const std::string& query, bool& accepted);

  private:
    const frame::RecordingObserver& m_recorder;
    input::InputDriver& m_input;
    frame::FrameCapture& m_capture;
    const ControllerStatusSource* m_controllerStatus{nullptr};
    const SetupStatusSource* m_setupStatus{nullptr};
    std::atomic<HostStopTarget*> m_hostStop{nullptr};
    const frame::FrameShapeLog& m_shapeLog;
    const guest::CallerCensus& m_callers;
    title::WindWakerPaint& m_paint;
    title::DrawInterpolation& m_drawInterpolation;
    const title::ParticleInterpolation& m_particleInterpolation;
    title::LogicGate& m_logic;
    GuestMemoryRead::GuestBytes m_guestBytes;
    frame::RecordingSnapshot& m_snapshot;
    frame::PresentPacing& m_pacing;
    frame::PresentPacing& m_scanOut;
    frame::VertexChanges& m_vertexChanges;
    frame::FrameGate& m_gate;
    std::unique_ptr<lucent::http::Server> m_server;
};

} // namespace wiiuport::control
