#include "wiiuport/control/ControlChannel.h"

#include <lucent/http.h>
#include <lucent/log.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace wiiuport::control {
namespace {

// The channel's own route list, in one place, because a list that is written
// twice is a list that can disagree with itself: a route the refusal advertises
// and no method reaches is a route that looks supported until a run needs it.
// tests/cxx/control_tests.cpp asks every entry here for itself and fails on any
// that comes back with the unknown-route refusal.
// **The routes the channel serves, and the list is the dispatch table's own.**
//
// The host-side frame interpolation is gone -- the camera search that found a 3x4 moving like a
// camera, the statistical blend of the last two views, the object and vertex planning, the recorded
// replay, the guest state guard, and the cut and neighbour checks -- and with it every route that
// asked about it or drove it. What is left is what the product still is: the title's own draws
// recorded and read back, the paint path, the logic gate, the data-area pose scan, and the input
// and pacing an agent needs to drive a run.
//
// A route that keeps a name keeps an answer, so `/pacing` is still here and answers with the
// pacing's own summary rather than with the interpolated frame's counts.
const char* const kRoutes =
    "GET /counters, GET /capture, "
    "GET /controllers, GET /setup, "
    "GET /recordings, GET /draws, GET /memory, GET /callers, GET /view, GET /quads, "
    "GET /paint, GET /blocks, GET /logic, GET /gate, GET /pose, "
    "POST /global-pose, POST /pose, POST /capture, POST /pacing, "
    "POST /draws, POST /recordings, POST /paint, POST /logic, POST /input "
    "and POST /quit";

lucent::http::Response notFound() {
    return lucent::http::Response::text(
        404, "Not Found", std::string("unknown route. This channel serves ") + kRoutes + ".\n");
}

// The one-shot routes each own a frame boundary, and so does continuous
// interpolation. Two owners of one boundary produce a capture of whichever
// happened to win, so a one-shot waits until continuous is switched off.
lucent::http::Response continuousOwnsFrames() {
    return lucent::http::Response::text(
        409, "Conflict",
        "continuous interpolation owns every frame boundary. POST /continuous?on=0 first.\n");
}

std::string_view withheldName(LatteFrameHooks::WithheldEffect effect) {
    switch (effect) {
    case LatteFrameHooks::WithheldEffect::Presentation:
        return "presentation";
    case LatteFrameHooks::WithheldEffect::Synchronisation:
        return "synchronisation";
    case LatteFrameHooks::WithheldEffect::GuestMemoryWrite:
        return "guestMemoryWrite";
    case LatteFrameHooks::WithheldEffect::OcclusionQuery:
        return "occlusionQuery";
    case LatteFrameHooks::WithheldEffect::TextureReadback:
        return "textureReadback";
    case LatteFrameHooks::WithheldEffect::Count:
        break;
    }
    return "unknown";
}

struct QueryParameter {
    std::string_view key;
    std::string_view value;
};

// One `key=value` pair at a time out of a query string, empty at the end; an
// unparseable pair stops the walk rather than being skipped, because a
// silently ignored parameter is a press the caller believes it sent.
std::optional<QueryParameter> nextParameter(std::string_view& rest) {
    if (rest.empty()) {
        return std::nullopt;
    }
    auto amp = rest.find('&');
    auto pair = rest.substr(0, amp);
    rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
    auto equals = pair.find('=');
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    return QueryParameter{.key = pair.substr(0, equals), .value = pair.substr(equals + 1)};
}

std::string_view shownStageName(LatteFrameHooks::ShownStage stage) {
    switch (stage) {
    case LatteFrameHooks::ShownStage::QueueDone:
        return "queueDone";
    case LatteFrameHooks::ShownStage::Dequeued:
        return "dequeued";
    case LatteFrameHooks::ShownStage::FirstPixelOut:
        return "firstPixelOut";
    case LatteFrameHooks::ShownStage::FirstPixelVisible:
        return "firstPixelVisible";
    }
    return "unknown";
}

std::string gateJson(const frame::FrameGate::Status& gate) {
    return std::string("{\"paused\":") + (gate.paused ? "true" : "false") +
           ",\"holding\":" + (gate.holding ? "true" : "false") +
           ",\"stepsLeft\":" + std::to_string(gate.stepsLeft) +
           ",\"framesHeld\":" + std::to_string(gate.framesHeld) +
           ",\"framesStepped\":" + std::to_string(gate.framesStepped) + "}\n";
}

std::string pacingJson(const frame::PresentPacing::Summary& pacing) {
    std::string body = "{\"guestFrames\":" + std::to_string(pacing.guestFrames);
    body += ",\"runtimeFrames\":" + std::to_string(pacing.runtimeFrames);
    body += ",\"intervals\":" + std::to_string(pacing.intervals);
    body += ",\"intervalsKept\":" + std::to_string(pacing.intervalsKept);
    body += ",\"p50Us\":" + std::to_string(pacing.p50.count());
    body += ",\"p95Us\":" + std::to_string(pacing.p95.count());
    body += ",\"p99Us\":" + std::to_string(pacing.p99.count());
    body += ",\"longestUs\":" + std::to_string(pacing.longest.count());
    body += ",\"guestToRuntimeMedianUs\":" + std::to_string(pacing.guestToRuntimeMedian.count());
    body += ",\"runtimeToGuestMedianUs\":" + std::to_string(pacing.runtimeToGuestMedian.count());
    body += ",\"stage\":";
    body += pacing.stage.has_value() ? "\"" + std::string(shownStageName(*pacing.stage)) + "\""
                                     : std::string("null");
    return body + "}";
}

} // namespace

ControlChannel::ControlChannel(const Sources& sources)
    : m_recorder(sources.recorder), m_input(sources.input), m_capture(sources.capture),
      m_shapeLog(sources.shapeLog), m_callers(sources.callers), m_paint(sources.paint),
      m_blocks(sources.blocks), m_poses(sources.poses), m_poseByShader(sources.poseByShader),
      m_poseBlend(sources.poseBlend), m_viewBlend(sources.viewBlend),
      m_quadBlend(sources.quadBlend), m_logic(sources.logic), m_globalPose(sources.globalPose),
      m_guestBytes(sources.guestBytes), m_snapshot(sources.snapshot), m_pacing(sources.pacing),
      m_scanOut(sources.scanOut), m_vertexChanges(sources.vertexChanges), m_gate(sources.gate) {
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

void ControlChannel::setHostStop(HostStopTarget* target) {
    m_hostStop.store(target);
}

std::string ControlChannel::requestHostStop(bool& accepted) {
    HostStopTarget* target = m_hostStop.load();
    accepted = target != nullptr;
    if (!accepted) {
        return "{\"stopping\":false,\"reason\":\"no title is running, so no host is there to "
               "stop\"}\n";
    }
    target->requestStop();
    return "{\"stopping\":true}\n";
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
    body += ",\"inputPollsSeen\":" + std::to_string(m_input.pollsSeen());
    body += ",\"inputPollsAnswered\":" + std::to_string(m_input.pollsAnswered());
    body += ",\"inputPressesQueued\":" + std::to_string(m_input.pressesQueued());
    body += ",\"capturesRequested\":" + std::to_string(m_capture.capturesRequested());
    body += ",\"capturesRefused\":" + std::to_string(m_capture.capturesRefused());
    body += ",\"imagesReceived\":" + std::to_string(m_capture.imagesReceived());
    body += ",\"nestedListsSeen\":" + std::to_string(m_recorder.nestedListsSeen());
    body += ",\"guestDrawsFromCommandBuffers\":" +
            std::to_string(m_recorder.guestDrawsFromCommandBuffers());
    body += ",\"guestDrawsFromRing\":" + std::to_string(m_recorder.guestDrawsFromRing());
    body += ",\"guestDrawsPrepared\":" + std::to_string(m_recorder.guestDrawsPrepared());
    body += ",\"guestDrawsWithoutVertexUniforms\":" +
            std::to_string(m_recorder.guestDrawsWithoutVertexUniforms());
    body += ",\"runtimeSubmissions\":" + std::to_string(m_recorder.runtimeSubmissions());
    body += ",\"runtimePacketsProcessed\":" + std::to_string(m_recorder.runtimePacketsProcessed());
    body += ",\"runtimeDrawsIssued\":" + std::to_string(m_recorder.runtimeDrawsIssued());
    body += "}\n";
    return body;
}

std::string ControlChannel::pacingJson() const {
    return control::pacingJson(m_pacing.summary());
}

// The pose table and the blend, as **one document with two named sections**, composed from the two
// owners' own `writeTo` rather than from their rendered text. Splicing rendered documents is how a
// report grows a second one, and this project has paid for that once; the composition is done by
// the classes that own the fields and this one only names them.
std::string ControlChannel::poseForShader(uint64_t shaderBaseHash, uint64_t shaderAuxHash) const {
    title::JsonBody body;
    body.string("shaderBaseHash", title::JsonBody::hex(shaderBaseHash));
    body.string("shaderAuxHash", title::JsonBody::hex(shaderAuxHash));
    // **What happened to this shader's own assemblies, and this is the answer the candidate
    // list cannot give.** "0 candidates of 300 considered" says the census found nothing that
    // belongs to this shader; it does not say whether the scan ever looked at one of them. A
    // shader whose every assembly is too short to hold twelve floats has no matrix in its
    // uniforms *to* find -- a fact about the title -- while a shader that was scanned and held
    // none was looked at and came back empty, which is a fact about the search. The two call
    // for opposite next steps, and before this the report carried only the whole-run total.
    // **A zero aux hash asks about every aux hash of this base hash**, for the same reason the
    // candidate filter below does: the census keys on the pair and a caller naming one hash was
    // never told the other. `std::optional` rather than a magic zero, so a caller's own zero
    // cannot be read as a request for "all aux hashes" by accident.
    const std::optional<uint64_t> wantAux =
        shaderAuxHash == 0 ? std::nullopt : std::optional<uint64_t>{shaderAuxHash};
    const title::ObjectPoseLocator::ShaderOutcome outcome =
        m_poses.outcomeOf(shaderBaseHash, wantAux);
    title::JsonBody counted;
    counted.number("assemblies", outcome.assemblies);
    counted.number("tooShortForAPose", outcome.tooShort);
    counted.number("unidentified", outcome.unidentified);
    counted.number("unscanned", outcome.unscanned);
    counted.number("scanned", outcome.scanned);
    counted.number("scannedWithoutBlocks", outcome.scannedWithoutBlocks);
    counted.number("largestUniformBytes", outcome.largestBytes);
    counted.raw("uniformBytesAgree", outcome.sizesAgree ? "true" : "false");
    // The threshold the size is read against, stated so a reader does not have to know it: a
    // shader below this cannot be applying a 3x4 from its uniforms, whatever it binds.
    counted.number("poseBytesWanted",
                   static_cast<uint64_t>(title::ObjectPoseLocator::kPoseWords) * sizeof(float));
    counted.raw("canHoldAPose",
                outcome.largestBytes >= title::ObjectPoseLocator::kPoseWords * sizeof(float)
                    ? "true"
                    : "false");
    body.object("assemblyOutcome", counted.text());
    const auto found = m_poses.found();
    title::JsonBody list;
    size_t index = 0;
    uint64_t considered = 0;
    for (const title::ObjectPoseLocator::Found& one : found) {
        ++considered;
        if (one.shaderBaseHash != shaderBaseHash) {
            continue;
        }
        // **An aux hash of zero asks about every aux hash of this base hash.** The census
        // keys its candidates on the pair, so matching the pair exactly answered "nothing" for
        // any shader whose aux hash the caller was never told -- and "nothing" is the one answer
        // a caller cannot tell from a real absence.
        if (shaderAuxHash != 0 && one.shaderAuxHash != shaderAuxHash) {
            continue;
        }
        title::PoseByShader::Entry entry;
        entry.byteOffset = one.byteOffset;
        entry.otherObjects = one.otherObjects;
        entry.otherObjectsSame = one.otherObjectsSame;
        entry.moved = one.moved;
        entry.compared = one.compared;
        // **The table's own refusal for this candidate, asked of the table.** Not a restatement of
        // the counts: a reader wants to know what the blend would do with this offset, and the
        // class that would do it is the one that says.
        title::JsonBody one_body;
        one_body.number("offset", one.byteOffset);
        one_body.number("moved", one.moved);
        one_body.number("compared", one.compared);
        one_body.number("otherObjects", one.otherObjects);
        one_body.number("otherObjectsSame", one.otherObjectsSame);
        one_body.string("refusal", m_poseByShader.refused(entry));
        list.object(std::to_string(index), one_body.text());
        ++index;
    }
    body.number("candidatesForThisShader", index);
    body.number("candidatesConsidered", considered);
    body.object("offsets", list.text());
    return body.finish();
}

std::string ControlChannel::poseReport() const {
    title::JsonBody body;
    title::JsonBody table;
    m_poseByShader.writeTo(table);
    title::JsonBody blend;
    m_poseBlend.writeTo(blend);
    body.object("table", table.finish());
    body.object("blend", blend.finish());
    return body.finish();
}

namespace {} // namespace

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

namespace {

std::string countsJson(const frame::VertexChanges::Counts& counts) {
    std::string body = ",\"draws\":" + std::to_string(counts.draws);
    body += ",\"compared\":" + std::to_string(counts.compared);
    body += ",\"changed\":" + std::to_string(counts.changed);
    return body + ",\"bytesHashed\":" + std::to_string(counts.bytesHashed);
}

std::string shaderJson(const frame::VertexChanges::VertexShader& shader) {
    return "\"baseHash\":" + std::to_string(shader.baseHash) +
           ",\"auxHash\":" + std::to_string(shader.auxHash);
}

std::string vertexChangesJson(
    const std::map<frame::VertexChanges::VertexShader, frame::VertexChanges::Counts>& byShader) {
    std::string body = "[";
    for (const auto& [shader, counts] : byShader) {
        body += body.size() == 1 ? "{" : ",{";
        body += shaderJson(shader) + countsJson(counts) + "}";
    }
    return body + "]";
}

std::string attributeChangesJson(
    const std::map<frame::VertexChanges::Attribute, frame::VertexChanges::Counts>& byAttribute) {
    std::string body = "[";
    for (const auto& [attribute, counts] : byAttribute) {
        body += body.size() == 1 ? "{" : ",{";
        body += shaderJson(attribute.shader);
        body += ",\"semanticId\":" + std::to_string(attribute.semanticId);
        body += ",\"format\":" + std::to_string(attribute.format) + countsJson(counts) + "}";
    }
    return body + "]";
}

} // namespace

std::string ControlChannel::drawsJson() const {
    std::string body = "{\"guestDrawsPrepared\":" + std::to_string(m_recorder.guestDrawsPrepared());
    body += ",\"withoutVertexUniforms\":" + vertexChangesJson(m_vertexChanges.withoutUniforms());
    frame::VertexChanges::Census census = m_vertexChanges.census();
    body += ",\"census\":{\"framesAsked\":" + std::to_string(census.framesAsked);
    body += ",\"framesTaken\":" + std::to_string(census.framesTaken);
    body += ",\"withVertexUniforms\":" + vertexChangesJson(census.byShader);
    body += ",\"attributes\":" + attributeChangesJson(census.byAttribute);
    return body + "}}\n";
}

// The slot a capture route is talking about. Out of range is refused by the
// capture itself rather than clamped, so a typo does not silently read the
// wrong image.
size_t ControlChannel::requestedSlot(const std::string& query) {
    std::string_view rest(query);
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
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
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
        if (key == name) {
            return !(value == "0" || value == "false");
        }
    }
    return fallback;
}

bool ControlChannel::requestedHashes(const std::string& query, std::string_view name,
                                     std::vector<uint64_t>& hashes) {
    std::string_view rest(query);
    hashes.clear();
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
        if (key != name) {
            continue;
        }
        // **`0x` in front is optional, and it is optional because every hash in this project is
        // written with it.** The length was the gate and it counted the prefix, so `?shader=0x…`
        // was refused where `?shader=…` was accepted -- a diagnostic that accepts one spelling of a
        // hash is read as broken, and `GET /memory` had the same fault and had it fixed there. The
        // sixteen digits are still required: a hash of another length is a different number, not a
        // spelling of this one.
        if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
            value.remove_prefix(2);
        }
        uint64_t parsed = 0;
        auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed, 16);
        if (value.size() != 16 || error != std::errc{} || end != value.data() + value.size()) {
            return false;
        }
        hashes.push_back(parsed);
    }
    return true;
}

size_t ControlChannel::requestedCount(const std::string& query, std::string_view name,
                                      size_t fallback) {
    std::string_view rest(query);
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
        if (key != name) {
            continue;
        }
        // Anything that is not a plain decimal count is zero, which every
        // caller refuses, rather than whatever prefix happened to parse.
        size_t count = 0;
        for (char digit : value) {
            if (digit < '0' || digit > '9' || count > 1000000) {
                return 0;
            }
            count = (count * 10) + static_cast<size_t>(digit - '0');
        }
        return value.empty() ? 0 : count;
    }
    return fallback;
}

// A text-valued query parameter, beside the count and flag ones because the range a scan is pointed
// at is a name and a name is not a number. The same parameter walk, and an empty value is the
// fallback rather than a blank: a caller that asked for a range with no name meant the default, and
// a caller that meant nothing at all would not have sent the parameter.
std::string ControlChannel::requestedText(const std::string& query, std::string_view name,
                                          std::string fallback) {
    std::string_view rest(query);
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
        if (key != name) {
            continue;
        }
        return value.empty() ? fallback : std::string(value);
    }
    return fallback;
}

float ControlChannel::requestedBlend(const std::string& query, float fallback) {
    std::string_view rest(query);
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
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
    uint32_t reads = kDefaultPressReads;
    uint32_t mask = 0;
    auto unknown = std::string();
    auto releasing = false;
    auto haveLeftStick = false;
    auto haveRightStick = false;
    input::StickPosition left;
    input::StickPosition right;
    while (auto parameter = nextParameter(rest)) {
        auto [key, value] = *parameter;
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
                left.x = number;
                haveLeftStick = true;
            } else if (key == "lefty") {
                left.y = number;
                haveLeftStick = true;
            } else if (key == "rightx") {
                right.x = number;
                haveRightStick = true;
            } else {
                right.y = number;
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
        m_input.setLeftStick(left);
        accepted = true;
    }
    if (haveRightStick) {
        m_input.setRightStick(right);
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
            return dispatch(request);
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

std::string_view ControlChannel::routeList() {
    return kRoutes;
}

// The routing table itself, as a member rather than a lambda inside start().
// A route table a test cannot ask is a route table whose mistakes only surface
// in a run that needs the route -- nine minutes in, with the title pressed
// through a menu. Here it is asked about every route it advertises, by name.
lucent::http::Response ControlChannel::dispatch(const lucent::http::Request& request) {
    // Arming is a deliberate one-shot: the next frame to end is
    // replayed, and nothing after it. A replay that repeated every
    // frame would make a crash impossible to attribute.
    if (request.method == "POST" && request.path() == "/paint") {
        if (!requestedFlag(std::string(request.query()), "on", true)) {
            std::string refusal = m_paint.disable();
            if (!refusal.empty()) {
                return lucent::http::Response::text(409, "Conflict", refusal + "\n");
            }
            return lucent::http::Response::json(200, "OK", m_paint.json());
        }
        // **The cast is stated, because `requestedCount` counts and a count is a `size_t`.** It was
        // narrowed into the signed type `modeFrom` takes without a word, which is a conversion fine
        // for every count a title can make and a defect in the reader.
        const auto asked =
            static_cast<long long>(requestedCount(std::string(request.query()), "mode", 3));
        const std::optional<title::WindWakerPaint::Mode> mode =
            title::WindWakerPaint::modeFrom(asked);
        if (!mode.has_value()) {
            return lucent::http::Response::text(
                400, "Bad Request",
                "mode " + std::to_string(asked) +
                    " is not 1 (paint once), "
                    "2 (paint twice) or 3 (paint twice at one vblank a flip)\n");
        }
        std::string refusal = m_paint.enable(*mode);
        if (!refusal.empty()) {
            return lucent::http::Response::text(409, "Conflict", refusal + "\n");
        }
        return lucent::http::Response::json(200, "OK", m_paint.json());
    }
    // Holds the title between frames: pause=1 holds at the next
    // frame's end, step=N lets N more end and answers once it holds
    // again, resume=1 lets it run.
    if (request.method == "POST" && request.path() == "/gate") {
        std::string query(request.query());
        if (requestedFlag(query, "resume", false)) {
            m_gate.resume();
            return lucent::http::Response::json(200, "OK", gateJson(m_gate.status()));
        }
        size_t steps = requestedCount(query, "step", 0);
        if (steps > 0) {
            m_gate.step(steps);
        } else if (requestedFlag(query, "pause", false)) {
            m_gate.pause();
        } else {
            return lucent::http::Response::text(
                400, "Bad Request", "name pause=1, step=N (a count above zero) or resume=1.\n");
        }
        if (!m_gate.awaitHeld(kGateHoldTimeout)) {
            return lucent::http::Response::json(504, "Gateway Timeout", gateJson(m_gate.status()));
        }
        return lucent::http::Response::json(200, "OK", gateJson(m_gate.status()));
    }
    // The logic gate, beside the frame gate it shares a name with and
    // does not: `/gate` holds the title between frames, `/logic` counts
    // its ticks. It lives above the `method != "GET"` refusal below,
    // because a POST handler under that refusal is a route the refusal
    // itself advertises and no request can reach.
    if (request.method == "POST" && request.path() == "/logic") {
        const bool wanted = requestedFlag(std::string(request.query()), "on", true);
        // `through=1` installs the pass-through control: a payload of one word
        // that branches back to the tick and keeps no state, so a tick rate that
        // holds says a direct branch into the arena executed and a tick rate that
        // stops says it did not -- with the census as the observer, so nothing in
        // the answer rests on the gate's own payload or its own counters.
        const bool through = requestedFlag(std::string(request.query()), "through", false);
        const int flavour =
            static_cast<int>(requestedCount(std::string(request.query()), "flavour", 2));
        const std::string refusal = wanted ? m_logic.enable(through, flavour) : m_logic.disable();
        if (!refusal.empty()) {
            return lucent::http::Response::text(409, "Conflict", refusal + "\n");
        }
        return lucent::http::Response::json(200, "OK", m_logic.json());
    }
    // Frame pacing measured from here on, so a walk is not averaged
    // with the boot and menus before it.
    if (request.method == "POST" && request.path() == "/pacing") {
        m_pacing.restart();
        m_scanOut.restart();
        return lucent::http::Response::json(200, "OK", pacingJson());
    }
    // A census of the next frames' draws that read uniforms, by
    // whether their vertex bytes change; GET /draws reads it back.
    if (request.method == "POST" && request.path() == "/draws") {
        size_t frames = requestedCount(std::string(request.query()), "frames", 1);
        if (frames == 0 || frames > frame::VertexChanges::kMaxCensusFrames) {
            return lucent::http::Response::text(
                400, "Bad Request",
                "frames must be a count from 1 to " +
                    std::to_string(frame::VertexChanges::kMaxCensusFrames) + ".\n");
        }
        m_vertexChanges.requestCensus(static_cast<uint32_t>(frames));
        return lucent::http::Response::json(200, "OK", drawsJson());
    }
    // Several consecutive frames' uniform assemblies, filled at the
    // frame boundaries that follow; GET /recordings reads them back.
    if (request.method == "POST" && request.path() == "/recordings") {
        size_t frames = requestedCount(std::string(request.query()), "frames", 2);
        if (!m_snapshot.arm(frames)) {
            return lucent::http::Response::text(
                409, "Conflict",
                "a snapshot is already filling, or frames is zero or above " +
                    std::to_string(frame::RecordingSnapshot::kMaxFrames) + ".\n");
        }
        return lucent::http::Response::json(
            200, "OK",
            "{\"armed\":true,\"frames\":" + std::to_string(frames) + ",\"snapshotsCompleted\":" +
                std::to_string(m_snapshot.snapshotsCompleted()) + "}\n");
    }

    // A present the runtime owns, so a replay's output can be seen
    // instead of being overdrawn by the guest's next frame. Refused
    // when the title has not presented yet, because the arguments
    // are observed and never invented.
    if (request.method == "POST" && request.path() == "/capture") {
        // `count` asks for that many *consecutive* presents rather than one. Two
        // arms of one is not two presents: the renderer's screenshot request is
        // one at a time, so the second arm waits a whole frame, and in a title
        // that animates that is a different picture. Comparing two consecutive
        // presents -- the two paints of a stand-in that paints twice -- is the
        // null case, and it needs this.
        const size_t count = requestedCount(std::string(request.query()), "count", 1);
        auto armed = count <= 1
                         ? m_capture.armOnce(requestedSlot(std::string(request.query())))
                         : m_capture.armRun(count, requestedSlot(std::string(request.query())));
        return lucent::http::Response::json(
            armed ? 200 : 503, armed ? "OK" : "Service Unavailable",
            std::string("{\"armed\":") + (armed ? "true" : "false") +
                ",\"imagesReceived\":" + std::to_string(m_capture.imagesReceived()) + "}\n");
    }
    if (request.method == "POST" && request.path() == "/quit") {
        auto accepted = false;
        auto body = requestHostStop(accepted);
        return lucent::http::Response::json(accepted ? 202 : 409,
                                            accepted ? "Accepted" : "Conflict", body);
    }
    // **Above the `method != "GET"` barrier below, which is the whole placement rule for a POST
    // route in this dispatcher and the one thing about it that is not obvious.** A POST route
    // written into the GET section is unreachable and answers "unknown route" for ever, and the
    // route table lists it, so the table and the dispatcher disagree while both look right. The
    // route table test caught exactly that when this route was first added.
    if (request.method == "POST" && request.path() == "/global-pose") {
        // On request rather than per frame, because it holds two snapshots of 3 MB and waits for a
        // frame between them. A route that ran on the display thread would spend milliseconds of a
        // sixty-hertz budget to answer a question asked once.
        std::string refusal;
        // The range is named rather than fixed, because the first range was measured to hold
        // nothing that moves and a scan that cannot be pointed elsewhere cannot answer the next
        // question. A name the code does not have is a refusal, not a default.
        // **A range the caller names, or one of the two this class holds in a table.** The two
        // named ranges have fixed addresses; the uniform block a draw sources does not, because it
        // is wherever the title's binder put it, and the measurement that unblocked the blend needs
        // exactly those. So `start=<hex>&bytes=<decimal>` scans a range the evidence named, bounded
        // and refused by reason, and `range=<token>` scans one of the constants. A request that
        // names neither scans nothing and says so, rather than scanning the module's data because
        // that is what a missing parameter defaulted to once.
        const std::string query(request.query());
        title::GlobalPoseCensus::Range wanted =
            title::GlobalPoseCensus::rangeByName(requestedText(query, "range", ""));
        if (!wanted.end) {
            const auto named = title::GlobalPoseCensus::namedRange(
                requestedText(query, "start", ""), requestedText(query, "bytes", ""));
            if (!named.refusal.empty()) {
                return lucent::http::Response::text(409, "Conflict", named.refusal + "\n");
            }
            wanted = named.range;
        }
        m_globalPose.scan(refusal, wanted);
        if (!refusal.empty()) {
            return lucent::http::Response::text(409, "Conflict", refusal + "\n");
        }
        return lucent::http::Response::json(200, "OK", m_globalPose.json());
    }
    // **Where a draw's pose is, per shader, and whether any draw was blended.** `POST /pose` feeds
    // the table from the census's own candidates -- the offsets the measurement gave, with the
    // counts that decided them -- and `GET /pose` reads the table and the blend beside it. **Two
    // routes and not one, because filling a table on a read is a mutation a reader of the report
    // would not expect**: a `GET` here answers what the table holds and nothing else, and the
    // census is the only thing that can change it.
    if (request.path() == "/pose" && request.method == "GET") {
        // **A named shader answers for itself and not for the table.** The report's table lists
        // eight candidates out of 285, so "why is my shader not in it" was a question the
        // instrument could not answer -- and that is worse than a negative measurement.
        std::vector<uint64_t> wanted;
        if (requestedHashes(std::string(request.query()), "shader", wanted) && !wanted.empty()) {
            return lucent::http::Response::json(200, "OK", poseForShader(wanted.front(), 0));
        }
        return lucent::http::Response::json(200, "OK", poseReport());
    }
    if (request.path() == "/pose" && request.method == "POST") {
        // **Fed from the census's own candidates, as the structure.** Reading its rendered report
        // back into a class would be a second implementation of the fields the report already has,
        // and a second one that could disagree with it.
        //
        // **A named `shader` narrows the feed to that shader's own candidates, and the narrowing is
        // the point.** The census is cumulative and `PoseByShader::offer` evaluates the counts it
        // is given, so a candidate refused on first sight -- because nothing had been compared
        // against it yet -- can be admitted by offering it again once it has drawn enough. The
        // blend's own unplaced-shader list names which shaders need that, so the caller holding the
        // measurement asks for exactly those rather than the whole census every time.
        std::vector<uint64_t> only;
        const bool narrowed =
            requestedHashes(std::string(request.query()), "shader", only) && !only.empty();
        const auto found = m_poses.found();
        std::vector<title::PoseByShader::Offered> offered;
        for (const title::ObjectPoseLocator::Found& one : found) {
            if (narrowed && std::none_of(only.begin(), only.end(), [&one](uint64_t hash) {
                    return hash == one.shaderBaseHash;
                })) {
                continue;
            }
            offered.push_back(title::PoseByShader::Offered{
                one.shaderBaseHash, one.shaderAuxHash, one.byteOffset, one.otherObjects,
                one.otherObjectsSame, one.moved, one.compared});
        }
        m_poseByShader.feed(offered.data(), offered.size());
        // The blend's counts are in the same document, so a caller that armed the table sees in one
        // read what it armed and what has happened since.
        return lucent::http::Response::json(200, "OK", poseReport());
    }
    if (request.method == "POST" && request.path() == "/input") {
        auto accepted = false;
        auto body = applyInput(std::string(request.query()), accepted);
        return lucent::http::Response::json(accepted ? 200 : 400, accepted ? "OK" : "Bad Request",
                                            body);
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
    if (request.path() == "/draws") {
        return lucent::http::Response::json(200, "OK", drawsJson());
    }
    if (request.path() == "/gate") {
        return lucent::http::Response::json(200, "OK", gateJson(m_gate.status()));
    }
    if (request.path() == "/recordings") {
        std::string framed = m_snapshot.framed();
        if (framed.empty()) {
            return lucent::http::Response::text(
                404, "Not Found",
                "no snapshot has completed yet. Arm one with POST /recordings?frames=K "
                "and let K frames end.\n");
        }
        return lucent::http::Response::binary(200, "OK", "application/octet-stream", framed);
    }
    if (request.path() == "/view") {
        return lucent::http::Response::json(200, "OK", m_viewBlend.json());
    }
    if (request.path() == "/quads") {
        return lucent::http::Response::json(200, "OK", m_quadBlend.json());
    }
    if (request.path() == "/callers") {
        return lucent::http::Response::json(200, "OK", m_callers.json());
    }
    if (request.method == "GET" && request.path() == "/paint") {
        return lucent::http::Response::json(200, "OK", m_paint.json());
    }
    if (request.path() == "/blocks") {
        return lucent::http::Response::json(200, "OK", m_blocks.json());
    }
    // The logic gate is `/logic` and not `/gate`: the frame gate has held
    // that route since it existed, and two things meaning "hold" by the
    // same name is how one of them ends up answering for the other.
    if (request.method == "GET" && request.path() == "/logic") {
        return lucent::http::Response::json(200, "OK", m_logic.json());
    }
    if (request.path() == "/memory") {
        std::string refusal;
        auto wanted = GuestMemoryRead::parse(request.query(), refusal);
        if (!wanted.has_value()) {
            return lucent::http::Response::text(400, "Bad Request", refusal);
        }
        auto bytes = GuestMemoryRead::read(*wanted, m_guestBytes);
        if (!bytes.has_value()) {
            return lucent::http::Response::text(404, "Not Found",
                                                "some of that range is not guest memory.\n");
        }
        return lucent::http::Response::binary(200, "OK", "application/octet-stream", *bytes);
    }
    return notFound();
}

bool ControlChannel::running() const {
    return m_server != nullptr;
}

uint16_t ControlChannel::port() const {
    return m_server ? m_server->port() : 0;
}

} // namespace wiiuport::control
