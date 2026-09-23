#include "wiiuport/frame/VertexChanges.h"

#include <algorithm>
#include <functional>
#include <string_view>

namespace wiiuport::frame {

namespace {

struct Hashed {
    uint64_t hash{0};
    uint64_t bytes{0};
};

Hashed hashVertexBytes(const LatteFrameHooks::DrawPrepared& draw) {
    Hashed hashed;
    for (uint32_t index = 0; index < draw.vertexBufferCount; ++index) {
        const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer = draw.vertexBuffers[index];
        std::string_view data(static_cast<const char*>(buffer.data), buffer.sizeInBytes);
        // Order matters: the same bytes in another buffer are another draw.
        hashed.hash = (hashed.hash * 0x100000001b3ULL) ^ std::hash<std::string_view>{}(data);
        hashed.bytes += buffer.sizeInBytes;
    }
    return hashed;
}

} // namespace

void VertexChanges::onDraw(const LatteFrameHooks::DrawPrepared& draw) {
    VertexShader shader{draw.vertexShaderBaseHash, draw.vertexShaderAuxHash};
    if (!draw.vertexUniforms) {
        Hashed hashed = hashVertexBytes(draw);
        std::lock_guard lock(m_mutex);
        m_withoutUniforms.add(shader, hashed.hash, hashed.bytes);
        return;
    }
    std::lock_guard lock(m_mutex);
    if (m_censusTaken < m_censusAsked) {
        Hashed hashed = hashVertexBytes(draw);
        m_withUniforms.add(shader, hashed.hash, hashed.bytes);
        addAttributes(shader, draw);
    }
}

void VertexChanges::addAttributes(const VertexShader& shader,
                                  const LatteFrameHooks::DrawPrepared& draw) {
    for (uint32_t index = 0; index < draw.vertexAttributeCount; ++index) {
        const LatteFrameHooks::DrawPrepared::VertexAttribute& attribute =
            draw.vertexAttributes[index];
        const LatteFrameHooks::DrawPrepared::VertexBuffer& buffer =
            draw.vertexBuffers[attribute.buffer];
        const auto* bytes = static_cast<const char*>(buffer.data);
        m_gathered.clear();
        // Every whole value the buffer holds at the attribute's offset.
        for (uint64_t at = attribute.offset; at + attribute.sizeInBytes <= buffer.sizeInBytes;
             at += buffer.stride) {
            m_gathered.insert(m_gathered.end(), bytes + at, bytes + at + attribute.sizeInBytes);
            if (buffer.stride == 0) {
                break;
            }
        }
        m_attributes.add(
            {shader, attribute.semanticId, attribute.format},
            std::hash<std::string_view>{}(std::string_view(m_gathered.data(), m_gathered.size())),
            m_gathered.size());
    }
}

void VertexChanges::onFrameComplete() {
    std::lock_guard lock(m_mutex);
    m_withoutUniforms.compareFrame();
    if (m_censusTaken < m_censusAsked) {
        m_withUniforms.compareFrame();
        m_attributes.compareFrame();
        ++m_censusTaken;
    }
}

void VertexChanges::requestCensus(uint32_t frames) {
    std::lock_guard lock(m_mutex);
    m_withUniforms.clear();
    m_attributes.clear();
    m_censusAsked = frames;
    m_censusTaken = 0;
}

std::map<VertexChanges::VertexShader, VertexChanges::Counts>
VertexChanges::withoutUniforms() const {
    std::lock_guard lock(m_mutex);
    return m_withoutUniforms.counts();
}

VertexChanges::Census VertexChanges::census() const {
    std::lock_guard lock(m_mutex);
    return {m_censusAsked, m_censusTaken, m_withUniforms.counts(), m_attributes.counts()};
}

template <typename Key>
void VertexChanges::Tally<Key>::add(const Key& key, uint64_t hash, uint64_t bytes) {
    Counts& counts = m_counts[key];
    ++counts.draws;
    counts.bytesHashed += bytes;
    m_frame[key].push_back(hash);
}

template <typename Key> void VertexChanges::Tally<Key>::compareFrame() {
    for (const auto& [key, hashes] : m_frame) {
        auto before = m_previous.find(key);
        if (before == m_previous.end() || before->second.empty()) {
            continue;
        }
        Counts& counts = m_counts[key];
        counts.compared += hashes.size();
        for (uint64_t hash : hashes) {
            if (!std::binary_search(before->second.begin(), before->second.end(), hash)) {
                ++counts.changed;
            }
        }
    }
    m_previous.swap(m_frame);
    for (auto& [key, hashes] : m_previous) {
        std::sort(hashes.begin(), hashes.end());
    }
    for (auto& [key, hashes] : m_frame) {
        hashes.clear();
    }
}

template <typename Key> void VertexChanges::Tally<Key>::clear() {
    m_counts.clear();
    m_frame.clear();
    m_previous.clear();
}

template class VertexChanges::Tally<VertexChanges::VertexShader>;
template class VertexChanges::Tally<VertexChanges::Attribute>;

} // namespace wiiuport::frame
