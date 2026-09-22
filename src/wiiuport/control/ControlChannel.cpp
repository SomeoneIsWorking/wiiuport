#include "wiiuport/control/ControlChannel.h"

#include <lucent/http.h>
#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::control {
namespace {

lucent::http::Response notFound() {
    return lucent::http::Response::text(
        404, "Not Found",
        "unknown route. This channel serves GET /counters, GET /transforms, GET /capture, "
        "GET /controllers, GET /setup, GET /substitution, GET /frames, POST /replay, POST "
        "/capture, POST "
        "/present, "
        "POST /nulldiff, POST /interpolate and POST /input.\n");
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
                               frame::FrameCapture& capture, frame::FramePresenter& presenter,
                               frame::ReplayScheduler& scheduler,
                               interp::FrameInterpolator& interpolator,
                               const frame::FrameShapeLog& shapeLog)
    : m_recorder(recorder), m_replayer(replayer), m_search(search), m_input(input),
      m_capture(capture), m_presenter(presenter), m_scheduler(scheduler),
      m_interpolator(interpolator), m_shapeLog(shapeLog) {
}

ControlChannel::~ControlChannel() = default;

void ControlChannel::setControllerStatus(const ControllerStatusSource* status) {
    m_controllerStatus = status;
}

std::string ControlChannel::controllersJson() const {
    if (m_controllerStatus == nullptr) {
        // Not the same as "no pad": this build has no host that attaches one.
        return "{\"hostReports\":false,\"attachedDevice\":\"\",\"devicesAttached\":0,"
               "\"devicesLost\":0,\"bindings\":0}";
    }
    std::string body = "{\"hostReports\":true";
    body += ",\"attachedDevice\":\"" + m_controllerStatus->attachedDevice() + "\"";
    body += ",\"devicesAttached\":" + std::to_string(m_controllerStatus->devicesAttached());
    body += ",\"devicesLost\":" + std::to_string(m_controllerStatus->devicesLost());
    body += ",\"bindings\":" + std::to_string(m_controllerStatus->bindingCount());
    body += "}";
    return body;
}

void ControlChannel::setSetupStatus(const SetupStatusSource* status) {
    m_setupStatus = status;
}

std::string ControlChannel::setupJson() const {
    if (m_setupStatus == nullptr) {
        // Not the same as a screen that is closed: nothing in this process
        // is in a position to show one.
        return "{\"hostReports\":false,\"shown\":false,\"state\":\"\",\"selectionsOffered\":0}";
    }
    std::string body = "{\"hostReports\":true";
    body += ",\"shown\":" + std::string(m_setupStatus->setupShown() ? "true" : "false");
    body += ",\"state\":\"" + m_setupStatus->setupState() + "\"";
    body += ",\"selectionsOffered\":" + std::to_string(m_setupStatus->setupSelectionsOffered());
    body += "}";
    return body;
}

std::string ControlChannel::countersJson() const {
    const frame::FrameRecording& last = m_recorder.lastCompleteFrame();
    std::string body = "{";
    body += "\"framesObserved\":" + std::to_string(m_recorder.framesObserved());
    body += ",\"framesRefusedIncomplete\":" + std::to_string(m_recorder.framesRefusedIncomplete());
    body += ",\"displayListsSeen\":" + std::to_string(m_recorder.displayListsSeen());
    body += ",\"uniformAssembliesSeen\":" + std::to_string(m_recorder.uniformAssembliesSeen());
    body += ",\"displayListsFromRuntime\":" + std::to_string(m_recorder.displayListsFromRuntime());
    body += ",\"uniformAssembliesFromRuntime\":" +
            std::to_string(m_recorder.uniformAssembliesFromRuntime());
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
    body += ",\"presentsObserved\":" + std::to_string(m_presenter.presentsObserved());
    body += ",\"presentsObservedTv\":" + std::to_string(m_presenter.presentsObservedTv());
    body += ",\"presentsObservedDrc\":" + std::to_string(m_presenter.presentsObservedDrc());
    body += ",\"presentsSubmitted\":" + std::to_string(m_presenter.presentsSubmitted());
    body +=
        ",\"presentsRefusedUnobserved\":" + std::to_string(m_presenter.presentsRefusedUnobserved());
    body += ",\"nullDiffsCompleted\":" + std::to_string(m_scheduler.nullDiffsCompleted());
    body += ",\"nullDiffPending\":" + std::string(m_scheduler.nullDiffPending() ? "true" : "false");
    body += ",\"presentsRefusedBySubmit\":" + std::to_string(m_presenter.presentsRefusedBySubmit());
    body += ",\"nestedListsSeen\":" + std::to_string(m_recorder.nestedListsSeen());
    body += ",\"guestDrawsFromCommandBuffers\":" +
            std::to_string(m_recorder.guestDrawsFromCommandBuffers());
    body += ",\"guestDrawsFromRing\":" + std::to_string(m_recorder.guestDrawsFromRing());
    body += ",\"runtimeSubmissions\":" + std::to_string(m_recorder.runtimeSubmissions());
    body += ",\"runtimePacketsProcessed\":" + std::to_string(m_recorder.runtimePacketsProcessed());
    body += ",\"runtimeDrawsIssued\":" + std::to_string(m_recorder.runtimeDrawsIssued());
    const interp::TransformSubstitution& substitution = m_interpolator.substitution();
    body += ",\"interpolatedFramesArmed\":" + std::to_string(m_interpolator.framesArmed());
    body += ",\"interpolatedFramesRefused\":" + std::to_string(m_interpolator.framesRefused());
    body += ",\"assembliesOffered\":" + std::to_string(substitution.assembliesOffered());
    body += ",\"assembliesSubstituted\":" + std::to_string(substitution.assembliesSubstituted());
    body += ",\"assembliesUnarmed\":" + std::to_string(substitution.assembliesUnarmed());
    body +=
        ",\"assembliesUnknownShader\":" + std::to_string(substitution.assembliesUnknownShader());
    body += ",\"assembliesTooShort\":" + std::to_string(substitution.assembliesTooShort());
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
    body += ",\"shadersInLastFrame\":" + std::to_string(report.shadersInLastFrame);
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

namespace {

std::string shaderJson(const interp::TransformSubstitution::OfferedShader& shader) {
    std::string body = "{";
    body += "\"stageIndex\":" + std::to_string(shader.shader.stageIndex);
    body += ",\"baseHash\":" + std::to_string(shader.shader.baseHash);
    body += ",\"auxHash\":" + std::to_string(shader.shader.auxHash);
    body += ",\"floats\":" + std::to_string(shader.floats);
    body += ",\"times\":" + std::to_string(shader.times);
    body += ",\"substituted\":" + std::string(shader.substituted ? "true" : "false");
    return body + "}";
}

std::string shaderArrayJson(const std::vector<interp::TransformSubstitution::OfferedShader>& set) {
    std::string body = "[";
    for (size_t i = 0; i < set.size(); ++i) {
        if (i != 0) {
            body += ",";
        }
        body += shaderJson(set[i]);
    }
    return body + "]";
}

} // namespace

std::string ControlChannel::framesJson() const {
    std::string body = "{\"framesLogged\":" + std::to_string(m_shapeLog.framesLogged());
    body += ",\"frames\":[";
    auto first = true;
    for (const frame::FrameShapeLog::FrameShape& shape : m_shapeLog.shapes()) {
        if (!first) {
            body += ",";
        }
        first = false;
        body += "{\"frameIndex\":" + std::to_string(shape.frameIndex);
        body += ",\"displayLists\":" + std::to_string(shape.displayLists);
        body += ",\"uniformAssemblies\":" + std::to_string(shape.uniformAssemblies);
        body += ",\"distinctShaders\":" + std::to_string(shape.distinctShaders);
        body += ",\"byteCount\":" + std::to_string(shape.byteCount);
        body += ",\"complete\":" + std::string(shape.complete ? "true" : "false") + "}";
    }
    return body + "]}\n";
}

std::string ControlChannel::substitutionJson() const {
    const interp::TransformSubstitution& substitution = m_interpolator.substitution();
    std::string body = "{";
    body += "\"armed\":" + std::string(substitution.isArmed() ? "true" : "false");
    body += ",\"blendPoint\":" + floatText(substitution.blendPoint());
    body += ",\"assembliesOffered\":" + std::to_string(substitution.assembliesOffered());
    body += ",\"assembliesSubstituted\":" + std::to_string(substitution.assembliesSubstituted());
    body += ",\"slots\":" + shaderArrayJson(substitution.armedSlots());
    body += ",\"offered\":" + shaderArrayJson(substitution.offeredShaders());
    return body + "}\n";
}

// The slot a capture route is talking about. Out of range is refused by the
// capture itself rather than clamped, so a typo does not silently read the
// wrong image.
size_t ControlChannel::requestedSlot(const std::string& query) {
    std::string_view rest(query);
    std::string_view key;
    std::string_view value;
    while (nextParameter(rest, key, value)) {
        if (key == "slot") {
            return static_cast<size_t>(std::strtoul(std::string(value).c_str(), nullptr, 10));
        }
    }
    return 0;
}

// A boolean query parameter, absent meaning the default. "0" and "false" are
// the only ways to turn one off, so a typo reads as the default rather than
// silently disabling a control.
bool ControlChannel::requestedFlag(const std::string& query, std::string_view name, bool fallback) {
    std::string_view rest(query);
    std::string_view key;
    std::string_view value;
    while (nextParameter(rest, key, value)) {
        if (key == name) {
            return !(value == "0" || value == "false");
        }
    }
    return fallback;
}

float ControlChannel::requestedBlend(const std::string& query, float fallback) {
    std::string_view rest(query);
    std::string_view key;
    std::string_view value;
    while (nextParameter(rest, key, value)) {
        if (key == "t") {
            char* end = nullptr;
            std::string text(value);
            float parsed = std::strtof(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0') {
                // Out of range on purpose: the interpolator refuses it and
                // says so, which a caller can read.
                return -1.0f;
            }
            return parsed;
        }
    }
    return fallback;
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
            // One frame captured twice, as the title drew it and as a replay
            // redrew it. The two halves have to be armed around the same
            // frame boundary, which only the scheduler can do.
            if (request.method == "POST" && request.path() == "/nulldiff") {
                bool redraw = requestedFlag(std::string(request.query()), "redraw", true);
                if (!m_scheduler.armNullDiff(redraw)) {
                    return lucent::http::Response::text(
                        409, "Conflict",
                        "a replay or a null diff is already armed, and taking it over would "
                        "compare a frame against one somebody else asked for.\n");
                }
                return lucent::http::Response::json(
                    200, "OK",
                    "{\"armed\":true,\"redraw\":" + std::string(redraw ? "true" : "false") +
                        ",\"nullDiffsCompleted\":" +
                        std::to_string(m_scheduler.nullDiffsCompleted()) + "}");
            }

            // One frame between two the title drew: the last frame's
            // geometry, replayed with the view blended between where the
            // camera stood in each. Captured as a null diff is, so the pair
            // that must differ is the title's frame and this one.
            if (request.method == "POST" && request.path() == "/interpolate") {
                float t = requestedBlend(std::string(request.query()), 0.5f);
                if (!m_interpolator.armOnce(t)) {
                    return lucent::http::Response::text(409, "Conflict",
                                                        m_interpolator.lastRefusal() + "\n");
                }
                return lucent::http::Response::json(
                    200, "OK",
                    "{\"armed\":true,\"t\":" + floatText(t) + ",\"slots\":" +
                        std::to_string(m_interpolator.substitution().slotCount()) + "}\n");
            }

            // A present the runtime owns, so a replay's output can be seen
            // instead of being overdrawn by the guest's next frame. Refused
            // when the title has not presented yet, because the arguments
            // are observed and never invented.
            if (request.method == "POST" && request.path() == "/present") {
                bool presented = m_presenter.presentNow();
                if (!presented && !m_presenter.hasObservedPresent()) {
                    return lucent::http::Response::text(
                        409, "Conflict",
                        "the title has not presented a frame yet, so there are no present "
                        "arguments to reuse. Reach gameplay first.\n");
                }
                return lucent::http::Response::json(
                    200, "OK",
                    "{\"presented\":" + std::string(presented ? "true" : "false") +
                        ",\"presentsSubmitted\":" +
                        std::to_string(m_presenter.presentsSubmitted()) +
                        ",\"presentsRefusedBySubmit\":" +
                        std::to_string(m_presenter.presentsRefusedBySubmit()) + "}");
            }
            if (request.method == "POST" && request.path() == "/capture") {
                auto armed = m_capture.armOnce(requestedSlot(std::string(request.query())));
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
            if (request.path() == "/controllers") {
                return lucent::http::Response::json(200, "OK", controllersJson());
            }
            if (request.path() == "/setup") {
                return lucent::http::Response::json(200, "OK", setupJson());
            }
            if (request.path() == "/counters") {
                return lucent::http::Response::json(200, "OK", countersJson());
            }
            if (request.path() == "/capture") {
                size_t slot = requestedSlot(std::string(request.query()));
                auto image = m_capture.lastImage(slot);
                if (image.empty()) {
                    // An empty body would read as a black frame. Refusing
                    // says which of the two actually happened.
                    return lucent::http::Response::text(
                        404, "Not Found",
                        "no frame has been captured into slot " + std::to_string(slot) +
                            " yet. Arm one with POST /capture and let the title present at "
                            "least once.\n");
                }
                return lucent::http::Response::binary(200, "OK", "application/octet-stream",
                                                      m_capture.lastImageFramed(slot));
            }
            if (request.path() == "/frames") {
                return lucent::http::Response::json(200, "OK", framesJson());
            }
            if (request.path() == "/substitution") {
                return lucent::http::Response::json(200, "OK", substitutionJson());
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
