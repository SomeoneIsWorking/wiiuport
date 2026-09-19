#pragma once

#include "wiiuport/frame/FrameReplayer.h"
#include "wiiuport/frame/RecordingObserver.h"
#include "wiiuport/input/InputDriver.h"
#include "wiiuport/interp/TransformSearch.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace lucent::http {
class Server;
}

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
    static constexpr size_t kDefaultTransformLimit = 20;

    // How many gamepad reads a press is held for when the caller does not
    // say. Long enough that a title sampling once a frame cannot miss it.
    static constexpr uint32_t kDefaultPressReads = 8;

    ControlChannel(const frame::RecordingObserver& recorder, frame::FrameReplayer& replayer,
                   const interp::TransformSearch& search, input::InputDriver& input);
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // False when the listener could not bind, which is reported and not fatal:
    // a busy port must not stop the product running.
    bool start(uint16_t port);
    bool running() const;
    uint16_t port() const;

    // The bodies of the two GET routes. Pure, so a test reads exactly what a
    // client would without opening a socket.
    std::string countersJson() const;

    // What the transform search has found, with the denominators that say
    // whether it looked. `limit` caps the candidate list only; the totals
    // describe the whole search.
    std::string transformsJson(size_t limit) const;

    // Applies one input request and returns the body describing what it did.
    // `accepted` is false when nothing in the query named a button or a
    // stick, so a typo is refused rather than answered with a cheerful no-op.
    std::string applyInput(const std::string& query, bool& accepted);

  private:
    const frame::RecordingObserver& m_recorder;
    frame::FrameReplayer& m_replayer;
    const interp::TransformSearch& m_search;
    input::InputDriver& m_input;
    std::unique_ptr<lucent::http::Server> m_server;
};

} // namespace wiiuport::control
