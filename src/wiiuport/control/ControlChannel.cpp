#include "wiiuport/control/ControlChannel.h"

#include <lucent/http.h>
#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace wiiuport::control {
namespace {

lucent::http::Response notFound() {
    return lucent::http::Response::text(
        404, "Not Found",
        "unknown route. This channel serves GET /counters, GET /transforms, GET /capture, "
        "POST /replay, POST /capture and POST /input.\n");
}

// One `key=value` pair at a time out of a query string. Returns false at the
// end; an unparseable pair stops the walk rather than being skipped, because
// a silently ignored parameter is a press the caller believes it sent.
bool nextParameter(std::string_view& rest, std::string_view& key, std::string_view& value) {
    if (rest.empty()) {
        return false;
    }
    auto amp = rest.find('&');
    auto pair = rest.substr(0, amp);
    rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
    auto equals = pair.find('=');
    if (equals == std::string_view::npos) {
        return false;
    }
    key = pair.substr(0, equals);
    value = pair.substr(equals + 1);
    return true;
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
                               const interp::TransformSearch& search, input::InputDriver& input,
                               frame::FrameCapture& capture)
    : m_recorder(recorder), m_replayer(replayer), m_search(search), m_input(input),
      m_capture(capture) {
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
    body += ",\"inputPollsSeen\":" + std::to_string(m_input.pollsSeen());
    body += ",\"inputPollsAnswered\":" + std::to_string(m_input.pollsAnswered());
    body += ",\"inputPressesQueued\":" + std::to_string(m_input.pressesQueued());
    body += ",\"capturesRequested\":" + std::to_string(m_capture.capturesRequested());
    body += ",\"capturesRefused\":" + std::to_string(m_capture.capturesRefused());
    body += ",\"imagesReceived\":" + std::to_string(m_capture.imagesReceived());
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
    body += ",\"sharedAndMoving\":" + std::to_string(report.sharedAndMoving);
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

std::string ControlChannel::applyInput(const std::string& query, bool& accepted) {
    accepted = false;
    std::string_view rest(query);
    std::string_view key;
    std::string_view value;
    uint32_t reads = kDefaultPressReads;
    uint32_t mask = 0;
    auto unknown = std::string();
    auto releasing = false;
    auto haveLeftStick = false;
    auto haveRightStick = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    while (nextParameter(rest, key, value)) {
        if (key == "reads") {
            reads = static_cast<uint32_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
            continue;
        }
        if (key == "release") {
            releasing = true;
            continue;
        }
        if (key == "press") {
            auto* button = input::InputDriver::buttonNamed(std::string(value));
            if (button == nullptr) {
                unknown = std::string(value);
                break;
            }
            mask |= button->mask;
            continue;
        }
        if (key == "leftx" || key == "lefty" || key == "rightx" || key == "righty") {
            auto number = std::strtof(std::string(value).c_str(), nullptr);
            if (key == "leftx") {
                leftX = number;
                haveLeftStick = true;
            } else if (key == "lefty") {
                leftY = number;
                haveLeftStick = true;
            } else if (key == "rightx") {
                rightX = number;
                haveRightStick = true;
            } else {
                rightY = number;
                haveRightStick = true;
            }
            continue;
        }
        unknown = std::string(key);
        break;
    }
    if (!unknown.empty()) {
        return "{\"accepted\":false,\"reason\":\"unknown parameter or button: " + unknown + "\"}\n";
    }
    if (releasing) {
        m_input.release();
        accepted = true;
    }
    if (haveLeftStick) {
        m_input.setLeftStick(leftX, leftY);
        accepted = true;
    }
    if (haveRightStick) {
        m_input.setRightStick(rightX, rightY);
        accepted = true;
    }
    if (mask != 0) {
        m_input.press(mask, reads);
        accepted = true;
    }
    if (!accepted) {
        return "{\"accepted\":false,\"reason\":\"nothing to do: name a button with press=, a "
               "stick with leftx=/lefty=/rightx=/righty=, or release=1\"}\n";
    }
    return "{\"accepted\":true,\"holdMask\":" + std::to_string(mask) +
           ",\"reads\":" + std::to_string(reads) +
           ",\"pollsAnswered\":" + std::to_string(m_input.pollsAnswered()) + "}\n";
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
            if (request.method == "POST" && request.path() == "/capture") {
                auto armed = m_capture.armOnce();
                return lucent::http::Response::json(
                    armed ? 200 : 503, armed ? "OK" : "Service Unavailable",
                    std::string("{\"armed\":") + (armed ? "true" : "false") +
                        ",\"imagesReceived\":" + std::to_string(m_capture.imagesReceived()) +
                        "}\n");
            }
            if (request.method == "POST" && request.path() == "/input") {
                auto accepted = false;
                auto body = applyInput(std::string(request.query()), accepted);
                return lucent::http::Response::json(accepted ? 200 : 400,
                                                    accepted ? "OK" : "Bad Request", body);
            }
            if (request.method != "GET") {
                return notFound();
            }
            if (request.path() == "/counters") {
                return lucent::http::Response::json(200, "OK", countersJson());
            }
            if (request.path() == "/capture") {
                auto image = m_capture.lastImage();
                if (image.empty()) {
                    // An empty body would read as a black frame. Refusing
                    // says which of the two actually happened.
                    return lucent::http::Response::text(
                        404, "Not Found",
                        "no frame has been captured yet. Arm one with POST /capture and "
                        "let the title present at least once.\n");
                }
                return lucent::http::Response::binary(200, "OK", "application/octet-stream",
                                                      m_capture.lastImageFramed());
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
