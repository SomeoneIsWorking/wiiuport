#include "wiiuport/control/ControlChannel.h"

#include <lucent/http.h>
#include <lucent/log.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wiiuport::control {
namespace {

lucent::http::Response notFound() {
    return lucent::http::Response::text(
        404, "Not Found",
        "unknown route. This channel serves GET /counters, GET /transforms, GET /capture, "
        "GET /controllers, GET /setup, GET /substitution, GET /frames, GET /interpolation, "
        "GET /recordings, GET /objects, GET /draws, POST /replay, POST /capture, POST /present, "
        "POST /nulldiff, POST /interpolate, POST /continuous, POST /restorecheck, POST /pacing, "
        "POST /objects, POST /draws, POST /recordings and POST /input.\n");
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

ControlChannel::ControlChannel(const Sources& sources)
    : m_recorder(sources.recorder), m_replayer(sources.replayer), m_search(sources.search),
      m_input(sources.input), m_capture(sources.capture), m_presenter(sources.presenter),
      m_scheduler(sources.scheduler), m_interpolator(sources.interpolator),
      m_shapeLog(sources.shapeLog), m_viewTracker(sources.viewTracker),
      m_continuous(sources.continuous), m_restoreCheck(sources.restoreCheck),
      m_objects(sources.objects), m_vertices(sources.vertices), m_snapshot(sources.snapshot),
      m_pacing(sources.pacing), m_vertexChanges(sources.vertexChanges) {
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
    body += ",\"guestDrawsPrepared\":" + std::to_string(m_recorder.guestDrawsPrepared());
    body += ",\"guestDrawsWithoutVertexUniforms\":" +
            std::to_string(m_recorder.guestDrawsWithoutVertexUniforms());
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

// Objects by what they came to, every outcome named, zero or not.
std::string outcomesJson(const interp::ObjectPlanner::Outcomes& outcomes) {
    std::string body = "{";
    for (size_t index = 0; index < interp::ObjectPlanner::kOutcomeCount; ++index) {
        auto outcome = static_cast<interp::ObjectPlanner::Outcome>(index);
        body += index == 0 ? "\"" : ",\"";
        body += std::string(interp::ObjectPlanner::outcomeName(outcome)) +
                "\":" + std::to_string(outcomes[index]);
    }
    return body + "}";
}

std::string censusJson(const interp::ObjectCensus& census) {
    std::string body = "{\"frames\":" + std::to_string(census.frames);
    body += ",\"objects\":" + std::to_string(census.objects);
    body += ",\"outcomes\":" + outcomesJson(census.outcomes);
    body += ",\"shaders\":" + std::to_string(census.shaders);
    body += ",\"rows\":[";
    for (size_t i = 0; i < census.rows.size(); ++i) {
        const interp::ObjectCensus::Row& row = census.rows[i];
        body += i == 0 ? "{" : ",{";
        body += "\"stageIndex\":" + std::to_string(row.shader.stageIndex);
        body += ",\"baseHash\":" + std::to_string(row.shader.baseHash);
        body += ",\"auxHash\":" + std::to_string(row.shader.auxHash);
        body += ",\"draws\":" + std::to_string(row.draws);
        body += ",\"outcomes\":" + outcomesJson(row.outcomes);
        body += ",\"mostValues\":" + std::to_string(row.mostValues);
        body += ",\"withoutBlocks\":" + std::to_string(row.withoutBlocks);
        body += "}";
    }
    return body + "]}\n";
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

std::string ControlChannel::verticesJson() const {
    std::string body = ",\"vertexDraws\":{";
    for (size_t index = 0; index < interp::kVertexOutcomeCount; ++index) {
        auto outcome = static_cast<interp::VertexOutcome>(index);
        body += index == 0 ? "\"" : ",\"";
        body += std::string(interp::vertexOutcomeName(outcome)) +
                "\":" + std::to_string(m_vertices.draws(outcome));
    }
    body += "}";
    body += ",\"runtimeDrawsReplaceable\":" + std::to_string(m_recorder.runtimeDrawsReplaceable());
    body += ",\"runtimeDrawsReplaced\":" + std::to_string(m_recorder.runtimeDrawsReplaced());
    body += ",\"vertexReplaysDiverged\":" + std::to_string(m_vertices.replaysDiverged());
    body += ",\"vertexReplaysUnaligned\":" + std::to_string(m_vertices.replaysUnaligned());
    body += ",\"vertexBytesCopied\":" + std::to_string(m_vertices.bytesCopied());
    body += ",\"vertexCopyingNanoseconds\":" + std::to_string(m_vertices.copying().count());
    body += ",\"vertexBlendingNanoseconds\":" + std::to_string(m_vertices.blending().count());
    body += ",\"vertexWaitingNanoseconds\":" + std::to_string(m_vertices.waiting().count());
    return body;
}

std::string ControlChannel::interpolationJson() const {
    using Skip = interp::ContinuousInterpolator::Skip;
    std::string body = "{";
    body += "\"enabled\":" + std::string(m_continuous.enabled() ? "true" : "false");
    body += ",\"ticks\":" + std::to_string(m_continuous.ticks());
    body += ",\"framesInterpolated\":" + std::to_string(m_continuous.framesInterpolated());
    body += ",\"skipped\":{";
    for (size_t index = 0; index < interp::ContinuousInterpolator::kSkipCount; ++index) {
        auto skip = static_cast<Skip>(index);
        body += index == 0 ? "\"" : ",\"";
        body += std::string(interp::ContinuousInterpolator::skipName(skip)) +
                "\":" + std::to_string(m_continuous.skipped(skip));
    }
    body += "}";
    body += ",\"restoresRefused\":" + std::to_string(m_continuous.restoresRefused());
    body += ",\"restoresByCopy\":" + std::to_string(m_continuous.restoresByCopy());
    body += ",\"restoresByReplay\":" + std::to_string(m_continuous.restoresByReplay());
    body += ",\"subresourcesRestored\":" + std::to_string(m_continuous.subresourcesRestored());
    body += ",\"shadowsCreated\":" + std::to_string(m_continuous.shadowsCreated());
    body += ",\"restoreChecksCompleted\":" + std::to_string(m_restoreCheck.completed());
    body += ",\"restoreChecksRefused\":" + std::to_string(m_restoreCheck.refused());
    const interp::ContinuousInterpolator::NotCopied& notCopied = m_continuous.notCopied();
    body += ",\"notCopied\":{\"subresources\":" + std::to_string(notCopied.subresources) +
            ",\"texturesCreated\":" + std::to_string(notCopied.texturesCreated) +
            ",\"streamoutWrites\":" + std::to_string(notCopied.streamoutWrites) + "}";
    body += ",\"phaseNanoseconds\":{";
    for (size_t index = 0; index < interp::ContinuousInterpolator::kPhaseCount; ++index) {
        auto phase = static_cast<interp::ContinuousInterpolator::Phase>(index);
        body += index == 0 ? "\"" : ",\"";
        body += std::string(interp::ContinuousInterpolator::phaseName(phase)) +
                "\":" + std::to_string(m_continuous.timeIn(phase).count());
    }
    body += "}";
    body += ",\"cutsByTurn\":" + std::to_string(m_continuous.cuts().cutsByTurn());
    body += ",\"cutsByStep\":" + std::to_string(m_continuous.cuts().cutsByStep());
    const interp::ObjectBlend& objects = m_objects;
    body += ",\"objects\":" + outcomesJson(objects.outcomes());
    body += ",\"objectPartnersDerived\":" + std::to_string(objects.planner().partnersDerived());
    body += ",\"objectPartnersSearched\":" + std::to_string(objects.planner().partnersSearched());
    body += ",\"objectPartnersReidentified\":" +
            std::to_string(objects.planner().partnersReidentified());
    body +=
        ",\"objectHeldPartnersDerived\":" + std::to_string(objects.planner().heldPartnersDerived());
    body += ",\"objectSearchesDeferred\":" + std::to_string(objects.planner().searchesDeferred());
    body +=
        ",\"objectReidentifyAttempts\":" + std::to_string(objects.planner().reidentifyAttempts());
    body += ",\"objectNearestCandidates\":" + std::to_string(objects.planner().nearestCandidates());
    body += ",\"objectPartnerCandidates\":" + std::to_string(objects.planner().partnerCandidates());
    body += ",\"objectValuesNotBlended\":" + std::to_string(objects.planner().valuesNotBlended());
    body += ",\"objectValuesAlternating\":" + std::to_string(objects.planner().valuesAlternating());
    body += ",\"objectDrawsWritten\":" + std::to_string(objects.drawsWritten());
    body += ",\"objectReplaysDiverged\":" + std::to_string(objects.replaysDiverged());
    body += ",\"objectFramesEnded\":" + std::to_string(objects.framesEnded());
    body += ",\"objectPlanningBusyNanoseconds\":" + std::to_string(objects.planningBusy().count());
    body += ",\"objectFrameEndPlanningNanoseconds\":" +
            std::to_string(objects.frameEndPlanning().count());
    body += verticesJson();
    frame::PresentPacing::Summary pacing = m_pacing.summary();
    body += ",\"pacing\":{\"guestFrames\":" + std::to_string(pacing.guestFrames);
    body += ",\"runtimeFrames\":" + std::to_string(pacing.runtimeFrames);
    body += ",\"intervals\":" + std::to_string(pacing.intervals);
    body += ",\"intervalsKept\":" + std::to_string(pacing.intervalsKept);
    body += ",\"p50Us\":" + std::to_string(pacing.p50.count());
    body += ",\"p95Us\":" + std::to_string(pacing.p95.count());
    body += ",\"p99Us\":" + std::to_string(pacing.p99.count());
    body += ",\"longestUs\":" + std::to_string(pacing.longest.count());
    body += ",\"guestToRuntimeMedianUs\":" + std::to_string(pacing.guestToRuntimeMedian.count());
    body += ",\"runtimeToGuestMedianUs\":" + std::to_string(pacing.runtimeToGuestMedian.count());
    body += "}";
    body += ",\"viewFramesTracked\":" + std::to_string(m_viewTracker.framesTracked());
    body += ",\"viewFramesLost\":" + std::to_string(m_viewTracker.framesLost());
    body += ",\"viewReseedsRun\":" + std::to_string(m_viewTracker.reseedsRun());
    body += ",\"viewReseedsFound\":" + std::to_string(m_viewTracker.reseedsFound());
    body += ",\"copiesSubmitted\":" + std::to_string(m_presenter.copiesSubmitted());
    body += ",\"withheld\":{";
    for (uint32_t index = 0; index < LatteFrameHooks::kWithheldEffectCount; ++index) {
        auto effect = static_cast<LatteFrameHooks::WithheldEffect>(index);
        body += index == 0 ? "\"" : ",\"";
        body += std::string(withheldName(effect)) +
                "\":" + std::to_string(m_recorder.runtimeWithheld(effect));
    }
    body += "}";
    body += ",\"recordingSnapshots\":" + std::to_string(m_snapshot.snapshotsCompleted());
    return body + "}\n";
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

size_t ControlChannel::requestedCount(const std::string& query, std::string_view name,
                                      size_t fallback) {
    std::string_view rest(query);
    std::string_view key;
    std::string_view value;
    while (nextParameter(rest, key, value)) {
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
            if (request.method == "POST" && request.path() == "/continuous") {
                bool on = requestedFlag(std::string(request.query()), "on", true);
                m_continuous.setEnabled(on);
                return lucent::http::Response::json(200, "OK", interpolationJson());
            }
            // Frame pacing measured from here on, so a walk is not averaged
            // with the boot and menus before it.
            if (request.method == "POST" && request.path() == "/pacing") {
                m_pacing.restart();
                return lucent::http::Response::json(200, "OK", interpolationJson());
            }
            // The guest's frame captured before and after the next in-between
            // frame is drawn over it and taken back out, into capture slots 0
            // and 1; with inbetween=1, slot 1 is the in-between frame instead,
            // which is the comparison's control.
            if (request.method == "POST" && request.path() == "/restorecheck") {
                bool inBetween = requestedFlag(std::string(request.query()), "inbetween", false);
                if (!m_restoreCheck.arm(inBetween ? interp::RestoreCheck::Against::InBetween
                                                  : interp::RestoreCheck::Against::Restored)) {
                    return lucent::http::Response::text(
                        409, "Conflict", "a restore check is already waiting for its tick.\n");
                }
                return lucent::http::Response::json(200, "OK", interpolationJson());
            }
            // A census of the next planned frames' objects by shader; GET
            // /objects reads it back once they are all planned.
            if (request.method == "POST" && request.path() == "/objects") {
                size_t frames = requestedCount(std::string(request.query()), "frames", 1);
                if (frames == 0 || frames > interp::ObjectBlend::kMaxCensusFrames) {
                    return lucent::http::Response::text(
                        400, "Bad Request",
                        "frames must be a count from 1 to " +
                            std::to_string(interp::ObjectBlend::kMaxCensusFrames) + ".\n");
                }
                m_objects.requestCensus(static_cast<uint32_t>(frames));
                return lucent::http::Response::json(
                    200, "OK", "{\"requested\":true,\"frames\":" + std::to_string(frames) + "}\n");
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
                    "{\"armed\":true,\"frames\":" + std::to_string(frames) +
                        ",\"snapshotsCompleted\":" +
                        std::to_string(m_snapshot.snapshotsCompleted()) + "}\n");
            }
            bool oneShot = request.method == "POST" &&
                           (request.path() == "/replay" || request.path() == "/nulldiff" ||
                            request.path() == "/interpolate");
            if (oneShot && m_continuous.enabled()) {
                return continuousOwnsFrames();
            }
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
            if (request.path() == "/draws") {
                return lucent::http::Response::json(200, "OK", drawsJson());
            }
            if (request.path() == "/frames") {
                return lucent::http::Response::json(200, "OK", framesJson());
            }
            if (request.path() == "/interpolation") {
                return lucent::http::Response::json(200, "OK", interpolationJson());
            }
            if (request.path() == "/objects") {
                std::optional<interp::ObjectCensus> census = m_objects.census();
                if (!census) {
                    return lucent::http::Response::text(
                        404, "Not Found",
                        "no census has been taken yet. Request one with POST /objects while "
                        "continuous interpolation is planning.\n");
                }
                return lucent::http::Response::json(200, "OK", censusJson(*census));
            }
            if (request.path() == "/recordings") {
                std::string framed = m_snapshot.framed();
                if (framed.empty()) {
                    return lucent::http::Response::text(
                        404, "Not Found",
                        "no snapshot has completed yet. Arm one with POST /recordings?frames=K "
                        "and let K frames end.\n");
                }
                return lucent::http::Response::binary(200, "OK", "application/octet-stream",
                                                      framed);
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
