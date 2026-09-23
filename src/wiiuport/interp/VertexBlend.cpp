#include "wiiuport/interp/VertexBlend.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/interp/Midpoint.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <tuple>

namespace wiiuport::interp {

namespace {

// Latte's float vertex formats (LatteReg.h) and how many floats each holds.
std::optional<uint32_t> floatsIn(uint8_t format) {
    switch (format) {
    case 0x0E:
        return 1;
    case 0x1E:
        return 2;
    case 0x30:
        return 3;
    case 0x23:
        return 4;
    default:
        return std::nullopt;
    }
}

// LatteConst::VertexFetchEndianMode: the guest's own big-endian words, or
// words already in the host's order.
enum class Endian : uint8_t {
    None = 0,
    SwapU32 = 2
};

uint32_t swapBytes(uint32_t word) {
    return (word >> 24) | ((word >> 8) & 0x0000ff00u) | ((word << 8) & 0x00ff0000u) | (word << 24);
}

uint32_t readWord(const std::byte* at, Endian endian) {
    uint32_t word = 0;
    std::memcpy(&word, at, sizeof(word));
    return endian == Endian::SwapU32 ? swapBytes(word) : word;
}

void writeWord(std::byte* at, uint32_t word, Endian endian) {
    word = endian == Endian::SwapU32 ? swapBytes(word) : word;
    std::memcpy(at, &word, sizeof(word));
}

// Calls `visit(word, endian)` with the byte offset of every float value the
// layout's attributes read, in every vertex, for the attributes in a float
// format and a word order this reads.
template <typename Visit> void forEachFloat(const VertexLayout& layout, Visit&& visit) {
    // Where each buffer starts in the bytes, one after another.
    std::vector<size_t> starts;
    size_t start = 0;
    for (const VertexLayout::Buffer& buffer : layout.buffers) {
        starts.push_back(start);
        start += buffer.sizeInBytes;
    }
    for (const VertexLayout::Attribute& attribute : layout.attributes) {
        std::optional<uint32_t> floats = floatsIn(attribute.format);
        auto endian = static_cast<Endian>(attribute.endianSwap);
        if (!floats.has_value() || (endian != Endian::None && endian != Endian::SwapU32)) {
            continue;
        }
        const VertexLayout::Buffer& buffer = layout.buffers[attribute.buffer];
        size_t base = starts[attribute.buffer];
        for (uint64_t at = attribute.offset; at + attribute.sizeInBytes <= buffer.sizeInBytes;
             at += buffer.stride) {
            for (uint32_t component = 0; component < *floats; ++component) {
                visit(base + at + (component * sizeof(float)), endian);
            }
            if (buffer.stride == 0) {
                break;
            }
        }
    }
}

float readFloat(std::span<const std::byte> bytes, size_t word, Endian endian) {
    return std::bit_cast<float>(readWord(bytes.data() + word, endian));
}

// Strictly between two different values, or equal to both.
bool liesBetween(float a, float blended, float b) {
    if (a == b) {
        return blended == a;
    }
    return std::min(a, b) < blended && blended < std::max(a, b);
}

// How much one draw resembles another: the float values it holds bit for bit
// the same, then how far apart the rest lie, where numbers in both. An
// object keeps what it does not animate -- a texture cell -- where another of
// its kind beside it differs, so what it kept outranks how near it stands.
struct Resemblance {
    size_t same{0};
    double apartSquared{0.0};

    bool closerThan(const Resemblance& other) const {
        if (same != other.same) {
            return same > other.same;
        }
        return apartSquared < other.apartSquared;
    }
};

Resemblance resemblance(const VertexLayout& layout, std::span<const std::byte> first,
                        std::span<const std::byte> second) {
    Resemblance resemblance;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        uint32_t a = readWord(first.data() + word, endian);
        uint32_t b = readWord(second.data() + word, endian);
        if (a == b) {
            ++resemblance.same;
            return;
        }
        float x = std::bit_cast<float>(a);
        float y = std::bit_cast<float>(b);
        if (isNumber(x) && isNumber(y)) {
            double apart = static_cast<double>(x) - y;
            resemblance.apartSquared += apart * apart;
        }
    });
    return resemblance;
}

} // namespace

std::string_view vertexOutcomeName(VertexOutcome outcome) {
    switch (outcome) {
    case VertexOutcome::Blended:
        return "blended";
    case VertexOutcome::Unchanged:
        return "unchanged";
    case VertexOutcome::Held:
        return "held";
    case VertexOutcome::Unverified:
        return "unverified";
    case VertexOutcome::NoPartner:
        return "noPartner";
    case VertexOutcome::ShapeDiffers:
        return "shapeDiffers";
    case VertexOutcome::Outside:
        return "outside";
    case VertexOutcome::NotFloats:
        return "notFloats";
    case VertexOutcome::Count:
        break;
    }
    return "unknown";
}

VertexLayout VertexLayout::of(const LatteFrameHooks::DrawPrepared& draw) {
    VertexLayout layout;
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        layout.buffers.push_back(
            {draw.vertexBuffers[index].sizeInBytes, draw.vertexBuffers[index].stride});
    }
    for (uint32_t index = 0; index < draw.vertexAttributeCount; ++index) {
        const LatteFrameHooks::DrawPrepared::VertexAttribute& attribute =
            draw.vertexAttributes[index];
        layout.attributes.push_back({attribute.buffer, attribute.offset, attribute.sizeInBytes,
                                     attribute.format, attribute.endianSwap});
    }
    return layout;
}

VertexOutcome blendVertexBytes(const VertexLayout& layout, std::span<const std::byte> twoBack,
                               std::span<const std::byte> before, std::span<const std::byte> after,
                               float t, PartnerIdentity identity, std::span<std::byte> out) {
    std::copy(after.begin(), after.end(), out.begin());
    if (std::equal(before.begin(), before.end(), after.begin(), after.end())) {
        return VertexOutcome::Unchanged;
    }
    bool floatChanged = false;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        floatChanged = floatChanged || readWord(before.data() + word, endian) !=
                                           readWord(after.data() + word, endian);
    });
    if (!floatChanged) {
        return VertexOutcome::NotFloats;
    }
    Midpoint midpoint = vertexMidpoint(layout, twoBack, before, after);
    if (midpoint.moved() == 0) {
        return VertexOutcome::Held;
    }
    if (identity == PartnerIdentity::ByPlace && !midpoint.landsOn()) {
        return VertexOutcome::Unverified;
    }
    uint64_t blended = 0;
    bool outside = false;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        float a = readFloat(before, word, endian);
        float b = readFloat(after, word, endian);
        // Only what moved from N-2 to N: a value N-2 and N agree on is
        // flipping, not moving, whatever N-1 holds.
        if (outside || !Midpoint::movedIn(readFloat(twoBack, word, endian), b) || !isNumber(a) ||
            std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b)) {
            return;
        }
        float value = a + ((b - a) * t);
        if (!liesBetween(a, value, b)) {
            outside = true;
            return;
        }
        writeWord(out.data() + word, std::bit_cast<uint32_t>(value), endian);
        ++blended;
    });
    if (outside) {
        std::copy(after.begin(), after.end(), out.begin());
        return VertexOutcome::Outside;
    }
    return blended > 0 ? VertexOutcome::Blended : VertexOutcome::Unchanged;
}

Midpoint vertexMidpoint(const VertexLayout& layout, std::span<const std::byte> twoBack,
                        std::span<const std::byte> between, std::span<const std::byte> after) {
    Midpoint midpoint;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        midpoint.add(readFloat(twoBack, word, endian), readFloat(between, word, endian),
                     readFloat(after, word, endian));
    });
    return midpoint;
}

VertexBlend::VertexBlend(const ObjectBlend& objects, float t)
    : m_objects(objects), m_t(t), m_thread([this](std::stop_token stop) {
          blendHandedOver(stop);
      }) {
}

VertexBlend::~VertexBlend() {
    m_thread.request_stop();
}

void VertexBlend::Frame::clear() {
    draws.clear();
    bytes.clear();
    copied.clear();
    byEntry.clear();
    byShader.clear();
    frameIndex = 0;
    assemblies = 0;
    lastVertexEntry.reset();
    lastVertexShaderBaseHash = 0;
    lastVertexShaderAuxHash = 0;
}

void VertexBlend::onAssemblyRecorded(const frame::RecordedUniformAssembly& assembly) {
    if (assembly.stageIndex == 0) {
        m_building.lastVertexEntry = m_building.assemblies;
        m_building.lastVertexShaderBaseHash = assembly.shaderBaseHash;
        m_building.lastVertexShaderAuxHash = assembly.shaderAuxHash;
    }
    ++m_building.assemblies;
}

void VertexBlend::onDrawRecorded(const LatteFrameHooks::DrawPrepared& draw) {
    Draw recorded{draw.vertexShaderBaseHash,
                  draw.vertexShaderAuxHash,
                  m_building.assemblies,
                  std::nullopt,
                  0,
                  {},
                  {}};
    // Kept when the vertex-stage uniforms last assembled are this draw's
    // own: a draw without them has no object to take a partner from.
    bool ownUniforms = draw.vertexUniforms && m_building.lastVertexEntry.has_value() &&
                       m_building.lastVertexShaderBaseHash == draw.vertexShaderBaseHash &&
                       m_building.lastVertexShaderAuxHash == draw.vertexShaderAuxHash;
    if (ownUniforms && m_objects.isPlanning()) {
        auto started = std::chrono::steady_clock::now();
        recorded.vertexEntry = m_building.lastVertexEntry;
        recorded.layout = VertexLayout::of(draw);
        for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
            const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
            auto [copy, fresh] = m_building.copied.try_emplace(
                std::pair{buffer.data, buffer.sizeInBytes}, m_building.bytes.size());
            if (fresh) {
                const auto* bytes = static_cast<const std::byte*>(buffer.data);
                m_building.bytes.insert(m_building.bytes.end(), bytes, bytes + buffer.sizeInBytes);
                m_bytesCopied += buffer.sizeInBytes;
            }
            recorded.bufferStarts.push_back(copy->second);
        }
        auto [slot, fresh] = m_building.byEntry.try_emplace(std::pair{*recorded.vertexEntry, 0u},
                                                            m_building.draws.size());
        // Draws sharing one vertex-stage assembly take their place among it.
        while (!fresh) {
            ++recorded.ordinal;
            std::tie(slot, fresh) = m_building.byEntry.try_emplace(
                std::pair{*recorded.vertexEntry, recorded.ordinal}, m_building.draws.size());
        }
        m_building.byShader[{draw.vertexShaderBaseHash, draw.vertexShaderAuxHash}].push_back(
            m_building.draws.size());
        m_copying += std::chrono::steady_clock::now() - started;
    }
    m_building.draws.push_back(std::move(recorded));
}

void VertexBlend::onFrameRecorded(const frame::FrameRecording& /*recording*/) {
    // The frames it blends are about to move.
    waitUntilBlended();
    m_building.frameIndex = m_objects.framesEnded();
    std::swap(m_twoBack, m_previous);
    std::swap(m_previous, m_latest);
    std::swap(m_latest, m_building);
    m_building.clear();
    startBlending();
}

void VertexBlend::startBlending() {
    m_drawBlends.assign(m_latest.draws.size(), std::nullopt);
    m_jobs.clear();
    if (m_previous.frameIndex + 1 != m_latest.frameIndex ||
        m_twoBack.frameIndex + 2 != m_latest.frameIndex) {
        m_blendedThrough.store(m_latest.draws.size(), std::memory_order_release);
        return;
    }
    // The plan is the latest frame's until the next frame ends: read here,
    // not on the blending thread, which the next frame's end does not wait
    // for before planning goes on. A mesh is drawn alike by every draw that
    // reads it, the first with a partner deciding for all: passes over one
    // mesh drawn differently tear it, as a shadow volume's passes did.
    std::map<std::pair<std::vector<size_t>, VertexLayout>, Job> meshes;
    for (size_t index = 0; index < m_latest.draws.size(); ++index) {
        const Draw& drawn = m_latest.draws[index];
        if (!drawn.vertexEntry.has_value()) {
            continue;
        }
        std::variant<Job, VertexOutcome> planned = planDraw(index);
        if (const Job* job = std::get_if<Job>(&planned)) {
            meshes.try_emplace({drawn.bufferStarts, drawn.layout}, *job);
        } else {
            m_drawBlends[index] = PairBlend{std::get<VertexOutcome>(planned), 0};
        }
    }
    for (size_t index = 0; index < m_latest.draws.size(); ++index) {
        const Draw& drawn = m_latest.draws[index];
        if (!drawn.vertexEntry.has_value()) {
            continue;
        }
        auto mesh = meshes.find({drawn.bufferStarts, drawn.layout});
        if (mesh != meshes.end()) {
            Job job = mesh->second;
            job.draw = index;
            m_jobs.push_back(job);
            m_drawBlends[index].reset();
        }
    }
    if (m_jobs.empty()) {
        m_blendedThrough.store(m_latest.draws.size(), std::memory_order_release);
        return;
    }
    m_blendedThrough.store(0, std::memory_order_release);
    std::lock_guard lock(m_mutex);
    m_jobHandedOver = true;
    m_handedOver.notify_one();
}

std::variant<VertexBlend::Job, VertexOutcome> VertexBlend::planDraw(size_t index) const {
    const Draw& drawn = m_latest.draws[index];
    const ObjectPlanner& planner = m_objects.planner();
    std::optional<size_t> partnerEntry = planner.partnerOf(*drawn.vertexEntry);
    if (!partnerEntry.has_value()) {
        return VertexOutcome::NoPartner;
    }
    // A partner is planned only with the object's entry two frames back.
    std::optional<size_t> earlierEntry = planner.earlierOf(*drawn.vertexEntry);
    std::optional<size_t> partner = drawOf(m_previous, *partnerEntry, drawn);
    std::optional<size_t> earlier;
    if (earlierEntry.has_value()) {
        earlier = drawOf(m_twoBack, *earlierEntry, drawn);
    }
    if (!partner.has_value() || !earlier.has_value()) {
        return VertexOutcome::ShapeDiffers;
    }
    PartnerIdentity identity = planner.latest().sharesKey(*drawn.vertexEntry)
                                   ? PartnerIdentity::ByPlace
                                   : PartnerIdentity::ByBlocks;
    return Job{index, *partner, *earlier, identity};
}

std::optional<size_t> VertexBlend::drawOf(const Frame& frame, size_t entry, const Draw& drawn) {
    auto found = frame.byEntry.find({static_cast<uint32_t>(entry), drawn.ordinal});
    if (found == frame.byEntry.end() || frame.draws[found->second].layout != drawn.layout) {
        return std::nullopt;
    }
    return found->second;
}

void VertexBlend::blendHandedOver(std::stop_token stop) {
    std::unique_lock lock(m_mutex);
    while (m_handedOver.wait(lock, stop, [this] {
        return m_jobHandedOver;
    })) {
        lock.unlock();
        auto started = std::chrono::steady_clock::now();
        // Each distinct pair's place first, so nothing blended moves.
        m_pairs.clear();
        m_pairSlots.clear();
        m_jobPairs.clear();
        m_placed.clear();
        size_t bytes = 0;
        for (Job& job : m_jobs) {
            if (job.identity == PartnerIdentity::ByPlace) {
                placeByVertices(job);
            }
            const Draw& drawn = m_latest.draws[job.draw];
            const Draw& partner = m_previous.draws[job.partner];
            const Draw& earlier = m_twoBack.draws[job.earlier];
            auto [pair, fresh] =
                m_pairs.try_emplace(PairKey{earlier.bufferStarts, partner.bufferStarts,
                                            drawn.bufferStarts, drawn.layout, job.identity},
                                    m_pairSlots.size());
            if (fresh) {
                m_pairSlots.push_back({bytes, std::nullopt});
                for (const VertexLayout::Buffer& buffer : drawn.layout.buffers) {
                    bytes += buffer.sizeInBytes;
                }
            }
            m_jobPairs.push_back(pair->second);
        }
        if (m_blended.size() < bytes) {
            m_blended.resize(bytes);
        }
        for (size_t index = 0; index < m_jobs.size(); ++index) {
            const Job& job = m_jobs[index];
            PairSlot& slot = m_pairSlots[m_jobPairs[index]];
            if (!slot.outcome.has_value()) {
                slot.outcome =
                    blendPair(m_twoBack.draws[job.earlier], m_previous.draws[job.partner],
                              m_latest.draws[job.draw], job.identity, slot.start);
            }
            m_drawBlends[job.draw] = PairBlend{*slot.outcome, slot.start};
            m_blendedThrough.store(job.draw + 1, std::memory_order_release);
            m_blendedThrough.notify_all();
        }
        m_blendedThrough.store(m_latest.draws.size(), std::memory_order_release);
        m_blendedThrough.notify_all();
        m_blendingNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
        lock.lock();
        m_jobHandedOver = false;
        m_blendedAll.notify_all();
    }
}

void VertexBlend::waitUntilBlendedThrough(size_t index) {
    size_t through = m_blendedThrough.load(std::memory_order_acquire);
    while (through <= index) {
        m_blendedThrough.wait(through, std::memory_order_acquire);
        through = m_blendedThrough.load(std::memory_order_acquire);
    }
}

void VertexBlend::waitUntilBlended() {
    std::unique_lock lock(m_mutex);
    m_blendedAll.wait(lock, [this] {
        return !m_jobHandedOver;
    });
}

void VertexBlend::count(uint64_t shaderBaseHash, VertexOutcome outcome) {
    ++m_outcomes[static_cast<size_t>(outcome)];
    std::lock_guard lock(m_byShaderMutex);
    ++m_byShader[shaderBaseHash][static_cast<size_t>(outcome)];
}

std::vector<ShaderVertexOutcomes> VertexBlend::drawsByShader() const {
    std::lock_guard lock(m_byShaderMutex);
    std::vector<ShaderVertexOutcomes> shaders;
    shaders.reserve(m_byShader.size());
    for (const auto& [shader, draws] : m_byShader) {
        shaders.push_back({shader, draws});
    }
    return shaders;
}

std::span<const std::byte> VertexBlend::bufferBytes(const Frame& frame, const Draw& draw,
                                                    size_t buffer) const {
    return {frame.bytes.data() + draw.bufferStarts[buffer],
            draw.layout.buffers[buffer].sizeInBytes};
}

bool VertexBlend::onRuntimeDraw(const LatteFrameHooks::DrawPrepared& draw,
                                LatteFrameHooks::VertexReplacements& replacements) {
    if (!m_objects.isArmed() || !m_blendingEnabled.load()) {
        m_replayArmed = false;
        return false;
    }
    if (!m_replayArmed || m_replayFrame != m_objects.armings()) {
        // A new replay: of the latest frame, which must be the one the plan
        // is for, with the frame before it held too.
        m_replayArmed = true;
        m_replayFrame = m_objects.armings();
        m_replayDraw = 0;
        m_replayStopped = m_latest.frameIndex != m_objects.framesEnded() ||
                          m_previous.frameIndex + 1 != m_latest.frameIndex;
        if (m_replayStopped) {
            ++m_replaysUnaligned;
        }
    }
    if (m_replayStopped) {
        return false;
    }
    if (m_replayDraw >= m_latest.draws.size()) {
        ++m_replaysDiverged;
        m_replayStopped = true;
        return false;
    }
    size_t index = m_replayDraw++;
    const Draw& drawn = m_latest.draws[index];
    if (drawn.vertexShaderBaseHash != draw.vertexShaderBaseHash ||
        drawn.vertexShaderAuxHash != draw.vertexShaderAuxHash ||
        drawn.assembliesBefore != m_objects.replayCursor()) {
        ++m_replaysDiverged;
        m_replayStopped = true;
        return false;
    }
    if (!drawn.vertexEntry.has_value()) {
        return false;
    }
    if (VertexLayout::of(draw) != drawn.layout) {
        count(drawn.vertexShaderBaseHash, VertexOutcome::ShapeDiffers);
        return false;
    }
    auto started = std::chrono::steady_clock::now();
    waitUntilBlendedThrough(index);
    m_waiting += std::chrono::steady_clock::now() - started;
    const std::optional<PairBlend>& blended = m_drawBlends[index];
    if (!blended.has_value()) {
        return false;
    }
    count(drawn.vertexShaderBaseHash, blended->outcome);
    if (blended->outcome != VertexOutcome::Blended) {
        return false;
    }
    size_t start = blended->start;
    for (size_t buffer = 0; buffer < drawn.layout.buffers.size(); ++buffer) {
        replacements.data[buffer] = m_blended.data() + start;
        start += drawn.layout.buffers[buffer].sizeInBytes;
    }
    return true;
}

VertexOutcome VertexBlend::blendPair(const Draw& earlier, const Draw& partner, const Draw& drawn,
                                     PartnerIdentity identity, size_t start) {
    bool changed = false;
    for (size_t buffer = 0; buffer < drawn.layout.buffers.size() && !changed; ++buffer) {
        std::span<const std::byte> was = bufferBytes(m_previous, partner, buffer);
        std::span<const std::byte> is = bufferBytes(m_latest, drawn, buffer);
        changed = std::memcmp(was.data(), is.data(), is.size()) != 0;
    }
    if (!changed) {
        return VertexOutcome::Unchanged;
    }
    gather(m_twoBack, earlier, m_twoBackBytes);
    gather(m_previous, partner, m_before);
    gather(m_latest, drawn, m_after);
    return blendVertexBytes(drawn.layout, m_twoBackBytes, m_before, m_after, m_t, identity,
                            std::span<std::byte>(m_blended.data() + start, m_after.size()));
}

void VertexBlend::gather(const Frame& frame, const Draw& draw, std::vector<std::byte>& into) const {
    into.clear();
    for (size_t buffer = 0; buffer < draw.layout.buffers.size(); ++buffer) {
        std::span<const std::byte> bytes = bufferBytes(frame, draw, buffer);
        into.insert(into.end(), bytes.begin(), bytes.end());
    }
}

size_t VertexBlend::mostResembling(const Draw& drawn, const Frame& frame, size_t planned) {
    gather(frame, frame.draws[planned], m_candidate);
    Resemblance closest = resemblance(drawn.layout, m_candidate, m_after);
    size_t found = planned;
    auto shader = frame.byShader.find({drawn.vertexShaderBaseHash, drawn.vertexShaderAuxHash});
    if (shader == frame.byShader.end()) {
        return found;
    }
    for (size_t index : shader->second) {
        if (index == planned || frame.draws[index].layout != drawn.layout) {
            continue;
        }
        gather(frame, frame.draws[index], m_candidate);
        Resemblance candidate = resemblance(drawn.layout, m_candidate, m_after);
        if (candidate.closerThan(closest)) {
            closest = candidate;
            found = index;
        }
    }
    return found;
}

void VertexBlend::placeByVertices(Job& job) {
    const Draw& drawn = m_latest.draws[job.draw];
    auto [found, fresh] =
        m_placed.try_emplace({m_twoBack.draws[job.earlier].bufferStarts,
                              m_previous.draws[job.partner].bufferStarts, drawn.bufferStarts},
                             std::pair{job.earlier, job.partner});
    if (!fresh) {
        std::tie(job.earlier, job.partner) = found->second;
        return;
    }
    size_t plannedEarlier = job.earlier;
    size_t plannedPartner = job.partner;
    gather(m_latest, drawn, m_after);
    job.earlier = mostResembling(drawn, m_twoBack, job.earlier);
    job.partner = mostResembling(drawn, m_previous, job.partner);
    if (job.earlier != plannedEarlier || job.partner != plannedPartner) {
        ++m_partnersFoundByVertices;
    }
    found->second = {job.earlier, job.partner};
}

} // namespace wiiuport::interp
