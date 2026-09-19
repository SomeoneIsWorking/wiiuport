#pragma once

#include "wiiuport/frame/RecordingObserver.h"

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
    explicit ControlChannel(const frame::RecordingObserver& recorder);
    ~ControlChannel();

    ControlChannel(const ControlChannel&) = delete;
    ControlChannel& operator=(const ControlChannel&) = delete;

    // False when the listener could not bind, which is reported and not fatal:
    // a busy port must not stop the product running.
    bool start(uint16_t port);
    bool running() const;
    uint16_t port() const;

    // The body of GET /counters. Pure, so a test reads exactly what a client
    // would without opening a socket.
    std::string countersJson() const;

  private:
    const frame::RecordingObserver& m_recorder;
    std::unique_ptr<lucent::http::Server> m_server;
};

} // namespace wiiuport::control
