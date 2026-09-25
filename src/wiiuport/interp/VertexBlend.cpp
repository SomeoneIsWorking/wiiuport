#include "wiiuport/interp/VertexBlend.h"

#include "wiiuport/interp/Blendable.h"
#include "wiiuport/interp/Midpoint.h"

#include <algorithm>
#include <bit>
#include <cmath>
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
// Given the second draw's own vertices two frames back, only what moved from
// there counts toward how near: a value it holds at both ends and the
// candidate otherwise is flipping with the title's double buffering, and it
// flips alike in every candidate -- the sea's patches, 430,000 apart in it
// whichever row, 208 apart in what moved between the right one and the next.
struct Resemblance {
    size_t same{0};
    double apartSquared{0.0};

    // `byMoveAlone` ranks by how near alone: where the draw two frames back is
    // known to be the object's own, what moved from it is where the object
    // passed, and a value it holds bit for bit is no better evidence than a
    // value the title flips in another object's buffers and happens to share.
    bool closerThan(const Resemblance& other, bool byMoveAlone) const {
        if (!byMoveAlone && same != other.same) {
            return same > other.same;
        }
        return apartSquared < other.apartSquared;
    }
};

Resemblance resemblance(const VertexLayout& layout, std::span<const std::byte> first,
                        std::span<const std::byte> second, std::span<const std::byte> twoBack) {
    Resemblance resemblance;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        uint32_t a = readWord(first.data() + word, endian);
        uint32_t b = readWord(second.data() + word, endian);
        if (a == b) {
            ++resemblance.same;
            return;
        }
        if (!twoBack.empty() && readWord(twoBack.data() + word, endian) == b) {
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
    case VertexOutcome::Started:
        return "started";
    case VertexOutcome::Excluded:
        return "excluded";
    case VertexOutcome::SharesBytes:
        return "sharesBytes";
    case VertexOutcome::Count:
        break;
    }
    return "unknown";
}

namespace {

VertexLayout::Buffer bufferOf(const LatteFrameHooks::DrawPrepared& draw, uint32_t index) {
    return {draw.vertexBuffers[index].sizeInBytes, draw.vertexBuffers[index].stride};
}

VertexLayout::Attribute attributeOf(const LatteFrameHooks::DrawPrepared& draw, uint32_t index) {
    const LatteFrameHooks::DrawPrepared::VertexAttribute& attribute = draw.vertexAttributes[index];
    return {attribute.buffer, attribute.offset, attribute.sizeInBytes, attribute.format,
            attribute.endianSwap};
}

} // namespace

VertexLayout VertexLayout::of(const LatteFrameHooks::DrawPrepared& draw) {
    VertexLayout layout;
    layout.assign(draw);
    return layout;
}

void VertexLayout::assign(const LatteFrameHooks::DrawPrepared& draw) {
    buffers.clear();
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        buffers.push_back(bufferOf(draw, index));
    }
    attributes.clear();
    for (uint32_t index = 0; index < draw.vertexAttributeCount; ++index) {
        attributes.push_back(attributeOf(draw, index));
    }
}

bool VertexLayout::describes(const LatteFrameHooks::DrawPrepared& draw) const {
    if (buffers.size() != draw.vertexBufferCount ||
        attributes.size() != draw.vertexAttributeCount) {
        return false;
    }
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        if (buffers[index] != bufferOf(draw, index)) {
            return false;
        }
    }
    for (uint32_t index = 0; index < draw.vertexAttributeCount; ++index) {
        if (attributes[index] != attributeOf(draw, index)) {
            return false;
        }
    }
    return true;
}

bool VertexLayout::absorb(const VertexLayout& other) {
    if (other.buffers != buffers) {
        return false;
    }
    for (const Attribute& attribute : other.attributes) {
        if (std::find(attributes.begin(), attributes.end(), attribute) == attributes.end()) {
            attributes.push_back(attribute);
        }
    }
    return true;
}

namespace {

// Whether the mesh stepped from N-1 to N no further than from N-2 to N-1,
// over the floats it moved in from N-2 to N.
struct MeshFrames {
    std::span<const std::byte> twoBack;
    std::span<const std::byte> before;
    std::span<const std::byte> after;
};

bool steppedNoFurther(const VertexLayout& layout, const MeshFrames& frames) {
    auto [twoBack, before, after] = frames;
    double toBefore = 0.0;
    double toAfter = 0.0;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        float first = readFloat(twoBack, word, endian);
        float between = readFloat(before, word, endian);
        float last = readFloat(after, word, endian);
        if (!Midpoint::movedIn(first, last) || !isNumber(between)) {
            return;
        }
        double stepped = static_cast<double>(between) - first;
        double leapt = static_cast<double>(last) - between;
        toBefore += stepped * stepped;
        toAfter += leapt * leapt;
    });
    return toAfter <= toBefore;
}

} // namespace

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
    if (midpoint.stoodAtStart()) {
        return VertexOutcome::Started;
    }
    if (!midpoint.landsOn()) {
        // Known by its blocks, its own animation may have turned back at
        // N-1, stepping to N no further than it stepped there; one that
        // leaps further was set back to the start of its run -- a band of
        // surf -- and half way is where it never was.
        if (identity != PartnerIdentity::ByBlocks ||
            !steppedNoFurther(layout, {.twoBack = twoBack, .before = before, .after = after})) {
            return VertexOutcome::Unverified;
        }
    }
    uint64_t blended = 0;
    bool outside = false;
    forEachFloat(layout, [&](size_t word, Endian endian) {
        float a = readFloat(before, word, endian);
        float b = readFloat(after, word, endian);
        // Only what moved from N-2 to N: a value N-2 and N agree on is
        // flipping, not moving, whatever N-1 holds.
        if (!Midpoint::movedIn(readFloat(twoBack, word, endian), b) || !isNumber(a) ||
            sameBits(a, b)) {
            return;
        }
        // Exact at both ends: at t=1 the vertex is N's bit for bit.
        float value = std::lerp(a, b, t);
        // A move of an ulp has no value between its ends; it stays at N, and
        // the rest of the mesh, halved, still lies between its two frames.
        if (!liesBetween(a, value, b)) {
            outside = true;
            return;
        }
        writeWord(out.data() + word, std::bit_cast<uint32_t>(value), endian);
        ++blended;
    });
    if (blended > 0) {
        return VertexOutcome::Blended;
    }
    return outside ? VertexOutcome::Outside : VertexOutcome::Unchanged;
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
    : m_objects(objects), m_t(t), m_pool(kBlendWorkers, [this](size_t slot, size_t worker) {
          blendSlot(slot, m_scratch[worker]);
      }) {
}

VertexBlend::~VertexBlend() = default;

VertexBlend::Draw& VertexBlend::Frame::appendDraw() {
    if (drawCount == drawSlots.size()) {
        drawSlots.emplace_back();
    }
    Draw& draw = drawSlots[drawCount++];
    draw.vertexEntry.reset();
    draw.ordinal = 0;
    draw.layout.buffers.clear();
    draw.layout.attributes.clear();
    draw.bufferStarts.clear();
    draw.bufferSources.clear();
    draw.mesh = 0;
    draw.nextOfEntry = 0;
    return draw;
}

void VertexBlend::Frame::clear() {
    drawCount = 0;
    bytes.clear();
    copied.clear();
    entryDraws.clear();
    byShader.clear();
    meshes.clear();
    bySource.clear();
    continuedFrom.clear();
    continuedBy.clear();
    reads.clear();
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
    size_t index = m_building.drawCount;
    Draw& recorded = m_building.appendDraw();
    recorded.vertexShaderBaseHash = draw.vertexShaderBaseHash;
    recorded.vertexShaderAuxHash = draw.vertexShaderAuxHash;
    recorded.assembliesBefore = m_building.assemblies;
    // Kept when the vertex-stage uniforms last assembled are this draw's
    // own: a draw without them has no object to take a partner from.
    bool ownUniforms = draw.vertexUniforms && m_building.lastVertexEntry.has_value() &&
                       m_building.lastVertexShaderBaseHash == draw.vertexShaderBaseHash &&
                       m_building.lastVertexShaderAuxHash == draw.vertexShaderAuxHash;
    if (ownUniforms && m_objects.isPlanning()) {
        auto started = std::chrono::steady_clock::now();
        recorded.vertexEntry = m_building.lastVertexEntry;
        recorded.layout.assign(draw);
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
            recorded.bufferSources.push_back(buffer.data);
        }
        if (!recorded.bufferSources.empty()) {
            m_building.bySource.try_emplace(
                std::pair{draw.vertexShaderBaseHash, recorded.bufferSources.front()},
                static_cast<uint32_t>(index));
        }
        // Draws sharing one vertex-stage assembly take their place among it.
        uint32_t entry = *recorded.vertexEntry;
        if (m_building.entryDraws.size() <= entry) {
            m_building.entryDraws.resize(entry + 1);
        }
        EntryDraws& drawsOfEntry = m_building.entryDraws[entry];
        recorded.ordinal = drawsOfEntry.count++;
        if (recorded.ordinal == 0) {
            drawsOfEntry.first = static_cast<uint32_t>(index);
        } else {
            m_building.drawSlots[drawsOfEntry.last].nextOfEntry = static_cast<uint32_t>(index);
        }
        drawsOfEntry.last = static_cast<uint32_t>(index);
        m_building.byShader[{draw.vertexShaderBaseHash, draw.vertexShaderAuxHash}].push_back(index);
        auto [mesh, fresh] = m_building.meshes.try_emplace(
            recorded.bufferStarts, static_cast<uint32_t>(m_building.meshes.size()));
        recorded.mesh = mesh->second;
        if (fresh) {
            recordReads(draw, recorded.mesh);
        }
        m_copying += std::chrono::steady_clock::now() - started;
    } else if (m_objects.isPlanning()) {
        recordReads(draw, std::nullopt);
    }
}

void VertexBlend::recordReads(const LatteFrameHooks::DrawPrepared& draw,
                              std::optional<uint32_t> mesh) {
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
        m_building.reads.push_back(VertexRead{.begin = reinterpret_cast<uintptr_t>(buffer.data),
                                              .size = buffer.sizeInBytes,
                                              .mesh = mesh});
    }
}

void VertexBlend::onFrameRecorded(const frame::FrameRecording& /*recording*/) {
    // The frames it blends are about to move.
    m_pool.waitAll();
    recordContinuations();
    publishCounts();
    m_building.frameIndex = m_objects.framesEnded();
    std::swap(m_twoBack, m_previous);
    std::swap(m_previous, m_latest);
    std::swap(m_latest, m_building);
    m_building.clear();
    startBlending();
}

void VertexBlend::startBlending() {
    m_drawBlends.assign(m_latest.draws().size(), std::nullopt);
    m_drawSlots.assign(m_latest.draws().size(), std::nullopt);
    m_pairSlots.clear();
    if (m_previous.frameIndex + 1 != m_latest.frameIndex ||
        m_twoBack.frameIndex + 2 != m_latest.frameIndex) {
        return;
    }
    // The plan is the latest frame's until the next frame ends: read here,
    // not on the blending thread, which the next frame's end does not wait
    // for before planning goes on. A mesh is drawn alike by every draw that
    // reads its buffers, whatever it fetches of them, the first with a
    // partner deciding for all: readers drawn differently tear it, as a
    // shadow volume's did.
    std::vector<uint64_t> excluded;
    {
        std::lock_guard lock(m_excludedMutex);
        excluded = m_excluded;
    }
    size_t meshCount = m_latest.meshes.size();
    m_meshLayouts.assign(meshCount, MeshLayout{});
    m_mergedLayouts.clear();
    m_meshExcluded.assign(meshCount, 0);
    m_meshJobs.assign(meshCount, std::nullopt);
    for (const Draw& drawn : m_latest.draws()) {
        if (!drawn.vertexEntry.has_value()) {
            continue;
        }
        MeshLayout& mesh = m_meshLayouts[drawn.mesh];
        if (mesh.layout == nullptr) {
            mesh.layout = &drawn.layout;
        } else if (!mesh.refused && *mesh.layout != drawn.layout) {
            if (!mesh.merged) {
                mesh.layout = &m_mergedLayouts.emplace_back(*mesh.layout);
                mesh.merged = true;
            }
            mesh.refused = !m_mergedLayouts.back().absorb(drawn.layout);
        }
        if (std::ranges::find(excluded, drawn.vertexShaderBaseHash) != excluded.end()) {
            m_meshExcluded[drawn.mesh] = 1;
        }
    }
    // A shader any of whose draws recur from their own buffers two frames
    // apart keeps its objects' vertices in buffers of their own.
    m_shadersKeepingBuffers.clear();
    for (const Draw& drawn : m_latest.draws()) {
        if (drawn.vertexEntry.has_value() && drawnFrom(m_twoBack, drawn).has_value()) {
            m_shadersKeepingBuffers.insert({drawn.vertexShaderBaseHash, drawn.vertexShaderAuxHash});
        }
    }
    VertexReadGroups groups = groupVertexReads(m_latest.reads, meshCount);
    for (size_t index = 0; index < m_latest.draws().size(); ++index) {
        const Draw& drawn = m_latest.draws()[index];
        if (!drawn.vertexEntry.has_value()) {
            continue;
        }
        if (m_meshExcluded[drawn.mesh] != 0) {
            m_drawBlends[index] = PairBlend{VertexOutcome::Excluded, 0};
            continue;
        }
        if (groups.readUnredirected[drawn.mesh] != 0) {
            m_drawBlends[index] = PairBlend{VertexOutcome::SharesBytes, 0};
            continue;
        }
        if (m_meshJobs[drawn.mesh].has_value()) {
            continue;
        }
        std::variant<Job, VertexOutcome> planned = planDraw(index);
        const MeshLayout& layout = m_meshLayouts[drawn.mesh];
        if (Job* job = std::get_if<Job>(&planned); job != nullptr && !layout.refused) {
            job->layout = layout.layout;
            m_meshJobs[drawn.mesh] = *job;
        } else {
            m_drawBlends[index] = PairBlend{
                job != nullptr ? VertexOutcome::ShapeDiffers : std::get<VertexOutcome>(planned), 0};
        }
    }
    // Each mesh's place, in the order the replay first draws it, so nothing
    // blended moves.
    m_meshSlots.assign(meshCount, std::nullopt);
    size_t bytes = 0;
    for (size_t index = 0; index < m_latest.draws().size(); ++index) {
        const Draw& drawn = m_latest.draws()[index];
        const std::optional<Job>& meshJob = m_meshJobs[drawn.mesh];
        if (!drawn.vertexEntry.has_value() || !meshJob.has_value()) {
            continue;
        }
        std::optional<size_t>& slot = m_meshSlots[drawn.mesh];
        if (!slot.has_value()) {
            const Job& job = *meshJob;
            slot = m_pairSlots.size();
            m_pairSlots.push_back(PairSlot{job, bytes});
            for (const VertexLayout::Buffer& buffer : job.layout->buffers) {
                bytes += buffer.sizeInBytes;
            }
        }
        m_drawSlots[index] = slot;
        m_drawBlends[index].reset();
    }
    if (m_blended.size() < bytes) {
        m_blended.resize(bytes);
    }
    groupMeshes(groups);
    m_pool.start(m_pairSlots.size());
}

namespace {

// Whether a mesh drawn with this outcome is drawn at N although its vertices
// may have moved: a mesh sharing its bytes cannot then be blended.
bool drawnAtNWhereItMoved(VertexOutcome outcome) {
    switch (outcome) {
    case VertexOutcome::Blended:
    case VertexOutcome::Unchanged:
    case VertexOutcome::Held:
    case VertexOutcome::NotFloats:
        return false;
    default:
        return true;
    }
}

} // namespace

void VertexBlend::groupMeshes(const VertexReadGroups& groups) {
    size_t meshCount = groups.groupOf.size();
    m_meshGroup = groups.groupOf;
    m_groupMeshes.assign(meshCount, 0);
    m_groupSlots.resize(meshCount);
    for (std::vector<size_t>& slots : m_groupSlots) {
        slots.clear();
    }
    m_groupStepped.assign(meshCount, 0);
    m_groupTorn.assign(meshCount, std::nullopt);
    for (uint32_t mesh = 0; mesh < meshCount; ++mesh) {
        uint32_t group = m_meshGroup[mesh];
        ++m_groupMeshes[group];
        if (const std::optional<size_t>& slot = m_meshSlots[mesh]; slot.has_value()) {
            m_groupSlots[group].push_back(*slot);
        }
    }
    for (size_t index = 0; index < m_latest.draws().size(); ++index) {
        const Draw& drawn = m_latest.draws()[index];
        const std::optional<PairBlend>& planned = m_drawBlends[index];
        if (drawn.vertexEntry.has_value() && !m_meshJobs[drawn.mesh].has_value() &&
            planned.has_value() && drawnAtNWhereItMoved(planned->outcome)) {
            m_groupStepped[m_meshGroup[drawn.mesh]] = 1;
        }
    }
}

bool VertexBlend::groupTorn(uint32_t group) {
    std::optional<bool>& torn = m_groupTorn[group];
    if (torn.has_value()) {
        return *torn;
    }
    torn = m_groupStepped[group] != 0;
    for (size_t slot : m_groupSlots[group]) {
        if (!m_pool.done(slot)) {
            auto started = std::chrono::steady_clock::now();
            m_pool.waitFor(slot);
            m_waiting += std::chrono::steady_clock::now() - started;
        }
        torn = *torn || drawnAtNWhereItMoved(m_pairSlots[slot].outcome);
    }
    return *torn;
}

std::variant<VertexBlend::Job, VertexOutcome> VertexBlend::planDraw(size_t index) const {
    const Draw& drawn = m_latest.draws()[index];
    if (!drawn.vertexEntry.has_value()) {
        return VertexOutcome::NoPartner;
    }
    size_t entry = *drawn.vertexEntry;
    const ObjectPlanner& planner = m_objects.planner();
    std::optional<size_t> partnerEntry = planner.partnerOf(entry);
    if (!partnerEntry.has_value()) {
        return VertexOutcome::NoPartner;
    }
    // A partner is planned only with the object's entry two frames back.
    std::optional<size_t> earlierEntry = planner.earlierOf(entry);
    std::optional<size_t> partner = drawOf(m_previous, *partnerEntry, drawn);
    std::optional<size_t> earlier;
    if (earlierEntry.has_value()) {
        earlier = drawOf(m_twoBack, *earlierEntry, drawn);
    }
    if (!partner.has_value() || !earlier.has_value()) {
        return VertexOutcome::ShapeDiffers;
    }
    PartnerIdentity identity = PartnerIdentity::ByBlocks;
    if (planner.latest().sharesKey(entry)) {
        identity = PartnerIdentity::ByPlace;
    } else if (planner.partnerFoundByValues(entry)) {
        identity = PartnerIdentity::ByValues;
    }
    Job job{index, *partner, *earlier, identity, index, &drawn.layout};
    if (identity != PartnerIdentity::ByPlace && keepsBuffers(drawn) &&
        continuesElsewhere(m_twoBack.draws()[*earlier], index)) {
        // Its blocks name an object whose buffers another draw reads at N:
        // that object goes on there, and this draw -- a ring new at N given
        // the blocks of one that moved to others -- has no frame before.
        return VertexOutcome::NoPartner;
    }
    if (identity == PartnerIdentity::ByPlace && keepsBuffers(drawn)) {
        // Told apart by nothing else, an object the title keeps buffers for
        // is its draw two frames back from the same buffers; with none, it
        // is new at N, and any other it resembled -- a ring of the same age
        // left further back -- is another object.
        std::optional<size_t> own = drawnFrom(m_twoBack, drawn);
        if (!own.has_value()) {
            return VertexOutcome::NoPartner;
        }
        job.earlier = *own;
        job.earlierByBuffers = true;
    }
    return job;
}

std::optional<size_t> VertexBlend::drawnFrom(const Frame& frame, const Draw& drawn) {
    if (drawn.bufferSources.empty()) {
        return std::nullopt;
    }
    auto found = frame.bySource.find({drawn.vertexShaderBaseHash, drawn.bufferSources.front()});
    if (found == frame.bySource.end()) {
        return std::nullopt;
    }
    const Draw& candidate = frame.draws()[found->second];
    if (candidate.vertexShaderAuxHash != drawn.vertexShaderAuxHash ||
        candidate.bufferSources != drawn.bufferSources || candidate.layout != drawn.layout) {
        return std::nullopt;
    }
    return found->second;
}

bool VertexBlend::continuesElsewhere(const Draw& earlier, size_t index) const {
    const Draw& drawn = m_latest.draws()[index];
    if (earlier.bufferSources == drawn.bufferSources) {
        return false;
    }
    std::optional<size_t> heir = drawnFrom(m_latest, earlier);
    return heir.has_value() && *heir != index;
}

bool VertexBlend::keepsBuffers(const Draw& drawn) const {
    return m_shadersKeepingBuffers.contains(
        {drawn.vertexShaderBaseHash, drawn.vertexShaderAuxHash});
}

std::optional<size_t> VertexBlend::drawOf(const Frame& frame, size_t entry, const Draw& drawn) {
    if (entry >= frame.entryDraws.size() || drawn.ordinal >= frame.entryDraws[entry].count) {
        return std::nullopt;
    }
    size_t found = frame.entryDraws[entry].first;
    for (uint32_t ordinal = 0; ordinal < drawn.ordinal; ++ordinal) {
        found = frame.draws()[found].nextOfEntry;
    }
    if (frame.draws()[found].layout != drawn.layout) {
        return std::nullopt;
    }
    return found;
}

void VertexBlend::blendSlot(size_t slot, Scratch& scratch) {
    PairSlot& pair = m_pairSlots[slot];
    Job job = pair.job;
    if (job.identity == PartnerIdentity::ByPlace && !placeByVertices(job, scratch)) {
        pair.outcome = VertexOutcome::NoPartner;
        return;
    }
    pair.outcome =
        blendPair(m_twoBack.draws()[job.earlier], m_previous.draws()[job.partner],
                  m_latest.draws()[job.draw], *job.layout, job.identity, pair.start, scratch);
    if (job.earlierByBuffers && pair.outcome == VertexOutcome::Blended) {
        pair.partnerSource = m_previous.draws()[job.partner].bufferSources.front();
    }
}

std::optional<VertexBlend::PairBlend> VertexBlend::blendOf(size_t index) {
    const std::optional<size_t>& slot = m_drawSlots[index];
    if (!slot.has_value()) {
        return m_drawBlends[index];
    }
    if (!m_pool.done(*slot)) {
        auto started = std::chrono::steady_clock::now();
        m_pool.waitFor(*slot);
        m_waiting += std::chrono::steady_clock::now() - started;
    }
    const PairSlot& pair = m_pairSlots[*slot];
    uint32_t group = m_meshGroup[m_latest.draws()[index].mesh];
    if (pair.outcome == VertexOutcome::Blended && m_groupMeshes[group] > 1 && groupTorn(group)) {
        return PairBlend{VertexOutcome::SharesBytes, 0};
    }
    return PairBlend{pair.outcome, pair.start};
}

void VertexBlend::count(uint64_t shaderBaseHash, VertexOutcome outcome) {
    ++m_outcomes[static_cast<size_t>(outcome)];
    ++m_byShaderPending[shaderBaseHash][static_cast<size_t>(outcome)];
}

void VertexBlend::publishCounts() {
    std::lock_guard lock(m_byShaderMutex);
    for (const auto& [shader, draws] : m_byShaderPending) {
        std::array<uint64_t, kVertexOutcomeCount>& total = m_byShader[shader];
        for (size_t outcome = 0; outcome < kVertexOutcomeCount; ++outcome) {
            total[outcome] += draws[outcome];
        }
    }
    m_byShaderPending.clear();
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
    if (m_replayDraw >= m_latest.draws().size()) {
        ++m_replaysDiverged;
        m_replayStopped = true;
        return false;
    }
    size_t index = m_replayDraw++;
    const Draw& drawn = m_latest.draws()[index];
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
    if (!drawn.layout.describes(draw)) {
        count(drawn.vertexShaderBaseHash, VertexOutcome::ShapeDiffers);
        return false;
    }
    std::optional<PairBlend> blended = blendOf(index);
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
                                     const VertexLayout& layout, PartnerIdentity identity,
                                     size_t start, Scratch& scratch) {
    bool changed = false;
    for (size_t buffer = 0; buffer < layout.buffers.size() && !changed; ++buffer) {
        std::span<const std::byte> was = bufferBytes(m_previous, partner, buffer);
        std::span<const std::byte> is = bufferBytes(m_latest, drawn, buffer);
        changed = std::memcmp(was.data(), is.data(), is.size()) != 0;
    }
    if (!changed) {
        return VertexOutcome::Unchanged;
    }
    gather(m_twoBack, earlier, scratch.twoBack);
    gather(m_previous, partner, scratch.before);
    gather(m_latest, drawn, scratch.after);
    // Each slot's bytes are its own: workers write m_blended apart.
    return blendVertexBytes(layout, scratch.twoBack, scratch.before, scratch.after, m_t, identity,
                            std::span<std::byte>(m_blended.data() + start, scratch.after.size()));
}

void VertexBlend::gather(const Frame& frame, const Draw& draw, std::vector<std::byte>& into) const {
    into.clear();
    for (size_t buffer = 0; buffer < draw.layout.buffers.size(); ++buffer) {
        std::span<const std::byte> bytes = bufferBytes(frame, draw, buffer);
        into.insert(into.end(), bytes.begin(), bytes.end());
    }
}

std::optional<size_t> VertexBlend::mostResembling(const Draw& drawn, const VertexLayout& layout,
                                                  const Frame& frame, size_t planned,
                                                  std::span<const std::byte> twoBack,
                                                  const void* ownSource, Scratch& scratch) const {
    std::optional<size_t> found;
    std::optional<Resemblance> closest;
    auto consider = [&](size_t index) {
        if (wentOnElsewhere(frame, frame.draws()[index], ownSource)) {
            return;
        }
        gather(frame, frame.draws()[index], scratch.candidate);
        Resemblance candidate = resemblance(layout, scratch.candidate, scratch.after, twoBack);
        if (!closest.has_value() || candidate.closerThan(*closest, ownSource != nullptr)) {
            closest = candidate;
            found = index;
        }
    };
    consider(planned);
    auto shader = frame.byShader.find({drawn.vertexShaderBaseHash, drawn.vertexShaderAuxHash});
    if (shader == frame.byShader.end()) {
        return found;
    }
    for (size_t index : shader->second) {
        if (index != planned && frame.draws()[index].layout.buffers == layout.buffers) {
            consider(index);
        }
    }
    return found;
}

bool VertexBlend::wentOnElsewhere(const Frame& frame, const Draw& draw, const void* ownSource) {
    if (ownSource == nullptr || draw.bufferSources.empty()) {
        return false;
    }
    const void* source = draw.bufferSources.front();
    auto wentOnFrom = frame.continuedFrom.find({draw.vertexShaderBaseHash, source});
    if (wentOnFrom != frame.continuedFrom.end() && wentOnFrom->second != ownSource) {
        return true;
    }
    auto wentOnBy = frame.continuedBy.find({draw.vertexShaderBaseHash, ownSource});
    return wentOnBy != frame.continuedBy.end() && wentOnBy->second != source;
}

void VertexBlend::recordContinuations() {
    for (const PairSlot& pair : m_pairSlots) {
        if (pair.partnerSource == nullptr) {
            continue;
        }
        const Draw& drawn = m_latest.draws()[pair.job.draw];
        const void* own = drawn.bufferSources.front();
        m_latest.continuedFrom[{drawn.vertexShaderBaseHash, own}] = pair.partnerSource;
        m_latest.continuedBy[{drawn.vertexShaderBaseHash, pair.partnerSource}] = own;
    }
}

bool VertexBlend::placeByVertices(Job& job, Scratch& scratch) {
    const Draw& drawn = m_latest.draws()[job.plannedBy];
    size_t plannedEarlier = job.earlier;
    size_t plannedPartner = job.partner;
    gather(m_latest, drawn, scratch.after);
    const Draw& earlier = m_twoBack.draws()[job.earlier];
    const void* ownSource = job.earlierByBuffers ? earlier.bufferSources.front() : nullptr;
    if (!job.earlierByBuffers) {
        // With no buffers named, every draw of the shader may be it.
        job.earlier =
            mostResembling(drawn, *job.layout, m_twoBack, job.earlier, {}, nullptr, scratch)
                .value_or(job.earlier);
    }
    gather(m_twoBack, m_twoBack.draws()[job.earlier], scratch.twoBack);
    std::optional<size_t> partner = mostResembling(drawn, *job.layout, m_previous, job.partner,
                                                   scratch.twoBack, ownSource, scratch);
    if (!partner.has_value()) {
        return false;
    }
    job.partner = *partner;
    if (job.earlier != plannedEarlier || job.partner != plannedPartner) {
        ++m_partnersFoundByVertices;
    }
    return true;
}

} // namespace wiiuport::interp
