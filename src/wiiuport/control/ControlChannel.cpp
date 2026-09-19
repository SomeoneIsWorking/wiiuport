#include "wiiuport/control/ControlChannel.h"

#include <lucent/http.h>
#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <string>

namespace wiiuport::control {
namespace {

lucent::http::Response notFound() {
    return lucent::http::Response::text(
        404, "Not Found",
        "unknown route. This channel serves GET /counters, GET /transforms and POST /replay.\n");
}

// Enough digits that two transforms which differ are never printed the same.
std::string floatText(float value) {
    std::array<char, 32> text{};
    auto written = std::snprintf(text.data(), text.size(), "%.9g", static_cast<double>(value));
    if (written <= 0) {
        return "0";
    }
    return std::string(text.data(), static_cast<size_t>(written));
}

std::string transformJson(const interp::TransformCandidate& candidate) {
    std::string body = "{";
    body += "\"stageIndex\":" + std::to_string(candidate.shader.stageIndex);
    body += ",\"baseHash\":" + std::to_string(candidate.shader.baseHash);
    body += ",\"auxHash\":" + std::to_string(candidate.shader.auxHash);
    body += ",\"floatOffset\":" + std::to_string(candidate.floatOffset);
    body += ",\"framesSeen\":" + std::to_string(candidate.framesSeen);
    body += ",\"shadersSharing\":" + std::to_string(candidate.shadersSharing);
    body += ",\"rotationError\":" + floatText(candidate.rotationError);
    body += ",\"meanTranslationStep\":" + floatText(candidate.meanTranslationStep);
    body += ",\"values\":[";
    auto first = true;
    for (auto value : candidate.latest.values()) {
        if (!first) {
            body += ",";
        }
        first = false;
        body += floatText(value);
    }
    body += "]}";
    return body;
}

} // namespace

ControlChannel::ControlChannel(const frame::RecordingObserver& recorder,
                               frame::FrameReplayer& replayer,
                               const interp::TransformSearch& search)
    : m_recorder(recorder), m_replayer(replayer), m_search(search) {
}

ControlChannel::~ControlChannel() = default;

std::string ControlChannel::countersJson() const {
    const frame::FrameRecording& last = m_recorder.lastCompleteFrame();
    std::string body = "{";
    body += "\"framesObserved\":" + std::to_string(m_recorder.framesObserved());
    body += ",\"framesRefusedIncomplete\":" + std::to_string(m_recorder.framesRefusedIncomplete());
    body += ",\"displayListsSeen\":" + std::to_string(m_recorder.displayListsSeen());
    body += ",\"uniformAssembliesSeen\":" + std::to_string(m_recorder.uniformAssembliesSeen());
    body += ",\"lastFrameDisplayLists\":" + std::to_string(last.displayLists().size());
    body += ",\"lastFrameUniformAssemblies\":" + std::to_string(last.uniformAssemblies().size());
    body += ",\"lastFrameBytes\":" + std::to_string(last.byteCount());
    body += ",\"replaysRun\":" + std::to_string(m_replayer.replaysRun());
    body += ",\"replayListsSubmitted\":" + std::to_string(m_replayer.listsSubmitted());
    body += ",\"replayListsRefused\":" + std::to_string(m_replayer.listsRefused());
    body += ",\"replayArmed\":" + std::string(m_replayer.isArmed() ? "true" : "false");
    body += "}\n";
    return body;
}

std::string ControlChannel::transformsJson(size_t limit) const {
    auto report = m_search.search();
    std::string body = "{";
    body += "\"framesObserved\":" + std::to_string(report.framesObserved);
    body += ",\"shadersTracked\":" + std::to_string(report.shadersTracked);
    body += ",\"spansExamined\":" + std::to_string(report.spansExamined);
    body += ",\"rejectedVaryingWithinFrame\":" + std::to_string(report.rejectedVaryingWithinFrame);
    body += ",\"rejectedNeverChanging\":" + std::to_string(report.rejectedNeverChanging);
    body += ",\"rejectedRotation\":" + std::to_string(report.rejectedRotation);
    body += ",\"candidatesFound\":" + std::to_string(report.candidates.size());
    body += ",\"candidates\":[";
    for (size_t i = 0; i < report.candidates.size() && i < limit; ++i) {
        if (i != 0) {
            body += ",";
        }
        body += transformJson(report.candidates[i]);
    }
    body += "]}\n";
    return body;
}

bool ControlChannel::start(uint16_t port) {
    if (m_server) {
        return true;
    }
    lucent::http::ServerOptions options;
    options.port = port;
    options.listen_scope = lucent::http::ListenScope::Loopback;
    m_server = std::make_unique<lucent::http::Server>(
        options, [this](const lucent::http::Request& request) -> lucent::http::Response {
            // Arming is a deliberate one-shot: the next frame to end is
            // replayed, and nothing after it. A replay that repeated every
            // frame would make a crash impossible to attribute.
            if (request.method == "POST" && request.path() == "/replay") {
                m_replayer.armOnce();
                return lucent::http::Response::json(
                    200, "OK",
                    "{\"armed\":true,\"replaysRun\":" + std::to_string(m_replayer.replaysRun()) +
                        "}\n");
            }
            if (request.method != "GET") {
                return notFound();
            }
            if (request.path() == "/counters") {
                return lucent::http::Response::json(200, "OK", countersJson());
            }
            if (request.path() == "/transforms") {
                return lucent::http::Response::json(200, "OK",
                                                    transformsJson(kDefaultTransformLimit));
            }
            return notFound();
        });
    if (!m_server->start()) {
        lucent::error("control",
                      "could not listen on loopback port {}; the product continues "
                      "without a control channel",
                      port);
        m_server.reset();
        return false;
    }
    lucent::info("control", "listening on http://127.0.0.1:{}/counters", m_server->port());
    return true;
}

bool ControlChannel::running() const {
    return m_server != nullptr;
}

uint16_t ControlChannel::port() const {
    return m_server ? m_server->port() : 0;
}

} // namespace wiiuport::control
